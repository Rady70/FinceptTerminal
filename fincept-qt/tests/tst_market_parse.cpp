// tst_market_parse.cpp — markets quote / history / sparkline / fundamentals
// parsing.
//
// The unit under test is services/markets/MarketQuoteParse.h, the markets-side
// twin of services/equity/EquityQuoteParse.h. It is where the decision "is this
// cell a reading, or is it absent?" is made for every live chart widget on the
// dashboard, for the DataHub `market:*` topics, and for the MCP markets tools
// that hand the same numbers to a model. MarketDataService.cpp itself links
// against the Python runner, the worker, the cache, the settings repository,
// the hub and the logger — which is exactly why the boundary is a header-only
// leaf and why this file needs Qt Core alone (see the HARD RULE at the top of
// CMakeLists.txt).
//
// What is being pinned down:
//   FINCEPT_FORK_PLAN.md §4 — "quote and historical observations display without
//   converting missing or failed fields to zero", and "the displayed or retained
//   result identifies its source and retrieval status".
//
// The equity domain was fixed first; the markets domain kept fabricating zeros
// on the history path, where the damage is worst: one missing `low` read as 0.0
// drags a candlestick chart's price axis to zero behind a plausible-looking
// axis ladder.

#include "screens/markets/QuoteDisplayFormat.h"
#include "screens/report_builder/ReportQuoteFormat.h"
#include "services/markets/MarketQuoteParse.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace fincept::services;

namespace {

QJsonObject obj_from(const char* json) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json), &err);
    Q_ASSERT(err.error == QJsonParseError::NoError);
    return doc.object();
}

QJsonArray array_from(const char* json) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json), &err);
    Q_ASSERT(err.error == QJsonParseError::NoError);
    return doc.array();
}

/// What both history call sites in MarketDataService.cpp do with the helper:
/// keep the bars it accepts, drop the ones it rejects.
QVector<HistoryPoint> parse_series(const QJsonArray& arr) {
    QVector<HistoryPoint> points;
    for (const auto& v : arr) {
        HistoryPoint pt;
        if (parse_history_point(v.toObject(), pt))
            points.append(pt);
    }
    return points;
}

} // namespace

class TstMarketParse : public QObject {
    Q_OBJECT

  private slots:
    void history_drops_closeless_bar_and_marks_missing_fields();
    void history_rejects_a_bar_with_no_timestamp();
    void quote_partial_nulls_keep_their_presence_flags();
    void sparkline_omits_a_non_numeric_close();
    void quote_cache_round_trip_keeps_provenance();
    void info_partial_nulls_keep_their_presence_flags();
    void ratios_partial_nulls_keep_their_presence_flags();
    void eps_reading_survives_across_both_payloads();
    void eps_reading_survives_the_opposite_arrival_order();
    void report_quote_keeps_missing_and_zero_apart();
    void market_cell_format_keeps_missing_and_zero_apart();
};

// ── History ──────────────────────────────────────────────────────────────────

void TstMarketParse::history_drops_closeless_bar_and_marks_missing_fields() {
    const QJsonArray arr = array_from(R"([
        {"timestamp": 1757000000, "open": null, "high": null, "low": null, "close": 1.5, "volume": null},
        {"timestamp": 1757086400, "open": 2, "high": 3, "low": 1, "close": null, "volume": 100},
        {"timestamp": 1757172800, "open": 3, "high": 4, "low": 2.5, "close": 3.5, "volume": 0}
    ])");

    const QVector<HistoryPoint> points = parse_series(arr);

    // The bar with no close is not a price point and is gone entirely; the bar
    // that is missing everything *but* its close is still a price point.
    QCOMPARE(points.size(), qsizetype(2));

    const HistoryPoint& first = points.at(0);
    QCOMPARE(first.timestamp, Q_INT64_C(1757000000));
    QCOMPARE(first.close, 1.5);
    QVERIFY2(!first.has_low, "a null low is an absent reading, not a price of zero");
    QVERIFY(!first.has_open);
    QVERIFY(!first.has_high);
    QVERIFY(!first.has_volume);
    // The stored number is the struct's default, which is precisely why the
    // flag has to exist — see the genuine zero below.
    QCOMPARE(first.low, 0.0);

    const HistoryPoint& last = points.at(1);
    QCOMPARE(last.timestamp, Q_INT64_C(1757172800));
    QCOMPARE(last.close, 3.5);
    QVERIFY(last.has_open);
    QVERIFY(last.has_high);
    QVERIFY(last.has_low);
    QCOMPARE(last.low, 2.5);

    // A day that genuinely traded nothing and a day whose volume never arrived
    // hold the same stored number and differ only in the flag. That is the
    // whole distinction the chart and the MCP payload read.
    QVERIFY2(last.has_volume, "a volume of 0 is a reading, and must survive as one");
    QCOMPARE(last.volume, Q_INT64_C(0));
    QCOMPARE(first.volume, last.volume);
    QVERIFY(first.has_volume != last.has_volume);
}

void TstMarketParse::history_rejects_a_bar_with_no_timestamp() {
    // A point with no time has no position on an axis. The current producer
    // cannot emit one — it `continue`s over closeless rows and stamps every
    // other with int(index.timestamp()) — so this is the guard that keeps a
    // broker payload or an old cache entry from being plotted at the epoch.
    HistoryPoint pt;
    QVERIFY(!parse_history_point(obj_from(R"({"timestamp": null, "close": 1.5})"), pt));
    QVERIFY(!parse_history_point(obj_from(R"({"timestamp": "2025-09-04", "close": 1.5})"), pt));
    QVERIFY(!parse_history_point(obj_from(R"({"close": 1.5})"), pt));

    // …and a bar carrying both is still accepted.
    QVERIFY(parse_history_point(obj_from(R"({"timestamp": 1757000000, "close": 1.5})"), pt));
    QCOMPARE(pt.timestamp, Q_INT64_C(1757000000));
}

// ── Quotes ───────────────────────────────────────────────────────────────────

void TstMarketParse::quote_partial_nulls_keep_their_presence_flags() {
    // "high" is absent entirely, "volume" is the JSON null get_batch_quotes
    // emits for a cell yfinance did not return, and "low" is the wrong type.
    // All three used to arrive as 0.00.
    const QuoteData q = parse_quote_object(obj_from(R"({
        "symbol": "AAPL", "name": "Apple Inc.", "price": 191.24, "change": -1.31,
        "change_percent": -0.68, "low": "n/a", "volume": null
    })"),
                                           QStringLiteral("yfinance"), 1757340000);

    QCOMPARE(q.symbol, QStringLiteral("AAPL"));
    QCOMPARE(q.name, QStringLiteral("Apple Inc."));

    QVERIFY(q.has_price);
    QCOMPARE(q.price, 191.24);
    QVERIFY(q.has_change);
    QVERIFY(q.has_change_pct);

    QVERIFY2(!q.has_high, "an absent key is not a high of zero");
    QVERIFY2(!q.has_low, "a wrong-typed cell is not a low of zero");
    QVERIFY2(!q.has_volume, "a null volume is not a halted session");
    QCOMPARE(q.high, 0.0);
    QCOMPARE(q.low, 0.0);
    QCOMPARE(q.volume, 0.0);

    // The row says so, and names the branch that produced it.
    QCOMPARE(q.status, QString::fromLatin1(kQuoteStatusPartial));
    QCOMPARE(q.source, QStringLiteral("yfinance"));
    QCOMPARE(q.retrieved_at, Q_INT64_C(1757340000));

    // A genuine zero is a reading and reports OK, not PARTIAL.
    const QuoteData zeroed = parse_quote_object(obj_from(R"({
        "symbol": "HALT", "price": 0, "change": 0, "change_percent": 0,
        "high": 0, "low": 0, "volume": 0
    })"),
                                                QStringLiteral("yfinance"), 1757340000);
    QVERIFY(zeroed.has_volume);
    QCOMPARE(zeroed.volume, 0.0);
    QCOMPARE(zeroed.status, QString::fromLatin1(kQuoteStatusOk));
    // Same stored number as the missing one above, opposite presence.
    QCOMPARE(zeroed.volume, q.volume);
    QVERIFY(zeroed.has_volume != q.has_volume);
    // No "name" in the payload: the symbol stands in, rather than an empty row.
    QCOMPARE(zeroed.name, QStringLiteral("HALT"));
}

// ── Sparklines ───────────────────────────────────────────────────────────────

void TstMarketParse::sparkline_omits_a_non_numeric_close() {
    // get_batch_sparklines drops NaNs on the Python side, so nothing
    // non-numeric reaches here today. This is what keeps that true: a null
    // plotted as 0.0 would drag the whole spark to the floor and read as a
    // crash that never happened.
    const QVector<double> prices = parse_sparkline_prices(array_from(R"([191.2, null, 192.4, "n/a", 0, 193.0])"));

    QCOMPARE(prices.size(), qsizetype(4));
    QCOMPARE(prices.at(0), 191.2);
    QCOMPARE(prices.at(1), 192.4);
    // A genuine zero close is still a close and is not filtered out.
    QCOMPARE(prices.at(2), 0.0);
    QCOMPARE(prices.at(3), 193.0);

    QVERIFY(parse_sparkline_prices(array_from("[]")).isEmpty());
}

// ── Provenance ───────────────────────────────────────────────────────────────

void TstMarketParse::quote_cache_round_trip_keeps_provenance() {
    const QuoteData fresh = parse_quote_object(obj_from(R"({
        "symbol": "AAPL", "name": "Apple Inc.", "price": 191.24, "change": -1.31,
        "change_percent": -0.68, "high": 193.0, "low": 190.0, "volume": null
    })"),
                                               QStringLiteral("yfinance"), 1757340000);

    // Into the envelope: an absent reading is written as null, never as 0, so
    // the gap cannot come back out of the cache as a fabricated print.
    const QJsonObject envelope = quote_to_cache(fresh);
    QVERIFY(envelope.value(QLatin1String("volume")).isNull());
    QVERIFY(envelope.value(QLatin1String("price")).isDouble());

    // Back out again: the cache is the immediate source, but it is not what
    // produced the prices, so the row names both.
    const QuoteData cached = quote_from_cache(envelope, /*stale=*/false);
    QCOMPARE(cached.source, QStringLiteral("cache (yfinance)"));
    QCOMPARE(cached.retrieved_at, fresh.retrieved_at);
    QCOMPARE(cached.price, fresh.price);
    QVERIFY(cached.has_price);
    QVERIFY2(!cached.has_volume, "the gap round-trips as a gap");
    QCOMPARE(cached.status, QString::fromLatin1(kQuoteStatusPartial));

    // An envelope written before the sidecar existed can only say "cache", and
    // says exactly that rather than inventing a provider.
    QJsonObject anonymous = envelope;
    anonymous.remove(QLatin1String("source"));
    QCOMPARE(quote_from_cache(anonymous, /*stale=*/false).source, QStringLiteral("cache"));

    // STALE outranks PARTIAL: a row served after a failed refresh is first of
    // all not current.
    QCOMPARE(quote_from_cache(envelope, /*stale=*/true).status, QString::fromLatin1(kQuoteStatusStale));

    // The shared label rule, which the equity service applies to the same
    // question on its own cache hits.
    QCOMPARE(cache_source_label(QStringLiteral("zerodha")), QStringLiteral("cache (zerodha)"));
    QCOMPARE(cache_source_label(QString()), QStringLiteral("cache"));
}

// ── Fundamentals ─────────────────────────────────────────────────────────────

void TstMarketParse::info_partial_nulls_keep_their_presence_flags() {
    // get_info's payload: market_cap is the JSON null the producer emits for a
    // cell yfinance did not return, beta is absent entirely, average_volume is
    // a genuine zero. All three used to read as 0.0 with nothing to tell them
    // apart.
    InfoData info;
    parse_info_object(obj_from(R"({
        "market_cap": null, "fifty_two_week_high": 237.49,
        "fifty_two_week_low": 164.08, "average_volume": 0, "revenue_per_share": 24.68
    })"),
                      info);

    QVERIFY2(!info.has_market_cap, "a null market cap is an absent reading, not a company worth zero");
    QCOMPARE(info.market_cap, 0.0);
    QVERIFY2(!info.has_beta, "an absent key is not a beta of zero");
    QVERIFY(info.has_week52_high);
    QCOMPARE(info.week52_high, 237.49);
    QVERIFY(info.has_week52_low);
    QCOMPARE(info.week52_low, 164.08);
    // A genuine zero volume is a reading and must survive as one.
    QVERIFY2(info.has_avg_volume, "a volume of 0 is a reading");
    QCOMPARE(info.avg_volume, 0.0);
    QVERIFY(info.has_eps);
    QCOMPARE(info.eps, 24.68);
}

void TstMarketParse::ratios_partial_nulls_keep_their_presence_flags() {
    // get_financial_ratios' payload: peRatio is the JSON null for an
    // unreported ratio, currentRatio is a genuine zero (a reported value the
    // provider could in principle print), and the rest are ordinary numbers.
    InfoData info;
    parse_ratios_object(obj_from(R"({
        "peRatio": null, "forwardPE": 31.2, "priceToBook": 51.4,
        "dividendYield": 0.0044, "returnOnEquity": 1.61, "profitMargin": 0.24,
        "debtToEquity": 1.51, "currentRatio": 0, "revenuePerShare": 24.68
    })"),
                        info);

    QVERIFY2(!info.has_pe_ratio, "a null P/E is an absent reading, not a P/E of zero");
    QCOMPARE(info.pe_ratio, 0.0);
    QVERIFY(info.has_forward_pe);
    QVERIFY(info.has_price_to_book);
    QVERIFY(info.has_dividend_yield);
    QVERIFY(info.has_roe);
    QVERIFY(info.has_profit_margin);
    QVERIFY(info.has_debt_to_equity);
    QVERIFY2(info.has_current_ratio, "a current ratio of 0 is a reading");
    QCOMPARE(info.current_ratio, 0.0);
    // Same stored number as the missing ratio above, opposite presence.
    QCOMPARE(info.current_ratio, info.pe_ratio);
    QVERIFY(info.has_current_ratio != info.has_pe_ratio);
    QVERIFY(info.has_eps);
    QCOMPARE(info.eps, 24.68);
}

void TstMarketParse::eps_reading_survives_across_both_payloads() {
    // revenue_per_share (get_info) and revenuePerShare (get_financial_ratios)
    // write the same field. A missing ratio row must not erase a present
    // get_info reading, and a present ratio row still wins on the value.
    InfoData info;
    parse_info_object(obj_from(R"({"revenue_per_share": 24.68})"), info);
    QVERIFY(info.has_eps);
    QCOMPARE(info.eps, 24.68);

    parse_ratios_object(obj_from(R"({"revenuePerShare": null})"), info);
    QVERIFY2(info.has_eps, "a missing ratio row must not erase a present get_info reading");
    QCOMPARE(info.eps, 24.68);

    InfoData ratio_only;
    parse_ratios_object(obj_from(R"({"revenuePerShare": 25.1})"), ratio_only);
    QVERIFY(ratio_only.has_eps);
    QCOMPARE(ratio_only.eps, 25.1);

    InfoData both;
    parse_info_object(obj_from(R"({"revenue_per_share": 24.68})"), both);
    parse_ratios_object(obj_from(R"({"revenuePerShare": 25.1})"), both);
    QVERIFY(both.has_eps);
    QCOMPARE(both.eps, 25.1);
}

void TstMarketParse::eps_reading_survives_the_opposite_arrival_order() {
    // The two Python runs behind fetch_info race, so the ratio payload may be
    // parsed first. The merge rule is symmetric: a present reading overwrites,
    // an absent one never erases — whichever callback lands last.
    InfoData ratios_first;
    parse_ratios_object(obj_from(R"({"revenuePerShare": 25.1})"), ratios_first);
    parse_info_object(obj_from(R"({"revenue_per_share": null})"), ratios_first);
    QVERIFY2(ratios_first.has_eps, "a missing get_info row must not erase a present ratio reading");
    QCOMPARE(ratios_first.eps, 25.1);

    InfoData info_first;
    parse_info_object(obj_from(R"({"revenue_per_share": 24.68})"), info_first);
    parse_ratios_object(obj_from(R"({"revenuePerShare": 25.1})"), info_first);
    QVERIFY(info_first.has_eps);
    QCOMPARE(info_first.eps, 25.1);

    // ...and both absent stays absent, in both orders.
    InfoData none;
    parse_ratios_object(obj_from(R"({"revenuePerShare": null})"), none);
    parse_info_object(obj_from(R"({"revenue_per_share": null})"), none);
    QVERIFY(!none.has_eps);
    QCOMPARE(none.eps, 0.0);
}

// ── Report builder quote config ──────────────────────────────────────────────

void TstMarketParse::report_quote_keeps_missing_and_zero_apart() {
    // The market_data component's config must carry the same missing-vs-zero
    // distinction the live surfaces do (§4): a missing cell is an empty config
    // value the canvas skips, a genuine zero stays a formatted number, and the
    // row's real retrieval status is propagated instead of being flattened to
    // "ok".
    const QuoteData partial = parse_quote_object(obj_from(R"({
        "symbol": "AAPL", "name": "Apple Inc.", "price": null, "change": -1.31,
        "change_percent": -0.68, "high": null, "low": null, "volume": null
    })"),
                                                  QStringLiteral("yfinance"), 1757340000);
    const QMap<QString, QString> pc = fincept::screens::quote_report_config(partial);
    QVERIFY2(pc.value(QStringLiteral("price")).isEmpty(),
             "a missing price is not a price of \"0.00\"");
    QCOMPARE(pc.value(QStringLiteral("change")), QStringLiteral("-1.31"));
    QCOMPARE(pc.value(QStringLiteral("change_pct")), QStringLiteral("-0.68"));
    QVERIFY(pc.value(QStringLiteral("high")).isEmpty());
    QVERIFY(pc.value(QStringLiteral("low")).isEmpty());
    QVERIFY(pc.value(QStringLiteral("volume")).isEmpty());
    QCOMPARE(pc.value(QStringLiteral("status")), QString::fromLatin1(kQuoteStatusPartial));

    // A genuine zero is a reading and renders as one.
    const QuoteData zeroed = parse_quote_object(obj_from(R"({
        "symbol": "HALT", "price": 0, "change": 0, "change_percent": 0,
        "high": 0, "low": 0, "volume": 0
    })"),
                                                QStringLiteral("yfinance"), 1757340000);
    const QMap<QString, QString> zc = fincept::screens::quote_report_config(zeroed);
    QCOMPARE(zc.value(QStringLiteral("price")), QStringLiteral("0.00"));
    QCOMPARE(zc.value(QStringLiteral("high")), QStringLiteral("0.00"));
    QCOMPARE(zc.value(QStringLiteral("volume")), QStringLiteral("0"));
    QCOMPARE(zc.value(QStringLiteral("status")), QString::fromLatin1(kQuoteStatusOk));
}

// ── Markets table cell formatting ────────────────────────────────────────────

void TstMarketParse::market_cell_format_keeps_missing_and_zero_apart() {
    // MarketPanel and ScreenerScreen read QuoteData directly; without the
    // presence flags a missing field prints as "$0.00" / "+0.00%" / "0" — a
    // reading, and an alarming one, for a value that never arrived. These are
    // the helpers both screens now format through.
    const QuoteData partial = parse_quote_object(obj_from(R"({
        "symbol": "AAPL", "name": "Apple Inc.", "price": null, "change": null,
        "change_percent": null, "high": null, "low": null, "volume": null
    })"),
                                                  QStringLiteral("cache (yfinance)"), 1757340000);

    const QString na = QStringLiteral("--");
    QCOMPARE(fincept::screens::quote_field_text(partial.has_price, partial.price, 2, QStringLiteral("$")), na);
    QCOMPARE(fincept::screens::quote_arrow_text(partial.has_change, partial.change, 2), na);
    QCOMPARE(fincept::screens::quote_arrow_text(partial.has_change_pct, partial.change_pct, 2, QStringLiteral("%")),
             na);
    QCOMPARE(fincept::screens::quote_field_text(partial.has_high, partial.high, 2, QStringLiteral("$")), na);
    QCOMPARE(fincept::screens::quote_field_text(partial.has_low, partial.low, 2, QStringLiteral("$")), na);
    QCOMPARE(fincept::screens::quote_volume_text(partial), na);

    // A genuine zero is a reading and renders as one — but with no "+" sign
    // and no up/down arrow, because zero is neither positive nor negative.
    const QuoteData zeroed = parse_quote_object(obj_from(R"({
        "symbol": "HALT", "price": 0, "change": 0, "change_percent": 0,
        "high": 0, "low": 0, "volume": 0
    })"),
                                                 QStringLiteral("yfinance"), 1757340000);
    QCOMPARE(fincept::screens::quote_field_text(zeroed.has_price, zeroed.price, 2, QStringLiteral("$")),
             QStringLiteral("$0.00"));
    QCOMPARE(fincept::screens::quote_signed_text(zeroed.has_change, zeroed.change, 2), QStringLiteral("0.00"));
    QCOMPARE(fincept::screens::quote_signed_text(zeroed.has_change_pct, zeroed.change_pct, 2, QStringLiteral("%")),
             QStringLiteral("0.00%"));
    QCOMPARE(fincept::screens::quote_arrow_text(zeroed.has_change, zeroed.change, 2),
             QString::fromUtf8("\xe2\x80\xa2 0.00"));
    QCOMPARE(fincept::screens::quote_arrow_text(zeroed.has_change_pct, zeroed.change_pct, 2, QStringLiteral("%")),
             QString::fromUtf8("\xe2\x80\xa2 0.00%"));
    // A genuine zero volume is a reading; only a missing volume is "--".
    QCOMPARE(fincept::screens::quote_volume_text(zeroed), QStringLiteral("0"));
    QCOMPARE(fincept::screens::quote_volume_text(partial), na);

    // The sign is added only for a strictly signed value.
    QCOMPARE(fincept::screens::quote_signed_text(true, 1.5, 2, QStringLiteral("%")), QStringLiteral("+1.50%"));
    QCOMPARE(fincept::screens::quote_signed_text(true, -1.5, 2, QStringLiteral("%")), QStringLiteral("-1.50%"));
    QCOMPARE(fincept::screens::quote_arrow_text(true, 1.5, 2), QString::fromUtf8("\xe2\x96\xb2 1.50"));
    QCOMPARE(fincept::screens::quote_arrow_text(true, -1.5, 2), QString::fromUtf8("\xe2\x96\xbc 1.50"));

    // A negative volume is malformed: unavailable, never clamped to a real 0.
    const QuoteData negative_volume = parse_quote_object(obj_from(R"({
        "symbol": "BADV", "price": 10, "change": 0, "change_percent": 0,
        "high": 0, "low": 0, "volume": -5
    })"),
                                                         QStringLiteral("yfinance"), 1757340000);
    QVERIFY2(!negative_volume.has_volume, "a negative volume is unavailable, not a reading");
    QCOMPARE(fincept::screens::quote_volume_text(negative_volume), na);

    // Provenance is appended to a cell's existing tooltip, never replaces it.
    QCOMPARE(fincept::screens::merge_quote_provenance(QStringLiteral("Apple Inc.  (AAPL)"),
                                                      QStringLiteral("Source: yfinance")),
             QStringLiteral("Apple Inc.  (AAPL)\nSource: yfinance"));
    QCOMPARE(fincept::screens::merge_quote_provenance(QString(), QStringLiteral("Source: yfinance")),
             QStringLiteral("Source: yfinance"));

    // Provenance and status travel with the row; a stale row says so.
    const QString partial_prov = fincept::screens::quote_provenance_text(partial);
    QVERIFY(partial_prov.contains(QStringLiteral("cache (yfinance)")));
    QVERIFY(partial_prov.contains(QStringLiteral("Status: PARTIAL")));
    QVERIFY(partial_prov.contains(QStringLiteral("Retrieved: ")));
    QVERIFY(!partial_prov.contains(QStringLiteral("Retrieved: unknown")));

    QuoteData stale = zeroed;
    stale.status = QString::fromLatin1(kQuoteStatusStale);
    const QString stale_prov = fincept::screens::quote_provenance_text(stale);
    QVERIFY(stale_prov.contains(QStringLiteral("Status: STALE")));
    QVERIFY(stale_prov.contains(QStringLiteral("refresh failed")));
}

QTEST_GUILESS_MAIN(TstMarketParse)
#include "tst_market_parse.moc"
