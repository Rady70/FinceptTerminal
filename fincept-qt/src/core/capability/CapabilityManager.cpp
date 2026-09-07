#include "core/capability/CapabilityManager.h"

#include <QHash>

namespace fincept::capability {

namespace {

// Plan §6 feature disposition, expressed per dock-screen id. Screens not
// listed here default to Unavailable ("not registered in this build").
ComponentAvailability screen_entry(const QString& id) {
    using S = AvailabilityState;

    struct Entry {
        const char* id;
        S state;
        const char* reason;
        const char* replacement;
    };

    static const Entry kEntries[] = {
        // Retained local shell / persistence.
        {"dashboard", S::Available, "local workspace dashboard", ""},
        {"watchlist", S::Available, "local watchlist with public-data quotes", ""},
        {"markets", S::Available, "public quote/history; hosted symbol search replaced by local search", ""},
        {"notes", S::Available, "local notes", ""},
        {"report_builder", S::Available, "local report builder", ""},
        {"code_editor", S::Available, "local code editor and notebooks", ""},
        {"file_manager", S::Available, "local file manager", ""},
        {"excel", S::Available, "local spreadsheet", ""},
        {"node_editor", S::Available, "local workflow editor (execution nodes removed)", ""},
        {"settings", S::Available, "local settings", ""},
        {"about", S::Available, "fork identity and capabilities", ""},
        {"docs", S::Available, "bundled documentation", ""},
        {"help", S::Available, "bundled help", ""},
        {"contact", S::Available, "local contact page", ""},
        {"terms", S::Available, "bundled terms text", ""},
        {"privacy", S::Available, "bundled privacy text", ""},
        {"trademarks", S::Available, "bundled trademark text", ""},

        // Retained public-data and local-analytics surfaces.
        {"equity_research", S::Available, "public financial/technical/news paths with source status", ""},
        {"screener", S::Available, "public-data screener", ""},
        {"news", S::Available, "RSS feeds, caching, clustering; hosted live feed and hosted analysis removed", ""},
        {"portfolio", S::Available, "manually maintained local portfolios and analytics", ""},
        {"backtesting", S::Available, "local historical simulation only", ""},
        {"economics", S::Available, "public economics data (no Fincept macro panel)", ""},
        {"dbnomics", S::Available, "public DBnomics API", ""},
        {"gov_data", S::Available, "public government data connectors", ""},
        {"akshare", S::Available, "public AkShare connectors", ""},
        {"asia_markets", S::Available, "public Asia market data", ""},
        {"geopolitics", S::Available, "HDX/ReliefWeb/trade/local geolocation; Fincept events feed removed", ""},
        {"derivatives", S::Available, "local derivatives calculator", ""},
        {"ma_analytics", S::Available, "local moving-average analytics", ""},
        {"relationship_map", S::Available, "local relationship map", ""},
        {"alt_investments", S::Available, "public alternative-investment data", ""},
        {"trade_viz", S::Available, "local trade visualization", ""},
        {"data_sources", S::Available, "local data-source registry", ""},
        {"data_mapping", S::Available, "user-configured data destinations", ""},
        {"mcp_servers", S::Available, "local MCP server management", ""},

        // Conditional — usable with a user-configured local provider.
        {"ai_chat", S::Conditional, "needs a user-configured local or API LLM provider (e.g. Ollama)",
         "Settings \u2192 LLM Config"},
        {"ai_quant_lab", S::Conditional, "needs user-configured local models and data", "Settings \u2192 LLM Config"},
        {"agent_config", S::Conditional, "needs user-configured local models; execution tools removed",
         "Settings \u2192 LLM Config"},
        {"surface_analytics", S::Conditional, "CSV and labelled demo modes; external providers optional", ""},

        // Unavailable — removed or disabled per plan §6.
        {"equity_trading", S::Unavailable, "external broker order entry is not exposed in this build",
         "Equity Research for public data; Backtesting for simulation"},
        {"algo_trading", S::Unavailable, "live/paper algo deployment is not exposed in this build",
         "Backtesting for simulation"},
        {"crypto_trading", S::Unavailable, "exchange order entry, credentials, and signing are not exposed",
         "Markets screen for public quotes"},
        {"crypto_center", S::Unavailable, "wallet, staking, and signing surfaces are removed", ""},
        {"polymarket", S::Unavailable, "prediction-market order actions and private keys are not exposed", ""},
        {"alpha_arena", S::Unavailable, "hosted arena and agent-wallet execution are not exposed", ""},
        {"fno", S::Unavailable, "requires a qualified independent data source and identity mapping",
         "Backtesting for simulation"},
        {"quantlib", S::Unavailable, "hosted QuantLib suite is removed", "Derivatives local calculator"},
        {"maritime", S::Unavailable, "no independent maritime connector selected yet", ""},
        {"forum", S::Unavailable, "Fincept-hosted forum is removed", ""},
        {"support", S::Unavailable, "Fincept-hosted support tickets are removed",
         "Help screen for bundled documentation"},
        {"profile", S::Unavailable, "Fincept account, billing, and subscription profile is removed",
         "Settings for local configuration"},
    };

    for (const auto& e : kEntries) {
        if (id == QLatin1String(e.id)) {
            ComponentAvailability a;
            a.state = e.state;
            a.reason = QString::fromUtf8(e.reason);
            a.replacement = QString::fromUtf8(e.replacement);
            return a;
        }
    }
    ComponentAvailability unknown;
    unknown.state = S::Unavailable;
    unknown.reason = QStringLiteral("screen id is not registered in this build");
    return unknown;
}

} // namespace

CapabilityManager& CapabilityManager::instance() {
    static CapabilityManager s;
    return s;
}

ComponentAvailability CapabilityManager::availability(Capability cap) const {
    using S = AvailabilityState;
    switch (cap) {
        case Capability::LocalWorkspace:
            return {S::Available, QStringLiteral("local workspace access without any account"), {}};
        case Capability::PublicData:
            return {S::Available, QStringLiteral("public market data from independently configured providers"), {}};
        case Capability::UserConfiguredProvider:
            return {S::Available, QStringLiteral("user-configured data and LLM providers"), {}};
        case Capability::LocalAnalytics:
            return {S::Available, QStringLiteral("local analytics and historical simulation"), {}};
        case Capability::FinceptHosted:
            return {S::Unavailable,
                    QStringLiteral("Fincept-hosted services are removed from this fork; all inventoried Fincept-owned "
                                   "destinations are rejected at the network boundary"),
                    QStringLiteral("local features and public providers")};
        case Capability::CloudSync:
            return {S::Unavailable,
                    QStringLiteral("Fincept cloud sync is removed"),
                    QStringLiteral("local state plus an ordinary user-managed backup")};
        case Capability::BrokerReadOnly:
            return {S::Conditional,
                    QStringLiteral("read-only IBKR consumer is deferred to a later phase and not yet integrated"),
                    {}};
        case Capability::BrokerExecution:
            return {S::Unavailable,
                    QStringLiteral("no external broker or exchange order route is exposed in this fork"),
                    QStringLiteral("historical simulation and paper backtests")};
    }
    return {S::Unavailable, QStringLiteral("unknown capability"), {}};
}

bool CapabilityManager::is_available(Capability cap) const {
    return availability(cap).state != AvailabilityState::Unavailable;
}

ComponentAvailability CapabilityManager::screen_availability(const QString& screen_id) const {
    return screen_entry(screen_id);
}

bool CapabilityManager::is_screen_allowed(const QString& screen_id) const {
    // Registered Conditional screens (ai_chat, ai_quant_lab, agent_config,
    // surface_analytics) stay reachable — they render their own conditional
    // state (e.g. "configure a local LLM provider"). Only Unavailable screens
    // are denied (FINCEPT_FORK_PLAN.md §5.2).
    return screen_entry(screen_id).state != AvailabilityState::Unavailable;
}

QStringList CapabilityManager::allowed_screens(const QStringList& ids) const {
    QStringList out;
    out.reserve(ids.size());
    for (const QString& id : ids) {
        if (is_screen_allowed(id))
            out << id;
    }
    return out;
}

} // namespace fincept::capability
