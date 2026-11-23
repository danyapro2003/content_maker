#pragma once
#include "storyprotocol.h"

namespace StoryContext {
struct SourceUnit {
    QString id;
    QList<S3Phrase> phrases;
    int episode = 0;
    double duration() const { return phrases.isEmpty() ? 0 : phrases.last().endSec - phrases.first().startSec; }
    QJsonObject card() const;
};
struct BranchContext {
    QString text;
    QJsonArray events;
    QStringList unitIds;
    QJsonArray issues;
    double availableSec = 0, evidenceSec = 0;
    bool ok() const { return issues.isEmpty(); }
};
QList<SourceUnit> buildSourceUnits(const QList<S3Phrase>& phrases);
QJsonArray eventCards(const QJsonArray& events, const QList<SourceUnit>& units);
BranchContext build(const QJsonObject& plan, const QJsonArray& events, const QList<SourceUnit>& units,
                    int maxChars, const QStringList& extraUnits = {});
QStringList unitsForReferences(const QJsonObject& request, const QJsonArray& events,
                               const QList<SourceUnit>& units, QString& error);
StoryProtocol::ParseResult<QJsonArray> plans(const QJsonObject& response, const QJsonArray& events,
    const QList<SourceUnit>& units, int target, int maximum, int reserve, double minSec, double maxSec, int maxContext, bool allowVariants = false);
}
