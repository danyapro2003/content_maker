#ifndef LLMTRANSPORT_H
#define LLMTRANSPORT_H

#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>

// One logical chat request; network retries never add turns to the LLM dialogue.
class LlmTransport : public QObject
{
    Q_OBJECT
public:
    struct RetryPolicy {
        int maxAttempts = 4;
        int baseDelayMs = 60000;
        int maxBackoffMs = 180000;
        int maxRetryAfterMs = 600000;
        int requestTimeoutMs = 900000;
        int progressIntervalMs = 30000;
    };
    using Completion = std::function<void(bool, const QString&, const QString&)>;

    explicit LlmTransport(QObject* parent = nullptr);
    explicit LlmTransport(const RetryPolicy& policy, QObject* parent = nullptr);
    ~LlmTransport() override;
    void send(const QNetworkRequest& request, const QJsonObject& body,
              const QString& stage, const QString& prefix, const QString& logDirectory,
              Completion completion, int timeoutMs = 0);
    // Silent cancellation: no completion callback or late retry in the next run.
    void cancel();
    void resetBudget(int maximumPosts = 0);
    int postsUsed() const { return m_posts; }
    static qint64 retryAfterMs(const QByteArray& header, const QDateTime& now);
    static qint64 retryDelayMs(const RetryPolicy& policy, int completedAttempts,
                              const QByteArray& retryAfter, const QDateTime& now);

signals:
    void logMessage(const QString& message);
    void attemptFinished(const QJsonObject& result);

private:
    struct Pending;
    void attempt(const std::shared_ptr<Pending>& pending);
    RetryPolicy m_policy;
    QNetworkAccessManager m_network;
    QTimer m_retryTimer;
    QPointer<QNetworkReply> m_reply;
    std::shared_ptr<Pending> m_pending;
    int m_maxPosts = 0, m_posts = 0, m_unknownOutcomes = 0;
    double m_knownTokens = 0;
};

#endif
