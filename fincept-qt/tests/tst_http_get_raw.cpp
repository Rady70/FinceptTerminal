// tst_http_get_raw.cpp — MarketLab ETF Capital Flows Batch B: the raw GET the
// SEC N-PORT route uses (HttpClient::get_raw).
//
// WHY THIS SUITE LINKS Qt6::Network (the same exception tst_redirect_guard
// explains): what it asserts is what goes over a socket and comes back — that
// the per-request User-Agent the SEC requires REPLACES the client's default on
// the wire, that the body arrives byte for byte (the SEC ingestor hashes it),
// that an HTTP error status is delivered with its body instead of being turned
// into something else, and that a transport failure carries no status. No
// predicate can show that. The suite talks only to loopback QTcpServers it
// starts itself and needs no internet.
//
// Deliberately NOT tested here: the Fincept-destination refusal. get_raw()
// reuses HostedPathGuard::is_fincept_destination (asserted in
// tst_marketlab_boundary) and GuardedNetworkAccessManager (tst_redirect_guard);
// driving a real Fincept URL through the transport would make "the guard broke"
// fail by contacting the forbidden service, which the redirect suite's header
// explains is not acceptable.

#include "network/http/HttpClient.h"

#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

using fincept::HttpClient;

namespace {

constexpr int kTimeoutMs = 5000;

/// A loopback HTTP server that records the request it received and answers
/// with one canned response.
class RecordingServer : public QTcpServer {
  public:
    QByteArray response;
    QByteArray last_request;

    QString url(const QString& path = QStringLiteral("/doc.xml")) const {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path);
    }

  protected:
    void incomingConnection(qintptr descriptor) override {
        auto* sock = new QTcpSocket(this);
        if (!sock->setSocketDescriptor(descriptor)) {
            sock->deleteLater();
            return;
        }
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock]() {
            if (!sock->peek(65536).contains("\r\n\r\n"))
                return;
            last_request = sock->readAll();
            sock->write(response);
            sock->flush();
            sock->disconnectFromHost();
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }
};

QByteArray http_response(int status, const char* reason, const QByteArray& body) {
    return QByteArray("HTTP/1.1 ") + QByteArray::number(status) + ' ' + reason +
           "\r\nContent-Type: application/xml\r\nContent-Length: " + QByteArray::number(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

HttpClient::RawResponse get(const QString& url, const HttpClient::Headers& headers) {
    HttpClient::RawResponse out;
    bool done = false;
    QEventLoop loop;
    HttpClient::instance().get_raw(
        url,
        [&](const HttpClient::RawResponse& r) {
            out = r;
            done = true;
            loop.quit();
        },
        nullptr, headers);
    if (!done) {
        QTimer::singleShot(kTimeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return out;
}

} // namespace

class TstHttpGetRaw : public QObject {
    Q_OBJECT

  private slots:
    void declared_user_agent_replaces_the_default_on_the_wire();
    void body_arrives_byte_for_byte();
    void http_error_status_is_delivered_with_its_body();
    void transport_failure_has_no_status();
};

void TstHttpGetRaw::declared_user_agent_replaces_the_default_on_the_wire() {
    RecordingServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.response = http_response(200, "OK", "<x/>");
    HttpClient::Headers headers;
    headers.insert("User-Agent", "MarketLab test admin@example.com");
    const auto r = get(server.url(), headers);
    QVERIFY2(r.transport_ok, qPrintable(r.error));
    // Header NAMES are case-insensitive (RFC 9110) and Qt 6.8 writes them in
    // lower case; the VALUE must arrive exactly as declared, once, and the
    // client's default agent must be gone.
    const QByteArray lower = server.last_request.toLower();
    QCOMPARE(lower.count("\r\nuser-agent:"), 1);
    QVERIFY2(server.last_request.contains(": MarketLab test admin@example.com\r\n"), server.last_request.constData());
    QVERIFY(!server.last_request.contains("MarketLabTerminal/"));
    QVERIFY(server.last_request.startsWith("GET /doc.xml HTTP/1.1"));
}

void TstHttpGetRaw::body_arrives_byte_for_byte() {
    RecordingServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QByteArray body = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\" ?><feed>";
    for (int i = 0; i < 20000; ++i)
        body += static_cast<char>('a' + i % 26);
    body += "\xE9</feed>"; // a Latin-1 byte: bytes are not re-encoded
    server.response = http_response(200, "OK", body);
    const auto r = get(server.url(), {});
    QVERIFY(r.transport_ok);
    QCOMPARE(r.status, 200);
    QCOMPARE(r.body, body);
}

void TstHttpGetRaw::http_error_status_is_delivered_with_its_body() {
    RecordingServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    for (int status : {404, 429, 503}) {
        server.response = http_response(status, "Error", "no");
        const auto r = get(server.url(), {});
        QVERIFY(r.transport_ok);
        QCOMPARE(r.status, status);
        QCOMPARE(r.body, QByteArray("no"));
    }
}

void TstHttpGetRaw::transport_failure_has_no_status() {
    quint16 port = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        port = probe.serverPort();
    } // closed: nothing listens on the port now
    const auto r = get(QStringLiteral("http://127.0.0.1:%1/").arg(port), {});
    QVERIFY(!r.transport_ok);
    QCOMPARE(r.status, 0);
    QVERIFY(!r.error.isEmpty());
}

QTEST_GUILESS_MAIN(TstHttpGetRaw)
#include "tst_http_get_raw.moc"
