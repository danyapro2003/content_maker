#include "appsettings.h"
#include "llmclient.h"
#include "storynarrative.h"
#include "s3cache.h"
#include "pipelinemanager.h"
#include "storytimeline.h"
#include "subtitlecomposer.h"
#include <QFile>
#include <QSaveFile>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTextStream>
#include <QTimer>
#include <stdexcept>

static void logLine(const QString& text) { QTextStream(stdout) << text << Qt::endl; }
void prepareV3Source(const QString& settingsPath, const QString& source)
{
    auto& settings = AppSettings::instance();
    if (!settings.load(settingsPath)) throw std::runtime_error("Cannot load settings");
    if (settings.s3ScriptsDir.isEmpty()) settings.s3ScriptsDir = QFileInfo(settingsPath).absolutePath() + "/scripts";
    settings.s3RunMode = AppSettings::StoryRunMode::Continue;
    PipelineManager manager; manager.setMode(PipelineManager::Mode::Stories5); manager.setOfflinePreview(true);
    QEventLoop loop; bool prepared = false;
    QObject::connect(&manager, &PipelineManager::logMessage, &loop, [&](const QString& text) {
        logLine(text); if (text.contains("OFFLINE_PREVIEW")) prepared = true;
    });
    QObject::connect(&manager, &PipelineManager::queueFinished, &loop, &QEventLoop::quit);
    manager.addFilesToQueue({source}); loop.exec();
    if (!prepared) throw std::runtime_error("Local transcript preparation failed");
    logLine("PREPARATION_COMPLETE: ASR/review/diarization cached; no LLM request sent.");
}

static QJsonObject readObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(("Cannot read " + path).toStdString());
    return QJsonDocument::fromJson(file.readAll()).object();
}
static void saveObject(const QString& path, const QJsonObject& object)
{
    QSaveFile file(path); const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error(("Cannot save " + path).toStdString());
}

// Explicit diagnostic command; never called by the automatic retry path.
void retryLoggedV3Request(const QStringList& args)
{
    auto& settings = AppSettings::instance();
    if (!settings.load(args[2])) throw std::runtime_error("Cannot load settings");
    const auto original = readObject(args[3]); const auto body = original["body"].toObject();
    QUrl endpoint(settings.llmBaseUrl);
    QString endpointText = endpoint.toString(); if (endpointText.endsWith('/')) endpointText.chop(1);
    endpointText += "/chat/completions";
    if (original["url"] != endpointText || body["model"] != settings.llmModel || !body["messages"].isArray())
        throw std::runtime_error("Logged request does not match configured provider/model");
    QJsonObject modelDependencies{{"model", settings.llmModel},
        {"provider", endpoint.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::StripTrailingSlash)}, {"temperature", .7}};
    if (body.contains("max_completion_tokens"))
        modelDependencies["max_completion_tokens"] = body["max_completion_tokens"];
    const auto modelKey = StoryRunStore::hash(modelDependencies);
    const auto key = StoryRunStore::hash({{"model_key", modelKey}, {"messages", body["messages"]}});
    QDir sourceRoot(QFileInfo(args[3]).absolutePath()); sourceRoot.cdUp(); sourceRoot.cdUp();
    QDir analysis(sourceRoot.filePath("analysis")); StoryRunStore store; bool found = false;
    for (const auto& name : analysis.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!QFileInfo::exists(analysis.filePath(name + "/request_" + key + ".json"))) continue;
        const auto manifest = readObject(analysis.filePath(name + "/manifest.json"));
        if (!store.open(analysis.absolutePath(), manifest["dependencies"].toObject())) throw std::runtime_error("Cannot open request ledger");
        if (store.load("request", key, false)["status"] != "outcome_unknown")
            throw std::runtime_error("Only an unknown-outcome request can be explicitly retried");
        found = true; break;
    }
    if (!found) throw std::runtime_error("Original unknown-outcome ledger record not found");
    if (QFileInfo::exists(args[4] + "/explicit_retry.json")) throw std::runtime_error("This explicit retry has already been started");
    if (!QDir().mkpath(args[4])) throw std::runtime_error("Cannot create retry logs");
    saveObject(args[4] + "/explicit_retry.json", {{"original_request", args[3]}, {"request_key", key}, {"post_limit", 1}});
    if (!store.save("request", key, {{"stage", original["stage"]}, {"explicit_retry_logs", args[4]}}, "pending"))
        throw std::runtime_error("Cannot persist retry before POST");
    QNetworkRequest request{QUrl(endpointText)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + settings.llmApiKey.toUtf8());
    LlmTransport::RetryPolicy policy; policy.maxAttempts = 1;
    LlmTransport transport(policy); transport.resetBudget(1); QEventLoop loop; bool success = false;
    QObject::connect(&transport, &LlmTransport::logMessage, &loop, logLine);
    transport.send(request, body, original["stage"].toString(), "001_explicit_retry", args[4],
        [&](bool ok, const QString& content, const QString&) {
            success = ok;
            store.save("request", key, {{"stage", original["stage"]}, {"content", content}, {"explicit_retry_logs", args[4]}},
                ok ? "complete" : "outcome_unknown");
            logLine(ok ? "EXPLICIT_RETRY_COMPLETE: response cached; no automatic further retry." : content); loop.quit();
        }, qBound(1, settings.llmTimeoutMinutes, 60) * 60000);
    loop.exec();
    if (!success) throw std::runtime_error("Explicit one-shot retry failed; inspect logs before any further request");
}

void renderV3Evaluation(const QStringList& args)
{
    auto& settings = AppSettings::instance();
    if (!settings.load(args[2])) throw std::runtime_error("Cannot load settings");
    const auto source = args[3], packedPath = args[4], wordsPath = args[5], resultPath = args[6], outDir = args[7];
    const auto run = readObject(QFileInfo(resultPath).absolutePath() + "/run.json");
    if (QFileInfo(run["source"].toString()).canonicalFilePath() != QFileInfo(source).canonicalFilePath()
        || run["source_fingerprint"] != S3Cache::fileFingerprint(source)) throw std::runtime_error("Source differs from evaluated film");
    const auto evaluated = readObject(resultPath);
    const auto stories = evaluated["stories"].toArray();
    if (evaluated["status"] != "analysis_complete" || stories.isEmpty()) throw std::runtime_error("No accepted analysis to render");
    const auto phrases = Stories3Runner::parsePacked(packedPath);
    const auto words = readObject(wordsPath);
    const auto language = words["language"].toString();
    QProcess probe;
    probe.start(settings.ffprobePath, {"-v", "error", "-print_format", "json", "-show_streams", "-show_format", source});
    if (!probe.waitForFinished(30000) || probe.exitCode()) throw std::runtime_error("Probe failed");
    MetadataParser parser; MediaInfo info;
    QObject::connect(&parser, &MetadataParser::parseCompleted, &parser, [&](const QJsonDocument&, const MediaInfo& parsed) { info = parsed; });
    parser.parse(probe.readAllStandardOutput(), source);
    const int audio = language == "ru" ? info.rusAudioIndex : (language == "en" ? info.engAudioIndex : -1);
    if (audio < 0) throw std::runtime_error("Cached transcript language has no matching audio");
    if (!QDir().mkpath(outDir + "/" + language)) throw std::runtime_error("Cannot create render output directory");
    QJsonArray outputs; QStringList errors;
    int number = 0;
    for (const auto& value : stories) {
        ++number; const auto saved = value.toObject();
        const auto parsed = StoryProtocol::parseEdit(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"protocol_version", 2}, {"action", "edit_story"}, {"branch_id", saved["branch_id"]}, {"story", saved}}).toJson()), phrases);
        if (!parsed.ok()) throw std::runtime_error(parsed.messages().join("; ").toStdString());
        auto clip = parsed.value;
        const auto segments = saved["segments"].toArray();
        QList<QPair<double, double>> ranges;
        for (int i = 0; i < clip.segments.size(); ++i) {
            if (!segments[i].toObject()["start_time"].isDouble() || !segments[i].toObject()["end_time"].isDouble()) throw std::runtime_error("Missing compiled interval");
            clip.segments[i].startSec = segments[i].toObject()["start_time"].toDouble();
            clip.segments[i].endSec = segments[i].toObject()["end_time"].toDouble();
            ranges.append({clip.segments[i].startSec, clip.segments[i].endSec});
        }
        if (saved["timeline_hash"].toString().isEmpty() || StoryTimeline::hash(clip) != saved["timeline_hash"].toString())
            throw std::runtime_error("Timeline changed since audit");
        QList<SrtCue> cues; QList<double> cuts; double offset = 0;
        for (const auto& range : ranges) {
            for (const auto& value : words["words"].toArray()) {
                const auto word = value.toObject(); const double start = word["t"].toDouble(), end = word["e"].toDouble();
                if (end <= range.first || start >= range.second || word["w"].toString().trimmed().isEmpty()) continue;
                SrtCue cue; cue.text = word["w"].toString(); cue.startSec = qMax(start, range.first) - range.first + offset;
                cue.endSec = qMin(end, range.second) - range.first + offset; cue.index = cues.size() + 1;
                if (cue.endSec - cue.startSec >= .02) cues.append(cue);
            }
            offset += range.second - range.first; cuts.append(offset);
        }
        if (!cuts.isEmpty()) cuts.removeLast();
        const auto base = QString("/story_%1").arg(number, 2, 10, QChar('0'));
        const auto ass = outDir + base + ".ass";
        if (cues.isEmpty() || !SubtitleComposer::writeAss(cues, ass, settings.s3SubtitleFontSize, settings.s3SubtitleWords, cuts))
            throw std::runtime_error("Cannot build subtitles from cached words");
        ClipRenderer renderer; QEventLoop loop;
        QObject::connect(&renderer, &ClipRenderer::clipRendered, &loop, [&](const QString& path) { outputs.append(path); logLine("VERIFIED_VIDEO: " + path); loop.quit(); });
        QObject::connect(&renderer, &ClipRenderer::errorOccurred, &loop, [&](const QString& error) { errors.append(error); logLine(error); loop.quit(); });
        logLine(QString("Render %1/%2 [%3], %4 seconds: %5").arg(number).arg(stories.size()).arg(language).arg(clip.totalSec()).arg(clip.title));
        QTimer progress; progress.setInterval(30000);
        QObject::connect(&progress, &QTimer::timeout, &loop, []() { logLine("Rendering in progress..."); }); progress.start();
        renderer.renderStoryClip(source, ranges, audio, ass, outDir + "/" + language + base + ".mp4", 0, true);
        loop.exec();
    }
    saveObject(outDir + "/result.json", {{"status", outputs.isEmpty() ? "failed" : (errors.isEmpty() ? "completed" : "partial")},
        {"diagnostic", true}, {"source", source}, {"cached_audio_language", language}, {"outputs", outputs},
        {"stories", stories}, {"errors", QJsonArray::fromStringList(errors)}});
    if (outputs.isEmpty()) throw std::runtime_error("No verified rendered videos");
}
void checkLiveModel(const QString& settingsPath, const QString& directory)
{
    if (!AppSettings::instance().load(settingsPath)) throw std::runtime_error("Cannot load settings");
    LlmClient client;
    QEventLoop loop;
    bool success = false;
    QObject::connect(&client, &LlmClient::logMessage, &loop, logLine);
    QObject::connect(&client, &LlmClient::modelFormatChecked, &loop, [&](bool ok, const QString& message) {
        success = ok; logLine(message); loop.quit();
    });
    client.checkModelFormat(directory); loop.exec();
    if (!success) throw std::runtime_error("Model format check failed; see logs");
}

void liveV3Run(const QStringList& args)
{
    auto& settings = AppSettings::instance();
    if (!settings.load(args[2])) throw std::runtime_error("Cannot load settings");
    const int count = args[7].toInt();
    if (count < 1 || count > 3) throw std::runtime_error("Diagnostic target must be 1..3 stories");
    settings.minClips = count; settings.maxClips = count == 1 ? 1 : count + 2;
    settings.s3ReserveBranches = count == 1 ? 0 : 2;
    const int budget = args.size() > 8 ? args[8].toInt() : (count == 1 ? 12 : 36);
    if (budget < 1 || budget > 36) throw std::runtime_error("Diagnostic request budget must be 1..36");
    settings.s3RequestLimit = budget;
    settings.s3RunMode = AppSettings::StoryRunMode::Continue;
    settings.s3AllowPartial = true; settings.s3AllowUnknownRetry = false; settings.agentFramesEnabled = false;
    const auto phrases = Stories3Runner::parsePacked(args[4]);
    if (phrases.isEmpty()) throw std::runtime_error("Cannot read cached transcript");
    QProcess probe;
    probe.start(settings.ffprobePath, {"-v", "error", "-print_format", "json", "-show_streams", "-show_format", args[3]});
    if (!probe.waitForFinished(30000) || probe.exitCode() != 0) throw std::runtime_error("Source probe failed");
    MetadataParser parser;
    MediaInfo info;
    QObject::connect(&parser, &MetadataParser::parseCompleted, &parser, [&](const QJsonDocument&, const MediaInfo& parsed) { info = parsed; });
    parser.parse(probe.readAllStandardOutput(), args[3]);
    if (info.durationSec <= 0) throw std::runtime_error("Invalid source duration");
    LlmClient client;
    const QString directory = QFileInfo(args[6]).absoluteFilePath();
    client.setRunLogDirectory(directory);
    const bool preview = args[1] == "--preview-v3";
    client.setOfflinePreview(preview);
    if (args[5] != "-") client.setImportLogDirectory(args[5]);
    client.writeRunArtifact("run.json", {{"source", info.filePath}, {"source_fingerprint", S3Cache::fileFingerprint(info.filePath)}, {"model", settings.llmModel},
        {"min_story_sec", settings.s3MinClipSec}, {"max_story_sec", settings.s3MaxClipSec}, {"diagnostic", true}});
    QEventLoop loop;
    bool success = false;
    QObject::connect(&client, &LlmClient::logMessage, &loop, logLine);
    QObject::connect(&client, &LlmClient::errorOccurred, &loop, [&](const QString& error) {
        if (preview && error.contains("OFFLINE_PREVIEW")) success = true;
        client.writeRunArtifact("evaluation_result.json", {{"status", "failed"}, {"error", error}});
        logLine(error); loop.quit();
    });
    QObject::connect(&client, &LlmClient::storiesFound, &loop, [&](const QList<StoryClip>& stories, const QJsonArray& json) {
        success = !stories.isEmpty();
        client.writeRunArtifact("evaluation_result.json", {{"status", "analysis_complete"}, {"stories", json}, {"rendered", false}});
        logLine(QString("ANALYSIS_COMPLETE: accepted=%1, requested=%2; rendering is a separate step.").arg(stories.size()).arg(count));
        loop.quit();
    });
    logLine(QString("LIVE DEBUG: target=%1, POST limit=%2, duration=%3–%4 seconds; logs=%5")
        .arg(count).arg(settings.s3RequestLimit).arg(settings.s3MinClipSec).arg(settings.s3MaxClipSec).arg(directory));
    client.findStoriesV3(info, StoryNarrative::packedTranscript(phrases), phrases);
    loop.exec();
    if (!success) throw std::runtime_error("No accepted diagnostic stories; see evaluation_result.json");
}
