// src/services/news/NewsService_LiveFeed.cpp
//
// WebSocket live-feed lifecycle: connect_live_feed / disconnect_live_feed /
// is_live_connected (both the HAS_QT_WEBSOCKETS branch and the stub fallback).
//
// Part of the partial-class split of NewsService.cpp.

#include "core/config/AppConfig.h"
#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "network/http/HttpClient.h"
#include "services/news/NewsService.h"
#include "storage/cache/CacheManager.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QXmlStreamReader>

#ifdef HAS_QT_WEBSOCKETS
#    include <QtWebSockets/QWebSocket>
#endif

#include <algorithm>
#include <memory>

namespace fincept::services {

// 10s before WebSocket reconnect.
static constexpr int kWsReconnectDelayMs = 10000;

#ifdef HAS_QT_WEBSOCKETS
void NewsService::connect_live_feed(const QString& ws_url) {
    Q_UNUSED(ws_url);
    // MarketLab: the hosted live-news WebSocket (wss://api.fincept.in/ws/news)
    // is removed (FINCEPT_FORK_PLAN.md §5.3, §6). RSS-based fetching, caching,
    // clustering, and local NLP remain; the live-feed toggle is inert and the
    // socket is never created.
    LOG_WARN("NewsService", "Hosted live feed is unavailable in MarketLab Terminal — using RSS feeds");
    emit live_state_changed(false);
}

void NewsService::disconnect_live_feed() {
    if (!live_ws_)
        return;
    live_ws_->close();
    live_ws_->deleteLater();
    live_ws_ = nullptr;
    live_connected_ = false;
    emit live_state_changed(false);
}

bool NewsService::is_live_connected() const {
    return live_connected_;
}
#else
// No WebSocket support — stubs
void NewsService::connect_live_feed(const QString&) {}
void NewsService::disconnect_live_feed() {}
bool NewsService::is_live_connected() const {
    return false;
}
#endif

// ── Auto-refresh ────────────────────────────────────────────────────────────

} // namespace fincept::services
