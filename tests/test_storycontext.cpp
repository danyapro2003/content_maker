#include "storycontext.h"
#include <stdexcept>

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void testStoryContext()
{
    QList<S3Phrase> phrases;
    for (int i = 0; i < 3; ++i) {
        S3Phrase p; p.id = i + 1; p.startSec = i * 3000; p.endSec = p.startSec + 20;
        p.text = i == 1 ? "POSTORONNY_DIALOG" : "STORY_DIALOG"; phrases.append(p);
    }
    const auto units = StoryContext::buildSourceUnits(phrases);
    require(units.size() == 3, "unrelated source intervals joined");
    const QJsonArray events{QJsonObject{{"event_id", "C1E1"}, {"evidence_phrase", 1}, {"observed", "cause"}},
        QJsonObject{{"event_id", "C2E2"}, {"evidence_phrase", 2}, {"observed", "unrelated"}},
        QJsonObject{{"event_id", "C3E3"}, {"evidence_phrase", 3}, {"observed", "payoff"}}};
    QJsonObject branch{{"branch_id", "B1"}, {"title", "Story"}, {"dramatic_question", "Question"},
        {"hook", QJsonObject{{"event_id", "C1E1"}, {"mode", "natural"}, {"why_interesting", "question"}}},
        {"chain", QJsonArray{QJsonObject{{"event_id", "C1E1"}, {"function", "cause"}, {"depends_on", QJsonArray{}}},
            QJsonObject{{"event_id", "C3E3"}, {"function", "payoff"}, {"depends_on", QJsonArray{"C1E1"}}}}},
        {"required_events", QJsonArray{"C1E1", "C3E3"}}, {"ending_event", "C3E3"}, {"ending_effect", "result"},
        {"source_unit_ids", QJsonArray{"U1", "U3"}}, {"missing_evidence", QJsonArray{}}};
    const auto context = StoryContext::build(branch, events, units, 40000);
    require(context.ok() && context.unitIds.size() == 2 && context.availableSec == 40, "whole-film chain unavailable");
    require(!context.text.contains("POSTORONNY_DIALOG") && context.text.contains("P3"), "full movie leaked into branch context");
    require(!StoryContext::build(branch, events, units, 10).ok(), "oversized context silently truncated");
    QJsonObject plan{{"protocol_version", 2}, {"action", "plan_stories"}, {"primary", QJsonArray{branch}}, {"reserve", QJsonArray{}}};
    require(StoryContext::plans(plan, events, units, 1, 3, 2, 30, 90, 40000).ok(), "valid causal chain rejected");
    require(!StoryContext::plans(plan, events, units, 1, 3, 2, 60, 90, 40000).ok(), "infeasible short plan accepted");
    require(!StoryContext::plans(plan, events, units, 1, 3, 2, 10, 30, 40000).ok(), "mandatory proof exceeds duration");
    auto cyclic = branch; auto chain = cyclic["chain"].toArray(); auto first = chain[0].toObject();
    first["depends_on"] = QJsonArray{"C3E3"}; chain[0] = first; cyclic["chain"] = chain;
    plan["primary"] = QJsonArray{cyclic};
    require(!StoryContext::plans(plan, events, units, 1, 3, 2, 30, 90, 40000).ok(), "causal dependency cycle accepted");
    QString error;
    require(StoryContext::unitsForReferences({{"purpose", "missing cause"}, {"phrase_ids", QJsonArray{9999}}}, events, units, error).isEmpty()
        && !error.isEmpty(), "context request escaped source IDs");
}
