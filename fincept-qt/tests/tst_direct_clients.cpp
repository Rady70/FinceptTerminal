// tst_direct_clients.cpp — MarketLab Terminal direct-client containment tests.
//
// Covers the gap left by tst_marketlab_boundary: that suite proves the
// deny-list PREDICATE is correct (HostedPathGuard) and that capability/screen
// gating holds, but nothing exercised the two retained clients that own their own
// QNetworkAccessManager and therefore have to reject for themselves, outside
// the shared HttpClient deny-list (FINCEPT_FORK_PLAN.md §5.3):
//
//   src/services/quantlib/QuantLibClient.cpp:call()
//   src/network/cloud/CloudClient.cpp:reject_hosted()
//
// ── Why this tests DirectRouteGuard and not the clients directly ─────────────
//
// Neither retained client can be constructed in a unit test: QuantLibClient
// needs AppConfig + CacheManager + Logger + the MCP result type, CloudClient
// needs Logger and a live QNetworkAccessManager. Linking any of them here
// would mean hauling in most of the app, which tests/CMakeLists.txt forbids in
// so many words: "If a unit cannot be tested without dragging in half the
// application, that is a signal about the unit, not about this file: extract
// the pure logic into a small header (or a leaf .cpp) and test that."
//
// So the guard decision was extracted to src/network/http/DirectRouteGuard.cpp
// and both clients now call it — what is asserted below is the production
// decision itself, not a copy of the predicate re-implemented in this file.
// Weaken DirectRouteGuard (or the HostedPathGuard deny-list under it) and these
// assertions fail for both clients. The vectors below are the
// exact (base, endpoint) inputs each call site composes, so a change to a
// client's URL composition shows up here too.
//
// ── What this suite does NOT cover, stated plainly ───────────────────────────
//
// It does not prove that the two clients still call the guard. Deleting the
// `if (route.rejected) { ...; return; }` block from QuantLibClient::call() or
// CloudClient::reject_hosted() still compiles and
// every assertion here still passes — this file links DirectRouteGuard, not the
// clients. Closing that properly needs the clients to be constructible in a
// test, which is the refactor tests/CMakeLists.txt is asking for and which
// nobody has done yet.
//
// What has changed since that paragraph was first written is where the
// containment lives, not what this file proves. Both clients used to own a
// raw QNetworkAccessManager, so a deleted guard call was a hosted route. They
// now own a GuardedNetworkAccessManager, which applies HostedPathGuard inside
// createRequest() — on the initial URL and on every redirect hop — so deleting
// the DirectRouteGuard call above would cost the earlier, better-worded
// rejection but would not open a route. Two other controls cover the linkage:
// tst_redirect_guard drives a real GuardedNetworkAccessManager over a real
// socket, and the sink inventory in marketlab/hosted_path_inventory.json pins
// the per-file occurrence count of every outbound sink, so swapping the guarded
// manager back for a raw one changes a pinned count and fails the audit.
//
// These are also not regression tests for a bug that was fixed. Before
// DirectRouteGuard was extracted, both retained clients already rejected Fincept
// destinations by calling HostedPathGuard::is_fincept_destination() on the same
// composed URL (CloudClient::reject_hosted() already honoured the
// absolute-endpoint form too), so every assertion below would have passed
// against the pre-extraction code. They describe current behaviour and lock it
// in going forward; they did not catch anything.
//
// ── The "no network I/O" claim ───────────────────────────────────────────────
//
// This executable links Qt6::Test and Qt6::Core ONLY (see fincept_add_test in
// tests/CMakeLists.txt — deliberately no Qt6::Network). There is no
// QNetworkAccessManager, no QTcpSocket and no Qt TLS stack in this binary at
// all, so a rejection reached in this translation unit is by construction a
// rejection reached before any socket could exist. The decision is also
// returned synchronously by value: the production early-return path is fully
// determined by the DirectRouteDecision asserted here, with nothing left to
// happen on an event loop.

#include "network/http/DirectRouteGuard.h"
#include "network/http/ProbeGuard.h"
#include "network/http/HostedPathGuard.h"

#include <QTest>
#include <QUrl>

using namespace fincept::network;

// The shipped defaults from src/core/config/AppConfig.cpp. They stay
// Fincept-shaped on purpose — the fork's containment is the rejection, not a
// rewritten default — which is exactly why these must be rejected.
namespace {
const auto kApiBase = QStringLiteral("https://api.fincept.in");
const auto kCloudBase = QStringLiteral("https://api.fincept.in/v1");
} // namespace

class TstDirectClients : public QObject {
    Q_OBJECT

  private slots:
    void quantlib_client_rejects_fincept();
    void cloud_client_rejects_fincept();
    void rejection_is_the_typed_error_callers_match();
    void configuration_derived_host_is_rejected();
    void third_party_destinations_are_not_rejected();
    void composition_matches_the_call_sites();
    void probe_guard_judges_the_host_that_gets_dialled();
    void probe_guard_allows_third_party_connectors();
};

// QuantLibClient::call() composes AppConfig::api_base_url() + "/quantlib/" +
// endpoint and hands both halves to DirectRouteGuard::check_route().
void TstDirectClients::quantlib_client_rejects_fincept() {
    const auto post_route = DirectRouteGuard::check_route(kApiBase, QStringLiteral("/quantlib/pricing/black-scholes"));
    QVERIFY(post_route.rejected);
    QCOMPARE(post_route.error, QStringLiteral("HOSTED_SERVICE_UNAVAILABLE: api.fincept.in"));

    // The cached GET endpoints take the same path through call(), so they must
    // reject too — a cache miss on one of these is what would otherwise reach
    // the network.
    const auto get_route = DirectRouteGuard::check_route(kApiBase, QStringLiteral("/quantlib/core/types/currencies"));
    QVERIFY(get_route.rejected);
    QCOMPARE(get_route.url.host(), QStringLiteral("api.fincept.in"));
}

// CloudClient::reject_hosted() runs on every get/post/put/del before the
// QNetworkAccessManager is touched, over base_url_ set from
// AppConfig::cloud_base_url() by CloudSyncEngine.
void TstDirectClients::cloud_client_rejects_fincept() {
    for (const QString& endpoint : {QStringLiteral("/sync/pull"), QStringLiteral("/sync/push"),
                                    QStringLiteral("/user/profile"), QStringLiteral("/credits")}) {
        const auto route = DirectRouteGuard::check_route(kCloudBase, endpoint);
        QVERIFY2(route.rejected, qPrintable(QStringLiteral("not rejected: %1").arg(endpoint)));
        QCOMPARE(route.error, QStringLiteral("HOSTED_SERVICE_UNAVAILABLE: api.fincept.in"));
    }

    // CloudClient callers may pass an already-absolute endpoint, which wins
    // over base_url_. That form must be judged as well — judging only the
    // relative form is precisely how a hosted route slips past a guard.
    const auto absolute = DirectRouteGuard::check_route(QStringLiteral("https://example.invalid"),
                                                        QStringLiteral("https://api.fincept.in/v1/sync/pull"));
    QVERIFY(absolute.rejected);
    QCOMPARE(absolute.url.host(), QStringLiteral("api.fincept.in"));
}

// Each client hands its own callback the guard's error string verbatim
// (mcp::ToolResult::fail(), CloudResponse::error), so
// the wording is an API contract with those callers, not prose.
void TstDirectClients::rejection_is_the_typed_error_callers_match() {
    const auto route = DirectRouteGuard::check_route(kCloudBase, QStringLiteral("/sync/pull"));
    QVERIFY(route.rejected);
    QVERIFY(route.error.startsWith(HostedPathGuard::unavailable_prefix()));
    QVERIFY(HostedPathGuard::is_hosted_unavailable_error(route.error));

    // An allowed route carries no error to leak into a caller's result.
    const auto allowed = DirectRouteGuard::check_route(QStringLiteral("https://api.anthropic.com"),
                                                       QStringLiteral("/v1/messages"));
    QVERIFY(!allowed.rejected);
    QVERIFY(allowed.error.isEmpty());
    QVERIFY(!HostedPathGuard::is_hosted_unavailable_error(allowed.error));
}

// The regression class this whole control exists for: the host never appears as
// a literal in the client's source — it arrives from stored configuration and
// is only assembled at run time, so no literal-host source grep could ever see
// it. Build the base the way a settings read would and confirm the guard still
// rejects.
void TstDirectClients::configuration_derived_host_is_rejected() {
    const QString scheme = QStringLiteral("https://");
    const QString stored_host = QStringLiteral("api.") + QStringLiteral("fincept") + QStringLiteral(".in");
    const auto route = DirectRouteGuard::check_route(scheme + stored_host, QStringLiteral("/quantlib/ping"));
    QVERIFY(route.rejected);
    QCOMPARE(route.url.host(), stored_host);

    // Subdomains and case variants of a config-supplied host too.
    QVERIFY(DirectRouteGuard::check_route(QStringLiteral("https://MARKETS.FINCEPT.IN"), QStringLiteral("/x")).rejected);
    QVERIFY(DirectRouteGuard::check_route(QStringLiteral("https://anything.fincept.com"), QStringLiteral("/x")).rejected);
}

// Containment must not become a general outbound block: everything that is not
// Fincept-owned keeps working, including look-alike hostnames and the parts of
// GitHub that are not the Fincept org.
void TstDirectClients::third_party_destinations_are_not_rejected() {
    QVERIFY(!DirectRouteGuard::check_url(QStringLiteral("https://query1.finance.yahoo.com/v8/finance/chart/AAPL"))
                 .rejected);
    QVERIFY(!DirectRouteGuard::check_url(QStringLiteral("https://notfincept.in/api")).rejected);
    QVERIFY(!DirectRouteGuard::check_url(QStringLiteral("https://fincept-in.example.com/api")).rejected);
    QVERIFY(!DirectRouteGuard::check_url(QStringLiteral("http://127.0.0.1:8765/mcp")).rejected);
    QVERIFY(!DirectRouteGuard::check_url(QStringLiteral("https://github.com/QuantConnect/Lean")).rejected);

    // …but the path-scoped Fincept GitHub rules still apply through this seam.
    QVERIFY(DirectRouteGuard::check_url(QStringLiteral("https://github.com/Fincept-Corporation/FinceptTerminal"))
                .rejected);
    QVERIFY(DirectRouteGuard::check_url(
                QStringLiteral("https://raw.githubusercontent.com/Fincept-Corporation/x/updates.json"))
                .rejected);
}

// compose() is the one composition rule, shared by CloudClient::reject_hosted()
// and CloudClient::build_request(): the URL that was cleared must be exactly
// the URL that is then requested.
void TstDirectClients::composition_matches_the_call_sites() {
    QCOMPARE(DirectRouteGuard::compose(kCloudBase, QStringLiteral("/sync/pull")),
             QStringLiteral("https://api.fincept.in/v1/sync/pull"));
    QCOMPARE(DirectRouteGuard::compose(kApiBase, QStringLiteral("/quantlib/core/types/currencies")),
             QStringLiteral("https://api.fincept.in/quantlib/core/types/currencies"));

    // An absolute endpoint wins over the base.
    QCOMPARE(DirectRouteGuard::compose(QStringLiteral("https://base.example"),
                                       QStringLiteral("https://other.example/x")),
             QStringLiteral("https://other.example/x"));

    // Pure and deterministic — no event loop, no hidden state, same answer twice.
    const auto a = DirectRouteGuard::check_route(kApiBase, QStringLiteral("/quantlib/ping"));
    const auto b = DirectRouteGuard::check_route(kApiBase, QStringLiteral("/quantlib/ping"));
    QCOMPARE(a.rejected, b.rejected);
    QCOMPARE(a.error, b.error);
}

// ── ProbeGuard: the four Data Sources connector probes ───────────────────────
//
// These are regression tests in the strict sense: every case below reached
// QTcpSocket::connectToHost() before the fix. The first one is the defect an
// adversarial review found in the guard's own first version — the caller
// judged the resolved `host` while the worker dialled QUrl(test_url).host(),
// so a connector naming a harmless host in "host" and a Fincept host in
// "serviceRoot" passed the guard and then resolved api.fincept.in.
void TstDirectClients::probe_guard_judges_the_host_that_gets_dialled() {
    using fincept::network::ProbeGuard;

    // Scheme-relative probe URL: no scheme means is_fincept_destination()'s
    // network-scheme test cannot help, and the resolved host is innocent.
    const auto odata = ProbeGuard::check(QStringLiteral("//api.fincept.in/svc/$metadata"),
                                         QStringLiteral("example.com"));
    QVERIFY2(odata.rejected, "scheme-relative Fincept probe URL must be refused");

    // A scheme this application never speaks is still a resolvable host.
    QVERIFY(ProbeGuard::check(QStringLiteral("redis://api.fincept.in:6379"), QString()).rejected);
    QVERIFY(ProbeGuard::check(QStringLiteral("mongodb://user:pw@api.fincept.in:27017"), QString()).rejected);

    // Bare host + port, the shape a connector config supplies with no URL at all.
    QVERIFY(ProbeGuard::check(QString(), QStringLiteral("api.fincept.in")).rejected);

    // Trailing-dot FQDN: a valid absolute name that QTcpSocket resolves happily.
    QVERIFY(ProbeGuard::check(QString(), QStringLiteral("api.fincept.in.")).rejected);
    QVERIFY(ProbeGuard::check(QStringLiteral("https://api.fincept.in./v1"), QString()).rejected);

    // Case and stray whitespace from a config field.
    QVERIFY(ProbeGuard::check(QString(), QStringLiteral("  API.Fincept.IN  ")).rejected);

    // Userinfo must not shift which host is judged.
    QVERIFY(ProbeGuard::check(QStringLiteral("http://x@api.fincept.in/probe"), QString()).rejected);

    // The path-scoped Fincept GitHub-org rules need the URL form; a bare-host
    // probe to github.com is not a Fincept destination and must stay allowed.
    QVERIFY(ProbeGuard::check(QStringLiteral("https://github.com/Fincept-Corporation/FinceptTerminal"),
                              QStringLiteral("github.com")).rejected);
    QVERIFY(!ProbeGuard::check(QString(), QStringLiteral("github.com")).rejected);

    // A refusal carries the typed error every caller matches on, and names the host.
    const auto d = ProbeGuard::check(QString(), QStringLiteral("api.fincept.in"));
    QVERIFY(fincept::network::HostedPathGuard::is_hosted_unavailable_error(d.error));
    QVERIFY(d.error.contains(QStringLiteral("api.fincept.in")));
}

// The guard must not cost the fork any legitimate connector: these are the
// shapes the Data Sources screen is for.
void TstDirectClients::probe_guard_allows_third_party_connectors() {
    using fincept::network::ProbeGuard;

    QVERIFY(!ProbeGuard::check(QStringLiteral("https://query1.finance.yahoo.com/v8/finance/chart/AAPL"),
                               QStringLiteral("query1.finance.yahoo.com")).rejected);
    QVERIFY(!ProbeGuard::check(QStringLiteral("postgresql://db.internal:5432/market"),
                               QStringLiteral("db.internal")).rejected);
    QVERIFY(!ProbeGuard::check(QString(), QStringLiteral("localhost")).rejected);
    QVERIFY(!ProbeGuard::check(QString(), QStringLiteral("127.0.0.1")).rejected);
    QVERIFY(!ProbeGuard::check(QStringLiteral("https://github.com/QuantConnect/Lean"),
                               QStringLiteral("github.com")).rejected);
    // Look-alikes are not Fincept-owned and must keep working.
    QVERIFY(!ProbeGuard::check(QString(), QStringLiteral("notfincept.in")).rejected);
    QVERIFY(!ProbeGuard::check(QString(), QStringLiteral("fincept-in.example.com")).rejected);
    // Nothing configured at all is not a destination.
    QVERIFY(!ProbeGuard::check(QString(), QString()).rejected);
}

QTEST_GUILESS_MAIN(TstDirectClients)
#include "tst_direct_clients.moc"
