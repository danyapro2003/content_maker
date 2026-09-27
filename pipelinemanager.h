#ifndef PIPELINEMANAGER_H
#define PIPELINEMANAGER_H

#include <QObject>
#include <QQueue>
#include "mediaextractor.h"
#include "metadataparser.h"

class PipelineManager: public QObject
{

    Q_OBJECT

public:
    //Конструктор по умолчанию в Qt всегда принимает указатель на родителя
    explicit PipelineManager(QObject *parent = nullptr);

    void addFilesToQueue(const QStringList& files);
    void startPipeline(const QString path);


signals:
    // void pipelineFinished();
    // void progressUpdated(int percent);
private slots:
    //слот вызывается когда ффпроб достает данные из видео файла
    void onffprobeFinished(const QByteArray& data);
    void ontransformBytesToJsonCompleted(const QJsonDocument& jsonData);
    void onErrorOccurredExtractManager(const QString& errorMessage);

private:
    void processNext(); // Запуск следующего файла из очереди

    QQueue<QString> m_fileQueue; //Очередь файлов
    bool m_isProcessing = false; //файл в обработке или нет

    QByteArray ffprobeMetaData;
    MediaExtractor extractor;
    MetadataParser dataParser;

    QString currentFilePath;
    QString fileName; //имя с расширением
    QString baseName; //имя без расширения
};

#endif // PIPELINEMANAGER_H