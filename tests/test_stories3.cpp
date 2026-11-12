#include "appsettings.h"
#include "llmclient.h"
#include "pipelinemanager.h"
#include "s3cache.h"
#include "subtitlecomposer.h"
#include <QRegularExpression>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <stdexcept>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "cannot create fixture");
    require(file.write(bytes) == bytes.size(), "cannot write fixture");
}

static QByteArray readFile(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "missing output fixture");
    return file.readAll();
}

static QJsonArray storyJson(const QString& label, int branch = 1)
{
    QJsonArray segments;
    const QStringList roles{"HOOK", "SETUP", "CLIMAX", "RESOLUTION"};
    const QStringList events{"BEGIN", "SECOND_BEGIN", "TURN", "SECOND_TURN", "HOOK", "SECOND_HOOK", "END", "SECOND_END"};
    const QList<int> ids = branch == 1 ? QList<int>{5, 1, 3, 7} : QList<int>{6, 2, 4, 8};
    for (int i = 0; i < ids.size(); ++i)
        segments.append(QJsonObject{{"start_phrase", ids[i]}, {"end_phrase", ids[i]},
            {"beat", roles[i]}, {"quote", label + "_" + events[ids[i] - 1]},
            {"event", roles[i]}, {"connection", "previous event causes this one"}, {"reason", "necessary"}});
    return QJsonArray{QJsonObject{{"title", label + QString::number(branch)}, {"description", "branch"},
        {"story_summary", "Goal, struggle, outcome."}, {"score", 90}, {"caption", "caption"},
        {"segments", segments}}};
}

class MockLlm {
public:
    QTcpServer server;
    int calls = 0;
    int plans = 0;
    int reviews = 0;
    bool invalidEdits = false;
    MockLlm()
    {
        require(server.listen(QHostAddress::LocalHost), "cannot start local mock");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    QByteArray data = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", data);
                    const int split = data.indexOf("\r\n\r\n");
                    if (split < 0 || socket->property("answered").toBool()) return;
                    int length = 0;
                    for (const auto& line : data.left(split).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (data.size() - split - 4 < length) return;
                    socket->setProperty("answered", true);
                    const auto request = QJsonDocument::fromJson(data.mid(split + 4, length)).object();
                    const auto messages = request.value("messages").toArray();
                    const QString system = messages.first().toObject().value("content").toString();
                    const QString label = data.contains("BETA_") ? "BETA" : "ALPHA";
                    QString content;
                    if (messages.last().toObject().value("content").toString().contains("Ответ пока только план")) {
                        ++plans;
                        QJsonArray branches;
                        for (int branch = 1; branch <= 2; ++branch) {
                            QJsonArray chain;
                            const QList<int> ids = branch == 1 ? QList<int>{1, 3, 7} : QList<int>{2, 4, 8};
                            for (const int id : ids)
                                chain.append(QJsonObject{{"event", "event"}, {"because", "cause"},
                                    {"start_phrase", id}, {"end_phrase", id}});
                            branches.append(QJsonObject{{"branch_id", branch}, {"title", label + QString::number(branch)},
                                {"protagonist", "hero"}, {"goal", "goal " + QString::number(branch)},
                                {"summary", "branch"}, {"ending", "outcome"},
                                {"hook_start_phrase", branch == 1 ? 5 : 6}, {"hook_end_phrase", branch == 1 ? 5 : 6},
                                {"hook_reason", "intrigue"}, {"causal_chain", chain}});
                        }
                        content = QString::fromUtf8(QJsonDocument(QJsonObject{{"action", "plan_stories"},
                            {"branches", branches}}).toJson());
                    } else {
                        if (system.contains("редактор связности")) ++reviews;
                        const QString user = messages.last().toObject().value("content").toString();
                        const auto match = QRegularExpression("\\\"branch_id\\\"\\s*:\\s*(\\d+)").match(user);
                        const int branch = match.hasMatch() ? match.captured(1).toInt() : 1;
                        auto stories = storyJson(label, branch);
                        if (invalidEdits) {
                            auto story = stories[0].toObject(); auto segments = story["segments"].toArray();
                            auto broken = segments[0].toObject(); broken["quote"] = "INVENTED_QUOTE";
                            segments[0] = broken; story["segments"] = segments; stories[0] = story;
                        }
                        content = QString::fromUtf8(QJsonDocument(stories).toJson());
                    }
                    ++calls;
                    const QByteArray body = QJsonDocument(QJsonObject{{"choices", QJsonArray{
                        QJsonObject{{"message", QJsonObject{{"content", content}}}}}}}).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return QString("http://127.0.0.1:%1/v1").arg(server.serverPort()); }
};

// The test binary also acts as an offline ffprobe/ffmpeg/whisper transport stub.
static bool fakeTool(const QStringList& args)
{
    if (args.contains("-show_streams")) {
        const QJsonObject probe{{"format", QJsonObject{{"duration", "7200"}}},
            {"streams", QJsonArray{
                QJsonObject{{"index", 0}, {"codec_type", "video"}},
                QJsonObject{{"index", 1}, {"codec_type", "audio"}, {"tags", QJsonObject{{"language", "rus"}}}},
                QJsonObject{{"index", 2}, {"codec_type", "audio"}, {"tags", QJsonObject{{"language", "eng"}}}}}}};
        QTextStream(stdout) << QJsonDocument(probe).toJson(QJsonDocument::Compact);
        return true;
    }
    if (args.contains("-osrt")) {
        const QString base = args.value(args.indexOf("-of") + 1);
        const QString language = args.value(args.indexOf("-l") + 1);
        writeFile(base + ".srt", "1\n00:00:00,000 --> 00:00:01,000\n" + language.toUtf8() + "_OWN_AUDIO\n\n");
        return true;
    }
    if (args.contains("-y")) {
        if (args.contains("-filter_complex")) {
            const QString filter = args.value(args.indexOf("-filter_complex") + 1);
            require(!filter.contains("force_style="), "old style overrode V3 ASS subtitles");
            require(!filter.contains("acrossfade="), "V3 dialogue was overlapped by a crossfade");
        }
        writeFile(args.last(), "offline media fixture");
        return true;
    }
    return false;
}

static void runTests(const QString& python)
{
    auto& settings = AppSettings::instance();
    settings.minClips = 2; settings.maxClips = 2;
    settings.s3MinClipSec = 60;
    settings.s3MaxClipSec = 300;
    settings.s3MaxSegments = 12;
    settings.s3MaxSpanSec = 0;
    settings.s3MinScore = 60;
    settings.s3QuoteCheck = true;
    settings.agentFramesEnabled = false;
    settings.llmApiKey.clear();
    S3Phrase proof;
    proof.text = "I killed nobody"; proof.startSec = 110; proof.endSec = 116;
    require(!LlmClient::checkQuote("I killed everybody", 100, 160, {proof}).found,
            "contradictory quote accepted");
    require(LlmClient::checkQuote(proof.text, 100, 160, {proof}).found, "exact quote rejected");
    S3Phrase left, omitted, right;
    left.text = "I"; left.startSec = 1; left.endSec = 2;
    omitted.text = "never"; omitted.startSec = 2; omitted.endSec = 3; omitted.flagCheck = true;
    right.text = "killed"; right.startSec = 3; right.endSec = 4;
    require(!LlmClient::checkQuote("I killed", 0, 5, {left, omitted, right}).found,
            "quote jumped over an omitted negation");

    QList<S3Phrase> phrases;
    const QList<double> times{100, 200, 3000, 3100, 3500, 4500, 7000, 7100};
    const QStringList events{"BEGIN", "SECOND_BEGIN", "TURN", "SECOND_TURN", "HOOK", "SECOND_HOOK", "END", "SECOND_END"};
    for (int i = 0; i < times.size(); ++i) {
        S3Phrase phrase; phrase.id = i + 1; phrase.startSec = times[i];
        phrase.endSec = times[i] + (events[i].contains("HOOK") ? 5 : 20);
        phrase.text = "ALPHA_" + events[i]; phrases.append(phrase);
    }
    StoryClip clip;
    clip.title = "ALPHA"; clip.storySummary = "Goal, struggle, outcome."; clip.score = 90;
    const auto jsonSegments = storyJson("ALPHA")[0].toObject()["segments"].toArray();
    for (const auto& value : jsonSegments) {
        const auto json = value.toObject(); const int id = json["start_phrase"].toInt();
        StorySegment seg; seg.startSec = phrases[id - 1].startSec; seg.endSec = phrases[id - 1].endSec;
        seg.startPhrase = id; seg.endPhrase = id;
        seg.beat = json["beat"].toString(); seg.quote = json["quote"].toString();
        seg.event = json["event"].toString(); seg.connection = "cause";
        clip.segments.append(seg);
    }
    LlmClient client;
    QStringList notes;
    require(client.validateStoriesV3({clip}, phrases, notes).size() == 1, "whole-film branch rejected");
    auto noHook = clip; noHook.segments[0].beat = "SETUP";
    require(client.validateStoriesV3({noHook}, phrases, notes).isEmpty(), "story without a hook accepted");
    auto bad = clip; bad.segments.last().quote = "invented ending";
    require(client.validateStoriesV3({bad}, phrases, notes).isEmpty(), "bad quote silently retained");
    bad = clip; bad.segments.removeLast();
    require(client.validateStoriesV3({bad}, phrases, notes).isEmpty(), "missing ending accepted");
    bad = clip; bad.score = 0;
    require(client.validateStoriesV3({bad}, phrases, notes).isEmpty(), "missing score bypassed threshold");
    bad = clip; bad.segments[1].startSec = 95; bad.segments[1].endSec = 125;
    const auto kept = client.validateStoriesV3({bad}, phrases, notes);
    require(kept.size() == 1 && kept.first().segments[1].endSec == 125,
            "quote collapsed editorial interval");

    QTemporaryDir temporary;
    require(temporary.isValid(), "cannot create temporary workspace");
    const QString source = temporary.filePath("source");
    const QString artifact = temporary.filePath("cache");
    const QString dependency = temporary.filePath("dependency");
    writeFile(source, "source"); writeFile(dependency, "one"); writeFile(artifact, "artifact");
    const auto key = S3Cache::makeKey(source, {{"engine", "a"}}, {dependency});
    require(S3Cache::writeMeta(artifact, key) && S3Cache::valid(artifact, key), "cache cannot be reused");
    require(!S3Cache::valid(artifact, S3Cache::makeKey(source, {{"engine", "b"}}, {dependency})),
            "engine switch did not invalidate cache");
    writeFile(dependency, "two");
    require(!S3Cache::valid(artifact, S3Cache::makeKey(source, {{"engine", "a"}}, {dependency})),
            "upstream edit did not invalidate cache");
    writeFile(artifact, "corrupted");
    require(!S3Cache::valid(artifact, key), "corrupted artifact reused");

    MockLlm mock;
    settings.llmBaseUrl = mock.url();
    settings.llmApiKey = "test-secret-do-not-log";
    client.setRunLogDirectory(temporary.filePath("standalone_logs"));
    settings.agentMaxTurns = 2;
    QEventLoop agentLoop;
    bool found = false;
    QObject::connect(&client, &LlmClient::storiesFound, &agentLoop,
        [&](const QList<StoryClip>& result, const QJsonArray& raw) {
            found = result.size() == 2 && result.first().totalSec() == 65
                && !result.first().narrativePlan.isEmpty() && raw.size() == 2
                && mock.calls == 5 && mock.reviews == 2;
            agentLoop.quit();
        });
    QObject::connect(&client, &LlmClient::errorOccurred, &agentLoop, [&](const QString& error) {
        QTextStream(stderr) << error << Qt::endl; agentLoop.quit();
    });
    QTimer::singleShot(15000, &agentLoop, &QEventLoop::quit);
    QString packed;
    for (const auto& phrase : phrases)
        packed += QString("P%1 [%2-%3] %4\n").arg(phrase.id, 6, 10, QChar('0'))
            .arg(phrase.startSec).arg(phrase.endSec).arg(phrase.text);
    MediaInfo info; info.durationSec = 7200;
    client.findStoriesV3(info, packed, phrases);
    agentLoop.exec();
    require(found, "multiple stories with hook/edit/review did not complete before output");
    const QDir logDirectory(temporary.filePath("standalone_logs"));
    require(logDirectory.entryList({"*.request.json"}, QDir::Files).size() == 5, "requests were not logged");
    require(logDirectory.entryList({"*.response.txt"}, QDir::Files).size() == 5, "responses were not logged");
    for (const auto& name : logDirectory.entryList({"*.json"}, QDir::Files))
        require(!readFile(logDirectory.filePath(name)).contains("test-secret-do-not-log"), "API key leaked into logs");
    QList<SrtCue> tinyWords;
    for (int i = 0; i < 4; ++i) {
        SrtCue cue; cue.startSec = i * .18; cue.endSec = cue.startSec + .16; cue.text = "word";
        tinyWords.append(cue);
    }
    require(SubtitleComposer::readablePhrases(tinyWords).size() == 1, "subtitles still flash single words");
    require(SubtitleComposer::readablePhrases(tinyWords, 4, {.36}).size() == 2,
            "subtitle phrase crossed a scene cut");
    const auto assPath = temporary.filePath("readable.ass");
    require(SubtitleComposer::writeAss(tinyWords, assPath), "cannot write styled subtitles");
    require(readFile(assPath).contains("PlayResY: 1920") && readFile(assPath).contains("word word word word"),
            "subtitle resolution or phrase grouping lost");

    const QString scripts = temporary.filePath("scripts");
    QDir().mkpath(scripts);
    require(QFile::copy(QString(CLIPPER_SOURCE_DIR) + "/scripts/merge_transcript.py", scripts + "/merge_transcript.py"),
            "cannot copy real merge script");
    writeFile(scripts + "/asr_bridge.py", R"PY(
import argparse, json, pathlib
p = argparse.ArgumentParser()
p.add_argument('--out'); p.add_argument('--language')
args, _ = p.parse_known_args()
label = 'ALPHA' if 'alpha_clips' in args.out else 'BETA'
events = ['BEGIN','SECOND_BEGIN','TURN','SECOND_TURN','HOOK','SECOND_HOOK','END','SECOND_END']
words = [{'w': label + '_' + event, 't': t, 'e': t + (5 if 'HOOK' in event else 20), 'lp': None, 'chunk': i}
         for i, (event, t) in enumerate(zip(events, [100,200,3000,3100,3500,4500,7000,7100]))]
path = pathlib.Path(args.out)
counter = path.parent / 'asr_calls.txt'
counter.write_text(str(int(counter.read_text()) + 1 if counter.exists() else 1))
path.write_text(json.dumps({'language':args.language,'audio_sec':7200,'words':words,
 'quality':{'n_words':8,'unique_ratio':1}}), encoding='utf-8')
)PY");
    settings.s3ScriptsDir = scripts;
    settings.s3PythonPath = python;
    settings.s3AudioMapEngine = AppSettings::AudioMapEngine::Off;
    settings.s3DiarEngine = AppSettings::DiarEngine::Off;
    settings.s3ReviewEnabled = false;
    settings.editMaxPhraseSec = 30;
    settings.editPaddingMs = 80;
    settings.ffprobePath = QCoreApplication::applicationFilePath();
    settings.ffmpegPath = settings.ffprobePath;
    settings.whisperCliPath = settings.ffprobePath;
    settings.whisperModelPath = "fixture.bin";
    settings.renderRu = true; settings.renderEn = true;
    const QString alpha = temporary.filePath("alpha.mkv");
    const QString beta = temporary.filePath("beta.mkv");
    writeFile(alpha, "alpha"); writeFile(beta, "beta");
    PipelineManager manager;
    manager.setMode(PipelineManager::Mode::Stories3);
    QStringList errors;
    QEventLoop queueLoop;
    QObject::connect(&manager, &PipelineManager::logMessage, &queueLoop, [&](const QString& message) {
        if (message.contains("[ОШИБКА]")) errors.append(message);
    });
    QObject::connect(&manager, &PipelineManager::queueFinished, &queueLoop, &QEventLoop::quit);
    QTimer::singleShot(30000, &queueLoop, &QEventLoop::quit);
    manager.addFilesToQueue({alpha, beta});
    queueLoop.exec();
    for (const auto& error : errors) QTextStream(stderr) << error << Qt::endl;
    require(errors.isEmpty(), "queue failed");
    for (const QString name : {QString("alpha"), QString("beta")}) {
        const auto base = temporary.filePath(name + "_clips");
        const auto ru = readFile(base + "/temp/story_1_ru.s3.srt");
        require(ru.contains(name.toUpper().toUtf8()), "previous movie leaked into subtitles");
        require(!ru.contains(name == "alpha" ? "BETA" : "ALPHA"), "foreign movie words found");
        require(readFile(base + "/temp/story_1_en.srt").contains("en_OWN_AUDIO"),
                "English version reused Russian transcript");
        require(QFileInfo::exists(base + "/ru/story_01.mp4") && QFileInfo::exists(base + "/en/story_01.mp4"),
                "missing render task output");
        require(QFileInfo::exists(base + "/ru/story_02.mp4") && QFileInfo::exists(base + "/en/story_02.mp4"),
                "second distinct story was not rendered");
        const auto saved = QJsonDocument::fromJson(readFile(base + "/stories.json")).array();
        require(saved.size() == 2, "saved EDL contains only one story");
        const auto json = saved.first().toObject();
        require(json["segments"].toArray().first().toObject()["beat"].toString() == "HOOK",
                "saved story does not start with a hook");
        require(json.value("duration_sec").toDouble() > 60, "saved EDL does not include final padding");
    }
    settings.s3ChunkSec += 1;
    manager.addFilesToQueue({alpha});
    queueLoop.exec();
    require(readFile(temporary.filePath("alpha_clips/temp/asr_calls.txt")) == "2",
            "changed ASR settings reused old transcript");
    require(mock.plans == 4 && mock.reviews == 8, "narrative stages skipped in queue");
    const auto originalEdl = readFile(temporary.filePath("alpha_clips/stories.json"));
    mock.invalidEdits = true;
    manager.addFilesToQueue({alpha}); queueLoop.exec();
    require(!errors.isEmpty(), "failed rerun was reported as successful");
    require(readFile(temporary.filePath("alpha_clips/stories.json")) == originalEdl,
            "failed rerun replaced old EDL");
    require(readFile(temporary.filePath("alpha_clips/temp/asr_calls.txt")) == "2",
            "LLM-only rerun unnecessarily repeated ASR");
    const QDir logs(temporary.filePath("alpha_clips/logs"));
    const auto runs = logs.entryList({"run_*"}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    require(runs.size() == 3, "rerun logs overwrote an earlier run");
    const QDir failedRun(logs.filePath(runs.last()));
    require(readFile(failedRun.filePath("result.json")).contains("failed")
            && !failedRun.entryList({"*.response.txt"}, QDir::Files).isEmpty(),
            "failed run lost its status or raw LLM responses");
    QTextStream(stdout) << "PASS: quote evidence, complete branches, cache dependencies, "
                          "plan/edit/review, two-file queue, language-specific subtitles, saved EDL" << Qt::endl;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        if (fakeTool(app.arguments())) return 0;
        if (app.arguments().contains("--compose")) {
            require(argc == 4, "--compose input.srt output.ass");
            require(SubtitleComposer::writeAss(SrtUtils::parseFile(QString::fromLocal8Bit(argv[2])),
                        QString::fromLocal8Bit(argv[3])), "subtitle composition failed");
            return 0;
        }
        require(argc >= 2, "pass Python interpreter");
        runTests(QString::fromLocal8Bit(argv[1]));
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << "FAIL: " << error.what() << Qt::endl;
        return 1;
    }
}
