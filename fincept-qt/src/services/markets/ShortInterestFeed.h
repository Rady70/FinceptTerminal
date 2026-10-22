#pragma once
// ShortInterestFeed — the S&P 500's aggregate short interest
// (scripts/finra_short_interest.py: FINRA consolidated short interest with
// the owner's FINRA API key, twice a month). The Market Sentiment widget shows
// it under the daily risk-appetite reading.
//
// The key file `finra_api.json` ({"client_id", "client_secret"}) lives in the
// MarketLab data folder next to sec_edgar.json and is never committed; without
// it the script reports status "needs_key" and reads nothing.
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

namespace fincept::services {

struct EconomicsResult;

class ShortInterestFeed : public QObject {
    Q_OBJECT
  public:
    static ShortInterestFeed& instance();

    /// Read unless a read is running. The first call also starts the
    /// automatic re-read (every 6 hours; the script asks FINRA at most every
    /// 12 hours, so most re-reads only use the local cache).
    void request();
    /// request() when there is no data yet or it is older than 6 hours.
    void request_if_stale();

    bool running() const { return !pending_id_.isEmpty(); }
    /// The last successful result's `data` object (empty before the first).
    const QJsonObject& data() const { return data_; }
    /// The last read's error (empty after a success).
    const QString& error() const { return error_; }
    /// Where the FINRA key file is expected.
    static QString key_file();

  signals:
    void changed();

  private:
    ShortInterestFeed();
    void on_result(const QString& request_id, const EconomicsResult& result);

    QString pending_id_;
    int sequence_ = 0;
    QJsonObject data_;
    QString error_;
    QDateTime last_success_;
    QTimer auto_refresh_;
};

} // namespace fincept::services
