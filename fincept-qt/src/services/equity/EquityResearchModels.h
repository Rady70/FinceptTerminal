// src/services/equity/EquityResearchModels.h
#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace fincept::services::equity {

// ── Symbol search ─────────────────────────────────────────────────────────────
struct SearchResult {
    QString symbol;
    QString name;
    QString exchange;
    QString type;
    QString currency;
    QString industry;
};

// ── Retrieval provenance ──────────────────────────────────────────────────────
// FINCEPT_FORK_PLAN.md §4 requires that "the displayed or retained result
// identifies its source and retrieval status". Recovering that afterwards from
// a log file and the Data Sources screen is not the same claim: it says what the
// application *usually* does, not what produced the number on screen. So the
// provenance rides on the result itself, set at the branch that actually
// produced it.
enum class RetrievalStatus {
    Ok,      ///< every requested field came back
    Partial, ///< at least one requested field was missing from the payload
    Stale,   ///< served from cache after its TTL had already elapsed
    Error    ///< the provider reported a failure instead of data
};

/// Short uppercase token for a status line or tooltip. Deliberately not
/// translated: it is a provenance token that has to read the same in a bug
/// report as it does on screen.
inline QString retrieval_status_text(RetrievalStatus s) {
    switch (s) {
        case RetrievalStatus::Ok:
            return QStringLiteral("OK");
        case RetrievalStatus::Partial:
            return QStringLiteral("PARTIAL");
        case RetrievalStatus::Stale:
            return QStringLiteral("STALE");
        case RetrievalStatus::Error:
            return QStringLiteral("ERROR");
    }
    return QStringLiteral("UNKNOWN");
}

/// Provenance for a result that is a series rather than a single struct.
/// Carried alongside QVector<Candle>, which cannot hold it per element.
struct RetrievalMeta {
    QString symbol;
    /// Who actually produced this: "cache", a broker id ("zerodha", "fyers", …),
    /// or "yfinance". Never a guess — set at the branch that returned the data.
    QString source;
    /// Epoch seconds at which the *provider* produced the data. For a cache hit
    /// this is the original retrieval time, not the time the cache was read.
    qint64 retrieved_at = 0;
    RetrievalStatus status = RetrievalStatus::Ok;
    int point_count = 0;   ///< observations retained
    int dropped_count = 0; ///< bars discarded for carrying no close
};

// ── Real-time quote ───────────────────────────────────────────────────────────
struct QuoteData {
    QString symbol;
    double price = 0.0;
    double change = 0.0;
    double change_pct = 0.0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double prev_close = 0.0;
    double volume = 0.0;
    QString exchange;
    qint64 timestamp = 0;

    // ── Presence ─────────────────────────────────────────────────────────────
    // The Python layer emits JSON null for a cell yfinance did not return
    // (halted session, thin book, a ticker with no previousClose). A null and a
    // genuine 0 are different observations and a bare `double` cannot tell them
    // apart, so every numeric field above is paired with the flag that says
    // whether it was actually present.
    //
    // Parallel flags rather than std::optional<double>: these fields are read
    // directly — `q.price`, `q.volume` — by consumers outside this change's
    // reach (src/mcp/tools/EquityResearchTools.cpp), and optional would break
    // every one of them. Flags are additive: existing readers keep compiling and
    // keep seeing 0.0, which is exactly the old behaviour, while readers that
    // care can ask.
    bool has_price = false;
    bool has_change = false;
    bool has_change_pct = false;
    bool has_open = false;
    bool has_high = false;
    bool has_low = false;
    bool has_prev_close = false;
    bool has_volume = false;

    // ── Provenance ───────────────────────────────────────────────────────────
    QString source;          ///< "cache" | broker id | "yfinance"
    qint64 retrieved_at = 0; ///< epoch seconds the provider produced this
    RetrievalStatus status = RetrievalStatus::Ok;
};

// ── Company fundamentals ──────────────────────────────────────────────────────
struct StockInfo {
    QString symbol;
    QString company_name;
    QString sector;
    QString industry;
    QString description;
    QString website;
    QString country;
    QString currency;
    QString exchange;
    int employees = 0;

    // Valuation
    double market_cap = 0.0;
    double enterprise_value = 0.0;
    double pe_ratio = 0.0;
    double forward_pe = 0.0;
    double peg_ratio = 0.0;
    double price_to_book = 0.0;
    double ev_to_revenue = 0.0;
    double ev_to_ebitda = 0.0;

    // Profitability
    double gross_margins = 0.0;
    double operating_margins = 0.0;
    double ebitda_margins = 0.0;
    double profit_margins = 0.0;
    double roe = 0.0;
    double roa = 0.0;
    double gross_profits = 0.0;

    // Per share / cash
    double book_value = 0.0;
    double revenue_per_share = 0.0;
    double free_cashflow = 0.0;
    double operating_cashflow = 0.0;
    double total_cash = 0.0;
    double total_debt = 0.0;
    double total_revenue = 0.0;

    // Growth
    double earnings_growth = 0.0;
    double revenue_growth = 0.0;

    // Share data
    double shares_outstanding = 0.0;
    double float_shares = 0.0;
    double held_insiders_pct = 0.0;
    double held_institutions_pct = 0.0;
    double short_ratio = 0.0;
    double short_pct_of_float = 0.0;

    // Price range / risk
    double week52_high = 0.0;
    double week52_low = 0.0;
    double avg_volume = 0.0;
    double beta = 0.0;
    double dividend_yield = 0.0;
    double current_price = 0.0;

    // Analyst targets
    double target_high = 0.0;
    double target_low = 0.0;
    double target_mean = 0.0;
    double recommendation_mean = 0.0;
    QString recommendation_key;
    int analyst_count = 0;
};

// ── Historical OHLCV candle ───────────────────────────────────────────────────
struct Candle {
    qint64 timestamp = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    qint64 volume = 0;

    // Same reasoning as QuoteData: a bar with no volume print is not a bar that
    // traded nothing. `close` has no flag — a bar without a close is not a price
    // point at all and never reaches this struct (see parse_candles_json).
    bool has_open = false;
    bool has_high = false;
    bool has_low = false;
    bool has_volume = false;
};

// ── Financial statements ──────────────────────────────────────────────────────
// period → line_item → value; stored as raw JSON since yfinance returns
// hundreds of heterogeneous line-item names that vary by company.
struct FinancialsData {
    QString symbol;
    // Each entry: (period_string, QJsonObject of line items)
    QVector<QPair<QString, QJsonObject>> income_statement;
    QVector<QPair<QString, QJsonObject>> balance_sheet;
    QVector<QPair<QString, QJsonObject>> cash_flow;
};

// ── Technical indicator signal ────────────────────────────────────────────────
enum class TechSignal { StrongBuy, Buy, Neutral, Sell, StrongSell };

struct TechIndicator {
    QString name;
    double value = 0.0;
    TechSignal signal = TechSignal::Neutral;
    QString category; // "trend" | "momentum" | "volatility" | "volume"
};

struct TechnicalsData {
    QString symbol;
    QVector<TechIndicator> trend;
    QVector<TechIndicator> momentum;
    QVector<TechIndicator> volatility;
    QVector<TechIndicator> volume;
    TechSignal overall_signal = TechSignal::Neutral;
    int strong_buy = 0;
    int buy = 0;
    int neutral = 0;
    int sell = 0;
    int strong_sell = 0;
};

// ── Peer comparison ───────────────────────────────────────────────────────────
struct PeerData {
    QString symbol;
    QString name;
    QString sector;
    double market_cap = 0.0;
    double pe_ratio = 0.0;
    double forward_pe = 0.0;
    double price_to_book = 0.0;
    double price_to_sales = 0.0;
    double peg_ratio = 0.0;
    double roe = 0.0;
    double roa = 0.0;
    double profit_margin = 0.0;
    double operating_margin = 0.0;
    double gross_margin = 0.0;
    double revenue_growth = 0.0;
    double earnings_growth = 0.0;
    double debt_to_equity = 0.0;
    double current_ratio = 0.0;
    double quick_ratio = 0.0;
    double dividend_yield = 0.0;
    double beta = 0.0;
    double price = 0.0;
    double change_pct = 0.0;
};

// ── News article ──────────────────────────────────────────────────────────────
struct NewsArticle {
    QString title;
    QString description;
    QString url;
    QString publisher;
    QString published_date;
};

// ── Optional market sentiment snapshot ──────────────────────────────────────
struct SentimentSourceSnapshot {
    QString source_id;
    QString label;
    bool available = false;
    double buzz_score = 0.0;
    double bullish_pct = 0.0;
    double sentiment_score = 0.0;
    double activity_count = 0.0;
};

struct MarketSentimentSnapshot {
    QString symbol;
    bool configured = false;
    bool available = false;
    QString status;
    QString message;
    double average_buzz = 0.0;
    double average_bullish_pct = 0.0;
    int coverage = 0;
    QString source_alignment;
    QVector<SentimentSourceSnapshot> sources;
    QString fetched_at;
};

// ── Self-computed (keyless) sentiment ───────────────────────────────────────
// Produced by EquitySentimentService by blending free signals (news headline
// NLP + price/technical momentum, plus optional Adanos when keyed). Replaces
// the Adanos-only view as the default Sentiment tab content.

// One scored headline, for the per-article list.
struct ArticleSentiment {
    QString title;
    QString publisher;
    QString published_date;
    QString url;
    QString label;      // "BULLISH" | "BEARISH" | "NEUTRAL"
    double score = 0.0; // -1..1
};

// One contributing signal in the blend (news / price / adanos).
struct SentimentSource {
    QString id;              // "news" | "price" | "adanos"
    QString label;           // human-readable
    double score = 0.0;      // -1..1
    double weight = 0.0;     // normalized blend weight actually applied (0..1)
    double confidence = 0.0; // 0..1
    bool available = false;
};

struct EquitySentimentSnapshot {
    QString symbol;
    bool available = false;
    QString status; // "ok" | "loading" | "unavailable"
    QString message;
    QString engine;             // "vader" | "lexicon"
    double overall_score = 0.0; // -1..1 blended
    QString label;              // "BULLISH" | "BEARISH" | "NEUTRAL"
    double confidence = 0.0;    // 0..1
    int bullish = 0;
    int bearish = 0;
    int neutral = 0;
    int article_count = 0;
    QVector<SentimentSource> sources;
    QVector<ArticleSentiment> articles;
    QString fetched_at;
};

} // namespace fincept::services::equity
