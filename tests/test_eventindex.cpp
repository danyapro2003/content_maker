#include "storynarrative.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTextStream>
#include <stdexcept>

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static QJsonObject readObject(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "event fixture missing");
    const auto document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), "invalid event fixture JSON");
    return document.object();
}

void testEventIndex()
{
    const auto fixture = readObject(QString(CLIPPER_SOURCE_DIR) + "/tests/fixtures/event_index_reference_errors.json");
    QList<S3Phrase> phrases;
    for (const auto& value : fixture["phrases"].toArray()) {
        const auto json = value.toObject();
        S3Phrase phrase;
        phrase.id = json["id"].toInt(); phrase.startSec = json["start"].toDouble();
        phrase.endSec = json["end"].toDouble(); phrase.text = json["text"].toString();
        phrases.append(phrase);
    }
    const auto responses = fixture["responses"].toArray();
    for (const auto& response : responses) {
        const auto result = StoryNarrative::checkEvents(response.toObject(), phrases, 1);
        require(result.complete() && result.accepted.size() == 3 && result.corrections.size() == 3,
                "real zero-based/text evidence references not recovered");
        for (int i = 0; i < 3; ++i) {
            const auto event = result.accepted[i].toObject();
            require(event["evidence_phrase"] == i + 1 && event["start_sec"] == phrases[i].startSec,
                    "recovery attached the wrong source phrase");
            require(event["quote"] == response.toObject()["events"].toArray()[i].toObject()["quote"],
                    "recovery rewrote a quote");
            require(result.corrections[i].toObject()["from"] == response.toObject()["events"].toArray()[i].toObject()["evidence_phrase"],
                    "normalization log lost original reference");
        }
    }
    auto first = responses.first().toObject()["events"].toArray().first().toObject();
    first["evidence_phrase"] = 1;
    auto check = [&](const QJsonArray& events, const QList<S3Phrase>& source, const QJsonArray& existing = {}) {
        return StoryNarrative::checkEvents({{"action", "index_events"}, {"events", events}}, source, 1, existing);
    };
    require(check({first}, phrases).complete(), "valid numeric evidence rejected");
    const auto saved = check({first}, phrases).accepted;
    require(!check({first}, phrases, saved).complete(), "saved event overwritten");
    require(check({first, first}, phrases).accepted.isEmpty(), "duplicate ID partially accepted");

    auto broken = first; broken["event_id"] = "C1E2"; broken["quote"] = "Полностью выдуманная цитата.";
    broken["evidence_phrase"] = 999;
    auto partial = check({first, broken}, phrases);
    require(!partial.complete() && partial.accepted.size() == 1 && partial.rejected.size() == 1,
            "invalid event discarded good records");
    require(partial.errors.join(" ").contains("P999") && partial.errors.join(" ").contains("C1E2"),
            "diagnostic lost event/phrase ID");
    require(partial.rejected.first().toObject()["repair_id"] == "R2", "unstable repair target");
    const auto prompt = StoryNarrative::eventRepairPrompt(phrases, 1, partial.rejected, partial.accepted);
    require(prompt.contains("repair_events") && prompt.contains("R2") && prompt.contains("P999"),
            "repair prompt lost precise error");
    auto fixed = responses.first().toObject()["events"].toArray()[1].toObject(); fixed["evidence_phrase"] = 2;
    const auto repair = QJsonObject{{"action", "repair_events"}, {"repairs", QJsonArray{
        QJsonObject{{"repair_id", "R2"}, {"event", fixed}}}}};
    auto repaired = StoryNarrative::checkEventRepairs(repair, partial.rejected, phrases, 1, partial.accepted);
    require(repaired.complete() && repaired.accepted.size() == 1, "targeted repair failed");
    require(repaired.accepted.first().toObject()["event_id"] == "C1E2", "wrong event repaired");
    auto omitted = StoryNarrative::checkEventRepairs({{"action", "repair_events"}, {"repairs", QJsonArray{}}},
                                                   partial.rejected, phrases, 1, partial.accepted);
    require(!omitted.complete() && omitted.rejected.size() == 1
            && omitted.rejected.first().toObject()["event"] == broken, "omitted repair silently lost an event");
    require(!StoryNarrative::checkEventRepairs({{"action", "index_events"}, {"events", QJsonArray{first}}},
                partial.rejected, phrases, 1, partial.accepted).complete(), "full map bypassed targeted repair");
    auto duplicateRepair = repair;
    auto replacements = repair["repairs"].toArray(); replacements.append(replacements.first());
    duplicateRepair["repairs"] = replacements;
    require(!StoryNarrative::checkEventRepairs(duplicateRepair, partial.rejected, phrases, 1, partial.accepted).complete(),
            "duplicate repair target accepted");
    auto overwrite = repair; overwrite["repairs"] = QJsonArray{QJsonObject{{"repair_id", "R2"}, {"event", first}}};
    require(!StoryNarrative::checkEventRepairs(overwrite, partial.rejected, phrases, 1, partial.accepted).complete(),
            "repair modified a saved event");

    // A successful partial repair must not renumber the remaining targets.
    auto third = broken; third["event_id"] = "C1E3";
    auto batch = check({first, broken, third}, phrases);
    auto oneFixed = StoryNarrative::checkEventRepairs(repair, batch.rejected, phrases, 1, batch.accepted);
    require(oneFixed.accepted.size() == 1 && oneFixed.rejected.size() == 1
            && oneFixed.rejected.first().toObject()["repair_id"] == "R3", "remaining repair target renumbered");

    auto duplicatePhrase = phrases.first(); duplicatePhrase.id = 100;
    duplicatePhrase.startSec += 100; duplicatePhrase.endSec += 100;
    auto ambiguousSource = phrases; ambiguousSource.append(duplicatePhrase);
    auto wrong = first; wrong["evidence_phrase"] = 0;
    require(check({wrong}, ambiguousSource).accepted.isEmpty(), "ambiguous quote guessed");
    require(check({first}, ambiguousSource).complete(), "explicit valid ID rejected for repeated dialogue");
    ambiguousSource.last().flagCheck = true;
    require(check({wrong}, ambiguousSource).accepted.isEmpty(), "uncertainty concealed ambiguous quote");
    auto uncertain = phrases; uncertain.first().flagCheck = true;
    require(check({wrong}, uncertain).accepted.isEmpty(), "uncertain quote auto-recovered");
    require(check({first}, uncertain).accepted.isEmpty(), "uncertain explicit evidence accepted");
    uncertain = phrases; uncertain.first().uncertain = true;
    require(check({first}, uncertain).accepted.isEmpty(), "uncertain evidence flag ignored");
    auto splitQuote = first; splitQuote["quote"] = first["quote"].toString() + " " + phrases[1].text;
    require(check({splitQuote}, phrases).accepted.isEmpty(), "quote spanning two phrases accepted");
    auto contradiction = first; contradiction["quote"] = "Не разгребать последствия урагана.";
    require(check({contradiction}, phrases).accepted.isEmpty(), "invented negation accepted");
    auto invalidId = first; invalidId["event_id"] = "C1E";
    require(!check({invalidId}, phrases).complete(), "malformed event ID accepted");
    auto wrongWindow = first; wrongWindow["event_id"] = "C2E1";
    require(!check({wrongWindow}, phrases).complete(), "foreign window ID accepted");
    auto wrongFields = first; wrongFields["viewer_learns"] = "a string";
    require(!check({wrongFields}, phrases).complete(), "invalid knowledge array accepted");
    require(!StoryNarrative::checkEvents({}, phrases, 1).complete(), "invalid envelope accepted");
}

// Offline diagnosis: run both saved replies through the exact production validator.
void replayEventIndex(const QString& directory, const QString& packedPath)
{
    const auto chunks = StoryNarrative::chunks(Stories3Runner::parsePacked(packedPath));
    require(!chunks.isEmpty(), "no phrases for saved response replay");
    for (const QString prefix : {QString("001_index_1"), QString("002_index_1")}) {
        const auto response = readObject(directory + "/" + prefix + ".response.json");
        const auto content = response["response"].toObject()["choices"].toArray().first().toObject()
            ["message"].toObject()["content"].toString();
        const auto result = StoryNarrative::checkEvents(QJsonDocument::fromJson(content.toUtf8()).object(), chunks.first(), 1);
        QTextStream(stdout) << prefix << ": accepted=" << result.accepted.size()
            << ", corrected=" << result.corrections.size() << ", rejected=" << result.rejected.size() << '\n';
        for (const auto& error : result.errors) QTextStream(stdout) << error << '\n';
        if (prefix.startsWith("001"))
            require(result.complete() && result.accepted.size() == 28 && result.corrections.size() == 3,
                    "original failed response still blocks indexing");
    }
}
