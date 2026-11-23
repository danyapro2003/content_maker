/* srtutils.cpp */

#include "srtutils.h"
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

static double parseTc(QString s)
{
    s = s.trimmed();
    s.replace(',', '.');
    QStringList parts = s.split(':');
    if (parts.size() == 3)
        return parts[0].toDouble() * 3600.0 + parts[1].toDouble() * 60.0 + parts[2].toDouble();
    if (parts.size() == 2)
        return parts[0].toDouble() * 60.0 + parts[1].toDouble();
    return s.toDouble();
}

// Очистка текста реплики от разметки: HTML-теги (<font>, <i>), ASS-теги ({\an8}),
// неразрывные пробелы и служебные символы — LLM должен видеть чистую речь.
static QString cleanCueText(QString s)
{
    static const QRegularExpression htmlTag("<[^>]*>");
    static const QRegularExpression assTag("\\{[^}]*\\}");

    s.remove(htmlTag);
    s.remove(assTag);
    s.replace("\\N", " ");
    s.replace("\\n", " ");
    s.replace(QChar(0x00A0), QLatin1Char(' '));   // неразрывный пробел
    s.remove(QChar(0xFEFF));                       // BOM
    s.remove(QChar(0x200E));                       // LRM-марк
    s.remove(QChar(0x200F));                       // RLM-марк
    return s.simplified();
}

QList<SrtCue> SrtUtils::parse(const QString& srtText)
{
    QList<SrtCue> cues;

    static const QRegularExpression blockSplit("\\r?\\n\\r?\\n");
    static const QRegularExpression tcRe(
        "(\\d{1,2}:\\d{2}:\\d{2}[,.]\\d{1,3})\\s*-->\\s*(\\d{1,2}:\\d{2}:\\d{2}[,.]\\d{1,3})");

    QStringList blocks = srtText.split(blockSplit, Qt::SkipEmptyParts);
    for (const QString& block : blocks) {
        QRegularExpressionMatch m = tcRe.match(block);
        if (!m.hasMatch())
            continue;

        SrtCue cue;
        cue.startSec = parseTc(m.captured(1));
        cue.endSec = parseTc(m.captured(2));

        // текст реплики — строки после строки с таймкодом
        QStringList lines = block.split(QRegularExpression("\\r?\\n"));
        for (int i = 0; i < lines.size(); ++i) {
            if (lines[i].contains("-->")) {
                cue.text = cleanCueText(lines.mid(i + 1).join(" "));
                break;
            }
        }
        cue.index = cues.size() + 1;
        if (cue.endSec > cue.startSec)
            cues.append(cue);
    }
    return cues;
}

QList<SrtCue> SrtUtils::parseFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return parse(QString::fromUtf8(f.readAll()));
}

QPair<double, double> SrtUtils::snapSegment(double startSec, double endSec,
                                            const QList<SrtCue>& cues,
                                            double padStart, double padEnd,
                                            double maxDuration)
{
    double newStart = startSec - padStart;
    double newEnd = endSec + padEnd;

    if (!cues.isEmpty()) {
        // первая реплика, которая ещё не закончилась к началу сегмента
        for (const SrtCue& c : cues) {
            if (c.endSec > startSec) {
                newStart = c.startSec - padStart;
                break;
            }
        }
        // последняя реплика, которая началась до конца сегмента
        for (int i = cues.size() - 1; i >= 0; --i) {
            if (cues[i].startSec < endSec) {
                newEnd = cues[i].endSec + padEnd;
                break;
            }
        }
    }

    if (newStart < 0.0)
        newStart = 0.0;
    if (maxDuration > 0.0 && newEnd > maxDuration)
        newEnd = maxDuration;
    if (newEnd <= newStart) { // страховка от вырожденного сегмента
        newStart = startSec;
        newEnd = endSec > startSec ? endSec : startSec + 1.0;
    }
    return {newStart, newEnd};
}

QList<QPair<double, double>> SrtUtils::mergeClose(QList<QPair<double, double>> segs,
                                                  double mergeGapSec)
{
    std::sort(segs.begin(), segs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    QList<QPair<double, double>> merged;
    for (const auto& s : segs) {
        if (!merged.isEmpty() && s.first - merged.last().second < mergeGapSec) {
            merged.last().second = qMax(merged.last().second, s.second);
        } else {
            merged.append(s);
        }
    }
    return merged;
}

double SrtUtils::uniqueTextRatio(const QList<SrtCue>& cues)
{
    QSet<QString> unique;
    int total = 0;
    for (const SrtCue& c : cues) {
        QString t = c.text.trimmed();
        if (t.isEmpty())
            continue;
        ++total;
        unique.insert(t);
    }
    if (total == 0)
        return 1.0;
    return double(unique.size()) / double(total);
}

int SrtUtils::clampWordDurations(QList<SrtCue>& cues, double maxWordSec)
{
    if (maxWordSec <= 0.0)
        maxWordSec = 2.0;

    int fixed = 0;
    for (int i = 0; i < cues.size(); ++i) {
        if (cues[i].endSec - cues[i].startSec <= maxWordSec)
            continue;
        double limit = cues[i].startSec + maxWordSec;
        // следующее слово не может начаться раньше, чем закончилось это
        if (i + 1 < cues.size() && cues[i + 1].startSec < limit)
            limit = cues[i + 1].startSec;
        cues[i].endSec = qMax(cues[i].startSec + 0.05, limit);
        ++fixed;
    }
    return fixed;
}
