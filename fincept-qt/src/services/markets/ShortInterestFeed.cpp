#include "services/markets/ShortInterestFeed.h"

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/economics/EconomicsService.h"

#include <QDir>

namespace fincept::services {

namespace {
constexpr int kShortInterestRefreshMs = 6 * 60 * 60 * 1000;
}

ShortInterestFeed& ShortInterestFeed::instance() {
    static ShortInterestFeed feed;
    return feed;
}

ShortInterestFeed::ShortInterestFeed() {
    connect(&EconomicsService::instance(), &EconomicsService::result_ready, this, &ShortInterestFeed::on_result);
    auto_refresh_.setInterval(kShortInterestRefreshMs);
    connect(&auto_refresh_, &QTimer::timeout, this, &ShortInterestFeed::request);
}

QString ShortInterestFeed::key_file() {
    return QDir(AppPaths::root()).filePath(QStringLiteral("finra_api.json"));
}

void ShortInterestFeed::request() {
    if (!auto_refresh_.isActive())
        auto_refresh_.start();
    if (running())
        return;
    pending_id_ = QStringLiteral("short-interest:%1").arg(++sequence_);
    // The script reads the S&P 500 member list from MarketBreadthFeed's cache
    // folder, the sibling `market_breadth`.
    const QString cache_dir = ProfileManager::instance().profile_root() + QStringLiteral("/short_interest");
    EconomicsService::instance().execute(
        QStringLiteral("finra_short_interest"), QStringLiteral("finra_short_interest.py"), QStringLiteral("summary"),
        {QStringLiteral("--cache-dir"), cache_dir, QStringLiteral("--key-file"), key_file()}, pending_id_,
        /*bypass_cache=*/true);
    emit changed();
}

void ShortInterestFeed::request_if_stale() {
    if (running())
        return;
    if (!last_success_.isValid() || last_success_.msecsTo(QDateTime::currentDateTimeUtc()) > kShortInterestRefreshMs)
        request();
}

void ShortInterestFeed::on_result(const QString& request_id, const EconomicsResult& result) {
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
