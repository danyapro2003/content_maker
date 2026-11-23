#ifndef SRTUTILS_H
#define SRTUTILS_H

#include <QString>
#include <QList>
#include <QPair>

struct SrtCue {
    int index = 0;
    double startSec = 0.0;
    double endSec = 0.0;
    QString text;
    QString translation = {}; // Optional second-language text for a shared scene caption.
};

class SrtUtils
{
public:
    // Парсинг SRT-текста / файла в реплики
    static QList<SrtCue> parse(const QString& srtText);
    static QList<SrtCue> parseFile(const QString& path);

    // «Прилипание» сегмента к границам реплик:
    // начало -> к началу первой пересекающейся реплики (минус padStart),
    // конец  -> к концу последней реплики, начавшейся до конца сегмента (плюс padEnd).
    static QPair<double, double> snapSegment(double startSec, double endSec,
                                             const QList<SrtCue>& cues,
                                             double padStart, double padEnd,
                                             double maxDuration);

    // Слияние соседних сегментов с зазором < mergeGapSec
    static QList<QPair<double, double>> mergeClose(QList<QPair<double, double>> segs,
                                                   double mergeGapSec);
    // Доля уникальных текстов среди непустых cue — детектор зацикливания whisper.
    // Нормальный word-level транскрипт фильма: 0.1–0.5; галлюцинационная петля: < 0.01.
    // Для пустого списка возвращает 1.0 (не считаем вырожденным).
    static double uniqueTextRatio(const QList<SrtCue>& cues);

    // Word-level cue от whisper иногда растягивается на десятки секунд: DTW «размазывает»
    // слово по музыке/тишине (на 15 мин фильма нашлось 17 таких cue — 279с из 873с,
    // рекорд: слово «знаю» длиной 66с). Это ломает упаковку фраз и снаппинг границ —
    // в клип попадает минута музыки. Начало слова при этом достоверно, поэтому
    // конец = min(начало следующего cue, start + maxWordSec).
    // Только для word-level: у обычных субтитров длинные cue легальны.
    static int clampWordDurations(QList<SrtCue>& cues, double maxWordSec = 2.0);
};

#endif // SRTUTILS_H
