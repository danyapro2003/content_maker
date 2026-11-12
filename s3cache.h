#ifndef S3CACHE_H
#define S3CACHE_H

#include <QJsonObject>
#include <QStringList>

namespace S3Cache {
QString fileFingerprint(const QString& path);
QString makeKey(const QString& source, const QJsonObject& parameters,
                const QStringList& dependencies = {});
bool valid(const QString& path, const QString& key);
bool writeMeta(const QString& path, const QString& key);
}

#endif
