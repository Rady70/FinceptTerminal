#pragma once
// McpProvider.h — Internal tool registry and executor (Qt port)
// Singleton managing all built-in terminal tools.
// Tools register handlers invoked by workflows and application features.

#include "mcp/McpTypes.h"

#include <QFuture>
#include <QHash>
#include <QMutex>
#include <QSet>

#include <optional>
#include <vector>

namespace fincept::mcp {

class McpProvider {
  public:
    static McpProvider& instance();

    // ── Tool Registration ──────────────────────────────────────────────────

    void register_tool(ToolDef tool);
    void register_tools(std::vector<ToolDef> tools);
    void unregister_tool(const QString& name);

    // ── Tool Enable/Disable ────────────────────────────────────────────────

    void set_tool_enabled(const QString& name, bool enabled);
    bool is_tool_enabled(const QString& name) const;

    // ── Tool Discovery ─────────────────────────────────────────────────────

    /// List all enabled tools as UnifiedTool.
    std::vector<UnifiedTool> list_tools() const;

    /// List ALL tools including disabled (for management UI)
    std::vector<UnifiedTool> list_all_tools() const;

    std::size_t tool_count() const;
    bool has_tool(const QString& name) const;

    /// O(1) lookup of a single tool's UnifiedTool snapshot by canonical name.
    /// Returns `std::nullopt` if the name is unknown or disabled. Used by
    /// tool_describe so the hot tool-pick path doesn't pay the O(N) cost of
    /// list_tools()+linear-scan (was ~3 ms p95 across the 583-tool catalog).
    std::optional<UnifiedTool> find_tool(const QString& name) const;

    /// Audit-friendly snapshot of one tool. Carries the bits the self-test /
    /// management UI need to verify wiring — crucially handler-presence, which
    /// the UnifiedTool snapshot omits. Does not expose the handler
    /// std::functions themselves (kept inside the registry).
    struct ToolAuditInfo {
        QString name;
        QString category;
        QString description;
        bool has_handler = false; // sync OR async handler is set
        bool is_async = false;    // async_handler is set
        int default_timeout_ms = kMcpDefaultTimeoutMs;
        bool supports_async = false; // the explicit override flag, not the derived verdict
        bool job_eligible = false;   // what supports_async_jobs() would answer
        bool enabled = true;
        bool is_destructive = false;
        AuthLevel auth_required = AuthLevel::None;
        QJsonObject input_schema; // serialised JSON Schema
        QStringList legacy_aliases;
    };

    /// One ToolAuditInfo per registered tool (enabled and disabled). Used by
    /// the headless tool self-test to verify every tool has a handler, a valid
    /// schema, and a usable description without invoking anything.
    std::vector<ToolAuditInfo> audit_all_tools() const;

    // ── Tool Execution ─────────────────────────────────────────────────────

    /// Synchronous execution. For tools registered with `handler` (legacy),
    /// invokes directly and returns. For tools registered with `async_handler`,
    /// dispatches asynchronously and waits for the future to complete (with
    /// the tool's default_timeout_ms applied). Validation runs first via
    /// mcp::validate_args; failures short-circuit before the handler.
    ToolResult call_tool(const QString& name, const QJsonObject& args);

    /// Phase 4: asynchronous execution. Returns a QFuture that resolves with
    /// the ToolResult. For sync handlers, returns an already-resolved future
    /// (the handler runs on the calling thread). For async handlers, builds
    /// a ToolContext (cancellation hook + timeout from the per-call ctx or
    /// the tool's default), invokes the handler, and arms a timeout timer.
    ///
    /// Pass an empty ToolContext to get the tool's defaults. Pass a populated
    /// one to override timeout / inject a cancellation hook (Phase 5 will
    /// thread cancellation tokens through here).
    QFuture<ToolResult> call_tool_async(const QString& name, const QJsonObject& args, ToolContext ctx = {});

    /// Long-running-task entry point for callers that understand job receipts.
    ///
    /// Behaves exactly like `call_tool` for tools that haven't opted into the
    /// job protocol (`ToolDef::supports_async`), so the ~580 existing tools and
    /// every internal C++ / workflow / agent-bridge caller are unaffected.
    ///
    /// For a tool that HAS opted in: the call runs under a `JobRegistry` job. If
    /// it completes within `grace_ms` the real result is returned inline and the
    /// job is retired — the common "it was fast this time" case costs nothing.
    /// Otherwise the work keeps running in the background and this returns a
    /// receipt:
    ///
    ///     { job_id: "job_00001f", status: "running", tool: "run_agent" }
    ///
    /// The caller then polls `job_status(job_id, wait_ms)` and collects
    /// with `job_result(job_id)`. That turns a five-minute blocking tool call
    /// into a ~40-token receipt plus a long-poll, instead of holding the
    /// provider HTTP turn open until it times out.
    ///
    /// Cancellation and progress are wired to the job automatically: handlers
    /// see `ctx.is_cancelled()` flip when `job_cancel` is called, and whatever
    /// they report through `ctx.on_progress` shows up in `job_status`.
    ToolResult call_tool_or_defer(const QString& name, const QJsonObject& args, int grace_ms = kMcpJobGraceMs);

    /// True if `name` is registered, enabled, and eligible for the job protocol.
    ///
    /// Requires an async handler (a sync one can't be backgrounded), plus
    /// either an explicit `supports_async` or a `default_timeout_ms` above
    /// `kMcpAsyncJobThresholdMs` — raising the budget IS the opt-in.
    bool supports_async_jobs(const QString& name) const;

    /// The tool's declared hard budget, or the 30 s default if unknown.
    /// Recorded on the job so `job_status` can report elapsed against it.
    int effective_timeout_ms(const QString& name) const;

    // ── Phase 6.3: Authorization hook ──────────────────────────────────────
    /// Caller-supplied predicate that returns true iff the call should
    /// proceed. The hook receives the tool's required AuthLevel and
    /// is_destructive flag; it is responsible for checking the active
    /// session (AuthManager), prompting the user (modal dialog), and
    /// returning the verdict synchronously. Hook lives in the app layer to
    /// avoid pulling auth/UI headers into McpTypes.h.
    ///
    /// When unset, tools with auth_required > Authenticated or
    /// is_destructive=true fail closed; lesser tools pass through.
    using AuthChecker = std::function<bool(AuthLevel required, bool is_destructive)>;
    void set_auth_checker(AuthChecker checker);

    // ── Destructive-tool capability (fail-closed) ─────────────────────────
    //
    // The gate fails closed: a tool that declares `is_destructive` is
    // refused unless destructive capability has been granted for the current
    // context. Session authorization is explicit through
    // set_destructive_allowed(true), persisted as
    //      `mcp/allow_destructive_tools`. Defaults to false.
    //
    // AuthLevel::ExplicitConfirm tools additionally trip the >= Verified
    // fail-closed branch below and stay refused even with the grant, until the
    // confirmation modal lands.

    /// Grant/revoke destructive-tool capability for this session. Persists to
    /// `mcp/allow_destructive_tools` so the choice survives a restart.
    static void set_destructive_allowed(bool allowed);

    /// True when destructive tools may run in the CURRENT context — i.e. the
    /// per-call capability token is present, or the session grant is on.
    static bool destructive_allowed();

    /// Run the Phase 6.3 authorization gate for one call WITHOUT executing
    /// anything. Returns std::nullopt when the call may proceed, or a
    /// ready-to-return failure ToolResult when blocked. Shared by
    /// call_tool_async (internal tools) and McpService::execute_tool
    /// (external tools, gated destructive-by-default) so both paths apply
    /// identical auth/destructive-confirmation rules. `name` is used for
    /// logging and error messages only.
    ///
    /// `destructive_declared` — true when `is_destructive` comes from the
    /// tool's own ToolDef. Pass false when the caller is merely applying a
    /// conservative default to a tool that carries no destructiveness metadata
    /// (external MCP servers over the wire). Only DECLARED destructive tools
    /// hit the fail-closed capability gate; undeclared ones stay on the
    /// checker-only path so user-configured external servers keep working.
    std::optional<ToolResult> check_authorization(const QString& name, AuthLevel auth_required, bool is_destructive,
                                                  bool destructive_declared = true) const;

    /// Parse "serverId__toolName" → { server_id, tool_name }.
    /// Reverses any wire-encoding applied by `encode_tool_name_for_wire` (e.g.
    /// `tool-dot-list` → `tool.list`) so the returned tool_name matches the
    /// registry key.
    static QPair<QString, QString> parse_wire_function_name(const QString& fn_name);

    /// Encode an internal tool name into the portable MCP wire subset:
    ///
    ///   ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$      total length 1..64
    ///
    /// Encoding rules:
    ///   1. Each '.' in the internal name → "-dot-" (no internal tool name uses
    ///      hyphens, so the round-trip is unambiguous).
    ///   2. If the resulting name fails the common-subset regex (e.g. starts
    ///      with a digit or contains other illegal chars), the encoder prefixes
    ///      it with `t_` and replaces every illegal byte with `_`.
    ///   3. The combined `<server_id>__<encoded_tool>` must fit in 64 chars.
    ///      Server prefix `fincept-terminal__` is 18 chars; longer tool names
    ///      are tail-truncated and a 4-char hash suffix preserves uniqueness.
    ///
    /// `decode_tool_name_from_wire` reverses the dotted-name encoding.
    static QString encode_tool_name_for_wire(const QString& tool_name);
    static QString decode_tool_name_from_wire(const QString& wire_name);

    // ── Lifecycle ──────────────────────────────────────────────────────────

    void clear();

    // ── Generation Counter ─────────────────────────────────────────────────

    /// Monotonically increasing — incremented on any mutation.
    /// McpService uses this to detect stale cache.
    quint64 generation() const;

    McpProvider(const McpProvider&) = delete;
    McpProvider& operator=(const McpProvider&) = delete;

  private:
    McpProvider() = default;

    mutable QMutex mutex_;
    QHash<QString, ToolDef> tools_;
    // Pre-serialised view of each tool, kept in lockstep with `tools_`. Built
    // once at registration so list_tools()/find_tool() never re-serialise the
    // input_schema. `disabled_tools_` is still consulted at read time.
    QHash<QString, UnifiedTool> snapshots_;
    QSet<QString> disabled_tools_;
    quint64 generation_ = 0;

    // Phase 6.3 — guarded by mutex_ so set_auth_checker / call_tool_async
    // see consistent state across threads.
    AuthChecker auth_checker_;
};

} // namespace fincept::mcp
