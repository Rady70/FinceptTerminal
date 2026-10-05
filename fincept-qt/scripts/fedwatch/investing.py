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
from datetime import datetime, timedelta, timezone

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
    r'<span[^>]*>([^<]+)</span>\s*(?:<i[^>]*>\s*</i>\s*)?'
    r'<div[^>]*>\s*</div>\s*<span[^>]*>([0-9.]+)%</span>'
)
_MEETING_TIME_FORMAT = "%b %d, %Y %I:%M%p ET"

# Structural markers used to detect rows the bucket regex did not match.
_DIV_CLASS_RE = re.compile(r'<div\b[^>]*\bclass\s*=\s*([\"\'])([^\"\']*)\1[^>]*>', re.I)

BP_STEP = 25
NORMALIZATION_MIN_SUM = 99.5
NORMALIZATION_MAX_SUM = 100.5
MAX_PROBABILITY_PCT = 100.0
MAX_PLAUSIBLE_RATE_HIGH = 10.0

# Each meeting card also publishes context the distribution parser does not
# need: an "Updated: <Eastern time>" stamp, the displayed 30-Day Fed Funds
# futures price, and a table repeating the current probabilities beside the
# Previous Day / Previous Week values. They are parsed separately so the
# qualified distribution rows and normalization stay byte-for-byte unchanged.
_FUTURE_PRICE_RE = re.compile(r'Future Price:\s*</span>\s*<i\b[^>]*>\s*([^<]*?)\s*</i>', re.I)
_UPDATED_RE = re.compile(r'class\s*=\s*["\'][^"\']*\bfedUpdate\b[^"\']*["\'][^>]*>\s*Updated:\s*([^<]+?)\s*<', re.I)
_CONTEXT_TABLE_RE = re.compile(r'<table\b[^>]*\bfedRateTbl\b[^>]*>(.*?)</table>', re.I | re.S)
_TABLE_ROW_RE = re.compile(r'<tr\b[^>]*>(.*?)</tr>', re.I | re.S)
_TABLE_CELL_RE = re.compile(r'<td\b[^>]*>(.*?)</td>', re.I | re.S)
_TAG_RE = re.compile(r'<[^>]+>')
_UPDATED_FORMAT = "%b %d, %Y %I:%M%p"
_EASTERN_ABBREVIATIONS = {"EDT": -4, "EST": -5}
# Futures do not trade at weekends, so the source is stale only after more
# than this many U.S. weekdays pass without an update.
SOURCE_STALE_AFTER_WEEKDAYS = 2
# Bars and table both show one decimal; this only absorbs display rounding.
PREVIOUS_TABLE_TOLERANCE_PP = 0.15
SOURCE_FUTURE_TOLERANCE_SECONDS = 600


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


_MONTHS = {name: index for index, names in enumerate(
    (("jan", "january"), ("feb", "february"), ("mar", "march"), ("apr", "april"), ("may",), ("jun", "june"),
     ("jul", "july"), ("aug", "august"), ("sep", "sept", "september"), ("oct", "october"), ("nov", "november"),
     ("dec", "december")), start=1) for name in names}
_TOLERANT_DATE_RE = re.compile(r'([A-Za-z]{3,9})\.?\s+(\d{1,2}),?\s+(\d{4})')
_TOLERANT_RANGE_RE = re.compile(r'(\d+(?:\.\d+)?)\s*%?\s*[-\u2013\u2014]\s*(\d+(?:\.\d+)?)\s*%?(?=\s|$)')
_TOLERANT_PERCENT_RE = re.compile(r'(\d+(?:\.\d+)?)\s*%')


def _text(fragment: str) -> str:
    """Visible text with tags replaced by spaces (attribute values are dropped)."""
    return re.sub(r'\s+', ' ', _TAG_RE.sub(" ", fragment).replace("&nbsp;", " ")).strip()


def _tolerant_date(text: str):
    """First ``Month D, YYYY`` in visible text, accepting short or full month names."""
    match = _TOLERANT_DATE_RE.search(text or "")
    if not match:
        return None
    month = _MONTHS.get(match.group(1).lower())
    try:
        return datetime(int(match.group(3)), month, int(match.group(2))).date() if month else None
    except ValueError:
        return None


def _meeting_time(fragment: str):
    """``(raw_text, rest)`` for a card's meeting time, tolerating markup changes.

    The qualified exact markup is tried first; otherwise the visible text after
    the "Meeting Time" caption is used. ``rest`` starts after the caption, so a
    card's buckets can never be read from before its own date.
    """
    match = re.search(r'Meeting Time:\s*</span>\s*<i\b[^>]*>([^<]+)</i>', fragment)
    if match:
        return match.group(1), fragment[match.end():]
    caption = re.search(r'Meeting\s+(?:Time|Date)\s*:?', fragment, re.I)
    if not caption:
        return None, None
    head = _text(fragment[caption.end():caption.end() + 400])
    found = _TOLERANT_DATE_RE.search(head)
    return (found.group(0) if found else head[:40]), fragment[caption.end():]


def _meeting_date(raw: str):
    try:
        return datetime.strptime(raw.strip(), _MEETING_TIME_FORMAT).date()
    except ValueError:
        return _tolerant_date(raw)


def _bucket_item(fragment: str):
    """``(label, percent)`` from one bucket row, read by meaning if the markup changed."""
    matches = list(_BUCKET_ITEM_RE.finditer(fragment))
    if len(matches) == 1:
        return matches[0].groups()
    text = _text(fragment)
    ranges = _TOLERANT_RANGE_RE.findall(text)
    percents = _TOLERANT_PERCENT_RE.findall(_TOLERANT_RANGE_RE.sub(" ", text))
    if len(ranges) == 1 and len(percents) == 1:
        return f"{ranges[0][0]} - {ranges[0][1]}", percents[0]
    return None


def _parse_updated(text: str):
    """``"Oct 05, 2026 07:35AM EDT"`` -> UTC instant, or None when unrecognized."""
    parts = text.strip().rsplit(" ", 1)
    if len(parts) != 2:
        return None
    wall = None
    for layout in (_UPDATED_FORMAT, "%B %d, %Y %I:%M%p", "%b %d, %Y %I:%M %p", "%B %d, %Y %I:%M %p",
                   "%b %d, %Y %H:%M", "%B %d, %Y %H:%M"):
        try:
            wall = datetime.strptime(parts[0].strip(), layout)
            break
        except ValueError:
            continue
    if wall is None:
        return None
    zone = parts[1].strip().upper()
    if zone in _EASTERN_ABBREVIATIONS:
        offset = _EASTERN_ABBREVIATIONS[zone]
    elif zone == "ET":
        offset = -4 if timeutil.is_us_dst_us_date(wall.date()) else -5
    else:
        return None
    return (wall - timedelta(hours=offset)).replace(tzinfo=timezone.utc)


_NOT_LISTED = {"&mdash;", "&ndash;", "—", "–", "-", "--"}


def _parse_percent(cell: str):
    """``(value, kind)``: kind is VALUE, NOT_LISTED (the page's dash) or INVALID."""
    text = _TAG_RE.sub(" ", cell).replace("&nbsp;", " ").strip()
    if text in _NOT_LISTED:
        return None, "NOT_LISTED"
    match = re.fullmatch(r'([0-9]+(?:\.[0-9]+)?)\s*%', text)
    if not match:
        return None, "INVALID"
    value = float(match.group(1))
    if not math.isfinite(value) or not 0 <= value <= MAX_PROBABILITY_PCT:
        return None, "INVALID"
    return value, "VALUE"


def parse_displayed_context(html: str) -> dict:
    """Per-meeting page context that accompanies each distribution.

    Returns ``{meeting_date: {source_updated_at, source_updated_text,
    futures_price, table}}`` using the first card for each meeting that carries
    the table. Unreadable values stay ``None``; nothing is inferred.
    """
    result: dict[str, dict] = {}
    for fragment in _class_fragments(html, "infoFed"):
        raw_time, _ = _meeting_time(fragment)
        meeting_day = _meeting_date(raw_time) if raw_time is not None else None
        if meeting_day is None:
            continue
        day = meeting_day.isoformat()
        table_match = _CONTEXT_TABLE_RE.search(fragment)
        if day in result and (result[day]["table"] or not table_match):
            continue
        price = None
        price_match = _FUTURE_PRICE_RE.search(fragment)
        if price_match:
            try:
                value = float(price_match.group(1).replace(",", ""))
                price = value if math.isfinite(value) and 0 < value <= 100 else None
            except ValueError:
                price = None
        updated_match = _UPDATED_RE.search(fragment)
        updated_text = updated_match.group(1).strip() if updated_match else None
        table = []
        if table_match:
            headers = [_text(cell).lower() for cell in re.findall(r'<th\b[^>]*>(.*?)</th>', table_match.group(1),
                                                                   re.I | re.S)]

            def column(word, fallback):
                found = [i for i, h in enumerate(headers) if word in h]
                return found[0] if len(found) == 1 else fallback

            columns = (column("current", 1), column("day", 2), column("week", 3))
            for row in _TABLE_ROW_RE.findall(table_match.group(1)):
                cells = _TABLE_CELL_RE.findall(row)
                if len(cells) < 2:
                    continue
                found = _TOLERANT_RANGE_RE.search(_text(cells[0]))
                if not found:
                    continue
                parsed = [_parse_percent(cells[i]) if i < len(cells) else (None, "NOT_LISTED") for i in columns]
                table.append({"rate_low": float(found.group(1)), "rate_high": float(found.group(2)),
                              "current_pct": parsed[0][0], "previous_day_pct": parsed[1][0],
                              "previous_week_pct": parsed[2][0], "current_invalid": parsed[0][1] == "INVALID",
                              "invalid_cells": sum(kind == "INVALID" for _, kind in parsed)})
        result[day] = {"source_updated_text": updated_text,
                       "source_updated_at": _parse_updated(updated_text) if updated_text else None,
                       "futures_price": price, "table": table}
    return result


def source_freshness(updated_at, now) -> dict:
    """Freshness from the page's own update time, counted in U.S. weekdays."""
    if updated_at is None:
        return {"status": "SOURCE_TIMESTAMP_UNAVAILABLE", "age_days": None, "basis": "provider retrieved_at"}
    age = (now - updated_at).total_seconds()
    if age < -SOURCE_FUTURE_TOLERANCE_SECONDS:
        return {"status": "SOURCE_TIMESTAMP_UNAVAILABLE", "age_days": None, "basis": "provider retrieved_at",
                "rejected_source_timestamp": timeutil.iso_z(updated_at), "reason": "FUTURE_SOURCE_TIMESTAMP"}
    start, end = timeutil.eastern_date_from_utc(updated_at), timeutil.eastern_date_from_utc(now)
    weekdays = sum(1 for offset in range(1, min((end - start).days, 400) + 1)
                   if (start + timedelta(days=offset)).weekday() < 5)
    return {"status": "CURRENT" if weekdays <= SOURCE_STALE_AFTER_WEEKDAYS else "STALE",
            "age_days": round(max(age, 0) / 86400.0, 6), "weekdays_since_update": weekdays,
            "basis": "Investing.com Updated time"}


def _displayed_previous(context: dict | None, raw_rows: list[dict]) -> dict:
    """Previous Day/Week values, used only when the table repeats the accepted bars.

    The table is an independent copy of the same distribution. Its current
    column must equal every accepted raw bucket (and nothing else) before its
    previous columns are trusted; otherwise they are withheld as a mismatch.
    """
    table = (context or {}).get("table") or []
    if not table:
        return {"status": "UNAVAILABLE", "reason": "TABLE_ABSENT"}
    accepted = {(row["rate_low"], row["rate_high"]): row["probability_pct"] for row in raw_rows}
    repeated = {(row["rate_low"], row["rate_high"]): row["current_pct"] for row in table}
    # The table only has to agree where both show a value: it proves the rows
    # and columns line up. Buckets absent from one side are not a conflict; a
    # clear disagreement (beyond display rounding) or an unreadable current
    # column means the previous values cannot be tied to this distribution.
    overlap = [key for key in accepted if repeated.get(key) is not None]
    disagree = [key for key in overlap if abs(repeated[key] - accepted[key]) > PREVIOUS_TABLE_TOLERANCE_PP]
    extra = [key for key in set(repeated) - set(accepted) if (repeated[key] or 0.0) > PREVIOUS_TABLE_TOLERANCE_PP]
    if len(repeated) != len(table) or not overlap or disagree or extra:
        return {"status": "MISMATCH", "reason": "TABLE_CURRENT_DIFFERS_FROM_DISTRIBUTION",
                "disagreeing_buckets": [list(key) for key in disagree + extra]}
    unreadable = sum(row.get("invalid_cells", 0) for row in table)
    out = {"status": "PARTIAL" if unreadable or len(overlap) < len(accepted) else "OK",
           "unreadable_cells": unreadable}
    for horizon in ("previous_day", "previous_week"):
        # A dash means the bucket was not listed for that horizon.
        values = [{"rate_low": row["rate_low"], "rate_high": row["rate_high"],
                   "probability_pct": row[horizon + "_pct"]} for row in sorted(table, key=lambda r: r["rate_low"])
                  if row[horizon + "_pct"] is not None]
        total = sum(item["probability_pct"] for item in values) if values else None
        complete = total is not None and NORMALIZATION_MIN_SUM <= total <= NORMALIZATION_MAX_SUM
        expected = (sum((item["rate_low"] + item["rate_high"]) / 2.0 * item["probability_pct"] / total
                        for item in values) if complete else None)
        out[horizon] = {"probabilities": values, "complete": complete,
                        "raw_probability_sum_pct": round(total, 9) if total is not None else None,
                        "normalized_expected_rate": expected}
    return out


def _table_distribution(context: dict | None, day: str) -> list[dict] | None:
    """The card table's current column as raw rows, when it is fully readable."""
    table = (context or {}).get("table") or []
    if not table or any(row.get("current_invalid") for row in table):
        return None
    rows = [{"meeting_date": day, "rate_low": row["rate_low"], "rate_high": row["rate_high"],
             "probability_pct": row["current_pct"]} for row in table if row["current_pct"]]
    total = sum(row["probability_pct"] for row in rows)
    return rows if rows and NORMALIZATION_MIN_SUM <= total <= NORMALIZATION_MAX_SUM else None


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
    may recover a broken main copy. Readable disagreements with every earlier
    broken copy are retained as meeting-scoped quality labels and diagnostics.
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
    parsed_buckets: dict[str, dict[tuple, set[float]]] = {}
    conflicting_copy_dates: set[str] = set()
    copy_conflicts: list[dict] = []

    # Split BEFORE parsing: a missing Future Price in one block must not let a
    # regex consume the next meeting's buckets under the wrong date.
    fragments = _class_fragments(html, "infoFed")
    blocks = []
    for fragment in fragments:
        raw_time, rest = _meeting_time(fragment)
        if raw_time is not None:
            blocks.append((raw_time, None, rest))
        else:
            dropped_meeting_blocks.append({"reason": "missing_meeting_time"})
    info_fed_marker_count = len(fragments)
    reported_meeting_dates: set[str] = set()
    if not blocks:
        warnings.append(
            "no meeting blocks found in the Investing.com page; the HTML structure may have changed"
        )

    for meeting_time_raw, _future_price_raw, rest in blocks:
        meeting_date = _meeting_date(meeting_time_raw)
        if meeting_date is None:
            warnings.append(f"unparseable meeting time {meeting_time_raw!r}; block skipped")
            dropped_meeting_blocks.append(
                {"meeting_time": meeting_time_raw, "reason": "unparseable_meeting_time"}
            )
            continue

        day = meeting_date.isoformat()
        reported_meeting_dates.add(day)
        already_complete = day in complete_dates
        bucket_fragments = _class_fragments(rest, "percfedRateItem", bounded=True)
        items = []
        block_partial = False
        for fragment in bucket_fragments:
            item = _bucket_item(fragment)
            if item is None:
                unmatched_bucket_items += 1
                block_partial = True
                warnings.append(f"bucket marker for meeting {day} did not contain exactly one range and percentage")
            else:
                items.append(item)
        marker_count = len(bucket_fragments)
        if not items:
            warnings.append(
                f"no parseable bucket rows for meeting {meeting_date.isoformat()}; meeting skipped"
            )
            dropped_meeting_blocks.append(
                {"meeting_time": meeting_time_raw, "meeting_date": meeting_date.isoformat(),
                 "reason": "no_parseable_bucket_rows"}
            )
            if already_complete:
                continue
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
        constraints = parsed_buckets.setdefault(day, {})
        for row in block_rows:
            key = (row["rate_low"], row["rate_high"])
            if key in unique and unique[key]["probability_pct"] != row["probability_pct"]:
                block_partial = True
                warnings.append(f"conflicting duplicate bucket for meeting {day}")
            for probability in sorted(constraints.get(key, set())):
                if probability != row["probability_pct"]:
                    conflicting_copy_dates.add(day)
                    copy_conflicts.append({"meeting_date": day, "rate_low": key[0], "rate_high": key[1],
                                           "earlier_probability_pct": probability,
                                           "copy_probability_pct": row["probability_pct"],
                                           "reason": "CONTRADICTORY_PARSED_BUCKET"})
            unique.setdefault(key, row)
        block_rows = list(unique.values())
        complete = not block_partial and bool(marker_count)
        if complete:
            try:
                _normalize_complete_meetings(block_rows)
            except InvestingDistributionError:
                complete = False
        if complete:
            for key, probabilities in constraints.items():
                if key not in unique:
                    conflicting_copy_dates.add(day)
                    for probability in sorted(probabilities):
                        copy_conflicts.append({"meeting_date": day, "rate_low": key[0], "rate_high": key[1],
                                               "earlier_probability_pct": probability,
                                               "copy_probability_pct": None,
                                               "reason": "MISSING_PREVIOUSLY_PARSED_BUCKET"})
            if already_complete:
                for key in set(unique) - set(constraints):
                    conflicting_copy_dates.add(day)
                    copy_conflicts.append({"meeting_date": day, "rate_low": key[0], "rate_high": key[1],
                                           "earlier_probability_pct": None,
                                           "copy_probability_pct": unique[key]["probability_pct"],
                                           "reason": "EXTRA_COPY_BUCKET"})
        # Compare only against earlier copies; conflicting duplicates within
        # this one copy are a separate structural rejection, never copy labels.
        for row in block_rows:
            constraints.setdefault((row["rate_low"], row["rate_high"]), set()).add(row["probability_pct"])
        if day in conflicting_copy_dates:
            warnings.append(f"readable copy disagreement retained for meeting {day}")
        if already_complete:
            # Parse for diagnostics, but never replace/blend the first usable copy.
            continue
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
        "conflicting_copy_meeting_dates": sorted(conflicting_copy_dates),
        "copy_conflicts": copy_conflicts,
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
    contexts = parse_displayed_context(html)
    # A card whose bars could not be read completely still publishes the same
    # distribution in its table; use it instead of dropping the meeting. A
    # contradiction between page copies is never resolved this way.
    recovered = {}
    for day in sorted(set(parse_report["partial_meeting_dates"]) - set(parse_report["conflicting_copy_meeting_dates"])):
        rows = _table_distribution(contexts.get(day), day)
        if rows:
            recovered[day] = rows
    if recovered:
        raw_rows = sorted([row for row in raw_rows if row["meeting_date"] not in recovered] +
                          [row for rows in recovered.values() for row in rows],
                          key=lambda row: (row["meeting_date"], row["rate_low"]))
        parse_report = dict(parse_report, partial_meeting_dates=sorted(
            set(parse_report["partial_meeting_dates"]) - set(recovered)),
            table_recovered_meeting_dates=sorted(recovered))
        warnings.append("meeting card bars unreadable; distribution read from the card's table for "
                        + ", ".join(sorted(recovered)))
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
            conflict = day in parse_report["conflicting_copy_meeting_dates"]
            rejected.append(FedwatchError(PROVIDER_INVESTING,
                                         "INVESTING_COPY_CONFLICT" if conflict else "INVESTING_PARSE_PARTIAL",
                                         f"contradictory Investing copies for meeting {day}" if conflict else
                                         f"incomplete Investing distribution for meeting {day}",
                                         detail={"meeting_date": day, "parse_report": parse_report,
                                                 "parsed_probabilities": group}).to_dict())
            continue
        try:
            normalized, meeting_records = normalize_cumulative(group)
        except InvestingDistributionError as exc:
            error = exc.to_dict()
            error.setdefault("detail", {})["parsed_probabilities"] = group
            rejected.append(error)
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
    for day in sorted(set(record_by_date) & set(parse_report["conflicting_copy_meeting_dates"])):
        rejected.append(FedwatchError(PROVIDER_INVESTING, "INVESTING_COPY_CONFLICT",
            "first intact same-meeting distribution retained despite readable disagreement between copies",
            detail={"meeting_date": day, "copy_conflicts": [item for item in parse_report["copy_conflicts"]
                    if item["meeting_date"] == day]}).to_dict())

    raw_by_date: dict[str, list[dict]] = {}
    for row in raw_rows:
        raw_by_date.setdefault(row["meeting_date"], []).append(row)

    meetings = []
    for meeting_date in sorted(record_by_date):
        record = record_by_date[meeting_date]
        context = contexts.get(meeting_date) or {}
        updated_at = context.get("source_updated_at")
        freshness = source_freshness(updated_at, retrieved_at)
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
                "copy_conflict": meeting_date in parse_report["conflicting_copy_meeting_dates"],
                "copy_conflicts": [item for item in parse_report["copy_conflicts"] if item["meeting_date"] == meeting_date],
                "source_timestamp": timeutil.iso_z(updated_at) if updated_at and freshness["status"] != "SOURCE_TIMESTAMP_UNAVAILABLE" else None,
                "freshness": freshness,
                "displayed_context": {
                    "source_updated_text": context.get("source_updated_text"),
                    "futures_price": context.get("futures_price"),
                    "futures_implied_rate": (round(100.0 - context["futures_price"], 6)
                                             if context.get("futures_price") is not None else None),
                    "previous": _displayed_previous(context, raw_by_date[meeting_date]),
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


def _timing(meeting: dict) -> dict:
    """Source time, freshness and page context for one Fed-side section."""
    timestamp = meeting.get("source_timestamp")
    return {
        "source_timestamp": timestamp,
        "freshness": meeting.get("freshness") or {
            "status": "SOURCE_TIMESTAMP_UNAVAILABLE", "age_days": None, "basis": "provider retrieved_at"},
        "displayed_context": meeting.get("displayed_context"),
        "timestamp_note": (
            "The Fed Rate Monitor card's 'Updated' time (U.S. Eastern, converted to UTC) is the "
            "source observation time; the provider retrieved_at is when MarketLab read it."
            if timestamp else
            "No readable 'Updated' time was found for this meeting card; the provider "
            "retrieved_at is the observation time."
        ),
    }


def with_local_probabilities(distributions: dict, upper: float | None, lower: float | None,
                             meeting_dates: list[str] | None = None,
                             first_error: dict | None = None,
                             unverified_pair_date: str | None = None) -> list[dict]:
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
        if previous is None and (upper is None or lower is None or first_error):
            continue
        prior = expected[previous] if previous else (upper + lower) / 2
        local_by_date[day] = _local_probabilities(expected[day], prior)
        ordinal_by_meeting[day] = index + 1

    sections = []
    for meeting in distributions["meetings"]:
        meeting_date = meeting["meeting_date"]
        mismatch = meeting_dates is not None and meeting_date not in ordinal_by_meeting
        first = ordinal_by_meeting.get(meeting_date) == 1
        unavailable_target = first and (upper is None or lower is None or first_error)
        sections.append(
            {
                "meeting_date": meeting_date,
                "method": "LIVE_INVESTING_DERIVED",
                "source": distributions["source"],
                "raw_probabilities": meeting["raw_probabilities"],
                "normalized_probabilities": meeting["normalized_probabilities"],
                "normalization": meeting["normalization"],
                "copy_conflict": meeting.get("copy_conflict", False),
                "copy_conflicts": meeting.get("copy_conflicts", []),
                "target_range_unverified": bool(first and unverified_pair_date and meeting_date in local_by_date),
                "target_range_pair_date": unverified_pair_date if first and meeting_date in local_by_date else None,
                "local_probabilities": local_by_date.get(meeting_date),
                "meeting_ordinal": ordinal_by_meeting.get(meeting_date),
                "local_status": "OK" if meeting_date in local_by_date else
                                "MEETING_DATE_MISMATCH" if mismatch else
                                first_error["code"] if unavailable_target and first_error else
                                "CURRENT_TARGET_RANGE_UNAVAILABLE" if unavailable_target else "PREVIOUS_MEETING_UNAVAILABLE",
                "local_error": None if meeting_date in local_by_date else first_error if unavailable_target and first_error else FedwatchError(
                    "fred" if unavailable_target else PROVIDER_INVESTING, "INVESTING_MEETING_DATE_MISMATCH" if mismatch else
                    "CURRENT_TARGET_RANGE_UNAVAILABLE" if unavailable_target else "INVESTING_LOCAL_DEPENDENCY_UNAVAILABLE",
                    f"meeting {meeting_date} is absent from the supplied official schedule" if mismatch else
                    f"first meeting {meeting_date} requires a readable FRED target pair" if unavailable_target else
                    f"local change for meeting {meeting_date} requires an established adjacent predecessor",
                    detail={"meeting_date": meeting_date}).to_dict(),
                **_timing(meeting),
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
            "copy_conflict": meeting.get("copy_conflict", False),
            "copy_conflicts": meeting.get("copy_conflicts", []),
            "local_probabilities": None,
            "meeting_ordinal": None,
            "local_status": "CURRENT_TARGET_RANGE_UNAVAILABLE",
            **_timing(meeting),
        }
        for meeting in distributions["meetings"]
    ]
