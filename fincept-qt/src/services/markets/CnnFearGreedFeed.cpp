#include "services/markets/CnnFearGreedFeed.h"

#include "services/economics/EconomicsService.h"

#include <QJsonArray>

namespace fincept::services {

namespace {
constexpr int kCnnRefreshMs = 30 * 60 * 1000;
}

CnnFearGreedFeed& CnnFearGreedFeed::instance() {
    static CnnFearGreedFeed feed;
    return feed;
}

CnnFearGreedFeed::CnnFearGreedFeed() {
    connect(&EconomicsService::instance(), &EconomicsService::result_ready, this, &CnnFearGreedFeed::on_result);
    auto_refresh_.setInterval(kCnnRefreshMs);
    connect(&auto_refresh_, &QTimer::timeout, this, &CnnFearGreedFeed::request);
}

void CnnFearGreedFeed::request() {
    if (!auto_refresh_.isActive())
        auto_refresh_.start();
    if (running())
        return;
    pending_id_ = QStringLiteral("cnn-fear-greed:%1").arg(++sequence_);
    EconomicsService::instance().execute(QStringLiteral("cnn_fear_greed"), QStringLiteral("cnn_fear_greed.py"),
                                         QStringLiteral("index"), {}, pending_id_, /*bypass_cache=*/true);
    emit changed();
}

void CnnFearGreedFeed::request_if_stale() {
    if (running())
        return;
    if (!last_success_.isValid() || last_success_.msecsTo(QDateTime::currentDateTimeUtc()) > kCnnRefreshMs)
        request();
}

QJsonObject CnnFearGreedFeed::component(const QString& key) const {
    for (const auto& v : data_.value(QStringLiteral("components")).toArray())
        if (v.toObject().value(QStringLiteral("key")).toString() == key)
            return v.toObject();
    return {};
}

void CnnFearGreedFeed::on_result(const QString& request_id, const EconomicsResult& result) {
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
