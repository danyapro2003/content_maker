/* pipelinemanager.cpp*/

#include "pipelinemanager.h"
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>


PipelineManager::PipelineManager(QObject *parent) : QObject(parent)
{
    // Здесь будет инициализация (например, создание MediaExtractor)

    connect(&extractor, &MediaExtractor::ffprobeFinished, this, &PipelineManager::onffprobeFinished);
    connect(&dataParser, &MetadataParser::transformBytesToJsonCompleted, this, &PipelineManager::ontransformBytesToJsonCompleted);
    connect(&extractor, &MediaExtractor::errorOccurred, this, &PipelineManager::onErrorOccurredExtractManager);
}

void PipelineManager::onErrorOccurredExtractManager(const QString& errorMessage) {
    qDebug() << "[ExtractManager_error] " << errorMessage;
}

void PipelineManager::addFilesToQueue(const QStringList& files)
{
    for (const QString& file : files) {
        m_fileQueue.enqueue(file);
    }

    if (!m_isProcessing) {
        processNext();
    }
}

void PipelineManager::processNext()
{
    if (m_fileQueue.isEmpty()) {
        m_isProcessing = false;
        qDebug() << "=== Очередь пуста. Все файлы успешно обработаны! ===";
        return;
    }

    m_isProcessing = true;
    QString nextPath = m_fileQueue.dequeue();
    startPipeline(nextPath);
}

void PipelineManager::startPipeline(const QString path) {
    qDebug() <<"start pipeline " << path;
    currentFilePath = path;
    fileName = QFileInfo(currentFilePath).fileName();
    baseName = QFileInfo(currentFilePath).baseName();

    extractor.runFFprobe(path);
}

void PipelineManager::onffprobeFinished(const QByteArray& data){
    qDebug() << "ffprobe finished";
    ffprobeMetaData = data;
    qDebug() << "start parser";
    dataParser.transformBytesToJson(data);
}

void PipelineManager::ontransformBytesToJsonCompleted(const QJsonDocument& jsonData) {
    QString savePath = QFileInfo(currentFilePath).absoluteDir().filePath(baseName + ".json");
    qDebug() << "parser is completed" << savePath;

    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Ошибка открытия файла:" << file.errorString();
    } else {
        file.write(jsonData.toJson(QJsonDocument::Indented));
        file.close();
    }

    // Текущий файл полностью закончен, берем следующий
    processNext();
}