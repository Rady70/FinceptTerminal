#pragma once

// MarketLab Terminal — app-context boundary self-test (no GUI / no network).
//
// Verifies the fork's containment invariants that need the full application
// link surface (FINCEPT_FORK_PLAN.md §5.2–§5.4, §11.1):
//   - capability defaults and screen gating (CapabilityManager)
//   - the shared HTTP client rejects Fincept-owned destinations with a typed
//     HOSTED_SERVICE_UNAVAILABLE error and never touches the network
//   - no live order-capable broker is registered (BrokerRegistry empty)
//   - the MCP registry contains no live-trading / forum / profile tools
//   - the workflow registry contains no trading (order) nodes
//   - fork identity: state root, instance key, application name
//
// Run headless:  QT_QPA_PLATFORM=offscreen MarketLabTerminal --selftest-marketlab-boundary
// Returns 0 when every assertion passes, 1 otherwise.

namespace fincept::marketlab {

int run_marketlab_boundary_selftest();

} // namespace fincept::marketlab
