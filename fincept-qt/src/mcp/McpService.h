#pragma once
// McpService.h — Unified tool interface merging internal + external MCP tools (Qt port)
// Single entry point for workflows and application features to discover and call tools.

#include "core/result/Result.h"
#include "mcp/McpTypes.h"

#include <QDateTime>
#include <QMutex>

#include <vector>

namespace fincept::mcp {

class McpService {
  public:
    static McpService& instance();

    // ── Unified Tool Discovery ──────────────────────────────────────────

    /// Get all available tools (internal + external, cached 5 s)
    std::vector<UnifiedTool> get_all_tools();

    std::size_t tool_count();

    // ── Unified Tool Execution ──────────────────────────────────────────

    /// Route to internal or external server.
    ///
    /// `allow_defer` opts the call into the long-running-job protocol for
    /// callers that understand job receipts. External servers never defer:
    /// the MCP wire carries no async metadata.
    ToolResult execute_tool(const QString& server_id, const QString& tool_name, const QJsonObject& args,
                            bool allow_defer = false);

    /// Execute from the stable MCP wire name ("serverId__toolName").
    ToolResult execute_wire_function(const QString& function_name, const QJsonObject& args, bool allow_defer = false);

    // ── Validation ──────────────────────────────────────────────────────
    // Phase 3: removed. McpProvider::call_tool now invokes
    // mcp::validate_args automatically; SchemaValidator.h is the single
    // entry point for input checks.

    // ── Lifecycle ───────────────────────────────────────────────────────

    void initialize();
    void shutdown();

    McpService(const McpService&) = delete;
    McpService& operator=(const McpService&) = delete;

  private:
    McpService() = default;

    mutable QMutex mutex_;

    std::vector<UnifiedTool> cached_tools_;
    QDateTime cache_time_;
    quint64 cached_generation_ = 0;
    static constexpr int CACHE_TTL_MS = 5000;

    void refresh_cache();        // requires mutex_ held
    bool is_cache_valid() const; // requires mutex_ held

    // Snapshot the unfiltered cached tool list (refreshing if stale). Caller
    // must hold mutex_. Splitting this out lets filtered and unfiltered callers
    // reuse the cache without re-locking.
    const std::vector<UnifiedTool>& cached_tools_locked();
};

} // namespace fincept::mcp
