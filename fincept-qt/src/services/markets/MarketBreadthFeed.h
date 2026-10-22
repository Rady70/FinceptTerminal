#pragma once
// MarketBreadthFeed — one daily read of every S&P 500 member
// (scripts/market_breadth.py). The Market Sentiment widget shows its risk
// appetite; the Market Pulse panel shows the day's up / down count from the
// same read, so the ~30 s collection runs once however many views want it.
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

namespace fincept::services {

struct EconomicsResult;

class MarketBreadthFeed : public QObject {
    Q_OBJECT
  public:
    static MarketBreadthFeed& instance();

    /// Start a collection unless one is running. The first call also starts
    /// the automatic refresh (every 15 minutes while the US market is open).
    void request();

    bool running() const { return !pending_id_.isEmpty(); }
    /// The last successful result's `data` object (empty before the first).
    const QJsonObject& data() const { return data_; }
    /// The last collection's error (empty after a success).
    const QString& error() const { return error_; }
    QDateTime last_success() const { return last_success_; }

  signals:
    /// A collection started or finished (successfully or not).
    void changed();

  private:
    MarketBreadthFeed();
    void on_result(const QString& request_id, const EconomicsResult& result);

    QString pending_id_;
    int sequence_ = 0;
    QJsonObject data_;
    QString error_;
    QDateTime last_success_;
    QTimer auto_refresh_;
};

} // namespace fincept::services
