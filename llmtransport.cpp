#include "llmtransport.h"
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <limits>

struct LlmTransport::Pending {
    QNetworkRequest request;
    QJsonObject body;
    QByteArray payload;
    QString stage, prefix, logDirectory;
    Completion completion;
    int attempts = 0;
    bool cancelled = false;
};

static void saveBytes(const QString& directory, const QString& name, const QByteArray& bytes)
{
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    QSaveFile file(directory + "/" + name);
    if (file.open(QIODevice::WriteOnly)) { file.write(bytes); file.commit(); }
}

static void saveJson(const QString& directory, const QString& name, const QJsonObject& value)
{
    saveBytes(directory, name, QJsonDocument(value).toJson(QJsonDocument::Indented));
}

LlmTransport::LlmTransport(QObject* parent) : LlmTransport(RetryPolicy{}, parent) {}

LlmTransport::LlmTransport(const RetryPolicy& policy, QObject* parent)
    : QObject(parent), m_policy(policy)
{
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_retryTimer, &QTimer::timeout, this, [this]() {
        const auto pending = m_pending;
        if (pending) attempt(pending);
    });
}

LlmTransport::~LlmTransport() { cancel(); }

void LlmTransport::cancel()
{
    m_retryTimer.stop();
    const auto pending = m_pending;
    m_pending.reset();
    if (pending) {
        pending->cancelled = true;
        saveJson(pending->logDirectory, pending->prefix + ".cancelled.json",
                 {{"stage", pending->stage}, {"attempts", pending->attempts},
                  {"cancelled_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    }
    const auto reply = m_reply;
    m_reply.clear();
    if (reply) reply->abort();
}

qint64 LlmTransport::retryAfterMs(const QByteArray& header, const QDateTime& now)
{
    const auto value = header.trimmed();
    if (value.isEmpty()) return -1;
    if (QRegularExpression(QStringLiteral("^[0-9]+$")).match(QString::fromLatin1(value)).hasMatch()) {
        bool ok = false;
        const auto seconds = value.toLongLong(&ok);
        if (!ok || seconds > std::numeric_limits<qint64>::max() / 1000)
            return std::numeric_limits<qint64>::max();
        return seconds * 1000;
    }
    // Qt's RFC2822 parser expects a numeric zone; HTTP dates use literal GMT.
    auto dateText = QString::fromLatin1(value);
    if (dateText.endsWith(" GMT")) {
        dateText.chop(4);
        dateText += " +0000";
    }
    const auto date = QDateTime::fromString(dateText, Qt::RFC2822Date);
    return date.isValid() ? std::max(qint64(0), now.msecsTo(date)) : -1;
}

qint64 LlmTransport::retryDelayMs(const RetryPolicy& policy, int completedAttempts,
                                const QByteArray& retryAfter, const QDateTime& now)
{
    if (completedAttempts >= policy.maxAttempts) return -1;
    const auto serverDelay = retryAfterMs(retryAfter, now);
    // Do not retry sooner than requested by the server, or wait indefinitely.
    if (serverDelay > policy.maxRetryAfterMs) return -1;
    const qint64 backoff = std::min(qint64(policy.maxBackoffMs),
                                    qint64(policy.baseDelayMs) * completedAttempts);
    return std::max(qint64(0), std::max(backoff, serverDelay));
}

void LlmTransport::send(const QNetworkRequest& request, const QJsonObject& body,
                       const QString& stage, const QString& prefix, const QString& logDirectory,
                       Completion completion)
{
    cancel();
    auto pending = std::make_shared<Pending>();
    pending->request = request;
    pending->body = body;
    pending->payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    pending->stage = stage;
    pending->prefix = prefix;
    pending->logDirectory = logDirectory;
    pending->completion = std::move(completion);
    m_pending = pending;
    attempt(pending);
}

void LlmTransport::attempt(const std::shared_ptr<Pending>& pending)
{
    if (m_pending != pending) return;
    const int number = ++pending->attempts;
    const QString prefix = pending->prefix + (number == 1 ? QString()
        : QString("_attempt_%1").arg(number, 2, 10, QChar('0')));
    saveJson(pending->logDirectory, prefix + ".request.json", {
        {"stage", pending->stage}, {"request_id", pending->prefix}, {"attempt", number},
        {"max_attempts", m_policy.maxAttempts},
        {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"url", pending->request.url().toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery)},
        {"body", pending->body}});

    QElapsedTimer elapsed;
    elapsed.start();
    auto* reply = m_network.post(pending->request, pending->payload);
    m_reply = reply;
    auto* deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("llmTimedOut", true);
        reply->abort();
    });
    deadline->start(m_policy.requestTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, pending, reply, deadline, elapsed, number, prefix]() {
        deadline->stop();
        const bool timedOut = reply->property("llmTimedOut").toBool();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        const QByteArray raw = reply->isOpen() ? reply->readAll() : QByteArray();
        const auto document = QJsonDocument::fromJson(raw);
        const auto retryAfter = reply->rawHeader("Retry-After");
        QString content = document.object().value("choices").toArray().first().toObject()
            .value("message").toObject().value("content").toString();
        const bool ok = !pending->cancelled && !timedOut && networkError == QNetworkReply::NoError
            && status >= 200 && status < 300 && !content.trimmed().isEmpty();
        QString reason;
        if (!ok) {
            if (pending->cancelled) reason = "Запрос отменён";
            else if (timedOut) reason = "Истекло время ожидания ответа LLM";
            else if (networkError == QNetworkReply::NoError && status >= 200 && status < 300)
                reason = "LLM вернул пустой или некорректный ответ";
            else reason = QString("HTTP %1: %2").arg(status).arg(reply->errorString());
            content = reason + " " + QString::fromUtf8(raw).left(500);
        }
        const bool temporary = timedOut || status == 408 || status == 429 || status == 500
            || status == 502 || status == 503 || status == 504
            || (status == 0 && (networkError == QNetworkReply::TimeoutError
                || networkError == QNetworkReply::RemoteHostClosedError
                || networkError == QNetworkReply::TemporaryNetworkFailureError
                || networkError == QNetworkReply::ConnectionRefusedError));
        const qint64 delay = !ok && temporary && !pending->cancelled
            ? retryDelayMs(m_policy, number, retryAfter, QDateTime::currentDateTimeUtc()) : -1;
        saveJson(pending->logDirectory, prefix + ".response.json", {
            {"ok", ok}, {"stage", pending->stage}, {"request_id", pending->prefix}, {"attempt", number},
            {"http_status", status}, {"elapsed_ms", double(elapsed.elapsed())},
            {"network_error", reply->errorString()}, {"timed_out", timedOut}, {"cancelled", pending->cancelled},
            {"retry_after", QString::fromLatin1(retryAfter)}, {"will_retry", delay >= 0},
            {"retry_delay_ms", double(delay)}, {"error", reason},
            {"response", document.isObject() ? QJsonValue(document.object()) : QJsonValue(QString::fromUtf8(raw))}});
        saveBytes(pending->logDirectory, prefix + ".response.txt", content.toUtf8());
        reply->deleteLater();
        if (m_pending != pending) return;
        m_reply.clear();
        if (delay >= 0) {
            m_retryTimer.start(int(delay));
            emit logMessage(QString("LLM [%1]: %2. Повтор %3/%4 через %5 с; продолжаем тот же запрос.")
                .arg(pending->stage, reason).arg(number + 1).arg(m_policy.maxAttempts)
                .arg((delay + 999) / 1000));
            return;
        }
        if (!ok) {
            if (temporary && number >= m_policy.maxAttempts)
                content += QString("\nИсчерпаны попытки LLM (%1/%2). ").arg(number).arg(m_policy.maxAttempts);
            else if (temporary && retryAfterMs(retryAfter, QDateTime::currentDateTimeUtc()) > m_policy.maxRetryAfterMs)
                content += "\nRetry-After сервера превышает допустимое ожидание; автоматический повтор остановлен. ";
            if (!pending->logDirectory.isEmpty()) content += "\nЛоги: " + pending->logDirectory;
        }
        m_pending.reset();
        pending->completion(ok, content, prefix);
    });
}
