#include "llmtransport.h"
#include "s3cache.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <stdexcept>

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static QByteArray readFile(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "missing test artifact");
    return file.readAll();
}

static void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "cannot write fixture");
    require(file.write(bytes) == bytes.size(), "incomplete fixture");
}

static QByteArray successBody()
{
    return R"({"choices":[{"message":{"content":"OK"}}]})";
}

struct Response {
    int status = 200;
    QByteArray retryAfter;
    QByteArray body = successBody();
    bool hang = false;
};

class MockServer {
public:
    QTcpServer server;
    QList<Response> responses;
    QList<QByteArray> bodies, headers;
    explicit MockServer(const QList<Response>& sequence) : responses(sequence)
    {
        require(server.listen(QHostAddress::LocalHost), "cannot listen locally");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto data = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", data);
                    const int split = data.indexOf("\r\n\r\n");
                    if (split < 0 || socket->property("answered").toBool()) return;
                    int length = 0;
                    for (const auto& line : data.left(split).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (data.size() < split + 4 + length) return;
                    socket->setProperty("answered", true);
                    const auto response = responses.value(bodies.size(), responses.last());
                    bodies.append(data.mid(split + 4, length));
                    headers.append(data.left(split));
                    if (response.hang) return;
                    QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\n"
                        "Content-Type: application/json\r\nConnection: close\r\n";
                    if (!response.retryAfter.isEmpty()) head += "Retry-After: " + response.retryAfter + "\r\n";
                    socket->write(head + "Content-Length: " + QByteArray::number(response.body.size())
                                  + "\r\n\r\n" + response.body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QNetworkRequest request() const
    {
        QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort())));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization", "Bearer transport-secret-do-not-log");
        return request;
    }
};

static LlmTransport::RetryPolicy fastPolicy()
{
    LlmTransport::RetryPolicy policy;
    policy.baseDelayMs = 10;
    policy.maxBackoffMs = 30;
    policy.maxRetryAfterMs = 1000;
    policy.requestTimeoutMs = 2000;
    return policy;
}

static void drain(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

static void testCase(const QList<Response>& sequence, int expectedAttempts, bool expectedSuccess,
                     const LlmTransport::RetryPolicy& policy = fastPolicy(), int minimumMs = 0)
{
    MockServer server(sequence);
    LlmTransport transport(policy);
    QTemporaryDir logs;
    require(logs.isValid(), "no temporary logs");
    const QJsonObject body{{"model", "test-model"}, {"temperature", 0.7},
        {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", "original request"}}}}};
    QEventLoop loop;
    int callbacks = 0;
    bool success = false;
    QString content, finalPrefix;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(5000);
    QElapsedTimer elapsed;
    elapsed.start();
    transport.send(server.request(), body, "index_1", "001_index_1", logs.path(),
        [&](bool ok, const QString& text, const QString& prefix) {
            ++callbacks; success = ok; content = text; finalPrefix = prefix; loop.quit();
        });
    loop.exec();
    require(callbacks == 1, "request did not finish exactly once");
    require(success == expectedSuccess, "wrong completion status");
    require(elapsed.elapsed() >= minimumMs, "ignored Retry-After wait");
    drain(50);
    require(callbacks == 1 && server.bodies.size() == expectedAttempts, "extra request or callback");
    if (success) require(content == "OK", "successful content lost");
    if (!success && expectedAttempts == policy.maxAttempts)
        require(content.contains("4/4"), "exhaustion not explained");
    const auto files = QDir(logs.path()).entryList({"*.request.json"}, QDir::Files);
    require(files.size() == expectedAttempts, "attempt request logs overwritten");
    for (int i = 0; i < expectedAttempts; ++i) {
        require(server.bodies[i] == server.bodies.first(), "retry changed request body or dialogue");
        require(server.headers[i].contains("Bearer transport-secret-do-not-log"), "retry lost authentication");
        const QString prefix = "001_index_1" + (i == 0 ? QString()
            : QString("_attempt_%1").arg(i + 1, 2, 10, QChar('0')));
        const auto response = QJsonDocument::fromJson(readFile(logs.filePath(prefix + ".response.json"))).object();
        require(response["attempt"].toInt() == i + 1, "wrong attempt number");
        require(response["will_retry"].toBool() == (i < expectedAttempts - 1), "wrong retry log");
        if (i == expectedAttempts - 1) require(finalPrefix == prefix, "audit prefix not attached to final attempt");
    }
    for (const auto& file : QDir(logs.path()).entryList(QDir::Files))
        require(!readFile(logs.filePath(file)).contains("transport-secret-do-not-log"), "API key leaked into logs");
}

static void testCancellation(bool duringRequest)
{
    auto policy = fastPolicy();
    policy.baseDelayMs = 150; policy.maxBackoffMs = 150;
    MockServer server({duringRequest ? Response{200, {}, {}, true} : Response{429}, Response{}});
    LlmTransport transport(policy);
    QTemporaryDir logs;
    int oldCallbacks = 0, newCallbacks = 0;
    bool cancelled = false;
    auto restart = [&]() {
        if (cancelled) return;
        cancelled = true;
        transport.cancel();
        transport.send(server.request(), {{"model", "new-run"}}, "index_1", "002_new", logs.path(),
            [&](bool ok, const QString&, const QString&) { if (ok) ++newCallbacks; });
    };
    QObject::connect(&transport, &LlmTransport::logMessage, &transport, [&](const QString&) {
        if (!duringRequest) restart();
    });
    transport.send(server.request(), {{"model", "old-run"}}, "index_1", "001_old", logs.path(),
        [&](bool, const QString&, const QString&) { ++oldCallbacks; });
    if (duringRequest) QTimer::singleShot(30, &transport, restart);
    drain(400);
    require(cancelled && oldCallbacks == 0 && newCallbacks == 1, "cancelled request reached new run");
    require(server.bodies.size() == 2, "cancelled timer sent a late request");
    require(QFileInfo::exists(logs.filePath("001_old.cancelled.json")), "cancellation not logged");
}

static void testCacheDeployment()
{
    QTemporaryDir temporary;
    require(temporary.isValid(), "no cache workspace");
    QDir().mkpath(temporary.filePath("source scripts"));
    const auto script = temporary.filePath("source scripts/asr.py");
    const auto deployed = temporary.filePath("deployed scripts/asr.py");
    const auto source = temporary.filePath("film");
    const auto artifact = temporary.filePath("asr.json");
    writeFile(script, "print('one')"); writeFile(source, "movie"); writeFile(artifact, "transcript");
    auto deploy = [&]() {
        QProcess process;
        process.start(QString::fromUtf8(CLIPPER_CMAKE), {
            "-DCLIPPER_SCRIPT_SOURCE=" + temporary.filePath("source scripts"),
            "-DCLIPPER_SCRIPT_DESTINATION=" + temporary.filePath("deployed scripts"),
            "-P", QString::fromUtf8(CLIPPER_SOURCE_DIR) + "/cmake/DeployScripts.cmake"});
        require(process.waitForFinished(10000) && process.exitStatus() == QProcess::NormalExit
            && process.exitCode() == 0, "script deployment failed");
    };
    deploy();
    // Give the installed copy an old timestamp so unconditional copying is detectable.
    QFile deployedFile(deployed);
    require(deployedFile.open(QIODevice::ReadWrite), "cannot open installed script");
    require(deployedFile.setFileTime(QDateTime::currentDateTimeUtc().addDays(-1), QFileDevice::FileModificationTime),
            "cannot set script timestamp");
    deployedFile.close();
    const auto stamp = QFileInfo(deployed).lastModified();
    const auto key = S3Cache::makeKey(source, {{"engine", "fixture"}}, {deployed});
    require(S3Cache::writeMeta(artifact, key), "cannot write cache metadata");
    deploy();
    require(QFileInfo(deployed).lastModified() == stamp, "unchanged build touched ASR script");
    require(S3Cache::valid(artifact, S3Cache::makeKey(source, {{"engine", "fixture"}}, {deployed})),
            "unchanged rebuild invalidated existing cache");
    writeFile(script, "print('two')"); deploy();
    require(readFile(deployed) == "print('two')", "updated script not deployed");
    require(!S3Cache::valid(artifact, S3Cache::makeKey(source, {{"engine", "fixture"}}, {deployed})),
            "real script change failed to invalidate cache");
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        const auto now = QDateTime::fromString("2026-11-13T12:00:00Z", Qt::ISODate);
        const LlmTransport::RetryPolicy defaults;
        require(LlmTransport::retryDelayMs(defaults, 1, {}, now) == 60000, "first delay changed");
        require(LlmTransport::retryDelayMs(defaults, 2, {}, now) == 120000, "second delay changed");
        require(LlmTransport::retryDelayMs(defaults, 3, {}, now) == 180000, "third delay changed");
        require(LlmTransport::retryDelayMs(defaults, 4, {}, now) == -1, "unbounded retries");
        require(LlmTransport::retryDelayMs(defaults, 1, "300", now) == 300000, "seconds header ignored");
        require(LlmTransport::retryDelayMs(defaults, 1, "Fri, 13 Nov 2026 12:02:00 GMT", now) == 120000,
                "HTTP-date header ignored");
        require(LlmTransport::retryDelayMs(defaults, 1, "Thu, 12 Nov 2026 12:02:00 GMT", now) == 60000,
                "past date caused a negative wait");
        require(LlmTransport::retryDelayMs(defaults, 1, "invalid", now) == 60000, "bad header broke backoff");
        require(LlmTransport::retryDelayMs(defaults, 1, "99999999999999999999999", now) == -1,
                "oversized wait overflowed");
        require(LlmTransport::retryDelayMs(defaults, 1, "601", now) == -1, "server wait truncated");
        testCase({Response{429}, Response{500}, Response{}}, 3, true);
        testCase({Response{503}}, 4, false);
        for (int status : {408, 502, 504}) testCase({Response{status}, Response{}}, 2, true);
        for (int status : {400, 401, 403, 404, 422}) testCase({Response{status}}, 1, false);
        testCase({Response{200, {}, "{}"}}, 1, false);
        testCase({Response{200, {}, "not json"}}, 1, false);
        testCase({Response{429, "2"}}, 1, false);
        testCase({Response{429, "1"}, Response{}}, 2, true, fastPolicy(), 1000);
        auto timeoutPolicy = fastPolicy(); timeoutPolicy.requestTimeoutMs = 70;
        testCase({Response{200, {}, {}, true}, Response{}}, 2, true, timeoutPolicy);
        testCase({Response{200, {}, {}, true}}, 4, false, timeoutPolicy);
        testCancellation(false);
        testCancellation(true);
        testCacheDeployment();
        QTextStream(stdout) << "LLM retry, cancellation, logging and deployment cache tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}
