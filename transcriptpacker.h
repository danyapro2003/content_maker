#ifndef TRANSCRIPTPACKER_H
#define TRANSCRIPTPACKER_H

#include <QString>
#include <QList>
#include "srtutils.h"

struct PackedPhrase {
    double startSec = 0.0;
    double endSec = 0.0;
    QString speaker; // "S0" или пусто (диаризация опциональна)
    QString text;
};

// Сегмент диаризации (от pyannote): кто звучал в интервале
struct DiarSegment {
    double startSec = 0.0;
    double endSec = 0.0;
    QString speaker; // "SPEAKER_00", ...
};

// Упаковка транскрипта в фразы уровня video-use:
// вход — word-level реплики (whisper -ml 1 -sow) или обычные SRT-реплики.
// Новая фраза начинается, если:
//   1) пауза >= pauseSec, или
//   2) сменился спикер (при наличии диаризации), или
//   3) фраза достигла maxPhraseSec — в плотных диалогах пауз почти нет и
//      без этого ограничения получались «фразы» по 50+ секунд,
//   4) фраза закончена (точка/!/?/…) и уже набрала 40% maxPhraseSec —
//      предпочитаем рвать на границах предложений.
// Выходной markdown — компактная «поверхность чтения» для LLM:
//   [312.52-315.36] Мы не можем просто сдаться.
class TranscriptPacker
{
public:
    static QList<PackedPhrase> pack(const QList<SrtCue>& cues, double pauseSec,
                                    double maxPhraseSec = 12.0);
    // с диаризацией: каждому слову назначается доминирующий по перекрытию спикер,
    // смена спикера — тоже разрыв фразы; метки S0/S1 по порядку появления
    static QList<PackedPhrase> pack(const QList<SrtCue>& cues, double pauseSec,
                                    const QList<DiarSegment>& diar,
                                    double maxPhraseSec = 12.0);
    static QString toMarkdown(const QList<PackedPhrase>& phrases);
};

#endif // TRANSCRIPTPACKER_H
