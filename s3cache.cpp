#include "s3cache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

QString S3Cache::fileFingerprint(const QString& path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile())
        return QStringLiteral("missing:") + path;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(info.canonicalFilePath().toUtf8());
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    // Scripts and JSON artifacts need content identity; large media/models use stat.
    if (info.size() <= 16 * 1024 * 1024) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
            return QStringLiteral("unreadable:") + path;
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString S3Cache::makeKey(const QString& source, const QJsonObject& parameters,
                       const QStringList& dependencies)
{
    QJsonArray inputs;
    for (const QString& path : dependencies)
        inputs.append(fileFingerprint(path));
    const QJsonObject value{{"schema", 3}, {"source", fileFingerprint(source)},
                            {"parameters", parameters}, {"inputs", inputs}};
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

bool S3Cache::valid(const QString& path, const QString& key)
{
    QFile meta(path + ".srcmeta");
    if (!QFileInfo(path).isFile() || !meta.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject value = QJsonDocument::fromJson(meta.readAll()).object();
    return value.value("key").toString() == key
           && value.value("artifact").toString() == fileFingerprint(path);
}

bool S3Cache::writeMeta(const QString& path, const QString& key)
{
    if (!QFileInfo(path).isFile())
        return false;
    QSaveFile meta(path + ".srcmeta");
    if (!meta.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = QJsonDocument(QJsonObject{
        {"key", key}, {"artifact", fileFingerprint(path)}}).toJson(QJsonDocument::Compact);
    return meta.write(data) == data.size() && meta.commit();
}
