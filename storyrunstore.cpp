#include "storyrunstore.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

QString StoryRunStore::hash(const QJsonObject& value)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}
QString StoryRunStore::hashText(const QString& value)
{
    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}
bool StoryRunStore::open(const QString& root, const QJsonObject& dependencies)
{
    m_directory.clear();
    if (root.isEmpty()) return false;
    const auto directory = QDir(root).filePath(hash(dependencies));
    if (!QDir().mkpath(directory)) return false;
    QSaveFile file(directory + "/manifest.json");
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"dependencies", dependencies}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return false;
    m_directory = directory;
    return true;
}
QString StoryRunStore::path(const QString& stage, const QString& key) const
{
    static const QRegularExpression safe("\\A[a-zA-Z0-9_-]+\\z");
    if (m_directory.isEmpty() || !safe.match(stage).hasMatch() || !safe.match(key).hasMatch()) return {};
    return m_directory + "/" + stage + "_" + key + ".json";
}
bool StoryRunStore::save(const QString& stage, const QString& key, const QJsonObject& data, const QString& status) const
{
    const auto target = path(stage, key);
    if (target.isEmpty()) return false;
    QSaveFile file(target);
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"key", key}, {"stage", stage},
        {"status", status}, {"data", data}, {"data_hash", hash(data)},
        {"saved_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QJsonObject StoryRunStore::load(const QString& stage, const QString& key, bool completeOnly) const
{
    const auto target = path(stage, key);
    if (target.isEmpty()) return {};
    QFile file(target);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto envelope = QJsonDocument::fromJson(file.readAll()).object();
    const auto data = envelope["data"].toObject();
    if (envelope["version"] != 1 || envelope["key"] != key || envelope["stage"] != stage
        || !envelope["data"].isObject() || envelope["data_hash"] != hash(data)
        || (completeOnly && envelope["status"] != "complete")) return {};
    return envelope;
}
