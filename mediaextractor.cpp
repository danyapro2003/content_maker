/* mediaextractor.cpp */

#include "mediaextractor.h"
#include "appsettings.h"
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QPointer>

MediaExtractor::MediaExtractor() {}

void MediaExtractor::startJob(JobType type, const QString& program, const QStringList& args,
                              const QString& outPath, int timeoutMs)
{
    m_currentJob = type;
    m_currentOutPath = outPath;

    emit logMessage(program + " " + args.join(' '));

    QProcess* proc = new QProcess(this);

    // watchdog: гарантированно убиваем процесс по превышении времени —
    // иначе зависший ffmpeg остаётся висеть и съедает всю память
    if (timeoutMs > 0) {
        QTimer* watchdog = new QTimer(this);
        watchdog->setSingleShot(true);
        QPointer<QProcess> guard(proc);
        connect(watchdog, &QTimer::timeout, this, [this, watchdog, guard, timeoutMs]() {
            if (guard && guard->state() != QProcess::NotRunning) {
                emit logMessage(QString("[ПРЕДУПРЕЖДЕНИЕ] процесс не завершился за %1 мс — убиваю")
                                    .arg(timeoutMs));
                guard->kill();
            }
            watchdog->deleteLater();
        });
        watchdog->start(timeoutMs);
    }

    // без этого пайплайн виснет вечно, если программу не удалось запустить
    connect(proc, &QProcess::errorOccurred, this,
            [=](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                QString msg = QString("Не удалось запустить %1: %2")
                                  .arg(program, proc->errorString());
                if (type == JobType::FramesRange)
                    finishFrameStep(false, msg);
                else
                    emit errorOccurred(msg);
                proc->deleteLater();
            });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [=](int exitCode, QProcess::ExitStatus exitStatus) {
                bool ok = (exitStatus == QProcess::NormalExit && exitCode == 0);
                // Для задач с выходным файлом дополнительно проверяем, что он создан
                if (ok && !outPath.isEmpty() && !QFile::exists(outPath))
                    ok = false;

                if (type == JobType::FramesRange) {
                    // сбой одного диапазона не ломает остальные
                    finishFrameStep(ok, QString("код %1: %2")
                                            .arg(exitCode)
                                            .arg(QString::fromLocal8Bit(proc->readAllStandardError()).right(300)));
                    proc->deleteLater();
                    return;
                }

                if (!ok) {
                    emit errorOccurred(QString("Ошибка процесса %1 (код %2): %3")
                                           .arg(program)
                                           .arg(exitCode)
                                           .arg(QString::fromLocal8Bit(proc->readAllStandardError())));
                } else {
                    switch (type) {
                    case JobType::Ffprobe:
                        emit ffprobeFinished(proc->readAllStandardOutput());
                        break;
                    case JobType::Subtitle:
                        emit subtitleExtracted(outPath);
                        break;
                    case JobType::Audio:
                        emit audioExtracted(outPath);
                        break;
                    case JobType::FramesRange:
                        break; // обработано выше
                    }
                }
                proc->deleteLater();
            });

    proc->start(program, args);
}

void MediaExtractor::runFFprobe(const QString& path)
{
    QStringList args;
    args << "-v" << "error"
         << "-print_format" << "json"
         << "-show_streams"
         << "-show_chapters"
         << "-show_format"
         << path;

    startJob(JobType::Ffprobe, AppSettings::instance().ffprobePath, args);
}

void MediaExtractor::extractSubtitle(const QString& filePath, int subIndex, const QString& outPath)
{
    QStringList args;
    args << "-y"
         << "-i" << filePath
         << "-map" << QString("0:s:%1").arg(subIndex)
         << "-c:s" << "srt"
         << outPath;

    startJob(JobType::Subtitle, AppSettings::instance().ffmpegPath, args, outPath);
}

void MediaExtractor::extractConcatAudioWav(const QString& filePath, int audioIndex,
                                           const QList<QPair<double, double>>& segments,
                                           const QString& outPath, double crossfadeSec)
{
    if (segments.isEmpty()) {
        emit errorOccurred("extractConcatAudioWav: пустой список сегментов");
        return;
    }

    QStringList args;
    args << "-y";
    // каждый сегмент — отдельный вход с input-сеek (кадр-точно, т.к. дальше декодируем)
    for (const auto& seg : segments) {
        double dur = seg.second - seg.first;
        if (dur <= 0.0)
            dur = 1.0;
        args << "-ss" << QString::number(seg.first, 'f', 3)
             << "-t" << QString::number(dur, 'f', 3)
             << "-i" << filePath;
    }

    const AppSettings& settings = AppSettings::instance();
    double fadeSec = settings.editAudioFadeMs / 1000.0;
    bool useFade = fadeSec > 0.0 && crossfadeSec <= 0.0 && segments.size() > 1;

    QString filter;
    for (int i = 0; i < segments.size(); ++i) {
        double dur = segments[i].second - segments[i].first;
        filter += QString("[%1:a:%2]asetpts=PTS-STARTPTS").arg(i).arg(audioIndex);
        // 30мс фейды на границах — убирают щелчки на склейках (hard rule video-use)
        if (useFade && dur > 2.0 * fadeSec)
            filter += QString(",afade=t=in:st=0:d=%1,afade=t=out:st=%2:d=%1")
                          .arg(fadeSec, 0, 'f', 3).arg(dur - fadeSec, 0, 'f', 3);
        filter += QString("[ap%1];").arg(i);
    }

    if (crossfadeSec > 0.0 && segments.size() > 1) {
        // acrossfade-цепочка — длительность должна совпадать с xfade в рендере
        QString prev = "ap0";
        for (int k = 1; k < segments.size(); ++k) {
            QString out = (k == segments.size() - 1) ? QString("aout") : QString("ac%1").arg(k);
            filter += QString("[%1][ap%2]acrossfade=d=%3[%4];")
                          .arg(prev).arg(k).arg(crossfadeSec, 0, 'f', 3).arg(out);
            prev = out;
        }
    } else {
        for (int i = 0; i < segments.size(); ++i)
            filter += QString("[ap%1]").arg(i);
        filter += QString("concat=n=%1:v=0:a=1[aout]").arg(segments.size());
    }

    args << "-filter_complex" << filter
         << "-map" << "[aout]"
         << "-vn"
         << "-ac" << "1"
         << "-ar" << "16000"
         << "-c:a" << "pcm_s16le"
         << outPath;

    startJob(JobType::Audio, AppSettings::instance().ffmpegPath, args, outPath);
}

void MediaExtractor::extractFramesRanges(const QString& filePath,
                                         const QList<QPair<double, double>>& ranges,
                                         double intervalSec, const QString& outDir)
{
    m_framePath = filePath;
    m_frameRanges = ranges;
    m_frameOutDir = outDir;
    m_frameIndex = 0;
    m_frameInterval = intervalSec < 0.2 ? 0.2 : intervalSec;

    if (m_frameRanges.isEmpty()) {
        emit framesExtracted(outDir);
        return;
    }

    QDir().mkpath(outDir);
    startNextFrameRange();
}

void MediaExtractor::startNextFrameRange()
{
    if (m_frameIndex >= m_frameRanges.size()) {
        emit framesExtracted(m_frameOutDir);
        return;
    }

    const QPair<double, double> r = m_frameRanges[m_frameIndex];
    double dur = qMax(0.5, r.second - r.first);
    QString pattern = QString("%1/r%2_").arg(m_frameOutDir).arg(m_frameIndex) + "%04d.jpg";

    // ОДИН вход и ОДИН выход на диапазон: несколько параллельных декодеров одного
    // большого файла съедают всю оперативку (проверено: 16 ГБ machine -> зависание)
    QStringList args;
    args << "-nostdin" << "-hide_banner" << "-y"
         << "-threads" << "2"
         << "-ss" << QString::number(r.first, 'f', 3)
         << "-t" << QString::number(dur, 'f', 3)
         << "-i" << m_framePath
         << "-map" << "0:v:0" // именно первый видеопоток: в файле могут быть обложки (attached_pic)
         << "-vf" << QString("fps=1/%1,scale=640:-2").arg(m_frameInterval, 0, 'f', 3)
         << "-frames:v" << QString::number(qMax(1, int(dur / m_frameInterval) + 2))
         << "-q:v" << "5"
         << pattern;

    // 2 минуты на диапазон: input-seek быстрый, больше не нужно
    startJob(JobType::FramesRange, AppSettings::instance().ffmpegPath, args, QString(), 120000);
}

void MediaExtractor::finishFrameStep(bool ok, const QString& errText)
{
    if (!ok)
        emit logMessage(QString("[ПРЕДУПРЕЖДЕНИЕ] кадры диапазона %1 не извлечены (%2)")
                            .arg(m_frameIndex).arg(errText));

    ++m_frameIndex;
    startNextFrameRange();
}

void MediaExtractor::extractAudioWav(const QString& filePath, int audioIndex, const QString& outPath,
                                     double startSec, double endSec)
{
    QStringList args;
    args << "-y";
    if (startSec >= 0.0) {
        double dur = endSec - startSec;
        if (dur <= 0.0)
            dur = 1.0;
        args << "-ss" << QString::number(startSec, 'f', 3)
             << "-t" << QString::number(dur, 'f', 3);
    }
    args << "-i" << filePath
         << "-map" << QString("0:a:%1").arg(audioIndex)
         << "-vn"
         << "-ac" << "1"
         << "-ar" << "16000"
         << "-c:a" << "pcm_s16le"
         << outPath;

    startJob(JobType::Audio, AppSettings::instance().ffmpegPath, args, outPath);
}
