#include "services/markets/MarketBreadthFeed.h"

#include "core/config/ProfileManager.h"
#include "screens/dashboard/widgets/MarketGauges.h"
#include "services/economics/EconomicsService.h"

namespace fincept::services {

namespace {
constexpr int kBreadthRefreshMs = 15 * 60 * 1000;
}

MarketBreadthFeed& MarketBreadthFeed::instance() {
    static MarketBreadthFeed feed;
    return feed;
}

MarketBreadthFeed::MarketBreadthFeed() {
    connect(&EconomicsService::instance(), &EconomicsService::result_ready, this, &MarketBreadthFeed::on_result);
    auto_refresh_.setInterval(kBreadthRefreshMs);
    // Breadth only moves while US stocks trade; outside the session the
    // stored result stays until a manual refresh.
    connect(&auto_refresh_, &QTimer::timeout, this, [this]() {
        if (screens::gauges::exchange_status(QStringLiteral("US"), QDateTime::currentDateTimeUtc()) ==
            screens::gauges::SessionState::Open)
            request();
    });
}

void MarketBreadthFeed::request() {
    if (!auto_refresh_.isActive())
        auto_refresh_.start();
    if (running())
        return;
    pending_id_ = QStringLiteral("market-breadth:%1").arg(++sequence_);
    const QString cache_dir = ProfileManager::instance().profile_root() + QStringLiteral("/market_breadth");
    EconomicsService::instance().execute(QStringLiteral("market_breadth"), QStringLiteral("market_breadth.py"),
                                         QStringLiteral("sentiment"), {QStringLiteral("--cache-dir"), cache_dir},
                                         pending_id_, /*bypass_cache=*/true);
    emit changed();
}

void MarketBreadthFeed::on_result(const QString& request_id, const EconomicsResult& result) {
    if (request_id != pending_id_)
        return;
    pending_id_.clear();
    const QJsonObject data = result.data.value(QStringLiteral("data")).toObject();
    if (result.success && !data.isEmpty()) {
        data_ = data;
        error_.clear();
        last_success_ = QDateTime::currentDateTimeUtc();
    } else {
        error_ = result.error.isEmpty() ? QStringLiteral("no usable response") : result.error;
    }
    emit changed();
}

} // namespace fincept::services
