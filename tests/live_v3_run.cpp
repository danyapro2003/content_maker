#include "appsettings.h"
#include "llmclient.h"
#include "storynarrative.h"
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
    settings.s3RequestLimit = count == 1 ? 12 : 36;
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
    client.setAnalysisDirectory(QFileInfo(directory).absolutePath() + "/analysis");
    if (args[5] != "-") client.setImportLogDirectory(args[5]);
    client.writeRunArtifact("run.json", {{"source", info.filePath}, {"model", settings.llmModel},
        {"min_story_sec", settings.s3MinClipSec}, {"max_story_sec", settings.s3MaxClipSec}, {"diagnostic", true}});
    QEventLoop loop;
    bool success = false;
    QObject::connect(&client, &LlmClient::logMessage, &loop, logLine);
    QObject::connect(&client, &LlmClient::errorOccurred, &loop, [&](const QString& error) {
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
