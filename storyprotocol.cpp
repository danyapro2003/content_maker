#include "storyprotocol.h"
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include <cmath>

namespace StoryProtocol {
QJsonObject ValidationIssue::json() const
{
    return {{"code", code}, {"stage", stage}, {"branch_id", branchId}, {"block_id", blockId},
        {"field", field}, {"actual", actual}, {"expected", expected},
        {"explanation", explanation}, {"evidence", evidence}};
}
QString ValidationIssue::message() const
{
    return QString("%1 [%2/%3] %4: %5; требуется %6")
        .arg(code, branchId, blockId, field, explanation, expected);
}
ParseResult<QJsonDocument> document(QString text)
{
    ParseResult<QJsonDocument> result;
    text = text.trimmed();
    const auto fence = QRegularExpression("\\A```(?:json)?\\s*\\n?([\\s\\S]*?)\\n?```\\z",
        QRegularExpression::CaseInsensitiveOption).match(text);
    if (fence.hasMatch()) text = fence.captured(1).trimmed();
    QJsonParseError error;
    result.value = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || result.value.isNull())
        result.issues.append({"INVALID_JSON", "protocol", {}, {}, "response", {},
            "один полный JSON-документ", error.errorString(), {{"offset", double(error.offset)}}});
    return result;
}
ParseResult<StoryClip> parseEdit(const QString& text, const QList<S3Phrase>& phrases, const QString& branchId)
{
    ParseResult<StoryClip> result;
    auto parsed = document(text);
    if (!parsed.ok()) { result.issues = parsed.issues; return result; }
    auto& clip = result.value;
    clip.branchId = branchId;
    auto issue = [&](const QString& code, const QString& block, const QString& field, const QJsonValue& actual,
                     const QString& expected, const QString& explanation, const QJsonObject& evidence = QJsonObject{}) {
        result.issues.append({code, "edit", branchId, block, field, actual, expected, explanation, evidence});
    };
    QJsonObject object;
    if (parsed.value.isArray()) {
        const auto list = parsed.value.array();
        if (list.size() != 1 || !list.first().isObject()) {
            issue("WRONG_STORY_COUNT", {}, "stories", list.size(), "массив из одной истории", "Неверное число историй");
            return result;
        }
        object = list.first().toObject();
    } else {
        const auto envelope = parsed.value.object();
        if (envelope["protocol_version"] != 2 || envelope["action"] != "edit_story" || !envelope["story"].isObject()) {
            issue("WRONG_ENVELOPE", {}, "action", envelope["action"], "protocol_version=2, action=edit_story, story={...}", "Неверный контракт монтажёра");
            return result;
        }
        if (!envelope["branch_id"].isString() || envelope["branch_id"].toString().isEmpty()
            || (!branchId.isEmpty() && envelope["branch_id"] != branchId))
            issue("WRONG_BRANCH", {}, "branch_id", envelope["branch_id"], branchId, "Ответ другой ветки или неверный тип ID");
        clip.protocolVersion = 2;
        clip.branchId = envelope["branch_id"].toString();
        clip.baseDraftHash = envelope["base_draft_hash"].toString();
        object = envelope["story"].toObject();
    }
    auto stringField = [&](const QString& field, bool required) {
        const auto value = object.value(field);
        if ((required || !value.isUndefined()) && (!value.isString() || (required && value.toString().trimmed().isEmpty())))
            issue("WRONG_TYPE", {}, field, value, "непустая строка", "Отсутствует текстовое поле или неверен тип");
        return value.toString();
    };
    clip.title = stringField("title", true);
    clip.description = stringField("description", false);
    clip.caption = stringField("caption", false);
    clip.emotion = stringField("emotion", false);
    clip.storySummary = stringField("story_summary", true);
    clip.hookReason = stringField("hook_reason", true);
    clip.openingMode = stringField("opening_mode", true);
    if (!object["score"].isDouble() || !std::isfinite(object["score"].toDouble()))
        issue("WRONG_TYPE", {}, "score", object["score"], "число 0–100", "Неверная оценка");
    clip.score = object["score"].toDouble();
    if (!object["segments"].isArray() || object["segments"].toArray().isEmpty())
        issue("EMPTY_SEGMENTS", {}, "segments", object["segments"], "непустой массив блоков", "Нет монтажных блоков");
    QHash<int, const S3Phrase*> byId;
    for (const auto& phrase : phrases) byId.insert(phrase.id, &phrase);
    QSet<QString> blockIds;
    int number = 0;
    for (const auto& value : object["segments"].toArray()) {
        ++number;
        const auto segment = value.toObject();
        StorySegment out;
        out.blockId = clip.protocolVersion == 2 ? segment["block_id"].toString() : QString("B%1").arg(number);
        if (out.blockId.isEmpty() || blockIds.contains(out.blockId))
            issue("INVALID_BLOCK_ID", out.blockId, "block_id", segment["block_id"], "уникальная непустая строка", "Неверный ID блока");
        blockIds.insert(out.blockId);
        auto endpoint = [&](const QString& field) -> const S3Phrase* {
            const auto id = segment[field];
            if (!id.isDouble() || id.toDouble() != std::floor(id.toDouble()) || id.toDouble() < 1 || id.toDouble() > 2147483647) {
                issue("WRONG_TYPE", out.blockId, field, id, "целочисленный ID речевой фразы", "Неверный тип или значение ID");
                return nullptr;
            }
            const auto* phrase = byId.value(id.toInt(), nullptr);
            if (!phrase) {
                issue("UNKNOWN_PHRASE", out.blockId, field, id, "ID из предоставленного исходника", "Фраза не существует");
                return nullptr;
            }
            if (phrase->marker) {
                QJsonObject evidence{{"start_sec", phrase->startSec}, {"end_sec", phrase->endSec}, {"text", phrase->text}};
                int previous = 0, next = 0;
                for (const auto& p : phrases) if (!p.marker) {
                    if (p.id < phrase->id) previous = qMax(previous, p.id);
                    if (p.id > phrase->id && (!next || p.id < next)) next = p.id;
                }
                if (previous) evidence["previous_speech_phrase"] = previous;
                if (next) evidence["next_speech_phrase"] = next;
                issue("INVALID_ENDPOINT", out.blockId, field, id, "ID речевой фразы",
                    QString("P%1 — маркер, %2–%3 с; не подменяй его автоматически соседней репликой")
                        .arg(phrase->id).arg(phrase->startSec).arg(phrase->endSec), evidence);
                return nullptr;
            }
            return phrase;
        };
        const auto* first = endpoint("start_phrase");
        const auto* last = endpoint("end_phrase");
        if (first && last) {
            out.startPhrase = first->id; out.endPhrase = last->id;
            out.startSec = first->startSec; out.endSec = last->endSec;
            if (last->id < first->id || last->endSec <= first->startSec)
                issue("REVERSED_RANGE", out.blockId, "end_phrase", last->id, "конец не раньше начала", "Обратный интервал");
        }
        out.beat = segment[clip.protocolVersion == 2 ? "role" : "beat"].toString();
        out.quote = segment["quote"].toString(); out.reason = segment["reason"].toString();
        out.event = segment["event"].toString(); out.connection = segment["connection"].toString();
        for (const auto& id : segment["event_ids"].toArray()) out.eventIds.append(id.toString());
        if (clip.protocolVersion == 2 && (out.beat.isEmpty() || out.reason.trimmed().isEmpty()))
            issue("MISSING_BLOCK_REASON", out.blockId, "role/reason", {}, "роль и обоснование блока", "Не объяснён выбор материала");
        clip.segments.append(out);
    }
    return result;
}
}
