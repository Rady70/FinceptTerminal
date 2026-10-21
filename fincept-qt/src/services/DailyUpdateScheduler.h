#pragma once
// DailyUpdateScheduler — runs the daily FedWatch and ETF Flow updates when
// DailyUpdateRule says one is due. While the app is open it checks every 10
// minutes (start()); the Windows scheduled task starts the app windowless
// with --daily-update, which runs whatever is due once and exits
// (run_due_once()). If the app is already open that launch exits at once and
// the open app does the work. Each update is the same as the tool's own
// REFRESH; open views re-read through ResearchDataEvents.
#include <QDate>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

namespace fincept::services {

struct EconomicsResult;

class DailyUpdateScheduler : public QObject {
    Q_OBJECT
  public:
    static DailyUpdateScheduler& instance();

    /// Start checking (first check a few minutes after start-up, then every
    /// 10 minutes). Idempotent.
    void start();

    /// Windowless --daily-update: run every due job once, then call `done`
    /// with 0 (nothing failed) or 1.
    void run_due_once(std::function<void(int)> done);

  private:
    DailyUpdateScheduler() = default;
    void check();
    void run_fedwatch();
    void run_etf();
    void on_economics_result(const QString& request_id, const EconomicsResult& result);
    void finish(const QString& job, bool ok, const QString& detail);

    QString state_path() const;
    QJsonObject read_state() const;
    void write_state(const QJsonObject& state) const;
    void log_line(const QString& line) const;

    QTimer timer_;
    bool started_ = false;
    QString runner_ = QStringLiteral("app"); ///< "app" (open app) or "task" (scheduled, windowless)
    std::function<void(int)> once_done_;
    bool once_failed_ = false;
    QString running_job_;
    QDate session_;
    QString fw_pending_id_;
    QString fw_command_;
    QString fw_detail_;
    bool fw_ok_ = false;
    int sequence_ = 0;
};

} // namespace fincept::services
