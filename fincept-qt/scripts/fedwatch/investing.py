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
  that meeting. Other cumulative distributions survive; a local change requires
  both adjacent expectations, so a missing predecessor is never skipped.
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

# One bucket row: interval label + displayed percentage.
_BUCKET_ITEM_RE = re.compile(
    r'<span[^>]*>([^<]+)</span>\s*(?:<i[^>]*></i>\s*)?'
    r'<div[^>]*></div>\s*<span[^>]*>([0-9.]+)%</span>'
)
_MEETING_TIME_FORMAT = "%b %d, %Y %I:%M%p ET"

# Structural markers used to detect rows the bucket regex did not match.
_DIV_CLASS_RE = re.compile(r'<div\b[^>]*\bclass\s*=\s*([\"\'])([^\"\']*)\1[^>]*>', re.I)

BP_STEP = 25
NORMALIZATION_MIN_SUM = 99.5
NORMALIZATION_MAX_SUM = 100.5
MAX_PROBABILITY_PCT = 100.0
MAX_PLAUSIBLE_RATE_HIGH = 10.0


def _class_fragments(html: str, class_name: str, *, bounded: bool = False) -> list[str]:
    markers = [match for match in _DIV_CLASS_RE.finditer(html) if class_name in match.group(2).split()]
    fragments = []
    for index, match in enumerate(markers):
        fragment = html[match.end():markers[index + 1].start() if index + 1 < len(markers) else len(html)]
        if bounded:
            # Bucket contents end at their own closing div, including nested
            # bar divs. The last bucket must not consume unrelated page markup.
            depth = 1
            closed = False
            for tag in re.finditer(r'</?div\b[^>]*>', fragment, re.I):
                depth += -1 if tag.group().startswith("</") else 1
                if depth == 0:
                    fragment = fragment[:tag.start()]
                    closed = True
                    break
            if not closed:
                fragment = ""
        fragments.append(fragment)
    return fragments


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


def parse_fed_rate_monitor(html: str) -> tuple[list[dict], list[str], dict]:
    """Parse the page's embedded Fed Rate Monitor table.

    Returns ``(rows, warnings, report)`` where each row is
    ``{meeting_date, rate_low, rate_high, probability_pct}`` and ``meeting_date``
    is an ISO date string. Rows that cannot be parsed are skipped with a
    warning, and the report records every structural loss (dropped meeting
    blocks, dropped bucket rows, and bucket markers that did not match the row
    structure). The provider excludes affected meetings, so a distribution is
    never normalized after source buckets disappeared. An empty result is reported as an empty list; the
    provider layer turns that into an explicit ``INVESTING_PARSE_EMPTY``
    failure.

    Deduplication matches the qualified behavior: the sidebar "Fed Rate Monitor
    Tool" repeats the nearest meeting, the main table appears first, so the
    first complete usable meeting block wins as a whole. A later intact copy
    may recover a broken main copy of that exact meeting, with diagnostics.
    Rows are then sorted by
    (meeting_date, rate_low); conflicting duplicates within that block reject
    the meeting instead of merging independent copies.
    """
    rows: list[dict] = []
    warnings: list[str] = []
    dropped_meeting_blocks: list[dict] = []
    dropped_bucket_rows: list[dict] = []
    partial_meeting_dates: set[str] = set()
    unmatched_bucket_items = 0
    recovered_meeting_dates: set[str] = set()
    chosen_blocks: dict[str, list[dict]] = {}
    complete_dates: set[str] = set()

    # Split BEFORE parsing: a missing Future Price in one block must not let a
    # regex consume the next meeting's buckets under the wrong date.
    fragments = _class_fragments(html, "infoFed")
    blocks = []
    for fragment in fragments:
        match = re.search(r'Meeting Time:</span>\s*<i>([^<]+)</i>', fragment)
        if match:
            blocks.append((match.group(1), None, fragment[match.end():]))
        else:
            dropped_meeting_blocks.append({"reason": "missing_meeting_time"})
    info_fed_marker_count = len(fragments)
    reported_meeting_dates: set[str] = set()
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
            dropped_meeting_blocks.append(
                {"meeting_time": meeting_time_raw, "reason": "unparseable_meeting_time"}
            )
            continue

        day = meeting_date.isoformat()
        reported_meeting_dates.add(day)
        if day in complete_dates:
            # Never blend independent copies or poison a usable earlier copy.
            continue
        bucket_fragments = _class_fragments(rest, "percfedRateItem", bounded=True)
        items = []
        block_partial = False
        for fragment in bucket_fragments:
            match = _BUCKET_ITEM_RE.fullmatch(fragment.strip())
            if match is None:
                unmatched_bucket_items += 1
                block_partial = True
                warnings.append(f"bucket marker for meeting {day} did not match the bucket row structure exactly once")
            else:
                items.append(match.groups())
        marker_count = len(bucket_fragments)
        if not items:
            warnings.append(
                f"no parseable bucket rows for meeting {meeting_date.isoformat()}; meeting skipped"
            )
            dropped_meeting_blocks.append(
                {"meeting_time": meeting_time_raw, "meeting_date": meeting_date.isoformat(),
                 "reason": "no_parseable_bucket_rows"}
            )
            partial_meeting_dates.add(meeting_date.isoformat())
            chosen_blocks.setdefault(day, [])
            continue

        block_rows = []
        for bucket_label, pct_raw in items:
            try:
                low_raw, high_raw = bucket_label.split("-")
                rate_low, rate_high = float(low_raw.strip()), float(high_raw.strip())
                probability_pct = float(pct_raw)
            except ValueError:
                warnings.append(
                    f"unparseable target-rate interval or percentage {bucket_label!r}/"
                    f"{pct_raw!r} for meeting {meeting_date.isoformat()}; row skipped"
                )
                dropped_bucket_rows.append(
                    {
                        "meeting_date": meeting_date.isoformat(),
                        "bucket": bucket_label,
                        "percentage": pct_raw,
                    }
                )
                block_partial = True
                continue

            block_rows.append(
                {
                    "meeting_date": meeting_date.isoformat(),
                    "rate_low": rate_low,
                    "rate_high": rate_high,
                    "probability_pct": probability_pct,
                }
            )

        unique = {}
        for row in block_rows:
            key = (row["rate_low"], row["rate_high"])
            if key in unique and unique[key]["probability_pct"] != row["probability_pct"]:
                block_partial = True
                warnings.append(f"conflicting duplicate bucket for meeting {day}")
            unique.setdefault(key, row)
        block_rows = list(unique.values())
        complete = not block_partial and bool(marker_count)
        if complete:
            try:
                _normalize_complete_meetings(block_rows)
            except InvestingDistributionError:
                complete = False
        if complete:
            if day in chosen_blocks:
                recovered_meeting_dates.add(day)
                warnings.append(f"meeting {day} recovered from an intact later copy; whole distribution replaced")
            chosen_blocks[day] = block_rows
            complete_dates.add(day)
            partial_meeting_dates.discard(day)
        else:
            chosen_blocks.setdefault(day, block_rows)
            if block_partial:
                partial_meeting_dates.add(day)

    rows = [row for block_rows in chosen_blocks.values() for row in block_rows]

    deduplicated: list[dict] = []
    seen: set[tuple] = set()
    first_by_key = {}
    for row in rows:
        key = (row["meeting_date"], row["rate_low"], row["rate_high"])
        if key in seen:
            if first_by_key[key]["probability_pct"] != row["probability_pct"]:
                partial_meeting_dates.add(row["meeting_date"])
                warnings.append(f"conflicting duplicate bucket for meeting {row['meeting_date']}")
            continue
        seen.add(key)
        first_by_key[key] = row
        deduplicated.append(row)
    deduplicated.sort(key=lambda row: (row["meeting_date"], row["rate_low"]))

    report = {
        "meeting_block_count": len(blocks),
        "info_fed_marker_count": info_fed_marker_count,
        "unmatched_bucket_item_count": unmatched_bucket_items,
        "dropped_bucket_row_count": len(dropped_bucket_rows),
        "dropped_bucket_rows": dropped_bucket_rows[:10],
        "dropped_meeting_blocks": dropped_meeting_blocks[:10],
        "partial_meeting_dates": sorted(partial_meeting_dates),
        "recovered_meeting_dates": sorted(recovered_meeting_dates),
        "reported_meeting_dates": sorted(reported_meeting_dates),
        "unknown_meeting_identity": any("meeting_date" not in block for block in dropped_meeting_blocks),
        "structurally_complete": bool(
            blocks
            and info_fed_marker_count == len(blocks)
            and not dropped_meeting_blocks
            and not dropped_bucket_rows
            and unmatched_bucket_items == 0
            and not partial_meeting_dates
        ),
    }
    return deduplicated, warnings, report


def normalize_cumulative(rows: list[dict], rejected: list | None = None) -> tuple[list[dict], list[dict]]:
    """Keep independently valid meetings; report invalid meetings explicitly.

    Returns accepted rows and successful normalization records only. Supplying
    ``rejected`` explicitly enables partial acceptance and receives error
    records separately. Without it, any rejected meeting raises. No usable
    meetings remains a provider failure in either mode.
    """
    if not rows:
        raise InvestingDistributionError("INVESTING_DISTRIBUTION_INVALID", "Investing cumulative table is empty")
    accepted, records, failures = [], [], []
    for day in sorted({row["meeting_date"] for row in rows}):
        try:
            values, details = _normalize_complete_meetings([r for r in rows if r["meeting_date"] == day])
            accepted.extend(values)
            records.extend(details)
        except InvestingDistributionError as exc:
            failures.append(exc)
    if rejected is not None:
        rejected.extend(exc.to_dict() for exc in failures)
    elif failures:
        raise failures[0]
    if not accepted:
        raise failures[0]
    return accepted, records


def _normalize_complete_meetings(rows: list[dict]) -> tuple[list[dict], list[dict]]:
    """Validate and normalize each Investing cumulative meeting independently.

    Returns ``(normalized_rows, records)``. The input is never mutated. The
    correction is the smallest possible: every bucket in a meeting is scaled by
    the same factor ``100 / raw_sum``. A meeting whose rate bounds or
    probabilities are malformed (non-finite, negative ranges, unordered or
    implausible ranges, probabilities outside [0, 100]) or whose raw sum falls
    outside [99.5, 100.5] raises :class:`InvestingDistributionError`
    (a ``ValueError``).
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
            rate_lows = [float(row["rate_low"]) for row in group]
            rate_highs = [float(row["rate_high"]) for row in group]
        except (KeyError, TypeError, ValueError, OverflowError) as exc:
            raise InvestingDistributionError(
                "INVESTING_DISTRIBUTION_INVALID",
                f"invalid Investing rate range or probability for meeting {meeting_date}",
                detail={"meeting_date": meeting_date},
            ) from exc

        # The previously qualified input-quality boundary: every rate bound and
        # probability must be finite, target ranges non-negative, ordered,
        # within the plausible bound the qualification used, and probabilities
        # inside [0, 100] before any expected-rate or local-step conversion.
        invalid_ranges = any(
            not math.isfinite(low)
            or not math.isfinite(high)
            or low < 0
            or high <= low
            or high > MAX_PLAUSIBLE_RATE_HIGH
            for low, high in zip(rate_lows, rate_highs)
        )
        invalid_probabilities = any(
            not math.isfinite(value) or value < 0 or value > MAX_PROBABILITY_PCT
            for value in values
        )
        bands = sorted(zip(rate_lows, rate_highs))
        overlap = any(high > next_low for (_, high), (next_low, _) in zip(bands, bands[1:]))
        if invalid_ranges or invalid_probabilities or overlap:
            raise InvestingDistributionError(
                "INVESTING_DISTRIBUTION_INVALID",
                f"invalid Investing rate range or probability for meeting {meeting_date}",
                detail={"meeting_date": meeting_date},
            )

        midpoints = [
            (low + high) / 2.0 for low, high in zip(rate_lows, rate_highs)
        ]

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
        rows.extend({"meeting_date": meeting_date, "meeting_ordinal": ordinal,
                     "local_bp_change": row["outcome_bp"], "probability_pct": row["probability_pct"]}
                    for row in _local_probabilities(expected_rate, previous_expected))
        previous_expected = expected_rate

    return rows


def _local_probabilities(expected_rate: float, previous_expected: float) -> list[dict]:
    """Shared qualified integer/mantissa conversion and six-decimal boundary."""
    change = (expected_rate - previous_expected) / BP_STEP * 100.0
    return [{"outcome_bp": bp, "probability_pct": round(probability * 100.0, 6)}
            for bp, probability in sorted(local_step_distribution(change).items())]


def fetch_distributions(transport: Transport, clock=timeutil.utc_now) -> dict:
    """Retrieve, parse, validate and normalize the current Investing table.

    Returns a JSON-ready provider payload with both the raw displayed values and
    the validated/normalized values kept distinct, plus the per-meeting
    normalization records. Raises :class:`FedwatchError` (provider
    ``investing``) when no meeting is usable. Partial responses preserve
    complete independently normalized meetings and explicit rejection errors.
    """
    html = fetch_fed_rate_monitor_html(transport)
    retrieved_at = clock()
    raw_rows, warnings, parse_report = parse_fed_rate_monitor(html)
    if not raw_rows:
        raise FedwatchError(
            PROVIDER_INVESTING,
            "INVESTING_PARSE_EMPTY",
            "Investing.com Fed Rate Monitor returned no parseable meeting rows",
            detail={
                "retrieved_at": timeutil.iso_z(retrieved_at),
                "parse_report": parse_report,
            },
        )
    rejected = []
    normalized_rows, records = [], []
    for day in sorted({row["meeting_date"] for row in raw_rows}):
        group = [row for row in raw_rows if row["meeting_date"] == day]
        if day in parse_report["partial_meeting_dates"]:
            rejected.append(FedwatchError(PROVIDER_INVESTING, "INVESTING_PARSE_PARTIAL",
                                         f"incomplete Investing distribution for meeting {day}",
                                         detail={"meeting_date": day, "parse_report": parse_report}).to_dict())
            continue
        try:
            normalized, meeting_records = normalize_cumulative(group)
        except InvestingDistributionError as exc:
            rejected.append(exc.to_dict())
            continue
        normalized_rows.extend(normalized)
        records.extend(meeting_records)
    for day in set(parse_report["partial_meeting_dates"]) - {r["meeting_date"] for r in raw_rows}:
        rejected.append(FedwatchError(PROVIDER_INVESTING, "INVESTING_PARSE_PARTIAL",
                                     f"no usable buckets for meeting {day}", detail={"meeting_date": day}).to_dict())
    if parse_report["unknown_meeting_identity"]:
        rejected.append(FedwatchError(PROVIDER_INVESTING, "INVESTING_PARSE_PARTIAL",
                                     "Investing meeting block identity could not be parsed",
                                     detail={"parse_report": parse_report}).to_dict())
    if not records:
        error = rejected[0]
        raise InvestingDistributionError(error["code"], error["error"], detail=error.get("detail"))
    record_by_date = {record["meeting_date"]: record for record in records}

    raw_by_date: dict[str, list[dict]] = {}
    for row in raw_rows:
        raw_by_date.setdefault(row["meeting_date"], []).append(row)

    meetings = []
    for meeting_date in sorted(record_by_date):
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
        "parse_report": parse_report,
        "errors": rejected,
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


def with_local_probabilities(distributions: dict, upper: float, lower: float,
                             meeting_dates: list[str] | None = None) -> list[dict]:
    """Merge the local-step conversion into the per-meeting fed-side sections.

    Returns a new list of meeting sections with ``local_probabilities``,
    ``meeting_ordinal`` and ``local_status: "OK"``. The normalized table is
    converted only when its adjacent predecessor is available. The official
    complete schedule, when supplied, establishes adjacency across parse gaps.
    """
    report = distributions.get("parse_report") or {}
    sequence = sorted(set(meeting_dates if meeting_dates is not None else
                          report.get("reported_meeting_dates", [m["meeting_date"] for m in distributions["meetings"]])))
    ordinal_by_meeting = {day: index + 1 for index, day in enumerate(sequence)}
    expected = {m["meeting_date"]: m["normalization"]["normalized_expected_rate"]
                for m in distributions["meetings"]}
    local_by_date = {}
    for index, day in enumerate(sequence):
        previous = sequence[index - 1] if index else None
        # An unknown block leaves adjacency uncertain unless an official full
        # schedule supplies the sequence. Resume after a known rejected meeting
        # only when both adjacent cumulative expectations are available.
        if day not in expected or (previous is not None and previous not in expected):
            continue
        if meeting_dates is None and report.get("unknown_meeting_identity"):
            continue
        prior = expected[previous] if previous else (upper + lower) / 2
        local_by_date[day] = _local_probabilities(expected[day], prior)
        ordinal_by_meeting[day] = index + 1

    sections = []
    for meeting in distributions["meetings"]:
        meeting_date = meeting["meeting_date"]
        mismatch = meeting_dates is not None and meeting_date not in ordinal_by_meeting
        sections.append(
            {
                "meeting_date": meeting_date,
                "method": "LIVE_INVESTING_DERIVED",
                "source": distributions["source"],
                "raw_probabilities": meeting["raw_probabilities"],
                "normalized_probabilities": meeting["normalized_probabilities"],
                "normalization": meeting["normalization"],
                "local_probabilities": local_by_date.get(meeting_date),
                "meeting_ordinal": ordinal_by_meeting.get(meeting_date),
                "local_status": "OK" if meeting_date in local_by_date else
                                "MEETING_DATE_MISMATCH" if mismatch else "PREVIOUS_MEETING_UNAVAILABLE",
                "local_error": None if meeting_date in local_by_date else FedwatchError(
                    PROVIDER_INVESTING, "INVESTING_MEETING_DATE_MISMATCH" if mismatch else
                    "INVESTING_LOCAL_DEPENDENCY_UNAVAILABLE",
                    f"meeting {meeting_date} is absent from the supplied official schedule" if mismatch else
                    f"local change for meeting {meeting_date} requires an established adjacent predecessor",
                    detail={"meeting_date": meeting_date}).to_dict(),
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
