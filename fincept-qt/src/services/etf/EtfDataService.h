// src/services/etf/EtfDataService.h
//
// The application's entry point to the ETF Capital Flows data foundation
// (Batch B). It wires the two enabled routes to their production transports
// and nothing else:
//
//   * SEC EDGAR N-PORT through the shared, guarded HttpClient, with the
//     declared User-Agent read from ignored local configuration;
//   * IBKR completed-session daily bars through the existing read-only
//     IbkrTwsService / ibkr_tws_data.py route.
//
// Lifecycle: nothing starts at application startup. There is no timer, no
// polling and no background schedule; a run happens only when a caller asks
// for one, and TWS is contacted only by an explicit IBKR run. A run's results
// are in the normal MarketLab database (migration v052).
//
// SEC configuration (never committed): `sec_edgar.json` in the MarketLab
// state root (AppPaths::root()), or the file named by MARKETLAB_SEC_CONFIG:
//     { "user_agent": "<name or organisation> <contact e-mail>",
//       "min_request_interval_ms": 500 }
// MARKETLAB_SEC_USER_AGENT, when set, overrides user_agent.
#pragma once
#include "services/etf/EtfIbkrDailyIngestor.h"
#include "services/etf/EtfSecNportIngestor.h"

#include <QObject>
#include <QString>

#include <functional>

namespace fincept::services::etf {

inline constexpr int kSecMinRequestIntervalFloorMs = 150;
inline constexpr int kSecDefaultRequestIntervalMs = 500;

class EtfDataService : public QObject {
    Q_OBJECT
  public:
    static EtfDataService& instance();

    struct SecConfig {
        QString config_path;   ///< the file read (it may not exist)
        QString user_agent;    ///< as configured; never logged or printed
        bool declared = false; ///< a name plus a contact e-mail (sec_user_agent_valid)
        int min_request_interval_ms = kSecDefaultRequestIntervalMs; ///< clamped to >= the floor
    };
    SecConfig sec_config() const;

    /// True when the read-only IBKR path has a local configuration.
    bool ibkr_configured() const;

    void ingest_sec_nport(const SecNportRequest& request, std::function<void(const SecNportRunSummary&)> done);
    void ingest_ibkr_daily(const QString& symbol, const QString& duration,
                           std::function<void(const IbkrDailyRunSummary&)> done);

  private:
    explicit EtfDataService(QObject* parent = nullptr);
};

} // namespace fincept::services::etf
