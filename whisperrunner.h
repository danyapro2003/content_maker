#ifndef WHISPERRUNNER_H
#define WHISPERRUNNER_H

#include <QObject>

class WhisperRunner : public QObject
{
    Q_OBJECT

public:
    WhisperRunner();

    // wordLevel = true: по одному слову на сегмент (-ml 1 -sow) — для сабов в клипах.
    // wordLevel = false: полная транскрибация (--vad -mc 0) — fallback вместо субтитров.
    // Результат: <outBaseName>.srt
    void transcribe(const QString& wavPath, const QString& outBaseName,
                    const QString& lang, bool wordLevel);

signals:
    void transcriptionDone(const QString& srtPath);
    void errorOccurred(const QString& errorMessage);

private:
    QString m_currentSrtPath;
};

#endif // WHISPERRUNNER_H
