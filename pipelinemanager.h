#ifndef PIPELINEMANAGER_H
#define PIPELINEMANAGER_H

#include <QObject>
#include <QQueue>
#include <QMap>
#include <QSet>
#include <QJsonArray>
#include "mediaextractor.h"
#include "metadataparser.h"
#include "llmclient.h"
#include "whisperrunner.h"
#include "cliprenderer.h"
#include "srtutils.h"
#include "stories3pipeline.h"

struct FileRunResult {
    QString source, status, error, logDirectory, outputDirectory;
    int considered = 0, accepted = 0, rejected = 0, renderedStories = 0, expectedVideos = 0;
    QStringList outputs;
    QJsonObject json() const;
};
struct QueueRunSummary {
    int totalFiles = 0;
    QList<FileRunResult> files;
    QString text() const;
    QStringList outputs() const;
};

class PipelineManager : public QObject
{
    Q_OBJECT

public:
    explicit PipelineManager(QObject *parent = nullptr);

    enum class Mode { Highlights = 0, Stories5 = 4 };

    void addFilesToQueue(const QStringList& files);
    void setV5Background(bool enabled) { m_v5Background = enabled; }
    void setMode(Mode mode) { m_mode = mode; llm.setV5Discovery(mode == Mode::Stories5); }
    void setImportLogDirectory(const QString& directory) { llm.setImportLogDirectory(directory); }
    void setOfflinePreview(bool enabled) { llm.setOfflinePreview(enabled); }

signals:
    void logMessage(const QString& msg);
    void clipsFound(const QList<Highlight>& clips);
    void storiesFound(const QList<StoryClip>& stories);
    void queueFinished();
    void queueSummaryReady(const QueueRunSummary& summary);

private slots:
    void onffprobeFinished(const QByteArray& data);
    void onParseCompleted(const QJsonDocument& json, const MediaInfo& info);
    void onSubtitleExtracted(const QString& outPath);
    void onAudioExtracted(const QString& outPath);
    void onFramesExtracted(const QString& dirPath);
    void onStoryFramesRequested(const QList<QPair<double, double>>& ranges);
    void onHighlightsFound(const QList<Highlight>& highlights, const QJsonArray& rawJson);
    void onStoriesFound(const QList<StoryClip>& stories, const QJsonArray& rawJson);
    void onTranscriptionDone(const QString& srtPath);
    void onClipRendered(const QString& outPath);
    void onErrorOccurred(const QString& errorMessage);

private:
    enum class AudioPurpose { None, FallbackTranscribe, ClipSegment, Stories3Audio };

    struct RenderTask {
        int clipIndex = 0;
        QString lang;           // "ru" / "en"
        bool isStory = false;   // пересказ (склейка сегментов) или цельный клип
        double startSec = 0.0;  // для цельного клипа
        double endSec = 0.0;
        QList<QPair<double, double>> segments; // для пересказа
        int audioIndex = -1;    // индекс среди аудиодорожек (0:a:N)
        QString wavPath;
        QString srtBase;
        QString outPath;
    };

    void processNext();          // следующий файл из очереди
    void startPipeline(const QString& path);

    void startSubtitleStage();   // разбор: какие сабы извлекать / нужен ли whisper-fallback
    void extractNextSubtitle();
    bool validateSubsDensity(const QString& srtPath); // отбраковка forced по плотности реплик
    void afterSubsStage();
    void proceedToLlm();       // -> startLlmStage
    void startLlmStage();

    // Пересказ 2.0: транскрипт whisper word-level по всему фильму (с кэшем)

    // Пересказ 3.0
    void startStories3();
    void onStories3Failed(const QString& error);

    void buildRenderTasks();
    void buildStoryRenderTasks();
    void startRenderTasks();
    void processNextRenderTask();
    void renderTaskWithSrt(const QString& srtPath);
    void renderTaskWithSubtitles(const QString& subtitlePath);
    bool isNarrativeMode() const { return m_mode == Mode::Stories5; }

    // Пересказ 3.0: субтитры клипа из чистого ASR-транскрипта (без whisper)
    QString buildS3ClipSrt(const RenderTask& task, const QString& srtBase);
    // Проверенные слова из s3_clean_words.json → m_s3Words (для сабов клипа).
    void loadS3Words();

    void finishCurrentFile();

    Mode m_mode = Mode::Highlights;

    QQueue<QString> m_fileQueue;
    bool m_isProcessing = false;
    bool m_fileActive = false;
    QueueRunSummary m_queueSummary;
    FileRunResult m_fileResult;
    QSet<int> m_renderedStoryIds;

    MediaExtractor extractor;
    MetadataParser dataParser;
    LlmClient llm;
    WhisperRunner whisper;
    ClipRenderer renderer;
    Stories3Pipeline m_s3;

    // контекст текущего файла
    QString currentFilePath;
    QString baseName;
    QString outDir;     // <папка_видео>/<basename>_clips
    QString tempDir;    // outDir/temp
    QString m_runLogDirectory;
    QString m_runOutputDirectory, m_renderTempDirectory;
    QString m_runError;
    QStringList m_runOutputs;
    MediaInfo currentInfo;

    // этап субтитров
    QList<QPair<QString, int>> m_pendingSubExtracts; // (lang, subIndex)
    QMap<QString, QString> m_subsPaths;              // lang -> путь к srt
    QMap<QString, QList<int>> m_subsCandidates;      // lang -> оставшиеся кандидаты (subIndex)
    QString m_currentSubLang;
    QStringList m_missingSubsLangs;                  // языки без сабов -> whisper fallback

    // аудио/whisper
    AudioPurpose m_audioPurpose = AudioPurpose::None;
    QString m_fallbackAudioLang;

    // этап рендера
    QList<Highlight> m_highlights;
    QList<StoryClip> m_stories;
    QList<RenderTask> m_renderTasks;
    QMap<QPair<int, QString>, QList<SrtCue>> m_translatedSceneCaptions;
    int m_taskIndex = -1;

    QList<SrtCue> m_cues; // реплики/слова — для снаппинга границ сегментов
    QList<SrtCue> m_s3Words; // Пересказ 3.0: word-level слова из s3_words.json
    bool m_v5Background = false;
    void recordRenderedTask(const QString& outPath);
    QString m_storyTranscriptLang;
    QString m_s3AudioKey;

    // агентный доступ к кадрам (пересказ 2.0)
    QList<QPair<double, double>> m_agentRanges; // диапазоны текущего запроса LLM
    double m_agentInterval = 2.0;               // фактический интервал кадров
    bool m_agentFramesPending = false;          // ждём завершения ffmpeg
};

#endif // PIPELINEMANAGER_H
