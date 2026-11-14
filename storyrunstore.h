#pragma once
#include <QJsonObject>
#include <QString>

// Content-addressed stage artifacts. ASR cache is deliberately independent.
class StoryRunStore {
public:
    bool open(const QString& root, const QJsonObject& dependencies);
    bool save(const QString& stage, const QString& key, const QJsonObject& data,
              const QString& status = "complete") const;
    QJsonObject load(const QString& stage, const QString& key, bool completeOnly = true) const;
    QString directory() const { return m_directory; }
    static QString hash(const QJsonObject& value);
    static QString hashText(const QString& value);
private:
    QString m_directory;
    QString path(const QString& stage, const QString& key) const;
};
