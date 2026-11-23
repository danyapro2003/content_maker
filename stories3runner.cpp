/* stories3runner.cpp */

#include "stories3runner.h"
#include "appsettings.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QDir>

Stories3Runner::Stories3Runner(QObject* parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &Stories3Runner::onTimeout);
}

QString Stories3Runner::pythonPath() const
{
    const AppSettings& s = AppSettings::instance();
    if (!s.s3PythonPath.isEmpty())
        return s.s3PythonPath;

    // дефолт: venv проекта рядом с репозиторием
    const QString guess =
        QStringLiteral("C:/Users/O/Documents/content_clipper/.venv-asr/Scripts/python.exe");
    return QFile::exists(guess) ? guess : QStringLiteral("python");
}

QString Stories3Runner::scriptPath(const QString& name) const
{
    const AppSettings& s = AppSettings::instance();
    if (!s.s3ScriptsDir.isEmpty())
        return s.s3ScriptsDir + "/" + name;

    // приложение запускается из build/... — скрипты копируются рядом с exe
    const QString nearExe = QCoreApplication::applicationDirPath() + "/scripts/" + name;
    if (QFile::exists(nearExe))
        return nearExe;

    const QString repo = QStringLiteral("C:/Users/O/Documents/content_clipper/scripts/") + name;
    return QFile::exists(repo) ? repo : QDir::currentPath() + "/scripts/" + name;
}

void Stories3Runner::runAudioMap(const QString& wavPath, const QString& outJson)
{
    const AppSettings& s = AppSettings::instance();
    QString engine = (s.s3AudioMapEngine == AppSettings::AudioMapEngine::Zipformer)
                         ? "zipformer" : "ina";
    QStringList args;
    args << scriptPath("audiomap.py")
         << "--wav" << wavPath
         << "--out" << outJson
         << "--engine" << engine;
    if (s.s3AudioMapEngine == AppSettings::AudioMapEngine::Zipformer)
        args << "--model-dir" << (s.s3ModelDir + "/sherpa-onnx-zipformer-small-audio-tagging-2024-04-15");
    start(S3Stage::AudioMap, args, outJson, 3600000);
}

void Stories3Runner::runAsr(const QString& wavPath, const QString& outJson,
                            const QString& engine, double chunkSec, const QString& language)
{
    const AppSettings& s = AppSettings::instance();
    QStringList args;
    args << scriptPath("asr_bridge.py")
         << "--engine" << engine
         << "--wav" << wavPath
         << "--out" << outJson
         << "--chunk" << QString::number(chunkSec, 'f', 1)
         << "--language" << language
         << "--whisper-cli" << s.whisperCliPath
         << "--whisper-model" << s.whisperModelPath
         << "--vad-model" << s.whisperVadModelPath;
    start(S3Stage::Asr, args, outJson, 7200000);
}

void Stories3Runner::runReview(const QString& wavPath, const QString& wordsJson,
                               const QString& outJson, const QString& engine,
                               double chunkSec, const QString& audioMapJson,
                               double minAgree, double minLogprob, const QString& language)
{
    const AppSettings& s = AppSettings::instance();
    QStringList args;
    args << scriptPath("review.py")
         << "--wav" << wavPath
         << "--words" << wordsJson
         << "--engine" << engine
         << "--out" << outJson
         << "--chunk" << QString::number(chunkSec, 'f', 1)
         << "--min-agree" << QString::number(minAgree, 'f', 2)
         << "--min-lp" << QString::number(minLogprob, 'f', 2);
    args << "--language" << language
         << "--whisper-cli" << s.whisperCliPath
         << "--whisper-model" << s.whisperModelPath
         << "--vad-model" << s.whisperVadModelPath;
    if (!audioMapJson.isEmpty())
        args << "--audiomap" << audioMapJson;
    start(S3Stage::Review, args, outJson, 7200000);
}

void Stories3Runner::runDiarizeSherpa(const QString& wavPath, const QString& outJson,
                                      double threshold, int threads)
{
    const AppSettings& s = AppSettings::instance();
    QStringList args;
    emit logMessage("Диаризация sherpa-onnx: " + s.s3SherpaDevice.toUpper());
    args << scriptPath("diarize_sherpa.py")
         << "--device" << s.s3SherpaDevice
         << "--wav" << wavPath
         << "--out" << outJson
         << "--threshold" << QString::number(threshold, 'f', 2)
         << "--threads" << QString::number(threads)
         << "--seg-model" << (s.s3ModelDir + "/sherpa-onnx-pyannote-segmentation-3-0/model.onnx")
         << "--emb-model" << (s.s3ModelDir + "/3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx");
    start(S3Stage::DiarizeSherpa, args, outJson, 3600000);
}

void Stories3Runner::runDiarizePyannote(const QString& wavPath, const QString& outJson,
                                        const QString& model, const QString& hfToken)
{
    // pyannote живёт в другом venv (.venv-diar) — путь берём из старых настроек
    const AppSettings& s = AppSettings::instance();
    QString py = s.pythonPath;
    if (py.isEmpty()) {
        const QString sibling = QFileInfo(pythonPath()).dir().absoluteFilePath(
            "../../.venv-diar/Scripts/python.exe");
        py = QFileInfo::exists(sibling) ? QDir::cleanPath(sibling) : pythonPath();
    }

    QStringList args;
    args << scriptPath("diarize.py")
         << wavPath
         << "--out" << outJson
         << "--model" << (model.isEmpty() ? QStringLiteral("pyannote/speaker-diarization-3.1") : model);
    if (!hfToken.isEmpty())
        args << "--hf-token" << hfToken;

    start(S3Stage::DiarizePyannote, args, outJson, 3600000, py);
}

void Stories3Runner::runMerge(const QString& wordsJson, const QString& outSrt,
                              const QString& packedMd, const QString& cleanWordsJson, const QString& audioMapJson,
                              const QString& diarJson, const QString& reviewJson,
                              double pauseSec, double maxPhraseSec,
                              bool dropMusicHard, bool dropMusicLowConf, double minLogprob)
{
    QStringList args;
    args << scriptPath("merge_transcript.py")
         << "--words" << wordsJson
         << "--out" << outSrt
         << "--packed" << packedMd
         << "--clean-words" << cleanWordsJson
         << "--pause" << QString::number(pauseSec, 'f', 2)
         << "--max-phrase" << QString::number(maxPhraseSec, 'f', 1)
         << "--min-lp" << QString::number(minLogprob, 'f', 2);
    if (!audioMapJson.isEmpty() && QFile::exists(audioMapJson))
        args << "--audiomap" << audioMapJson;
    if (!diarJson.isEmpty() && QFile::exists(diarJson))
        args << "--diar" << diarJson;
    if (!reviewJson.isEmpty() && QFile::exists(reviewJson))
        args << "--review" << reviewJson;
    if (dropMusicHard)
        args << "--drop-music";
    else if (dropMusicLowConf)
        args << "--drop-music-lowconf";
    start(S3Stage::Merge, args, outSrt, 1800000);
}

void Stories3Runner::start(S3Stage stage, const QStringList& args,
                           const QString& outPath, int timeoutMs, const QString& interpreter)
{
    if (m_proc) {
        emit failed(stage, "Уже выполняется другой шаг Пересказа 3.0.");
        return;
    }

    m_stage = stage;
    m_outPath = outPath;
    m_errBuffer.clear();

    QProcess* proc = new QProcess(this);
    m_proc = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(QFileInfo(scriptPath(".")).absolutePath());

    // UTF-8 в выводе python
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONUNBUFFERED", "1");
    env.insert("TF_CPP_MIN_LOG_LEVEL", "3");
    proc->setProcessEnvironment(env);

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus st) { onFinished(code, int(st)); });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, stage](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            QString err = QString("Не удалось запустить: %1").arg(proc->program());
            cancel();
            emit failed(stage, err);
        }
    });
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        const QString out = QString::fromUtf8(proc->readAllStandardOutput());
        m_errBuffer += out;
        // python пишет прогресс в stderr; в лог отдаём только строки-маркеры "[...]"
        for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
            const QString t = line.trimmed();
            if (t.startsWith('['))
                emit logMessage("[s3] " + t);
        }
    });

    emit logMessage(QString("Пересказ 3.0: python %1 ...").arg(args.first()));
    proc->start(interpreter.isEmpty() ? pythonPath() : interpreter, args);

    if (timeoutMs > 0)
        m_timeout.start(timeoutMs);
}

void Stories3Runner::onFinished(int exitCode, int exitStatus)
{
    QProcess* proc = m_proc;
    if (!proc)
        return;
    m_timeout.stop();

    QString out = QString::fromUtf8(proc->readAll());
    m_errBuffer += out;

    S3Stage stage = m_stage;
    QString outPath = m_outPath;

    proc->deleteLater();
    m_proc = nullptr;
    m_stage = S3Stage::None;

    bool ok = (exitStatus == QProcess::ExitStatus(int(QProcess::NormalExit))
               && exitCode == 0 && QFile::exists(outPath));

    if (!ok) {
        QString tail = m_errBuffer.right(800).trimmed();
        emit failed(stage, QString("Код %1. %2").arg(exitCode).arg(tail));
        return;
    }
    emit finished(stage, outPath);
}

void Stories3Runner::cancel()
{
    m_timeout.stop();
    if (!m_proc)
        return;
    QProcess* proc = m_proc;
    m_proc = nullptr;
    proc->disconnect(this);
    proc->kill();
    proc->waitForFinished(3000);
    proc->deleteLater();
    m_stage = S3Stage::None;
}

void Stories3Runner::onTimeout()
{
    if (!m_proc)
        return;
    S3Stage stage = m_stage;
    cancel();
    emit failed(stage, "Превышено время ожидания скрипта — процесс остановлен.");
}

// ---------------------------------------------------------------- разбор

static bool readJson(const QString& path, QJsonDocument& doc)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError err;
    doc = QJsonDocument::fromJson(f.readAll(), &err);
    return err.error == QJsonParseError::NoError;
}

QList<S3Word> Stories3Runner::parseWords(const QString& jsonPath)
{
    QList<S3Word> out;
    QJsonDocument doc;
    if (!readJson(jsonPath, doc))
        return out;

    const QJsonArray arr = doc.object().value("words").toArray();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        S3Word w;
        w.text = o.value("w").toString();
        w.startSec = o.value("t").toDouble();
        w.endSec = o.value("e").toDouble();
        w.chunk = o.value("chunk").toInt();
        if (o.contains("lp") && !o.value("lp").isNull()) {
            w.logprob = o.value("lp").toDouble();
            w.hasLogprob = true;
        }
        if (w.text.isEmpty() || w.endSec < w.startSec)
            continue;
        out.append(w);
    }
    return out;
}

QList<S3AudioSeg> Stories3Runner::parseAudioMap(const QString& jsonPath)
{
    QList<S3AudioSeg> out;
    QJsonDocument doc;
    if (!readJson(jsonPath, doc))
        return out;

    const QJsonArray arr = doc.object().value("segments").toArray();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        S3AudioSeg s;
        s.startSec = o.value("start").toDouble();
        s.endSec = o.value("end").toDouble();
        s.label = o.value("label").toString();
        if (s.endSec > s.startSec)
            out.append(s);
    }
    return out;
}

QList<S3Phrase> Stories3Runner::parsePacked(const QString& mdPath)
{
    // формат строки:
    //   [12.34-56.78] S0 текст
    //   [12.34-56.78] S0 текст  ?
    //   [12.34-56.78] S0 текст  [проверить]
    //   [12.34-56.78] (музыка, 15.7 с)
    QList<S3Phrase> out;
    QFile f(mdPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;

    static const QRegularExpression re(
        QStringLiteral("^\\[(\\d+(?:\\.\\d+)?)-(\\d+(?:\\.\\d+)?)\\]\\s*(.*)$"));

    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty())
            continue;
        const auto m = re.match(line);
        if (!m.hasMatch())
            continue;

        S3Phrase p;
        p.id = out.size() + 1;
        p.startSec = m.captured(1).toDouble();
        p.endSec = m.captured(2).toDouble();
        QString rest = m.captured(3);

        if (rest.startsWith('(')) {
            p.marker = true;
            p.text = rest;
            out.append(p);
            continue;
        }

        // метка спикера S0/S1
        static const QRegularExpression reSpk(QStringLiteral("^(S\\d+)\\s+"));
        const auto ms = reSpk.match(rest);
        if (ms.hasMatch()) {
            p.speaker = ms.captured(1);
            rest = rest.mid(ms.capturedLength());
        }

        if (rest.endsWith("[проверить]")) {
            p.flagCheck = true;
            rest.chop(QStringLiteral("[проверить]").size());
            rest = rest.trimmed();
        }
        if (rest.endsWith('?')) {
            // '?' добавляется через два пробела
            if (rest.endsWith("  ?")) {
                p.uncertain = true;
                rest.chop(3);
                rest = rest.trimmed();
            }
        }

        p.text = rest.trimmed();
        if (!p.text.isEmpty())
            out.append(p);
    }
    return out;
}

QJsonObject Stories3Runner::parseReview(const QString& jsonPath)
{
    QJsonDocument doc;
    if (!readJson(jsonPath, doc))
        return QJsonObject();
    return doc.object();
}

QJsonObject Stories3Runner::parseQuality(const QString& wordsJsonPath)
{
    QJsonDocument doc;
    if (!readJson(wordsJsonPath, doc))
        return QJsonObject();
    return doc.object().value("quality").toObject();
}
