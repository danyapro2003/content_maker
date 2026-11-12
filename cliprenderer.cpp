/* cliprenderer.cpp */

#include "cliprenderer.h"
#include "appsettings.h"
#include <QProcess>
#include <QFile>
#include <QFileInfo>

ClipRenderer::ClipRenderer() {}

void ClipRenderer::startFfmpeg(const QStringList& args, const QString& workDir, const QString& outPath)
{
    m_currentOutPath = outPath;

    QProcess* proc = new QProcess(this);
    if (!workDir.isEmpty())
        proc->setWorkingDirectory(workDir);

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [=](int exitCode, QProcess::ExitStatus exitStatus) {
                bool ok = (exitStatus == QProcess::NormalExit && exitCode == 0
                           && QFile::exists(m_currentOutPath));
                if (ok) {
                    emit clipRendered(m_currentOutPath);
                } else {
                    emit errorOccurred(QString("Ошибка ffmpeg при рендере клипа (код %1): %2")
                                           .arg(exitCode)
                                           .arg(QString::fromLocal8Bit(proc->readAllStandardError())));
                }
                proc->deleteLater();
            });

    proc->start(AppSettings::instance().ffmpegPath, args);
}

void ClipRenderer::renderClip(const QString& inputPath, double startSec, double endSec,
                              int audioIndex, const QString& srtPath, const QString& outPath)
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

    startFfmpeg(args, workDir, outPath);
}

void ClipRenderer::renderStoryClip(const QString& inputPath, const QList<QPair<double, double>>& segments,
                                   int audioIndex, const QString& srtPath, const QString& outPath,
                                   double crossfadeSec)
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

    startFfmpeg(args, workDir, outPath);
}
