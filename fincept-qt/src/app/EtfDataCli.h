#pragma once

namespace fincept::marketlab {

/// True when argv carries `--etf-data`.
bool etf_data_cli_requested(int argc, char* argv[]);

/// Headless ETF Capital Flows data-foundation command (Batch B). Runs one
/// on-demand operation against the active profile's database and prints one
/// JSON document on stdout:
///
///   --etf-data sec-nport  --cik <cik> [--series <S#########>] [--max-filings <1-40>]
///                         [--from <yyyy-MM-dd>] [--to <yyyy-MM-dd>]
///   --etf-data ibkr-daily --symbol <SYM> [--duration "<n> D|W|M|Y">]   (default "2 Y")
///   --etf-data export     --out <file.json>
///   --etf-data status
///
/// Exit codes: 0 the operation ran and its outcome is in the JSON (a source
/// error is an outcome, not a crash); 2 usage error; 1 the database is not
/// open, the run did not finish in time, or the export could not be written.
/// There is no UI and nothing is scheduled; combine with --profile to keep a
/// validation run out of the everyday profile.
int run_etf_data_cli(int argc, char* argv[]);

} // namespace fincept::marketlab
