/* appsettings.cpp */

#include "appsettings.h"
#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>

AppSettings& AppSettings::instance()
{
    static AppSettings s;
    return s;
}

QString AppSettings::settingsFilePath() const
{
    return QCoreApplication::applicationDirPath() + "/settings-highlights-v5.json";
}

static QString subsToStr(AppSettings::SubsForLlm v)
{
    switch (v) {
    case AppSettings::SubsForLlm::RuOnly: return "ru";
    case AppSettings::SubsForLlm::EnOnly: return "en";
    case AppSettings::SubsForLlm::Both:   return "both";
    }
    return "both";
}

static AppSettings::SubsForLlm strToSubs(const QString& s)
{
    if (s == "ru") return AppSettings::SubsForLlm::RuOnly;
    if (s == "en") return AppSettings::SubsForLlm::EnOnly;
    return AppSettings::SubsForLlm::Both;
}

static QString asrToStr(AppSettings::AsrEngine v)
{
    switch (v) {
    case AppSettings::AsrEngine::WhisperCpp:    return "whispercpp";
    case AppSettings::AsrEngine::FasterWhisper: return "faster-whisper";
    case AppSettings::AsrEngine::GigaAmV3Ctc:   return "gigaam-v3-ctc";
    case AppSettings::AsrEngine::GigaAmV3E2e:   return "gigaam-v3-e2e";
    case AppSettings::AsrEngine::ParakeetV3:    return "parakeet-v3";
    }
    return "gigaam-v3-ctc";
}

static AppSettings::AsrEngine strToAsr(const QString& s)
{
    if (s == "whispercpp")     return AppSettings::AsrEngine::WhisperCpp;
    if (s == "faster-whisper") return AppSettings::AsrEngine::FasterWhisper;
    if (s == "gigaam-v3-e2e")  return AppSettings::AsrEngine::GigaAmV3E2e;
    if (s == "parakeet-v3")    return AppSettings::AsrEngine::ParakeetV3;
    return AppSettings::AsrEngine::GigaAmV3Ctc;
}

static QString audioMapToStr(AppSettings::AudioMapEngine v)
{
    switch (v) {
    case AppSettings::AudioMapEngine::Off:               return "off";
    case AppSettings::AudioMapEngine::Zipformer:         return "zipformer";
    case AppSettings::AudioMapEngine::InaSpeechSegmenter: return "ina";
    }
    return "ina";
}

static AppSettings::AudioMapEngine strToAudioMap(const QString& s)
{
    if (s == "off")       return AppSettings::AudioMapEngine::Off;
    if (s == "zipformer") return AppSettings::AudioMapEngine::Zipformer;
    return AppSettings::AudioMapEngine::InaSpeechSegmenter;
}

static QString diarToStr(AppSettings::DiarEngine v)
{
    switch (v) {
    case AppSettings::DiarEngine::Off:        return "off";
    case AppSettings::DiarEngine::SherpaOnnx: return "sherpa";
    case AppSettings::DiarEngine::Pyannote:   return "pyannote";
    }
    return "sherpa";
}

static AppSettings::DiarEngine strToDiar(const QString& s)
{
    if (s == "off")      return AppSettings::DiarEngine::Off;
    if (s == "pyannote") return AppSettings::DiarEngine::Pyannote;
    return AppSettings::DiarEngine::SherpaOnnx;
}

bool AppSettings::load(const QString& path)
{
    if (path.isEmpty() && !QFile::exists(settingsFilePath())) {
        const QString previous = QCoreApplication::applicationDirPath() + "/settings.json";
        if (QFile::exists(previous)) return load(previous) && save();
    }
    QFile f(path.isEmpty() ? settingsFilePath() : path);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    QJsonObject o = doc.object();
    ffprobePath      = o.value("ffprobePath").toString(ffprobePath);
    ffmpegPath       = o.value("ffmpegPath").toString(ffmpegPath);
    whisperCliPath   = o.value("whisperCliPath").toString(whisperCliPath);
    whisperModelPath = o.value("whisperModelPath").toString(whisperModelPath);
    whisperVadModelPath = o.value("whisperVadModelPath").toString(whisperVadModelPath);
    llmBaseUrl       = o.value("llmBaseUrl").toString(llmBaseUrl);
    llmApiKey        = o.value("llmApiKey").toString(llmApiKey);
    llmModel         = o.value("llmModel").toString(llmModel);
    llmMaxOutputTokens = qBound(1, o.value("llmMaxOutputTokens").toInt(llmMaxOutputTokens), 1000000);
    llmTimeoutMinutes = qBound(1, o.value("llmTimeoutMinutes").toInt(llmTimeoutMinutes), 60);
    subsForLlm       = strToSubs(o.value("subsForLlm").toString("both"));
    renderRu         = o.value("renderRu").toBool(renderRu);
    renderEn         = o.value("renderEn").toBool(renderEn);
    minClips         = o.value("minClips").toInt(minClips);
    maxClips         = o.value("maxClips").toInt(maxClips);
    minClipSec       = o.value("minClipSec").toInt(minClipSec);
    maxClipSec       = o.value("maxClipSec").toInt(maxClipSec);
    scoutChunkChars  = o.value("scoutChunkChars").toInt(scoutChunkChars);
    subtitleStyle    = o.value("subtitleStyle").toString(subtitleStyle);
    padStartSec      = o.value("padStartSec").toDouble(padStartSec);
    padEndSec        = o.value("padEndSec").toDouble(padEndSec);
    minSubsCuesPerHour = o.value("minSubsCuesPerHour").toInt(minSubsCuesPerHour);
    whisperDtw       = o.value("whisperDtw").toBool(whisperDtw);
    agentFramesEnabled   = o.value("agentFramesEnabled").toBool(agentFramesEnabled);
    agentFrameIntervalSec = o.value("agentFrameIntervalSec").toDouble(agentFrameIntervalSec);
    agentMaxTurns        = o.value("agentMaxTurns").toInt(agentMaxTurns);
    agentMaxFramesPerTurn = o.value("agentMaxFramesPerTurn").toInt(agentMaxFramesPerTurn);
    editPauseSec       = o.value("editPauseSec").toDouble(editPauseSec);
    editMaxPhraseSec   = o.value("editMaxPhraseSec").toDouble(editMaxPhraseSec);
    editPaddingMs      = o.value("editPaddingMs").toInt(editPaddingMs);
    editAudioFadeMs    = o.value("editAudioFadeMs").toInt(editAudioFadeMs);
    pythonPath         = o.value("pythonPath").toString(pythonPath);
    hfToken            = o.value("hfToken").toString(hfToken);
    pyannoteModel      = o.value("pyannoteModel").toString(pyannoteModel);

    // --- Пересказ 3.0 ---
    s3AsrEngine       = strToAsr(o.value("s3AsrEngine").toString(asrToStr(s3AsrEngine)));
    s3AudioMapEngine  = strToAudioMap(o.value("s3AudioMapEngine").toString(audioMapToStr(s3AudioMapEngine)));
    s3DiarEngine      = strToDiar(o.value("s3DiarEngine").toString(diarToStr(s3DiarEngine)));
    s3PythonPath      = o.value("s3PythonPath").toString(s3PythonPath);
    s3ScriptsDir      = o.value("s3ScriptsDir").toString(s3ScriptsDir);
    s3ModelDir        = o.value("s3ModelDir").toString(s3ModelDir);
    s3ChunkSec        = o.value("s3ChunkSec").toDouble(s3ChunkSec);
    s3Threads         = o.value("s3Threads").toInt(s3Threads);
    s3ReviewEnabled   = o.value("s3ReviewEnabled").toBool(s3ReviewEnabled);
    s3ReviewEngine    = strToAsr(o.value("s3ReviewEngine").toString(asrToStr(s3ReviewEngine)));
    s3ReviewChunkSec  = o.value("s3ReviewChunkSec").toDouble(s3ReviewChunkSec);
    s3ReviewMinAgree  = o.value("s3ReviewMinAgree").toDouble(s3ReviewMinAgree);
    s3DiarThreshold   = o.value("s3DiarThreshold").toDouble(s3DiarThreshold);
    s3SherpaDevice = o.value("s3SherpaDevice").toString() == "cuda" ? "cuda" : "cpu";
    s3DiarThreads     = o.value("s3DiarThreads").toInt(s3DiarThreads);
    s3DropMusicLowConf = o.value("s3DropMusicLowConf").toBool(s3DropMusicLowConf);
    s3MinLogprob      = o.value("s3MinLogprob").toDouble(s3MinLogprob);
    s3DropMusicHard   = o.value("s3DropMusicHard").toBool(s3DropMusicHard);
    s3Filmstrip       = o.value("s3Filmstrip").toBool(s3Filmstrip);
    s3QuoteCheck      = o.value("s3QuoteCheck").toBool(s3QuoteCheck);
    s3MinScore        = o.value("s3MinScore").toDouble(s3MinScore);
    s3RunMode = StoryRunMode(qBound(0, o.value("s3RunMode").toInt(int(s3RunMode)), 2));
    s3RequestLimit = qBound(1, o.value("s3RequestLimit").toInt(s3RequestLimit), 500);
    s3ReserveBranches = qBound(0, o.value("s3ReserveBranches").toInt(s3ReserveBranches), 5);
    s3BranchContextChars = qBound(5000, o.value("s3BranchContextChars").toInt(s3BranchContextChars), 100000);
    s3AllowPartial = o.value("s3AllowPartial").toBool(s3AllowPartial);
    s3MinClipSec      = o.value("s3MinClipSec").toDouble(s3MinClipSec);
    s3MaxClipSec      = o.value("s3MaxClipSec").toDouble(s3MaxClipSec);
    s3MaxSpanSec      = o.value("s3MaxSpanSec").toDouble(s3MaxSpanSec);
    s3MaxSegments     = o.value("s3MaxSegments").toInt(s3MaxSegments);
    s3MaxSegmentSec   = o.value("s3MaxSegmentSec").toDouble(s3MaxSegmentSec);
    s3SubtitleFontSize = o.value("s3SubtitleFontSize").toInt(s3SubtitleFontSize);
    s3SubtitleWords   = o.value("s3SubtitleWords").toInt(s3SubtitleWords);
    // Migrate only the old scene-mode defaults; keep custom duration choices.
    if (o.value("s3NarrativeVersion").toInt() < 1) {
        if (s3MinClipSec == 20.0) s3MinClipSec = 60.0;
        if (s3MaxClipSec == 90.0) s3MaxClipSec = 180.0;
        if (s3MaxSpanSec == 300.0) s3MaxSpanSec = 0.0;
        if (s3MaxSegments == 6) s3MaxSegments = 12;
    }
    return true;
}

bool AppSettings::save() const
{
    QJsonObject o;
    o["ffprobePath"]      = ffprobePath;
    o["ffmpegPath"]       = ffmpegPath;
    o["whisperCliPath"]   = whisperCliPath;
    o["whisperModelPath"] = whisperModelPath;
    o["whisperVadModelPath"] = whisperVadModelPath;
    o["llmBaseUrl"]       = llmBaseUrl;
    o["llmApiKey"]        = llmApiKey;
    o["llmModel"]         = llmModel;
    o["llmTimeoutMinutes"] = llmTimeoutMinutes;
    o["llmMaxOutputTokens"] = llmMaxOutputTokens;
    o["subsForLlm"]       = subsToStr(subsForLlm);
    o["renderRu"]         = renderRu;
    o["renderEn"]         = renderEn;
    o["minClips"]         = minClips;
    o["maxClips"]         = maxClips;
    o["minClipSec"]       = minClipSec;
    o["maxClipSec"]       = maxClipSec;
    o["scoutChunkChars"]  = scoutChunkChars;
    o["subtitleStyle"]    = subtitleStyle;
    o["padStartSec"]      = padStartSec;
    o["padEndSec"]        = padEndSec;
    o["minSubsCuesPerHour"] = minSubsCuesPerHour;
    o["whisperDtw"]       = whisperDtw;
    o["agentFramesEnabled"]   = agentFramesEnabled;
    o["agentFrameIntervalSec"] = agentFrameIntervalSec;
    o["agentMaxTurns"]        = agentMaxTurns;
    o["agentMaxFramesPerTurn"] = agentMaxFramesPerTurn;
    o["editPauseSec"]       = editPauseSec;
    o["editMaxPhraseSec"]   = editMaxPhraseSec;
    o["editPaddingMs"]      = editPaddingMs;
    o["editAudioFadeMs"]    = editAudioFadeMs;
    o["pythonPath"]         = pythonPath;
    o["hfToken"]            = hfToken;
    o["pyannoteModel"]      = pyannoteModel;

    // --- Пересказ 3.0 ---
    o["s3AsrEngine"]       = asrToStr(s3AsrEngine);
    o["s3AudioMapEngine"]  = audioMapToStr(s3AudioMapEngine);
    o["s3DiarEngine"]      = diarToStr(s3DiarEngine);
    o["s3PythonPath"]      = s3PythonPath;
    o["s3ScriptsDir"]      = s3ScriptsDir;
    o["s3ModelDir"]        = s3ModelDir;
    o["s3ChunkSec"]        = s3ChunkSec;
    o["s3Threads"]         = s3Threads;
    o["s3ReviewEnabled"]   = s3ReviewEnabled;
    o["s3ReviewEngine"]    = asrToStr(s3ReviewEngine);
    o["s3ReviewChunkSec"]  = s3ReviewChunkSec;
    o["s3ReviewMinAgree"]  = s3ReviewMinAgree;
    o["s3DiarThreshold"]   = s3DiarThreshold;
    o["s3SherpaDevice"] = s3SherpaDevice;
    o["s3DiarThreads"]     = s3DiarThreads;
    o["s3DropMusicLowConf"] = s3DropMusicLowConf;
    o["s3MinLogprob"]      = s3MinLogprob;
    o["s3DropMusicHard"]   = s3DropMusicHard;
    o["s3Filmstrip"]       = s3Filmstrip;
    o["s3QuoteCheck"]      = s3QuoteCheck;
    o["s3MinScore"]        = s3MinScore;
    o["s3RunMode"] = int(s3RunMode);
    o["s3RequestLimit"] = s3RequestLimit;
    o["s3ReserveBranches"] = s3ReserveBranches;
    o["s3BranchContextChars"] = s3BranchContextChars;
    o["s3AllowPartial"] = s3AllowPartial;
    o["s3MinClipSec"]      = s3MinClipSec;
    o["s3MaxClipSec"]      = s3MaxClipSec;
    o["s3MaxSpanSec"]      = s3MaxSpanSec;
    o["s3MaxSegments"]     = s3MaxSegments;
    o["s3MaxSegmentSec"]   = s3MaxSegmentSec;
    o["s3SubtitleFontSize"] = s3SubtitleFontSize;
    o["s3SubtitleWords"]   = s3SubtitleWords;
    o["s3NarrativeVersion"] = 1;

    QSaveFile f(settingsFilePath());
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const auto data = QJsonDocument(o).toJson(QJsonDocument::Indented);
    return f.write(data) == data.size() && f.commit();
}
