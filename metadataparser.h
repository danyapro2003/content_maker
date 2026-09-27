#ifndef METADATAPARSER_H
#define METADATAPARSER_H

#include <QObject>
#include <QJsonDocument>

class MetadataParser: public  QObject
{

    Q_OBJECT

public:
    MetadataParser();

    void transformBytesToJson(const QByteArray& bytes);
signals:
    void transformBytesToJsonCompleted(const QJsonDocument& json);
};

#endif // METADATAPARSER_H