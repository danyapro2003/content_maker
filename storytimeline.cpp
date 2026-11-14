#include "storytimeline.h"
#include "storyrunstore.h"
#include "llmclient.h"
#include <QHash>
#include <QSet>
#include <cmath>

namespace StoryTimeline {
QString hash(const StoryClip& story)
{
    QJsonArray blocks;
    for (const auto& block : story.segments)
        blocks.append(QJsonObject{{"start", block.startSec}, {"end", block.endSec},
            {"start_phrase", block.startPhrase}, {"end_phrase", block.endPhrase}});
    return StoryRunStore::hash({{"version", 2}, {"blocks", blocks}});
}

StoryProtocol::ParseResult<CompiledStory> compile(const StoryClip& draft, const QList<S3Phrase>& phrases,
    const QJsonArray& events, const QJsonObject& plan, const Options& o)
{
    StoryProtocol::ParseResult<CompiledStory> result;
    auto& clip = result.value.story;
    clip = draft; clip.timelineHash.clear();
    auto fail = [&](const QString& code, const QString& block, const QString& field, const QJsonValue& actual,
                    const QString& expected, const QString& explanation, const QJsonObject& evidence = QJsonObject{}) {
        result.issues.append({code, "compile", clip.branchId, block, field, actual, expected, explanation, evidence});
    };
    QHash<int, int> positions;
    for (int i = 0; i < phrases.size(); ++i) positions.insert(phrases[i].id, i);
    if (clip.segments.isEmpty()) fail("EMPTY_SEGMENTS", {}, "segments", {}, "непустой монтаж", "Нет блоков");
    if (clip.storySummary.trimmed().isEmpty() || clip.hookReason.trimmed().isEmpty())
        fail("MISSING_STORY", {}, "story_summary/hook_reason", {}, "смысл истории и интерес начала", "Нет замысла или хука");
    if (clip.openingMode != "natural" && clip.openingMode != "cold_open")
        fail("INVALID_OPENING", {}, "opening_mode", clip.openingMode, "natural или cold_open", "Неверный режим начала");
    if (!std::isfinite(clip.score) || clip.score < o.minScore || clip.score < 0 || clip.score > 100)
        fail("LOW_SCORE", {}, "score", clip.score, QString("%1–100").arg(o.minScore), "Оценка ниже порога");
    for (int i = 0; i < clip.segments.size(); ++i) {
        auto& segment = clip.segments[i];
        if (segment.blockId.isEmpty()) segment.blockId = QString("B%1").arg(i + 1);
        if (o.resolveEndpoints) {
            const int first = positions.value(segment.startPhrase, -1), last = positions.value(segment.endPhrase, -1);
            if (first < 0 || last < first || phrases[first].marker || phrases[last].marker) {
                fail("INVALID_ENDPOINT", segment.blockId, "start_phrase/end_phrase", {}, "существующие речевые границы по порядку", "Неверная ссылка");
                continue;
            }
            segment.startSec = phrases[first].startSec; segment.endSec = phrases[last].endSec;
        }
        if (!std::isfinite(segment.startSec) || !std::isfinite(segment.endSec) || segment.startSec < 0
            || segment.endSec - segment.startSec < .5 || (o.sourceDuration > 0 && segment.endSec > o.sourceDuration + .001))
            fail("INVALID_RANGE", segment.blockId, "interval", {}, "интервал внутри исходника >=0.5 с", "Неверные границы");
        if (clip.protocolVersion == 1 && o.checkLegacyQuotes
            && !LlmClient::checkQuote(segment.quote, segment.startSec, segment.endSec, phrases, 0).found)
            fail("QUOTE_NOT_FOUND", segment.blockId, "quote", segment.quote, "уверенная дословная цитата", "Цитата не подтверждена");
    }
    if (!result.ok()) return result;
    QList<StorySegment> merged;
    for (const auto& segment : clip.segments) {
        bool joined = false;
        if (!merged.isEmpty() && !(merged.size() == 1 && clip.openingMode == "cold_open")) {
            auto& previous = merged.last();
            const double overlap = previous.endSec - segment.startSec;
            const bool adjacent = positions.contains(previous.endPhrase) && positions.contains(segment.startPhrase)
                && positions[previous.endPhrase] + 1 == positions[segment.startPhrase];
            if (overlap > 0 && overlap <= o.overlapToleranceSec + .000001 && adjacent
                && segment.endSec > previous.endSec) {
                result.value.normalizations.append(QJsonObject{{"kind", "union_asr_overlap"},
                    {"kept_block", previous.blockId}, {"merged_block", segment.blockId}, {"overlap_sec", overlap}});
                previous.endSec = segment.endSec; previous.endPhrase = segment.endPhrase;
                previous.reason += "; " + segment.reason;
                joined = true;
            }
        }
        if (!joined) merged.append(segment);
    }
    clip.segments = merged;
    if (o.maxSegments > 0 && clip.segments.size() > o.maxSegments)
        fail("TOO_MANY_BLOCKS", {}, "segments", clip.segments.size(), QString::number(o.maxSegments), "Слишком много монтажных блоков");
    for (int i = 0; i < clip.segments.size(); ++i) {
        const auto& segment = clip.segments[i];
        if (i > 0 && !(i == 1 && clip.openingMode == "cold_open") && segment.startSec < clip.segments[i - 1].endSec - .001)
            fail("CHRONOLOGY", segment.blockId, "start_phrase", segment.startPhrase, "исходный порядок без перекрытия", "Нарушен порядок показа");
        for (int j = 0; j < i; ++j)
            if (qMin(segment.endSec, clip.segments[j].endSec) - qMax(segment.startSec, clip.segments[j].startSec) > .001)
                fail("REPEATED_MATERIAL", segment.blockId, "interval", {}, "без повторов", "Исходный материал используется дважды");
    }
    if (!result.ok()) return result;
    const auto unpadded = clip.segments;
    for (auto& segment : clip.segments) {
        double previous = 0, next = o.sourceDuration > 0 ? o.sourceDuration : segment.endSec;
        for (const auto& other : unpadded) {
            if (other.endSec <= segment.startSec) previous = qMax(previous, other.endSec);
            if (other.startSec >= segment.endSec) next = qMin(next, other.startSec);
        }
        // Padding may occupy silence, never an extra question/word excluded from the audit.
        for (const auto& phrase : phrases) {
            if (phrase.marker || (phrase.id >= segment.startPhrase && phrase.id <= segment.endPhrase)) continue;
            if (phrase.startSec < segment.startSec) previous = qMax(previous, qMin(segment.startSec, phrase.endSec));
            if (phrase.endSec > segment.endSec) next = qMin(next, qMax(segment.endSec, phrase.startSec));
        }
        segment.startSec -= qMin(o.paddingSec, qMax(0.0, segment.startSec - previous) / 2);
        segment.endSec += qMin(o.paddingSec, qMax(0.0, next - segment.endSec) / 2);
        segment.eventIds.clear();
        for (const auto& value : events) {
            const auto event = value.toObject();
            const int pos = positions.value(event["evidence_phrase"].toInt(), -1);
            if (pos < 0 || phrases[pos].marker || phrases[pos].uncertain || phrases[pos].flagCheck) continue;
            if (segment.startSec <= phrases[pos].startSec + .001 && segment.endSec >= phrases[pos].endSec - .001)
                segment.eventIds.append(event["event_id"].toString());
        }
        if (clip.protocolVersion == 2) {
            QStringList text;
            for (const auto& phrase : phrases)
                if (!phrase.marker && phrase.startSec >= segment.startSec - .001 && phrase.endSec <= segment.endSec + .001)
                    text << phrase.text;
            segment.quote = text.join(" ");
        }
    }
    if (clip.openingMode == "cold_open" && !clip.segments.isEmpty()
        && clip.segments.first().endSec - clip.segments.first().startSec > 12.0 + .001)
        fail("HOOK_TOO_LONG", clip.segments.first().blockId, "opening_mode", "cold_open", "хук <=12 с или natural", "Холодное открытие слишком длинное");
    QSet<QString> covered;
    int endingIndex = -1;
    for (int i = 0; i < clip.segments.size(); ++i) {
        for (const auto& id : clip.segments[i].eventIds) covered.insert(id);
        if (clip.segments[i].eventIds.contains(plan["ending_event"].toString())) endingIndex = i;
    }
    for (const auto& id : plan["required_events"].toArray()) if (!covered.contains(id.toString())) {
        QJsonObject evidence;
        for (const auto& event : events) if (event.toObject()["event_id"] == id) evidence = event.toObject();
        fail("MISSING_EVENT", {}, "required_events", id, "доказательство включено целиком", "Пропущено обязательное событие", evidence);
    }
    if (!plan.isEmpty()) {
        bool validEnding = endingIndex >= 0;
        for (int i = endingIndex + 1; validEnding && i < clip.segments.size(); ++i) {
            const auto& tail = clip.segments[i]; const auto& previous = clip.segments[i - 1];
            validEnding = (tail.beat.compare("reaction", Qt::CaseInsensitive) == 0 || tail.beat.compare("context", Qt::CaseInsensitive) == 0)
                && tail.startSec >= previous.endSec - .001 && tail.startSec - previous.endSec <= 8
                && positions.value(previous.endPhrase, -2) + 1 == positions.value(tail.startPhrase, -1);
        }
        if (!validEnding) fail("MISSING_ENDING", {}, "ending_event", plan["ending_event"],
            "финальное событие в завершающем эпизоде", "После финала возможна только соседняя подготовленная реакция");
    }
    const double duration = clip.totalSec();
    QJsonArray lengths;
    double first = o.sourceDuration, last = 0;
    for (const auto& segment : clip.segments) {
        first = qMin(first, segment.startSec); last = qMax(last, segment.endSec);
        lengths.append(QJsonObject{{"block_id", segment.blockId}, {"seconds", segment.endSec - segment.startSec}});
    }
    if (duration < o.minSec - .001 || duration > o.maxSec + .001)
        fail(duration < o.minSec ? "DRAFT_TOO_SHORT" : "DRAFT_TOO_LONG", {}, "duration_sec", duration,
            QString("%1–%2 с").arg(o.minSec).arg(o.maxSec), "Измени выбор материала, сохранив причины и финал",
            {{"actual_sec", duration}, {"min_sec", o.minSec}, {"max_sec", o.maxSec},
             {"deficit_sec", qMax(0.0, o.minSec - duration)}, {"excess_sec", qMax(0.0, duration - o.maxSec)}, {"blocks", lengths}});
    if (o.maxSpanSec > 0 && last - first > o.maxSpanSec)
        fail("SOURCE_SPAN", {}, "span_sec", last - first, QString::number(o.maxSpanSec), "Превышен охват исходника");
    if (result.ok()) clip.timelineHash = hash(clip);
    return result;
}
}
