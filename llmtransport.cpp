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
    int timeoutMs = 0;
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

void LlmTransport::resetBudget(int maximumPosts)
{
    m_maxPosts = qMax(0, maximumPosts); m_posts = 0; m_unknownOutcomes = 0; m_knownTokens = 0;
}

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
                       Completion completion, int timeoutMs)
{
    cancel();
    auto pending = std::make_shared<Pending>();
    pending->request = request;
    // Long non-streaming gateway responses have failed with HTTP/2 protocol
    // errors after several minutes. Use HTTP/1.1 for subsequent requests; do
    // not replay a failed POST automatically when its billing outcome is unknown.
    pending->request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    pending->body = body;
    pending->payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    pending->stage = stage;
    pending->prefix = prefix;
    pending->logDirectory = logDirectory;
    pending->completion = std::move(completion);
    pending->timeoutMs = timeoutMs > 0 ? timeoutMs : m_policy.requestTimeoutMs;
    m_pending = pending;
    attempt(pending);
}

void LlmTransport::attempt(const std::shared_ptr<Pending>& pending)
{
    if (m_pending != pending) return;
    if (m_maxPosts > 0 && m_posts >= m_maxPosts) {
        m_pending.reset();
        const QString error = QString("REQUEST_BUDGET_EXHAUSTED: использовано %1/%2 API-запросов; состояние сохранено.")
            .arg(m_posts).arg(m_maxPosts);
        saveJson(pending->logDirectory, pending->prefix + ".budget_exhausted.json",
            {{"stage", pending->stage}, {"posts", m_posts}, {"limit", m_maxPosts}});
        pending->completion(false, error, pending->prefix); return;
    }
    const int number = ++pending->attempts;
    const QString prefix = pending->prefix + (number == 1 ? QString()
        : QString("_attempt_%1").arg(number, 2, 10, QChar('0')));
    saveJson(pending->logDirectory, prefix + ".request.json", {
        {"stage", pending->stage}, {"request_id", pending->prefix}, {"attempt", number},
        {"max_attempts", m_policy.maxAttempts},
        {"timeout_ms", pending->timeoutMs},
        {"http2_allowed", false},
        {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"url", pending->request.url().toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery)},
        {"body", pending->body}});

    QElapsedTimer elapsed;
    elapsed.start();
    ++m_posts;
    auto* reply = m_network.post(pending->request, pending->payload);
    m_reply = reply;
    const auto received = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, reply, [reply, received]() { received->append(reply->readAll()); });
    auto* deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("llmTimedOut", true);
        reply->abort();
    });
    deadline->start(pending->timeoutMs);
    auto* progress = new QTimer(reply);
    progress->setInterval(m_policy.progressIntervalMs);
    connect(progress, &QTimer::timeout, this, [this, pending, reply, received, elapsed]() {
        const bool headers = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid();
        emit logMessage(QString("LLM [%1] %2: ожидание ответа %3 с из %4 с; HTTP-заголовки %5, получено %6 байт.")
            .arg(pending->stage, pending->body["model"].toString())
            .arg(elapsed.elapsed() / 1000).arg(pending->timeoutMs / 1000)
            .arg(headers ? "получены" : "ещё не получены").arg(received->size()));
    });
    progress->start();
    connect(reply, &QNetworkReply::finished, this, [this, pending, reply, deadline, progress, received, elapsed, number, prefix]() {
        deadline->stop();
        progress->stop();
        const bool timedOut = reply->property("llmTimedOut").toBool();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        if (reply->isOpen()) received->append(reply->readAll());
        const QByteArray raw = *received;
        const auto document = QJsonDocument::fromJson(raw);
        const auto retryAfter = reply->rawHeader("Retry-After");
        const bool transportError = networkError != QNetworkReply::NoError
            && (networkError < QNetworkReply::ContentAccessDenied
                || networkError == QNetworkReply::ProtocolFailure || networkError == QNetworkReply::ProtocolUnknownError);
        // A missing/truncated response does not establish that the server rejected the POST.
        // Replaying it can create a second billable generation.
        const bool outcomeUnknown = timedOut || status == 0 || transportError;
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
            if (outcomeUnknown && !pending->cancelled)
                content += "\nРезультат запроса на сервере неизвестен. Автоповтор отключён: "
                           "сервер мог выполнить генерацию и учесть расход.";
        }
        const bool temporary = status == 408 || status == 429 || status == 500
            || status == 502 || status == 503 || status == 504;
        const bool budgetExhausted = m_maxPosts > 0 && m_posts >= m_maxPosts;
        const qint64 delay = !ok && temporary && !outcomeUnknown && !pending->cancelled && !budgetExhausted
            ? retryDelayMs(m_policy, number, retryAfter, QDateTime::currentDateTimeUtc()) : -1;
        saveJson(pending->logDirectory, prefix + ".response.json", {
            {"ok", ok}, {"stage", pending->stage}, {"request_id", pending->prefix}, {"attempt", number},
            {"http_status", status}, {"elapsed_ms", double(elapsed.elapsed())},
            {"http2_used", reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool()},
            {"network_error", networkError == QNetworkReply::NoError ? QString() : reply->errorString()},
            {"timed_out", timedOut}, {"cancelled", pending->cancelled}, {"outcome_unknown", outcomeUnknown},
            {"response_bytes", raw.size()}, {"timeout_ms", pending->timeoutMs},
            {"server_request_id", QString::fromUtf8(reply->rawHeader("x-request-id"))},
            {"retry_after", QString::fromLatin1(retryAfter)}, {"will_retry", delay >= 0},
            {"retry_delay_ms", double(delay)}, {"error", reason},
            {"response", document.isObject() ? QJsonValue(document.object()) : QJsonValue(QString::fromUtf8(raw))}});
        saveBytes(pending->logDirectory, prefix + ".response.txt", content.toUtf8());
        if (m_pending == pending) {
            const auto usage = document.object()["usage"].toObject();
            if (usage["total_tokens"].isDouble() && usage["total_tokens"].toDouble() >= 0)
                m_knownTokens += usage["total_tokens"].toDouble();
            if (outcomeUnknown) ++m_unknownOutcomes;
            saveJson(pending->logDirectory, "request_budget.json", {{"posts", m_posts}, {"limit", m_maxPosts},
                {"known_total_tokens", m_knownTokens}, {"unknown_outcomes", m_unknownOutcomes}});
            emit attemptFinished({{"request_id", pending->prefix}, {"stage", pending->stage},
                {"attempt", number}, {"ok", ok}, {"outcome_unknown", outcomeUnknown},
                {"usage", usage}, {"server_request_id", QString::fromUtf8(reply->rawHeader("x-request-id"))}});
        }
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
            if (budgetExhausted) content += QString("\nREQUEST_BUDGET_EXHAUSTED: использовано %1/%2 API-запросов.").arg(m_posts).arg(m_maxPosts);
            if (temporary && number >= m_policy.maxAttempts)
                content += QString("\nИсчерпаны попытки LLM (%1/%2). ").arg(number).arg(m_policy.maxAttempts);
            else if (temporary && retryAfterMs(retryAfter, QDateTime::currentDateTimeUtc()) > m_policy.maxRetryAfterMs)
                content += "\nRetry-After сервера превышает допустимое ожидание; автоматический повтор остановлен. ";
            if (!pending->logDirectory.isEmpty()) content += "\nЛоги: " + pending->logDirectory;
        }
        m_pending.reset();
        pending->completion(ok, content, prefix);
    });
    emit logMessage(QString("LLM [%1]: %2, попытка %3/%4, ожидание до %5 с.")
        .arg(pending->stage, pending->body["model"].toString())
        .arg(number).arg(m_policy.maxAttempts).arg(pending->timeoutMs / 1000));
}
