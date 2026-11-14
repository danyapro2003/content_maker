#include "storynarrative.h"
/* stories3pipeline.cpp */

#include "stories3pipeline.h"
#include "appsettings.h"
#include "srtutils.h"
#include "s3cache.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>

QString s3AsrEngineName(int engine)
{
    switch (AppSettings::AsrEngine(engine)) {
    case AppSettings::AsrEngine::WhisperCpp:    return "whisper.cpp";
    case AppSettings::AsrEngine::FasterWhisper: return "faster-whisper";
    case AppSettings::AsrEngine::GigaAmV3Ctc:   return "GigaAM v3 CTC";
    case AppSettings::AsrEngine::GigaAmV3E2e:   return "GigaAM v3 e2e";
    case AppSettings::AsrEngine::ParakeetV3:    return "Parakeet v3";
    }
    return "?";
}

QString s3AudioMapEngineName(int engine)
{
    switch (AppSettings::AudioMapEngine(engine)) {
    case AppSettings::AudioMapEngine::Off:               return "выкл";
    case AppSettings::AudioMapEngine::Zipformer:         return "zipformer";
    case AppSettings::AudioMapEngine::InaSpeechSegmenter: return "inaSpeechSegmenter";
    }
    return "?";
}

QString s3DiarEngineName(int engine)
{
    switch (AppSettings::DiarEngine(engine)) {
    case AppSettings::DiarEngine::Off:        return "выкл";
    case AppSettings::DiarEngine::SherpaOnnx: return "sherpa-onnx";
    case AppSettings::DiarEngine::Pyannote:   return "pyannote";
    }
    return "?";
}

Stories3Pipeline::Stories3Pipeline(QObject* parent)
    : QObject(parent)
{
    connect(&m_runner, &Stories3Runner::logMessage, this, &Stories3Pipeline::logMessage);
    connect(&m_runner, &Stories3Runner::finished, this, &Stories3Pipeline::onRunnerFinished);
    connect(&m_runner, &Stories3Runner::failed, this, &Stories3Pipeline::onRunnerFailed);
}

QString Stories3Pipeline::cached(const QString& name) const
{
    return m_tempDir + "/" + name;
}

QString Stories3Pipeline::cacheKey(const QString& cachePath) const
{
    const AppSettings& s = AppSettings::instance();
    QJsonObject params{{"language", m_language}, {"python", S3Cache::fileFingerprint(s.s3PythonPath)}};
    QStringList deps{m_wavPath, m_wavPath + ".srcmeta", m_runner.scriptPath("gpu_env.py")};
    if (cachePath == m_audioMapJson) {
        params["engine"] = int(s.s3AudioMapEngine);
        params["modelDir"] = s.s3ModelDir;
        const QString dir = s.s3ModelDir + "/sherpa-onnx-zipformer-small-audio-tagging-2024-04-15/";
        deps << m_runner.scriptPath("audiomap.py") << dir + "model.onnx" << dir + "model.int8.onnx";
    } else if (cachePath == m_wordsJson || cachePath == m_reviewJson) {
        params["asrEngine"] = int(s.s3AsrEngine);
        params["chunkSec"] = s.s3ChunkSec;
        params["whisperCli"] = s.whisperCliPath;
        params["whisperModel"] = s.whisperModelPath;
        params["vadModel"] = s.whisperVadModelPath;
        deps << m_runner.scriptPath("asr_bridge.py") << s.whisperCliPath
             << s.whisperModelPath << s.whisperVadModelPath;
        if (cachePath == m_reviewJson) {
            params["reviewEngine"] = int(s.s3ReviewEngine);
            params["reviewChunk"] = s.s3ReviewChunkSec;
            params["minAgree"] = s.s3ReviewMinAgree;
            params["minLogprob"] = s.s3MinLogprob;
            deps << m_wordsJson << m_runner.scriptPath("review.py");
            if (m_haveAudioMap) deps << m_audioMapJson;
        }
    } else if (cachePath == m_diarJson) {
        params["engine"] = int(s.s3DiarEngine);
        params["threshold"] = s.s3DiarThreshold;
        params["threads"] = s.s3DiarThreads;
        params["model"] = s.pyannoteModel;
        params["pythonDiar"] = S3Cache::fileFingerprint(s.pythonPath);
        deps << m_runner.scriptPath("diarize.py") << m_runner.scriptPath("diarize_sherpa.py")
             << s.s3ModelDir + "/sherpa-onnx-pyannote-segmentation-3-0/model.onnx"
             << s.s3ModelDir + "/3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx";
    } else {
        params["pause"] = s.editPauseSec;
        params["maxPhrase"] = s.editMaxPhraseSec;
        params["dropMusic"] = s.s3DropMusicHard;
        params["dropMusicLowConf"] = s.s3DropMusicLowConf;
        params["minLogprob"] = s.s3MinLogprob;
        deps << m_wordsJson << m_runner.scriptPath("merge_transcript.py");
        if (m_haveAudioMap) deps << m_audioMapJson;
        if (m_haveReview) deps << m_reviewJson;
        if (m_haveDiar) deps << m_diarJson;
    }
    return S3Cache::makeKey(m_videoPath, params, deps);
}

bool Stories3Pipeline::cacheValid(const QString& cachePath) const
{
    return S3Cache::valid(cachePath, cacheKey(cachePath));
}

void Stories3Pipeline::writeCacheMeta(const QString& cachePath) const
{
    S3Cache::writeMeta(cachePath, cacheKey(cachePath));
}

void Stories3Pipeline::start(const QString& videoPath, const MediaInfo& info,
                             const QString& outDir, const QString& tempDir,
                             const QString& wavPath, const QString& language)
{
    m_videoPath = videoPath;
    m_info = info;
    m_outDir = outDir;
    m_tempDir = tempDir;
    m_wavPath = wavPath;
    m_language = language;

    m_audioMapJson = cached("s3_audiomap.json");
    m_wordsJson    = cached("s3_words.json");
    m_cleanWordsJson = cached("s3_clean_words.json");
    m_reviewJson   = cached("s3_review.json");
    m_diarJson     = cached("s3_diar.json");
    m_speechSrt    = cached("s3_speech.srt");
    m_packedMd     = cached("s3_packed.md");

    m_haveAudioMap = m_haveReview = m_haveDiar = false;
    m_quality = QJsonObject();
    m_audioSegs.clear();
    m_phrases.clear();

    const AppSettings& s = AppSettings::instance();
    emit logMessage(QString("Пересказ 3.0: движок ASR %1, карта музыки %2, диаризация %3")
                        .arg(s3AsrEngineName(int(s.s3AsrEngine)))
                        .arg(s3AudioMapEngineName(int(s.s3AudioMapEngine)))
                        .arg(s3DiarEngineName(int(s.s3DiarEngine))));

    runAudioMapOrSkip();
}

void Stories3Pipeline::runAudioMapOrSkip()
{
    const AppSettings& s = AppSettings::instance();

    if (s.s3AudioMapEngine == AppSettings::AudioMapEngine::Off) {
        emit logMessage("Пересказ 3.0: карта музыки отключена.");
        runAsr();
        return;
    }

    if (cacheValid(m_audioMapJson)) {
        emit logMessage("Карта аудио из кэша: " + m_audioMapJson);
        m_audioSegs = Stories3Runner::parseAudioMap(m_audioMapJson);
        m_haveAudioMap = true;
        runAsr();
        return;
    }

    emit logMessage("Пересказ 3.0: строю карту аудио (где речь, где музыка)...");
    m_runner.runAudioMap(m_wavPath, m_audioMapJson);
}

void Stories3Pipeline::runAsr()
{
    const AppSettings& s = AppSettings::instance();

    if (cacheValid(m_wordsJson)) {
        emit logMessage("Транскрипт из кэша: " + m_wordsJson);
        m_quality = Stories3Runner::parseQuality(m_wordsJson);
        runReviewOrDiarize();
        return;
    }

    emit logMessage(QString("Пересказ 3.0: транскрибация чанками по %1 с "
                            "(изоляция чанков = защита от зацикливания)...")
                        .arg(s.s3ChunkSec, 0, 'f', 0));

    auto engineToStr = [](AppSettings::AsrEngine e) -> QString {
        switch (e) {
        case AppSettings::AsrEngine::WhisperCpp:    return "whispercpp";
        case AppSettings::AsrEngine::FasterWhisper: return "faster-whisper";
        case AppSettings::AsrEngine::GigaAmV3Ctc:   return "gigaam-v3-ctc";
        case AppSettings::AsrEngine::GigaAmV3E2e:   return "gigaam-v3-e2e";
        case AppSettings::AsrEngine::ParakeetV3:    return "parakeet-v3";
        }
        return "gigaam-v3-ctc";
    };

    QString engine = engineToStr(s.s3AsrEngine);
    if (m_language != "ru" && engine.startsWith("gigaam")) {
        engine = "parakeet-v3";
        emit logMessage("Для нерусской дорожки использую Parakeet вместо GigaAM.");
    }
    m_runner.runAsr(m_wavPath, m_wordsJson, engine, s.s3ChunkSec, m_language);
}

void Stories3Pipeline::runReviewOrDiarize()
{
    const AppSettings& s = AppSettings::instance();

    // Отчёт качества — в лог
    emit logMessage(QString("Пересказ 3.0: слов %1, уникальных %2, растянутых %3, "
                            "повторов чанков %4")
                        .arg(m_quality.value("n_words").toInt())
                        .arg(m_quality.value("unique_ratio").toDouble(), 0, 'f', 3)
                        .arg(m_quality.value("stretched_words").toInt())
                        .arg(m_quality.value("duplicate_chunks").toInt()));

    // жёсткая проверка на вырожденный транскрипт
    const int nWords = m_quality.value("n_words").toInt();
    const double uniq = m_quality.value("unique_ratio").toDouble(1.0);
    if (nWords == 0 || (nWords >= 100 && uniq < 0.02)) {
        emit failed(QString("Транскрипт вырожденный: %1 слов, уникальных %2% — "
                            "движок зациклился. Попробуй другой движок ASR.")
                        .arg(nWords).arg(uniq * 100.0, 0, 'f', 2));
        return;
    }

    if (!s.s3ReviewEnabled) {
        runDiarizeOrMerge();
        return;
    }

    if (cacheValid(m_reviewJson)) {
        emit logMessage("Ревизор из кэша: " + m_reviewJson);
        m_haveReview = true;
        runDiarizeOrMerge();
        return;
    }

    auto engineToStr = [](AppSettings::AsrEngine e) -> QString {
        switch (e) {
        case AppSettings::AsrEngine::WhisperCpp:    return "whispercpp";
        case AppSettings::AsrEngine::FasterWhisper: return "faster-whisper";
        case AppSettings::AsrEngine::GigaAmV3Ctc:   return "gigaam-v3-ctc";
        case AppSettings::AsrEngine::GigaAmV3E2e:   return "gigaam-v3-e2e";
        case AppSettings::AsrEngine::ParakeetV3:    return "parakeet-v3";
        }
        return "parakeet-v3";
    };

    emit logMessage(QString("Пересказ 3.0: ревизор (%1) проверяет весь фильм "
                            "чанками по %2 с...")
                        .arg(s3AsrEngineName(int(s.s3ReviewEngine)))
                        .arg(s.s3ReviewChunkSec, 0, 'f', 0));

    m_runner.runReview(m_wavPath, m_wordsJson, m_reviewJson,
                       engineToStr(s.s3ReviewEngine), s.s3ReviewChunkSec,
                       m_haveAudioMap ? m_audioMapJson : QString(),
                       s.s3ReviewMinAgree, s.s3MinLogprob, m_language);
}

void Stories3Pipeline::runDiarizeOrMerge()
{
    const AppSettings& s = AppSettings::instance();

    if (s.s3DiarEngine == AppSettings::DiarEngine::Off) {
        runMerge();
        return;
    }

    if (cacheValid(m_diarJson)) {
        emit logMessage("Диаризация из кэша: " + m_diarJson);
        m_haveDiar = true;
        runMerge();
        return;
    }

    if (s.s3DiarEngine == AppSettings::DiarEngine::SherpaOnnx) {
        emit logMessage("Пересказ 3.0: диаризация sherpa-onnx (без HF-токена)...");
        m_runner.runDiarizeSherpa(m_wavPath, m_diarJson, s.s3DiarThreshold, s.s3DiarThreads);
    } else {
        emit logMessage("Пересказ 3.0: диаризация pyannote...");
        m_runner.runDiarizePyannote(m_wavPath, m_diarJson, s.pyannoteModel, s.hfToken);
    }
}

void Stories3Pipeline::runMerge()
{
    const AppSettings& s = AppSettings::instance();

    if (cacheValid(m_speechSrt) && cacheValid(m_packedMd) && cacheValid(m_cleanWordsJson)) {
        emit logMessage("Сшитый транскрипт из кэша.");
        startLlm();
        return;
    }

    emit logMessage("Пересказ 3.0: сшиваю транскрипт (фразы, спикеры, маркеры музыки)...");
    m_runner.runMerge(m_wordsJson, m_speechSrt, m_packedMd, m_cleanWordsJson,
                      m_haveAudioMap ? m_audioMapJson : QString(),
                      m_haveDiar ? m_diarJson : QString(),
                      m_haveReview ? m_reviewJson : QString(),
                      s.editPauseSec, s.editMaxPhraseSec,
                      s.s3DropMusicHard, s.s3DropMusicLowConf, s.s3MinLogprob);
}

void Stories3Pipeline::startLlm()
{
    const QList<S3Phrase> phrases = Stories3Runner::parsePacked(m_packedMd);
    if (phrases.isEmpty()) {
        emit failed("Не удалось прочитать упакованный транскрипт.");
        return;
    }
    m_phrases = phrases;

    const QString packed = StoryNarrative::packedTranscript(phrases);

    emit logMessage(QString("Пересказ 3.0: транскрипт для LLM — %1 фраз, %2 символов.")
                        .arg(phrases.size())
                        .arg(packed.size()));

    if (!m_llm) {
        emit failed("LLM-клиент не подключён.");
        return;
    }

    m_llm->findStoriesV3(m_info, packed, phrases);
}

void Stories3Pipeline::cancel()
{
    m_runner.cancel();
}

void Stories3Pipeline::onRunnerFinished(S3Stage stage, const QString& outPath)
{
    switch (stage) {
    case S3Stage::AudioMap:
        writeCacheMeta(m_audioMapJson);
        m_audioSegs = Stories3Runner::parseAudioMap(m_audioMapJson);
        m_haveAudioMap = true;
        emit logMessage(QString("Карта аудио готова: %1 сегментов.").arg(m_audioSegs.size()));
        runAsr();
        break;

    case S3Stage::Asr: {
        writeCacheMeta(m_wordsJson);
        m_quality = Stories3Runner::parseQuality(m_wordsJson);
        const int n = m_quality.value("n_words").toInt();
        emit logMessage(QString("Транскрипт готов: %1 слов.").arg(n));
        if (n == 0) {
            emit failed("Транскрипт пуст — ASR ничего не распознал.");
            return;
        }
        runReviewOrDiarize();
        break;
    }

    case S3Stage::Review: {
        writeCacheMeta(m_reviewJson);
        m_haveReview = true;
        const QJsonObject rev = Stories3Runner::parseReview(m_reviewJson);
        emit logMessage(QString("Ревизор: согласие %1%, подозрительных чанков %2 из %3.")
                            .arg(rev.value("agreement_avg").toDouble() * 100.0, 0, 'f', 1)
                            .arg(rev.value("n_suspicious").toInt())
                            .arg(rev.value("n_chunks").toInt()));
        runDiarizeOrMerge();
        break;
    }

    case S3Stage::DiarizeSherpa:
    case S3Stage::DiarizePyannote: {
        writeCacheMeta(m_diarJson);
        m_haveDiar = true;
        emit logMessage("Диаризация готова: " + outPath);
        runMerge();
        break;
    }

    case S3Stage::Merge:
        writeCacheMeta(m_speechSrt);
        writeCacheMeta(m_packedMd);
        writeCacheMeta(m_cleanWordsJson);
        startLlm();
        break;

    default:
        break;
    }
}

void Stories3Pipeline::onRunnerFailed(S3Stage stage, const QString& error)
{
    QString name;
    switch (stage) {
    case S3Stage::AudioMap:       name = "карта аудио"; break;
    case S3Stage::Asr:            name = "транскрибация"; break;
    case S3Stage::Review:         name = "ревизор"; break;
    case S3Stage::DiarizeSherpa:
    case S3Stage::DiarizePyannote: name = "диаризация"; break;
    case S3Stage::Merge:          name = "сшивка"; break;
    default:                      name = "шаг"; break;
    }
    emit failed(QString("Пересказ 3.0 (%1): %2").arg(name, error));
}
