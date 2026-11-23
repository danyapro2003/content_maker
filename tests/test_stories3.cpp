#include "storynarrative.h"
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

void testEventIndex();
void testStoryProtocol();
void testStoryRunStore();
void testStoryTimeline();
void testStoryContext();
void checkLiveModel(const QString&, const QString&);
void liveV3Run(const QStringList&);
void prepareV3Source(const QString&, const QString&);
void renderV3Evaluation(const QStringList&);
void retryLoggedV3Request(const QStringList&);
void replayV3Run(const QString&, const QString&);
void replayEventIndex(const QString& directory, const QString& packedPath);

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
    const QList<int> ids = branch == 1 ? QList<int>{1, 3, 5, 7} : QList<int>{2, 4, 6, 8};
    for (int i = 0; i < ids.size(); ++i)
        segments.append(QJsonObject{{"start_phrase", ids[i]}, {"end_phrase", ids[i]},
            {"event_ids", QJsonArray{QString("C1E%1").arg(ids[i])}}, {"beat", roles[i]}, {"quote", label + "_" + events[ids[i] - 1]},
            {"event", roles[i]}, {"connection", "previous event causes this one"}, {"reason", "necessary"}});
    return QJsonArray{QJsonObject{{"title", label + QString::number(branch)}, {"description", "branch"},
        {"opening_mode", "natural"}, {"hook_reason", "intrigue"}, {"story_summary", "Goal, struggle, outcome."}, {"score", 90}, {"caption", "caption"},
        {"segments", segments}}};
}

static int idsForMissing(int branch) { return branch == 1 ? 5 : 6; }

class MockLlm {
public:
    QTcpServer server;
    int calls = 0;
    int plans = 0;
    int reviews = 0;
    bool invalidEdits = false;
    bool visualPlan = false, visualUnresolved = false;
    int replans = 0;
    bool partialCaptions = false, badCaption = false, captionAuditFail = false, lateCaption = false;
    int invalidBranch = 0;
    int emptyBranch = 0;
    int longBranch = 0;
    int shortenings = 0;
    bool omitRequired = false;
    bool viewerFailOnce = false;
    bool viewerAlwaysFails = false;
    bool fidelityFailOnce = false;
    bool malformedAudit = false;
    bool unchangedRepair = false;
    int indexFailure = 0; // 1: recover, 2: bad repair, 3: omitted repair, 4: bad envelope
    int repairs = 0, fidelities = 0, indexes = 0;
    QList<QJsonObject> requests;
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
                    requests.append(request);
                    require(request["max_completion_tokens"].toInt() == AppSettings::instance().llmMaxOutputTokens,
                            "configured output limit missing from actual HTTP request");
                    const auto messages = request.value("messages").toArray();
                    const QString system = messages.first().toObject().value("content").toString();
                    const QString label = data.contains("BETA_") ? "BETA" : "ALPHA";
                    QString content;
                    if (system.contains("STAGE_V4_CAPTION_AUDIT")) {
                        content = partialCaptions ? R"({"approved_blocks":[0]})" : captionAuditFail ? R"({"pass":false,"reason":"unsupported claim"})" : R"({"pass":true,"reason":""})";
                    } else if (system.contains("STAGE_V4_CAPTIONS")) {
                        content = partialCaptions ? R"({"captions":[{"block":0,"offset_sec":1,"text":"A difficult choice","translation":"Трудный выбор","evidence_quote":"I must choose"},{"block":1,"text":"Wrong quote","evidence_quote":"invented"},{"block":2,"text":"Another choice","translation":"Другой выбор","evidence_quote":"I chose"}]})" : lateCaption ? R"({"captions":[{"block":0,"offset_sec":4,"text":"A difficult choice","evidence_quote":"I must choose"}]})" : badCaption ? R"({"captions":[{"block":0,"text":"A difficult choice","evidence_quote":"invented"}]})"
                            : R"({"captions":[{"block":0,"offset_sec":1,"text":"A difficult choice","viewer_gap":"Why he hesitates","evidence_quote":"I must choose"}]})";
                    } else if (system.contains("STAGE_V5_DISCOVERY")) {
                        content = R"({"candidates":[{"title":"shared decision","expectation":"Will they agree?","payoff":"agreement","hook_event":"C1E1","payoff_event":"C1E7"},{"title":"another angle","expectation":"Who gives in?","payoff":"agreement","hook_event":"C1E1","payoff_event":"C1E7"},{"title":"invalid candidate","expectation":"?","payoff":"unknown","hook_event":"BAD","payoff_event":"BAD"}]})";
                    } else if (system.contains("STAGE_INDEX")) {
                        ++indexes;
                        QJsonArray events;
                        const auto user = messages.last().toObject()["content"].toString();
                        const int chunk = QRegularExpression("C(\\d+)E1").match(user).captured(1).toInt();
                        auto matches = QRegularExpression("P(\\d+) \\[([^\\]]+)\\] [^\\n]*?(" + label + "_[A-Z_]+)").globalMatch(user);
                        while (matches.hasNext()) {
                            const auto match = matches.next();
                            const int id = match.captured(1).toInt();
                            events.append(QJsonObject{{"event_id", QString("C%1E%2").arg(chunk).arg(id)},
                                {"observed", "observed fact"}, {"evidence_phrase", id}, {"quote", match.captured(3)},
                                {"viewer_requires", QJsonArray{}}, {"viewer_learns", QJsonArray{"fact"}},
                                {"character_knowledge", QJsonArray{"hero knows fact"}}});
                        }
                        if (user.contains("STAGE_INDEX_REPAIR")) {
                            const auto start = user.indexOf("Цели исправления:\n") + QString("Цели исправления:\n").size();
                            const auto end = user.indexOf("\nМатериал:\n", start);
                            const auto pending = QJsonDocument::fromJson(user.mid(start, end - start).toUtf8()).array();
                            QJsonArray repairs;
                            if (indexFailure != 3) for (const auto& value : pending) {
                                const auto target = value.toObject();
                                const auto id = target["event"].toObject()["event_id"];
                                for (const auto& good : events) if (good.toObject()["event_id"] == id) {
                                    auto fixed = good.toObject();
                                    if (indexFailure == 2) fixed["quote"] = "INVENTED_INDEX_QUOTE";
                                    repairs.append(QJsonObject{{"repair_id", target["repair_id"]}, {"event", fixed}});
                                }
                            }
                            content = QString::fromUtf8(QJsonDocument(QJsonObject{{"action", "repair_events"}, {"repairs", repairs}}).toJson());
                        } else if (indexFailure == 4 && indexes == 1) {
                            content = "{}";
                        } else {
                            if (indexFailure >= 1 && indexFailure <= 3 && !events.isEmpty()) {
                                auto broken = events.first().toObject(); broken["quote"] = "INVENTED_INDEX_QUOTE"; events[0] = broken;
                            }
                            content = QString::fromUtf8(QJsonDocument(QJsonObject{{"action", "index_events"}, {"events", events}}).toJson());
                        }
                    } else if (system.contains("STAGE_PLAN") || system.contains("STAGE_REPLAN")) {
                        ++plans;
                        const bool replan = system.contains("STAGE_REPLAN");
                        if (replan) ++replans;
                        const int selected = replan && messages.last().toObject()["content"].toString().contains("\"branch_id\": \"B2\"") ? 2 : 1;
                        QJsonArray branches;
                        for (int branch = replan ? selected : 1; branch <= (replan ? selected : 2); ++branch) {
                            const QJsonArray required = branch == 1 ? QJsonArray{"C1E1", "C1E3", "C1E7"} : QJsonArray{"C1E2", "C1E4", "C1E8"};
                            QJsonArray chain;
                            for (int i = 0; i < required.size(); ++i) chain.append(QJsonObject{{"event_id", required[i]},
                                {"function", i == required.size() - 1 ? "payoff" : "cause"},
                                {"depends_on", i ? QJsonArray{required[i - 1]} : QJsonArray{}}});
                            branches.append(QJsonObject{{"branch_id", QString("B%1").arg(branch)}, {"title", label + QString::number(branch)},
                                {"dramatic_question", "goal " + QString::number(branch)}, {"ending_effect", "reaction"},
                                {"hook", QJsonObject{{"event_id", required[0]}, {"mode", "natural"}, {"why_interesting", "intrigue"}}},
                                {"chain", chain}, {"missing_evidence", visualPlan && (!replan || visualUnresolved) ? QJsonArray{"execution not shown"} : QJsonArray{}},
                                {"source_unit_ids", branch == 1 ? QJsonArray{"U1", "U3", "U5", "U7"} : QJsonArray{"U2", "U4", "U6", "U8"}},
                                {"required_events", required}, {"ending_event", branch == 1 ? "C1E7" : "C1E8"}});
                        }
                        content = QString::fromUtf8(QJsonDocument(QJsonObject{{"protocol_version", 2}, {"action", "plan_stories"},
                            {"primary", branches}, {"reserve", QJsonArray{}}}).toJson());
                    } else if (system.contains("STAGE_VIEWER") || system.contains("STAGE_FIDELITY")) {
                        const bool viewer = system.contains("STAGE_VIEWER");
                        if (viewer) ++reviews; else ++fidelities;
                        const bool fail = viewer ? (viewerAlwaysFails || (viewerFailOnce && reviews == 1))
                                                 : (fidelityFailOnce && fidelities == 1);
                        content = malformedAudit ? "{\"pass\":true}" : QString::fromUtf8(QJsonDocument(QJsonObject{
                            {"protocol_version", 2}, {"action", "audit_story"}, {"understood_story", "Observed story"}, {"verdict", fail ? "repair" : "pass"},
                            {"issues", fail ? QJsonArray{QJsonObject{{"block_id", "V2"}, {"kind", "missing_context"},
                                {"severity", "blocking"}, {"evidence", "Transition from first block to second"}, {"problem", "Missing motivation"}}} : QJsonArray{}}
                        }).toJson());
                    } else {
                        if (system.contains("STAGE_REPAIR")) ++repairs;
                        const QString user = messages[1].toObject().value("content").toString();
                        const auto match = QRegularExpression("\\\"branch_id\\\"\\s*:\\s*\\\"?B?(\\d+)").match(user);
                        const int branch = match.hasMatch() ? match.captured(1).toInt() : 1;
                        auto stories = storyJson(label, branch);
                        if (invalidEdits || omitRequired || invalidBranch == branch) {
                            auto story = stories[0].toObject(); auto segments = story["segments"].toArray();
                            auto broken = segments[1].toObject();
                            if (invalidEdits || invalidBranch == branch) broken["end_phrase"] = 99999;
                            if (omitRequired) { broken["start_phrase"] = idsForMissing(branch); broken["end_phrase"] = idsForMissing(branch); }
                            segments[1] = broken; story["segments"] = segments; stories[0] = story;
                        }
                        auto story = stories[0].toObject();
                        auto segments = story["segments"].toArray();
                        if (system.contains("STAGE_REPAIR") && !unchangedRepair && (viewerFailOnce || fidelityFailOnce || viewerAlwaysFails)
                            && repairs % 2 == 1) segments.removeAt(2);
                        for (int i = 0; i < segments.size(); ++i) {
                            auto segment = segments[i].toObject();
                            segment["block_id"] = QString("B%1_%2").arg(branch).arg(i + 1);
                            segment["role"] = segment["beat"].toString().toLower();
                            segments[i] = segment;
                        }
                        story["segments"] = segments;
                        if (system.contains("STAGE_SHORTEN")) ++shortenings;
                        if (longBranch == branch && !system.contains("STAGE_SHORTEN")) {
                            auto continuous = segments.first().toObject();
                            continuous["end_phrase"] = segments.last().toObject()["end_phrase"];
                            story["segments"] = QJsonArray{continuous};
                        }
                        QJsonObject response{{"protocol_version", 2}, {"action", "edit_story"},
                            {"branch_id", QString("B%1").arg(branch)}, {"story", story}};
                        const auto baseHash = QRegularExpression("base_draft_hash: ([a-f0-9]{64})").match(user);
                        if (baseHash.hasMatch()) response["base_draft_hash"] = baseHash.captured(1);
                        content = QString::fromUtf8(QJsonDocument(response).toJson());
                        if (emptyBranch == branch) content.clear();
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
        if (args.last().endsWith(".part.mp4")) {
            const auto fixture = QJsonDocument::fromJson(readFile(args.last())).object();
            const double duration = fixture["duration"].toDouble() + (qEnvironmentVariableIsSet("CLIPPER_TEST_BAD_PROBE") ? 100 : 0);
            const QJsonObject probe{{"format", QJsonObject{{"duration", duration}}}, {"streams", QJsonArray{
                QJsonObject{{"codec_type", "video"}, {"avg_frame_rate", "25/1"}}, QJsonObject{{"codec_type", "audio"}}}}};
            QTextStream(stdout) << QJsonDocument(probe).toJson(QJsonDocument::Compact); return true;
        }
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
        if (args.last().endsWith(".part.mp4")) {
            if (qEnvironmentVariableIsSet("CLIPPER_TEST_FAIL_EN") && args.last().contains("/en/"))
                throw std::runtime_error("scripted EN render failure");
            double duration = 0;
            for (int i = 0; i + 1 < args.size(); ++i) if (args[i] == "-t") duration += args[i + 1].toDouble();
            writeFile(args.last(), QJsonDocument(QJsonObject{{"duration", duration}}).toJson());
        } else writeFile(args.last(), "offline media fixture");
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
    for (int scenarioId = 0; scenarioId < 5; ++scenarioId) {
        MockLlm mock; mock.badCaption = scenarioId == 1; mock.captionAuditFail = scenarioId == 2; mock.lateCaption = scenarioId == 3;
        settings.llmBaseUrl = mock.url(); LlmClient captionClient; QEventLoop loop;
        mock.partialCaptions = scenarioId == 4; captionClient.setV5Discovery(mock.partialCaptions);
        bool completed = false, passed = false;
        QTimer timer; timer.setSingleShot(true); timer.start(5000);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        captionClient.createSceneCaptionsV4({{1, 0, 5, "I must choose"}, {2, 5, 10, "Actually"}, {3, 10, 15, "I chose"}}, {5, 10}, "en",
            [&](bool ok, const QList<SrtCue>& captions, const QString&) {
                completed = true; passed = ok;
                if (ok) require(captions.size() == 1 && captions.first().startSec == 1 && captions.first().endSec <= 5, "invalid caption timing");
                if (mock.partialCaptions && ok) require(captions.first().translation == "Трудный выбор", "approved translation lost");
                loop.quit();
            }, {}, mock.partialCaptions ? "ru" : "");
        loop.exec(); require(completed && passed == (scenarioId == 0 || scenarioId == 4), "caption evidence/audit validation failed");
        require(mock.requests.size() == ((scenarioId == 1 || scenarioId == 3) ? 1 : 2), "caption validation request count wrong");
        const auto requestText = mock.requests.first()["messages"].toArray().last().toObject()["content"].toString();
        require(requestText.contains("film_events") && requestText.contains("source_phrases") && requestText.contains("clip_start"),
                "caption request missing context or clip timing");
    }
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
    clip.hookReason = "a compelling situation";
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
    auto noHook = clip; noHook.hookReason.clear();
    require(client.validateStoriesV3({noHook}, phrases, notes).isEmpty(), "story without a hook accepted");
    auto bad = clip; bad.segments.last().quote = "invented ending";
    require(client.validateStoriesV3({bad}, phrases, notes).isEmpty(), "bad quote silently retained");
    bad = clip; bad.segments.removeLast();
    require(client.validateStoriesV3({bad}, phrases, notes).isEmpty(), "missing ending accepted");
    bad = clip; bad.score = 0;
    require(client.validateStoriesV3({bad}, phrases, notes).isEmpty(), "missing score bypassed threshold");
    bad = clip; bad.segments[0].startSec = 95; bad.segments[0].endSec = 145;
    const auto kept = client.validateStoriesV3({bad}, phrases, notes);
    require(kept.size() == 1 && kept.first().segments[0].endSec == 145,
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
            found = result.size() == 2 && result.first().totalSec() >= 65 && result.first().totalSec() < 70
                && !result.first().narrativePlan.isEmpty() && raw.size() == 2
                && mock.calls == 8 && mock.reviews == 2 && mock.fidelities == 2 && mock.indexes == 1;
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
    require(logDirectory.entryList({"*.request.json"}, QDir::Files).size() == 8, "requests were not logged");
    require(logDirectory.entryList({"*.response.txt"}, QDir::Files).size() == 8, "responses were not logged");
    for (const auto& name : logDirectory.entryList({"*.json"}, QDir::Files))
        require(!readFile(logDirectory.filePath(name)).contains("test-secret-do-not-log"), "API key leaked into logs");
    // Viewer sees only selected dialogue, never metadata, another branch or omitted source.
    int viewers = 0;
    for (const auto& request : mock.requests) {
        const auto messages = request["messages"].toArray();
        if (!messages.first().toObject()["content"].toString().contains("STAGE_VIEWER")) continue;
        ++viewers;
        const auto user = messages[1].toObject()["content"].toString();
        require(messages.size() == 2 && !user.contains("branch_id") && !user.contains("goal")
                && !user.contains("C1E") && !user.contains("P000") && !user.contains("Источник"),
                "editorial/source metadata leaked to blind viewer");
        require(viewers == 1 ? !user.contains("SECOND_") : !user.contains("ALPHA_BEGIN"),
                "unselected dialogue leaked to blind viewer");
    }
    require(viewers == 2, "viewer audit skipped");
    settings.s3RunMode = AppSettings::StoryRunMode::Continue;
    found = false;
    client.findStoriesV3(info, packed, phrases); agentLoop.exec();
    require(found && mock.calls == 8, "resume repaid completed stages");
    const QDir analysis(temporary.filePath("analysis"));
    const auto keys = analysis.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    require(keys.size() == 1, "unexpected analysis cache key");
    const QDir cached(analysis.filePath(keys.first()));
    bool markedPending = false;
    for (const auto& name : cached.entryList({"request_*.json"}, QDir::Files)) {
        auto envelope = QJsonDocument::fromJson(readFile(cached.filePath(name))).object();
        if (envelope["data"].toObject()["stage"] != "index_1") continue;
        envelope["status"] = "pending";
        writeFile(cached.filePath(name), QJsonDocument(envelope).toJson());
        markedPending = true;
    }
    require(markedPending, "no persistent request ledger");
    settings.s3RunMode = AppSettings::StoryRunMode::FullAnalysis;
    LlmClient uncertain;
    uncertain.setRunLogDirectory(temporary.filePath("uncertain_logs"));
    QEventLoop unknownLoop; bool blockedUnknown = false;
    QObject::connect(&uncertain, &LlmClient::errorOccurred, &unknownLoop, [&](const QString& error) {
        blockedUnknown = error.contains("OUTCOME_UNKNOWN"); unknownLoop.quit();
    });
    QTimer::singleShot(1000, &unknownLoop, &QEventLoop::quit);
    uncertain.findStoriesV3(info, packed, phrases); unknownLoop.exec();
    require(blockedUnknown && mock.calls == 8, "unknown request automatically repeated after restart");
    settings.s3RunMode = AppSettings::StoryRunMode::RebuildStories;
    auto natural = clip;
    natural.segments = {clip.segments.first()};
    natural.segments[0].endSec = natural.segments[0].startSec + 65;
    natural.segments[0].beat = "REACTION";
    notes.clear();
    require(client.validateStoriesV3({}, phrases, notes).isEmpty(), "empty montage accepted");
    LlmClient standalone;
    notes.clear();
    require(standalone.validateStoriesV3({natural}, phrases, notes).size() == 1,
            "natural opening/long single interval still requires four dramatic roles");
    natural.openingMode = "cold_open";
    notes.clear();
    require(standalone.validateStoriesV3({natural}, phrases, notes).isEmpty(), "long cold open accepted");
    const auto windows = StoryNarrative::chunks(phrases, 180);
    QSet<int> seen;
    for (const auto& window : windows) for (const auto& phrase : window) seen.insert(phrase.id);
    require(windows.size() > 1 && seen.size() == phrases.size(), "event chunking loses source phrases");
    testEventIndex();
    testStoryProtocol();
    testStoryRunStore();
    testStoryTimeline();
    testStoryContext();
    QString auditError;
    require(!StoryNarrative::acceptAudit({{"action", "audit"}, {"pass", true},
                {"understood_story", "story"}, {"issues", QJsonArray{QJsonObject{{"block", 1}, {"kind", "gap"}, {"problem", "gap"}}}}}, 2, auditError),
            "contradictory audit accepted");
    for (bool unresolved : {false, true}) {
        MockLlm scenario; scenario.visualPlan = true; scenario.visualUnresolved = unresolved;
        settings.llmBaseUrl = scenario.url();
        LlmClient candidate; QEventLoop loop;
        candidate.setV5Discovery(!unresolved);
        bool success = false, failed = false;
        QObject::connect(&candidate, &LlmClient::storiesFound, &loop, [&](const QList<StoryClip>&, const QJsonArray&) { success = true; loop.quit(); });
        QObject::connect(&candidate, &LlmClient::errorOccurred, &loop, [&](const QString&) { failed = true; loop.quit(); });
        QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(10000); candidate.findStoriesV3(info, packed, phrases); loop.exec();
        for (const auto& req : scenario.requests) {
            const auto msgs = req["messages"].toArray();
            if (msgs.first().toObject()["content"].toString().contains("STAGE_PLAN")) {
                const auto prompt = msgs.last().toObject()["content"].toString();
                require(prompt.contains("shared decision") == !unresolved && prompt.contains("another angle") == !unresolved && !prompt.contains("invalid candidate"),
                        "V5 candidates lost, ungrounded, or leaked into V3");
            }
        }
        require(scenario.replans == 2, "each visual branch must have exactly one replan");
        require(unresolved ? (failed && !success && scenario.reviews == 0) : (success && !failed && scenario.reviews == 2),
                "visual evidence repair must recover or reject without bypassing audits");
    }
    auto runScenario = [&](bool failOnce, bool alwaysFail, bool malformed, bool omit, bool fidelityFail, bool unchanged = false) {
        MockLlm scenario;
        scenario.viewerFailOnce = failOnce; scenario.viewerAlwaysFails = alwaysFail;
        scenario.malformedAudit = malformed; scenario.omitRequired = omit; scenario.fidelityFailOnce = fidelityFail;
        scenario.unchangedRepair = unchanged;
        settings.llmBaseUrl = scenario.url();
        LlmClient candidate;
        QEventLoop loop;
        bool success = false, failed = false;
        QObject::connect(&candidate, &LlmClient::storiesFound, &loop, [&](const QList<StoryClip>&, const QJsonArray&) { success = true; loop.quit(); });
        QObject::connect(&candidate, &LlmClient::errorOccurred, &loop, [&](const QString&) { failed = true; loop.quit(); });
        QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(10000);
        candidate.findStoriesV3(info, packed, phrases); loop.exec();
        if (alwaysFail || malformed || omit) require(failed && !success, "failed audit/missing required event reached render");
        else require(success && !failed, "repair did not complete");
        if (failOnce || fidelityFail) require(scenario.repairs == 1 && scenario.reviews == 3,
                "repair was not checked again by isolated viewer");
        if (alwaysFail) require(scenario.repairs == (unchanged ? 2 : 4) && scenario.reviews == (unchanged ? 2 : 4), "unchanged edit was re-audited / repair budget exceeded");
        if (omit) require(scenario.reviews == 0, "missing mandatory event reached reviewer");
    };
    runScenario(true, false, false, false, false);
    runScenario(false, false, false, false, true);
    runScenario(false, true, false, false, false);
    runScenario(false, false, true, false, false);
    runScenario(false, false, false, true, false);
    runScenario(false, true, false, false, false, true);
    for (int mode : {1, 2, 3, 4}) {
        MockLlm scenario; scenario.indexFailure = mode;
        settings.llmBaseUrl = scenario.url();
        LlmClient candidate;
        const auto logDir = temporary.filePath(QString("index_repair_%1").arg(mode));
        candidate.setRunLogDirectory(logDir);
        QEventLoop loop;
        bool success = false, failed = false;
        QObject::connect(&candidate, &LlmClient::storiesFound, &loop, [&](const QList<StoryClip>&, const QJsonArray&) { success = true; loop.quit(); });
        QObject::connect(&candidate, &LlmClient::errorOccurred, &loop, [&](const QString&) { failed = true; loop.quit(); });
        QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(10000);
        candidate.findStoriesV3(info, packed, phrases); loop.exec();
        const bool expectedSuccess = mode == 1 || mode == 4;
        require(success == expectedSuccess && failed != expectedSuccess, "index repair outcome wrong");
        require(scenario.indexes == (expectedSuccess ? 2 : 3), "index repair budget not respected");
        const auto saved = QJsonDocument::fromJson(readFile(logDir + "/index_1.events.json")).object();
        require(saved["events"].toArray().size() == (expectedSuccess ? 8 : 7), "checked events lost/duplicated during repair");
        require(saved["complete"].toBool() == expectedSuccess, "partial map falsely marked complete");
        if (!expectedSuccess) require(scenario.plans == 0 && saved["pending"].toArray().size() == 1,
                                      "unresolved index proceeded to planning");
        if (mode != 4) {
            const auto repairMessages = scenario.requests[1]["messages"].toArray();
            require(repairMessages.size() == 2 && repairMessages.last().toObject()["content"].toString().contains("R1"),
                    "index repair regenerated entire history instead of failed record");
        }
    }
    settings.llmBaseUrl = mock.url();
    QList<SrtCue> tinyWords;
    for (int i = 0; i < 4; ++i) {
        SrtCue cue; cue.startSec = i * .18; cue.endSec = cue.startSec + .16; cue.text = "word";
        tinyWords.append(cue);
    }
    require(SubtitleComposer::readablePhrases(tinyWords).size() == 1, "subtitles still flash single words");
    require(SubtitleComposer::readablePhrases(tinyWords, 4, {.36}).size() == 2,
            "subtitle phrase crossed a scene cut");
    const auto assPath = temporary.filePath("readable.ass");
    const QString v4Path = assPath + ".v4.ass";
    require(SubtitleComposer::writeAssV4({{1, 0, 1, "one"}, {2, .5, 1.5, "two"}},
        {{0, 0, 1, "Scene {text}"}}, v4Path, 64, {.8}), "cannot write V4 subtitles");
    QFile v4File(v4Path); require(v4File.open(QIODevice::ReadOnly), "missing V4 ASS");
    const auto v4Ass = v4File.readAll();
    require(v4Ass.contains("0:00:00.00,0:00:00.50,Word") && v4Ass.contains("0:00:00.50,0:00:00.80,Word"),
            "V4 words overlap or cross a cut");
    require(v4Ass.contains("Style: Scene") && !v4Ass.contains("Scene {text}"), "V4 captions not escaped/styled");
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
    const auto framing = scripts + "/framing_fixture.py";
    writeFile(framing, R"PY(
import json, pathlib, sys
times = sys.argv[5:sys.argv.index('--reaction-room')]
filters = [f'[{i}:v:0]setpts=PTS-STARTPTS[vp{i}];' for i in range(len(times)//2)]
pathlib.Path(sys.argv[2]).write_text(json.dumps(dict(filters=filters)))
)PY");
    qputenv("CLIPPER_V5_PYTHON", python.toUtf8());
    qputenv("CLIPPER_V5_FRAMING_SCRIPT", framing.toUtf8());
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
    manager.setMode(PipelineManager::Mode::Stories5);
    QStringList errors;
    QEventLoop queueLoop;
    QObject::connect(&manager, &PipelineManager::logMessage, &queueLoop, [&](const QString& message) {
        if (message.contains("[ОШИБКА]")) errors.append(message);
    });
    bool queueCompleted = false;
    QObject::connect(&manager, &PipelineManager::queueFinished, &queueLoop, [&]() { queueCompleted = true; queueLoop.quit(); });
    QTimer queueDeadline; queueDeadline.setSingleShot(true);
    QObject::connect(&queueDeadline, &QTimer::timeout, &queueLoop, &QEventLoop::quit);
    auto runQueue = [&](const QStringList& files) {
        queueCompleted = false; queueDeadline.start(30000); manager.addFilesToQueue(files);
        if (!queueCompleted) queueLoop.exec();
        queueDeadline.stop(); require(queueCompleted, "queue watchdog expired before completion");
    };
    runQueue({alpha, beta});
    for (const auto& error : errors) QTextStream(stderr) << error << Qt::endl;
    require(errors.isEmpty(), "queue failed");
    for (const QString name : {QString("alpha"), QString("beta")}) {
        const auto base = temporary.filePath(name + "_clips");
        const auto manifest = QJsonDocument::fromJson(readFile(base + "/latest_outputs.json")).object();
        const auto outputs = manifest["output_directory"].toString();
        const auto renderTemp = base + "/temp/runs/" + QFileInfo(outputs).fileName();
        require(manifest["status"] == "completed" && manifest["rendered_stories"] == 2 && manifest["created_videos"] == 4,
                "output manifest counts language versions as stories");
        const auto ru = readFile(renderTemp + "/story_1_ru.s3.srt");
        require(ru.contains(name.toUpper().toUtf8()), "previous movie leaked into subtitles");
        require(!ru.contains(name == "alpha" ? "BETA" : "ALPHA"), "foreign movie words found");
        require(readFile(renderTemp + "/story_1_en.srt").contains("en_OWN_AUDIO"),
                "English version reused Russian transcript");
        require(QFileInfo::exists(outputs + "/ru/story_01.mp4") && QFileInfo::exists(outputs + "/en/story_01.mp4"),
                "missing render task output");
        require(QFileInfo::exists(outputs + "/ru/story_02.mp4") && QFileInfo::exists(outputs + "/en/story_02.mp4"),
                "second distinct story was not rendered");
        const auto saved = QJsonDocument::fromJson(readFile(outputs + "/stories.json")).array();
        require(saved.size() == 2, "saved EDL contains only one story");
        const auto json = saved.first().toObject();
        require(json["segments"].toArray().first().toObject()["beat"].toString().toLower() == "hook",
                "saved story does not start with a hook");
        require(json.value("duration_sec").toDouble() > 60, "saved EDL does not include final padding");
    }
    settings.s3ChunkSec += 1;
    runQueue({alpha});
    require(readFile(temporary.filePath("alpha_clips/temp/asr_calls.txt")) == "2",
            "changed ASR settings reused old transcript");
    require(mock.plans == 4 && mock.reviews == 8 && mock.fidelities == 8, "narrative stages skipped in queue");
    const auto previousOutputs = QJsonDocument::fromJson(readFile(temporary.filePath("alpha_clips/latest_outputs.json"))).object();
    const auto originalEdlPath = previousOutputs["output_directory"].toString() + "/stories.json";
    const auto originalEdl = readFile(originalEdlPath);
    mock.invalidEdits = true;
    runQueue({alpha});
    require(!errors.isEmpty(), "failed rerun was reported as successful");
    require(readFile(originalEdlPath) == originalEdl,
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
    require(QJsonDocument::fromJson(readFile(temporary.filePath("alpha_clips/latest_outputs.json"))).object() == previousOutputs,
            "failed run replaced latest successful output pointer");
    mock.invalidEdits = false; mock.invalidBranch = 2;
    runQueue({alpha});
    auto partial = QJsonDocument::fromJson(readFile(temporary.filePath("alpha_clips/latest_attempt.json"))).object();
    require(partial["status"] == "partial" && partial["accepted_stories"] == 1 && partial["created_videos"] == 2,
            "one accepted story lost because another failed");
    mock.invalidBranch = 0;
    mock.emptyBranch = 1;
    runQueue({alpha});
    partial = QJsonDocument::fromJson(readFile(temporary.filePath("alpha_clips/latest_attempt.json"))).object();
    require(partial["status"] == "partial" && partial["accepted_stories"] == 1 && partial["created_videos"] == 2,
            "empty first branch stopped processing later stories");
    mock.emptyBranch = 0;
    mock.longBranch = 1;
    const int previousShortenings = mock.shortenings;
    runQueue({alpha});
    partial = QJsonDocument::fromJson(readFile(temporary.filePath("alpha_clips/latest_attempt.json"))).object();
    require(mock.shortenings > previousShortenings && partial["accepted_stories"] == 2,
            "long montage did not reach shortening and subsequent audits");
    mock.longBranch = 0;
    qputenv("CLIPPER_TEST_FAIL_EN", "1");
    runQueue({alpha});
    qunsetenv("CLIPPER_TEST_FAIL_EN");
    partial = QJsonDocument::fromJson(readFile(temporary.filePath("alpha_clips/latest_attempt.json"))).object();
    require(partial["status"] == "partial" && partial["rendered_stories"] == 2 && partial["created_videos"] == 2,
            "EN failure discarded successful RU outputs or blocked next story");
    qputenv("CLIPPER_TEST_BAD_PROBE", "1");
    runQueue({alpha});
    qunsetenv("CLIPPER_TEST_BAD_PROBE");
    const auto badProbe = QJsonDocument::fromJson(readFile(temporary.filePath("alpha_clips/latest_attempt.json"))).object();
    require(badProbe["status"] == "failed" && badProbe["outputs"].toArray().isEmpty(),
            "invalid video/part file reported as ready output");
    QTextStream(stdout) << "PASS: quote evidence, complete branches, cache dependencies, "
                          "plan/edit/review, two-file queue, language-specific subtitles, saved EDL" << Qt::endl;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        if (fakeTool(app.arguments())) return 0;
        if (app.arguments().contains("--replay-events")) {
            require(argc == 4, "--replay-events log-directory packed-transcript.md");
            replayEventIndex(app.arguments()[2], app.arguments()[3]);
            return 0;
        }
        if (app.arguments().contains("--replay-v3-run")) {
            require(argc == 4, "--replay-v3-run log-directory packed-transcript.md");
            replayV3Run(app.arguments()[2], app.arguments()[3]); return 0;
        }
        if (app.arguments().contains("--prepare-v3")) {
            require(argc == 4, "--prepare-v3 settings.json source");
            prepareV3Source(app.arguments()[2], app.arguments()[3]); return 0;
        }
        if (app.arguments().contains("--retry-logged-v3")) {
            require(argc == 5, "--retry-logged-v3 settings.json original.request.json new-log-directory (requires explicit approval)");
            retryLoggedV3Request(app.arguments()); return 0;
        }
        if (app.arguments().contains("--render-v5-story")) {
            const auto a = app.arguments();
            require(argc == 9 || argc == 10, "--render-v5-story source final_edl.json story-number audio-index subtitles.ass output.mp4 plan.json");
            QFile edl(a[3]); require(edl.open(QIODevice::ReadOnly), "cannot open EDL");
            const auto stories = QJsonDocument::fromJson(edl.readAll()).object()["stories"].toArray();
            const int number = a[4].toInt();
            require(number > 0 && number <= stories.size(), "invalid story number");
            QList<QPair<double, double>> ranges;
            for (const auto& segment : stories[number-1].toObject()["segments"].toArray())
                ranges.append({segment.toObject()["start_time"].toDouble(), segment.toObject()["end_time"].toDouble()});
            require(!ranges.isEmpty(), "empty EDL");
            ClipRenderer renderer;
            QObject::connect(&renderer, &ClipRenderer::clipRendered, &app, &QCoreApplication::quit);
            QObject::connect(&renderer, &ClipRenderer::logMessage, &app, [](const QString& text) { qInfo().noquote() << text; });
            QObject::connect(&renderer, &ClipRenderer::errorOccurred, &app, [&](const QString& error) {
                qCritical().noquote() << error; app.exit(1);
            });
            QTimer::singleShot(0, &app, [&] { renderer.renderV5Story(a[2], ranges, a[5].toInt(), a[6], a[7], a[8], a.value(9)); });
            return app.exec();
        }
        if (app.arguments().contains("--render-v3")) {
            require(argc == 8, "--render-v3 settings.json source packed.md clean-words.json evaluation_result.json output-directory");
            renderV3Evaluation(app.arguments()); return 0;
        }
        if (app.arguments().contains("--check-live-model")) {
            require(argc == 4, "--check-live-model settings.json log-directory");
            checkLiveModel(app.arguments()[2], app.arguments()[3]); return 0;
        }
        if (app.arguments().contains("--live-v3") || app.arguments().contains("--preview-v3")) {
            require(argc == 8 || argc == 9, "--live-v3 settings.json source packed.md import-log-dir-or-dash output-dir count [request-budget<=36]");
            liveV3Run(app.arguments()); return 0;
        }
        if (app.arguments().contains("--compose-v5")) {
            require(argc == 4, "--compose-v5 input.srt output.ass");
            require(SubtitleComposer::writeAssV4(SrtUtils::parseFile(app.arguments()[2]), {}, app.arguments()[3],
                58, {}, true), "cannot compose V5");
            return 0;
        }
        if (app.arguments().contains("--compose-v4")) {
            require(argc == 4, "--compose-v4 input.srt output.ass");
            require(SubtitleComposer::writeAssV4(SrtUtils::parseFile(app.arguments()[2]),
                {{0, 0, 4, "Питеру предстоит сделать выбор"}}, app.arguments()[3]), "cannot compose V4");
            return 0;
        }
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
