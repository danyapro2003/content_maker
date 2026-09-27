/* metadataparser.cpp*/
#include "metadataparser.h"

MetadataParser::MetadataParser() {}

void MetadataParser::transformBytesToJson(const QByteArray& bytes) {
    // 1. Парсим байты в документ Qt
    QJsonDocument jsonDoc = QJsonDocument::fromJson(bytes);

    // 2. Превращаем в красивую строку с отступами и переносами
    //QString prettyJson = jsonDoc.toJson(QJsonDocument::Indented);

    // 3. Выводим в консоль без кавычек
    //qDebug().noquote() << prettyJson;

    //return jsonDoc;

    emit transformBytesToJsonCompleted(jsonDoc);
}