#include "storycontext.h"
#include "storynarrative.h"
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <functional>

namespace StoryContext {
QJsonObject SourceUnit::card() const
{
    if (phrases.isEmpty()) return {};
    return {{"unit_id", id}, {"episode", episode}, {"start_phrase", phrases.first().id},
        {"end_phrase", phrases.last().id}, {"start_sec", phrases.first().startSec},
        {"end_sec", phrases.last().endSec}, {"duration_sec", duration()}};
}
QList<SourceUnit> buildSourceUnits(const QList<S3Phrase>& phrases)
{
    QList<SourceUnit> result;
    SourceUnit current;
    int episode = 1, chars = 0;
    auto flush = [&]() {
        if (current.phrases.isEmpty()) return;
        current.id = QString("U%1").arg(result.size() + 1); current.episode = episode;
        result.append(current); current = {}; chars = 0;
    };
    for (const auto& phrase : phrases) {
        if (phrase.marker) { flush(); ++episode; continue; }
        if (!current.phrases.isEmpty() && phrase.startSec - current.phrases.last().endSec > 3) { flush(); ++episode; }
        // Presentation pages preserve episode and neighbor identity; never imply a scene cut.
        if (chars > 3000) flush();
        current.phrases.append(phrase); chars += phrase.text.size() + 70;
    }
    flush(); return result;
}
static int unitFor(int phrase, const QList<SourceUnit>& units)
{
    for (int i = 0; i < units.size(); ++i)
        for (const auto& p : units[i].phrases) if (p.id == phrase) return i;
    return -1;
}
QJsonArray eventCards(const QJsonArray& events, const QList<SourceUnit>& units)
{
    QJsonArray cards;
    for (const auto& value : events) {
        const auto event = value.toObject();
        QJsonObject card;
        for (const auto& field : {"event_id", "observed", "evidence_phrase", "viewer_requires", "viewer_learns", "character_knowledge", "uncertainties"})
            if (event.contains(field)) card[field] = event[field];
        const int index = unitFor(event["evidence_phrase"].toInt(), units);
        // The complete catalog is sent once after the event cards. Repeating its
        // timestamps for every event inflated real planning requests substantially.
        if (index >= 0) card["source_unit_id"] = units[index].id;
        cards.append(card);
    }
    return cards;
}
BranchContext build(const QJsonObject& plan, const QJsonArray& events, const QList<SourceUnit>& units,
                    int maxChars, const QStringList& extraUnits)
{
    BranchContext result;
    QSet<int> selected;
    QSet<QString> wanted;
    auto issue = [&](const QString& code, const QString& explanation) {
        result.issues.append(QJsonObject{{"code", code}, {"explanation", explanation}});
    };
    for (const auto& id : plan["required_events"].toArray()) wanted.insert(id.toString());
    for (const auto& id : plan["optional_events"].toArray()) wanted.insert(id.toString());
    QList<QPair<double, double>> proofs;
    for (const auto& value : events) {
        const auto event = value.toObject();
        if (!wanted.contains(event["event_id"].toString())) continue;
        const int index = unitFor(event["evidence_phrase"].toInt(), units);
        if (index < 0) { issue("MISSING_SOURCE_UNIT", "Нет исходного материала для " + event["event_id"].toString()); continue; }
        selected.insert(index); result.events.append(event);
        if (plan["required_events"].toArray().contains(event["event_id"]))
            for (const auto& phrase : units[index].phrases) if (phrase.id == event["evidence_phrase"].toInt())
                proofs.append({phrase.startSec, phrase.endSec});
    }
    QStringList explicitUnits = extraUnits;
    for (const auto& id : plan["source_unit_ids"].toArray()) explicitUnits.append(id.toString());
    for (const auto& id : explicitUnits) {
        bool found = false;
        for (int i = 0; i < units.size(); ++i) if (units[i].id == id) { selected.insert(i); found = true; break; }
        if (!found) issue("UNKNOWN_UNIT", "Нет единицы исходника " + id);
    }
    auto render = [&](const QSet<int>& indices) {
        QString text;
        auto ordered = indices.values(); std::sort(ordered.begin(), ordered.end());
        for (int index : ordered)
            text += QString::fromUtf8(QJsonDocument(units[index].card()).toJson(QJsonDocument::Compact))
                + "\n" + StoryNarrative::transcript(units[index].phrases) + "\n";
        return text;
    };
    const QString metadata = "События ветки:\n" + QString::fromUtf8(QJsonDocument(result.events).toJson(QJsonDocument::Compact))
        + "\nИсточник ветки (границы выбирай только по этим ID):\n";
    if (metadata.size() + render(selected).size() > maxChars) {
        issue("CONTEXT_TOO_LARGE", "Обязательный материал не помещается: сузь замысел, не обрезай реплики."); return result;
    }
    // Add both sides of a dialogue when available. No one-sided arbitrary string truncation.
    auto mandatory = selected.values(); std::sort(mandatory.begin(), mandatory.end());
    for (int index : mandatory) for (int adjacent : {index - 1, index + 1}) {
        if (adjacent < 0 || adjacent >= units.size() || selected.contains(adjacent)) continue;
        const double gap = adjacent < index ? units[index].phrases.first().startSec - units[adjacent].phrases.last().endSec
                                           : units[adjacent].phrases.first().startSec - units[index].phrases.last().endSec;
        if (gap > 10) continue;
        auto candidate = selected; candidate.insert(adjacent);
        if (metadata.size() + render(candidate).size() <= maxChars) selected = candidate;
    }
    result.text = metadata + render(selected);
    auto ordered = selected.values(); std::sort(ordered.begin(), ordered.end());
    QList<QPair<double, double>> ranges;
    for (int i : ordered) { result.unitIds.append(units[i].id); ranges.append({units[i].phrases.first().startSec, units[i].phrases.last().endSec}); }
    auto unionDuration = [](QList<QPair<double, double>> intervals) {
        std::sort(intervals.begin(), intervals.end()); double total = 0, end = -1;
        for (const auto& span : intervals) { total += qMax(0.0, span.second - qMax(end, span.first)); end = qMax(end, span.second); }
        return total;
    };
    result.availableSec = unionDuration(ranges); result.evidenceSec = unionDuration(proofs);
    return result;
}
QStringList unitsForReferences(const QJsonObject& request, const QJsonArray& events,
                              const QList<SourceUnit>& units, QString& error)
{
    QSet<int> indices;
    if (request["purpose"].toString().trimmed().isEmpty()) { error = "get_context требует purpose."; return {}; }
    for (const auto& id : request["event_ids"].toArray()) {
        int phrase = -1;
        for (const auto& event : events) if (event.toObject()["event_id"] == id) phrase = event.toObject()["evidence_phrase"].toInt();
        const int index = unitFor(phrase, units);
        if (index < 0) { error = "Неизвестный event_id в get_context."; return {}; }
        indices.insert(index);
    }
    for (const auto& id : request["phrase_ids"].toArray()) {
        const int index = id.isDouble() && id.toDouble() == id.toInt() ? unitFor(id.toInt(), units) : -1;
        if (index < 0) { error = "Неизвестный/нечисловой phrase_id в get_context."; return {}; }
        indices.insert(index);
    }
    if (indices.isEmpty() || indices.size() > 6) { error = "get_context: нужны 1–6 известных участков."; return {}; }
    QStringList result;
    for (int index : indices) {
        result.append(units[index].id);
        // Explicit request expands the local dialogue by one page on either side.
        for (int adjacent : {index - 1, index + 1})
            if (adjacent >= 0 && adjacent < units.size() && units[adjacent].episode == units[index].episode)
                result.append(units[adjacent].id);
    }
    result.removeDuplicates(); return result;
}
StoryProtocol::ParseResult<QJsonArray> plans(const QJsonObject& response, const QJsonArray& events,
    const QList<SourceUnit>& units, int target, int maximum, int reserve, double minSec, double maxSec, int maxContext)
{
    StoryProtocol::ParseResult<QJsonArray> result;
    auto fail = [&](const QString& code, const QString& id, const QString& field, const QString& reason) {
        result.issues.append({code, "plan", id, {}, field, {}, "реализуемый план по исходнику", reason, {}});
    };
    const bool v2 = response["protocol_version"] == 2;
    QJsonArray branches = v2 ? response["primary"].toArray() : response["branches"].toArray();
    const int primary = branches.size();
    const int allowedReserve = qMin(reserve, qMax(0, maximum - target));
    if (v2) for (const auto& branch : response["reserve"].toArray()) branches.append(branch);
    if (response["action"] != "plan_stories" || primary < target || (v2 && primary != target)
        || branches.size() > maximum || (v2 && response["reserve"].toArray().size() > allowedReserve)) {
        fail("WRONG_PLAN_COUNT", {}, "primary/reserve", QString("Нужно %1 основных и до %2 резервных веток").arg(target).arg(allowedReserve)); return result;
    }
    QSet<QString> known, ids, goals, endings;
    for (const auto& value : events) known.insert(value.toObject()["event_id"].toString());
    for (int index = 0; index < branches.size(); ++index) {
        auto branch = branches[index].toObject();
        const auto idValue = branch["branch_id"];
        const QString id = v2 ? idValue.toString() : QString("B%1").arg(idValue.toInt());
        if ((!v2 && (!idValue.isDouble() || idValue.toDouble() != idValue.toInt() || idValue.toInt() < 1)) || id.isEmpty() || ids.contains(id))
            fail("INVALID_BRANCH_ID", id, "branch_id", "ID должен быть уникальным");
        ids.insert(id);
        branch["branch_id"] = id;
        branch["protocol_version"] = v2 ? 2 : 1;
        branch["is_reserve"] = index >= target;
        if (v2) {
            branch["goal"] = branch.value("dramatic_question");
            branch["ending"] = branch.value("ending_effect");
            branch["opening_mode"] = branch["hook"].toObject()["mode"];
            branch["hook_reason"] = branch["hook"].toObject()["why_interesting"];
            branch["summary"] = branch.value("dramatic_question");
        }
        for (const auto& field : {"title", "goal", "summary", "ending", "hook_reason"})
            if (branch[field].toString().trimmed().isEmpty()) fail("MISSING_PLAN_FIELD", id, field, "Поле должно быть непустой строкой");
        const auto goal = branch["goal"].toString().simplified().toLower();
        if (goals.contains(goal)) fail("DUPLICATE_PLAN", id, "goal", "Повторяется замысел");
        goals.insert(goal);
        if (branch["opening_mode"] != "natural" && branch["opening_mode"] != "cold_open")
            fail("INVALID_OPENING", id, "opening_mode", "Нужно natural или cold_open");
        const auto required = branch["required_events"].toArray();
        const auto ending = branch["ending_event"].toString();
        if (required.isEmpty() || !required.contains(ending)) fail("MISSING_ENDING", id, "required_events", "Финал должен быть обязательным событием");
        QSet<QString> used;
        for (const auto& event : required) {
            if (!event.isString() || !known.contains(event.toString()) || used.contains(event.toString()))
                fail("UNKNOWN_EVENT", id, "required_events", "Нужны уникальные ID из карты");
            used.insert(event.toString());
        }
        auto identity = used.values(); std::sort(identity.begin(), identity.end());
        const auto fingerprint = ending + ":" + identity.join(",");
        if (endings.contains(fingerprint)) fail("DUPLICATE_PLAN", id, "required_events", "Одинаковые обязательные события и финал");
        endings.insert(fingerprint);
        if (v2) {
            QMap<QString, QStringList> dependencies;
            for (const auto& item : branch["chain"].toArray()) {
                const auto link = item.toObject(); const auto eventId = link["event_id"].toString();
                if (!known.contains(eventId) || dependencies.contains(eventId) || link["function"].toString().isEmpty()
                    || !link["depends_on"].isArray()) fail("INVALID_CHAIN", id, "chain", "Нужны известные события, роль и массив depends_on");
                QStringList parents; for (const auto& parent : link["depends_on"].toArray()) parents.append(parent.toString());
                dependencies[eventId] = parents;
            }
            QSet<QString> visiting, done;
            std::function<bool(const QString&)> visit = [&](const QString& event) {
                if (!dependencies.contains(event) || visiting.contains(event)) return false;
                if (done.contains(event)) return true;
                visiting.insert(event);
                for (const auto& parent : dependencies[event]) if (!visit(parent)) return false;
                visiting.remove(event); done.insert(event); return true;
            };
            for (const auto& event : used) if (!visit(event)) fail("INVALID_CHAIN", id, "chain", "Пропуск обязательного события или цикл зависимостей");
            for (auto it = dependencies.cbegin(); it != dependencies.cend(); ++it)
                if (!visit(it.key())) fail("INVALID_CHAIN", id, "chain", "Цикл или неизвестная зависимость в дополнительной части цепочки");
            QSet<QString> optional;
            for (const auto& event : branch["optional_events"].toArray()) {
                if (!event.isString() || !known.contains(event.toString()) || optional.contains(event.toString()) || used.contains(event.toString()))
                    fail("UNKNOWN_EVENT", id, "optional_events", "Нужны уникальные необязательные события из карты");
                optional.insert(event.toString());
            }
            if (!known.contains(branch["hook"].toObject()["event_id"].toString())) fail("INVALID_HOOK", id, "hook", "Хук должен ссылаться на карту");
            if (!branch["source_unit_ids"].isArray() || branch["source_unit_ids"].toArray().isEmpty()) fail("MISSING_UNITS", id, "source_unit_ids", "Нужны ID реальных единиц материала");
            if (!branch["missing_evidence"].isArray()) fail("MISSING_EVIDENCE_FIELD", id, "missing_evidence", "Нужен массив недостающих подтверждений");
            if (!branch["missing_evidence"].toArray().isEmpty()) branch["needs_visual"] = true;
        }
        const auto context = build(branch, events, units, maxContext);
        if (!context.ok()) fail("CONTEXT_INVALID", id, "source_unit_ids", QString::fromUtf8(QJsonDocument(context.issues).toJson()));
        // Conservative lower bound: only indispensable evidence, never whole conversations.
        if (context.evidenceSec > maxSec) fail("PLAN_TOO_LONG", id, "required_events", QString("Обязательные доказательства занимают %1 с при максимуме %2").arg(context.evidenceSec).arg(maxSec));
        if (v2 && context.availableSec < minSec) fail("PLAN_TOO_SHORT", id, "source_unit_ids", QString("Выбрано лишь %1 с при минимуме %2. Расширь связанную цепочку или выбери другой замысел.").arg(context.availableSec).arg(minSec));
        if (index < target + allowedReserve) result.value.append(branch);
    }
    return result;
}
}
