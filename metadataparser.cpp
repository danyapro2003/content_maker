/* metadataparser.cpp */

#include "metadataparser.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QFileInfo>
#include <algorithm>

MetadataParser::MetadataParser() {}

QString MetadataParser::normalizeLanguage(const QString& lang, const QString& title)
{
    QString l = lang.trimmed().toLower();
    if (l == "ru" || l == "rus" || l == "russian") return "ru";
    if (l == "en" || l == "eng" || l == "english") return "en";
    if (l.isEmpty() && !title.isEmpty()) {
        // Слабая эвристика по названию дорожки, если тега языка нет
        QString t = title.toLower();
        if (t.contains("рус") || t.contains(" rus")) return "ru";
        if (t.contains("eng") || t.contains("english")) return "en";
    }
    return l;
}

// Все текстовые кандидаты на сабы для языка, лучший первый:
// предпочитаем не-forced, не-SDH, с пометкой "full" в названии (если есть).
static QList<int> pickSubCandidates(const QList<SubtitleTrack>& subs, const QString& lang)
{
    QList<QPair<int, int>> scored; // (score, subIndex)
    for (const SubtitleTrack& s : subs) {
        if (s.language != lang)
            continue;
        int score = 0;
        if (!s.forced) score += 2;
        if (!s.sdh)    score += 2;
        if (s.title.contains("full", Qt::CaseInsensitive)) score += 1;
        if (s.title.contains("forced", Qt::CaseInsensitive)
            || s.title.contains("форсир", Qt::CaseInsensitive)) score -= 5;
        if (s.bitmap)  score -= 100; // картиночные сабы не извлечь в SRT
        scored.append({score, s.subIndex});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    QList<int> result;
    for (const auto& p : scored) {
        if (p.first < 0)
            break; // bitmap и хуже — не кандидаты
        result.append(p.second);
    }
    return result;
}

void MetadataParser::parse(const QByteArray& bytes, const QString& filePath)
{
    QJsonParseError err;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !jsonDoc.isObject()) {
        emit errorOccurred("Ошибка парсинга JSON ffprobe: " + err.errorString());
        return;
    }

    QJsonObject root = jsonDoc.object();
    MediaInfo info;
    info.filePath = filePath;

    // --- format ---
    QJsonObject format = root.value("format").toObject();
    info.durationSec = format.value("duration").toString().toDouble();
    QJsonObject formatTags = format.value("tags").toObject();
    info.title = formatTags.value("title").toString();
    if (info.title.isEmpty())
        info.title = QFileInfo(filePath).completeBaseName();

    // --- streams ---
    QJsonArray streams = root.value("streams").toArray();
    int audioCounter = 0;
    int subCounter = 0;
    for (const QJsonValue& v : streams) {
        QJsonObject s = v.toObject();
        QString codecType = s.value("codec_type").toString();
        QJsonObject tags = s.value("tags").toObject();
        QString title = tags.value("title").toString();
        QString rawLang = tags.value("language").toString();
        QString lang = normalizeLanguage(rawLang, title);

        if (codecType == "audio") {
            AudioTrack t;
            t.streamIndex = s.value("index").toInt();
            t.audioIndex = audioCounter++;
            t.language = lang;
            t.rawLanguage = rawLang;
            t.title = title;
            t.codec = s.value("codec_name").toString();
            t.channels = s.value("channels").toInt();
            info.audioTracks.append(t);
        } else if (codecType == "subtitle") {
            SubtitleTrack t;
            t.streamIndex = s.value("index").toInt();
            t.subIndex = subCounter++;
            t.language = lang;
            t.rawLanguage = rawLang;
            t.title = title;
            t.codec = s.value("codec_name").toString();
            QJsonObject disp = s.value("disposition").toObject();
            t.forced = disp.value("forced").toInt() == 1;
            t.sdh = disp.value("hearing_impaired").toInt() == 1
                    || title.contains("sdh", Qt::CaseInsensitive);
            t.bitmap = (t.codec == "hdmv_pgs_subtitle" || t.codec == "dvd_subtitle"
                        || t.codec == "xsub" || t.codec == "dvb_subtitle");
            info.subtitleTracks.append(t);
        }
    }

    // --- chapters ---
    QJsonArray chapters = root.value("chapters").toArray();
    for (const QJsonValue& v : chapters) {
        QJsonObject c = v.toObject();
        Chapter ch;
        ch.startSec = c.value("start_time").toString().toDouble();
        ch.endSec = c.value("end_time").toString().toDouble();
        ch.title = c.value("tags").toObject().value("title").toString();
        info.chapters.append(ch);
    }

    // --- выбор нужных дорожек ---
    for (const AudioTrack& t : info.audioTracks) {
        if (t.language == "ru" && info.rusAudioIndex < 0)
            info.rusAudioIndex = t.audioIndex;
        if (t.language == "en" && info.engAudioIndex < 0)
            info.engAudioIndex = t.audioIndex;
    }
    info.rusSubsCandidates = pickSubCandidates(info.subtitleTracks, "ru");
    info.engSubsCandidates = pickSubCandidates(info.subtitleTracks, "en");
    info.rusSubsIndex = info.rusSubsCandidates.isEmpty() ? -1 : info.rusSubsCandidates.first();
    info.engSubsIndex = info.engSubsCandidates.isEmpty() ? -1 : info.engSubsCandidates.first();

    emit parseCompleted(jsonDoc, info);
}
