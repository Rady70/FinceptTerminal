// tst_market_parse.cpp — markets quote / history / sparkline parsing.
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

QTEST_GUILESS_MAIN(TstMarketParse)
#include "tst_market_parse.moc"
