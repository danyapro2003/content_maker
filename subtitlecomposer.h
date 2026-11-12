#ifndef SUBTITLECOMPOSER_H
#define SUBTITLECOMPOSER_H

#include "srtutils.h"

namespace SubtitleComposer {
QList<SrtCue> readablePhrases(const QList<SrtCue>& words, int maxWords = 4,
                             const QList<double>& cuts = {});
bool writeAss(const QList<SrtCue>& words, const QString& path, int fontSize = 58, int maxWords = 4,
              const QList<double>& cuts = {});
}

#endif
