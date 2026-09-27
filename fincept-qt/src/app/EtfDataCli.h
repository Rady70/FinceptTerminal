#pragma once

#include <QString>

namespace fincept::marketlab {

/// True when argv carries `--etf-data`.
bool etf_data_cli_requested(int argc, char* argv[]);

/// True when argv carries a headless ETF command that needs the profile's
/// database: `--etf-data` or `--selftest-etf-data`.
bool etf_headless_command_requested(int argc, char* argv[]);

/// When another MarketLab process already runs with this profile it owns the
/// profile's database, and the single-instance lock would only forward argv to
/// it (which raises its window and runs nothing). The headless ETF commands
/// refuse instead of exiting 0 without running: `--etf-data` prints
/// {"ok": false, "error": "profile_in_use", ...}, `--selftest-etf-data` a FAIL
/// line. Returns the exit code, 1.
int refuse_etf_command_profile_in_use(int argc, char* argv[], const QString& profile);

/// Headless ETF Capital Flows data-foundation command (Batch B), with the
/// derived values of Batch C. Runs one on-demand operation against the active
/// profile's database and prints one JSON document on stdout:
///
///   --etf-data sec-nport  --cik <cik> [--series <S#########>] [--max-filings <1-40>]
///                         [--from <yyyy-MM-dd>] [--to <yyyy-MM-dd>]
///   --etf-data ibkr-daily --symbol <SYM> [--duration "<n> D|W|M|Y">]   (default "2 Y")
///   --etf-data derived    --out <file.json> [--as-of <instant>] [--known-at <instant>]
///                         [--cik <cik> [--series <S#########>]] [--symbol <SYM> | --con-id <n>]
///                         [--reference-symbol <SYM> | --reference-con-id <n>]
///                         [--from <yyyy-MM-dd>] [--to <yyyy-MM-dd>]
///   --etf-data export     --out <file.json>
///   --etf-data status
///
/// `derived` writes the regulatory-flow analytics and the rotation-proxy
/// measures of the stored subjects (all of them, or those named) to --out. It
/// reads only. Instants are ISO-8601 with Z or an offset; both default to the
/// time of the run and are recorded in the file, so the same request over the
/// same store reproduces it byte for byte. --from/--to bound the listed months
/// and sessions, never the history a calculation uses. A reference is only
/// ever the one the caller names: no benchmark is assigned here.
///
/// Exit codes: 0 the operation ran and its outcome is in the JSON (a source
/// error is an outcome, not a crash); 2 usage error; 1 nothing ran because the
/// database is not open or a running MarketLab owns the profile
/// (profile_in_use), the run did not finish in time, or the export could not
/// be written.
/// There is no UI and nothing is scheduled; combine with --profile to keep a
/// validation run out of the everyday profile.
int run_etf_data_cli(int argc, char* argv[]);

} // namespace fincept::marketlab
