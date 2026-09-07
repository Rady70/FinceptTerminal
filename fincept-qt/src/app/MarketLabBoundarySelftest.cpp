#include "app/MarketLabBoundarySelftest.h"

#include "app/InstanceLock.h"
#include "core/capability/CapabilityManager.h"
#include "core/config/AppConfig.h"
#include "core/config/AppPaths.h"
#include "core/logging/Logger.h"
#include "mcp/McpProvider.h"
#include "network/http/HostedPathGuard.h"
#include "network/http/HttpClient.h"
#include "services/workflow/NodeRegistry.h"
#include "trading/BrokerRegistry.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

#include <cstdio>

namespace fincept::marketlab {

namespace {

int failures = 0;

#define CHECK(cond, what)                                                                                       \
    do {                                                                                                        \
        if (!(cond)) {                                                                                          \
            ++failures;                                                                                         \
            std::fprintf(stderr, "[marketlab-boundary] FAIL: %s\n", what);                                      \
            LOG_ERROR("MarketLabBoundary", QStringLiteral("FAIL: %1").arg(QString::fromUtf8(what)));            \
        } else {                                                                                                \
            LOG_INFO("MarketLabBoundary", QStringLiteral("ok: %1").arg(QString::fromUtf8(what)));               \
        }                                                                                                       \
    } while (0)

} // namespace

int run_marketlab_boundary_selftest() {
    LOG_INFO("MarketLabBoundary", "Starting MarketLab boundary self-test");
    using namespace capability;

    // ── Capability defaults (plan §5.2) ───────────────────────────────────
    auto& mgr = CapabilityManager::instance();
    CHECK(mgr.is_available(Capability::LocalWorkspace), "LocalWorkspace available");
    CHECK(mgr.is_available(Capability::PublicData), "PublicData available");
    CHECK(mgr.is_available(Capability::UserConfiguredProvider), "UserConfiguredProvider available");
    CHECK(mgr.is_available(Capability::LocalAnalytics), "LocalAnalytics available");
    CHECK(!mgr.is_available(Capability::FinceptHosted), "FinceptHosted unavailable");
    CHECK(!mgr.is_available(Capability::CloudSync), "CloudSync unavailable");
    CHECK(!mgr.is_available(Capability::BrokerExecution), "BrokerExecution unavailable");
    CHECK(mgr.availability(Capability::BrokerReadOnly).state == AvailabilityState::Conditional,
          "BrokerReadOnly conditional");

    // ── Screen gating (plan §5.2) ─────────────────────────────────────────
    CHECK(mgr.is_screen_allowed(QStringLiteral("watchlist")), "watchlist allowed");
    CHECK(mgr.is_screen_allowed(QStringLiteral("markets")), "markets allowed");
    CHECK(!mgr.is_screen_allowed(QStringLiteral("equity_trading")), "equity_trading denied");
    CHECK(!mgr.is_screen_allowed(QStringLiteral("crypto_trading")), "crypto_trading denied");
    CHECK(!mgr.is_screen_allowed(QStringLiteral("forum")), "forum denied");
    CHECK(!mgr.is_screen_allowed(QStringLiteral("profile")), "profile denied");
    CHECK(!mgr.is_screen_allowed(QStringLiteral("quantlib")), "quantlib denied");

    // ── No live brokers registered (plan §5.4) ────────────────────────────
    {
        const auto brokers = trading::BrokerRegistry::instance().list_brokers();
        CHECK(brokers.isEmpty(), "BrokerRegistry empty (no live order-capable adapters)");
    }

    // ── MCP registration set (plan §5.4) ──────────────────────────────────
    {
        const QStringList forbidden = {
            QStringLiteral("live_place_order"),      QStringLiteral("live_cancel_order"),
            QStringLiteral("live_close_position"),   QStringLiteral("live_close_all_positions"),
            QStringLiteral("live_cancel_all_orders"), QStringLiteral("live_smart_order"),
        };
        for (const QString& name : forbidden) {
            CHECK(!mcp::McpProvider::instance().has_tool(name),
                  ("MCP tool not registered: " + name).toUtf8().constData());
        }
        CHECK(!mcp::McpProvider::instance().has_tool(QStringLiteral("forum_get_posts")),
              "MCP forum tool not registered");
    }

    // ── Workflow node registry (plan §5.4) ────────────────────────────────
    {
        bool found_trading_node = false;
        for (const auto& def : workflow::NodeRegistry::instance().all()) {
            if (def.type_id.startsWith(QStringLiteral("trading."))) {
                found_trading_node = true;
                break;
            }
        }
        CHECK(!found_trading_node, "no trading.* workflow nodes registered");
    }

    // ── HttpClient hosted rejection (plan §5.3) ───────────────────────────
    {
        bool called = false;
        QString err_text;
        QEventLoop loop;
        QTimer::singleShot(5000, &loop, &QEventLoop::quit); // safety: never wait forever
        HttpClient::instance().get(QStringLiteral("https://api.fincept.in/user/profile"),
                                   [&](Result<QJsonDocument> r) {
                                       called = true;
                                       if (r.is_err())
                                           err_text = QString::fromStdString(r.error());
                                       loop.quit();
                                   },
                                   qApp);
        loop.exec();
        CHECK(called, "HttpClient callback delivered for rejected host");
        CHECK(network::HostedPathGuard::is_hosted_unavailable_error(err_text),
              "typed HOSTED_SERVICE_UNAVAILABLE error delivered");
    }
    {
        // Relative path against the default Fincept base URL must be rejected
        // the same way (configuration-derived hosted route). The base URL is
        // restored afterwards so this selftest leaves no process-global
        // mutation behind.
        const QString default_base = [] {
            HttpClient::instance().set_base_url(QStringLiteral("https://api.fincept.in"));
            return QString();
        }();
        Q_UNUSED(default_base);
        bool called = false;
        QString err_text;
        QEventLoop loop;
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        HttpClient::instance().get(QStringLiteral("/user/subscriptions"), [&](Result<QJsonDocument> r) {
            called = true;
            if (r.is_err())
                err_text = QString::fromStdString(r.error());
            loop.quit();
        }, qApp);
        loop.exec();
        CHECK(called, "relative Fincept-API path rejected");
        CHECK(network::HostedPathGuard::is_hosted_unavailable_error(err_text), "relative path typed error");
        HttpClient::instance().set_base_url(AppConfig::instance().api_base_url());
    }

    // ── Fork identity ─────────────────────────────────────────────────────
    CHECK(QCoreApplication::applicationName() == QStringLiteral("MarketLabTerminal"),
          "application name is MarketLabTerminal");
    CHECK(QCoreApplication::applicationVersion() == QStringLiteral("0.1.0"), "fork version 0.1.0");
    CHECK(AppPaths::root().contains(QStringLiteral("com.marketlab.terminal")),
          "state root is com.marketlab.terminal");
    CHECK(!AppPaths::root().contains(QStringLiteral("com.fincept.terminal")),
          "state root is not the Fincept root");

    if (failures == 0) {
        LOG_INFO("MarketLabBoundary", "MarketLab boundary self-test PASSED");
        std::fprintf(stderr, "[marketlab-boundary] PASSED\n");
        return 0;
    }
    LOG_ERROR("MarketLabBoundary", QString("MarketLab boundary self-test FAILED (%1 failures)").arg(failures));
    std::fprintf(stderr, "[marketlab-boundary] FAILED (%d failures)\n", failures);
    return 1;
}

} // namespace fincept::marketlab
