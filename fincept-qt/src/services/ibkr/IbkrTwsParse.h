// src/services/ibkr/IbkrTwsParse.h
//
// The JSON → model boundary for the optional IBKR TWS read-only consumer.
//
// The Python wrapper (scripts/ibkr_tws_data.py) already normalizes adapter
// payloads once at the connection boundary; this header is the second and last
// place a shape is interpreted, on its way into MarketLab's own models. It is
// header-only over Qt Core plus the header-only markets parsers so it can be
// unit-tested without dragging PythonRunner, the DataHub, the Python process
// machinery or the screens into the link line (tests/CMakeLists.txt HARD RULE).
//
// Central rule (FINCEPT_FORK_PLAN.md §4): a missing IBKR field is missing. It
// never becomes a zero reading, a live classification, or a directional value.
// The wrapper omits fields the adapter did not normalize; QJsonValue::toDouble()
// would flatten an absent key and a genuine zero to the same 0.0, so presence is
// read through the markets-domain `take_num`/`take_volume` helpers exactly as
// the yfinance boundary reads it.
#pragma once
#include "services/markets/MarketDataService.h"
#include "services/markets/MarketQuoteParse.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

namespace fincept::services::ibkr {

/// Source label carried by every result this consumer produces. Deliberately
/// untranslated and stable: it is provenance.
inline constexpr const char* kIbkrSource = "ibkr_tws";

/// Non-secret local configuration. Credentials and account identifiers are
/// never part of this structure (FINCEPT_FORK_PLAN.md §8).
struct IbkrTwsConfig {
    QString config_path;        ///< ignored local JSON file actually read
    QString trading_desk_root;  ///< configured TRADING_DESK checkout
    QString trading_desk_commit;
    QString ibapi_path;
    QString host = QStringLiteral("127.0.0.1");
    int port = 7496;
    int client_id = 71;
    QString tws_version;
    /// Instruments explicitly routed to IBKR. Empty means the optional
    /// provider is not selected for any symbol, so no automatic consumer
    /// routes to it; the qualification self-test and explicit API calls may
    /// still name a symbol directly.
    QStringList symbols;

    bool is_configured() const { return !trading_desk_root.trimmed().isEmpty(); }
};

/// Runtime identity observed for one command, as reported by the adapter.
struct IbkrTwsIdentity {
    QString commit;
    QString adapter_sha256;
    QString ibapi_version;
    QString ibapi_version_source;
    QString ibapi_location;
    QString ibapi_runtime_path_verified;
    QString uses_official_runtime;
    QString tws_version;
    QString tws_version_source;
    QString host;
    int port = 0;
    int client_id = 0;
};

/// How a read completed. `feed` is one of the four IBKR market-data types when
/// IBKR supplied usable data; it is empty for entitlement blocks, timeout, and
/// genuine errors, and `status` then names the failure instead. A delayed feed
/// is never reported as live.
struct IbkrTwsClassification {
    bool usable = false;
    QString feed;        ///< LIVE | FROZEN | DELAYED | DELAYED_FROZEN | HISTORICAL
    QString status;      ///< feed name when usable, else NOT_ENTITLED / NO_VALUE / ERROR / TIMEOUT / NO_DATA / VALUES_INVALID / STALE
    QString entitlement; ///< AVAILABLE | DELAYED | BLOCKED | UNKNOWN
    bool value_present = false;
    bool delayed_fallback = false;
    bool timed_out = false;
    QJsonValue error_code;
    QString error_message;
    QString error_class;
    QString validation_reason; ///< wrapper value/identity validation token, "OK" when it passed
    // Both snapshot attempts stay visible: a deciding live entitlement block
    // must not hide a genuine delayed-attempt failure.
    QString live_status;
    QString live_usable;
    bool delayed_attempted = false;
    QString delayed_status;
    QString delayed_usable;
    QString delayed_validation_reason;
    QJsonValue delayed_error_code;
    QString delayed_error_message;
};

struct IbkrTwsProbeResult {
    bool ok = false;
    IbkrTwsIdentity identity;
    bool connected = false;
    bool ready = false;
    bool clean_disconnect = false;
    QString failure_type;
    QString failure_stage;
    QString failure_message;
};

struct IbkrTwsQuoteResult {
    bool ok = false; ///< the wrapper command completed (even if data is entitlement-blocked)
    QString symbol;
    int con_id = 0;
    QJsonObject contract;
    QuoteData quote;
    IbkrTwsClassification classification;
    IbkrTwsIdentity identity;
    QString failure_type;
    QString failure_stage;
    QString failure_message;
};

struct IbkrTwsHistoryResult {
    bool ok = false;
    QString symbol;
    int con_id = 0;
    QJsonObject contract;
    QVector<HistoryPoint> bars;
    int dropped_bars = 0;
    IbkrTwsClassification classification;
    IbkrTwsIdentity identity;
    QJsonObject parameters;
    QString failure_type;
    QString failure_stage;
    QString failure_message;
};

inline qint64 ibkr_retrieved_at_from_iso(const QString& iso) {
    const QDateTime parsed = QDateTime::fromString(iso, Qt::ISODate);
    return parsed.isValid() ? parsed.toSecsSinceEpoch() : 0;
}

inline IbkrTwsIdentity ibkr_identity_from_json(const QJsonObject& adapter) {
    IbkrTwsIdentity identity;
    identity.commit = adapter.value(QLatin1String("commit")).toString();
    identity.adapter_sha256 = adapter.value(QLatin1String("adapter_sha256")).toString();
    identity.ibapi_version = adapter.value(QLatin1String("ibapi_version")).toString();
    identity.ibapi_version_source = adapter.value(QLatin1String("ibapi_version_source")).toString();
    identity.ibapi_location = adapter.value(QLatin1String("ibapi_location")).toString();
    identity.ibapi_runtime_path_verified = adapter.value(QLatin1String("ibapi_runtime_path_verified")).toString();
    identity.uses_official_runtime = adapter.value(QLatin1String("uses_official_runtime")).toString();
    identity.tws_version = adapter.value(QLatin1String("tws_version")).toString();
    identity.tws_version_source = adapter.value(QLatin1String("tws_version_source")).toString();
    identity.host = adapter.value(QLatin1String("host")).toString();
    identity.port = adapter.value(QLatin1String("port")).toInt();
    identity.client_id = adapter.value(QLatin1String("client_id")).toInt();
    return identity;
}

inline IbkrTwsClassification ibkr_classification_from_json(const QJsonObject& o) {
    IbkrTwsClassification classification;
    classification.usable = o.value(QLatin1String("usable")).toBool(false);
    classification.feed = o.value(QLatin1String("feed")).toString();
    classification.status = o.value(QLatin1String("status")).toString();
    classification.entitlement = o.value(QLatin1String("entitlement")).toString();
    classification.value_present = o.value(QLatin1String("value_present")).toBool(false);
    classification.delayed_fallback = o.value(QLatin1String("delayed_fallback")).toBool(false);
    classification.timed_out = o.value(QLatin1String("timed_out")).toBool(false);
    classification.error_code = o.value(QLatin1String("error_code"));
    classification.error_message = o.value(QLatin1String("error_message")).toString();
    classification.error_class = o.value(QLatin1String("error_class")).toString();
    classification.validation_reason = o.value(QLatin1String("validation_reason")).toString();
    classification.live_status = o.value(QLatin1String("live_market_data_status")).toString();
    classification.live_usable = o.value(QLatin1String("live_usable_market_data")).toString();
    classification.delayed_attempted = o.value(QLatin1String("delayed_attempted")).toBool(false);
    classification.delayed_status = o.value(QLatin1String("delayed_market_data_status")).toString();
    classification.delayed_usable = o.value(QLatin1String("delayed_usable_market_data")).toString();
    classification.delayed_validation_reason = o.value(QLatin1String("delayed_validation_reason")).toString();
    classification.delayed_error_code = o.value(QLatin1String("delayed_error_code"));
    classification.delayed_error_message = o.value(QLatin1String("delayed_error_message")).toString();
    return classification;
}

/// The wrapper's classified outcome → QuoteData carrying provenance.
///
/// A quote snapshot has no high/low and no traded previous close beyond the
/// adapter's `close` tick, so:
///   * price is the observed `last` only (a bid/ask midpoint is a derived
///     number and is deliberately not substituted);
///   * change/change_percent exist only when both `last` and `close` arrived,
///     and are computed from those two observed values;
///   * volume keeps its presence, and a negative volume is malformed, not zero.
inline QuoteData ibkr_quote_from_snapshot(const QJsonObject& snapshot, const IbkrTwsClassification& classification,
                                          qint64 retrieved_at) {
    QuoteData out;
    out.source = QLatin1String(kIbkrSource);
    out.retrieved_at = retrieved_at;
    out.status = classification.status.isEmpty() ? QStringLiteral("UNKNOWN") : classification.status;

    // The wrapper's validation layer already blocks a non-positive/non-finite
    // price, but this boundary must not turn a supplied zero into a reading even
    // if a future payload reaches it without that layer: a price of 0 is not a
    // price, and close=0 must never produce change=last.
    take_num(snapshot, "last", out.price, out.has_price);
    if (out.has_price && !(out.price > 0.0)) {
        out.has_price = false;
        out.price = 0.0;
    }
    double close = 0.0;
    bool has_close = false;
    take_num(snapshot, "close", close, has_close);
    if (has_close && !(close > 0.0))
        has_close = false;
    if (out.has_price && has_close) {
        out.change = out.price - close;
        out.has_change = true;
        out.change_pct = out.change / close * 100.0;
        out.has_change_pct = true;
    }
    double volume = 0.0;
    if (take_volume(snapshot, volume, out.has_volume))
        out.volume = volume;
    // A snapshot request does not carry high/low; their presence stays false so
    // the table renders the missing placeholder rather than a fabricated value.
    return out;
}

/// Bars → HistoryPoint, dropping (not zero-filling) a bar that is not a price
/// point. A bar without a positive timestamp has no position on an axis and a
/// bar without a positive close is not a price; both are counted in `dropped`
/// so the caller can report a partial series truthfully. Zero or negative
/// open/high/low are treated as absent, never plotted as readings.
inline QVector<HistoryPoint> ibkr_history_points(const QJsonArray& bars, int* dropped = nullptr) {
    QVector<HistoryPoint> points;
    points.reserve(bars.size());
    int dropped_count = 0;
    for (const QJsonValue& value : bars) {
        if (!value.isObject()) {
            ++dropped_count;
            continue;
        }
        const QJsonObject bar = value.toObject();
        const double timestamp = bar.value(QLatin1String("timestamp")).toDouble(0.0);
        const double close = bar.value(QLatin1String("close")).toDouble(0.0);
        if (!bar.value(QLatin1String("timestamp")).isDouble() || !bar.value(QLatin1String("close")).isDouble() ||
            !(timestamp > 0.0) || !(close > 0.0)) {
            ++dropped_count;
            continue;
        }
        HistoryPoint point;
        point.timestamp = static_cast<qint64>(timestamp);
        point.close = close;
        auto take_positive = [&bar](const char* key, double& out, bool& has) {
            take_num(bar, key, out, has);
            if (has && !(out > 0.0)) {
                has = false;
                out = 0.0;
            }
        };
        take_positive("open", point.open, point.has_open);
        take_positive("high", point.high, point.has_high);
        take_positive("low", point.low, point.has_low);
        double volume = 0.0;
        if (take_volume(bar, volume, point.has_volume))
            point.volume = static_cast<qint64>(volume);
        points.append(point);
    }
    if (dropped)
        *dropped = dropped_count;
    return points;
}

inline void ibkr_read_failure(const QJsonObject& payload, QString& type, QString& stage, QString& message,
                              const QString& fallback) {
    const QJsonObject failure = payload.value(QLatin1String("failure")).toObject();
    type = failure.value(QLatin1String("type")).toString(QStringLiteral("IBKR_LAUNCH_FAILED"));
    stage = failure.value(QLatin1String("stage")).toString(QStringLiteral("wrapper"));
    message = failure.value(QLatin1String("message")).toString(fallback);
    if (message.isEmpty())
        message = fallback;
}

/// Validate a wrapper envelope before any of its fields are trusted.
///
/// The Phase 5 acceptance contract requires a failure when the output shape is
/// missing or unexpected. A malformed "success" envelope must not reach the
/// model parsers, where absent fields would default to empty/false values and
/// be indistinguishable from a real result. Returns true only when the
/// envelope carries the command-specific required shape.
inline bool ibkr_wrapper_payload_valid(const QJsonObject& payload, const QString& expected_command, QString* reason) {
    auto fail = [reason](const QString& text) {
        if (reason)
            *reason = text;
        return false;
    };

    const QJsonValue source = payload.value(QLatin1String("source"));
    const QJsonValue command = payload.value(QLatin1String("command"));
    const QJsonValue ok_value = payload.value(QLatin1String("ok"));
    if (!source.isString() || source.toString() != QLatin1String(kIbkrSource))
        return fail(QStringLiteral("envelope source is not ibkr_tws"));
    if (!command.isString() || command.toString() != expected_command)
        return fail(QStringLiteral("envelope command does not match the request"));
    if (!ok_value.isBool())
        return fail(QStringLiteral("envelope ok flag is missing or not a boolean"));
    const QJsonValue retrieved = payload.value(QLatin1String("retrieved_at"));
    if (!retrieved.isString() || retrieved.toString().isEmpty())
        return fail(QStringLiteral("envelope has no retrieval timestamp"));

    if (!ok_value.toBool()) {
        const QJsonObject failure = payload.value(QLatin1String("failure")).toObject();
        if (failure.value(QLatin1String("type")).toString().isEmpty() ||
            failure.value(QLatin1String("stage")).toString().isEmpty() ||
            failure.value(QLatin1String("message")).toString().isEmpty())
            return fail(QStringLiteral("failure envelope is missing type, stage, or message"));
        return true;
    }

    const QJsonObject adapter = payload.value(QLatin1String("adapter")).toObject();
    if (adapter.value(QLatin1String("commit")).toString().isEmpty())
        return fail(QStringLiteral("envelope has no adapter commit"));
    if (adapter.value(QLatin1String("ibapi_version")).toString().isEmpty())
        return fail(QStringLiteral("envelope has no observed ibapi version"));
    if (adapter.value(QLatin1String("uses_official_runtime")).toString() != QLatin1String("true"))
        return fail(QStringLiteral("envelope does not report the official runtime"));
    if (adapter.value(QLatin1String("ibapi_runtime_path_verified")).toString() != QLatin1String("true"))
        return fail(QStringLiteral("envelope does not report a verified ibapi path"));

    if (expected_command == QLatin1String("probe")) {
        if (!payload.value(QLatin1String("connected")).isBool() || !payload.value(QLatin1String("ready")).isBool() ||
            !payload.value(QLatin1String("clean_disconnect")).isBool())
            return fail(QStringLiteral("probe envelope is missing connection booleans"));
        return true;
    }

    if (payload.value(QLatin1String("symbol")).toString().isEmpty())
        return fail(QStringLiteral("envelope has no symbol"));

    if (expected_command == QLatin1String("contract")) {
        if (payload.value(QLatin1String("resolution")).toString() != QLatin1String("RESOLVED"))
            return fail(QStringLiteral("contract envelope is not a resolved result"));
        const QJsonObject resolved = payload.value(QLatin1String("resolved")).toObject();
        if (!(resolved.value(QLatin1String("con_id")).toInt(0) > 0))
            return fail(QStringLiteral("contract envelope has no positive conId"));
        if (resolved.value(QLatin1String("currency")).toString().isEmpty() ||
            resolved.value(QLatin1String("security_type")).toString().isEmpty() ||
            resolved.value(QLatin1String("exchange")).toString().isEmpty())
            return fail(QStringLiteral("contract envelope is missing identity fields"));
        return true;
    }

    const QJsonObject contract = payload.value(QLatin1String("contract")).toObject();
    if (!(contract.value(QLatin1String("con_id")).toInt(0) > 0))
        return fail(QStringLiteral("envelope has no resolved contract identity"));
    const QJsonObject classification = payload.value(QLatin1String("classification")).toObject();
    if (!classification.value(QLatin1String("usable")).isBool())
        return fail(QStringLiteral("classification has no usable flag"));
    if (classification.value(QLatin1String("status")).toString().isEmpty())
        return fail(QStringLiteral("classification has no status"));
    if (classification.value(QLatin1String("entitlement")).toString().isEmpty())
        return fail(QStringLiteral("classification has no entitlement"));
    const bool usable = classification.value(QLatin1String("usable")).toBool();

    if (expected_command == QLatin1String("snapshot")) {
        if (!payload.value(QLatin1String("quote")).isObject())
            return fail(QStringLiteral("snapshot envelope has no quote object"));
        if (usable && payload.value(QLatin1String("quote")).toObject().isEmpty())
            return fail(QStringLiteral("usable snapshot envelope has an empty quote"));
        return true;
    }
    if (expected_command == QLatin1String("history")) {
        if (!payload.value(QLatin1String("bars")).isArray())
            return fail(QStringLiteral("history envelope has no bars array"));
        if (usable && payload.value(QLatin1String("bars")).toArray().isEmpty())
            return fail(QStringLiteral("usable history envelope has no bars"));
        if (!usable && !payload.value(QLatin1String("bars")).toArray().isEmpty())
            return fail(QStringLiteral("unusable history envelope carries bars"));
        return true;
    }
    return fail(QStringLiteral("envelope command is not a supported read"));
}

inline IbkrTwsProbeResult ibkr_probe_result_from_payload(const QJsonObject& payload, bool command_ok,
                                                         const QString& error) {
    IbkrTwsProbeResult result;
    result.ok = command_ok && payload.value(QLatin1String("ok")).toBool(false);
    result.identity = ibkr_identity_from_json(payload.value(QLatin1String("adapter")).toObject());
    if (result.ok) {
        result.connected = payload.value(QLatin1String("connected")).toBool(false);
        result.ready = payload.value(QLatin1String("ready")).toBool(false);
        result.clean_disconnect = payload.value(QLatin1String("clean_disconnect")).toBool(false);
    } else {
        ibkr_read_failure(payload, result.failure_type, result.failure_stage, result.failure_message, error);
    }
    return result;
}

inline IbkrTwsQuoteResult ibkr_quote_result_from_payload(const QJsonObject& payload, bool command_ok,
                                                         const QString& error) {
    IbkrTwsQuoteResult result;
    result.ok = command_ok && payload.value(QLatin1String("ok")).toBool(false);
    result.identity = ibkr_identity_from_json(payload.value(QLatin1String("adapter")).toObject());
    if (!result.ok) {
        ibkr_read_failure(payload, result.failure_type, result.failure_stage, result.failure_message, error);
        return result;
    }
    const qint64 retrieved_at = ibkr_retrieved_at_from_iso(payload.value(QLatin1String("retrieved_at")).toString());
    result.symbol = payload.value(QLatin1String("symbol")).toString();
    result.contract = payload.value(QLatin1String("contract")).toObject();
    result.con_id = result.contract.value(QLatin1String("con_id")).toInt();
    result.classification = ibkr_classification_from_json(payload.value(QLatin1String("classification")).toObject());
    result.quote = ibkr_quote_from_snapshot(payload.value(QLatin1String("quote")).toObject(), result.classification,
                                            retrieved_at);
    // The row must carry the symbol it answers for: the watchlist keys its
    // quote map by QuoteData::symbol, and a symbol-less row would render as a
    // placeholder while claiming the fetch succeeded.
    result.quote.symbol = result.symbol;
    return result;
}

inline IbkrTwsHistoryResult ibkr_history_result_from_payload(const QJsonObject& payload, bool command_ok,
                                                            const QString& error) {
    IbkrTwsHistoryResult result;
    result.ok = command_ok && payload.value(QLatin1String("ok")).toBool(false);
    result.identity = ibkr_identity_from_json(payload.value(QLatin1String("adapter")).toObject());
    if (!result.ok) {
        ibkr_read_failure(payload, result.failure_type, result.failure_stage, result.failure_message, error);
        return result;
    }
    result.symbol = payload.value(QLatin1String("symbol")).toString();
    result.contract = payload.value(QLatin1String("contract")).toObject();
    result.con_id = result.contract.value(QLatin1String("con_id")).toInt();
    result.parameters = payload.value(QLatin1String("parameters")).toObject();
    result.classification = ibkr_classification_from_json(payload.value(QLatin1String("classification")).toObject());
    result.bars = ibkr_history_points(payload.value(QLatin1String("bars")).toArray(), &result.dropped_bars);
    return result;
}

} // namespace fincept::services::ibkr
