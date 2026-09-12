// tst_ibkr_parse.cpp — the JSON → model boundary of MarketLab's optional
// read-only IBKR TWS consumer (services/ibkr/IbkrTwsParse.h).
//
// The Python wrapper already normalizes adapter payloads once at the
// connection boundary; this header is where they become MarketLab's own
// QuoteData / HistoryPoint without ever converting absence into a reading.
// IbkrTwsService.cpp links PythonRunner and the process machinery, which is
// exactly why the decision lives in a header-only Qt Core leaf and this file
// needs no application sources (tests/CMakeLists.txt HARD RULE).
//
// What is pinned down:
//   * the four IBKR market-data types stay distinguishable and a failure is
//     never reported as a live feed;
//   * a missing snapshot field never becomes a zero, a price, or a change;
//   * a malformed bar is dropped, not plotted at the epoch or at zero;
//   * provenance (source ibkr_tws + retrieval time + status) reaches the
//     consumer result; and
//   * wrapper failure envelopes become typed failures, not empty successes.

#include "services/ibkr/IbkrTwsParse.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace fincept::services;
using namespace fincept::services::ibkr;

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

IbkrTwsClassification usable(const char* feed) {
    IbkrTwsClassification classification;
    classification.usable = true;
    classification.feed = QString::fromUtf8(feed);
    classification.status = classification.feed;
    classification.entitlement = qstrcmp(feed, "DELAYED") == 0 ? QStringLiteral("DELAYED")
                                                               : QStringLiteral("AVAILABLE");
    classification.value_present = true;
    return classification;
}

} // namespace

class TstIbkrParse : public QObject {
    Q_OBJECT

  private slots:
    void identity_reads_observed_runtime_facts();
    void classification_keeps_failed_feeds_distinct();
    void classification_carries_both_attempt_diagnostics();
    void quote_maps_last_against_previous_close();
    void quote_missing_last_is_not_a_price();
    void quote_zero_or_negative_last_is_not_a_price();
    void quote_zero_close_never_becomes_a_change();
    void quote_missing_close_has_no_change();
    void quote_volume_zero_is_a_reading_and_negative_is_not();
    void quote_status_preserves_the_observed_feed();
    void quote_result_carries_the_symbol_it_answers_for();
    void history_drops_bars_without_timestamp_or_close();
    void history_keeps_presence_and_rejects_negative_volume();
    void history_rejects_zero_prices_and_nonpositive_ohlc();
    void failure_envelope_becomes_a_typed_failure();
    void probe_payload_maps_connection_readiness_and_pin();
};

void TstIbkrParse::identity_reads_observed_runtime_facts() {
    const QJsonObject adapter = obj_from(R"({
        "commit": "4a3c606e07f98dce83482c8b1c14bcac2f97b820",
        "adapter_sha256": "ABC",
        "ibapi_version": "10.45.01",
        "ibapi_version_source": "OBSERVED_RUNTIME_FACT",
        "ibapi_location": "C:\\TWS API\\source\\pythonclient",
        "ibapi_runtime_path_verified": "true",
        "uses_official_runtime": "true",
        "tws_version": "10.48.1c",
        "tws_version_source": "WORKSTATION_INVENTORY",
        "host": "127.0.0.1",
        "port": 7496,
        "client_id": 71
    })");
    const IbkrTwsIdentity identity = ibkr_identity_from_json(adapter);
    QCOMPARE(identity.commit, QStringLiteral("4a3c606e07f98dce83482c8b1c14bcac2f97b820"));
    QCOMPARE(identity.ibapi_version, QStringLiteral("10.45.01"));
    QCOMPARE(identity.uses_official_runtime, QStringLiteral("true"));
    QCOMPARE(identity.tws_version, QStringLiteral("10.48.1c"));
    QCOMPARE(identity.port, 7496);
    QCOMPARE(identity.client_id, 71);
}

void TstIbkrParse::classification_keeps_failed_feeds_distinct() {
    const IbkrTwsClassification live = ibkr_classification_from_json(
        obj_from(R"({"usable": true, "feed": "LIVE", "status": "LIVE", "entitlement": "AVAILABLE", "value_present": true})"));
    QCOMPARE(live.feed, QStringLiteral("LIVE"));

    const IbkrTwsClassification delayed = ibkr_classification_from_json(
        obj_from(R"({"usable": true, "feed": "DELAYED", "status": "DELAYED", "entitlement": "DELAYED", "value_present": true, "delayed_fallback": true})"));
    QVERIFY(delayed.usable);
    QCOMPARE(delayed.feed, QStringLiteral("DELAYED"));
    QVERIFY(delayed.delayed_fallback);
    QVERIFY(delayed.feed != live.feed);

    const IbkrTwsClassification blocked = ibkr_classification_from_json(
        obj_from(R"({"usable": false, "feed": null, "status": "NOT_ENTITLED", "entitlement": "BLOCKED", "value_present": false, "error_code": 10089, "error_class": "ENTITLEMENT"})"));
    QVERIFY(!blocked.usable);
    QVERIFY(blocked.feed.isEmpty());
    QCOMPARE(blocked.status, QStringLiteral("NOT_ENTITLED"));
    QCOMPARE(blocked.error_code.toInt(), 10089);

    const IbkrTwsClassification timed_out = ibkr_classification_from_json(
        obj_from(R"({"usable": false, "status": "TIMEOUT", "entitlement": "UNKNOWN", "timed_out": true})"));
    QVERIFY(timed_out.timed_out);

    const IbkrTwsClassification values_invalid = ibkr_classification_from_json(
        obj_from(R"({"usable": false, "status": "VALUES_INVALID", "entitlement": "UNKNOWN", "validation_reason": "BAR_OHLC_RELATION_INVALID"})"));
    QCOMPARE(values_invalid.status, QStringLiteral("VALUES_INVALID"));
    QCOMPARE(values_invalid.validation_reason, QStringLiteral("BAR_OHLC_RELATION_INVALID"));
}

void TstIbkrParse::classification_carries_both_attempt_diagnostics() {
    const IbkrTwsClassification classification = ibkr_classification_from_json(obj_from(R"({
        "usable": false,
        "feed": null,
        "status": "NOT_ENTITLED",
        "entitlement": "BLOCKED",
        "value_present": false,
        "live_market_data_status": "NOT_ENTITLED",
        "live_usable_market_data": "BLOCKED_FOR_ENTITLEMENT",
        "delayed_attempted": true,
        "delayed_market_data_status": "ERROR",
        "delayed_usable_market_data": "FAIL",
        "delayed_error_code": 322,
        "delayed_error_message": "Error processing request"
    })"));
    QVERIFY(!classification.usable);
    QCOMPARE(classification.live_status, QStringLiteral("NOT_ENTITLED"));
    QVERIFY(classification.delayed_attempted);
    QCOMPARE(classification.delayed_status, QStringLiteral("ERROR"));
    QCOMPARE(classification.delayed_error_code.toInt(), 322);
    QCOMPARE(classification.delayed_error_message, QStringLiteral("Error processing request"));
}

void TstIbkrParse::quote_maps_last_against_previous_close() {
    const QuoteData quote =
        ibkr_quote_from_snapshot(obj_from(R"({"last": 226.12, "close": 225.00, "volume": 123456})"),
                                 usable("DELAYED"), 1785974400);
    QCOMPARE(quote.source, QStringLiteral("ibkr_tws"));
    QCOMPARE(quote.retrieved_at, Q_INT64_C(1785974400));
    QCOMPARE(quote.status, QStringLiteral("DELAYED"));
    QVERIFY(quote.has_price);
    QCOMPARE(quote.price, 226.12);
    QVERIFY(quote.has_change);
    QVERIFY(qAbs(quote.change - 1.12) < 1e-9);
    QVERIFY(quote.has_change_pct);
    QVERIFY(qAbs(quote.change_pct - (1.12 / 225.0 * 100.0)) < 1e-9);
    QVERIFY(quote.has_volume);
    QCOMPARE(quote.volume, 123456.0);
    // A snapshot request carries no high/low; they stay missing, not zero.
    QVERIFY(!quote.has_high);
    QVERIFY(!quote.has_low);
}

void TstIbkrParse::quote_missing_last_is_not_a_price() {
    // Delayed snapshots can carry a book without a trade; the book is not a
    // last price and no derived substitute is allowed to appear in PRICE.
    const QuoteData quote =
        ibkr_quote_from_snapshot(obj_from(R"({"bid": 226.10, "ask": 226.14, "close": 225.00})"),
                                 usable("DELAYED"), 1785974400);
    QVERIFY(!quote.has_price);
    QVERIFY(!quote.has_change);
    QVERIFY(!quote.has_change_pct);
    QVERIFY(!quote.has_volume);
    QCOMPARE(quote.price, 0.0);
}

void TstIbkrParse::quote_missing_close_has_no_change() {
    const QuoteData quote = ibkr_quote_from_snapshot(obj_from(R"({"last": 226.12})"), usable("LIVE"), 1785974400);
    QVERIFY(quote.has_price);
    QVERIFY(!quote.has_change);
    QVERIFY(!quote.has_change_pct);
}

void TstIbkrParse::quote_zero_or_negative_last_is_not_a_price() {
    // A zero or negative last is not a reading; it must not appear in PRICE or
    // produce a directional change even if a payload reaches this boundary
    // without the wrapper's validation layer.
    const QuoteData zero = ibkr_quote_from_snapshot(obj_from(R"({"last": 0.0, "close": 225.0})"), usable("DELAYED"), 1);
    QVERIFY(!zero.has_price);
    QVERIFY(!zero.has_change);
    QCOMPARE(zero.price, 0.0);

    const QuoteData negative =
        ibkr_quote_from_snapshot(obj_from(R"({"last": -2.0, "close": 225.0})"), usable("DELAYED"), 1);
    QVERIFY(!negative.has_price);
    QVERIFY(!negative.has_change);
}

void TstIbkrParse::quote_zero_close_never_becomes_a_change() {
    // The reference snapshot validator rejects a non-positive close; without
    // it, last=100, close=0 would fabricate change=+100.
    const QuoteData quote =
        ibkr_quote_from_snapshot(obj_from(R"({"last": 100.0, "close": 0.0})"), usable("DELAYED"), 1);
    QVERIFY(quote.has_price);
    QVERIFY(!quote.has_change);
    QVERIFY(!quote.has_change_pct);
    QCOMPARE(quote.change, 0.0);
}

void TstIbkrParse::quote_volume_zero_is_a_reading_and_negative_is_not() {
    const QuoteData zero = ibkr_quote_from_snapshot(obj_from(R"({"last": 1.0, "volume": 0})"), usable("LIVE"), 1);
    QVERIFY(zero.has_volume);
    QCOMPARE(zero.volume, 0.0);

    const QuoteData negative =
        ibkr_quote_from_snapshot(obj_from(R"({"last": 1.0, "volume": -5})"), usable("LIVE"), 1);
    QVERIFY(!negative.has_volume);
    QCOMPARE(negative.volume, 0.0);
}

void TstIbkrParse::quote_status_preserves_the_observed_feed() {
    for (const char* feed : {"LIVE", "FROZEN", "DELAYED", "DELAYED_FROZEN"}) {
        const QuoteData quote = ibkr_quote_from_snapshot(obj_from(R"({"last": 1.0})"), usable(feed), 1);
        QCOMPARE(quote.status, QString::fromUtf8(feed));
    }
}

void TstIbkrParse::quote_result_carries_the_symbol_it_answers_for() {
    // The watchlist keys its quote map by QuoteData::symbol; a successful
    // result without the symbol renders as a placeholder row instead.
    const QJsonObject payload = obj_from(R"({
        "source": "ibkr_tws",
        "command": "snapshot",
        "ok": true,
        "retrieved_at": "2026-09-12T12:13:05Z",
        "symbol": "AAPL",
        "contract": {"con_id": 265598, "symbol": "AAPL", "currency": "USD"},
        "classification": {"usable": true, "feed": "DELAYED", "status": "DELAYED", "entitlement": "DELAYED", "value_present": true},
        "quote": {"last": 332.58, "close": 326.57, "volume": 1267936.0}
    })");
    const IbkrTwsQuoteResult result = ibkr_quote_result_from_payload(payload, true, QString());
    QVERIFY(result.ok);
    QCOMPARE(result.con_id, 265598);
    QCOMPARE(result.quote.symbol, QStringLiteral("AAPL"));
    QVERIFY(result.quote.has_price);
    QVERIFY(result.quote.has_change);
}

void TstIbkrParse::history_drops_bars_without_timestamp_or_close() {
    const QJsonArray bars = array_from(R"([
        {"date": "20260806", "timestamp": 1785974400, "open": 224.5, "high": 227.0, "low": 223.9, "close": 226.0, "volume": 1000.5},
        {"date": "20260807", "open": 226.0, "high": 228.4, "low": 225.1, "close": 227.3},
        {"date": "20260808", "timestamp": 1786147200, "open": 227.3, "high": 229.0, "low": 226.0, "close": null}
    ])");
    int dropped = 0;
    const QVector<HistoryPoint> points = ibkr_history_points(bars, &dropped);
    QCOMPARE(points.size(), 1);
    QCOMPARE(dropped, 2);
    QCOMPARE(points.first().timestamp, Q_INT64_C(1785974400));
    QCOMPARE(points.first().close, 226.0);
    QVERIFY(points.first().has_open);
    QVERIFY(points.first().has_high);
    QVERIFY(points.first().has_low);
    QVERIFY(points.first().has_volume);
}

void TstIbkrParse::history_rejects_zero_prices_and_nonpositive_ohlc() {
    const QJsonArray bars = array_from(R"([
        {"timestamp": 0, "open": 1.0, "high": 1.2, "low": 0.9, "close": 1.0, "volume": 1},
        {"timestamp": 1785974400, "open": 1.0, "high": 1.2, "low": 0.9, "close": 0.0, "volume": 1},
        {"timestamp": 1786060800, "open": 0.0, "high": 1.2, "low": 0.9, "close": 1.1, "volume": 1},
        {"timestamp": 1786147200, "open": -1.0, "high": 1.2, "low": 0.9, "close": 1.1, "volume": 1},
        {"timestamp": 1786233600, "open": 1.0, "high": 1.2, "low": 0.9, "close": 1.1, "volume": 1}
    ])");
    int dropped = 0;
    const QVector<HistoryPoint> points = ibkr_history_points(bars, &dropped);
    // A non-positive timestamp or close is not a price point and drops the
    // bar; a non-positive open/high/low keeps the bar but records the field as
    // absent, never as a 0.0 reading.
    QCOMPARE(points.size(), 3);
    QCOMPARE(dropped, 2);
    QVERIFY(!points[0].has_open);
    QCOMPARE(points[0].open, 0.0);
    QVERIFY(!points[1].has_open);
    QCOMPARE(points[1].open, 0.0);
    QCOMPARE(points[2].timestamp, Q_INT64_C(1786233600));
    QVERIFY(points[2].has_open);
    QVERIFY(points[2].has_high);
    QVERIFY(points[2].has_low);
}

void TstIbkrParse::history_keeps_presence_and_rejects_negative_volume() {
    const QJsonArray bars = array_from(R"([
        {"timestamp": 1785974400, "open": null, "high": null, "low": null, "close": 226.0, "volume": -1},
        {"timestamp": 1786060800, "open": 226.0, "high": 228.4, "low": 225.1, "close": 227.3, "volume": 0}
    ])");
    int dropped = 0;
    const QVector<HistoryPoint> points = ibkr_history_points(bars, &dropped);
    QCOMPARE(points.size(), 2);
    QCOMPARE(dropped, 0);
    QVERIFY(!points[0].has_open && !points[0].has_high && !points[0].has_low);
    QVERIFY(!points[0].has_volume);
    QVERIFY(points[1].has_volume);
    QCOMPARE(points[1].volume, Q_INT64_C(0));
}

void TstIbkrParse::failure_envelope_becomes_a_typed_failure() {
    const QJsonObject payload = obj_from(R"({
        "source": "ibkr_tws",
        "command": "snapshot",
        "ok": false,
        "failure": {"type": "IBKR_ADAPTER_PIN_MISMATCH", "stage": "pin", "message": "checkout is not pinned"}
    })");
    const IbkrTwsQuoteResult quote = ibkr_quote_result_from_payload(payload, false, QString());
    QVERIFY(!quote.ok);
    QCOMPARE(quote.failure_type, QStringLiteral("IBKR_ADAPTER_PIN_MISMATCH"));
    QCOMPARE(quote.failure_stage, QStringLiteral("pin"));
    QCOMPARE(quote.failure_message, QStringLiteral("checkout is not pinned"));
    QVERIFY(!quote.quote.has_price);

    // A launcher-level failure with no wrapper payload keeps the process error.
    const IbkrTwsHistoryResult history = ibkr_history_result_from_payload({}, false, QStringLiteral("Python not available"));
    QVERIFY(!history.ok);
    QCOMPARE(history.failure_type, QStringLiteral("IBKR_LAUNCH_FAILED"));
    QCOMPARE(history.failure_message, QStringLiteral("Python not available"));
}

void TstIbkrParse::probe_payload_maps_connection_readiness_and_pin() {
    const QJsonObject payload = obj_from(R"({
        "source": "ibkr_tws",
        "command": "probe",
        "ok": true,
        "adapter": {"commit": "abc", "uses_official_runtime": "true", "host": "127.0.0.1", "port": 7496, "client_id": 71},
        "connected": true,
        "ready": true,
        "clean_disconnect": true
    })");
    const IbkrTwsProbeResult probe = ibkr_probe_result_from_payload(payload, true, QString());
    QVERIFY(probe.ok);
    QVERIFY(probe.connected && probe.ready && probe.clean_disconnect);
    QCOMPARE(probe.identity.commit, QStringLiteral("abc"));
    QCOMPARE(probe.identity.port, 7496);
}

QTEST_GUILESS_MAIN(TstIbkrParse)
#include "tst_ibkr_parse.moc"
