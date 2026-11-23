#pragma once
#include "storyprotocol.h"

namespace StoryTimeline {
struct Options {
    double sourceDuration = 0, minSec = 60, maxSec = 190, minScore = 60, maxSpanSec = 0;
    double paddingSec = .08, overlapToleranceSec = .08;
    int maxSegments = 12;
    bool checkLegacyQuotes = true, resolveEndpoints = true;
};
struct CompiledStory {
    StoryClip story;
    QJsonArray normalizations;
};
StoryProtocol::ParseResult<CompiledStory> compile(const StoryClip& draft, const QList<S3Phrase>& phrases,
    const QJsonArray& events, const QJsonObject& plan, const Options& options);
QString hash(const StoryClip& story);
}
