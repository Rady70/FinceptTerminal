#pragma once

namespace fincept::marketlab {

/// Headless Phase 5 IBKR qualification suite (FINCEPT_FORK_PLAN.md §11.4).
///
/// When no local non-secret configuration exists the suite reports
/// `configured: false` and exits 0 so hosted CI (which never has TWS) can run
/// the standard self-test list unchanged. When a configuration is present the
/// suite exercises the real MarketLab service path against the configured
/// local TWS endpoint: connection/readiness, contract resolution, a bounded
/// quote snapshot, bounded historical bars, and deterministic failure paths.
int run_ibkr_tws_selftest();

} // namespace fincept::marketlab
