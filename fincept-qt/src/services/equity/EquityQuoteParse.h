// src/services/equity/EquityQuoteParse.h
//
// The JSON → model boundary for equity quotes and candles, extracted out of
// EquityResearchService so it can be unit-tested without dragging the Python
// runner, the cache and the network into the link line (see the HARD RULE at
// the top of tests/CMakeLists.txt).
//
// Header-only on purpose: a new .cpp would have to be added to the explicit
// source list in fincept-qt/CMakeLists.txt, and every function here is a pure
// value transform with no state to hide. Leaf by construction — Qt Core plus
// EquityResearchModels.h, nothing else.
#pragma once
#include "services/equity/EquityResearchModels.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString> // also brings in QLatin1String
#include <QVector>

#include <optional>

namespace fincept::services::equity {

// A JSON null (what the Python layer emits for a cell yfinance did not
// return) and an absent key are both "missing", not zero. QJsonValue::
// toDouble() flattens all three, which is how a halted session used to
// render as a genuine 0.00 print. Returns a value only when the key is
// present AND actually numeric — a wrong-typed cell ("n/a") is missing too.
inline std::optional<double> opt_num(const QJsonObject& o, const char* key) {
    const QLatin1String k(key);
    if (!o.contains(k))
        return std::nullopt;
    const QJsonValue v = o.value(k);
    // isDouble() is false for null, for a string and for a bool, which is the
    // point: every one of those is "no reading", not "the reading was zero".
    if (!v.isDouble())
        return std::nullopt;
    return v.toDouble();
}

/// Outcome counters for parse_candles_json, so the caller can report a
/// truthful Partial without walking the series again.
struct CandleParseStats {
    int dropped = 0;    ///< bars discarded for carrying no close
    int incomplete = 0; ///< bars kept, but missing at least one of O/H/L/volume
};

namespace detail {

/// Assign through one (value, flag) pair; returns whether the field arrived.
inline bool take_num(const QJsonObject& o, const char* key, double& out, bool& has) {
    const auto v = opt_num(o, key);
    if (v) {
        out = *v;
        has = true;
        return true;
    }
    has = false; // `out` keeps its 0.0 default, so existing readers see exactly
                 // what they always saw; readers that ask get told the truth.
    return false;
}

} // namespace detail

/// yfinance quote object → QuoteData with per-field presence preserved.
///
/// `source` is the branch that actually produced the bytes ("cache", a broker
/// id, "yfinance"); it is passed in because only the caller knows which one ran.
/// status is Error for a provider error envelope, Partial when any expected
/// field was missing, otherwise Ok — a caller that detects staleness overrides
/// it. retrieved_at is seeded from the payload's own "timestamp", which the
/// Python layer stamps at fetch time.
inline QuoteData parse_quote_json(const QJsonObject& o, const QString& source = QString()) {
    QuoteData q;
    q.symbol = o.value(QLatin1String("symbol")).toString();
    q.exchange = o.value(QLatin1String("exchange")).toString();
    q.timestamp = static_cast<qint64>(o.value(QLatin1String("timestamp")).toDouble());
    q.source = source;
    q.retrieved_at = q.timestamp;

    int missing = 0;
    missing += detail::take_num(o, "price", q.price, q.has_price) ? 0 : 1;
    missing += detail::take_num(o, "change", q.change, q.has_change) ? 0 : 1;
    missing += detail::take_num(o, "change_percent", q.change_pct, q.has_change_pct) ? 0 : 1;
    missing += detail::take_num(o, "open", q.open, q.has_open) ? 0 : 1;
    missing += detail::take_num(o, "high", q.high, q.has_high) ? 0 : 1;
    missing += detail::take_num(o, "low", q.low, q.has_low) ? 0 : 1;
    missing += detail::take_num(o, "previous_close", q.prev_close, q.has_prev_close) ? 0 : 1;
    missing += detail::take_num(o, "volume", q.volume, q.has_volume) ? 0 : 1;

    // A provider error envelope is not a quote with eight missing fields — it
    // is a failed retrieval, and saying so is the whole point of the status.
    if (o.contains(QLatin1String("error")))
        q.status = RetrievalStatus::Error;
    else if (missing > 0)
        q.status = RetrievalStatus::Partial;
    else
        q.status = RetrievalStatus::Ok;

    return q;
}

/// yfinance candle array → Candle[]. A bar with no close is not a price point
/// and is skipped entirely, matching what the Python side already does.
inline QVector<Candle> parse_candles_json(const QJsonArray& arr, CandleParseStats* stats = nullptr) {
    QVector<Candle> candles;
    candles.reserve(arr.size());
    CandleParseStats local;

    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();

        // A null close arriving here (a broker payload, or a cache entry written
        // before the Python guard existed) must not become a 0.00 bar — one of
        // those collapses the entire price axis of the chart it lands in.
        const auto close = opt_num(o, "close");
        if (!close) {
            ++local.dropped;
            continue;
        }

        Candle c;
        c.timestamp = static_cast<qint64>(o.value(QLatin1String("timestamp")).toDouble());
        c.close = *close;

        int missing = 0;
        missing += detail::take_num(o, "open", c.open, c.has_open) ? 0 : 1;
        missing += detail::take_num(o, "high", c.high, c.has_high) ? 0 : 1;
        missing += detail::take_num(o, "low", c.low, c.has_low) ? 0 : 1;

        if (const auto vol = opt_num(o, "volume")) {
            c.volume = static_cast<qint64>(*vol);
            c.has_volume = true;
        } else {
            c.has_volume = false;
            ++missing;
        }

        if (missing > 0)
            ++local.incomplete;
        candles.append(c);
    }

    if (stats)
        *stats = local;
    return candles;
}

/// Provenance for a parsed series. `source`/`retrieved_at` come from the
/// caller's branch; status is derived from `stats` and whether anything parsed.
inline RetrievalMeta candles_meta(const QString& symbol, const QString& source, qint64 retrieved_at, int point_count,
                                  const CandleParseStats& stats) {
    RetrievalMeta m;
    m.symbol = symbol;
    m.source = source;
    m.retrieved_at = retrieved_at;
    m.point_count = point_count;
    m.dropped_count = stats.dropped;
    if (point_count == 0)
        m.status = RetrievalStatus::Error; // the provider returned no observations
    else if (stats.dropped > 0 || stats.incomplete > 0)
        m.status = RetrievalStatus::Partial;
    else
        m.status = RetrievalStatus::Ok;
    return m;
}

} // namespace fincept::services::equity
