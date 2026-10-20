// tests/tst_dashboard_widget_rules.cpp
//
// MarketLab: the rules the dashboard widgets show their numbers with
// (2026-10-20 widget review):
//   * the quote payload's session date, change reference, open and previous
//     close survive parsing and the cache round trip;
//   * price decimals widen below 1 (0.0793, not 0.08);
//   * the footer names the trading day(s) and the failed / not-refreshed symbols;
//   * one VIX regime rule and one basket-score rule for every widget;
//   * portfolio values are converted into the portfolio currency, minor units
//     (GBp) are scaled, and a holding without a rate is left out, not mixed.
//
// Header-only over Qt Core; no app sources.

#include "screens/dashboard/canvas/DashboardPages.h"
#include "screens/dashboard/widgets/MarketGauges.h"
#include "screens/dashboard/widgets/PortfolioValuation.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "services/markets/MarketQuoteParse.h"

#include <QJsonObject>
#include <QTest>

using namespace fincept::services;
using namespace fincept::screens;

class TestDashboardWidgetRules : public QObject {
    Q_OBJECT

  private slots:
    void quote_payload_carries_session_and_reference() {
        QJsonObject o;
        o["symbol"] = "CL=F";
        o["price"] = 100.68;
        o["change"] = 2.14;
        o["change_percent"] = 2.1717;
        o["high"] = 101.37;
        o["low"] = 98.04;
        o["volume"] = 90954;
        o["open"] = 98.24;
        o["previous_close"] = 98.54;
        o["session_date"] = "2026-10-20";
        o["day_basis"] = "prior_settlement";
        o["contract"] = "CLZ26.NYM";
        const QuoteData q = parse_quote_object(o, "yfinance", 1);
        QVERIFY(q.has_open);
        QCOMPARE(q.open, 98.24);
        QVERIFY(q.has_prev_close);
        QCOMPARE(q.prev_close, 98.54);
        QCOMPARE(q.session_date, QDate(2026, 10, 20));
        QCOMPARE(q.day_basis, QStringLiteral("prior_settlement"));
        QCOMPARE(q.contract, QStringLiteral("CLZ26.NYM"));
        QCOMPARE(q.status, QStringLiteral("OK"));

        const QuoteData back = quote_from_cache(quote_to_cache(q), false);
        QCOMPARE(back.open, q.open);
        QCOMPARE(back.prev_close, q.prev_close);
        QCOMPARE(back.session_date, q.session_date);
        QCOMPARE(back.day_basis, q.day_basis);
        QCOMPARE(back.contract, q.contract);
    }

    void older_payload_leaves_session_unknown() {
        QJsonObject o;
        o["symbol"] = "AAPL";
        o["price"] = 1.0;
        const QuoteData q = parse_quote_object(o, "yfinance", 1);
        QVERIFY(!q.session_date.isValid());
        QVERIFY(!q.has_open);
        QVERIFY(q.day_basis.isEmpty());
    }

    void price_decimals_widen_below_one() {
        // Four significant digits below 1, at any magnitude.
        QCOMPARE(quote_price_decimals(true, 0.2164, 2), 4);
        QCOMPARE(quote_price_decimals(true, 0.0793, 2), 5);
        QCOMPARE(quote_price_decimals(true, -0.5, 2), 4);
        QCOMPARE(quote_price_decimals(true, 0.00000712, 2), 9); // 0.000007120, not 0.0000
        QCOMPARE(quote_price_decimals(true, 1e-12, 2), 10);     // capped
        QCOMPARE(quote_price_decimals(true, 1.12, 4), 4);
        QCOMPARE(quote_price_decimals(true, 342.94, 2), 2);
        QCOMPARE(quote_price_decimals(false, 0.0, 2), 2);
        QCOMPARE(quote_price_decimals(true, 0.0, 2), 2); // a genuine zero keeps the base
        QCOMPARE(QString::number(0.00000712, 'f', quote_price_decimals(true, 0.00000712, 2)),
                 QStringLiteral("0.000007120"));
    }

    void redelivered_value_does_not_clear_a_failure() {
        QuoteData shown;
        shown.retrieved_at = 1000;
        QuoteData same = shown; // the hub's cached copy, re-delivered on re-subscribe
        QVERIFY(!quote_value_clears_error(&shown, same));
        QuoteData newer = shown;
        newer.retrieved_at = 1060;
        QVERIFY(quote_value_clears_error(&shown, newer));
        QVERIFY(quote_value_clears_error(nullptr, same)); // first value
        QuoteData unknown;
        QVERIFY(quote_value_clears_error(&unknown, same)); // retrieval time unknown
    }

    void page_layout_round_trip_and_keys() {
        using namespace fincept::screens::pages;
        GridLayout layout;
        GridItem item;
        item.id = "watchlist";
        item.instance_id = "abc";
        item.cell = {4, 2, 6, 5, 2, 3};
        item.config.insert("symbols", "AAPL");
        layout.items.append(item);
        GridLayout back;
        QVERIFY(decode_layout(encode_layout(layout), back));
        QCOMPARE(back.items.size(), 1);
        QCOMPARE(back.items[0].id, QStringLiteral("watchlist"));
        QCOMPARE(back.items[0].cell.w, 6);
        QCOMPARE(back.items[0].config.value("symbols").toString(), QStringLiteral("AAPL"));
        GridLayout empty;
        QVERIFY(!decode_layout(QString(), empty));
        QVERIFY(!decode_layout(encode_layout(GridLayout{}), empty)); // zero items = nothing stored
        // The first page keeps the single-dashboard key (old layouts open as page 1).
        QCOMPARE(layout_key("main"), QStringLiteral("dashboard_canvas_layout"));
        QCOMPARE(layout_key("p2"), QStringLiteral("dashboard_canvas_layout:p2"));
    }

    void page_list_is_cleaned_and_first_page_kept() {
        using namespace fincept::screens::pages;
        QString current;
        auto list = parse_pages(QString(), "Main", &current); // never saved: one first page
        QCOMPARE(list.size(), 1);
        QCOMPARE(list[0].id, QStringLiteral("main"));
        QCOMPARE(current, QStringLiteral("main"));

        const QString stored = R"({"pages":[{"id":"p2","name":"Research"},{"id":"main","name":"Main"},)"
                               R"({"id":"p2","name":"dup"},{"id":"","name":"x"}],"current":"gone"})";
        list = parse_pages(stored, "Main", &current);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].id, QStringLiteral("main")); // first page always first
        QCOMPARE(list[1].name, QStringLiteral("Research"));
        QCOMPARE(current, QStringLiteral("main")); // a missing current page falls back

        list = parse_pages(pages_json(list, "p2"), "Main", &current);
        QCOMPARE(list.size(), 2);
        QCOMPARE(current, QStringLiteral("p2"));
    }

    void deleting_another_page_keeps_the_shown_one() {
        using namespace fincept::screens::pages;
        // Pages [Main, A, B]; showing Main (0), delete A (1) -> still Main (0).
        QCOMPARE(current_after_delete(1, 0, 2), 0);
        // Showing B (2), delete A (1) -> B is now index 1.
        QCOMPARE(current_after_delete(1, 2, 2), 1);
        // Showing A (1), delete A -> its neighbour (now index 1 = B).
        QCOMPARE(current_after_delete(1, 1, 2), 1);
        // Showing the last page and deleting it -> the new last.
        QCOMPARE(current_after_delete(2, 2, 2), 1);
    }

    void basis_is_described() {
        QuoteData q;
        q.day_basis = "helsinki_midnight";
        QVERIFY(quote_basis_text(q).contains("Helsinki"));
        q.day_basis = "prior_settlement";
        q.contract = "CLZ26.NYM";
        QVERIFY(quote_basis_text(q).contains("CLZ26.NYM"));
        q.day_basis = "prior_bar_close_unverified";
        QVERIFY(quote_basis_text(q).contains("roll"));
    }

    void footer_names_days_failures_and_not_refreshed() {
        QHash<QString, QuoteData> cache;
        QuoteData a;
        a.symbol = "AAPL";
        a.session_date = QDate(2026, 10, 19);
        cache.insert("AAPL", a);
        QuoteData d;
        d.symbol = "^GDAXI";
        d.session_date = QDate(2026, 10, 20);
        cache.insert("^GDAXI", d);
        QHash<QString, QString> errors;
        errors.insert("ZZZZQX", "no data");
        errors.insert("^GDAXI", "timeout"); // value kept, refresh failed
        const auto s = quote_feed_summary({"AAPL", "^GDAXI", "ZZZZQX", "PENDING"}, cache, errors);
        QVERIFY(s.text.contains("19 Oct"));
        QVERIFY(s.text.contains("20 Oct"));
        QCOMPARE(s.failed, QStringList{"ZZZZQX"});
        QCOMPARE(s.not_updated, QStringList{"^GDAXI"});
        QVERIFY(s.warn);
        QCOMPARE(s.settled, 3); // PENDING has neither a value nor a failure
        QVERIFY(s.tooltip.contains("ZZZZQX — failed: no data"));
    }

    void footer_is_quiet_when_all_good() {
        QHash<QString, QuoteData> cache;
        QuoteData a;
        a.session_date = QDate(2026, 10, 19);
        cache.insert("AAPL", a);
        const auto s = quote_feed_summary({"AAPL"}, cache, {});
        QVERIFY(!s.warn);
        QCOMPARE(s.text, QStringLiteral("Data: 19 Oct"));
    }

    void vix_regime_is_one_rule() {
        using namespace fincept::screens::gauges;
        QCOMPARE(vix_regime(14.99), VixRegime::Low);
        QCOMPARE(vix_regime(15.0), VixRegime::Normal);
        QCOMPARE(vix_regime(18.76), VixRegime::Normal);
        QCOMPARE(vix_tone(vix_regime(18.76)), Tone::Neutral); // not a warning at 18.76
        QCOMPARE(vix_regime(20.0), VixRegime::Elevated);
        QCOMPARE(vix_regime(30.0), VixRegime::High);
        QCOMPARE(vix_regime(40.0), VixRegime::Extreme);
    }

    void basket_score_matches_market_pulse_rule() {
        using namespace fincept::screens::gauges;
        QCOMPARE(classify_move(0.31), 1);
        QCOMPARE(classify_move(0.30), 0);
        QCOMPARE(classify_move(-0.31), -1);
        QVERIFY(!basket_score(0, 0, 0, std::nullopt));
        // 9 up, 8 down, 7 flat, VIX 18.76: 50 + (1/24)*50 = 52 → NEUTRAL
        auto s = basket_score(9, 8, 7, 18.76);
        QCOMPARE(s->score, 52);
        QCOMPARE(QString(s->key), QStringLiteral("NEUTRAL"));
        // all down, VIX 35: 0 - 20 → bounded at 0
        s = basket_score(0, 10, 0, 35.0);
        QCOMPARE(s->score, 0);
        QCOMPARE(QString(s->key), QStringLiteral("EXTREME FEAR"));
        // all up, VIX 12: 100 + 10 → bounded at 100
        s = basket_score(10, 0, 0, 12.0);
        QCOMPARE(s->score, 100);
        QCOMPARE(QString(s->key), QStringLiteral("EXTREME GREED"));
    }

    void portfolio_converts_into_portfolio_currency() {
        using namespace fincept::screens::widgets::valuation;
        HoldingInput h;
        h.shares = 10;
        h.avg_cost = 180;
        h.has_price = true;
        h.price = 200;
        h.has_change = true;
        h.change = 2;
        h.quote_currency = "USD";
        // EUR portfolio, EURUSD... the rate passed is EUR per USD (USDEUR=X)
        const auto v = value_holding(h, "EUR", 0.9);
        QVERIFY(v.converted);
        QCOMPARE(v.value, 1800.0);
        QCOMPARE(v.cost, 1620.0);
        QCOMPARE(*v.day, 18.0);
        QCOMPARE(fx_symbol("USD", "EUR"), QStringLiteral("USDEUR=X"));
    }

    void same_currency_needs_no_rate_and_pence_are_scaled() {
        using namespace fincept::screens::widgets::valuation;
        HoldingInput h;
        h.shares = 100;
        h.avg_cost = 2400; // pence
        h.has_price = true;
        h.price = 2450; // pence
        h.quote_currency = "GBp";
        const auto v = value_holding(h, "GBP", std::nullopt);
        QVERIFY(v.converted);
        QCOMPARE(v.value, 2450.0); // £2,450, not £245,000
        QCOMPARE(normalize_currency("GBp").code, QStringLiteral("GBP"));
    }

    void missing_rate_or_currency_is_left_out() {
        using namespace fincept::screens::widgets::valuation;
        HoldingInput h;
        h.shares = 1;
        h.has_price = true;
        h.price = 202.5;
        h.quote_currency = "EUR";
        auto v = value_holding(h, "USD", std::nullopt);
        QVERIFY(!v.converted);
        QVERIFY(v.why_not.contains("EUR"));
        h.quote_currency.clear();
        v = value_holding(h, "USD", 1.1);
        QVERIFY(!v.converted);
    }

    void unpriced_holding_is_valued_at_cost() {
        using namespace fincept::screens::widgets::valuation;
        HoldingInput h;
        h.shares = 2;
        h.avg_cost = 50;
        h.quote_currency = "USD";
        const auto v = value_holding(h, "USD", std::nullopt);
        QVERIFY(v.converted);
        QVERIFY(!v.priced);
        QCOMPARE(v.value, 100.0);
        QVERIFY(!v.day);
    }

    void shares_keep_their_digits() {
        using namespace fincept::screens::widgets::valuation;
        QCOMPARE(shares_text(10), QStringLiteral("10"));
        QCOMPARE(shares_text(0.0012), QStringLiteral("0.0012"));
        QCOMPARE(shares_text(1.5), QStringLiteral("1.5"));
    }
};

QTEST_GUILESS_MAIN(TestDashboardWidgetRules)
#include "tst_dashboard_widget_rules.moc"
