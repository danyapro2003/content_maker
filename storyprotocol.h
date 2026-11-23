#pragma once
#include "storytypes.h"
#include "stories3runner.h"
#include <QJsonArray>
#include <QJsonDocument>

namespace StoryProtocol {
struct ValidationIssue {
    QString code, stage, branchId, blockId, field;
    QJsonValue actual;
    QString expected, explanation;
    QJsonObject evidence;
    QJsonObject json() const;
    QString message() const;
};
template<class T> struct ParseResult {
    T value{};
    QList<ValidationIssue> issues;
    bool ok() const { return issues.isEmpty(); }
    QStringList messages() const {
        QStringList out; for (const auto& issue : issues) out << issue.message(); return out;
    }
    QJsonArray errors() const {
        QJsonArray out; for (const auto& issue : issues) out.append(issue.json()); return out;
    }
};
ParseResult<QJsonDocument> document(QString text);
ParseResult<StoryClip> parseEdit(const QString& text, const QList<S3Phrase>& phrases,
                               const QString& branchId = {});
ParseResult<QJsonObject> parseAudit(const QString& text, const StoryClip& story);
}
