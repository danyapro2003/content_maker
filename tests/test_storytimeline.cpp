#include "storytimeline.h"
#include <stdexcept>

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void testStoryTimeline()
{
    QList<S3Phrase> phrases;
    for (int i = 0; i < 4; ++i) {
        S3Phrase p; p.id = i + 1; p.startSec = 1 + i * 3; p.endSec = p.startSec + 2;
        p.text = QString("Реплика %1").arg(i + 1); phrases.append(p);
    }
    phrases[0].endSec = 4.04; // two consecutive ASR phrases overlap by 40 ms
    StoryClip clip; clip.protocolVersion = 2; clip.branchId = "B1";
    clip.title = "История"; clip.storySummary = "Причина и результат"; clip.hookReason = "Интересный вопрос"; clip.score = 85;
    for (int i = 0; i < phrases.size(); ++i) {
        StorySegment segment; segment.blockId = QString("B1_%1").arg(i + 1);
        segment.startPhrase = segment.endPhrase = i + 1; segment.beat = i == 3 ? "reaction" : "development";
        clip.segments.append(segment);
    }
    StoryTimeline::Options options; options.sourceDuration = 100; options.minSec = 5; options.maxSec = 30;
    const QJsonArray events{QJsonObject{{"event_id", "C1E3"}, {"evidence_phrase", 3}}};
    const QJsonObject plan{{"required_events", QJsonArray{"C1E3"}}, {"ending_event", "C1E3"}};
    auto compiled = StoryTimeline::compile(clip, phrases, events, plan, options);
    require(compiled.ok() && compiled.value.story.segments.size() == 3, "small adjacent ASR overlap not normalized");
    require(compiled.value.normalizations.size() == 1 && compiled.value.story.segments.first().endPhrase == 2,
        "normalization lost phrase or provenance");
    require(compiled.value.story.segments[1].eventIds.contains("C1E3")
        && compiled.value.story.segments.last().eventIds.isEmpty(), "coverage depends on LLM labels / reaction rejected");
    require(compiled.value.story.totalSec() < 10 && compiled.value.story.totalSec() > 9, "overlap counted twice");
    auto changed = compiled.value.story; changed.segments[0].endSec += .1;
    require(StoryTimeline::hash(changed) != compiled.value.story.timelineHash, "timeline edit retained old audit hash");
    auto bad = clip; bad.segments[1].startPhrase = 1; bad.segments[1].endPhrase = 1;
    require(!StoryTimeline::compile(bad, phrases, events, plan, options).ok(), "repeated source accepted");
    bad = clip; bad.segments.removeAt(2);
    auto missing = StoryTimeline::compile(bad, phrases, events, plan, options);
    require(!missing.ok() && missing.messages().join(" ").contains("MISSING_EVENT"), "missing evidence concealed");
    options.minSec = 60;
    auto shortStory = StoryTimeline::compile(clip, phrases, events, plan, options);
    require(!shortStory.ok() && shortStory.messages().join(" ").contains("DRAFT_TOO_SHORT"), "short story passed");
    options.minSec = 0; options.maxSec = 9.01;
    require(!StoryTimeline::compile(clip, phrases, events, plan, options).ok(), "padding applied after length audit");
    options.maxSec = 100;
    auto distant = phrases; distant[3].startSec = 80; distant[3].endSec = 82;
    require(!StoryTimeline::compile(clip, distant, events, plan, options).ok(), "unrelated tail after ending accepted");
    auto uncertain = phrases; uncertain[2].flagCheck = true;
    require(!StoryTimeline::compile(clip, uncertain, events, plan, options).ok(), "uncertain evidence promoted to fact");
}
