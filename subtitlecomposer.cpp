#include "subtitlecomposer.h"

#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

QList<SrtCue> SubtitleComposer::readablePhrases(const QList<SrtCue>& input, int maxWords,
                                              const QList<double>& cuts)
{
    QList<SrtCue> words = input;
    std::stable_sort(words.begin(), words.end(), [](const auto& a, const auto& b) { return a.startSec < b.startSec; });
    QList<SrtCue> result;
    SrtCue current;
    int count = 0;
    const auto flush = [&]() {
        if (current.text.isEmpty()) return;
        current.index = result.size() + 1;
        result.append(current);
        current = {};
        count = 0;
    };
    maxWords = qBound(1, maxWords, 6);
    for (const auto& word : words) {
        const QString text = word.text.simplified();
        if (text.isEmpty() || word.endSec <= word.startSec) continue;
        const int incoming = text.split(' ', Qt::SkipEmptyParts).size();
        const bool crossesCut = std::any_of(cuts.begin(), cuts.end(), [&](double cut) {
            return current.startSec < cut && word.startSec >= cut;
        });
        if (!current.text.isEmpty() && (count + incoming > maxWords
            || current.text.size() + text.size() + 1 > 34
            || word.startSec - current.endSec > 0.4
            || word.endSec - current.startSec > 2.6
            || crossesCut
            || QRegularExpression("[.!?…]$").match(current.text).hasMatch())) flush();
        if (current.text.isEmpty()) { current = word; current.text = text; }
        else { current.text += ' ' + text; current.endSec = qMax(current.endSec, word.endSec); }
        count += incoming;
    }
    flush();
    for (int i = 0; i < result.size(); ++i) {
        auto& phrase = result[i];
        double next = i + 1 < result.size() ? result[i + 1].startSec : phrase.endSec + 0.12;
        for (double cut : cuts) if (cut > phrase.startSec) next = qMin(next, cut);
        // Short words remain readable; do not flash or overlap the following phrase.
        phrase.endSec = qMin(next, qMax(phrase.endSec, qMin(phrase.startSec + 0.7, phrase.endSec + 0.25)));
    }
    return result;
}

static QString assTime(double seconds)
{
    const qint64 cs = qMax<qint64>(0, qRound64(seconds * 100));
    return QString("%1:%2:%3.%4").arg(cs / 360000).arg((cs / 6000) % 60, 2, 10, QChar('0'))
        .arg((cs / 100) % 60, 2, 10, QChar('0')).arg(cs % 100, 2, 10, QChar('0'));
}

bool SubtitleComposer::writeAss(const QList<SrtCue>& words, const QString& path, int fontSize, int maxWords,
                               const QList<double>& cuts)
{
    QString document = QStringLiteral(
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 1080\nPlayResY: 1920\n"
        "WrapStyle: 0\nScaledBorderAndShadow: yes\n\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Readable,Segoe UI,%1,&H00FFFFFF,&H00FFFFFF,&H00101010,&H90000000,"
        "-1,0,0,0,100,100,0,0,1,3,1,2,90,90,620,1\n\n[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n")
        .arg(qBound(32, fontSize, 90));
    for (const auto& phrase : readablePhrases(words, maxWords, cuts)) {
        QString text = phrase.text;
        text.replace('\\', QChar(0xFF3C));
        text.replace('{', QChar(0xFF5B)); text.replace('}', QChar(0xFF5D));
        text.replace('\n', ' '); text.replace('\r', ' ');
        document += QString("Dialogue: 0,%1,%2,Readable,,0,0,0,,%3\n")
                        .arg(assTime(phrase.startSec), assTime(phrase.endSec), text);
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return false;
    const auto bytes = document.toUtf8();
    return output.write(bytes) == bytes.size() && output.commit();
}
