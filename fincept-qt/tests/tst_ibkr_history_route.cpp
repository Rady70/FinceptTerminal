// tst_ibkr_history_route.cpp — the routing policy of the retained Equity
// Research candle route's optional IBKR provider.
//
// The policy lives in services/ibkr/IbkrHistoryRouting.h so these decisions are
// deterministic and do not depend on the cache, the Python runner or signals:
// IBKR is used only for explicitly routed symbols and bounded periods, an IBKR
// cache hit must carry IBKR provenance, and a routed-history failure clears the
// displayed series instead of leaving old bars under the failed source label.

#include "services/ibkr/IbkrHistoryRouting.h"

#include <QTest>

using namespace fincept::services::ibkr;

class TstIbkrHistoryRoute : public QObject {
    Q_OBJECT

  private slots:
    void routed_symbol_supported_period_uses_ibkr();
    void non_routed_or_unconfigured_symbol_stays_public();
    void unsupported_period_stays_public();
    void ibkr_cache_requires_ibkr_origin();
    void failure_disposition_clears_series();
    void duration_map_stays_within_one_bounded_request();
};

void TstIbkrHistoryRoute::routed_symbol_supported_period_uses_ibkr() {
    for (const char* period : {"1mo", "3mo", "6mo", "1y"}) {
        QCOMPARE(ibkr_history_route(true, true, QString::fromUtf8(period)), IbkrHistoryRoute::Ibkr);
        QVERIFY(!ibkr_duration_for_period(QString::fromUtf8(period)).isEmpty());
    }
}

void TstIbkrHistoryRoute::non_routed_or_unconfigured_symbol_stays_public() {
    QCOMPARE(ibkr_history_route(true, false, QStringLiteral("1y")), IbkrHistoryRoute::PublicProvider);
    QCOMPARE(ibkr_history_route(false, true, QStringLiteral("1y")), IbkrHistoryRoute::PublicProvider);
    QCOMPARE(ibkr_history_route(false, false, QStringLiteral("1y")), IbkrHistoryRoute::PublicProvider);
}

void TstIbkrHistoryRoute::unsupported_period_stays_public() {
    // 2y/5y/10y/max would need pagination the bounded adapter request does not
    // provide; they keep the public-provider path and its own provenance.
    for (const char* period : {"2y", "5y", "10y", "max", "5d", "", "1 Y"}) {
        QCOMPARE(ibkr_history_route(true, true, QString::fromUtf8(period)), IbkrHistoryRoute::PublicProvider);
    }
}

void TstIbkrHistoryRoute::ibkr_cache_requires_ibkr_origin() {
    QVERIFY(ibkr_cache_origin_is_ibkr(QStringLiteral("ibkr_tws")));
    QVERIFY(!ibkr_cache_origin_is_ibkr(QStringLiteral("yfinance")));
    QVERIFY(!ibkr_cache_origin_is_ibkr(QStringLiteral("cache (yfinance)")));
    QVERIFY(!ibkr_cache_origin_is_ibkr(QString()));
}

void TstIbkrHistoryRoute::failure_disposition_clears_series() {
    // A routed IBKR failure reports with an empty series so the previous
    // provider's bars cannot remain plotted under an IBKR error label.
    QCOMPARE(ibkr_history_failure_disposition(), IbkrHistoryFailureDisposition::ClearSeriesAndReport);
}

void TstIbkrHistoryRoute::duration_map_stays_within_one_bounded_request() {
    QVERIFY(ibkr_duration_for_period(QStringLiteral(" 1MO ")) == QLatin1String("1 M"));
    QVERIFY(ibkr_duration_for_period(QStringLiteral("1Y")) == QLatin1String("1 Y"));
    QVERIFY(ibkr_duration_for_period(QStringLiteral("2y")).isEmpty());
}

QTEST_GUILESS_MAIN(TstIbkrHistoryRoute)
#include "tst_ibkr_history_route.moc"
