# MCP Tools — Author and Maintainer Guide

MarketLab retains Model Context Protocol (MCP) as generic integration infrastructure. It is used by the workflow editor and application features; no model-provider, chat, or agent runtime is bundled.

## Architecture

`McpService` provides discovery and dispatch across two sources:

- `McpProvider` registers built-in C++ tools from `src/mcp/tools/`.
- `McpManager` manages explicitly configured external MCP servers over JSON-RPC.
- `SchemaValidator` validates every built-in call before its handler runs.
- `McpService::execute_tool` routes a canonical server and tool name.
- `McpService::execute_wire_function` accepts the stable `serverId__toolName` form used by saved workflows.

## Adding a tool

1. Define a `ToolDef` with a stable name, description, category, input schema, authorization level, destructiveness flag, and handler.
2. Prefer `ToolSchemaBuilder` for an object schema with explicit properties and required fields.
3. Return validation failures as `ToolResult::fail`; do not silently coerce invalid financial inputs.
4. Mark all state-mutating tools `is_destructive = true`. The Security settings capability remains off by default.
5. Set `default_timeout_ms` deliberately for asynchronous handlers.
6. Register the factory in `McpInit.cpp` and add new source files to `CMakeLists.txt`.
7. Add a focused unit test for non-trivial parsing, validation, authorization, or transformation logic.
8. Run the headless tool self-test to verify every registered tool has a handler, usable description, and valid schema.

New tool names should use dot-separated `<area>.<verb>` names, for example `markets.get_quote`. Use `legacy_aliases` only when a saved workflow must continue resolving an older name.

## Diagnosis

- Tool not found: confirm its factory returns it, `McpInit.cpp` registers the factory, and CMake compiles its source.
- Authorization failure: inspect `auth_required`, `is_destructive`, and the generic MCP destructive-tool capability.
- Validation failure: run the same JSON object through `SchemaValidator` and compare it with the declared schema.
- Timeout: set a tool-specific budget; do not raise global limits to conceal a blocking handler.
- Discovery: inspect `McpProvider::audit_all_tools()` or use the registered MCP metadata tools.

External MCP tools are treated as destructive by default because the protocol does not carry equivalent safety metadata. Adding an external server does not grant broker authority or external order-execution permission.
