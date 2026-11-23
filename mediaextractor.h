#ifndef MEDIAEXTRACTOR_H
#define MEDIAEXTRACTOR_H

#include <QObject>

class MediaExtractor : public QObject
{
    Q_OBJECT

public:
    MediaExtractor();

    void runFFprobe(const QString& path);

    // Извлечение субтитров: ffmpeg -i file -map 0:s:subIndex outPath
    void extractSubtitle(const QString& filePath, int subIndex, const QString& outPath);

    // Извлечение аудиодорожки в wav 16kHz mono (для whisper).
    // startSec < 0 — весь файл, иначе сегмент [startSec, endSec].
    void extractAudioWav(const QString& filePath, int audioIndex, const QString& outPath,
                         double startSec = -1.0, double endSec = -1.0);

    // Склейка нескольких аудио-сегментов в один wav 16kHz mono (для whisper по пересказу).
    // crossfadeSec > 0 — acrossfade между сегментами (должен совпадать с рендером!)
    void extractConcatAudioWav(const QString& filePath, int audioIndex,
                               const QList<QPair<double, double>>& segments,
                               const QString& outPath, double crossfadeSec = 0.0);

    // Кадры выбранных фрагментов (агентный режим пересказа 2.0): LLM сама просит показать
    // нужные ей диапазоны. Диапазоны обрабатываются ПОСЛЕДОВАТЕЛЬНО (по одному процессу
    // ffmpeg с ограничением потоков — иначе на больших файлах несколько декодеров подряд
    // съедают всю память). Файлы: outDir/r<номер диапазона>_%04d.jpg, 1 кадр / intervalSec.
    void extractFramesRanges(const QString& filePath,
                             const QList<QPair<double, double>>& ranges,
                             double intervalSec, const QString& outDir);

signals:
    void ffprobeFinished(const QByteArray& jsonData);
    void subtitleExtracted(const QString& outPath);
    void audioExtracted(const QString& outPath);
    void framesExtracted(const QString& dirPath);
    void logMessage(const QString& msg);
    void errorOccurred(const QString& errorMessage);

private:
    enum class JobType { Ffprobe, Subtitle, Audio, Frames, FramesRange };

    void startJob(JobType type, const QString& program, const QStringList& args,
                  const QString& outPath = QString(), int timeoutMs = 0);

    // последовательное извлечение кадров по диапазонам
    void startNextFrameRange();
    void finishFrameStep(bool ok, const QString& errText);

    JobType m_currentJob = JobType::Ffprobe;
    QString m_currentOutPath;

    QString m_framePath;
    QString m_frameOutDir;
    double m_frameInterval = 2.0;
    QList<QPair<double, double>> m_frameRanges;
    int m_frameIndex = 0;
};

#endif // MEDIAEXTRACTOR_H
