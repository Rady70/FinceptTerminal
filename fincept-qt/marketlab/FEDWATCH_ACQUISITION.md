# FedWatch bounded acquisition

This backend change starts from `main` at
`dcaf3bec15e5ff0fa25080f773cba8a7bffcbaca`. It now integrates finalized
Batch C application main
`971a8b86c8dfc6910bc77ee92524d9b575f73ff1` (PR #39 merged). The acquisition
branch preserves both histories through a local integration merge. The
finalized panel is wired below; layout/charts/primary controls are preserved.

The workflow is **explicit acquisition → validation → durable storage → local
reuse**. There is no scheduler, browser automation, paid API, or background
collection. `EconomicsService` bypasses its generic result cache for
`fedwatch_data.py` and excludes that script from DataHub refresh replay. The
Python backend owns acquisition reuse and freshness.

## Commands and consumer contract

All commands retain the existing Economics JSON envelope. Use the existing
Economics service, script `fedwatch_data.py`, source tag `fedwatch`.

| User operation | Backend command | Internet |
|---|---|---|
| Open saved research | `local_snapshot` | None |
| Change meeting/outcome/range/method; view details | `local_snapshot`, `history_meetings`, `history_series`, `history_analytics`, `history_sources`, `history_compare_cme` | None |
| Manual current Refresh | `collect --meeting YYYY-MM-DD` | Existing bounded current requests; complete valid recent attempts reused |
| Explicit refresh even when recently retained | `collect --meeting YYYY-MM-DD --force` | Existing current requests |
| Explicit Load/Retry Polymarket history (including resolved meetings) | `history_backfill --meeting YYYY-MM-DD` | Existing validated-token CLOB backfill; stored refresh state reused |
| Import permitted published history | `history_cme_import --meeting YYYY-MM-DD --file PATH --source-url CME-URL` | None |
| Reconstruct from permitted monthly exports | `history_zq_import --input-format investing --data-dir PATH --watch-date YYYY-MM-DD --meeting YYYY-MM-DD` | Official calendar and two FRED target histories, once per import batch; unchanged input/complete retained selected dates reused locally |

`--db PATH` is supported for isolated research/tests; normal runtime uses the
existing `FINCEPT_DATA_DIR/fedwatch/fedwatch_history.db`. The `snapshot`,
`fed_side`, `fred_target`, `fomc_meetings`, and `polymarket_fomc` commands remain
explicit current-provider commands for existing callers. They must not be used
for saved-data navigation. Direct CLI invocation is user-triggered; it is not a
background acquisition mechanism.

Current reuse is limited to **6 hours** and requires a complete acquisition,
the requested meeting, usable Fed-side values, validated Polymarket mappings,
and still-current CLOB quotes under the existing three-day window. Partial
attempts are not replayed as a successful current refresh. `--force` retries
explicitly. An unscoped `collect` always discovers the complete upcoming
inventory: a selected meeting cannot prove that inventory is complete.
Loading old saved data re-evaluates quote age and meeting lifecycle:
stale Fed-side values cannot populate current distributions, stale Polymarket
values retain `STALE`, and current comparisons are cleared. Resolved/past/pending
selected meetings return retained history without a current provider call.
An explicit Refresh of a stored UPCOMING or PENDING meeting whose day has passed
advances only that meeting using at most the two existing FRED target-history
requests. It resolves from official paired observations or becomes PENDING
with the exact refusal reason. PENDING retries are selected and FRED-only,
as authorized by the owner after the latest review. No Investing/current Polymarket/calendar query
is made and the old probability acquisition timestamp is not refreshed.

Resolved Refresh remains fully local. Explicit Load/Retry history is available
for a retained validated selected token with absent or retryable Polymarket
history, including after current mapping revalidation returns NOT_FOUND or
AMBIGUOUS. These actions use the normal bounded backfill without `--force`.
A successful resolved backfill reuses its terminal state without further provider
requests; it does not enable current probabilities. PENDING Refresh reports
"Retrying FRED meeting resolution" while its selected FRED-only request runs.

Schema **v4** stores the latest current attempt independently for each meeting.
A selected refresh replaces that meeting's attempt even when it returns no
meetings; other meetings keep their original values, errors and timestamps.
A full refresh replaces all prior attempts, including omitted meetings after a
failure. Errors with another explicit `detail.meeting_date` or `detail.meeting_dates` are excluded from
each meeting's envelope; `partial` and `failed_components` are recomputed.
FRED/calendar/discovery failures without a meeting identity remain global.
Token-request failures inherit their validated meeting identity. Selected
Investing date validation ignores unrelated dates without narrowing the
cumulative chain needed for the local probability transformation.
Historical lifecycle collection is separately retained in `history_collection`.
Its errors stay inspectable after restart but do not determine unrelated
current-acquisition partial/reuse state. When lifecycle evaluation is the explicit
selected Refresh operation, its failure returns a partial Economics envelope
and appears in the normal meeting summary, including after reopening. The
retained probability quality and acquisition clock remain separate. Current-context FRED, calendar and discovery failures still
fail closed. Per-meeting retained source statuses/warnings are scoped to that
meeting; shared method notes remain global methodology. The full acquisition
response keeps aggregate diagnostics. A compact `aggregate_acquisition` report
(the latest unscoped attempt timestamp, errors, warnings and source statuses)
is retained in the existing per-meeting JSON envelopes and exposed on local
overview reads. It survives selected writes without affecting selected reuse.
This is a bounded last-report record, not an event journal or a new schema.
A selected official meeting without usable Investing observations reports
`INVESTING_SELECTED_MEETING_UNAVAILABLE`, rather than source success with no
quality explanation. Unrelated date errors remain excluded from selected quality.

The initial empty-key failure diagnostic is removed when real attempts exist. Accepted observation history is never used to manufacture current
state. Local overview reads qualify each meeting separately and expose its
acquisition age; overview `retrieved_at` is null because there is no single
retrieval instant. Source entries carry their meeting scope. The top-level
target context is null when retained contexts disagree or any is stale.

Opening v2 adds the table without changing observations. Opening v3 migrates
its singleton envelope transactionally into the meetings actually present;
it cannot recover meetings already overwritten by the old singleton. An empty
legacy attempt remains an unscoped diagnostic. Invalid legacy JSON refuses the
migration and preserves the v3 table. Older v2/v3 applications refuse v4.
Qualification uses temporary databases, not the user's application profile.

Default `history_series` returns only `LIVE_INVESTING_DERIVED`,
`HISTORICAL_ZQ_RECONSTRUCTED`, and `POLYMARKET_CLOB` local-change rows, labelled
`LOCAL_MEETING_CHANGE_BP`. Published CME absolute target bands require an
explicit `--method HISTORICAL_CME_PUBLISHED_TARGET_RANGE` request.

## Public availability and access limitations

The detailed 2026-10-03 source audit, live request accounting, and validation
record are in the control repository's
`docs/FEDWATCH_FREE_ACQUISITION.md`. `history_sources` exposes route availability,
implementation, automation limitations and actual retained method coverage.
CME interchange and Investing monthly imports are explicitly
`PROVISIONAL_FIXTURE_ONLY`: zero representative real provider files have been
consumed. Native CME workbook and real Investing export compatibility remain
unqualified until a permitted representative input is supplied.

These are distinct: `AVAILABLE_AND_INTEGRATED`,
`AVAILABLE_NOT_IMPLEMENTED`, `AVAILABLE_MANUAL_OR_USER_TRIGGERED`,
`DERIVED_OR_RECONSTRUCTED`, `PARTIAL`, `PROVIDER_TEMPORARILY_FAILED`,
`NOT_QUALIFIED_FOR_AUTOMATED_COLLECTION`, and `UNAVAILABLE` for a specific
requested observation/coverage. A provider error changes an attempt's quality;
it does not establish that public data does not exist.

CME's public guide describes historical Excel downloads for selected meetings
with up to one year of history. Its website data-use restrictions prevent
qualifying a new automated downloader here. Investing exposes monthly futures
history but also restricts storage/reuse without permission. These imports
accept only files lawfully obtained and permitted for this use; selecting a URL
or supplying a file is not proof of provider permission. The existing qualified
Investing current parser is preserved, not newly licensed by this work.

Yahoo is a reference-only route: one individual contract working once does not
prove a full monthly ladder or historical depth. FRED target/EFFR series and
the Fed calendar are official contextual inputs, not probability histories.
Polymarket's identity, freshness and daily backfill semantics are retained;
the failure-isolation corrections below separate identity from Gamma prices.

## Published probability import

This is a small explicit **CSV interchange**, not a qualified parser for CME's
native XLS/XLSX. Preserve the permitted original workbook locally. Export/map
the selected meeting's sheet to UTF-8 CSV with these exact columns:

```csv
meeting_date,observation_date,rate_low_bp,rate_high_bp,probability_pct
2026-10-28,2026-09-25,375,400,60
2026-10-28,2026-09-25,400,425,40
```

These are **synthetic format examples**, not source observations. Rates are
absolute target bounds in bp (375 means 3.75%); probability is percent, not a
0–1 fraction. Meeting identity must match every row and a retained official
calendar entry. Future calendar fallback identity requires its existing freshness
window; historical entries may be checked against the retained dated calendar.
Observation dates cannot exceed the meeting or import date. Bands are exact
25 bp targets, with complete totals within 0.5 pp of 100. Rounding normalization
retains raw values and factors. Invalid rows quarantine their identifiable
reporting date, including when the remaining buckets would sum to 100.
Complete independently valid dates are imported and rejected dates/errors are
reported explicitly. An unreadable reporting date cannot identify which
distribution lost a bucket, including a bucket inside the rounding tolerance:
`FEDWATCH_PUBLISHED_HISTORY_DATE_UNCERTAIN` refuses the import before writes.
Invalid headers/source identity/size bounds or no usable dates remain
whole-import failures. Values are never zero-filled.

The method is `HISTORICAL_CME_PUBLISHED_TARGET_RANGE`, source `cme_published`,
quality `PUBLISHED_USER_IMPORT`, provenance `USER_DECLARED_CME_PUBLISHED`.
`outcome_bp` for this method is **absolute upper target bp**, with financial
object `TARGET_RANGE_UPPER_BP`. It is not an outcome delta. The CLI reports this
object explicitly. This method is excluded from local-change analytics and
current differences. A cumulative target distribution for a later meeting
cannot be equated with the change made at that meeting.

Each row retains the file SHA-256, source URL, target bounds, report date, raw
percent and normalized percent. Date-only observations use UTC midnight as a
storage key; publication time is unknown. They are not intraday/PIT evidence.
Identical date/band/value reimports are duplicates; same-band restatements
revise those rows using the existing store semantics. Conflicting duplicates
inside one input or revisions changing the retained bucket set are rejected
for review, preserving stored history. Files are bounded to 5 MB/50,000 rows.
Native download format, actual export compatibility and rights remain live
qualification limitations until a permitted representative file is supplied.

## Monthly futures reconstruction (provisional)

The optional `qualified` input format remains available and unchanged in
structure. `--input-format investing` adds a local adapter for permitted daily
exports. Name each file `ZQ<month code><yy>.csv`, prepend the verified monthly
symbol and contract-specific URL, then retain Investing's export header:

```csv
Symbol: ZQV26
Source: https://www.investing.com/rates-bonds/cbot-30-day-federal-funds-comp-c1-futures-historical-data?cid=VERIFIED_CONTRACT_ID
Date,Price,Open,High,Low,Vol.,Change %
"Sep 25, 2026",96.4,96.4,96.4,96.4,1K,0%
```

This is a synthetic format example. Verify the **actual monthly contract** from
the provider before adding the declaration. Rolling `FFc1`/`FFcN` and the `cid`
alone do not establish a fixed month. The adapter retains user-declared identity,
URL, digest, span and price type. `Price` is an indicative daily close, not CME
settlement. Missing close is `None`; absent OI/volume remain `None` and confidence
is flagged. Conflicting duplicates, malformed input and generic-symbol declarations
are rejected at the daily-observation or file-identity boundary. Good rows and
other valid contracts survive. Dates are whitespace-normalized. Standardized
rows carry `close_status: OBSERVED`, `SOURCE_MISSING` or `REJECTED`; file reports
retain separate missing/rejected counts and dated `close_quality` entries in
stored reconstructed-observation detail. A rejected dated close remains an
explicit unknown, preventing earlier-price carryover across it. An undated
rejection preserves good observations but marks the contract `DATE_UNCERTAIN`:
its price lookup is blocked, and `FEDWATCH_ZQ_DATE_UNCERTAIN` blocks a watch-date
reconstruction requiring that contract. Unneeded contracts do not block an
independent selected-meeting reconstruction. No website collection is
implemented. Input is bounded to 60
files, 5 MB and 20,000 rows per file.

For a watch date in September 2026, the 28 October meeting requires the
September, October and November contracts (`ZQU26`, `ZQV26`, `ZQX26`), with
observations on/before the watch date. The 9 December meeting also requires
December and the following January (`ZQZ26`, `ZQF27`). Generally retain every
month from watch month through the selected meeting's following month. Full
calendar classification includes meetings earlier in the watch month. A
selected-meeting frame explicitly includes the following-month propagation
anchor; the existing formula and default all-meeting path are unchanged.

Accepted local changes retain `HISTORICAL_ZQ_RECONSTRUCTED`. Per-meeting
cumulative distributions and input digests are retained in detail for
`history_compare_cme`: compare only matching report dates/absolute target
bands; missing bands are `null`. Differences are **ZQ minus published** in pp,
not evidence of equivalence. Timing is date matched, not necessarily simultaneous.
The existing current Fed-side/Polymarket difference remains Polymarket minus
Fed-side. No history is interpolated or created by polling current quotes.

## Acquisition/parser failure isolation — 2026-10-04

Audited application baseline: `65cc9b35683d740b4eab798b10a59072a5d5e533`.
The natural financial unit is the rejection boundary:

- **Investing:** complete meetings normalize independently; dropped buckets,
  conflicting duplicate buckets, overlapping ranges or invalid totals reject
  their meeting. The first complete usable block wins as a whole; an intact
  later copy of the exact meeting can recover a broken main copy, reported in
  `recovered_meeting_dates`. Copies are never blended. Each bucket is bounded
  by its own closing div and must match exactly one complete row; unrelated
  trailing markup cannot replace a missing bucket. Meeting blocks are bounded
  before extraction, and cosmetic width styles/HTML attributes do not determine
  probability identity. Payloads carry `errors` and `parse_report`.
  `normalize_cumulative` returns only successful normalization metadata;
  supplying its optional `rejected` list explicitly enables partial acceptance
  and receives separate errors. Its default is strict. No usable meetings
  always raises. A local change requires the adjacent predecessor:
  `PREVIOUS_MEETING_UNAVAILABLE` and
  `INVESTING_LOCAL_DEPENDENCY_UNAVAILABLE` keep cumulative values inspectable
  without producing a multi-meeting change. A complete official schedule
  establishes the sequence; unknown source identities cannot establish it.
  Off-schedule dates report `MEETING_DATE_MISMATCH`/
  `INVESTING_MEETING_DATE_MISMATCH`, separately from predecessor failure. The
  production path and qualified helper share the integer/mantissa conversion
  and six-decimal rounding boundary, with direct production numerical tests.
- **CLOB:** current and backfill share individual point validation, including
  numeric timestamp ordering, boolean/non-finite/negative-epoch rejection,
  future tolerance and `[0,1]` probabilities. Conflicting values at the same
  instant quarantine that instant. Current reads use the latest valid point
  and expose per-outcome `point_quality`; they retain its real timestamp and
  freshness. A bad probability sum leaves individually valid quotes readable
  as `PARTIAL`, without current comparison. Invalid document shapes are
  explicit retryable errors, distinct from a genuine empty history.
- **Gamma/discovery:** embedded prices are advisory mapping evidence
  (`gamma_price_status`), not meeting identity. Exact event dates/titles,
  question identities, unique outcomes/tokens and binary Yes/No structure
  remain required. Case/whitespace normalization of binary labels preserves
  the exact Yes-token index, including reversed Yes/No order; duplicate or
  nonbinary labels remain invalid. A failed later discovery page preserves earlier candidates,
  with `page_error` and `coverage_complete: false` rather than exhaustive absence.
- **FRED:** malformed rows, non-finite values and conflicting dates are
  excluded individually, sorted observations survive, and `parse_reports`
  plus `FRED_PARSE_PARTIAL` expose rejection evidence. Headers must identify a
  date and one unambiguous requested series. Extra declared columns are allowed
  when the named requested series identifies its column; unnamed extra-column
  shapes and duplicate requested-series columns remain invalid. `.` remains
  ordinary source missingness.
  The latest valid non-future common-date range survives lagging/rejected
  bounds as `STALE`; ranges older than three days are also stale. Current
  conversion/comparison never use a stale pair, and the panel labels its
  observation date. Lifecycle resolution refuses a rejected identifiable date
  inside the required before/first-post window, rather than skipping to a
  later hold. Unrelated old rejected dates do not block that decision.
- **FOMC:** a partial live page uses a fresh tracked calendar backup to recover
  coverage as `SCRAPED_WITH_FALLBACK`. Capture time, `fallback_used`, live
  `parse_report` and per-row sources remain explicit. Live rows replace
  same-date/overlapping fallback identities, with `merge_conflicts` reporting
  both identities and `LIVE_ROW_WINS`. Contradictory live identities remain
  quarantined with incomplete coverage. Unavailable, malformed or stale backups
  leave valid live rows as `SCRAPED_PARTIAL`, `coverage_complete: false`, without
  treating those live observations as stale. Positive live identities may
  anchor mapping/comparison; incomplete coverage cannot disprove absent dates.
  Invalid statement dates, reversed ranges, conflicting identities and
  unfinished rows are isolated. ZQ reconstruction still requires complete
  calendar coverage because it needs the entire intervening schedule.
- **CSV imports:** files and complete reporting dates are independent.
  Malformed quote rows cannot consume subsequent valid rows. Supported
  numeric/date fields occupy one physical line; multiline cells are invalid.
  Rejected dated monthly closes stay unknown; CME distributions with any
  identifiable bad row are rejected as a whole date. Undated monthly rejection
  blocks the affected required contract's reconstruction; undated published
  rows block certification of any reporting distribution in that input.
  Retained history is preserved when a date revision changes its bucket set.

Aggregate errors remain available. Explicit meeting-scoped errors are excluded
from unrelated selected refreshes/retained attempts; dependency errors belong
to the meeting whose local change is unavailable. Calendar/FRED/discovery
coverage errors remain global. No missing data becomes zero and no partial
distribution becomes current merely because its remaining values sum to 100.

Regressions: `marketlab/tests/test_fedwatch_failure_isolation.py`,
`marketlab/tests/test_fedwatch_review_corrections.py` and the existing
provider/acquisition suites. Run from `fincept-qt`:

```text
python -m unittest discover -s marketlab/tests
cmake --build build/win-dev --target tst_fedwatch_panel tst_fedwatch_view_model tst_fedwatch_dispatch
ctest --test-dir build/win-dev -R "^tst_fedwatch_(panel|view_model|dispatch)$" --output-on-failure
```

These are offline injected-transport/store regressions and hidden Qt widget
checks. They do not qualify live endpoints or native CME/Investing exports.
The control record is `docs/FEDWATCH_FAILURE_ISOLATION.md` in Market_Lab.

The review corrections are isolated on `codex/fedwatch-failure-isolation-review`
in `C:\Users\non_s\AppData\Local\Temp\opencode\MarketLab-FedWatch-Review`.
The final correction run passed **496 Python tests** (60.303 s), including
25 additional reviewer regressions, and **3/3 Qt gates** (352.03 s). Qt targets
were rebuilt from this worktree into
`C:\Users\non_s\AppData\Local\Temp\opencode\fedwatch-review-build` using
MSVC 19.44/Qt 6.8.3 and existing unchanged dependency sources. These corrections
have not been synchronized into the shared checkout's application executable.

## Finalized Batch C consumer wiring

Activation uses `history_meetings`, `local_snapshot`, `history_series`, and
`history_analytics` only. Meeting changes reload retained current/history;
outcome/method changes reload local analytics; range/details controls use
local state. No activation/navigation dispatches `collect` or history imports.
The existing horizontal current bars, compact navigation, default upcoming
0 bp outcome, adaptive charts and source-specific outside-range labels remain.
Validated mapping outcomes remain available before the first stored quote.

Primary Refresh dispatches `collect --meeting <selected upcoming date>` once.
Resolved Refresh remains local-only. PENDING Refresh explicitly retries only
official FRED history, with no upcoming-current collection. Empty inventory stays empty on
activation and offers deliberate acquisition via the existing Refresh or
Update upcoming meetings buttons. Update upcoming meetings explicitly performs
unscoped `collect`. PENDING meetings permit explicit history backfill using
their previously VALIDATED tokens even after negative current revalidation;
UPCOMING demoted mappings remain blocked and no current quote is rehabilitated.
RESOLVED Refresh remains fully local, without current probability acquisition.
Both explicit history actions permit qualified resolved Load/Retry when history
is absent or retryable, using retained previously VALIDATED identity even after
NOT_FOUND/AMBIGUOUS current revalidation. Completed terminal resolved backfills
are reused locally and do not issue repeated provider requests. Historical
mapping provenance does not rehabilitate invalid current quotes.
Load/Retry history remains explicit bounded Polymarket
backfill; provisional CME/monthly imports are never automatic or new primary
controls. Their `PROVISIONAL_FIXTURE_ONLY` status is retained in Research details.

## Review correction validation

The review regressions first reproduced both defects on the reviewed head.
The corrected offline suite includes October/December retention across store
and process restarts, failed selected/full attempts, independent quote ages,
v2 preservation, transactional v3 migration/rollback, and default financial
object separation. The C++ dispatch test links the real EconomicsService and
DataHub with small test-only process/cache recorders. Explicit `collect
--meeting` dispatches once; producer/hub replay cannot dispatch FedWatch, while
the same hub successfully replays a non-FedWatch control request. The combined
panel regression uses the existing injected dispatch seam and
real Python CLI history/analytics, with captured current responses. A dedicated
activation/navigation case reads the actual SQLite retained current through
`acquisition.local_snapshot` with a deterministic clock; it records zero
network commands. Manual Refresh records exactly one selected collect.
Pending/resolved refresh and empty-inventory activation are explicitly tested.
These are hidden-widget fixtures and service checks, not owner native acceptance.
Exact-head full application build receipts are retained in the paired control
record. Existing Batch C owner acceptance is historical; this new combined
executable needs later owner manual inspection. No desktop automation is run.
