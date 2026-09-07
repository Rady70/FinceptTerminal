// tst_marketlab_boundary.cpp — MarketLab Terminal boundary unit tests.
//
// Pure-logic checks for the fork's containment layer:
//   - capability defaults (FINCEPT_FORK_PLAN.md §5.2)
//   - screen availability gating
//   - HostedPathGuard deny-list (FINCEPT_FORK_PLAN.md §5.3)
//
// App-context checks (MCP registration set, broker registry, workflow nodes,
// HttpClient rejection, fork identity) live in
// src/app/MarketLabBoundarySelftest.cpp and run via
// `--selftest-marketlab-boundary`.

#include "core/capability/CapabilityManager.h"
#include "network/http/HostedPathGuard.h"

#include <QTest>
#include <QUrl>

using namespace fincept::capability;
using namespace fincept::network;

class TstMarketlabBoundary : public QObject {
    Q_OBJECT

  private slots:
    void capability_defaults();
    void screen_availability();
    void hosted_path_guard();
};

void TstMarketlabBoundary::capability_defaults() {
    auto& mgr = CapabilityManager::instance();

    QVERIFY(mgr.is_available(Capability::LocalWorkspace));
    QVERIFY(mgr.is_available(Capability::PublicData));
    QVERIFY(mgr.is_available(Capability::UserConfiguredProvider));
    QVERIFY(mgr.is_available(Capability::LocalAnalytics));

    QVERIFY(!mgr.is_available(Capability::FinceptHosted));
    QVERIFY(!mgr.is_available(Capability::CloudSync));
    QVERIFY(!mgr.is_available(Capability::BrokerExecution));

    // BrokerReadOnly is conditional until the read-only IBKR consumer is
    // integrated and qualified (Phase 5) — never "Available" yet.
    const auto ro = mgr.availability(Capability::BrokerReadOnly);
    QCOMPARE(ro.state, AvailabilityState::Conditional);

    // Unavailable capabilities carry a truthful reason.
    QVERIFY(!mgr.availability(Capability::FinceptHosted).reason.isEmpty());
    QVERIFY(!mgr.availability(Capability::BrokerExecution).reason.isEmpty());
}

void TstMarketlabBoundary::screen_availability() {
    auto& mgr = CapabilityManager::instance();

    // Representative-task screens must be reachable.
    QVERIFY(mgr.is_screen_allowed(QStringLiteral("dashboard")));
    QVERIFY(mgr.is_screen_allowed(QStringLiteral("markets")));
    QVERIFY(mgr.is_screen_allowed(QStringLiteral("watchlist")));
    QVERIFY(mgr.is_screen_allowed(QStringLiteral("news")));
    QVERIFY(mgr.is_screen_allowed(QStringLiteral("settings")));
    QVERIFY(mgr.is_screen_allowed(QStringLiteral("about")));

    // Conditional screens stay reachable (they render their own conditional
    // state) and report their condition truthfully.
    for (const QString& id : {QStringLiteral("ai_chat"), QStringLiteral("ai_quant_lab"), QStringLiteral("agent_config"),
                              QStringLiteral("surface_analytics")}) {
        QVERIFY(mgr.is_screen_allowed(id));
        QCOMPARE(mgr.screen_availability(id).state, AvailabilityState::Conditional);
    }

    // Execution and hosted screens must be denied with a reason.
    const QStringList denied = {
        QStringLiteral("equity_trading"), QStringLiteral("algo_trading"),  QStringLiteral("crypto_trading"),
        QStringLiteral("crypto_center"),  QStringLiteral("polymarket"),    QStringLiteral("alpha_arena"),
        QStringLiteral("fno"),            QStringLiteral("quantlib"),      QStringLiteral("maritime"),
        QStringLiteral("forum"),          QStringLiteral("support"),       QStringLiteral("profile"),
    };
    for (const QString& id : denied) {
        QVERIFY2(!mgr.is_screen_allowed(id), qPrintable("denied: " + id));
        const auto avail = mgr.screen_availability(id);
        QCOMPARE(avail.state, AvailabilityState::Unavailable);
        QVERIFY2(!avail.reason.isEmpty(), qPrintable("reason for: " + id));
    }

    // Unknown ids are denied too (fail closed).
    QVERIFY(!mgr.is_screen_allowed(QStringLiteral("no_such_screen")));

    // Filter keeps only allowed ids, in order (Conditional screens are
    // allowed; Unavailable are dropped).
    const QStringList filtered = mgr.allowed_screens(
        {QStringLiteral("equity_trading"), QStringLiteral("markets"), QStringLiteral("quantlib"),
         QStringLiteral("watchlist"), QStringLiteral("ai_chat")});
    QCOMPARE(filtered,
             QStringList({QStringLiteral("markets"), QStringLiteral("watchlist"), QStringLiteral("ai_chat")}));
}

void TstMarketlabBoundary::hosted_path_guard() {
    // Fincept-owned destinations are matched…
    QVERIFY(HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://api.fincept.in/user/profile"))));
    QVERIFY(HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://sub.fincept.in/v1/x"))));
    QVERIFY(HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("wss://api.fincept.in/ws/news"))));
    QVERIFY(HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://markets.fincept.in/api/v1"))));
    QVERIFY(HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://fincept.com/x"))));
    QVERIFY(HostedPathGuard::is_fincept_destination(
        QUrl(QStringLiteral("https://raw.githubusercontent.com/Fincept-Corporation/FinceptTerminal/main/updates.json"))));
    QVERIFY(HostedPathGuard::is_fincept_destination(
        QUrl(QStringLiteral("https://github.com/Fincept-Corporation/FinceptTerminal"))));

    // …third-party destinations are not…
    QVERIFY(!HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://query1.finance.yahoo.com/v8/finance/chart/AAPL"))));
    QVERIFY(!HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://api.db.nomics.world/v22"))));
    QVERIFY(!HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://raw.githubusercontent.com/QuantConnect/Lean/master/x"))));
    QVERIFY(!HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://github.com/QuantConnect/Lean"))));
    // Look-alike hostnames are not fincept domains.
    QVERIFY(!HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://notfincept.in/x"))));
    QVERIFY(!HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("https://fincept-in.example.com/x"))));
    // Non-network schemes (local files, loopback for auth callbacks) are not
    // network destinations.
    QVERIFY(!HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("file:///C:/x"))));
    QVERIFY(!HostedPathGuard::is_fincept_destination(QUrl(QStringLiteral("http://127.0.0.1:8080/cb"))));

    // Typed error: prefix + host.
    const QString err = HostedPathGuard::unavailable_error(QUrl(QStringLiteral("https://api.fincept.in/x")));
    QCOMPARE(err, QStringLiteral("HOSTED_SERVICE_UNAVAILABLE: api.fincept.in"));
    QVERIFY(HostedPathGuard::is_hosted_unavailable_error(err));
    QVERIFY(!HostedPathGuard::is_hosted_unavailable_error(QStringLiteral("HTTP_500")));
}

QTEST_GUILESS_MAIN(TstMarketlabBoundary)
#include "tst_marketlab_boundary.moc"
