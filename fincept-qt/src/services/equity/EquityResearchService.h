// src/services/equity/EquityResearchService.h
#pragma once
#include "services/equity/EquityResearchModels.h"

#include <QObject>
#include <QTimer>

#include <functional>

class QNetworkAccessManager;

namespace fincept::services::equity {

/// News source for fetch_news(). Auto = the default GNews → Yahoo chain;
/// NewsApi = newsapi.org (only when a key is configured in Data Sources).
enum class NewsProvider { Auto, NewsApi };

class EquityResearchService : public QObject {
    Q_OBJECT
  public:
    static EquityResearchService& instance();

    // ── Async API — results delivered via signals ─────────────────────────────
    void search_symbols(const QString& query);
    void schedule_search(const QString& query); // debounced entry point

    /// Fetches quote + info + historical in parallel (three Python calls).
    /// Used by the UI panels that need all three. Tool handlers that only
    /// need one slice should use the per-data loaders below to avoid wasting
    /// daemon slots and Yahoo API calls.
    void load_symbol(const QString& symbol, const QString& period = "1y");

    /// Per-data loaders. Each fires exactly one Python call and emits the
    /// matching *_loaded signal. Cache-aware: re-emits immediately on hit.
    void load_quote_only(const QString& symbol);
    void load_info_only(const QString& symbol);
    void load_historical_only(const QString& symbol, const QString& period = "1y");

    void fetch_financials(const QString& symbol);
    void fetch_technicals(const QString& symbol, const QString& period = "1y");
    void fetch_peers(const QString& symbol, const QStringList& peer_symbols);
    void fetch_news(const QString& symbol, int count = 20, NewsProvider provider = NewsProvider::Auto);

    /// The configured & enabled NewsAPI.org key from the Data Sources tab, or an
    /// empty string when none. Used to gate the News-tab provider switch and to
    /// supply the key at fetch time.
    QString configured_newsapi_key() const;

  signals:
    void search_results_loaded(QVector<fincept::services::equity::SearchResult> results);
    void quote_loaded(fincept::services::equity::QuoteData quote);
    void info_loaded(fincept::services::equity::StockInfo info);
    void historical_loaded(QString symbol, QVector<fincept::services::equity::Candle> candles);
    /// Provenance for the series `historical_loaded` is about to deliver —
    /// emitted immediately before it, so a receiver has the source, the
    /// retrieval time and the status in hand when the candles land.
    ///
    /// A separate signal rather than an extra argument or a same-name overload:
    /// `historical_loaded` is connected by member-pointer address in
    /// src/mcp/tools/EquityResearchTools.cpp (two sites), which an overload
    /// would make ambiguous and which this change may not touch. This way no
    /// existing connect() moves.
    void historical_meta_loaded(QString symbol, fincept::services::equity::RetrievalMeta meta);
    void financials_loaded(fincept::services::equity::FinancialsData data);
    void technicals_loaded(fincept::services::equity::TechnicalsData data);
    void peers_loaded(QVector<fincept::services::equity::PeerData> peers);
    void news_loaded(QString symbol, QVector<fincept::services::equity::NewsArticle> articles);
    void error_occurred(QString context, QString message);

  private:
    explicit EquityResearchService(QObject* parent = nullptr);
    Q_DISABLE_COPY(EquityResearchService)

    // ── Python helpers ────────────────────────────────────────────────────────
    void run_python(const QString& script, const QStringList& args, std::function<void(bool, const QString&)> cb);

    // Candle source: cache → region-matched connected broker → yfinance
    // fallback. done(ok, hist_json, meta) is invoked on the main thread; `meta`
    // names which of those three branches actually answered, so no caller has to
    // reconstruct it from the logs.
    void ensure_candles(const QString& symbol, const QString& period,
                        std::function<void(bool, const QString&, const RetrievalMeta&)> done);

    // yfinance news path — the fallback fetch_news() uses when Google News
    // (fetch_company_news.py) is unavailable or returns no articles.
    void fetch_news_yfinance(const QString& symbol, int count);

    // NewsAPI.org path (native HTTP via news_nam_). Used when the equity News tab
    // selects the NewsApi provider and a key is configured. Caches under
    // "equity:news:newsapi:<symbol>"; on any failure (401/quota/network/empty)
    // it falls back to fetch_news(symbol, count, NewsProvider::Auto).
    void fetch_news_newsapi(const QString& symbol, int count, const QString& api_key);

    // ── Parsers ───────────────────────────────────────────────────────────────
    // Quote and candle parsing lives in services/equity/EquityQuoteParse.h: it is
    // pure, it is where the missing-vs-zero decision is made, and keeping it out
    // of this class is what lets tst_equity_parse cover it without linking the
    // Python runner, the cache and the network.
    StockInfo parse_info(const QJsonObject& obj) const;
    FinancialsData parse_financials(const QJsonObject& obj) const;
    TechnicalsData parse_technicals(const QString& symbol, const QJsonArray& rows) const;
    TechSignal score_indicator(const QString& name, double value, double sma20, double sma50) const;
    QVector<PeerData> parse_peers(const QJsonArray& arr) const;
    QVector<NewsArticle> parse_news(const QJsonArray& arr) const;

    // ── Cache TTLs — delegated to CacheManager ───────────────────────────────
    static constexpr int kQuoteTtlSec = 30;
    static constexpr int kInfoTtlSec = 300;
    static constexpr int kHistoricalTtlSec = 120;
    static constexpr int kNewsTtlSec = 180;

    // ── Debounce ──────────────────────────────────────────────────────────────
    static constexpr int kDebounceMs = 350;
    QTimer* search_debounce_ = nullptr;
    QString pending_query_;

    // Dedicated NAM for third-party NewsAPI.org calls — kept separate from the
    // Fincept-backend HttpClient singleton (which carries base-url/session token).
    QNetworkAccessManager* news_nam_ = nullptr;
};

} // namespace fincept::services::equity
