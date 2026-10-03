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
| Load Polymarket history | `history_backfill --meeting YYYY-MM-DD` | Existing validated-token CLOB backfill; stored refresh state reused |
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

Schema **v4** stores the latest current attempt independently for each meeting.
A selected refresh replaces that meeting's attempt even when it returns no
meetings; other meetings keep their original values, errors and timestamps.
A full refresh replaces all prior attempts, including omitted meetings after a
failure. Errors with another explicit `detail.meeting_date` are excluded from
each meeting's envelope; `partial` and `failed_components` are recomputed.
FRED/calendar/discovery failures without a meeting identity remain global.
Token-request failures inherit their validated meeting identity. Selected
Investing date validation ignores unrelated dates without narrowing the
cumulative chain needed for the local probability transformation.
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
Polymarket's current mapping/quality rules and daily CLOB backfill remain intact.

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
retains raw values and factors. Missing/empty/invalid input is rejected before
writing, never zero-filled.

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
are rejected. No website collection is implemented. Input is bounded to 60
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

## Finalized Batch C consumer wiring

Activation uses `history_meetings`, `local_snapshot`, `history_series`, and
`history_analytics` only. Meeting changes reload retained current/history;
outcome/method changes reload local analytics; range/details controls use
local state. No activation/navigation dispatches `collect` or history imports.
The existing horizontal current bars, compact navigation, default upcoming
0 bp outcome, adaptive charts and source-specific outside-range labels remain.
Validated mapping outcomes remain available before the first stored quote.

Primary Refresh dispatches `collect --meeting <selected upcoming date>` once.
Resolved/PENDING Refresh remains local-only. Empty inventory stays empty on
activation and offers deliberate acquisition via the existing Refresh or
Update upcoming meetings buttons. Update upcoming meetings explicitly performs
unscoped `collect`. Load/Retry history remains explicit bounded Polymarket
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
