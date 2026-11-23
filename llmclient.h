#ifndef LLMCLIENT_H
#define LLMCLIENT_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include "llmtransport.h"
#include "metadataparser.h"
#include "stories3runner.h"
#include "storytypes.h"
#include "storyrunstore.h"
#include "storycontext.h"
#include "srtutils.h"

struct Highlight {
    QString title;
    double startSec = 0.0;
    double endSec = 0.0;
    QString description;
    QString viralityReason;
};

class LlmClient : public QObject
{
    Q_OBJECT

public:
    LlmClient();
    void setRunLogDirectory(const QString& directory);
    void writeRunArtifact(const QString& name, const QJsonObject& value) const;
    void setAnalysisDirectory(const QString& directory) { m_analysisDirectory = directory; }
    bool importEventMapV3(const QString& logDirectory, QString& error);
    void setImportLogDirectory(const QString& directory) { m_importLogDirectory = directory; }
    void setV5Discovery(bool enabled) { m_v5Discovery = enabled; }
    void checkModelFormat(const QString& logDirectory);
    void createSceneCaptionsV4(const QList<SrtCue>& words, const QList<double>& cuts, const QString& language,
        const std::function<void(bool, const QList<SrtCue>&, const QString&)>& callback, const StoryClip& story = {},
        const QString& translationLanguage = {});
    void setOfflinePreview(bool enabled) { m_offlinePreview = enabled; }

    // subsRu / subsEn — текст субтитров (может быть пустым, если отключено в настройках)
    void findHighlights(const MediaInfo& info, const QString& subsRu, const QString& subsEn);

    // Пайплайн «Пересказ 2.0»: агентный цикл.
    // LLM читает упакованный транскрипт, сама запрашивает кадры интересных ей фрагментов
    // (сигнал storyFramesRequested), смотрит их и выдаёт финальный EDL коротких моментов.
    void findStories(const MediaInfo& info, const QString& packedTranscript);

    // V3: события -> замыслы -> монтаж -> зритель -> верность -> исправление.
    void findStoriesV3(const MediaInfo& info, const QString& packedTranscript,
                       const QList<S3Phrase>& phrases);
    void deliverStoryFramesV3(const QList<QPair<double, QString>>& frames);

    // Проверка цитаты по упакованному транскрипту: ищем слова цитаты в окне
    // вокруг интервала. Возвращает границы доказательства, не меняет монтаж.
    struct QuoteCheck {
        bool found = false;
        double startSec = 0.0;
        double endSec = 0.0;
        double overlap = 0.0; // доля слов цитаты, найденных в окне
    };
    static QuoteCheck checkQuote(const QString& quote, double startSec, double endSec,
                                 const QList<S3Phrase>& phrases, double windowSec = 4.0);

    // Валидация EDL в коде (не в промпте): длительность, число склеек, разброс.
    QList<StoryClip> validateStoriesV3(const QList<StoryClip>& in,
                                       const QList<S3Phrase>& phrases,
                                       QStringList& notes) const;
    static QJsonArray serializeStories(const QList<StoryClip>& stories);

    // Ответ на storyFramesRequested: кадры (сек -> путь к jpg).
    // Пустой список — извлечь не удалось, агент продолжит без визуала.
    void deliverStoryFrames(const QList<QPair<double, QString>>& frames);

    // Разбор агентного ответа: запрос кадров {"action":"get_frames","ranges":[...]}.
    // Диапазоны клампятся к durationSec и ограничиваются 300 с / 8 шт.
    static bool parseFrameRequest(const QString& content, double durationSec,
                                  QList<QPair<double, double>>& ranges);

signals:
    void highlightsFound(const QList<Highlight>& highlights, const QJsonArray& rawJson);
    void storiesFound(const QList<StoryClip>& stories, const QJsonArray& rawJson);
    // LLM хочет посмотреть фрагменты: список (начало, конец) в секундах
    void storyFramesRequested(const QList<QPair<double, double>>& ranges);
    void logMessage(const QString& msg);
    void errorOccurred(const QString& errorMessage);
    void storySelectionFinished(int considered, int accepted, int rejected);
    void modelFormatChecked(bool ok, const QString& message);

private:
    void runAnalyst();
    void runScout();
    void runNextScoutChunk();
    void runProducer(const QString& extraUserMsg = QString());
    void runStoryTurn();

    void sendChat(const QJsonArray& messages,
                  const std::function<void(bool ok, const QString& content)>& callback,
                  const QString& stage = QStringLiteral("chat"));

    QList<Highlight> parseHighlights(const QString& text, QJsonArray& rawOut);
    QList<StoryClip> parseStories(const QString& text, QJsonArray& rawOut,
                                bool phraseReferences = false);

    // Пересказ 3.0
    void runStoryTurnV3();
    QList<S3Phrase> m_s3Phrases;   // для проверки цитат
    int m_story3Turn = 0;
    int m_story3Sent = 0;
    bool m_story3Retried = false;
    QJsonArray m_story3Messages;
    enum class Story3Phase { Index, Plan, Edit, Viewer, Fidelity, Repair, Replan };
    Story3Phase m_story3Phase = Story3Phase::Plan;
    QJsonObject m_story3Plan;
    QJsonArray m_story3Plans;
    QList<StoryClip> m_story3Results;
    QStringList m_story3Failures;
    int m_story3BranchIndex = -1;
    void startNextStoryBranchV3();
    QString storyFramesInstructionsV3() const;
    bool acceptStoryPlanV3(const QString& content, QString& error);
    void reviewStoryDraftV3(const QList<StoryClip>& draft);
    void discoverStoriesV5();
    void startStoryPlanV3(const QJsonArray& candidates = {});
    bool m_v5Discovery = false;
    void nextEventChunkV3();
    void handleEventResponseV3(const QJsonObject& response);
    void auditStoryV3(bool fidelity);
    void repairStoryV3(const QJsonObject& report);
    void beginStoryBranchV3();
    void replanStoryV3(const QString& reason, const QJsonObject& report);
    int m_story3Shortenings = 0;
    int m_story3Replans = 0;
    void rejectStoryV3(const QString& reason);
    bool checkEventCoverageV3(const StoryClip& clip, QStringList& notes) const;
    QJsonArray m_story3Events;
    QJsonArray m_story3IndexPending;
    int m_story3IndexRepairs = 0;
    QList<QList<S3Phrase>> m_story3Chunks;
    int m_story3Chunk = -1;
    int m_story3Repairs = 0;
    QList<StoryClip> m_story3Draft;
    StoryRunStore m_story3Store;
    QString m_analysisDirectory, m_story3ModelKey, m_story3IndexKey, m_story3PlanKey;
    QString m_story3RequestKey;
    bool m_story3Active = false;
    bool m_offlinePreview = false;
    QString m_importLogDirectory;
    QList<StoryContext::SourceUnit> m_story3Units;
    StoryContext::BranchContext m_story3Context;
    QStringList m_story3ExtraUnits;
    int m_story3ContextRequests = 0;
    QString storyEditorMaterial() const;
    int m_story3FormatRepairs = 0;
    QSet<QString> m_story3SeenTimelines;
    QJsonObject m_story3LastReport;
    QString m_story3BaseDraftHash;
    void saveStoryStateV3(const QString& status, const QJsonObject& details = {});
    void finishSelectionV3(const QString& stopReason = {});
    void checkModelTurn(int index);
    QJsonArray m_modelCheckReports;
    QString m_modelCheckKey;
    QString stageCacheKey(const QJsonArray& messages) const;

    // Общая подготовка текста сабов по настройке; false — текста нет
    bool buildSubsText(const QString& subsRu, const QString& subsEn);

    static double parseTimecode(QString s);
    static QString secToTimecode(double sec);
    static QStringList chunkSrt(const QString& text, int maxChars);

    LlmTransport m_transport;
    QString m_runLogDirectory;
    QString m_lastRequestPrefix;
    int m_requestNumber = 0;

    MediaInfo m_info;
    QString m_subsText;      // объединённый текст сабов для LLM (по настройке)
    QString m_packedText;    // упакованный транскрипт для пересказа
    QString m_synopsis;
    QString m_candidates;    // накопленные ответы Скаута
    QStringList m_scoutChunks;
    int m_scoutChunkIndex = 0;
    bool m_producerRetried = false;
    QString m_producerUserMsg;

    // агентный диалог пересказа
    QJsonArray m_storyMessages;  // полная история (system/user/assistant)
    int m_storyTurn = 0;         // сколько раз LLM уже запрашивала кадры
    int m_storySent = 0;         // всего запросов к LLM (защита от вечного цикла)
    bool m_storyRetried = false; // ретрай невалидного финального JSON
    bool m_awaitingFrames = false;
};

#endif // LLMCLIENT_H
