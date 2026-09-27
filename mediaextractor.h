#ifndef MEDIAEXTRACTOR_H
#define MEDIAEXTRACTOR_H

#include <QObject>

class MediaExtractor : public  QObject
{

    Q_OBJECT

public:

    MediaExtractor();

    void runFFprobe(QString path); // запуск ffprobe для получения инфы о видео
    // void extractSubtitles(int TrackInd0ex);

signals:
    void ffprobeFinished(const QByteArray& jsonData);
    void errorOccurred(const QString& errorMessage);

private:
    QString currentFilePath; // текущий путь файла
};

#endif // MEDIAEXTRACTOR_H
