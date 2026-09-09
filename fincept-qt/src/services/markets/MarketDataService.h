#pragma once
#include "core/result/Result.h"
#include "datahub/Producer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <functional>

namespace fincept::services {

struct QuoteData {
    QString symbol;
    QString name;
    double price = 0;
    double change = 0;
    double change_pct = 0;
    double high = 0;
    double low = 0;
    double volume = 0;

    // ── Provenance ───────────────────────────────────────────────────────────
    // FINCEPT_FORK_PLAN.md §4: "the displayed or retained result identifies its
    // source and retrieval status". Appended after the existing members on
    // purpose — every brace-init site in MarketDataService.cpp lists only the
    // eight above, which stays valid aggregate initialisation, so no consumer of
    // this struct has to change to keep compiling.
    QString source;          ///< "yfinance", or "cache (yfinance)" on a cache hit
    qint64 retrieved_at = 0; ///< epoch seconds at which the provider answered
    QString status;          ///< "OK" | "PARTIAL" | "STALE" — see kQuoteStatus* below

    // ── Presence ─────────────────────────────────────────────────────────────
    // yfinance_data.py emits JSON null for a cell the provider did not return,
    // and QJsonValue::toDouble() flattens null, an absent key and a genuine zero
    // to the same 0.0 — which is how a halted session reached the watchlist as a
    // volume of "0". A bare `double` cannot carry that difference, so every
    // numeric field above is paired with the flag that says whether it arrived.
    //
    // Appended after the existing members, like the provenance block above and
    // for the same reason: every brace-init site lists only the eight leading
    // fields, so those stay valid aggregate initialisation and no consumer has
    // to change to keep compiling. A reader that does not ask still sees 0.0,
    // exactly as before; a reader that asks is told the truth.
    bool has_price = false;
    bool has_change = false;
    bool has_change_pct = false;
    bool has_high = false;
    bool has_low = false;
    bool has_volume = false;
};

/// Retrieval status tokens for QuoteData::status. Deliberately untranslated:
/// they are provenance, and have to read the same in a bug report as on screen.
/// "PARTIAL" means at least one has_* flag above came back false, so that value
/// renders as its widget's missing-value placeholder rather than as a number.
/// "STALE" outranks it — a row served after a failed refresh is first of all
/// not current.
inline constexpr const char* kQuoteStatusOk = "OK";
inline constexpr const char* kQuoteStatusPartial = "PARTIAL";
inline constexpr const char* kQuoteStatusStale = "STALE";

struct InfoData {
    QString symbol;
    QString name;
    QString sector;
    QString industry;
    QString country;
    QString currency;
    double market_cap = 0;
    double pe_ratio = 0;
    double forward_pe = 0;
    double price_to_book = 0;
    double dividend_yield = 0;
    double beta = 0;
    double week52_high = 0;
    double week52_low = 0;
    double avg_volume = 0;
    double eps = 0; // revenuePerShare as proxy
    double roe = 0;
    double profit_margin = 0;
    double debt_to_equity = 0;
    double current_ratio = 0;

    // ── Presence ─────────────────────────────────────────────────────────────
    // Same rule as QuoteData above: get_info and get_financial_ratios emit JSON
    // null for a fundamental yfinance did not report, and toDouble() flattens
    // null, an absent key and a genuine zero to the same 0.0 — which is how a
    // missing market cap reached the report builder as "$0". A bare `double`
    // cannot carry that difference, so every numeric field is paired with the
    // flag that says whether it arrived.
    //
    // Appended after the existing members, like the QuoteData and HistoryPoint
    // presence blocks and for the same reason: aggregate initialisation of the
    // leading fields stays valid and no consumer has to change to keep
    // compiling. A reader that does not ask still sees 0.0, exactly as before;
    // a reader that asks is told the truth.
    bool has_market_cap = false;
    bool has_pe_ratio = false;
    bool has_forward_pe = false;
    bool has_price_to_book = false;
    bool has_dividend_yield = false;
    bool has_beta = false;
    bool has_week52_high = false;
    bool has_week52_low = false;
    bool has_avg_volume = false;
    bool has_eps = false;
    bool has_roe = false;
    bool has_profit_margin = false;
    bool has_debt_to_equity = false;
    bool has_current_ratio = false;
};

struct HistoryPoint {
    qint64 timestamp = 0; // seconds since epoch
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    qint64 volume = 0;

    // ── Presence ─────────────────────────────────────────────────────────────
    // Same rule as QuoteData above: yfinance_data.py emits JSON null for an
    // OHLCV cell the provider did not return, and toDouble() flattens null, an
    // absent key and a genuine zero to the same 0.0. On a chart that is worse
    // than on a table — one missing `low` read as 0.0 drags the whole price
    // axis down to zero and squashes the series into a few pixels.
    //
    // `close` has no flag on purpose: a bar with no close is not a price point
    // at all and never reaches this struct (see parse_history_point() in
    // MarketQuoteParse.h), which mirrors equity's Candle.
    //
    // Appended after the existing members, like the two blocks above and for
    // the same reason: no brace-init site has to change to keep compiling, and
    // a reader that does not ask still sees 0.0 exactly as before.
    bool has_open = false;
    bool has_high = false;
    bool has_low = false;
    bool has_volume = false;
};

struct TickerDef {
    QString symbol;
    QString name;
};

struct MarketCategory {
    QString category;
    QStringList tickers;
};

struct RegionalMarket {
    QString region;
    QVector<TickerDef> tickers;
};

/// Fetches market quotes via Python/yfinance.
/// Features:
///   - Request batching: collects symbols over a 100ms window, deduplicates, single Python call
///   - Quote caching: returns cached data immediately, refreshes in background
///   - DataHub producer: owns the `market:quote:*` topic family. Phase 2 —
///     see fincept-qt/docs/datahub-phases/phase-02-market-data-pilot.md.
class MarketDataService : public QObject, public fincept::datahub::Producer {
    Q_OBJECT
  public:
    using QuoteCallback = std::function<void(bool, QVector<QuoteData>)>;

    static MarketDataService& instance();

    /// Register this service as a DataHub producer + install the default
    /// `market:quote:*` policy. Idempotent — safe if called more than once.
    /// Called from main.cpp after `datahub::register_metatypes()`.
    void ensure_registered_with_hub();

    // ── fincept::datahub::Producer ────────────────────────────────────────
    QStringList topic_patterns() const override;
    void refresh(const QStringList& topics) override;
    int max_requests_per_sec() const override;

    /// Fetch quotes — batched and cached. Callback receives filtered results for requested symbols.
    /// Phase 3+: prefer `DataHub::subscribe(this, "market:quote:<sym>", ...)` for streaming widgets.
    /// This callback API remains for one-shot reads (e.g. report builder snapshots).
    void fetch_quotes(const QStringList& symbols, QuoteCallback cb);

    using NewsCallback = std::function<void(bool, QJsonArray)>;
    void fetch_news(const QString& symbol, int count, NewsCallback cb);

    /// Fetch full company info (P/E, 52W range, market cap, ratios, etc.)
    using InfoCallback = std::function<void(bool, InfoData)>;
    void fetch_info(const QString& symbol, InfoCallback cb);

    /// Fetch historical OHLCV data. period: "1mo","3mo","6mo","1y","2y","5y"
    /// interval: "1d","1wk","1mo"
    /// Phase 3+: prefer `DataHub::subscribe(this, "market:history:<sym>:<period>:<interval>", ...)`
    /// for streaming chart widgets. Callback API remains for one-shot reads.
    using HistoryCallback = std::function<void(bool, QVector<HistoryPoint>)>;
    void fetch_history(const QString& symbol, const QString& period, const QString& interval, HistoryCallback cb);

    /// Fetch 5-day hourly sparkline data for multiple symbols in one Python call.
    /// Callback: map of symbol -> list of close prices (chronological).
    /// Phase 3+: prefer `DataHub::subscribe(this, "market:sparkline:<sym>", ...)` for live tables.
    using SparklineCallback = std::function<void(bool, QHash<QString, QVector<double>>)>;
    void fetch_sparklines(const QStringList& symbols, SparklineCallback cb);

    /// Resolve human-readable display names (e.g. "^GSPC" → "S&P 500",
    /// "GC=F" → "Gold") for cryptic tickers so market tables can show names
    /// instead of raw symbols. Names come from yfinance (longName/shortName).
    ///
    /// Names are static, so they are cached to disk: the callback fires once
    /// immediately with whatever is already cached for `symbols`, and — if any
    /// were missing — again after a single `quote_names` Python call resolves
    /// and persists them. The network hit is paid once per symbol, never on
    /// the quote-refresh path.
    using NamesCallback = std::function<void(const QHash<QString, QString>&)>;
    void resolve_names(const QStringList& symbols, NamesCallback cb);

    /// Currency-symbol prefix for a symbol's price (e.g. "AAPL" → "$",
    /// "RELIANCE.NS" → "₹"), or an empty string when unknown or not meaningful
    /// (forex pairs, index levels/yields). Resolved alongside display names by
    /// `resolve_names` and read from the same disk-backed cache, so this is a
    /// cheap synchronous lookup.
    QString currency_prefix(const QString& symbol);

    static QVector<MarketCategory> default_global_markets();
    static QVector<RegionalMarket> default_regional_markets();

    /// Default symbol lists for dashboard widgets
    static QStringList indices_symbols();
    static QStringList forex_symbols();
    static QStringList crypto_symbols();
    static QStringList commodity_symbols();
    static QStringList mover_symbols();
    static QStringList global_snapshot_symbols();

  private:
    MarketDataService();
    void flush_batch();

    /// Internal: publish the per-symbol result to the hub and clear
    /// in_flight for the matching topic. Called from inside `flush_batch`.
    void publish_quote_to_hub(const QuoteData& q);
    void publish_history_to_hub(const QString& symbol, const QString& period, const QString& interval,
                                const QVector<HistoryPoint>& points);
    void publish_sparkline_to_hub(const QString& symbol, const QVector<double>& points);

    // ── Display-name cache (symbol → human-readable name) ──
    // Persisted to SettingsRepository so resolution survives restarts and the
    // per-symbol yfinance .info cost is paid at most once.
    void load_name_cache();
    void persist_name_cache();
    QHash<QString, QString> name_cache_;
    QHash<QString, QString> currency_cache_; // symbol → ISO currency code (e.g. "USD")
    bool name_cache_loaded_ = false;

    // ── Batching ──
    struct PendingRequest {
        QStringList symbols;
        QuoteCallback cb;
    };
    QVector<PendingRequest> pending_;
    bool batch_scheduled_ = false;

    bool hub_registered_ = false;

    // ── Caching — delegated to CacheManager ──
    static constexpr int kQuoteCacheTtlSec = 30;
};

} // namespace fincept::services
