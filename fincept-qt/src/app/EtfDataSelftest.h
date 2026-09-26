#pragma once

namespace fincept::marketlab {

/// Headless ETF Capital Flows data-foundation self-test (Batch B).
///
/// Runs inside the shipped binary against the active profile's real database,
/// after the real migration sequence, and proves that migration v052 is applied
/// and that the repository keeps its contract there: exact replay is a no-op, a
/// later retrieval confirms, an amendment and a revision are new vintages with
/// the earlier ones kept, missing stays NULL, timing classes are stored, a
/// disabled route is refused, and the database's own CHECK constraints refuse a
/// backfilled "observed" value and a zero-filled missing value.
///
/// Everything runs in one transaction that is rolled back, so the self-test
/// leaves no row behind; when that transaction cannot be opened it writes
/// nothing and fails. It needs no network, no SEC access and no TWS, so CI
/// can run it with the rest of the --selftest-list suites.
int run_etf_data_selftest();

} // namespace fincept::marketlab
