#pragma once
#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>

struct StorySegment {
    QString blockId;
    double startSec = 0.0;
    double endSec = 0.0;
    QString beat;   // HOOK / SETUP / CONFLICT / CLIMAX / RESOLUTION / ...
    QString quote;  // ключевая фраза склейки (дословно)
    QString reason; // зачем склейка в истории
    int startPhrase = 0;
    int endPhrase = 0;
    QString event;
    QString connection; // причинная связь с предыдущим фрагментом
    QStringList eventIds;
};

// Клип-пересказ и его монтажные блоки; V3 сохраняет план сюжетной ветки.
struct StoryClip {
    int protocolVersion = 1;
    QString branchId;
    QString timelineHash;
    QString baseDraftHash;
    QString title;
    QString description;
    QString caption; // готовый текст поста
    QString emotion; // чем цепляет: смех / умиление / напряжение / восторг...
    double score = 0.0; // 0-100 по рубрике (0 = модель не прислала)
    QList<StorySegment> segments;
    QString storySummary;
    QJsonObject narrativePlan;
    QString openingMode = "natural";
    QString hookReason;

    double totalSec() const {
        double t = 0.0;
        for (const StorySegment& s : segments)
            t += s.endSec - s.startSec;
        return t;
    }
};

