#include "services/DailyUpdateScheduler.h"

#include "core/config/ProfileManager.h"
#include "core/logging/Logger.h"
#include "services/DailyUpdateRule.h"
#include "services/ResearchDataEvents.h"
#include "services/economics/EconomicsService.h"
#include "services/etf/research/EtfResearchService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace fincept::services {

namespace {
constexpr int kDailyCheckMs = 10 * 60 * 1000;
constexpr int kDailyFirstCheckMs = 3 * 60 * 1000;
const char* const kJobs[] = {"fedwatch", "etf"};
} // namespace

DailyUpdateScheduler& DailyUpdateScheduler::instance() {
    static DailyUpdateScheduler s;
    return s;
}

void DailyUpdateScheduler::start() {
    if (started_)
        return;
    started_ = true;
    connect(&EconomicsService::instance(), &EconomicsService::result_ready, this,
            &DailyUpdateScheduler::on_economics_result, Qt::UniqueConnection);
    timer_.setInterval(kDailyCheckMs);
    connect(&timer_, &QTimer::timeout, this, &DailyUpdateScheduler::check);
    timer_.start();
    QTimer::singleShot(kDailyFirstCheckMs, this, &DailyUpdateScheduler::check);
}

void DailyUpdateScheduler::run_due_once(std::function<void(int)> done) {
    runner_ = QStringLiteral("task");
    once_done_ = std::move(done);
    once_failed_ = false;
    connect(&EconomicsService::instance(), &EconomicsService::result_ready, this,
            &DailyUpdateScheduler::on_economics_result, Qt::UniqueConnection);
    check();
}

QString DailyUpdateScheduler::state_path() const {
    return ProfileManager::instance().profile_root() + QStringLiteral("/daily_update/state.json");
}

QJsonObject DailyUpdateScheduler::read_state() const {
    QFile f(state_path());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void DailyUpdateScheduler::write_state(const QJsonObject& state) const {
    QDir().mkpath(QFileInfo(state_path()).absolutePath());
    QSaveFile f(state_path());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
    f.commit();
}

void DailyUpdateScheduler::log_line(const QString& line) const {
    LOG_INFO("DailyUpdate", line);
    QDir().mkpath(QFileInfo(state_path()).absolutePath());
    QFile f(QFileInfo(state_path()).absolutePath() + QStringLiteral("/log.txt"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + QStringLiteral(" [") + runner_ +
                 QStringLiteral("] ") + line + "\n")
                    .toUtf8());
}

void DailyUpdateScheduler::check() {
    if (!running_job_.isEmpty())
        return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    session_ = daily_update::session_for(now);
    const QJsonObject state = read_state();
    for (const char* job : kJobs) {
        const QString name = QString::fromLatin1(job);
        if (!daily_update::job_due(state.value(name).toObject(), session_, now))
            continue;
        if (name == QLatin1String("fedwatch"))
            run_fedwatch();
        else
            run_etf();
        if (!running_job_.isEmpty())
            return;
    }
    // Nothing (more) due: a windowless run ends here.
    if (once_done_) {
        auto done = std::move(once_done_);
        once_done_ = nullptr;
        done(once_failed_ ? 1 : 0);
    }
}

void DailyUpdateScheduler::run_fedwatch() {
    running_job_ = QStringLiteral("fedwatch");
    fw_ok_ = false;
    fw_detail_.clear();
    log_line(QStringLiteral("fedwatch: updating for session %1").arg(session_.toString(Qt::ISODate)));
    fw_command_ = QStringLiteral("collect");
    fw_pending_id_ = QStringLiteral("daily-update-fedwatch:%1").arg(++sequence_);
    EconomicsService::instance().execute(QStringLiteral("fedwatch"), QStringLiteral("fedwatch_data.py"), fw_command_,
                                         {QStringLiteral("--db"), ProfileManager::instance().profile_root() +
                                                                      QStringLiteral("/fedwatch/fedwatch_history.db")},
                                         fw_pending_id_, /*bypass_cache=*/true);
}

void DailyUpdateScheduler::on_economics_result(const QString& request_id, const EconomicsResult& result) {
    if (request_id != fw_pending_id_)
        return;
    fw_pending_id_.clear();
    const bool usable = result.success || (result.data.value(QStringLiteral("success")).toBool() &&
                                           result.data.value(QStringLiteral("data")).isObject());
    if (fw_command_ == QLatin1String("collect")) {
        fw_ok_ = usable;
        fw_detail_ = usable ? QStringLiteral("collected") : QStringLiteral("collect failed: ") + result.error;
        // The FedWatch screen's REFRESH also updates the Polymarket history.
        fw_command_ = QStringLiteral("history_backfill");
        fw_pending_id_ = QStringLiteral("daily-update-fedwatch:%1").arg(++sequence_);
        EconomicsService::instance().execute(
            QStringLiteral("fedwatch"), QStringLiteral("fedwatch_data.py"), fw_command_,
            {QStringLiteral("--db"),
             ProfileManager::instance().profile_root() + QStringLiteral("/fedwatch/fedwatch_history.db")},
            fw_pending_id_, /*bypass_cache=*/true);
        return;
    }
    if (!usable)
        fw_detail_ += QStringLiteral("; Polymarket history failed: ") + result.error;
    emit ResearchDataEvents::instance().fedwatch_saved(nullptr);
    finish(QStringLiteral("fedwatch"), fw_ok_, fw_detail_);
}

void DailyUpdateScheduler::run_etf() {
    auto& service = etf::research::EtfResearchService::instance();
    if (service.refresh_running())
        return; // a refresh someone started is running; check again later
    running_job_ = QStringLiteral("etf");
    log_line(QStringLiteral("etf: updating for session %1").arg(session_.toString(Qt::ISODate)));
    service.refresh(
        QStringLiteral("scheduled"), [](const QString&, int, int) {},
        [this](const etf::research::RefreshResult& r) {
            // A source that failed completely (e.g. FRED down for maintenance)
            // is tried again later in the session; a partial one (a few
            // symbols missing) is normal and counts as done.
            QStringList parts;
            bool source_failed = false;
            for (const auto& s : r.stages) {
                parts << QStringLiteral("%1 %2").arg(s.stage, s.status);
                source_failed = source_failed || s.status == QLatin1String("FAILED");
            }
            emit ResearchDataEvents::instance().etf_saved(nullptr);
            finish(QStringLiteral("etf"), r.ok && !source_failed, r.ok ? parts.join(QStringLiteral(", ")) : r.error);
        });
}

void DailyUpdateScheduler::finish(const QString& job, bool ok, const QString& detail) {
    QJsonObject state = read_state();
    state.insert(job, daily_update::after_attempt(state.value(job).toObject(), session_,
                                                  QDateTime::currentDateTimeUtc(), ok, runner_, detail));
    write_state(state);
    log_line(QStringLiteral("%1: %2 — %3").arg(job, ok ? QStringLiteral("done") : QStringLiteral("failed"), detail));
    running_job_.clear();
    once_failed_ = once_failed_ || !ok;
    QTimer::singleShot(1000, this, &DailyUpdateScheduler::check); // the next due job, if any
}

} // namespace fincept::services
