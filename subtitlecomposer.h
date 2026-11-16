#ifndef SUBTITLECOMPOSER_H
#define SUBTITLECOMPOSER_H

#include "srtutils.h"

namespace SubtitleComposer {
bool writeAssV4(const QList<SrtCue>& words, const QList<SrtCue>& captions, const QString& path,
                int fontSize = 64, const QList<double>& cuts = {});
QList<SrtCue> readablePhrases(const QList<SrtCue>& words, int maxWords = 4,
                             const QList<double>& cuts = {});
bool writeAss(const QList<SrtCue>& words, const QString& path, int fontSize = 58, int maxWords = 4,
              const QList<double>& cuts = {});
}

#endif
