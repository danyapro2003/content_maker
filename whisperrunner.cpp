/* whisperrunner.cpp */

#include "whisperrunner.h"
#include "appsettings.h"
#include <QProcess>
#include <QFile>
#include <QFileInfo>

WhisperRunner::WhisperRunner() {}

// DTW-пресет из имени файла модели (пресеты из whisper.cpp: tiny, base, small, medium, large.v1/v2/v3...)
static QString dtwPresetForModel(const QString& modelPath)
{
    QString f = QFileInfo(modelPath).fileName().toLower();
    if (f.contains("turbo"))     return "large.v3.turbo";
    if (f.contains("large-v3"))  return "large.v3";
    if (f.contains("large-v2"))  return "large.v2";
    if (f.contains("large"))     return "large.v1";
    if (f.contains("medium.en")) return "medium.en";
    if (f.contains("medium"))    return "medium";
    if (f.contains("small.en"))  return "small.en";
    if (f.contains("small"))     return "small";
    if (f.contains("base.en"))   return "base.en";
    if (f.contains("base"))      return "base";
    if (f.contains("tiny.en"))   return "tiny.en";
    if (f.contains("tiny"))      return "tiny";
    return QString();
}

void WhisperRunner::transcribe(const QString& wavPath, const QString& outBaseName,
                               const QString& lang, bool wordLevel)
{
    const AppSettings& s = AppSettings::instance();
    if (s.whisperCliPath.isEmpty() || s.whisperModelPath.isEmpty()) {
        emit errorOccurred("Whisper: не настроены пути к whisper-cli и/или модели (вкладка «Настройки»).");
        return;
    }

    m_currentSrtPath = outBaseName + ".srt";

    QStringList args;
    args << "-m" << s.whisperModelPath
         << "-f" << wavPath
         << "-osrt"
         << "-of" << outBaseName;

    if (!lang.isEmpty())
        args << "-l" << lang;

    if (wordLevel)
        args << "-ml" << "1" << "-sow";   // одно слово на сегмент
    if (wordLevel && s.whisperDtw) {
        QString preset = dtwPresetForModel(s.whisperModelPath);
        if (!preset.isEmpty()) {
            // DTW даёт точное выравнивание слов по звуку; несовместим с flash-attn
            args << "-dtw" << preset << "-nfa";
        }
    }

    // ОБЯЗАТЕЛЬНО для обоих режимов:
    // -mc 0 — не передавать предыдущий текст как контекст. С контекстом whisper на длинном
    //         аудио «теряется» и зацикливается на одной фразе (проверено: 2 часа фильма
    //         превратились в 3295 повторов «тебе не понравится!»).
    // --vad — пропускать музыку/тишину: меньше галлюцинаций и в ~2.5 раза быстрее.
    args << "-mc" << "0";
    args << "--vad";
    if (!s.whisperVadModelPath.isEmpty())
        args << "-vm" << s.whisperVadModelPath;

    QProcess* proc = new QProcess(this);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [=](int exitCode, QProcess::ExitStatus exitStatus) {
                bool ok = (exitStatus == QProcess::NormalExit && exitCode == 0
                           && QFile::exists(m_currentSrtPath));
                if (ok) {
                    emit transcriptionDone(m_currentSrtPath);
                } else {
                    emit errorOccurred(QString("Ошибка whisper (код %1): %2")
                                           .arg(exitCode)
                                           .arg(QString::fromLocal8Bit(proc->readAllStandardError())));
                }
                proc->deleteLater();
            });

    proc->start(s.whisperCliPath, args);
}
