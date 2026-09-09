// McpService.cpp — Unified tool interface (Qt port)

#include "mcp/McpService.h"

#include "core/config/AppConfig.h"
#include "core/logging/Logger.h"
#include "mcp/McpManager.h"
#include "mcp/McpProvider.h"
#include "mcp/ToolRetriever.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QThread>

#include <algorithm>

namespace fincept::mcp {

static constexpr const char* TAG = "McpService";

McpService& McpService::instance() {
    static McpService s;
    return s;
}

// ============================================================================
// Lifecycle
// ============================================================================

void McpService::initialize() {
    // Load external server configs from DB (fast, synchronous)
    McpManager::instance().initialize();

    // Invalidate tool cache whenever external servers change (start/stop/add/remove)
    QObject::connect(&McpManager::instance(), &McpManager::servers_changed, [this]() {
        QMutexLocker lock(&mutex_);
        cache_time_ = QDateTime(); // force refresh on next get_all_tools()
        // The Tool RAG index covers external tools too (ToolRetriever builds
        // over get_all_tools()), so it must follow this cache. Queued, never
        // inline: servers_changed can fire while McpManager's mutex is held
        // (error paths), and the retriever rebuild takes ToolRetriever →
        // McpService → McpManager locks — invalidating inline here would
        // invert that order.
        QMetaObject::invokeMethod(qApp, []() { ToolRetriever::instance().invalidate(); }, Qt::QueuedConnection);
        LOG_INFO(TAG, "Tool cache invalidated — external servers changed");
    });

    // Auto-start enabled servers in a background thread so the UI never
    // freezes. Only servers with both enabled AND auto_start flags are
    // started at launch. Users can manually enable others from the MCP tab.
    const auto servers = McpManager::instance().get_servers();
    QStringList to_start;
    for (const auto& srv : servers) {
        if (srv.enabled && srv.auto_start)
            to_start.append(srv.id);
    }

    if (!to_start.isEmpty()) {
        QThread* t = QThread::create([to_start]() {
            for (const auto& id : to_start) {
                LOG_INFO("McpService", "Auto-starting MCP server: " + id);
                auto r = McpManager::instance().start_server(id);
                if (r.is_err())
                    LOG_WARN("McpService", "Auto-start failed for " + id + ": " + QString::fromStdString(r.error()));
            }
            LOG_INFO("McpService", "All external MCP servers started");
        });
        t->setObjectName("mcp-autostart");
        t->start();
        QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    }

    McpManager::instance().start_health_check();

    LOG_INFO(TAG, QString("McpService initialized — %1 internal tools, %2 external servers queued")
                      .arg(McpProvider::instance().tool_count())
                      .arg(to_start.size()));
}

void McpService::shutdown() {
    McpManager::instance().shutdown();
    McpProvider::instance().clear();
    LOG_INFO(TAG, "McpService shut down");
}

// ============================================================================
// Tool Discovery
// ============================================================================

std::vector<UnifiedTool> McpService::get_all_tools() {
    QMutexLocker lock(&mutex_);
    return cached_tools_locked();
}

const std::vector<UnifiedTool>& McpService::cached_tools_locked() {
    if (!is_cache_valid())
        refresh_cache();
    return cached_tools_;
}

std::size_t McpService::tool_count() {
    return get_all_tools().size();
}

// ============================================================================
// Tool Execution
// ============================================================================

ToolResult McpService::execute_tool(const QString& server_id, const QString& tool_name, const QJsonObject& args,
                                    bool allow_defer) {
    // Route to internal provider
    if (server_id == INTERNAL_SERVER_ID) {
        if (!allow_defer)
            return McpProvider::instance().call_tool(tool_name, args);
        // How long a supports_async tool may run inline before it backgrounds
        // itself. Clamped: below ~250 ms almost everything would background
        // (costing an extra round-trip for nothing), above ~30 s the deferral
        // stops buying anything over just blocking.
        const int grace_ms =
            std::clamp(AppConfig::instance().get("mcp/job_grace_ms", QVariant(kMcpJobGraceMs)).toInt(), 250, 30000);
        return McpProvider::instance().call_tool_or_defer(tool_name, args, grace_ms);
    }

    // Route to external server — through the same Phase 6.3 auth/destructive
    // gate internal tools get inside call_tool_async (previously this path
    // bypassed it entirely). The MCP wire carries no auth/destructiveness
    // metadata, so external tools are gated destructive-by-default: the
    // installed checker must approve them exactly like a destructive internal
    // tool (agent-originated calls are denied unless the agent opts in).
    // `destructive_declared=false`: the `true` above is OUR conservative
    // assumption, not something the external tool declared. Marking it as
    // undeclared keeps these on the checker-only path instead of the
    // fail-closed capability gate, so a user who configured a Notion/Postgres
    // server in the MCP Servers tab does not have to also flip the destructive
    // switch. Agent-originated calls are still denied by the checker unless the
    // agent opted in with the destructive token.
    if (auto denied = McpProvider::instance().check_authorization(server_id + "__" + tool_name, AuthLevel::None,
                                                                  /*is_destructive=*/true,
                                                                  /*destructive_declared=*/false))
        return *denied;

    auto result = McpManager::instance().call_external_tool(server_id, tool_name, args);
    if (result.is_err())
        return ToolResult::fail(QString::fromStdString(result.error()));

    const QJsonObject& data = result.value();

    bool is_error = data["isError"].toBool(false);
    QJsonArray content = data["content"].toArray();

    QString text;
    for (const auto& item : content) {
        QJsonObject obj = item.toObject();
        if (obj["type"].toString() == "text")
            text += obj["text"].toString();
    }

    if (is_error)
        return ToolResult::fail(text.isEmpty() ? "External tool error" : text);

    // Try to parse text as JSON data
    QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isNull()) {
        if (doc.isObject())
            return ToolResult::ok(text, doc.object());
        if (doc.isArray())
            return ToolResult::ok(text, doc.array());
    }

    return ToolResult::ok(text);
}

ToolResult McpService::execute_wire_function(const QString& function_name, const QJsonObject& args, bool allow_defer) {
    auto [server_id, tool_name] = McpProvider::parse_wire_function_name(function_name);

    if (server_id.isEmpty() || tool_name.isEmpty()) {
        // Parsing only fails to resolve when no registered
        // tool matches, so this is "no such tool", not a syntax problem. Saying
        // "invalid format" sent models off re-spelling a name that simply does
        // not exist — one turn retried a hallucinated create_workbook/add_tab
        // API repeatedly instead of concluding the tools weren't there.
        LOG_WARN(TAG, "No such tool: " + function_name);
        return ToolResult::fail("No tool named '" + function_name +
                                "' exists. Select a registered MCP tool name instead.");
    }

    LOG_INFO(TAG, QString("Dispatch: %1 -> server=%2 tool=%3").arg(function_name, server_id, tool_name));
    auto result = execute_tool(server_id, tool_name, args, allow_defer);
    LOG_INFO(TAG, QString("Dispatch result: %1 success=%2").arg(tool_name, result.success ? "true" : "false"));
    return result;
}

// ============================================================================
// Validation
// ============================================================================
// Phase 3: validation now lives in mcp::validate_args (SchemaValidator.cpp)
// and is invoked automatically by McpProvider::call_tool. The previous
// McpService::validate_params helper was orphaned (never called from any
// execution path) and has been removed. See plans/mcp-refactor-phase-3-schema-validation.md.

// ============================================================================
// Cache
// ============================================================================

bool McpService::is_cache_valid() const {
    if (McpProvider::instance().generation() != cached_generation_)
        return false;
    if (cache_time_.isNull() || cached_tools_.empty())
        return false;
    return cache_time_.msecsTo(QDateTime::currentDateTime()) < CACHE_TTL_MS;
}

void McpService::refresh_cache() {
    cached_tools_.clear();

    // Internal tools
    auto internal = McpProvider::instance().list_tools();
    cached_tools_.insert(cached_tools_.end(), internal.begin(), internal.end());

    // External tools
    auto external = McpManager::instance().get_all_external_tools();
    for (auto& ext : external) {
        // External tools default category="" (their server doesn't tag) and
        // is_destructive=true — the MCP spec carries no destructiveness
        // signal on the wire, so treat external tools as destructive-by-
        // default. Keeps tool_list / Tool RAG surfacing honest and matches
        // execute_tool(), which gates them like destructive internal tools.
        cached_tools_.push_back(
            {ext.server_id, ext.server_name, ext.name, ext.description, ext.input_schema, false, QString{}, true});
    }

    cache_time_ = QDateTime::currentDateTime();
    cached_generation_ = McpProvider::instance().generation();

    LOG_INFO(TAG, QString("Refreshed tool cache: %1 total (%2 internal, %3 external)")
                      .arg(cached_tools_.size())
                      .arg(internal.size())
                      .arg(external.size()));
}

} // namespace fincept::mcp
