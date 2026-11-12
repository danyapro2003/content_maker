#ifndef STORIES3PIPELINE_H
#define STORIES3PIPELINE_H

#include <QObject>
#include <QString>
#include <QList>
#include <QJsonObject>
#include "metadataparser.h"
#include "stories3runner.h"
#include "llmclient.h"

// Человекочитаемые имена движков для лога
QString s3AsrEngineName(int engine);
QString s3AudioMapEngineName(int engine);
QString s3DiarEngineName(int engine);

// Пересказ 3.0 — отдельный пайплайн, НЕ затрагивающий Пересказ 2.0.
//
// Отличия от v2:
//   1. Карта музыки (ina/zipformer) -> помечаем интервалы без речи.
//   2. ASR чанками по 20 с на выбранном движке (GigaAM/Parakeet/whisper.cpp/faster-whisper);
//      изоляция чанков не даёт зациклиться на весь фильм.
//   3. Ревизор: второй движок проверяет ВЕСЬ фильм чанками по 30 с.
//   4. Диаризация на выбор: sherpa-onnx (без токена) / pyannote / выкл.
//   5. Сшивка в SRT + packed.md с маркерами (музыка) и флагами неуверенности.
//   6. Кэш по источнику, настройкам этапа, скриптам и входным артефактам.
class Stories3Pipeline : public QObject
{
    Q_OBJECT

public:
    explicit Stories3Pipeline(QObject* parent = nullptr);

    // Вызывается после того, как ffprobe/аудио готовы
    void start(const QString& videoPath, const MediaInfo& info,
               const QString& outDir, const QString& tempDir,
               const QString& wavPath, const QString& language);

    void setLlmClient(LlmClient* llm) { m_llm = llm; }
    void cancel();

signals:
    void logMessage(const QString& msg);
    void failed(const QString& error);

private slots:
    void onRunnerFinished(S3Stage stage, const QString& outPath);
    void onRunnerFailed(S3Stage stage, const QString& error);

private:
    void runAudioMapOrSkip();
    void runAsr();
    void runReviewOrDiarize();
    void runDiarizeOrMerge();
    void runMerge();
    void startLlm();

    // Stage-specific keys include settings, scripts and upstream artifacts.
    QString cacheKey(const QString& cachePath) const;
    bool cacheValid(const QString& cachePath) const;
    void writeCacheMeta(const QString& cachePath) const;

    QString cached(const QString& name) const;

    Stories3Runner m_runner;
    LlmClient* m_llm = nullptr;

    QString m_videoPath;
    QString m_outDir;
    QString m_tempDir;
    QString m_wavPath;
    QString m_language;
    MediaInfo m_info;

    // промежуточные артефакты
    QString m_audioMapJson;
    QString m_wordsJson;
    QString m_cleanWordsJson;
    QString m_reviewJson;
    QString m_diarJson;
    QString m_speechSrt;
    QString m_packedMd;

    bool m_haveAudioMap = false;
    bool m_haveReview = false;
    bool m_haveDiar = false;

    QJsonObject m_quality;
    QList<S3AudioSeg> m_audioSegs;
    QList<S3Phrase> m_phrases; // чтобы не парсить packed повторно
};

#endif // STORIES3PIPELINE_H
