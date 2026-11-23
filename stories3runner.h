#ifndef STORIES3RUNNER_H
#define STORIES3RUNNER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

// Стадия работы python-скрипта Пересказа 3.0
enum class S3Stage {
    None,
    AudioMap,      // audiomap.py   -> audiomap.json
    Asr,           // asr_bridge.py -> words.json
    Review,        // review.py     -> review.json
    DiarizeSherpa, // diarize_sherpa.py -> diar.json
    DiarizePyannote, // diarize.py   -> diar.json
    Merge          // merge_transcript.py -> speech.srt + packed.md
};

// Слово из words.json
struct S3Word {
    QString text;
    double startSec = 0.0;
    double endSec = 0.0;
    int chunk = 0;
    double logprob = 0.0;
    bool hasLogprob = false;
};

// Фраза из packed.md (то, что видит LLM)
struct S3Phrase {
    int id = 0;       // stable reference in this transcript, including markers
    double startSec = 0.0;
    double endSec = 0.0;
    QString speaker;   // "S0"/"" — пусто у маркеров
    QString text;
    bool uncertain = false;  // низкий logprob
    bool flagCheck = false;  // ревизор пометил
    bool marker = false;     // (музыка, N с)
};

// Отрезок карты аудио
struct S3AudioSeg {
    double startSec = 0.0;
    double endSec = 0.0;
    QString label;   // speech / music / noise / silence
};

// Запуск python-скриптов Пересказа 3.0 через QProcess, с таймаутом и Kill().
// Один процесс за раз: скрипты тяжёлые (GPU/память), параллелить нельзя.
class Stories3Runner : public QObject
{
    Q_OBJECT

public:
    explicit Stories3Runner(QObject* parent = nullptr);

    // wavPath — 16 кГц моно; outBase — путь без расширения для выходного json
    void runAudioMap(const QString& wavPath, const QString& outJson);
    void runAsr(const QString& wavPath, const QString& outJson,
                const QString& engine, double chunkSec, const QString& language);
    void runReview(const QString& wavPath, const QString& wordsJson,
                   const QString& outJson, const QString& engine,
                   double chunkSec, const QString& audioMapJson,
                   double minAgree, double minLogprob, const QString& language);
    void runDiarizeSherpa(const QString& wavPath, const QString& outJson,
                          double threshold, int threads);
    void runDiarizePyannote(const QString& wavPath, const QString& outJson,
                            const QString& model, const QString& hfToken);
    void runMerge(const QString& wordsJson, const QString& outSrt,
                  const QString& packedMd, const QString& cleanWordsJson, const QString& audioMapJson,
                  const QString& diarJson, const QString& reviewJson,
                  double pauseSec, double maxPhraseSec,
                  bool dropMusicHard, bool dropMusicLowConf, double minLogprob);

    bool isRunning() const { return m_proc != nullptr; }
    void cancel();

    // разбор результатов
    static QList<S3Word> parseWords(const QString& jsonPath);
    static QList<S3AudioSeg> parseAudioMap(const QString& jsonPath);
    static QList<S3Phrase> parsePacked(const QString& mdPath);
    static QJsonObject parseReview(const QString& jsonPath);
    static QJsonObject parseQuality(const QString& wordsJsonPath);
    QString scriptPath(const QString& name) const;

signals:
    void logMessage(const QString& msg);
    void finished(S3Stage stage, const QString& outPath);
    void failed(S3Stage stage, const QString& error);

private:
    void start(S3Stage stage, const QStringList& args, const QString& outPath,
               int timeoutMs, const QString& interpreter = {});
    void onFinished(int exitCode, int exitStatus);
    void onTimeout();

    QProcess* m_proc = nullptr;
    S3Stage m_stage = S3Stage::None;
    QString m_outPath;
    QString m_errBuffer;
    QTimer m_timeout;

    QString pythonPath() const;
};

#endif // STORIES3RUNNER_H
