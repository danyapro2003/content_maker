#ifndef METADATAPARSER_H
#define METADATAPARSER_H

#include <QObject>
#include <QJsonDocument>
#include <QList>

struct Chapter {
    QString title;
    double startSec = 0.0;
    double endSec = 0.0;
};

struct AudioTrack {
    int streamIndex = -1; // глобальный индекс стрима в контейнере
    int audioIndex = -1;  // индекс среди аудиодорожек (для -map 0:a:N)
    QString language;     // нормализованный код: "ru", "en", ...
    QString rawLanguage;  // как записан в файле
    QString title;
    QString codec;
    int channels = 0;
};

struct SubtitleTrack {
    int streamIndex = -1;
    int subIndex = -1;    // индекс среди субтитров (для -map 0:s:N)
    QString language;
    QString rawLanguage;
    QString title;
    QString codec;
    bool forced = false;
    bool sdh = false;     // hearing impaired
    bool bitmap = false;  // картиночные сабы (PGS/DVD) — в SRT не конвертируются
};

struct MediaInfo {
    QString filePath;
    QString title;
    double durationSec = 0.0;
    QList<Chapter> chapters;
    QList<AudioTrack> audioTracks;
    QList<SubtitleTrack> subtitleTracks;

    int rusAudioIndex = -1; // audioIndex (-1 — нет)
    int engAudioIndex = -1;
    int rusSubsIndex = -1;  // subIndex лучшего кандидата (-1 — нет)
    int engSubsIndex = -1;
    // все текстовые кандидаты на сабы по языку, лучший первый
    // (нужны для перебора, если лучший оказался forced по содержимому)
    QList<int> rusSubsCandidates;
    QList<int> engSubsCandidates;
};

class MetadataParser : public QObject
{
    Q_OBJECT

public:
    MetadataParser();

    void parse(const QByteArray& bytes, const QString& filePath);

    // Нормализация кода языка: rus/russian -> "ru", eng/english -> "en" и т.п.
    static QString normalizeLanguage(const QString& lang, const QString& title = QString());

signals:
    void parseCompleted(const QJsonDocument& json, const MediaInfo& info);
    void errorOccurred(const QString& errorMessage);
};

#endif // METADATAPARSER_H
