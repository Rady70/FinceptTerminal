"""Investing.com Fed Rate Monitor: the live Fed-side probability source.

Rehomed from the previously qualified ``fedwatch-vs-polymarket`` component
(``fedwatch/livesource/investing.py``, revision
04b15bcd1e7d9f911630d5f3afcbb0a6945d4115) with the project-owned rounding
boundary added at qualification time. The parsing rules, the local-step
conversion, and the normalization acceptance band are preserved exactly; see
FEDWATCH_INTEGRATION_PLAN.md sections 3 and 4.

Semantics that must not drift:

* Investing publishes a *rounded cumulative display distribution* per meeting
  (probability that the target range equals each bucket at that meeting). It is
  NOT a raw CME/ZQ observation and must never be labelled as one.
* Each meeting's raw displayed percentages are validated (finite, non-negative,
  sum within [99.5, 100.5]) and then scaled by the smallest possible uniform
  factor ``100 / raw_sum``. The whole cumulative table is normalized BEFORE the
  local-step conversion, because the conversion chains each meeting's expected
  rate through the previous meeting; a later meeting can legitimately shift
  even when its own displayed sum is exactly 100.
* A malformed distribution (sum outside the band, negative, non-finite) rejects
  the current conversion; it is never silently repaired or replaced.
* The derived meeting-level ("local") distribution reuses the unchanged CME
  integer+mantissa split: at most two adjacent 25 bp outcomes. This binary
  local shape is intentional; Polymarket may price broader tails.
"""

from __future__ import annotations

import math
import re
from datetime import datetime

from fedwatch import timeutil
from fedwatch.errors import (
    PROVIDER_INVESTING,
    FedwatchError,
    InvestingDistributionError,
)
from fedwatch.transport import Transport, TransportError

FED_RATE_MONITOR_URL = "https://www.investing.com/central-banks/fed-rate-monitor"

# The honest, self-identifying bot User-Agent the qualified implementation
# settled on: a spoofed browser UA was rejected with 403, this one returned the
# same valid page. MarketLab identifies itself for the same reason.
USER_AGENT = (
    "MarketLabTerminal/0.1.0 (+https://github.com/Rady70/Market_Lab; "
    "personal research project; ~1 request per manual run)"
)
REQUEST_TIMEOUT = 20

# One meeting block: "Meeting Time: <date>" ... "Future Price: <price>" followed
# by the bucket rows before the next infoFed block (or end of document).
_MEETING_BLOCK_RE = re.compile(
    r"Meeting Time:</span>\s*<i>([^<]+)</i>.*?"
    r"Future Price:</span>\s*<i>([^<]+)</i>(.*?)(?=<div class=\"infoFed\">|\Z)",
    re.S,
)
# One bucket row: interval label + displayed percentage.
_BUCKET_ITEM_RE = re.compile(
    r'percfedRateItem">\s*<span>([^<]+)</span>\s*<i></i>\s*'
    r'<div[^>]*style="width: [0-9.]+%"></div>\s*<span>([0-9.]+)%</span>'
)
_MEETING_TIME_FORMAT = "%b %d, %Y %I:%M%p ET"

BP_STEP = 25
NORMALIZATION_MIN_SUM = 99.5
NORMALIZATION_MAX_SUM = 100.5


def fetch_fed_rate_monitor_html(transport: Transport, url: str = FED_RATE_MONITOR_URL) -> str:
    try:
        return transport.get_text(url, headers={"User-Agent": USER_AGENT}, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        raise FedwatchError(
            PROVIDER_INVESTING,
            "INVESTING_SOURCE_UNAVAILABLE",
            f"Investing.com Fed Rate Monitor request failed: {exc}",
            detail=exc.detail(),
        ) from exc


def parse_fed_rate_monitor(html: str) -> tuple[list[dict], list[str]]:
    """Parse the page's embedded Fed Rate Monitor table.

    Returns ``(rows, warnings)`` where each row is
    ``{meeting_date, rate_low, rate_high, probability_pct}`` and ``meeting_date``
    is an ISO date string. Rows that cannot be parsed are skipped with a warning
    rather than aborting the whole retrieval, because the page structure is
    Investing.com's own and can change without notice. An empty result is
    reported as an empty list; the provider layer turns that into an explicit
    ``INVESTING_PARSE_EMPTY`` failure.

    Deduplication matches the qualified behavior: the sidebar "Fed Rate Monitor
    Tool" repeats the nearest meeting, the main table appears first, so the
    first occurrence of a (meeting, bucket) pair wins. Rows are then sorted by
    (meeting_date, rate_low).
    """
    rows: list[dict] = []
    warnings: list[str] = []
    blocks = _MEETING_BLOCK_RE.findall(html)
    if not blocks:
        warnings.append(
            "no meeting blocks found in the Investing.com page; the HTML structure may have changed"
        )

    for meeting_time_raw, _future_price_raw, rest in blocks:
        try:
            meeting_date = datetime.strptime(
                meeting_time_raw.strip(), _MEETING_TIME_FORMAT
            ).date()
        except ValueError:
            warnings.append(f"unparseable meeting time {meeting_time_raw!r}; block skipped")
            continue

        items = _BUCKET_ITEM_RE.findall(rest)
        if not items:
            warnings.append(
                f"no parseable bucket rows for meeting {meeting_date.isoformat()}; meeting skipped"
            )
            continue

        for bucket_label, pct_raw in items:
            try:
                low_raw, high_raw = bucket_label.split("-")
                rate_low, rate_high = float(low_raw.strip()), float(high_raw.strip())
            except ValueError:
                warnings.append(
                    f"unparseable target-rate interval {bucket_label!r} for meeting "
                    f"{meeting_date.isoformat()}; row skipped"
                )
                continue

            rows.append(
                {
                    "meeting_date": meeting_date.isoformat(),
                    "rate_low": rate_low,
                    "rate_high": rate_high,
                    "probability_pct": float(pct_raw),
                }
            )

    deduplicated: list[dict] = []
    seen: set[tuple] = set()
    for row in rows:
        key = (row["meeting_date"], row["rate_low"], row["rate_high"])
        if key in seen:
            continue
        seen.add(key)
        deduplicated.append(row)
    deduplicated.sort(key=lambda row: (row["meeting_date"], row["rate_low"]))
    return deduplicated, warnings


def normalize_cumulative(rows: list[dict]) -> tuple[list[dict], list[dict]]:
    """Validate and normalize each Investing cumulative meeting independently.

    Returns ``(normalized_rows, records)``. The input is never mutated. The
    correction is the smallest possible: every bucket in a meeting is scaled by
    the same factor ``100 / raw_sum``. A meeting whose raw sum falls outside
    [99.5, 100.5], or whose values are negative or non-finite, raises
    :class:`InvestingDistributionError` (a ``ValueError``).
    """
    if not rows:
        raise InvestingDistributionError(
            "INVESTING_DISTRIBUTION_INVALID", "Investing cumulative table is empty"
        )

    normalized_rows = [dict(row) for row in rows]
    records: list[dict] = []
    meeting_dates = sorted({row["meeting_date"] for row in normalized_rows})

    for meeting_date in meeting_dates:
        group = [row for row in normalized_rows if row["meeting_date"] == meeting_date]
        try:
            values = [float(row["probability_pct"]) for row in group]
            midpoints = [
                (float(row["rate_low"]) + float(row["rate_high"])) / 2.0 for row in group
            ]
        except (TypeError, ValueError) as exc:
            raise InvestingDistributionError(
                "INVESTING_DISTRIBUTION_INVALID",
                f"invalid Investing probabilities for meeting {meeting_date}",
                detail={"meeting_date": meeting_date},
            ) from exc

        if any(not math.isfinite(value) or value < 0 for value in values):
            raise InvestingDistributionError(
                "INVESTING_DISTRIBUTION_INVALID",
                f"invalid Investing probabilities for meeting {meeting_date}",
                detail={"meeting_date": meeting_date},
            )

        raw_sum = float(sum(values))
        if not math.isfinite(raw_sum) or not (
            NORMALIZATION_MIN_SUM <= raw_sum <= NORMALIZATION_MAX_SUM
        ):
            raise InvestingDistributionError(
                "INVESTING_DISTRIBUTION_INVALID",
                f"Investing probability sum {raw_sum!r} is outside the accepted range "
                f"[{NORMALIZATION_MIN_SUM}, {NORMALIZATION_MAX_SUM}] for meeting {meeting_date}",
                detail={"meeting_date": meeting_date, "raw_probability_sum_pct": raw_sum},
            )

        normalization_factor = 100.0 / raw_sum
        normalized_values = [value * normalization_factor for value in values]
        normalized_sum = float(sum(normalized_values))
        if not math.isclose(normalized_sum, 100.0, rel_tol=0.0, abs_tol=1e-9):
            raise InvestingDistributionError(
                "INVESTING_DISTRIBUTION_INVALID",
                f"normalized Investing probability sum {normalized_sum!r} is not 100% "
                f"for meeting {meeting_date}",
                detail={"meeting_date": meeting_date},
            )

        raw_expected_rate = float(
            sum(midpoint * value / 100.0 for midpoint, value in zip(midpoints, values))
        )
        normalized_expected_rate = float(
            sum(
                midpoint * value / 100.0
                for midpoint, value in zip(midpoints, normalized_values)
            )
        )

        for row, normalized_value in zip(group, normalized_values):
            row["probability_pct"] = normalized_value

        records.append(
            {
                "meeting_date": meeting_date,
                "rows": len(group),
                "raw_probability_sum_pct": round(raw_sum, 9),
                "normalization_applied": not math.isclose(
                    raw_sum, 100.0, rel_tol=0.0, abs_tol=1e-12
                ),
                "normalization_factor": normalization_factor,
                "normalized_probability_sum_pct": round(normalized_sum, 9),
                "raw_expected_rate": raw_expected_rate,
                "normalized_expected_rate": normalized_expected_rate,
                "expected_rate_difference": normalized_expected_rate - raw_expected_rate,
            }
        )

    return normalized_rows, records


def local_step_distribution(change: float) -> dict:
    """The unchanged CME integer+mantissa split for one FOMC meeting.

    ``change`` is the expected number of 25 bp steps at the meeting (negative =
    cut). The result is exactly two adjacent outcomes (or a point mass when the
    change is an exact number of steps): the unique two-point distribution that
    matches a single expected value.
    """
    sign = 1 if change >= 0 else -1
    abs_change = abs(change)
    floor_steps = math.trunc(abs_change)
    mantissa = abs_change - floor_steps

    if mantissa == 0.0:
        return {sign * floor_steps * BP_STEP: 1.0}

    bp_floor = sign * floor_steps * BP_STEP
    bp_next = sign * (floor_steps + 1) * BP_STEP
    return {bp_floor: 1 - mantissa, bp_next: mantissa}


def local_steps_from_cumulative(
    cumulative: list[dict], current_rate_upper: float, current_rate_lower: float
) -> list[dict]:
    """Derive meeting-level ("local") step distributions from the cumulative table.

    ``E[local_N] = E[cumulative_N] - E[cumulative_{N-1}]`` by linearity of
    expectation, with the chain seeded at the current target-range midpoint.
    Returns rows ``{meeting_date, meeting_ordinal, local_bp_change,
    probability_pct}`` in the same shape the qualified conversion produced,
    with the meeting-level percentages rounded to 6 decimals.
    """
    if not cumulative:
        return []

    expected_current = (current_rate_upper + current_rate_lower) / 2.0
    rows: list[dict] = []
    previous_expected = expected_current
    meeting_dates = sorted({row["meeting_date"] for row in cumulative})

    for ordinal, meeting_date in enumerate(meeting_dates, start=1):
        group = [row for row in cumulative if row["meeting_date"] == meeting_date]
        expected_rate = float(
            sum(
                ((float(row["rate_low"]) + float(row["rate_high"])) / 2.0)
                * (float(row["probability_pct"]) / 100.0)
                for row in group
            )
        )
        change = (expected_rate - previous_expected) / BP_STEP * 100.0
        distribution = local_step_distribution(change)

        for bp, probability in sorted(distribution.items()):
            rows.append(
                {
                    "meeting_date": meeting_date,
                    "meeting_ordinal": ordinal,
                    "local_bp_change": bp,
                    "probability_pct": round(probability * 100.0, 6),
                }
            )
        previous_expected = expected_rate

    return rows


def fetch_distributions(transport: Transport, clock=timeutil.utc_now) -> dict:
    """Retrieve, parse, validate and normalize the current Investing table.

    Returns a JSON-ready provider payload with both the raw displayed values and
    the validated/normalized values kept distinct, plus the per-meeting
    normalization records. Raises :class:`FedwatchError` (provider
    ``investing``) for transport, empty-parse and malformed-distribution
    failures.
    """
    html = fetch_fed_rate_monitor_html(transport)
    retrieved_at = clock()
    raw_rows, warnings = parse_fed_rate_monitor(html)
    if not raw_rows:
        raise FedwatchError(
            PROVIDER_INVESTING,
            "INVESTING_PARSE_EMPTY",
            "Investing.com Fed Rate Monitor returned no parseable meeting rows",
            detail={"retrieved_at": timeutil.iso_z(retrieved_at)},
        )

    normalized_rows, records = normalize_cumulative(raw_rows)
    record_by_date = {record["meeting_date"]: record for record in records}

    raw_by_date: dict[str, list[dict]] = {}
    for row in raw_rows:
        raw_by_date.setdefault(row["meeting_date"], []).append(row)

    meetings = []
    for meeting_date in sorted(raw_by_date):
        record = record_by_date[meeting_date]
        meetings.append(
            {
                "meeting_date": meeting_date,
                "raw_probabilities": [
                    {
                        "rate_low": row["rate_low"],
                        "rate_high": row["rate_high"],
                        "probability_pct": row["probability_pct"],
                    }
                    for row in raw_by_date[meeting_date]
                ],
                "normalized_probabilities": [
                    {
                        "rate_low": row["rate_low"],
                        "rate_high": row["rate_high"],
                        "probability_pct": row["probability_pct"],
                    }
                    for row in normalized_rows
                    if row["meeting_date"] == meeting_date
                ],
                "normalization": {
                    "applied": record["normalization_applied"],
                    "raw_probability_sum_pct": record["raw_probability_sum_pct"],
                    "normalization_factor": record["normalization_factor"],
                    "normalized_probability_sum_pct": record[
                        "normalized_probability_sum_pct"
                    ],
                    "raw_expected_rate": record["raw_expected_rate"],
                    "normalized_expected_rate": record["normalized_expected_rate"],
                    "expected_rate_difference": record["expected_rate_difference"],
                },
            }
        )

    raw_sums = [record["raw_probability_sum_pct"] for record in records]
    return {
        "retrieved_at": timeutil.iso_z(retrieved_at),
        "source": FED_RATE_MONITOR_URL,
        "method": "LIVE_INVESTING_DERIVED",
        "meetings": meetings,
        "normalized_rows": normalized_rows,
        "quality": {
            "parsed_meeting_count": len(meetings),
            "parsed_row_count": len(raw_rows),
            "normalization_applied_meeting_count": sum(
                1 for record in records if record["normalization_applied"]
            ),
            "raw_probability_sum_min_pct": min(raw_sums),
            "raw_probability_sum_max_pct": max(raw_sums),
            "normalized_probability_sum_target_pct": 100.0,
        },
        "warnings": warnings,
    }


def with_local_probabilities(distributions: dict, upper: float, lower: float) -> list[dict]:
    """Merge the local-step conversion into the per-meeting fed-side sections.

    Returns a new list of meeting sections with ``local_probabilities``,
    ``meeting_ordinal`` and ``local_status: "OK"``. The normalized table is
    converted as a whole so the chained expected-rate semantics are preserved.
    """
    local_rows = local_steps_from_cumulative(
        distributions["normalized_rows"], current_rate_upper=upper, current_rate_lower=lower
    )
    by_meeting: dict[str, list[dict]] = {}
    for row in local_rows:
        by_meeting.setdefault(row["meeting_date"], []).append(row)

    ordinal_by_meeting: dict[str, int] = {}
    for row in local_rows:
        ordinal_by_meeting.setdefault(row["meeting_date"], row["meeting_ordinal"])

    sections = []
    for meeting in distributions["meetings"]:
        meeting_date = meeting["meeting_date"]
        sections.append(
            {
                "meeting_date": meeting_date,
                "method": "LIVE_INVESTING_DERIVED",
                "source": distributions["source"],
                "raw_probabilities": meeting["raw_probabilities"],
                "normalized_probabilities": meeting["normalized_probabilities"],
                "normalization": meeting["normalization"],
                "local_probabilities": [
                    {
                        "outcome_bp": row["local_bp_change"],
                        "probability_pct": row["probability_pct"],
                    }
                    for row in by_meeting.get(meeting_date, [])
                ],
                "meeting_ordinal": ordinal_by_meeting.get(meeting_date),
                "local_status": "OK",
                "source_timestamp": None,
                "freshness": {
                    "status": "SOURCE_TIMESTAMP_UNAVAILABLE",
                    "age_days": None,
                    "basis": "provider retrieved_at",
                },
                "timestamp_note": (
                    "Investing.com publishes no observation timestamp on the Fed Rate "
                    "Monitor page; the provider retrieved_at is the observation time."
                ),
            }
        )
    return sections


def without_local_probabilities(distributions: dict) -> list[dict]:
    """Per-meeting sections for the case where the current target range failed.

    The raw and normalized distributions are still truthful and useful; the
    local conversion is explicitly unavailable, never fabricated.
    """
    return [
        {
            "meeting_date": meeting["meeting_date"],
            "method": "LIVE_INVESTING_DERIVED",
            "source": distributions["source"],
            "raw_probabilities": meeting["raw_probabilities"],
            "normalized_probabilities": meeting["normalized_probabilities"],
            "normalization": meeting["normalization"],
            "local_probabilities": None,
            "meeting_ordinal": None,
            "local_status": "CURRENT_TARGET_RANGE_UNAVAILABLE",
            "source_timestamp": None,
            "freshness": {
                "status": "SOURCE_TIMESTAMP_UNAVAILABLE",
                "age_days": None,
                "basis": "provider retrieved_at",
            },
            "timestamp_note": (
                "Investing.com publishes no observation timestamp on the Fed Rate "
                "Monitor page; the provider retrieved_at is the observation time."
            ),
        }
        for meeting in distributions["meetings"]
    ]
