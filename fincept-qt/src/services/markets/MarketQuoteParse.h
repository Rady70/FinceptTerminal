// src/services/markets/MarketQuoteParse.h
//
// The JSON → model boundary for market quotes, histories and sparklines,
// extracted out of MarketDataService so it can be unit-tested without dragging
// the Python runner, the worker, the cache, the settings repository, the
// DataHub and the logger into the link line (see the HARD RULE at the top of
// tests/CMakeLists.txt).
//
// Header-only for the same reason as services/equity/EquityQuoteParse.h: a new
// .cpp would have to be added to the explicit source list in
// fincept-qt/CMakeLists.txt, and every function here is a pure value transform
// with no state to hide. Leaf by construction — Qt Core only.
#pragma once
#include "services/markets/MarketDataService.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString> // also brings in QLatin1String
#include <QVector>

namespace fincept::services {

/// Read one numeric field, recording whether the payload actually carried it.
/// QJsonValue::toDouble() answers 0.0 for an absent key, for the JSON null
/// yfinance_data.py emits for a cell the provider did not return, and for a
/// genuine zero alike; isDouble() is true for only the last of the three.
///
/// Deliberately a local twin of equity::detail::take_num rather than a call
/// into it: that one writes through the equity domain's own models. Same rule,
/// stated once per boundary.
inline bool take_num(const QJsonObject& o, const char* key, double& out, bool& has) {
    const QJsonValue v = o.value(QLatin1String(key));
    has = v.isDouble();
    if (has)
        out = v.toDouble();
    // `out` otherwise keeps whatever it already held — its 0.0 default at a
    // fresh parse, so a reader that ignores the flag sees exactly what it
    // always saw; an earlier, better value where one has already been written.
    return has;
}

/// A cache hit's source label: "cache (yfinance)" when the envelope still names
/// who produced the bytes, plain "cache" when it does not (an entry written
/// before the envelope carried a source).
///
/// Deliberately a local twin of equity::cache_source_label rather than a call
/// into it, for the same reason take_num is duplicated above: reaching into
/// services/equity/EquityQuoteParse.h would pull EquityResearchModels.h in with
/// it, and the markets domain has no other reason to know the equity models.
/// Same rule, stated once per boundary.
inline QString cache_source_label(const QString& origin) {
    return origin.isEmpty() ? QStringLiteral("cache") : QStringLiteral("cache (%1)").arg(origin);
}

/// get_info numeric fields → InfoData, with per-field presence preserved.
/// What MarketDataService::fetch_info() runs, extracted here for the same
/// reason as the quote/history parsers above: the "is this cell a reading, or
/// is it absent?" decision belongs to the testable boundary, not to the
/// service that links the Python runner.
inline void parse_info_object(const QJsonObject& o, InfoData& info) {
    take_num(o, "market_cap", info.market_cap, info.has_market_cap);
    take_num(o, "beta", info.beta, info.has_beta);
    take_num(o, "fifty_two_week_high", info.week52_high, info.has_week52_high);
    take_num(o, "fifty_two_week_low", info.week52_low, info.has_week52_low);
    take_num(o, "average_volume", info.avg_volume, info.has_avg_volume);
    // eps shares the same rule as the ratio parser below: either payload may
    // arrive first (the two Python runs race), so a present reading overwrites
    // and an absent one never erases — take_num writes the flag
    // unconditionally, which is why the pair is handled on its own.
    double eps = info.eps;
    bool has_eps = false;
    take_num(o, "revenue_per_share", eps, has_eps);
    if (has_eps) {
        info.eps = eps;
        info.has_eps = true;
    }
}

/// get_financial_ratios numeric fields → the same InfoData.
///
/// revenuePerShare is also delivered by get_info as revenue_per_share: the
/// eps pair is handled on its own because take_num writes the flag
/// unconditionally — a missing ratio row would otherwise erase a present
/// get_info reading. Whichever source actually carried the value sets the
/// flag; a reading from the ratio payload still wins on the value, exactly as
/// it did before the flags existed, and the mirror rule in parse_info_object
/// keeps the race (either callback first) symmetric.
inline void parse_ratios_object(const QJsonObject& o, InfoData& info) {
    take_num(o, "peRatio", info.pe_ratio, info.has_pe_ratio);
    take_num(o, "forwardPE", info.forward_pe, info.has_forward_pe);
    take_num(o, "priceToBook", info.price_to_book, info.has_price_to_book);
    take_num(o, "dividendYield", info.dividend_yield, info.has_dividend_yield);
    take_num(o, "returnOnEquity", info.roe, info.has_roe);
    take_num(o, "profitMargin", info.profit_margin, info.has_profit_margin);
    take_num(o, "debtToEquity", info.debt_to_equity, info.has_debt_to_equity);
    take_num(o, "currentRatio", info.current_ratio, info.has_current_ratio);
    double eps = info.eps;
    bool has_eps = false;
    take_num(o, "revenuePerShare", eps, has_eps);
    if (has_eps) {
        info.eps = eps;
        info.has_eps = true;
    }
}

/// One numeric field on its way *into* the cache envelope. Null rather than 0
/// for an absent reading, so a later cache hit cannot resurrect the fabricated
/// zero the flags exist to prevent.
inline QJsonValue num_or_null(double v, bool has) {
    return has ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

/// "OK" unless something is missing. STALE outranks PARTIAL: a row served after
/// a failed refresh is first of all not current.
inline const char* quote_status(int missing, bool stale) {
    if (stale)
        return kQuoteStatusStale;
    return missing > 0 ? kQuoteStatusPartial : kQuoteStatusOk;
}

/// yfinance quote object → QuoteData with per-field presence preserved.
/// Shared by the batch_all and batch_quotes branches, which read the same
/// payload shape and differ only in what they do with the result.
inline QuoteData parse_quote_object(const QJsonObject& q, const QString& source, qint64 retrieved_at) {
    QuoteData out;
    out.symbol = q["symbol"].toString();
    out.name = q["name"].toString(out.symbol);
    int missing = 0;
    missing += take_num(q, "price", out.price, out.has_price) ? 0 : 1;
    missing += take_num(q, "change", out.change, out.has_change) ? 0 : 1;
    missing += take_num(q, "change_percent", out.change_pct, out.has_change_pct) ? 0 : 1;
    missing += take_num(q, "high", out.high, out.has_high) ? 0 : 1;
    missing += take_num(q, "low", out.low, out.has_low) ? 0 : 1;
    missing += take_num(q, "volume", out.volume, out.has_volume) ? 0 : 1;
    // Provenance, stamped at the branch that actually produced the row rather
    // than assumed later from a log line.
    out.source = source;
    out.retrieved_at = retrieved_at;
    out.status = QLatin1String(quote_status(missing, /*stale=*/false));
    return out;
}

/// QuoteData → the cache envelope. Mirrors quote_from_cache below; the two are
/// one round trip and are kept adjacent so they stay that way.
inline QJsonObject quote_to_cache(const QuoteData& q) {
    QJsonObject o;
    o["symbol"] = q.symbol;
    o["name"] = q.name;
    o["price"] = num_or_null(q.price, q.has_price);
    o["change"] = num_or_null(q.change, q.has_change);
    o["change_pct"] = num_or_null(q.change_pct, q.has_change_pct);
    o["high"] = num_or_null(q.high, q.has_high);
    o["low"] = num_or_null(q.low, q.has_low);
    o["volume"] = num_or_null(q.volume, q.has_volume);
    // CacheManager exposes no insert time, so the retrieval time rides in the
    // envelope; without it a cache hit could only ever report when it was read
    // back, which is a different fact.
    o["source"] = q.source;
    o["retrieved_at"] = static_cast<double>(q.retrieved_at);
    return o;
}

/// Rebuild a QuoteData from a cached envelope, carrying its provenance back out.
///
/// The cache is the immediate source, but it is not what produced the prices, so
/// name both. `retrieved_at` comes out of the envelope rather than the read time:
/// CacheManager exposes no insert time (its unified_cache row has a created_at,
/// but nothing reads it back, and ON CONFLICT keeps the *first* insert rather
/// than the latest refresh), so an envelope written before this change simply
/// reports an unknown retrieval time instead of a fabricated one.
inline QuoteData quote_from_cache(const QJsonObject& o, bool stale) {
    QuoteData q;
    q.symbol = o["symbol"].toString();
    q.name = o["name"].toString();

    // An envelope written before num_or_null existed carries a plain double in
    // every slot, so every flag comes back true and the row reads exactly as it
    // did before. One written since round-trips the gap as a gap.
    int missing = 0;
    missing += take_num(o, "price", q.price, q.has_price) ? 0 : 1;
    missing += take_num(o, "change", q.change, q.has_change) ? 0 : 1;
    missing += take_num(o, "change_pct", q.change_pct, q.has_change_pct) ? 0 : 1;
    missing += take_num(o, "high", q.high, q.has_high) ? 0 : 1;
    missing += take_num(o, "low", q.low, q.has_low) ? 0 : 1;
    missing += take_num(o, "volume", q.volume, q.has_volume) ? 0 : 1;

    q.source = cache_source_label(o["source"].toString());
    q.retrieved_at = static_cast<qint64>(o["retrieved_at"].toDouble());
    q.status = QLatin1String(quote_status(missing, stale));
    return q;
}

/// One OHLCV bar → HistoryPoint. Returns false when the bar is not a price
/// point and must be dropped rather than drawn.
///
/// A bar with no close is exactly that: yfinance_data.py already `continue`s
/// over one, and a null close arriving here anyway (a cache entry written
/// before that guard existed) must not become a 0.00 bar — one of those
/// collapses the entire price axis of the chart it lands in. The timestamp is
/// held to the same standard: the current producer cannot emit a closeless or
/// timestampless bar, but a point with no time has no position on an axis, so
/// it is dropped rather than plotted at the epoch.
///
/// O/H/L/volume are kept with their presence recorded — a bar that traded but
/// has no volume print is still a bar. Mirrors parse_candles_json in
/// services/equity/EquityQuoteParse.h.
inline bool parse_history_point(const QJsonObject& o, HistoryPoint& pt) {
    if (!o.value(QLatin1String("timestamp")).isDouble() || !o.value(QLatin1String("close")).isDouble())
        return false;
    pt.timestamp = static_cast<qint64>(o.value(QLatin1String("timestamp")).toDouble());
    pt.close = o.value(QLatin1String("close")).toDouble();
    take_num(o, "open", pt.open, pt.has_open);
    take_num(o, "high", pt.high, pt.has_high);
    take_num(o, "low", pt.low, pt.has_low);
    double volume = 0;
    if (take_num(o, "volume", volume, pt.has_volume))
        pt.volume = static_cast<qint64>(volume);
    return true;
}

/// Sparkline close array → prices. get_batch_sparklines drops NaNs on the
/// Python side (`hist['Close'].dropna()`), so nothing non-numeric reaches here
/// today; the guard is what keeps that true — a null in the array would
/// otherwise be plotted as a 0.0 close and drag the whole spark to the floor.
inline QVector<double> parse_sparkline_prices(const QJsonArray& closes) {
    QVector<double> prices;
    prices.reserve(closes.size());
    for (const auto& c : closes) {
        if (c.isDouble())
            prices.append(c.toDouble());
    }
    return prices;
}

} // namespace fincept::services
