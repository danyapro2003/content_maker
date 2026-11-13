#pragma once
#include "llmclient.h"

// Pure prompt/data helpers. Viewer input deliberately has no source or editorial metadata.
namespace StoryNarrative {
QList<QList<S3Phrase>> chunks(const QList<S3Phrase>& phrases, int maxChars = 14000);
QString transcript(const QList<S3Phrase>& phrases);
QString indexPrompt(const QList<S3Phrase>& phrases, int chunk);
struct EventCheck {
    QJsonArray accepted, rejected, corrections;
    QStringList errors;
    bool complete() const { return errors.isEmpty(); }
    QJsonObject report() const;
};
EventCheck checkEvents(const QJsonObject& response, const QList<S3Phrase>& chunk,
                      int chunkNumber, const QJsonArray& existing = {});
EventCheck checkEventRepairs(const QJsonObject& response, const QJsonArray& pending,
                            const QList<S3Phrase>& chunk, int chunkNumber, const QJsonArray& existing);
QString eventRepairPrompt(const QList<S3Phrase>& chunk, int chunkNumber,
                         const QJsonArray& pending, const QJsonArray& existing);
QString planPrompt(const QJsonArray& events, int minimum, int maximum, double minSec, double maxSec);
QString editPrompt(double minSec, double maxSec, int maxSegments, double preferredBlockSec);
QString viewerMaterial(const StoryClip& clip, const QList<S3Phrase>& phrases);
QString auditPrompt(bool fidelity);
bool acceptAudit(const QJsonObject& report, int segments, QString& error);
}
