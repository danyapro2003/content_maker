#ifndef CLIPRENDERER_H
#define CLIPRENDERER_H

#include <QObject>

class ClipRenderer : public QObject
{
    Q_OBJECT

public:
    ClipRenderer();

    // Один проход ffmpeg: нарезка [startSec, endSec] + вертикаль 9:16 (blur background)
    // + прожиг субтитров srtPath (слово-за-словом) + аудиодорожка audioIndex.
    void renderClip(const QString& inputPath, double startSec, double endSec,
                    int audioIndex, const QString& srtPath, const QString& outPath, bool verifyOutput = false);

    // Клип-пересказ: склейка нескольких сегментов + 9:16 + прожиг сабов + аудиодорожка.
    // crossfadeSec > 0 — xfade/acrossfade между сегментами.
    void renderStoryClip(const QString& inputPath, const QList<QPair<double, double>>& segments,
                         int audioIndex, const QString& srtPath, const QString& outPath,
                         double crossfadeSec = 0.0, bool verifyOutput = false,
                         const QStringList& framing = {}, const QString& music = {},
                         int firstMusicShot = -1, double hook = 0);

    void renderV5Story(const QString& source, const QList<QPair<double, double>>& segments,
                       int audio, const QString& subtitles, const QString& output, const QString& plan,
                       const QString& track = {}, double reactionRoom = 0);

signals:
    void logMessage(const QString& message);
    void clipRendered(const QString& outPath);
    void errorOccurred(const QString& errorMessage);

private:
    void startFfmpeg(const QStringList& args, const QString& workDir, const QString& outPath,
                    double expectedDuration = 0, int segments = 1, bool verifyOutput = false);
    void verifyAndPublish(const QString& temporaryPath, const QString& outPath, double expectedDuration, int segments);
};

#endif // CLIPRENDERER_H
