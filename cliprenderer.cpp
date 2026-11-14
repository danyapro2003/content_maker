/* cliprenderer.cpp */

#include "cliprenderer.h"
#include "appsettings.h"
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <cmath>

ClipRenderer::ClipRenderer() {}

void ClipRenderer::startFfmpeg(const QStringList& arguments, const QString& workDir, const QString& outPath,
                               double expectedDuration, int segments, bool verifyOutput)
{
    const QString temporaryPath = verifyOutput ? outPath.left(outPath.size() - 4) + ".part.mp4" : outPath;
    if (verifyOutput && QFileInfo::exists(outPath)) {
        emit errorOccurred("Готовый результат уже существует; нужен новый каталог запуска: " + outPath); return;
    }
    QStringList args = arguments;
    args.last() = temporaryPath;
    auto* proc = new QProcess(this);
    if (!workDir.isEmpty()) proc->setWorkingDirectory(workDir);
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || proc->property("reported").toBool()) return;
        proc->setProperty("reported", true);
        emit errorOccurred("Не удалось запустить ffmpeg: " + proc->errorString()); proc->deleteLater();
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, proc, temporaryPath, outPath, expectedDuration, segments, verifyOutput](int code, QProcess::ExitStatus status) {
            if (proc->property("reported").toBool()) return;
            proc->setProperty("reported", true);
            if (status != QProcess::NormalExit || code != 0 || !QFileInfo(temporaryPath).isFile() || QFileInfo(temporaryPath).size() <= 0)
                emit errorOccurred(QString("Ошибка ffmpeg при рендере (код %1): %2").arg(code)
                    .arg(QString::fromLocal8Bit(proc->readAllStandardError())));
            else if (verifyOutput) verifyAndPublish(temporaryPath, outPath, expectedDuration, segments);
            else emit clipRendered(outPath);
            proc->deleteLater();
        });
    proc->start(AppSettings::instance().ffmpegPath, args);
}

void ClipRenderer::verifyAndPublish(const QString& temporaryPath, const QString& outPath, double expectedDuration, int segments)
{
    auto* probe = new QProcess(this);
    auto* timeout = new QTimer(probe); timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, probe, [probe]() { probe->setProperty("timeout", true); probe->kill(); });
    connect(probe, &QProcess::errorOccurred, this, [this, probe](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || probe->property("reported").toBool()) return;
        probe->setProperty("reported", true);
        emit errorOccurred("Не удалось запустить ffprobe для проверки результата: " + probe->errorString()); probe->deleteLater();
    });
    connect(probe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, probe, timeout, temporaryPath, outPath, expectedDuration, segments](int code, QProcess::ExitStatus status) {
            timeout->stop();
            if (probe->property("reported").toBool()) return;
            probe->setProperty("reported", true);
            const auto data = QJsonDocument::fromJson(probe->readAllStandardOutput()).object();
            const auto durationValue = data["format"].toObject()["duration"];
            const double duration = durationValue.isDouble() ? durationValue.toDouble() : durationValue.toString().toDouble();
            bool video = false, audio = false; double fps = 24;
            for (const auto& value : data["streams"].toArray()) {
                const auto stream = value.toObject();
                if (stream["codec_type"] == "audio") audio = true;
                if (stream["codec_type"] == "video") {
                    video = true; const auto ratio = stream["avg_frame_rate"].toString().split('/');
                    if (ratio.size() == 2 && ratio[1].toDouble() > 0 && ratio[0].toDouble() > 0)
                        fps = ratio[0].toDouble() / ratio[1].toDouble();
                }
            }
            const double tolerance = .12 + qMax(1, segments) / qMax(1.0, fps);
            if (status != QProcess::NormalExit || code != 0 || !video || !audio || !std::isfinite(duration)
                || duration <= 0 || std::abs(duration - expectedDuration) > tolerance) {
                emit errorOccurred(QString("VERIFY_OUTPUT_FAILED: ожидалось %1 с, получено %2 с; файл остался .part.mp4: %3")
                    .arg(expectedDuration, 0, 'f', 3).arg(duration, 0, 'f', 3).arg(temporaryPath));
            } else if (!QFile::rename(temporaryPath, outPath)) {
                emit errorOccurred("Не удалось сохранить проверенное видео: " + outPath);
            } else emit clipRendered(outPath);
            probe->deleteLater();
        });
    probe->start(AppSettings::instance().ffprobePath, {"-v", "error", "-print_format", "json", "-show_format", "-show_streams", temporaryPath});
    timeout->start(30000);
}

void ClipRenderer::renderClip(const QString& inputPath, double startSec, double endSec,
                              int audioIndex, const QString& srtPath, const QString& outPath, bool verifyOutput)
{
    double dur = endSec - startSec;
    if (dur <= 0.0)
        dur = 1.0;

    // Важно: из-за двоеточий/кириллицы/пробелов в путях Windows фильтр subtitles= ломается.
    // Поэтому запускаем ffmpeg с рабочей папкой = папка с сабами и используем только имя файла.
    QFileInfo srtInfo(srtPath);
    QString srtFileName = srtInfo.fileName();
    QString workDir = srtInfo.absolutePath();
    const QString subtitleFilter = srtInfo.suffix().compare("ass", Qt::CaseInsensitive) == 0
        ? "subtitles=" + srtFileName
        : QString("subtitles=%1:force_style='%2'").arg(srtFileName, AppSettings::instance().subtitleStyle);

    QString filter = QString(
        "[0:v:0]split[a][b];"
        "[a]scale=1080:1920:force_original_aspect_ratio=increase,crop=1080:1920,gblur=sigma=20[bg];"
        "[b]scale=1080:1920:force_original_aspect_ratio=decrease[fg];"
        "[bg][fg]overlay=(W-w)/2:(H-h)/2,"
        "%1[v]").arg(subtitleFilter);

    QStringList args;
    args << "-y"
         << "-ss" << QString::number(startSec, 'f', 3)
         << "-t" << QString::number(dur, 'f', 3)
         << "-i" << inputPath
         << "-filter_complex" << filter
         << "-map" << "[v]"
         << "-map" << QString("0:a:%1").arg(audioIndex)
         << "-c:v" << "libx264"
         << "-preset" << "veryfast"
         << "-crf" << "20"
         << "-pix_fmt" << "yuv420p"
         // исходники обычно 5.1 — даунмикс в стерео, иначе 6 каналов в AAC звучат ужасно
         << "-ac" << "2"
         << "-c:a" << "aac"
         << "-b:a" << "192k"
         << "-movflags" << "+faststart"
         << outPath;

    startFfmpeg(args, workDir, outPath, dur, 1, verifyOutput);
}

void ClipRenderer::renderStoryClip(const QString& inputPath, const QList<QPair<double, double>>& segments,
                                   int audioIndex, const QString& srtPath, const QString& outPath,
                                   double crossfadeSec, bool verifyOutput)
{
    if (segments.isEmpty()) {
        emit errorOccurred("renderStoryClip: пустой список сегментов");
        return;
    }

    QFileInfo srtInfo(srtPath);
    QString srtFileName = srtInfo.fileName();
    QString workDir = srtInfo.absolutePath();
    const QString subtitleFilter = srtInfo.suffix().compare("ass", Qt::CaseInsensitive) == 0
        ? "subtitles=" + srtFileName
        : QString("subtitles=%1:force_style='%2'").arg(srtFileName, AppSettings::instance().subtitleStyle);

    QStringList args;
    args << "-y";
    // каждый сегмент — отдельный вход с input-seek (кадр-точно, т.к. перекодируем)
    double minDur = 1e9;
    for (const auto& seg : segments) {
        double dur = seg.second - seg.first;
        if (dur <= 0.0)
            dur = 1.0;
        minDur = qMin(minDur, dur);
        args << "-ss" << QString::number(seg.first, 'f', 3)
             << "-t" << QString::number(dur, 'f', 3)
             << "-i" << inputPath;
    }

    // xfade требует, чтобы каждый сегмент был длиннее фейда (с запасом)
    bool useXfade = crossfadeSec > 0.0 && segments.size() > 1 && minDur > 2.0 * crossfadeSec;
    double fadeSec = AppSettings::instance().editAudioFadeMs / 1000.0;
    bool useFade = fadeSec > 0.0 && !useXfade && segments.size() > 1;

    QString filter;
    for (int i = 0; i < segments.size(); ++i) {
        double dur = segments[i].second - segments[i].first;
        filter += QString("[%1:v:0]setpts=PTS-STARTPTS[vp%1];").arg(i);
        filter += QString("[%1:a:%2]asetpts=PTS-STARTPTS").arg(i).arg(audioIndex);
        // 30мс фейды на границах — убирают щелчки на склейках (hard rule video-use)
        if (useFade && dur > 2.0 * fadeSec)
            filter += QString(",afade=t=in:st=0:d=%1,afade=t=out:st=%2:d=%1")
                          .arg(fadeSec, 0, 'f', 3).arg(dur - fadeSec, 0, 'f', 3);
        filter += QString("[ap%1];").arg(i);
    }

    if (useXfade) {
        // видео: цепочка xfade; offset k-го перехода = сумма длин предыдущих сегментов - k*F
        double cum = segments[0].second - segments[0].first;
        QString prevV = "vp0";
        for (int k = 1; k < segments.size(); ++k) {
            double offset = cum - crossfadeSec;
            QString out = (k == segments.size() - 1) ? QString("cv") : QString("xv%1").arg(k);
            filter += QString("[%1][vp%2]xfade=transition=fade:duration=%3:offset=%4[%5];")
                          .arg(prevV).arg(k).arg(crossfadeSec, 0, 'f', 3)
                          .arg(offset, 0, 'f', 3).arg(out);
            prevV = out;
            cum += (segments[k].second - segments[k].first) - crossfadeSec;
        }
        // аудио: цепочка acrossfade (длительность совпадает с видео)
        QString prevA = "ap0";
        for (int k = 1; k < segments.size(); ++k) {
            QString out = (k == segments.size() - 1) ? QString("ca") : QString("xa%1").arg(k);
            filter += QString("[%1][ap%2]acrossfade=d=%3[%4];")
                          .arg(prevA).arg(k).arg(crossfadeSec, 0, 'f', 3).arg(out);
            prevA = out;
        }
    } else {
        for (int i = 0; i < segments.size(); ++i)
            filter += QString("[vp%1][ap%1]").arg(i);
        filter += QString("concat=n=%1:v=1:a=1[cv][ca];").arg(segments.size());
    }

    // вертикаль 9:16 + прожиг сабов
    filter += QString(
        "[cv]split[x][y];"
        "[x]scale=1080:1920:force_original_aspect_ratio=increase,crop=1080:1920,gblur=sigma=20[bg];"
        "[y]scale=1080:1920:force_original_aspect_ratio=decrease[fg];"
        "[bg][fg]overlay=(W-w)/2:(H-h)/2,"
        "%1[v]").arg(subtitleFilter);

    args << "-filter_complex" << filter
         << "-map" << "[v]"
         << "-map" << "[ca]"
         << "-c:v" << "libx264"
         << "-preset" << "veryfast"
         << "-crf" << "20"
         << "-pix_fmt" << "yuv420p"
         << "-ac" << "2"
         << "-c:a" << "aac"
         << "-b:a" << "192k"
         << "-movflags" << "+faststart"
         << outPath;

    double expectedDuration = 0;
    for (const auto& segment : segments) expectedDuration += segment.second - segment.first;
    if (useXfade) expectedDuration -= (segments.size() - 1) * crossfadeSec;
    startFfmpeg(args, workDir, outPath, expectedDuration, segments.size(), verifyOutput);
}
