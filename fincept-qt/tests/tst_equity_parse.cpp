// tst_equity_parse.cpp — equity quote / candle parsing.
//
// The unit under test is services/equity/EquityQuoteParse.h, which is where the
// decision "is this cell a reading, or is it absent?" is actually made. It
// exists as a header-only leaf precisely so this test can cover it with Qt Core
// alone — no Python runner, no cache, no network (see the HARD RULE at the top
// of CMakeLists.txt).
//
// What is being pinned down:
//   FINCEPT_FORK_PLAN.md §4 — "quote and historical observations display without
//   converting missing or failed fields to zero", and "the displayed or retained
//   result identifies its source and retrieval status".
//
// QJsonValue::toDouble() answers 0.0 for a null, for an absent key and for a
// wrong-typed cell, which is how a halted session used to render as a genuine
// 0.00 print. Every case below separates those from a real zero.

#include "services/equity/EquityQuoteParse.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace fincept::services::equity;

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

} // namespace

class TstEquityParse : public QObject {
    Q_OBJECT

  private slots:
    void opt_num_distinguishes_missing_from_zero();
    void quote_missing_fields_are_not_zero();
    void quote_genuine_zero_volume_is_present();
    void negative_volume_is_malformed_not_a_reading();
    void candles_keep_missing_volume_and_drop_missing_close();
    void provenance_rides_on_the_result();
};

// ── opt_num ──────────────────────────────────────────────────────────────────

void TstEquityParse::opt_num_distinguishes_missing_from_zero() {
    const QJsonObject o = obj_from(R"({"present": 12.5, "zero": 0, "nulled": null, "typed": "n/a", "flag": true})");

    QVERIFY(opt_num(o, "present").has_value());
    QCOMPARE(*opt_num(o, "present"), 12.5);

    // A real zero is a reading and must survive as one.
    QVERIFY(opt_num(o, "zero").has_value());
    QCOMPARE(*opt_num(o, "zero"), 0.0);

    // …while every flavour of "no reading" stays absent.
    QVERIFY(!opt_num(o, "nulled").has_value());
    QVERIFY(!opt_num(o, "typed").has_value());
    QVERIFY(!opt_num(o, "flag").has_value());
    QVERIFY(!opt_num(o, "absent").has_value());
}

// ── Quotes ───────────────────────────────────────────────────────────────────

void TstEquityParse::quote_missing_fields_are_not_zero() {
    // "high" is absent entirely, "volume" is the JSON null the Python layer now
    // emits for a cell yfinance did not return, and "low" is the wrong type.
    // All three used to arrive as 0.00.
    const QJsonObject o = obj_from(R"({
        "symbol": "AAPL",
        "price": 191.24,
        "change": -1.31,
        "change_percent": -0.68,
        "open": 192.10,
        "low": "n/a",
        "previous_close": 192.55,
        "volume": null,
        "exchange": "NMS",
        "timestamp": 1757340000
    })");

    const QuoteData q = parse_quote_json(o, QStringLiteral("yfinance"));

    // Present fields parse normally.
    QCOMPARE(q.symbol, QStringLiteral("AAPL"));
    QCOMPARE(q.exchange, QStringLiteral("NMS"));
    QVERIFY(q.has_price);
    QCOMPARE(q.price, 191.24);
    QVERIFY(q.has_change);
    QCOMPARE(q.change, -1.31);
    QVERIFY(q.has_change_pct);
    QVERIFY(q.has_open);
    QCOMPARE(q.open, 192.10);
    QVERIFY(q.has_prev_close);
    QCOMPARE(q.prev_close, 192.55);

    // The three broken cells read as MISSING, not as zero.
    QVERIFY2(!q.has_volume, "a JSON null volume must not read as a session that traded nothing");
    QVERIFY2(!q.has_high, "an absent high must not read as a high of 0.00");
    QVERIFY2(!q.has_low, "a wrong-typed low must not read as a low of 0.00");

    // A result with holes in it says so.
    QCOMPARE(q.status, RetrievalStatus::Partial);
}

void TstEquityParse::quote_genuine_zero_volume_is_present() {
    // Every field arrives, and volume is a real, reported zero — a symbol that
    // did not trade. That is an observation, and it has to be distinguishable
    // from the missing volume in the previous case.
    const QJsonObject o = obj_from(R"({
        "symbol": "HALT",
        "price": 10.0,
        "change": 0.0,
        "change_percent": 0.0,
        "open": 10.0,
        "high": 10.0,
        "low": 10.0,
        "previous_close": 10.0,
        "volume": 0,
        "timestamp": 1757340000
    })");

    const QuoteData q = parse_quote_json(o, QStringLiteral("yfinance"));

    QVERIFY2(q.has_volume, "a reported volume of 0 is present, not missing");
    QCOMPARE(q.volume, 0.0);

    // A zero change is likewise a reading, not an absence.
    QVERIFY(q.has_change);
    QCOMPARE(q.change, 0.0);

    // Nothing was missing, so nothing is flagged.
    QCOMPARE(q.status, RetrievalStatus::Ok);

    // The whole point: identical stored value, opposite meaning.
    const QuoteData missing = parse_quote_json(obj_from(R"({"symbol": "HALT", "volume": null})"));
    QCOMPARE(missing.volume, q.volume);
    QVERIFY(missing.has_volume != q.has_volume);
}

void TstEquityParse::negative_volume_is_malformed_not_a_reading() {
    // A count cannot be negative: the malformed cell is recorded as absent (so
    // provenance reports Partial) rather than presented as a complete quote
    // whose UI then has to guess whether negative volume means "unavailable".
    const QuoteData negative = parse_quote_json(obj_from(R"({
        "symbol": "BADV", "price": 10.0, "change": 0.0, "change_percent": 0.0,
        "open": 10.0, "high": 10.0, "low": 10.0, "previous_close": 10.0,
        "volume": -5, "timestamp": 1757340000
    })"),
                                                QStringLiteral("yfinance"));
    QVERIFY2(!negative.has_volume, "a negative volume is unavailable, not a reading");
    QCOMPARE(negative.volume, 0.0);
    QCOMPARE(negative.status, RetrievalStatus::Partial);

    // The same rule holds for candle bars.
    CandleParseStats stats;
    const QVector<Candle> candles = parse_candles_json(
        array_from(R"([{"timestamp": 1757172800, "open": 3.0, "high": 4.0, "low": 2.5, "close": 3.5, "volume": -1}])"),
        &stats);
    QCOMPARE(candles.size(), qsizetype(1));
    QVERIFY2(!candles.at(0).has_volume, "a negative bar volume is unavailable, not zero");
    QCOMPARE(candles.at(0).volume, Q_INT64_C(0));
    QCOMPARE(stats.incomplete, 1);
}

// ── Candles ──────────────────────────────────────────────────────────────────

void TstEquityParse::candles_keep_missing_volume_and_drop_missing_close() {
    const QJsonArray arr = array_from(R"([
        {"timestamp": 1757000000, "open": 1.0, "high": 2.0, "low": 0.5, "close": 1.5, "volume": null},
        {"timestamp": 1757086400, "open": 2.0, "high": 3.0, "low": 1.5, "close": null, "volume": 1000},
        {"timestamp": 1757172800, "open": 3.0, "high": 4.0, "low": 2.5, "close": 3.5, "volume": 2500}
    ])");

    CandleParseStats stats;
    const QVector<Candle> candles = parse_candles_json(arr, &stats);

    // The bar with no close is not a price point and is gone entirely; the bar
    // with no volume is still a price point and is kept.
    QCOMPARE(candles.size(), qsizetype(2));
    QCOMPARE(stats.dropped, 1);
    QCOMPARE(stats.incomplete, 1);

    const Candle& first = candles.at(0);
    QCOMPARE(first.timestamp, Q_INT64_C(1757000000));
    QCOMPARE(first.close, 1.5);
    QVERIFY(first.has_open);
    QVERIFY(first.has_high);
    QVERIFY(first.has_low);
    QVERIFY2(!first.has_volume, "a null-volume bar is kept, with its volume marked missing");
    QCOMPARE(first.volume, Q_INT64_C(0)); // the default value, which is why the flag exists

    const Candle& last = candles.at(1);
    QCOMPARE(last.timestamp, Q_INT64_C(1757172800));
    QCOMPARE(last.close, 3.5);
    QVERIFY(last.has_volume);
    QCOMPARE(last.volume, Q_INT64_C(2500));

    // The series reports that it did not arrive whole.
    const RetrievalMeta meta = candles_meta(QStringLiteral("AAPL"), QStringLiteral("yfinance"), 1757200000,
                                            static_cast<int>(candles.size()), stats);
    QCOMPARE(meta.status, RetrievalStatus::Partial);
    QCOMPARE(meta.point_count, 2);
    QCOMPARE(meta.dropped_count, 1);

    // A whole series reports Ok.
    CandleParseStats clean_stats;
    const QVector<Candle> clean = parse_candles_json(
        array_from(R"([{"timestamp": 1757172800, "open": 3.0, "high": 4.0, "low": 2.5, "close": 3.5, "volume": 25}])"),
        &clean_stats);
    QCOMPARE(clean.size(), qsizetype(1));
    QCOMPARE(clean_stats.dropped, 0);
    QCOMPARE(clean_stats.incomplete, 0);
    QCOMPARE(candles_meta(QStringLiteral("AAPL"), QStringLiteral("yfinance"), 1757200000,
                          static_cast<int>(clean.size()), clean_stats)
                 .status,
             RetrievalStatus::Ok);
}

// ── Provenance ───────────────────────────────────────────────────────────────

void TstEquityParse::provenance_rides_on_the_result() {
    const QJsonObject o = obj_from(R"({
        "symbol": "AAPL", "price": 191.24, "change": -1.31, "change_percent": -0.68,
        "open": 192.10, "high": 193.0, "low": 190.0, "previous_close": 192.55,
        "volume": 41234567, "timestamp": 1757340000
    })");

    // The result names the branch that produced it and when the provider
    // answered — not something the UI has to reconstruct from a log file.
    const QuoteData fresh = parse_quote_json(o, QStringLiteral("yfinance"));
    QCOMPARE(fresh.source, QStringLiteral("yfinance"));
    QCOMPARE(fresh.retrieved_at, Q_INT64_C(1757340000));
    QCOMPARE(fresh.status, RetrievalStatus::Ok);

    // The same payload out of the cache is the same observation from a
    // different source, and it keeps the ORIGINAL retrieval time.
    //
    // A bare "cache" names the shelf and loses the provider, which is what the
    // retrieval-meta sidecar exists to prevent: cache_source_label() is the one
    // rule that turns the recorded origin into the label, and both services
    // apply it. Only an entry written before the sidecar existed has no origin
    // to report, and then "cache" is the whole truth available.
    QCOMPARE(cache_source_label(QStringLiteral("yfinance")), QStringLiteral("cache (yfinance)"));
    QCOMPARE(cache_source_label(QString()), QStringLiteral("cache"));

    const QuoteData cached = parse_quote_json(o, cache_source_label(QStringLiteral("yfinance")));
    QCOMPARE(cached.source, QStringLiteral("cache (yfinance)"));
    QCOMPARE(cached.retrieved_at, fresh.retrieved_at);

    // A provider error envelope is a failed retrieval, not a quote of zeroes.
    const QuoteData failed =
        parse_quote_json(obj_from(R"({"symbol": "NOPE", "error": "No data available"})"), QStringLiteral("yfinance"));
    QCOMPARE(failed.status, RetrievalStatus::Error);
    QCOMPARE(failed.source, QStringLiteral("yfinance"));
    QVERIFY(!failed.has_price);

    // A series carries the same three facts, including a broker id as source.
    CandleParseStats stats;
    const QVector<Candle> candles = parse_candles_json(
        array_from(R"([{"timestamp": 1757172800, "open": 3.0, "high": 4.0, "low": 2.5, "close": 3.5, "volume": 25}])"),
        &stats);
    const RetrievalMeta meta = candles_meta(QStringLiteral("RELIANCE.NS"), QStringLiteral("zerodha"), 1757200000,
                                            static_cast<int>(candles.size()), stats);
    QCOMPARE(meta.symbol, QStringLiteral("RELIANCE.NS"));
    QCOMPARE(meta.source, QStringLiteral("zerodha"));
    QCOMPARE(meta.retrieved_at, Q_INT64_C(1757200000));
    QCOMPARE(meta.status, RetrievalStatus::Ok);

    // An empty series is a failed retrieval, not an empty market.
    CandleParseStats none;
    QCOMPARE(candles_meta(QStringLiteral("AAPL"), QStringLiteral("yfinance"), 1757200000, 0, none).status,
             RetrievalStatus::Error);

    // Status tokens are exactly what the UI prints.
    QCOMPARE(retrieval_status_text(RetrievalStatus::Ok), QStringLiteral("OK"));
    QCOMPARE(retrieval_status_text(RetrievalStatus::Partial), QStringLiteral("PARTIAL"));
    QCOMPARE(retrieval_status_text(RetrievalStatus::Stale), QStringLiteral("STALE"));
    QCOMPARE(retrieval_status_text(RetrievalStatus::Error), QStringLiteral("ERROR"));
}

QTEST_GUILESS_MAIN(TstEquityParse)
#include "tst_equity_parse.moc"
