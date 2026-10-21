#pragma once
// CnnFearGreedFeed — CNN's Fear & Greed data (scripts/cnn_fear_greed.py),
// read once and shared by the CNN index widget and the seven CNN indicator
// widgets of the dashboard.
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

namespace fincept::services {

struct EconomicsResult;

class CnnFearGreedFeed : public QObject {
    Q_OBJECT
  public:
    static CnnFearGreedFeed& instance();

    /// Read CNN unless a read is running. The first call also starts the
    /// automatic re-read (every 30 minutes).
    void request();
    /// request() when there is no data yet or it is older than 30 minutes.
    void request_if_stale();

    bool running() const { return !pending_id_.isEmpty(); }
    /// The last successful read's `data` object (empty before the first).
    const QJsonObject& data() const { return data_; }
    /// The last read's error (empty after a success).
    const QString& error() const { return error_; }

    /// One indicator's object from data().components, by CNN key.
    QJsonObject component(const QString& key) const;

  signals:
    void changed();

  private:
    CnnFearGreedFeed();
    void on_result(const QString& request_id, const EconomicsResult& result);

    QString pending_id_;
    int sequence_ = 0;
    QJsonObject data_;
    QString error_;
    QDateTime last_success_;
    QTimer auto_refresh_;
};

} // namespace fincept::services
