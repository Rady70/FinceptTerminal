#pragma once

#include <QString>

namespace fincept::marketlab {

/// True when argv carries `--etf-research`.
bool etf_research_cli_requested(int argc, char* argv[]);

/// Headless ETF Flow & Sector Rotation commands (consolidated Batch E). Each
/// prints one JSON document on stdout and runs against the active profile:
///
///   --etf-research request  [--out <file.json>]
///         the acquisition request a manual refresh would send (no network)
///   --etf-research refresh
///         the user-initiated "Refresh ETF Research Data" run (network)
///   --etf-research snapshot --out <file.json> [--as-of <instant>] [--known-at <instant>] [--compact]
///         the research snapshot recomputed from stored observations (reads only)
///   --etf-research export   --out <file.json>
///         every research-source table row (restart / replay checks)
///   --etf-research status
///         table counts and the last completed refresh
///   --etf-research capture  --out-dir <dir> [--width <px>] [--height <px>] [--as-of] [--known-at]
///         native off-screen renders of the workspace views (QWidget::grab), from
///         stored data only; no acquisition
///
/// Exit codes: 0 ran (source failures are outcomes in the JSON); 2 usage;
/// 1 nothing ran (database not open, profile in use, write failure, timeout).
int run_etf_research_cli(int argc, char* argv[]);

/// Native self-test of the ETF Flow & Sector Rotation workspace inside the
/// shipped binary, against the active profile: the research universe parses,
/// migration v053 is applied, the real screen opens off screen and completes
/// its stored-data read, NO acquisition is started and no research or ETF row
/// is written, and the snapshot of a fixed frame is byte-identical twice.
int run_etf_research_selftest();

} // namespace fincept::marketlab
