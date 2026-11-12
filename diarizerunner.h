#ifndef DIARIZERUNNER_H
#define DIARIZERUNNER_H

#include <QObject>
#include "transcriptpacker.h"

class QProcess;

// Диаризация спикеров через pyannote.audio: QProcess -> scripts/diarize.py -> JSON.
// Требует: python venv с pyannote.audio (настройка pythonPath) + HF токен.
class DiarizeRunner : public QObject
{
    Q_OBJECT

public:
    DiarizeRunner();

    void diarize(const QString& wavPath, const QString& outJsonPath);

    // парсинг JSON от diarize.py: [{start, end, speaker}, ...]
    static QList<DiarSegment> parseJson(const QString& path);

signals:
    void diarizationDone(const QString& jsonPath);
    void errorOccurred(const QString& errorMessage);

private:
    QProcess* m_proc = nullptr;
    QString m_outPath;
};

#endif // DIARIZERUNNER_H
