// MarketLab — redirect containment in network::GuardedNetworkAccessManager.
//
// WHY THIS ONE SUITE LINKS Qt6::Network.
//
// The rule in tests/CMakeLists.txt is that a unit test needing Network is
// testing the wrong seam, and that rule is right for every other suite here:
// tst_direct_clients deliberately links Qt Core alone precisely so the binary
// has no network stack to initiate I/O with, and it asserts a pure decision
// (DirectRouteGuard) that needs nothing more.
//
// This suite asserts something that is not a decision. The defect it covers is
// that Qt follows a 3xx *inside the reply*: createRequest() is called exactly
// once, the redirect target never re-enters the override, and an allowed host
// answering "302 Location: https://api.fincept.in/..." reached the Fincept
// server. That is a property of the TRANSPORT, not of a predicate — there is no
// pure function whose return value proves it, because the bug lived in what Qt
// did after the predicate had already said yes. Asserting it therefore requires
// a real reply following a real 302 over a real socket. The suite gets that
// from loopback QTcpServers it starts itself; it makes no DNS query it expects
// to resolve, opens no connection off the machine, and needs no internet.
//
// Every other suite keeps its Core-only link line untouched. Only this target
// adds Qt6::Network, and only this comment claims the exception.
//
// WHY THE SUITE IS HERMETIC EVEN IF THE GUARD BREAKS.
//
// A regression test must not have "if the protection breaks, contact the real
// forbidden production service" as its failure mode — a broken guard here used
// to fetch https://api.fincept.in/telemetry and a real HTTP 404 from Fincept
// was actually observed once while proving the test non-vacuous. So the
// Fincept destination decision is now separated from the transport:
//
//   * the REAL Fincept URL appears only in
//     default_predicate_classifies_real_fincept_url(), a pure predicate
//     assertion (no sockets exist in that slot — the manager is never even
//     constructed);
//   * every transport case drives the manager with an INJECTED deny predicate
//     (setDeniedDestination) that denies the reserved RFC 2606 host
//     "denied.invalid". .invalid names are never resolved to an address, so a
//     regression in the guard at worst produces a DNS error against a name
//     that cannot contact anything — never traffic to api.fincept.in.
//
// The deny predicate is injectable for this reason alone: it is the seam that
// lets the transport be exercised end to end while the only real Fincept URL
// string in the file sits in a no-I/O assertion.
//
// What is observed in the transport cases is the discriminating outcome: the
// reply finishes with OperationCanceledError, which only the guard's abort()
// produces. Follow the redirect instead and the reply finishes with a
// DNS/connect error or a success — never OperationCanceledError. That is what
// the vacuity check exercises. The https->http downgrade rule cannot be driven
// end-to-end without a local TLS server, and a non-http redirect target dies
// in Qt's transport regardless (Qt follows a redirect by restarting the same
// HTTP reply implementation), so a transport-level assertion on it would pass
// with the guard removed and prove nothing. The rule is asserted directly
// against GuardedNetworkAccessManager::redirect_is_less_safe() instead, which
// is the actual code the vetting callback runs.
//
// The counter on the second local server is the "must never be hit" observable
// for every local case, and redirect_is_followed_when_permitted() proves the
// counter is live rather than stuck at zero.

#include "network/http/GuardedNetworkAccessManager.h"
#include "network/http/HostedPathGuard.h"

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <optional>

using fincept::network::GuardedNetworkAccessManager;

namespace {

constexpr int kTimeoutMs = 5000;
const char* const kSecondServerBody = "SECOND-SERVER-BODY";

// The hermetic stand-in for the real forbidden destination. RFC 2606 reserves
// .invalid: no resolver will ever answer it with an address, so even a
// completely broken guard produces a DNS failure against a name that cannot
// reach anything. The real Fincept URL appears in this file only inside the
// pure predicate slot.
const char* const kDeniedTarget = "https://denied.invalid/telemetry";

/// Deny exactly the fake host the transport cases redirect to.
GuardedNetworkAccessManager::DestinationDeny deny_invalid() {
    return [](const QUrl& u) {
        return u.host().compare(QStringLiteral("denied.invalid"), Qt::CaseInsensitive) == 0;
    };
}

/// A loopback HTTP server that answers every request with one canned response
/// and counts the connections it accepted. The counter is the whole point: it
/// is how "this target was never reached" is observed without trusting a log
/// line or an error code.
class LocalServer : public QTcpServer {
  public:
    explicit LocalServer(QObject* parent = nullptr) : QTcpServer(parent) {}

    QByteArray response;
    int connections = 0;

    QUrl url(const QString& path = QStringLiteral("/")) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path));
    }

  protected:
    void incomingConnection(qintptr descriptor) override {
        ++connections;
        auto* sock = new QTcpSocket(this);
        if (!sock->setSocketDescriptor(descriptor)) {
            sock->deleteLater();
            return;
        }
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock]() {
            // Wait for the end of the request headers before answering, so the
            // client never sees a response to a half-sent request.
            if (!sock->peek(65536).contains("\r\n\r\n"))
                return;
            sock->readAll();
            sock->write(response);
            sock->flush();
            sock->disconnectFromHost();
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }
};

QByteArray redirect_response(const QString& location) {
    return QByteArray("HTTP/1.1 302 Found\r\nLocation: ") + location.toUtf8() +
           QByteArray("\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

QByteArray ok_response(const QByteArray& body) {
    return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ") +
           QByteArray::number(body.size()) + QByteArray("\r\nConnection: close\r\n\r\n") + body;
}

struct Outcome {
    bool finished = false;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    int status = 0;
    QByteArray body;
    QUrl final_url;
};

/// Issue a GET and run the event loop until the reply finishes or the guard
/// timer fires. `policy` mirrors what real callers set on their own requests —
/// NoLessSafeRedirectPolicy is what WebScraperWidget, FeedMonitor, NewsService
/// and the two RSS dialogs ask for, so passing it here is also the check that
/// the guard's policy swap does not break them.
/// A nullopt `policy` leaves the attribute unset, which is what most callers in
/// the tree do and is the case the guard is easiest to get wrong on:
/// ManualRedirectPolicy is 0, so an unset attribute reads as "manual" to any
/// check that does not test the QVariant for validity first.
Outcome fetch(QNetworkAccessManager& nam, const QUrl& url,
              std::optional<QNetworkRequest::RedirectPolicy> policy =
                  QNetworkRequest::NoLessSafeRedirectPolicy) {
    QNetworkRequest req(url);
    if (policy)
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, *policy);
    req.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QTimer bail;
    bail.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&bail, &QTimer::timeout, &loop, &QEventLoop::quit);
    bail.start(kTimeoutMs);
    loop.exec();

    Outcome out;
    out.finished = reply->isFinished();
    out.error = reply->error();
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // A refused reply is closed, and reading a closed QIODevice logs a Qt
    // warning that has nothing to do with what is being asserted.
    if (reply->isOpen())
        out.body = reply->readAll();
    out.final_url = reply->url();
    reply->deleteLater();
    return out;
}

} // namespace

class TestRedirectGuard : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();

    // The Fincept destination DECISION, asserted purely: the real Fincept URL
    // appears nowhere else in this file, and this slot opens no socket.
    void default_predicate_classifies_real_fincept_url();

    // The initial-URL refusal that already existed — kept green so the redirect
    // work cannot be mistaken for a replacement of it.
    void initial_denied_url_is_refused();

    // Proves the fix did not simply break every redirect. Also proves the
    // "never reached" counter used by the refusal cases actually moves.
    void redirect_is_followed_when_permitted();

    // The adversarial case the whole change exists for.
    void redirect_to_denied_destination_is_refused();

    // The NoLessSafeRedirectPolicy protection the policy swap replaced.
    void less_safe_redirect_is_refused();

    // A caller that asked to handle redirects itself still gets the 302.
    void manual_redirect_policy_is_preserved();

    // ...and a caller that set no policy at all is still vetted.
    void unset_redirect_policy_is_still_vetted();

  private:
    LocalServer redirector_; // answers 302 to wherever `redirect_to_` points
    LocalServer target_;     // the "must never be hit unless allowed" server
    QString redirect_to_;

    void point_redirector_at(const QString& location) {
        redirect_to_ = location;
        redirector_.response = redirect_response(location);
    }
};

void TestRedirectGuard::initTestCase() {
    QVERIFY2(redirector_.listen(QHostAddress::LocalHost, 0), qPrintable(redirector_.errorString()));
    QVERIFY2(target_.listen(QHostAddress::LocalHost, 0), qPrintable(target_.errorString()));
    target_.response = ok_response(QByteArray(kSecondServerBody));
}

void TestRedirectGuard::default_predicate_classifies_real_fincept_url() {
    // Pure: the predicate the manager ships with must classify the real
    // Fincept destination. No manager is constructed, no reply exists, no
    // socket can be opened — is_fincept_destination is a decision over a URL
    // string, and this is the ONLY place in this file where the real Fincept
    // URL appears. The predicate itself is further exercised without I/O in
    // tst_marketlab_boundary.
    QVERIFY(fincept::network::HostedPathGuard::is_fincept_destination(
        QUrl(QStringLiteral("https://api.fincept.in/telemetry"))));
}

void TestRedirectGuard::initial_denied_url_is_refused() {
    GuardedNetworkAccessManager nam;
    nam.setDeniedDestination(deny_invalid());
    const int before = target_.connections;

    // The injected predicate denies denied.invalid, so the manager refuses the
    // request before any connection attempt — the hermetic stand-in for the
    // real Fincept URL.
    const Outcome out = fetch(nam, QUrl(QString::fromLatin1(kDeniedTarget)));

    QVERIFY(out.finished);
    QCOMPARE(out.error, QNetworkReply::ContentAccessDenied);
    QCOMPARE(out.status, 0);
    // Nothing on this machine was contacted either.
    QCOMPARE(target_.connections, before);
}

void TestRedirectGuard::redirect_is_followed_when_permitted() {
    GuardedNetworkAccessManager nam;
    point_redirector_at(target_.url().toString());
    const int before = target_.connections;

    const Outcome out = fetch(nam, redirector_.url());

    QVERIFY(out.finished);
    QCOMPARE(out.error, QNetworkReply::NoError);
    QCOMPARE(out.status, 200);
    QCOMPARE(out.body, QByteArray(kSecondServerBody));
    QCOMPARE(out.final_url, target_.url());
    // The redirect target WAS reached — which is what makes a delta of zero in
    // the refusal cases below mean something.
    QCOMPARE(target_.connections, before + 1);
}

void TestRedirectGuard::redirect_to_denied_destination_is_refused() {
    GuardedNetworkAccessManager nam;
    nam.setDeniedDestination(deny_invalid());
    // The redirect target is the reserved .invalid name, never the real
    // Fincept URL: if the guard breaks, this test fails with a DNS error
    // against a name that cannot contact anything.
    point_redirector_at(QString::fromLatin1(kDeniedTarget));
    const int before = target_.connections;

    const Outcome out = fetch(nam, redirector_.url());

    QVERIFY(out.finished);
    // OperationCanceledError is produced ONLY by the guard's abort() inside the
    // redirected() vetting callback. Following the redirect instead yields a
    // DNS/connect failure or a 2xx — either way, not this. That is the whole
    // assertion: the guard decided, and it decided before any connection.
    QCOMPARE(out.error, QNetworkReply::OperationCanceledError);
    QVERIFY(out.body.isEmpty());
    QCOMPARE(target_.connections, before);

    // Deliberately NOT asserted: that reply->url() is still the local one. Qt
    // rewrites the reply's URL when it PREPARES the redirect request, which
    // happens before redirected() is emitted and therefore before the vetting
    // runs — so it reports the target whether or not the hop was ever made, and
    // is evidence of nothing either way. Verified against Qt 6.8.3 rather than
    // assumed: with the guard in place the reply's url is the Fincept one while
    // the connection was never opened.
}

void TestRedirectGuard::less_safe_redirect_is_refused() {
    // The rules QNetworkRequest::NoLessSafeRedirectPolicy enforces, asserted
    // against the predicate the vetting callback actually calls. See the file
    // header for why this one is not driven through the socket.
    const QUrl https(QStringLiteral("https://allowed.example/a"));
    const QUrl http(QStringLiteral("http://allowed.example/a"));

    // Downgrade: https -> http strips TLS from a route the caller encrypted.
    QVERIFY(GuardedNetworkAccessManager::redirect_is_less_safe(https, http));
    // Scheme change: anything that is not http(s) is a protocol the caller
    // never agreed to speak.
    QVERIFY(GuardedNetworkAccessManager::redirect_is_less_safe(https, QUrl("ftp://allowed.example/a")));
    QVERIFY(GuardedNetworkAccessManager::redirect_is_less_safe(http, QUrl("file:///etc/passwd")));
    QVERIFY(GuardedNetworkAccessManager::redirect_is_less_safe(http, QUrl("about:blank")));
    QVERIFY(GuardedNetworkAccessManager::redirect_is_less_safe(http, QUrl("/relative/only")));

    // Same-or-better is allowed, otherwise the guard would just break redirects.
    QVERIFY(!GuardedNetworkAccessManager::redirect_is_less_safe(http, http));
    QVERIFY(!GuardedNetworkAccessManager::redirect_is_less_safe(https, https));
    QVERIFY(!GuardedNetworkAccessManager::redirect_is_less_safe(http, https));
    QVERIFY(!GuardedNetworkAccessManager::redirect_is_less_safe(https, QUrl("HTTPS://other.example/b")));

    // And end to end: a non-http redirect target never reaches the second
    // server. Belt and braces — Qt's own transport would refuse it too, so this
    // asserts the guard does not somehow route around that, not that the guard
    // is what stops it.
    GuardedNetworkAccessManager nam;
    point_redirector_at(QStringLiteral("ftp://127.0.0.1:%1/").arg(target_.serverPort()));
    const int before = target_.connections;

    const Outcome out = fetch(nam, redirector_.url());

    QVERIFY(out.finished);
    QVERIFY(out.error != QNetworkReply::NoError);
    QCOMPARE(target_.connections, before);
}

void TestRedirectGuard::manual_redirect_policy_is_preserved() {
    // A caller that asked for ManualRedirectPolicy reads the Location header
    // itself and issues the next request through this same manager — where
    // createRequest() checks it again. Overriding that policy would auto-follow
    // a hop the caller expects to handle, so the guard leaves it alone.
    GuardedNetworkAccessManager nam;
    point_redirector_at(target_.url().toString());
    const int before = target_.connections;

    const Outcome out = fetch(nam, redirector_.url(), QNetworkRequest::ManualRedirectPolicy);

    QVERIFY(out.finished);
    QCOMPARE(out.error, QNetworkReply::NoError);
    QCOMPARE(out.status, 302);
    QCOMPARE(target_.connections, before); // not followed
}

void TestRedirectGuard::unset_redirect_policy_is_still_vetted() {
    // Most requests in the tree never touch RedirectPolicyAttribute. Because
    // ManualRedirectPolicy is 0, a guard that compares the attribute's value
    // without first checking the QVariant is valid reads every one of them as
    // "the caller handles redirects" and lets them through unvetted. This case
    // exists so that mistake cannot come back silently.
    GuardedNetworkAccessManager nam;
    nam.setDeniedDestination(deny_invalid());
    point_redirector_at(QString::fromLatin1(kDeniedTarget));
    const int before = target_.connections;

    const Outcome out = fetch(nam, redirector_.url(), std::nullopt);

    QVERIFY(out.finished);
    QCOMPARE(out.error, QNetworkReply::OperationCanceledError);
    QCOMPARE(target_.connections, before);
}

QTEST_MAIN(TestRedirectGuard)
#include "tst_redirect_guard.moc"
