/* pipelinemanager.cpp */

#include "pipelinemanager.h"
#include "s3cache.h"
#include "subtitlecomposer.h"
#include "appsettings.h"
#include "storytimeline.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QImage>
#include <QPainter>
#include <QDateTime>
#include <QUuid>
#include <QTimer>
#include <QSaveFile>
#include <QRegularExpression>
#include <algorithm>

QJsonObject FileRunResult::json() const
{
    return {{"source", source}, {"status", status}, {"error", error}, {"log_directory", logDirectory}, {"output_directory", outputDirectory},
        {"considered_stories", considered}, {"accepted_stories", accepted}, {"rejected_stories", rejected},
        {"rendered_stories", renderedStories}, {"expected_videos", expectedVideos},
        {"created_videos", outputs.size()}, {"outputs", QJsonArray::fromStringList(outputs)}};
}
QStringList QueueRunSummary::outputs() const
{
    QStringList result; for (const auto& file : files) result.append(file.outputs); return result;
}
QString QueueRunSummary::text() const
{
    int completed = 0, partial = 0, failed = 0, stories = 0;
    for (const auto& file : files) {
        completed += file.status == "completed"; partial += file.status == "partial";
        failed += file.status == "failed"; stories += file.renderedStories;
    }
    return QString("Очередь завершена: файлов %1, успешно %2, частично %3, с ошибками %4. "
                   "Новых историй: %5. Новых видеофайлов: %6.")
        .arg(files.size()).arg(completed).arg(partial).arg(failed).arg(stories).arg(outputs().size());
}

static bool writeResultJson(const QString& path, const QJsonObject& result)
{
    QSaveFile file(path);
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

PipelineManager::PipelineManager(QObject *parent) : QObject(parent)
{
    AppSettings::instance().load();
    connect(this, &PipelineManager::logMessage, this, [this](const QString& message) {
        if (m_runLogDirectory.isEmpty()) return;
        QFile log(m_runLogDirectory + "/application.log");
        if (log.open(QIODevice::WriteOnly | QIODevice::Append))
            log.write((QDateTime::currentDateTime().toString("[yyyy-MM-dd HH:mm:ss.zzz] ")
                       + message + "\n").toUtf8());
    });

    connect(&extractor, &MediaExtractor::ffprobeFinished, this, &PipelineManager::onffprobeFinished);
    connect(&extractor, &MediaExtractor::subtitleExtracted, this, &PipelineManager::onSubtitleExtracted);
    connect(&extractor, &MediaExtractor::audioExtracted, this, &PipelineManager::onAudioExtracted);
    connect(&extractor, &MediaExtractor::framesExtracted, this, &PipelineManager::onFramesExtracted);
    connect(&extractor, &MediaExtractor::logMessage, this, &PipelineManager::logMessage);
    connect(&extractor, &MediaExtractor::errorOccurred, this, &PipelineManager::onErrorOccurred);

    connect(&dataParser, &MetadataParser::parseCompleted, this, &PipelineManager::onParseCompleted);
    connect(&dataParser, &MetadataParser::errorOccurred, this, &PipelineManager::onErrorOccurred);

    connect(&llm, &LlmClient::highlightsFound, this, &PipelineManager::onHighlightsFound);
    connect(&llm, &LlmClient::storiesFound, this, &PipelineManager::onStoriesFound);
    connect(&llm, &LlmClient::storySelectionFinished, this, [this](int considered, int accepted, int rejected) {
        m_fileResult.considered = considered; m_fileResult.accepted = accepted; m_fileResult.rejected = rejected;
    });
    connect(&llm, &LlmClient::logMessage, this, &PipelineManager::logMessage);
    connect(&llm, &LlmClient::errorOccurred, this, &PipelineManager::onErrorOccurred);
    connect(&llm, &LlmClient::storyFramesRequested, this, &PipelineManager::onStoryFramesRequested);

    // Пересказ 3.0 — отдельный пайплайн
    m_s3.setLlmClient(&llm);
    connect(&m_s3, &Stories3Pipeline::logMessage, this, &PipelineManager::logMessage);
    connect(&m_s3, &Stories3Pipeline::failed, this, &PipelineManager::onStories3Failed);
    // клипы v3 приходят через llm::storiesFound (он уже подключён выше);
    // запрос кадров идёт через llm::storyFramesRequested — тоже уже подключён.
    // Дублировать эти связи нельзя: получим двойной рендер.

    connect(&whisper, &WhisperRunner::transcriptionDone, this, &PipelineManager::onTranscriptionDone);
    connect(&whisper, &WhisperRunner::errorOccurred, this, &PipelineManager::onErrorOccurred);

    connect(&diarizer, &DiarizeRunner::diarizationDone, this, &PipelineManager::onDiarizationDone);
    connect(&diarizer, &DiarizeRunner::errorOccurred, this, [this](const QString& err) {
        // диаризация опциональна — при ошибке просто продолжаем без меток спикеров
        emit logMessage("[ПРЕДУПРЕЖДЕНИЕ] " + err + " Продолжаю без диаризации.");
        m_diarSegs.clear();
        finishStoryPacking();
    });

    connect(&renderer, &ClipRenderer::clipRendered, this, &PipelineManager::onClipRendered);
    connect(&renderer, &ClipRenderer::errorOccurred, this, &PipelineManager::onErrorOccurred);
}

void PipelineManager::onErrorOccurred(const QString& errorMessage)
{
    if (!m_fileActive) return;
    // сбой извлечения кадров для агента не смертелен: продолжаем без визуала
    if (m_agentFramesPending) {
        m_agentFramesPending = false;
        emit logMessage("[ПРЕДУПРЕЖДЕНИЕ] " + errorMessage + " Продолжаю без кадров.");
        if (m_mode == Mode::Stories3)
            llm.deliverStoryFramesV3({});
        else
            llm.deliverStoryFrames({});
        return;
    }

    m_runError += (m_runError.isEmpty() ? QString() : "\n") + errorMessage;
    emit logMessage("[ОШИБКА] " + errorMessage);
    if (m_mode == Mode::Stories3 && m_taskIndex >= 0 && m_taskIndex < m_renderTasks.size()) {
        const bool fatal = errorMessage.contains(QRegularExpression(
            "no space left|not enough space|недостаточно места|Не удалось запустить", QRegularExpression::CaseInsensitiveOption));
        if (!fatal) {
            emit logMessage("Эта версия не создана. Уже готовые видео сохранены; продолжаю следующие задачи.");
            ++m_taskIndex; m_audioPurpose = AudioPurpose::None;
            QTimer::singleShot(0, this, &PipelineManager::processNextRenderTask); return;
        }
        while (!m_fileQueue.isEmpty()) {
            FileRunResult skipped; skipped.source = m_fileQueue.dequeue(); skipped.status = "failed";
            skipped.error = "Очередь остановлена из-за общей ошибки рендера: " + errorMessage;
            m_queueSummary.files.append(skipped);
        }
    }
    emit logMessage("Файл пропущен: " + currentFilePath);
    finishCurrentFile();
}

// ---------- очередь ----------

void PipelineManager::addFilesToQueue(const QStringList& files)
{
    if (files.isEmpty()) return;
    if (!m_isProcessing) m_queueSummary = {};
    m_queueSummary.totalFiles += files.size();
    for (const QString& file : files)
        m_fileQueue.enqueue(file);

    if (!m_isProcessing)
        processNext();
}

void PipelineManager::processNext()
{
    if (m_fileQueue.isEmpty()) {
        m_isProcessing = false;
        emit logMessage(m_queueSummary.text());
        emit queueSummaryReady(m_queueSummary);
        emit queueFinished();
        return;
    }

    m_isProcessing = true;
    QString nextPath = m_fileQueue.dequeue();
    startPipeline(nextPath);
}

void PipelineManager::startPipeline(const QString& path)
{
    m_fileActive = true;
    m_fileResult = {}; m_fileResult.source = path;
    m_renderedStoryIds.clear();
    currentFilePath = path;
    baseName = QFileInfo(currentFilePath).completeBaseName();

    outDir = QFileInfo(currentFilePath).absoluteDir().filePath(baseName + "_clips");
    tempDir = outDir + "/temp";
    QDir().mkpath(tempDir);
    m_runLogDirectory = outDir + "/logs/run_"
        + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz") + "_"
        + QUuid::createUuid().toString(QUuid::Id128).left(8);
    QDir().mkpath(m_runLogDirectory);
    m_fileResult.logDirectory = m_runLogDirectory;
    const auto runId = QFileInfo(m_runLogDirectory).fileName();
    m_runOutputDirectory = m_mode == Mode::Stories3 ? outDir + "/runs/" + runId : outDir;
    m_renderTempDirectory = m_mode == Mode::Stories3 ? tempDir + "/runs/" + runId : tempDir;
    QDir().mkpath(m_runOutputDirectory); QDir().mkpath(m_renderTempDirectory);
    m_fileResult.outputDirectory = m_runOutputDirectory;
    if (m_mode == Mode::Stories3)
        writeResultJson(outDir + "/latest_attempt.json", {{"status", "running"}, {"output_directory", m_runOutputDirectory}, {"log_directory", m_runLogDirectory}});
    m_runError.clear();
    m_runOutputs.clear();
    llm.setRunLogDirectory(m_runLogDirectory);
    const auto& settings = AppSettings::instance();
    llm.writeRunArtifact("run.json", {{"status", "running"}, {"source", currentFilePath},
        {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"mode", int(m_mode)}, {"model", settings.llmModel},
        {"min_stories", settings.minClips}, {"max_stories", settings.maxClips},
        {"min_story_sec", settings.s3MinClipSec}, {"max_story_sec", settings.s3MaxClipSec}});

    m_subsPaths.clear();
    m_missingSubsLangs.clear();
    m_pendingSubExtracts.clear();
    m_subsCandidates.clear();
    m_renderTasks.clear();
    m_taskIndex = -1;
    m_audioPurpose = AudioPurpose::None;
    m_cues.clear();
    m_s3Words.clear();
    m_diarSegs.clear();
    m_highlights.clear();
    m_stories.clear();
    m_packedText.clear();
    m_storyTranscriptLang.clear();
    m_s3AudioKey.clear();
    m_fallbackAudioLang.clear();
    m_currentSubLang.clear();
    m_agentRanges.clear();
    m_agentFramesPending = false;

    emit logMessage("=== Файл: " + baseName + " ===");
    emit logMessage("Логи текущего прогона: " + m_runLogDirectory);
    if (m_mode == Mode::Stories3)
        emit logMessage(QString("V3: %1; готовые транскрипты берутся из кэша. ").arg(
            QStringList{"продолжение анализа", "пересборка историй по готовой карте", "полный анализ LLM"}.value(int(settings.s3RunMode))) +
                        "Число историй " + QString::number(settings.minClips) + "–"
                        + QString::number(settings.maxClips) + ", лимит каждой истории "
                        + QString::number(settings.s3MaxClipSec) + " с.");
    emit logMessage("Шаг 1/6: ffprobe...");
    extractor.runFFprobe(path);
}

// ---------- ffprobe / парсинг ----------

void PipelineManager::onffprobeFinished(const QByteArray& data)
{
    // сохраняем сырой JSON рядом с видео (как раньше)
    QString savePath = QFileInfo(currentFilePath).absoluteDir().filePath(baseName + ".json");
    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonDocument raw = QJsonDocument::fromJson(data);
        file.write(raw.isNull() ? data : raw.toJson(QJsonDocument::Indented));
        file.close();
    }

    emit logMessage("Шаг 2/6: разбор метаданных...");
    dataParser.parse(data, currentFilePath);
}

void PipelineManager::onParseCompleted(const QJsonDocument& json, const MediaInfo& info)
{
    Q_UNUSED(json);
    currentInfo = info;

    emit logMessage(QString("Метаданные: длительность %1 сек, глав: %2, аудио: %3, субтитров: %4")
                        .arg(info.durationSec)
                        .arg(info.chapters.size())
                        .arg(info.audioTracks.size())
                        .arg(info.subtitleTracks.size()));
    emit logMessage(QString("Дорожки: RU аудио=%1, EN аудио=%2, RU сабы=%3, EN сабы=%4 "
                            "(-1 = не найдено; сабы — индекс 0:s:N, аудио — 0:a:N)")
                        .arg(info.rusAudioIndex).arg(info.engAudioIndex)
                        .arg(info.rusSubsIndex).arg(info.engSubsIndex));

    startSubtitleStage();
}

// ---------- субтитры ----------

void PipelineManager::startSubtitleStage()
{
    const AppSettings& s = AppSettings::instance();

    // Пересказ 3.0 с whisper-транскриптом: сабы из файла не нужны вовсе —
    // транскрибируем весь фильм word-level (с кэшем)
    if (m_mode == Mode::Stories && s.storyWhisperTranscript) {
        emit logMessage("Шаг 3/6: транскрипт фильма (whisper word-level)...");
        startStoryTranscript();
        return;
    }

    // Пересказ 3.0: свой путь (карта музыки + чанки + ревизор), сабы не нужны
    if (m_mode == Mode::Stories3) {
        emit logMessage("Шаг 3/6: транскрипт фильма (Пересказ 3.0, чистый ASR)...");
        startStories3();
        return;
    }

    emit logMessage("Шаг 3/6: субтитры...");

    bool needRu = (s.subsForLlm != AppSettings::SubsForLlm::EnOnly);
    bool needEn = (s.subsForLlm != AppSettings::SubsForLlm::RuOnly);

    if (needRu) {
        m_subsCandidates["ru"] = currentInfo.rusSubsCandidates;
        if (!m_subsCandidates["ru"].isEmpty())
            m_pendingSubExtracts.append({"ru", m_subsCandidates["ru"].takeFirst()});
        else
            m_missingSubsLangs.append("ru");
    }
    if (needEn) {
        m_subsCandidates["en"] = currentInfo.engSubsCandidates;
        if (!m_subsCandidates["en"].isEmpty())
            m_pendingSubExtracts.append({"en", m_subsCandidates["en"].takeFirst()});
        else
            m_missingSubsLangs.append("en");
    }

    if (m_pendingSubExtracts.isEmpty())
        afterSubsStage();
    else
        extractNextSubtitle();
}

void PipelineManager::extractNextSubtitle()
{
    auto job = m_pendingSubExtracts.takeFirst();
    m_currentSubLang = job.first;
    QString outPath = tempDir + "/subs_" + job.first + ".srt";

    emit logMessage(QString("Извлекаю субтитры [%1], дорожка 0:s:%2...")
                        .arg(job.first).arg(job.second));
    extractor.extractSubtitle(currentFilePath, job.second, outPath);
}

void PipelineManager::onSubtitleExtracted(const QString& outPath)
{
    // отбраковка forced-сабов по содержимому (в MP4/M4V нет флагов forced —
    // отличить можно только по плотности реплик)
    if (!validateSubsDensity(outPath)) {
        QList<int>& cands = m_subsCandidates[m_currentSubLang];
        if (!cands.isEmpty()) {
            int next = cands.takeFirst();
            emit logMessage(QString("Пробую следующую дорожку [%1]: 0:s:%2...")
                                .arg(m_currentSubLang).arg(next));
            extractor.extractSubtitle(currentFilePath, next, outPath);
            return;
        }
        emit logMessage(QString("Все дорожки [%1] похожи на forced — транскрибация whisper.")
                            .arg(m_currentSubLang));
        m_missingSubsLangs.append(m_currentSubLang);
    } else {
        m_subsPaths[m_currentSubLang] = outPath;
        emit logMessage(QString("Субтитры [%1] извлечены: %2").arg(m_currentSubLang, outPath));
    }

    if (!m_pendingSubExtracts.isEmpty())
        extractNextSubtitle();
    else
        afterSubsStage();
}

bool PipelineManager::validateSubsDensity(const QString& srtPath)
{
    const AppSettings& s = AppSettings::instance();
    if (s.minSubsCuesPerHour <= 0)
        return true;

    int cues = SrtUtils::parseFile(srtPath).size();
    double hours = currentInfo.durationSec / 3600.0;
    double density = hours > 0.0 ? cues / hours : double(cues);

    if (density >= s.minSubsCuesPerHour)
        return true;

    emit logMessage(QString("Дорожка [%1] похожа на forced: %2 реплик (%3/час, порог %4/час).")
                        .arg(m_currentSubLang)
                        .arg(cues)
                        .arg(density, 0, 'f', 1)
                        .arg(s.minSubsCuesPerHour));
    return false;
}

void PipelineManager::afterSubsStage()
{
    if (m_missingSubsLangs.isEmpty()) {
        proceedToLlm();
        return;
    }

    // Whisper fallback: транскрибируем одну аудиодорожку и используем для всех недостающих языков
    int audioIndex = -1;
    if (m_missingSubsLangs.contains("en") && currentInfo.engAudioIndex >= 0) {
        audioIndex = currentInfo.engAudioIndex;
        m_fallbackAudioLang = "en";
    } else if (currentInfo.rusAudioIndex >= 0) {
        audioIndex = currentInfo.rusAudioIndex;
        m_fallbackAudioLang = "ru";
    } else if (currentInfo.engAudioIndex >= 0) {
        audioIndex = currentInfo.engAudioIndex;
        m_fallbackAudioLang = "en";
    } else if (!currentInfo.audioTracks.isEmpty()) {
        audioIndex = currentInfo.audioTracks.first().audioIndex;
        m_fallbackAudioLang = currentInfo.audioTracks.first().language;
    }

    if (audioIndex < 0) {
        onErrorOccurred("Нет ни субтитров, ни аудиодорожки для whisper-транскрибации.");
        return;
    }

    emit logMessage(QString("Субтитры [%1] отсутствуют — whisper-транскрибация аудио 0:a:%2...")
                        .arg(m_missingSubsLangs.join(", "))
                        .arg(audioIndex));
    m_audioPurpose = AudioPurpose::FallbackTranscribe;
    extractor.extractAudioWav(currentFilePath, audioIndex, tempDir + "/full_audio.wav");
}

// ---------- whisper ----------

void PipelineManager::onAudioExtracted(const QString& outPath)
{
    if (m_audioPurpose == AudioPurpose::FallbackTranscribe) {
        whisper.transcribe(outPath, tempDir + "/full_transcript", m_fallbackAudioLang, false);
    } else if (m_audioPurpose == AudioPurpose::StoryTranscript) {
        emit logMessage(QString("Whisper word-level [%1] по всему фильму (долго, кэшируется)...")
                            .arg(m_storyTranscriptLang));
        whisper.transcribe(outPath, tempDir + "/story_transcript_v2_" + m_storyTranscriptLang,
                           m_storyTranscriptLang, true);
    } else if (m_audioPurpose == AudioPurpose::ClipSegment) {
        const RenderTask& task = m_renderTasks[m_taskIndex];
        emit logMessage(QString("Whisper (word-level) [%1] для клипа %2...")
                            .arg(task.lang).arg(task.clipIndex + 1));
        whisper.transcribe(task.wavPath, task.srtBase, task.lang, true);
    } else if (m_audioPurpose == AudioPurpose::Stories3Audio) {
        m_audioPurpose = AudioPurpose::None;
        emit logMessage("Полное аудио готово, запускаю Пересказ 3.0...");
        S3Cache::writeMeta(outPath, m_s3AudioKey);
        m_s3.start(currentFilePath, currentInfo, outDir, tempDir, outPath, m_storyTranscriptLang);
    }
}

void PipelineManager::onTranscriptionDone(const QString& srtPath)
{
    if (m_audioPurpose == AudioPurpose::FallbackTranscribe) {
        emit logMessage("Whisper-транскрибация завершена: " + srtPath);
        for (const QString& lang : m_missingSubsLangs)
            m_subsPaths[lang] = srtPath;
        m_missingSubsLangs.clear();
        m_audioPurpose = AudioPurpose::None;
        proceedToLlm();
    } else if (m_audioPurpose == AudioPurpose::StoryTranscript) {
        emit logMessage("Транскрипт фильма готов: " + srtPath);
        onStoryTranscriptReady(srtPath);
    } else if (m_audioPurpose == AudioPurpose::ClipSegment) {
        m_audioPurpose = AudioPurpose::None;
        renderTaskWithSrt(srtPath);
    }
}

// ---------- Пересказ 3.0: чистый ASR + карта музыки + ревизор ----------

void PipelineManager::startStories3()
{
    // дорожка: RU, затем EN, затем первая доступная (как в v2)
    int audioIndex = -1;
    if (currentInfo.rusAudioIndex >= 0)
        audioIndex = currentInfo.rusAudioIndex;
    else if (currentInfo.engAudioIndex >= 0)
        audioIndex = currentInfo.engAudioIndex;
    else if (!currentInfo.audioTracks.isEmpty())
        audioIndex = currentInfo.audioTracks.first().audioIndex;

    if (audioIndex < 0) {
        onStories3Failed("Нет аудиодорожки для Пересказа 3.0.");
        return;
    }

    const QString wavPath = tempDir + "/s3_full_audio.wav";
    m_storyTranscriptLang = audioIndex == currentInfo.engAudioIndex ? "en" : "ru";
    m_s3AudioKey = S3Cache::makeKey(currentFilePath,
        {{"audioIndex", audioIndex}, {"language", m_storyTranscriptLang},
         {"format", "pcm_s16le/16000/mono"},
         {"ffmpeg", AppSettings::instance().ffmpegPath}});
    const bool reuse = S3Cache::valid(wavPath, m_s3AudioKey);
    if (reuse) {
        emit logMessage("Пересказ 3.0: аудио из кэша — " + wavPath);
        m_s3.start(currentFilePath, currentInfo, outDir, tempDir, wavPath, m_storyTranscriptLang);
        return;
    }

    emit logMessage(QString("Пересказ 3.0: извлекаю аудио 16 кГц моно (дорожка 0:a:%1)...")
                        .arg(audioIndex));
    m_audioPurpose = AudioPurpose::Stories3Audio;
    extractor.extractAudioWav(currentFilePath, audioIndex, wavPath);
}

void PipelineManager::onStories3Failed(const QString& error)
{
    m_runError = error;
    emit logMessage("[ОШИБКА] " + error);
    emit logMessage("Файл пропущен: " + currentFilePath);
    finishCurrentFile();
}

// Единый очищенный транскрипт: один cue на слово, абсолютное время фильма.
void PipelineManager::loadS3Words()
{
    m_s3Words.clear();
    const QString path = tempDir + "/s3_clean_words.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (doc.object().value("language").toString() != m_storyTranscriptLang)
        return;
    const QJsonArray words = doc.object().value("words").toArray();

    for (const QJsonValue& v : words) {
        const QJsonObject o = v.toObject();
        const QString w = o.value("w").toString();
        const double t = o.value("t").toDouble();
        const double e = o.value("e").toDouble();
        if (w.trimmed().isEmpty() || e <= t)
            continue;
        SrtCue cue;
        cue.index = m_s3Words.size() + 1;
        cue.startSec = t;
        cue.endSec = e;
        cue.text = w.trimmed();
        m_s3Words.append(cue);
    }
    emit logMessage(QString("Пересказ 3.0: проверенных слов — %1.").arg(m_s3Words.size()));
}

// ---------- Пересказ 2.0: транскрипт фильма ----------

void PipelineManager::startStoryTranscript()
{
    // язык транскрипта: предпочитаем RU, затем EN, затем первую доступную дорожку
    QString lang;
    int audioIndex = -1;
    if (currentInfo.rusAudioIndex >= 0) {
        lang = "ru"; audioIndex = currentInfo.rusAudioIndex;
    } else if (currentInfo.engAudioIndex >= 0) {
        lang = "en"; audioIndex = currentInfo.engAudioIndex;
    } else if (!currentInfo.audioTracks.isEmpty()) {
        lang = currentInfo.audioTracks.first().language;
        audioIndex = currentInfo.audioTracks.first().audioIndex;
    }
    if (audioIndex < 0) {
        onErrorOccurred("Нет аудиодорожки для whisper-транскрибации фильма.");
        return;
    }
    m_storyTranscriptLang = lang;

    QFileInfo srcInfo(currentFilePath);
    QString srtPath = tempDir + "/story_transcript_v2_" + lang + ".srt";
    QFileInfo srtInfo(srtPath);
    // кэш: не пересчитываем, если транскрипт свежее исходника (hard rule 9 video-use)
    if (srtInfo.exists() && srtInfo.size() > 0 && srtInfo.lastModified() >= srcInfo.lastModified()) {
        emit logMessage("Транскрипт из кэша: " + srtPath);
        onStoryTranscriptReady(srtPath);
        return;
    }

    QString wavPath = tempDir + "/story_full_audio.wav";
    QFileInfo wavInfo(wavPath);
    m_audioPurpose = AudioPurpose::StoryTranscript;
    if (wavInfo.exists() && wavInfo.size() > 0 && wavInfo.lastModified() >= srcInfo.lastModified()) {
        emit logMessage(QString("Полное аудио из кэша. Whisper word-level [%1] по всему фильму...")
                            .arg(lang));
        whisper.transcribe(wavPath, tempDir + "/story_transcript_v2_" + lang, lang, true);
        return;
    }

    emit logMessage(QString("Извлекаю полное аудио (дорожка 0:a:%1)...").arg(audioIndex));
    extractor.extractAudioWav(currentFilePath, audioIndex, wavPath);
}

// ---------- Пересказ 3.0: чистый ASR + карта музыки + ревизор ----------

void PipelineManager::onStoryTranscriptReady(const QString& srtPath)
{
    m_audioPurpose = AudioPurpose::None;

    // word-level реплики → слова для снаппинга и упаковки
    m_cues = SrtUtils::parseFile(srtPath);
    m_diarSegs.clear();

    // whisper иногда растягивает слово на десятки секунд (DTW по музыке/тишине) —
    // без правки это ломает и фразы, и границы клипов
    const int fixed = SrtUtils::clampWordDurations(m_cues);
    if (fixed > 0)
        emit logMessage(QString("Выравнивание слов: исправлено %1 растянутых cue.").arg(fixed));

    // защита от зацикливания whisper: вырожденный транскрипт нельзя отдавать в LLM —
    // модель узнает фильм по названию и выдумает и сюжет, и цитаты, и таймкоды
    const double uniq = SrtUtils::uniqueTextRatio(m_cues);
    if (m_cues.size() >= 100 && uniq < 0.02) {
        onErrorOccurred(QString("Whisper зациклился: в транскрипте %1 реплик, но уникальных "
                                "текстов всего %2% — одна и та же фраза повторяется весь фильм. "
                                "Такое бывает без флагов -mc 0 / --vad (теперь они включены всегда) "
                                "или на проблемной модели/дорожке. Удали кэш транскрипта в папке "
                                "temp и запусти заново; либо выбери источник транскрипта "
                                "«Субтитры из файла».")
                            .arg(m_cues.size())
                            .arg(uniq * 100.0, 0, 'f', 2));
        return;
    }

    const AppSettings& s = AppSettings::instance();
    QString wavPath = tempDir + "/story_full_audio.wav";
    QString diarJson = tempDir + "/story_diar.json";

    // опциональная диаризация (pyannote через python)
    if (s.diarizationEnabled && !s.pythonPath.isEmpty()) {
        QFileInfo di(diarJson), srcInfo(currentFilePath);
        if (di.exists() && di.size() > 0 && di.lastModified() >= srcInfo.lastModified()) {
            emit logMessage("Диаризация из кэша: " + diarJson);
            onDiarizationDone(diarJson);
            return;
        }
        if (QFile::exists(wavPath)) {
            emit logMessage("Диаризация pyannote (python, может занять минуты)...");
            diarizer.diarize(wavPath, diarJson);
            return;
        }
        emit logMessage("[ПРЕДУПРЕЖДЕНИЕ] Нет wav для диаризации, пропускаю.");
    }

    finishStoryPacking();
}

void PipelineManager::onDiarizationDone(const QString& jsonPath)
{
    m_diarSegs = DiarizeRunner::parseJson(jsonPath);
    QSet<QString> speakers;
    for (const DiarSegment& d : m_diarSegs)
        speakers.insert(d.speaker);
    emit logMessage(QString("Диаризация: %1 сегментов, %2 спикеров.")
                        .arg(m_diarSegs.size()).arg(speakers.size()));
    finishStoryPacking();
}

void PipelineManager::finishStoryPacking()
{
    const AppSettings& s = AppSettings::instance();
    auto phrases = TranscriptPacker::pack(m_cues, s.editPauseSec, m_diarSegs, s.editMaxPhraseSec);
    m_packedText = TranscriptPacker::toMarkdown(phrases);

    // сохраняем packed-транскрипт рядом с клипами — полезно для отладки
    QFile f(outDir + "/movie_packed.md");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(m_packedText.toUtf8());
        f.close();
    }

    emit logMessage(QString("Транскрипт упакован: %1 слов → %2 фраз (%3 символов).")
                        .arg(m_cues.size()).arg(phrases.size()).arg(m_packedText.size()));
    proceedToLlm();
}

void PipelineManager::proceedToLlm()
{
    startLlmStage();
}

// ---------- Пересказ 3.0: свой путь (карта музыки + ASR чанками + ревизор) ----------

// ---------- Пересказ 2.0: агентный доступ к кадрам ----------

void PipelineManager::onStoryFramesRequested(const QList<QPair<double, double>>& ranges)
{
    const AppSettings& s = AppSettings::instance();

    m_agentRanges = ranges;

    // плотность кадров: суммарно не больше agentMaxFramesPerTurn
    double totalSec = 0.0;
    for (const auto& r : ranges)
        totalSec += r.second - r.first;
    double interval = s.agentFrameIntervalSec > 0.1 ? s.agentFrameIntervalSec : 2.0;
    int maxFrames = qMax(1, s.agentMaxFramesPerTurn);
    if (totalSec / interval > maxFrames)
        interval = totalSec / maxFrames;
    m_agentInterval = interval;

    QString dir = tempDir + "/agent_frames";
    QDir(dir).removeRecursively(); // кадры прошлого запроса не нужны

    m_agentFramesPending = true;
    emit logMessage(QString("Извлекаю кадры %1 фрагментов для агента (кадр каждые %2 с)...")
                        .arg(ranges.size()).arg(interval, 0, 'f', 1));
    extractor.extractFramesRanges(currentFilePath, ranges, interval, dir);
}

void PipelineManager::onFramesExtracted(const QString& dirPath)
{
    m_agentFramesPending = false;

    // собираем кадры по диапазонам: r<i>_%04d.jpg, время = начало диапазона + k*интервал
    QList<QPair<double, QString>> frames;
    QDir d(dirPath);
    for (int i = 0; i < m_agentRanges.size(); ++i) {
        QStringList files = d.entryList({QString("r%1_*.jpg").arg(i)}, QDir::Files, QDir::Name);
        for (int k = 0; k < files.size(); ++k)
            frames.append({m_agentRanges[i].first + k * m_agentInterval, d.filePath(files[k])});
    }

    emit logMessage(QString("Кадры для агента готовы: %1 шт.").arg(frames.size()));
    if (m_mode == Mode::Stories3 && AppSettings::instance().s3Filmstrip) {
        QList<QPair<double, QString>> sheets;
        for (int first = 0; first < frames.size(); first += 9) {
            const int count = qMin(9, int(frames.size()) - first);
            QImage sheet(960, ((count + 2) / 3) * 204, QImage::Format_RGB32);
            sheet.fill(Qt::black);
            QPainter painter(&sheet);
            painter.setPen(Qt::white);
            for (int k = 0; k < count; ++k) {
                const auto& frame = frames[first + k];
                const QImage image(frame.second);
                const QImage scaled = image.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                const int x = (k % 3) * 320, y = (k / 3) * 204;
                painter.drawImage(x + (320 - scaled.width()) / 2, y, scaled);
                painter.drawText(QRect(x + 4, y + 180, 312, 24),
                    QString::number(frame.first, 'f', 2) + " s");
            }
            painter.end();
            const QString path = d.filePath(QString("filmstrip_%1.jpg").arg(first / 9));
            if (sheet.save(path, "JPG", 88)) sheets.append({frames[first].first, path});
        }
        if (!sheets.isEmpty()) frames = sheets;
    }
    if (m_mode == Mode::Stories3)
        llm.deliverStoryFramesV3(frames);
    else
        llm.deliverStoryFrames(frames);
}

// ---------- LLM ----------

void PipelineManager::startLlmStage()
{
    if (m_mode == Mode::Highlights)
        emit logMessage("Шаг 4/6: анализ в LLM (Аналитик → Скаут → Продюсер)...");
    else
        emit logMessage("Шаг 4/6: анализ в LLM (пересказ с драматургией, один запрос)...");

    if (m_mode == Mode::Stories) {
        const AppSettings& s = AppSettings::instance();

        // источник транскрипта: whisper word-level (уже упакован) или сабы из файла
        if (!s.storyWhisperTranscript) {
            m_cues = SrtUtils::parseFile(m_subsPaths.value("ru"));
            if (m_cues.isEmpty())
                m_cues = SrtUtils::parseFile(m_subsPaths.value("en"));
            if (m_cues.isEmpty()) {
                onErrorOccurred("Нет текста субтитров для LLM-анализа.");
                return;
            }
            auto phrases = TranscriptPacker::pack(m_cues, s.editPauseSec, s.editMaxPhraseSec);
            m_packedText = TranscriptPacker::toMarkdown(phrases);
            emit logMessage(QString("Сабы упакованы: %1 реплик → %2 фраз.")
                                .arg(m_cues.size()).arg(phrases.size()));
        }

        if (m_packedText.trimmed().isEmpty()) {
            onErrorOccurred("Пустой транскрипт для LLM-анализа.");
            return;
        }

        // агентный режим: LLM сама запросит кадры нужных фрагментов
        llm.findStories(currentInfo, m_packedText);
        return;
    }

    auto readSrt = [](const QString& path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QString();
        return QString::fromUtf8(f.readAll());
    };

    QString subsRu = readSrt(m_subsPaths.value("ru"));
    QString subsEn = readSrt(m_subsPaths.value("en"));

    if (subsRu.trimmed().isEmpty() && subsEn.trimmed().isEmpty()) {
        onErrorOccurred("Нет текста субтитров для LLM-анализа.");
        return;
    }

    // реплики для снаппинга границ сегментов (таймкоды у RU/EN одинаковые)
    m_cues = SrtUtils::parseFile(m_subsPaths.value("ru"));
    if (m_cues.isEmpty())
        m_cues = SrtUtils::parseFile(m_subsPaths.value("en"));

    llm.findHighlights(currentInfo, subsRu, subsEn);
}

void PipelineManager::onHighlightsFound(const QList<Highlight>& highlights, const QJsonArray& rawJson)
{
    const AppSettings& s = AppSettings::instance();

    // снаппинг границ к репликам — убирает обрывы на полуслове
    m_highlights = highlights;
    for (Highlight& h : m_highlights) {
        auto snapped = SrtUtils::snapSegment(h.startSec, h.endSec, m_cues,
                                             s.padStartSec, s.padEndSec,
                                             currentInfo.durationSec);
        h.startSec = snapped.first;
        h.endSec = snapped.second;
    }

    emit logMessage(QString("LLM отобрала %1 хайлайтов.").arg(m_highlights.size()));
    emit clipsFound(m_highlights);

    // сохраняем метаданные клипов
    QFile f(outDir + "/clips.json");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(rawJson).toJson(QJsonDocument::Indented));
        f.close();
    }

    emit logMessage("Шаг 5/6: подготовка задач рендера...");
    buildRenderTasks();
    startRenderTasks();
}

void PipelineManager::onStoriesFound(const QList<StoryClip>& stories, const QJsonArray& rawJson)
{
    const AppSettings& s = AppSettings::instance();

    // Пересказ 3.0: слова/фразы чище, чем у whisper, и приходят из своего SRT.
    // Снаппинг делаем по ним же, чтобы не тянуть whisper-эвристики.
    if (m_mode == Mode::Stories3) {
        loadS3Words();
        m_cues = m_s3Words;
    }

    // паддинги: whisper/ASR-режим — от границ фраз, subs-режим — от реплик
    const bool wordLevel = s.storyWhisperTranscript || m_mode == Mode::Stories3;
    double padStart = wordLevel ? s.editPaddingMs / 1000.0 : s.padStartSec;
    double padEnd   = wordLevel ? s.editPaddingMs / 1000.0 : s.padEndSec;

    m_stories.clear();
    for (StoryClip story : stories) {
        if (m_mode == Mode::Stories3) {
            if (story.timelineHash.isEmpty() || story.timelineHash != StoryTimeline::hash(story)
                || story.totalSec() < s.s3MinClipSec - .001 || story.totalSec() > s.s3MaxClipSec + .001) {
                onErrorOccurred("V3: монтаж изменился после аудита или не прошёл компиляцию временной шкалы."); return;
            }
            m_stories.append(story); continue;
        }
        // снаппинг каждой склейки к границам слов/реплик
        for (int si = 0; si < story.segments.size(); ++si) {
            StorySegment& seg = story.segments[si];
            auto sn = SrtUtils::snapSegment(seg.startSec, seg.endSec, m_cues,
                                            padStart, padEnd, currentInfo.durationSec);
            seg.startSec = sn.first;
            seg.endSec = sn.second;
        }
        // хронологический порядок + слияние близких склеек (метаданные — от первой)
        std::sort(story.segments.begin(), story.segments.end(),
                  [](const StorySegment& a, const StorySegment& b) { return a.startSec < b.startSec; });
        QList<StorySegment> merged;
        for (const StorySegment& seg : story.segments) {
            if (!merged.isEmpty() && seg.startSec - merged.last().endSec < s.mergeGapSec)
                merged.last().endSec = qMax(merged.last().endSec, seg.endSec);
            else
                merged.append(seg);
        }
        story.segments = merged;

        if (!story.segments.isEmpty())
            m_stories.append(story);
    }

    emit logMessage(QString("LLM создала %1 пересказов (после снаппинга: %2).")
                        .arg(stories.size()).arg(m_stories.size()));
    if (m_stories.isEmpty() || (m_mode == Mode::Stories3 && m_stories.size() < s.minClips && !s.s3AllowPartial)) {
        onErrorOccurred("Не осталось пересказов с допустимой длительностью.");
        return;
    }
    if (m_mode == Mode::Stories3 && m_stories.size() < s.minClips)
        m_runError = QString("Частичный результат: %1 историй из требуемых %2.").arg(m_stories.size()).arg(s.minClips);
    emit storiesFound(m_stories);
    llm.writeRunArtifact("final_edl.json", {{"stories", LlmClient::serializeStories(m_stories)}});

    // сохраняем метаданные + отдельные txt с подписями для постов
    QFile f(m_runOutputDirectory + "/stories.json");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(m_mode == Mode::Stories3
                    ? LlmClient::serializeStories(m_stories) : rawJson).toJson(QJsonDocument::Indented));
        f.close();
    }
    for (int i = 0; i < m_stories.size(); ++i) {
        QFile txt(m_runOutputDirectory + QString("/story_%1_caption.txt").arg(i + 1, 2, 10, QChar('0')));
        if (txt.open(QIODevice::WriteOnly)) {
            txt.write(m_stories[i].caption.toUtf8());
            txt.close();
        }
    }

    emit logMessage("Шаг 5/6: подготовка задач рендера...");
    buildStoryRenderTasks();
    startRenderTasks();
}

void PipelineManager::startRenderTasks()
{
    const auto& settings = AppSettings::instance();
    const int count = m_mode == Mode::Highlights ? m_highlights.size() : m_stories.size();
    m_fileResult.expectedVideos = count * (int(settings.renderRu) + int(settings.renderEn));
    if (m_mode != Mode::Stories3) m_fileResult.accepted = count;
    if (m_renderTasks.isEmpty()) {
        m_runError = "Нет задач рендера (проверь настройки языков и наличие аудиодорожек).";
        emit logMessage(m_runError);
        finishCurrentFile();
        return;
    }

    emit logMessage(QString("Шаг 6/6: рендер %1 клипов...").arg(m_renderTasks.size()));
    m_taskIndex = 0;
    processNextRenderTask();
}

// ---------- рендер ----------

void PipelineManager::buildRenderTasks()
{
    const AppSettings& s = AppSettings::instance();

    struct LangDef { QString lang; bool enabled; int audioIndex; };
    QList<LangDef> langs = {
        {"ru", s.renderRu, currentInfo.rusAudioIndex},
        {"en", s.renderEn, currentInfo.engAudioIndex},
    };

    for (int i = 0; i < m_highlights.size(); ++i) {
        const Highlight& h = m_highlights[i];
        for (const LangDef& l : langs) {
            if (!l.enabled)
                continue;
            if (l.audioIndex < 0) {
                emit logMessage(QString("Клип %1 [%2]: аудиодорожка не найдена, версия пропущена.")
                                    .arg(i + 1).arg(l.lang));
                continue;
            }
            QString langDir = outDir + "/" + l.lang;
            QDir().mkpath(langDir);

            RenderTask t;
            t.clipIndex = i;
            t.lang = l.lang;
            t.isStory = false;
            t.startSec = h.startSec;
            t.endSec = h.endSec;
            t.audioIndex = l.audioIndex;
            t.wavPath = tempDir + QString("/clip_%1_%2.wav").arg(i + 1).arg(l.lang);
            t.srtBase = tempDir + QString("/clip_%1_%2").arg(i + 1).arg(l.lang);
            t.outPath = langDir + QString("/clip_%1.mp4").arg(i + 1, 2, 10, QChar('0'));
            m_renderTasks.append(t);
        }
    }
}

void PipelineManager::buildStoryRenderTasks()
{
    const AppSettings& s = AppSettings::instance();

    struct LangDef { QString lang; bool enabled; int audioIndex; };
    QList<LangDef> langs = {
        {"ru", s.renderRu, currentInfo.rusAudioIndex},
        {"en", s.renderEn, currentInfo.engAudioIndex},
    };

    for (int i = 0; i < m_stories.size(); ++i) {
        const StoryClip& story = m_stories[i];
        for (const LangDef& l : langs) {
            if (!l.enabled)
                continue;
            if (l.audioIndex < 0) {
                emit logMessage(QString("Пересказ %1 [%2]: аудиодорожка не найдена, версия пропущена.")
                                    .arg(i + 1).arg(l.lang));
                continue;
            }
            QString langDir = m_runOutputDirectory + "/" + l.lang;
            QDir().mkpath(langDir);

            RenderTask t;
            t.clipIndex = i;
            t.lang = l.lang;
            t.isStory = true;
            for (const StorySegment& seg : story.segments)
                t.segments.append({seg.startSec, seg.endSec});
            // для фолбэка, если после снаппинга остался один сегмент
            t.startSec = story.segments.first().startSec;
            t.endSec = story.segments.last().endSec;
            t.audioIndex = l.audioIndex;
            t.wavPath = m_renderTempDirectory + QString("/story_%1_%2.wav").arg(i + 1).arg(l.lang);
            t.srtBase = m_renderTempDirectory + QString("/story_%1_%2").arg(i + 1).arg(l.lang);
            t.outPath = langDir + QString("/story_%1.mp4").arg(i + 1, 2, 10, QChar('0'));
            m_renderTasks.append(t);
        }
    }
}

void PipelineManager::processNextRenderTask()
{
    if (m_taskIndex >= m_renderTasks.size()) {
        emit logMessage(QString("Задачи рендера завершены: создано %1 из %2 видео.").arg(m_runOutputs.size()).arg(m_renderTasks.size()));
        finishCurrentFile();
        return;
    }

    const RenderTask& task = m_renderTasks[m_taskIndex];

    // Пересказ 3.0: субтитры клипа берём из чистого ASR-транскрипта,
    // whisper для клипа не запускаем вовсе (он и был источником галлюцинаций).
    if (m_mode == Mode::Stories3 && task.lang == m_storyTranscriptLang) {
        const QString s3Srt = buildS3ClipSrt(task, task.srtBase);
        if (!s3Srt.isEmpty()) {
            emit logMessage(QString("Пересказ 3.0: клип %1/%2 на чистых сабах.")
                                .arg(m_taskIndex + 1).arg(m_renderTasks.size()));
            m_audioPurpose = AudioPurpose::None;
            renderTaskWithSrt(s3Srt);
            return;
        }
        onErrorOccurred("Пересказ 3.0: нет проверенных слов для выбранного фрагмента.");
        return;
    }

    m_audioPurpose = AudioPurpose::ClipSegment;

    if (task.isStory && task.segments.size() > 1) {
        emit logMessage(QString("Пересказ %1/%2: склейка аудио из %3 сегментов [%4]...")
                            .arg(m_taskIndex + 1).arg(m_renderTasks.size())
                            .arg(task.segments.size()).arg(task.lang));
        extractor.extractConcatAudioWav(currentFilePath, task.audioIndex, task.segments, task.wavPath,
                                        storyTransitionSec());
    } else {
        emit logMessage(QString("Клип %1/%2: извлечение аудио [%3]...")
                            .arg(m_taskIndex + 1).arg(m_renderTasks.size()).arg(task.lang));
        extractor.extractAudioWav(currentFilePath, task.audioIndex, task.wavPath,
                                  task.startSec, task.endSec);
    }
}

// Субтитры клипа v3 прямо из чистого транскрипта фильма.
// Склейка нескольких сегментов смещает время, поэтому пересчитываем таймкоды
// с учётом crossfade (длительность итога = сумма - (n-1)*xfade, как в ClipRenderer).
static QString s3SrtTime(double t)
{
    if (t < 0.0)
        t = 0.0;
    const qint64 ms = qint64(qRound64(t * 1000.0));
    const qint64 h = ms / 3600000;
    const qint64 m = (ms / 60000) % 60;
    const qint64 s = (ms / 1000) % 60;
    const qint64 mm = ms % 1000;
    return QString("%1:%2:%3,%4")
        .arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0')).arg(mm, 3, 10, QChar('0'));
}

QString PipelineManager::buildS3ClipSrt(const RenderTask& task, const QString& srtBase)
{
    // Слова после ревизора и фильтрации, общие с LLM-транскриптом.
    if (m_s3Words.isEmpty())
        loadS3Words();
    const QList<SrtCue>& words = m_s3Words;

    if (words.isEmpty() || task.segments.isEmpty())
        return QString();

    const double xfade = task.segments.size() > 1 ? storyTransitionSec() : 0.0;

    QString out;
    int idx = 0;
    double offset = 0.0;

    for (int si = 0; si < task.segments.size(); ++si) {
        const double a = task.segments[si].first;
        const double b = task.segments[si].second;
        const double dur = qMax(0.05, b - a);

        for (const SrtCue& c : words) {
            if (c.text.trimmed().isEmpty())
                continue;
            // слово должно попадать в сегмент
            if (c.endSec <= a || c.startSec >= b)
                continue;
            const double s0 = qMax(c.startSec, a) - a + offset;
            const double s1 = qMin(c.endSec, b) - a + offset;
            if (s1 - s0 < 0.02)
                continue;
            out += QString("%1\n%2 --> %3\n%4\n\n")
                       .arg(++idx)
                       .arg(s3SrtTime(s0), s3SrtTime(s1), c.text.trimmed());
        }

        // склейка: следующий сегмент начинается раньше на длину crossfade
        offset += dur;
        if (si + 1 < task.segments.size())
            offset -= xfade;
    }

    if (idx == 0)
        return QString();

    const QString path = srtBase + ".s3.srt";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(out.toUtf8());
    f.close();
    return path;
}

double PipelineManager::storyTransitionSec() const
{
    // Dialogue keeps its full timing. Brief edge audio fades are applied by the renderer.
    return m_mode == Mode::Stories3 ? 0.0 : AppSettings::instance().storyCrossfadeSec;
}

void PipelineManager::renderTaskWithSrt(const QString& srtPath)
{
    if (m_taskIndex < 0 || m_taskIndex >= m_renderTasks.size())
        return;
    const RenderTask& task = m_renderTasks[m_taskIndex];
    QString subtitlePath = srtPath;
    if (m_mode == Mode::Stories3) {
        subtitlePath = task.srtBase + ".readable.ass";
        const auto& settings = AppSettings::instance();
        QList<double> cuts;
        double position = 0;
        for (int i = 0; i + 1 < task.segments.size(); ++i) {
            position += task.segments[i].second - task.segments[i].first;
            cuts.append(position);
        }
        if (!SubtitleComposer::writeAss(SrtUtils::parseFile(srtPath), subtitlePath,
                                        settings.s3SubtitleFontSize, settings.s3SubtitleWords, cuts)) {
            onErrorOccurred("V3: не удалось сохранить оформленные субтитры.");
            return;
        }
    }

    if (task.isStory && task.segments.size() > 1) {
        emit logMessage(QString("Рендер пересказа %1 [%2]...")
                            .arg(task.clipIndex + 1).arg(task.lang));
        renderer.renderStoryClip(currentFilePath, task.segments, task.audioIndex,
                                 subtitlePath, task.outPath,
                                  storyTransitionSec(), m_mode == Mode::Stories3);
    } else {
        emit logMessage(QString("Рендер клипа %1 [%2]...")
                            .arg(task.clipIndex + 1).arg(task.lang));
        renderer.renderClip(currentFilePath, task.startSec, task.endSec, task.audioIndex,
                            subtitlePath, task.outPath, m_mode == Mode::Stories3);
    }
}

void PipelineManager::onClipRendered(const QString& outPath)
{
    if (!m_fileActive || m_taskIndex < 0 || m_taskIndex >= m_renderTasks.size()) return;
    if (outPath != m_renderTasks[m_taskIndex].outPath || m_runOutputs.contains(outPath)) return;
    if (!QFileInfo(outPath).isFile() || QFileInfo(outPath).size() <= 0) {
        onErrorOccurred("Рендер сообщил об успехе, но файл отсутствует или пуст: " + outPath); return;
    }
    m_runOutputs.append(outPath);
    if (m_renderTasks[m_taskIndex].isStory) m_renderedStoryIds.insert(m_renderTasks[m_taskIndex].clipIndex);
    emit logMessage("Клип готов: " + outPath);
    ++m_taskIndex;
    processNextRenderTask();
}

void PipelineManager::finishCurrentFile()
{
    if (!m_fileActive) return;
    m_fileActive = false;
    if (m_runOutputs.isEmpty() && m_runError.isEmpty()) m_runError = "Запуск не создал ни одного видеофайла.";
    if (m_runOutputs.size() < m_fileResult.expectedVideos && m_runError.isEmpty())
        m_runError = "Созданы не все запрошенные языковые версии.";
    m_fileResult.status = m_runOutputs.isEmpty() ? "failed" : (m_runError.isEmpty() ? "completed" : "partial");
    m_fileResult.error = m_runError; m_fileResult.outputs = m_runOutputs;
    m_fileResult.renderedStories = m_renderedStoryIds.size();
    auto result = m_fileResult.json();
    result["finished_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    llm.writeRunArtifact("result.json", result);
    if (m_mode == Mode::Stories3) {
        const bool saved = writeResultJson(m_runOutputDirectory + "/result.json", result)
            && writeResultJson(outDir + "/latest_attempt.json", result);
        if (!saved) emit logMessage("[ПРЕДУПРЕЖДЕНИЕ] Не удалось сохранить манифест результата; проверь logs/result.json.");
        if (!m_runOutputs.isEmpty() && !writeResultJson(outDir + "/latest_outputs.json", result))
            emit logMessage("[ПРЕДУПРЕЖДЕНИЕ] Не удалось обновить latest_outputs.json; пути готовых файлов указаны в логах.");
    }
    m_queueSummary.files.append(m_fileResult);
    emit logMessage(QString("Результат файла: %1. Новых историй: %2, видео: %3. %4")
        .arg(m_fileResult.status).arg(m_fileResult.renderedStories).arg(m_runOutputs.size()).arg(m_runError));
    if (!m_runError.isEmpty() && m_runOutputs.isEmpty())
        emit logMessage("Текущий прогон не создал новых роликов. Файлы прошлых прогонов остались в папке; "
                        "подробности ошибки и ответы LLM сохранены в logs.");
    m_audioPurpose = AudioPurpose::None;
    QTimer::singleShot(0, this, &PipelineManager::processNext);
}
