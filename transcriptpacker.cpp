/* transcriptpacker.cpp */

#include "transcriptpacker.h"
#include <QHash>

// Доминирующий спикер для интервала слова: максимум перекрытия с сегментами диаризации.
// cues и diar отсортированы по времени — двигаем указатель, амортизированно O(n+m).
static QString dominantSpeaker(double start, double end, const QList<DiarSegment>& diar,
                               int& idx)
{
    QString best;
    double bestOverlap = 0.0;
    int i = idx;
    while (i < diar.size() && diar[i].endSec <= start)
        ++i;
    idx = i;
    for (int j = i; j < diar.size() && diar[j].startSec < end; ++j) {
        double overlap = qMin(end, diar[j].endSec) - qMax(start, diar[j].startSec);
        if (overlap > bestOverlap) {
            bestOverlap = overlap;
            best = diar[j].speaker;
        }
    }
    return best;
}

// последний символ текста — конец предложения?
static bool endsSentence(const QString& text)
{
    if (text.isEmpty())
        return false;
    const QChar c = text.at(text.size() - 1);
    return c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?')
           || c == QLatin1Char(':') || c == QChar(0x2026) /* … */;
}

static QList<PackedPhrase> packImpl(const QList<SrtCue>& cues, double pauseSec,
                                    double maxPhraseSec, const QList<DiarSegment>* diar)
{
    QList<PackedPhrase> phrases;
    if (maxPhraseSec < 1.0)
        maxPhraseSec = 1.0;

    QHash<QString, QString> speakerMap; // SPEAKER_XX -> S0/S1... по порядку появления
    int diarIdx = 0;

    for (const SrtCue& cue : cues) {
        const QString word = cue.text.trimmed();
        if (word.isEmpty())
            continue;

        QString speaker;
        if (diar) {
            const QString raw = dominantSpeaker(cue.startSec, cue.endSec, *diar, diarIdx);
            if (!raw.isEmpty()) {
                if (!speakerMap.contains(raw))
                    speakerMap[raw] = "S" + QString::number(speakerMap.size());
                speaker = speakerMap[raw];
            }
        }

        bool newPhrase = phrases.isEmpty();
        if (!newPhrase) {
            const PackedPhrase& last = phrases.last();
            const double dur = last.endSec - last.startSec;
            if (cue.startSec - last.endSec >= pauseSec)
                newPhrase = true;                                        // пауза в речи
            else if (diar && !speaker.isEmpty() && speaker != last.speaker)
                newPhrase = true;                                        // сменился говорящий
            else if (dur >= maxPhraseSec)
                newPhrase = true;                                        // жёсткий лимит длины
            else if (dur >= 0.4 * maxPhraseSec && endsSentence(last.text))
                newPhrase = true;                                        // граница предложения
        }

        if (newPhrase) {
            PackedPhrase p;
            p.startSec = cue.startSec;
            p.endSec = cue.endSec;
            p.speaker = speaker;
            p.text = word;
            phrases.append(p);
        } else {
            PackedPhrase& p = phrases.last();
            p.endSec = cue.endSec;
            p.text += " " + word;
        }
    }
    return phrases;
}

QList<PackedPhrase> TranscriptPacker::pack(const QList<SrtCue>& cues, double pauseSec,
                                           double maxPhraseSec)
{
    return packImpl(cues, pauseSec, maxPhraseSec, nullptr);
}

QList<PackedPhrase> TranscriptPacker::pack(const QList<SrtCue>& cues, double pauseSec,
                                           const QList<DiarSegment>& diar,
                                           double maxPhraseSec)
{
    return packImpl(cues, pauseSec, maxPhraseSec, diar.isEmpty() ? nullptr : &diar);
}

QString TranscriptPacker::toMarkdown(const QList<PackedPhrase>& phrases)
{
    QString out;
    out.reserve(phrases.size() * 60);
    for (const PackedPhrase& p : phrases) {
        out += QString("[%1-%2] ")
                   .arg(p.startSec, 0, 'f', 2)
                   .arg(p.endSec, 0, 'f', 2);
        if (!p.speaker.isEmpty())
            out += p.speaker + " ";
        out += p.text + "\n";
    }
    return out;
}
