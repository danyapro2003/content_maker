#include "storyprotocol.h"
#include "pipelinemanager.h"
#include <stdexcept>

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void testStoryProtocol()
{
    S3Phrase speech; speech.id = 1048; speech.startSec = 4790; speech.endSec = 4794;
    speech.text = "Объяснение решения";
    S3Phrase marker; marker.id = 1049; marker.startSec = 4794.22; marker.endSec = 4818.06; marker.marker = true;
    auto story = QJsonObject{{"title", "test"}, {"story_summary", "cause"}, {"hook_reason", "question"},
        {"opening_mode", "natural"}, {"score", 80},
        {"segments", QJsonArray{QJsonObject{{"start_phrase", 1048}, {"end_phrase", 1049}}}}};
    auto parse = [&](const QJsonArray& list) {
        return StoryProtocol::parseEdit(QString::fromUtf8(QJsonDocument(list).toJson()), {speech, marker}, "B12");
    };
    auto result = parse({story});
    require(!result.ok() && result.issues.size() == 1 && result.issues.first().code == "INVALID_ENDPOINT",
        "marker masked as wrong story count");
    require(result.issues.first().evidence["previous_speech_phrase"] == 1048, "marker diagnostic missing neighbor");
    require(parse({story, story}).issues.first().code == "WRONG_STORY_COUNT", "multiple stories accepted");
    story["segments"] = QJsonArray{QJsonObject{{"start_phrase", 1048}, {"end_phrase", "1048"}}};
    require(parse({story}).issues.first().code == "WRONG_TYPE", "string ID silently converted");
    story["segments"] = QJsonArray{QJsonObject{{"start_phrase", 1048}, {"end_phrase", 9999}}};
    require(parse({story}).issues.first().code == "UNKNOWN_PHRASE", "unknown phrase accepted");
    story["segments"] = QJsonArray{QJsonObject{{"start_phrase", 1048}, {"end_phrase", 1048}}};
    require(parse({story}).ok(), "valid legacy story rejected");
    const auto json = QString::fromUtf8(QJsonDocument(QJsonArray{story}).toJson());
    require(StoryProtocol::parseEdit("```json\n" + json + "\n```", {speech}).ok(), "single fence rejected");
    require(!StoryProtocol::parseEdit("explanation " + json, {speech}).ok(), "arbitrary prose accepted");
    StoryClip audited; StorySegment block; block.blockId = "private_branch_id"; audited.segments = {block};
    QJsonObject report{{"protocol_version", 2}, {"action", "audit_story"}, {"understood_story", "Hero made a choice"},
        {"verdict", "pass"}, {"issues", QJsonArray{}}};
    auto audit = [&]() { return StoryProtocol::parseAudit(QString::fromUtf8(QJsonDocument(report).toJson()), audited); };
    require(audit().ok(), "valid audit rejected");
    report["understood_story"] = true;
    require(!audit().ok() && audit().messages().join(" ").contains("understood_story"), "boolean understanding fabricated into pass");
    report["understood_story"] = "A choice";
    QJsonObject issue{{"block_id", "V1"}, {"kind", "missing_context"}, {"severity", "blocking"},
        {"problem", "No cause"}, {"evidence", "A sudden decision"}};
    report["issues"] = QJsonArray{issue};
    require(!audit().ok(), "pass with blocking issue accepted");
    report["verdict"] = "repair";
    require(audit().ok() && audit().value["issues"].toArray()[0].toObject()["block_id"] == "private_branch_id", "neutral audit IDs not mapped");
    issue["severity"] = "advisory"; report["issues"] = QJsonArray{issue}; report["verdict"] = "pass";
    require(audit().ok(), "advisory blocks release");
    report["issues"] = QJsonArray{"fix the spelling"};
    require(!audit().ok(), "unstructured issue silently removed");
    FileRunResult failed; failed.status = "failed";
    FileRunResult complete; complete.status = "completed"; complete.renderedStories = 1;
    complete.outputs = {"ru.mp4", "en.mp4"};
    QueueRunSummary queue; queue.files = {failed, complete};
    require(queue.outputs().size() == 2 && queue.text().contains("Новых историй: 1")
        && queue.text().contains("с ошибками 1"), "queue double counts language versions");
}
