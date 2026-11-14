#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#include <QString>

class AppSettings
{
public:
    enum class SubsForLlm { RuOnly, EnOnly, Both };

    static AppSettings& instance();

    bool load(const QString& path = QString());
    bool save() const;
    QString settingsFilePath() const;

    // Пути к инструментам
    QString ffprobePath = "ffprobe";
    QString ffmpegPath = "ffmpeg";
    QString whisperCliPath;
    QString whisperModelPath;
    QString whisperVadModelPath; // ggml-silero (для --vad)

    // LLM (OpenAI-совместимый API)
    QString llmBaseUrl = "https://api.openai.com/v1";
    QString llmApiKey;
    QString llmModel = "gpt-4o-mini";
    int llmTimeoutMinutes = 15;

    // Какие субтитры отправлять в LLM
    SubsForLlm subsForLlm = SubsForLlm::Both;

    // Языки рендера клипов
    bool renderRu = true;
    bool renderEn = true;

    // Ограничения для отбора клипов
    int minClips = 3;
    int maxClips = 7;
    int minClipSec = 40;
    int maxClipSec = 120;

    // Макс. размер текста субтитров (в символах) на один запрос Скаута
    int scoutChunkChars = 60000;

    // Стиль прожигаемых субтитров (ASS force_style)
    QString subtitleStyle = "FontName=Segoe UI,FontSize=14,Bold=0,"
                            "PrimaryColour=&H00FFFFFF,Outline=0,Shadow=0,"
                            "Alignment=2,MarginV=80";

    // Снаппинг границ сегментов к репликам
    double padStartSec = 0.2;    // запас до начала первой реплики
    double padEndSec = 0.4;      // запас после конца последней реплики
    double mergeGapSec = 1.0;    // склейка сегментов с зазором меньше этого

    // Отбраковка forced-сабов по содержимому: мин. реплик в час (0 = проверка выкл).
    // Полные сабы: 500–1500/час, forced: обычно < 50/час.
    int minSubsCuesPerHour = 100;

    // Точное выравнивание word-сабов (DTW, требует -nfa)
    bool whisperDtw = true;

    // Агентный доступ к кадрам (пересказ 2.0): LLM сама запрашивает фрагменты,
    // которые хочет посмотреть, вместо готовой нарезки всего фильма
    bool agentFramesEnabled = true;
    double agentFrameIntervalSec = 2.0; // плотность кадров в запрошенном фрагменте
    int agentMaxTurns = 6;              // сколько запросов кадров может сделать LLM
    int agentMaxFramesPerTurn = 40;     // лимит кадров на один запрос

    // Кроссфейд между склейками в пересказе (0 = выкл)
    double storyCrossfadeSec = 0.25;

    // --- Пересказ 2.0 ---
    // Источник транскрипта для LLM: true = whisper word-level по всему фильму
    // (точно, медленно, с кэшем), false = субтитры из файла (быстро, точность до реплики)
    bool storyWhisperTranscript = true;
    double editPauseSec = 0.5;   // разрыв фразы при упаковке транскрипта
    double editMaxPhraseSec = 12.0; // макс. длина фразы (в плотных диалогах пауз почти нет)
    int editPaddingMs = 80;      // паддинг границ склейки (whisper-режим)
    int editAudioFadeMs = 30;    // аудио-фейды на границах склеек (0 = выкл)

    // Диаризация через pyannote (опционально, требует python venv + HF токен)
    bool diarizationEnabled = false;
    QString pythonPath;          // путь к python.exe из venv
    QString hfToken;             // Hugging Face токен (модели pyannote gated)
    QString pyannoteModel = "pyannote/speaker-diarization-3.1";

    // ---------------- Пересказ 3.0 ----------------
    // Отдельный режим: чистый транскрипт (карта музыки + чанки + ревизор),
    // выбор движков ASR/диаризации. Пересказ 2.0 не затрагивается.
    enum class AsrEngine { WhisperCpp, FasterWhisper, GigaAmV3Ctc, GigaAmV3E2e, ParakeetV3 };
    enum class AudioMapEngine { Off, Zipformer, InaSpeechSegmenter };
    enum class DiarEngine { Off, SherpaOnnx, Pyannote };

    AsrEngine s3AsrEngine = AsrEngine::GigaAmV3Ctc;
    AudioMapEngine s3AudioMapEngine = AudioMapEngine::InaSpeechSegmenter;
    DiarEngine s3DiarEngine = DiarEngine::SherpaOnnx;

    QString s3PythonPath;        // .venv-asr\Scripts\python.exe
    QString s3ScriptsDir;        // <repo>/scripts
    QString s3ModelDir = "D:/ai_models/audio";
    double s3ChunkSec = 20.0;    // длина чанка для ASR (не больше 25!)
    int s3Threads = 4;

    // ревизор: второй движок проверяет весь фильм чанками
    bool s3ReviewEnabled = true;
    AsrEngine s3ReviewEngine = AsrEngine::ParakeetV3;
    double s3ReviewChunkSec = 30.0;
    double s3ReviewMinAgree = 0.60;

    // диаризация sherpa-onnx: порог кластеризации (БОЛЬШЕ = МЕНЬШЕ спикеров!)
    double s3DiarThreshold = 1.2;
    // потоки CPU — главный рычаг скорости (GPU в sherpa не помогает, проверено)
    int s3DiarThreads = 12;

    // фильтрация галлюцинаций
    bool s3DropMusicLowConf = true;  // music + низкий logprob -> выбросить
    double s3MinLogprob = -0.55;
    bool s3DropMusicHard = false;    // вырезать ВСЮ музыку (осторожно: режет диалог)

    // агент v3
    bool s3Filmstrip = true;         // сетка кадров вместо 40 отдельных JPEG
    bool s3QuoteCheck = true;        // сверка цитат с транскриптом
    double s3MinScore = 60.0;        // порог рубрики (0 = выкл)
    double s3MinClipSec = 60.0;
    double s3MaxClipSec = 180.0;
    double s3MaxSpanSec = 0.0;       // 0: ветка может охватывать весь фильм
    int s3MaxSegments = 12;
    double s3MaxSegmentSec = 35.0; // preferred block length, not a hard rejection limit
    int s3SubtitleFontSize = 58;  // pixels at 1080x1920
    int s3SubtitleWords = 4;
    enum class StoryRunMode { Continue, RebuildStories, FullAnalysis };
    StoryRunMode s3RunMode = StoryRunMode::RebuildStories;
    int s3RequestLimit = 80;
    int s3ReserveBranches = 2;
    int s3BranchContextChars = 40000;
    int s3TimestampOverlapMs = 80;
    bool s3AllowPartial = true;
    bool s3AllowUnknownRetry = false; // explicit, one-run UI choice; never persisted

private:
    AppSettings() = default;
};

#endif // APPSETTINGS_H
