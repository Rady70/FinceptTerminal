// src/services/ibkr/IbkrTwsService.h
//
// MarketLab's thin consumer for the optional read-only IBKR TWS market-data
// provider (FINCEPT_FORK_PLAN.md §8, Phase 5).
//
// This service never talks to TWS itself. It invokes the project-owned Python
// wrapper (scripts/ibkr_tws_data.py) through the existing bounded
// PythonRunner process route; the wrapper imports the unchanged TRADING_DESK
// adapter from the configured checkout. There is deliberately no daemon, no
// connection pool, and no fallback to another market-data provider: a failed
// IBKR request stays an IBKR failure.
//
// The service exposes market-data reads only. It has no account, order,
// position, portfolio, or execution surface and must never gain one.
#pragma once
#include "services/ibkr/IbkrTwsParse.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace fincept::services::ibkr {

class IbkrTwsService : public QObject {
    Q_OBJECT
  public:
    using ProbeCallback = std::function<void(const IbkrTwsProbeResult&)>;
    using QuoteCallback = std::function<void(const IbkrTwsQuoteResult&)>;
    using HistoryCallback = std::function<void(const IbkrTwsHistoryResult&)>;

    static IbkrTwsService& instance();

    /// True when a local non-secret configuration file exists and names a
    /// checkout. Deeper validation (pin, dependency identity, bounds) belongs
    /// to the wrapper, which fails closed with a typed failure.
    bool configured() const;

    /// The local configuration as read from disk (ignored state root, or the
    /// `MARKETLAB_IBKR_CONFIG` override).
    IbkrTwsConfig config() const;

    /// True only when the symbol is explicitly listed in the local
    /// configuration's `symbols`. Automatic consumers (the watchlist action and
    /// the Equity Research candle route) use this so the optional provider is
    /// never applied to instruments the Phase 5 qualification did not cover.
    bool routes_symbol(const QString& symbol) const;

    /// Connect, reach readiness, report runtime identity, disconnect.
    void probe(ProbeCallback cb);
    /// Same, against an explicit non-secret endpoint. Used by the qualification
    /// self-test to exercise bounded refusal/timeout paths without touching the
    /// configured TWS session.
    void probe_with(const IbkrTwsConfig& cfg, int readiness_timeout_sec, ProbeCallback cb);

    /// Resolve the symbol's contract and read one bounded quote snapshot. A
    /// live attempt is followed by an explicit delayed attempt only when the
    /// live feed is entitlement-blocked; the classification records the
    /// deciding feed, the entitlement state, and whether a delayed fallback
    /// occurred (the wrapper envelope additionally retains both attempts).
    void fetch_quote(const QString& symbol, QuoteCallback cb);
    void fetch_quote_with(const IbkrTwsConfig& cfg, const QString& symbol, QuoteCallback cb);

    /// Read bounded historical bars for a completed market window.
    void fetch_history(const QString& symbol, const QString& duration, const QString& bar_size, HistoryCallback cb);

  private:
    explicit IbkrTwsService(QObject* parent = nullptr);

    IbkrTwsConfig load_config() const;
    void run(const IbkrTwsConfig& cfg, const QStringList& arguments, int timeout_ms,
             std::function<void(bool, const QJsonObject&, const QString&)> cb);
    static QStringList endpoint_arguments(const IbkrTwsConfig& cfg);
};

} // namespace fincept::services::ibkr
