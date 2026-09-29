"""Historical FedWatch analytics computed from durable stored observations.

Batch B must let Batch C consume the approved historical calculations without
inventing financial semantics in the UI layer (FEDWATCH_INTEGRATION_PLAN.md
section 11). Every value here is computed on demand from stored observation
episodes; nothing derived is persisted.

Guarantees:

* the actual stored chronology is used — no interpolation and no forward-fill;
* a lookback (1/7/30-day) change is only produced when an accepted observation
  exists at or before the lookback instant, and the reference observation's
  timestamp is reported with it;
* insufficient coverage is an explicit state, never a fabricated number;
* series are isolated by meeting, method, outcome and open-endedness;
* all dates and instants are UTC, reported as such;
* ``probability_diff_pp = Polymarket - Fed-side`` (the established sign
  convention); differences are descriptive only and are never labeled
  mispricing, hawkish/dovish, buy/sell or a trading signal.
"""

from __future__ import annotations

import math
from datetime import date, datetime, timedelta, timezone

from fedwatch import timeutil
from fedwatch.errors import PROVIDER_HISTORY
from fedwatch.history import (
    FED_METHOD_LIVE,
    FED_METHOD_ZQ,
    POLY_METHOD,
    POLY_SOURCE,
)
from fedwatch.store import FedwatchHistoryStore

FED_METHODS = (FED_METHOD_LIVE, FED_METHOD_ZQ)

METHOD_NOTES = [
    "probability_diff_pp = Polymarket - Fed-side.",
    "All historical calculations use the stored observation chronology on a UTC basis; "
    "no value is interpolated or forward-filled and no missing observation is replaced.",
    "A change is reported only when an accepted observation exists at or before the "
    "lookback instant; the reference observation timestamp is part of the result.",
    "A probability difference can reflect both genuine market disagreement and the "
    "structural Fed-side binary versus Polymarket broad-tail methodology difference; it "
    "is descriptive research data, not a mispricing or trading signal.",
]

# The divergence history is bounded to the most recent window so a pathological
# stored date can never make the day-by-day scan unbounded. Polymarket's public
# history does not usefully predate this window for FedWatch.
MAX_DIVERGENCE_DAYS = 3700


def _finite(value) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    parsed = float(value)
    return parsed if math.isfinite(parsed) else None


def _parse_instant(value: str | None) -> datetime | None:
    if not value:
        return None
    try:
        return timeutil.parse_iso_z(value)
    except (AttributeError, ValueError):
        return None


def _matches_tail(outcome_bp: int, open_ended: bool, requested_bp: int, requested_open: bool) -> bool:
    if not requested_open:
        return outcome_bp == requested_bp and not open_ended
    if requested_bp >= 0:
        return outcome_bp >= requested_bp
    return outcome_bp <= requested_bp


def change_points(
    store: FedwatchHistoryStore,
    meeting_date: str,
    method: str,
    outcome_bp: int,
    open_ended: bool,
) -> tuple[list[dict], list[dict]]:
    """Return the stored value-change chronology for one outcome series.

    For a Fed-side method the series is derived from each stored meeting-level
    distribution exactly like the established comparison semantics: an
    open-ended request is the tail sum on the same side, and an outcome the
    binary Fed-side distribution does not carry is exactly 0 while that
    distribution exists (never fabricated when no distribution exists). A
    Polymarket open-ended outcome is its own stored market. Returns
    ``(points, errors)`` ordered by observation instant; each point carries the
    episode coverage end so date-coverage questions are answered from
    observation intervals, never from interpolation.
    """
    rows = store.observations(
        meeting_date=meeting_date, method=method, source=POLY_SOURCE if method == POLY_METHOD else None
    )
    errors: list[dict] = []
    valid: list[dict] = []
    for row in rows:
        if _finite(row["probability_pct"]) is None or _parse_instant(row["observed_at"]) is None:
            errors.append(
                {
                    "error": "stored observation is malformed and was excluded from analytics",
                    "provider": PROVIDER_HISTORY,
                    "code": "FEDWATCH_HISTORY_ROW_INVALID",
                    "detail": {"observation_id": row["id"]},
                }
            )
            continue
        valid.append(row)

    if method == POLY_METHOD:
        matching = [
            row for row in valid
            if row["outcome_bp"] == outcome_bp and bool(row["open_ended"]) == open_ended
        ]
        points = [
            {
                "observed_at": row["observed_at"],
                "last_observed_at": row["last_observed_at"],
                "probability_pct": float(row["probability_pct"]),
                "source_observed_at": row["source_observed_at"],
                "retrieved_at": row["retrieved_at"],
                "quality_status": row["quality_status"],
                "observation_count": row["observation_count"],
            }
            for row in sorted(matching, key=lambda row: (row["observed_at"], row["id"]))
        ]
        return points, errors

    boundaries = sorted({row["observed_at"] for row in valid})
    points: list[dict] = []
    for boundary in boundaries:
        active_by_outcome: dict[tuple, dict] = {}
        for row in valid:
            if row["observed_at"] <= boundary <= row["last_observed_at"]:
                key = (row["outcome_bp"], bool(row["open_ended"]))
                current = active_by_outcome.get(key)
                if current is None or row["observed_at"] > current["observed_at"]:
                    # At most one episode per exact outcome is active at an
                    # instant; the latest-starting one wins any legacy overlap.
                    active_by_outcome[key] = row
        active = list(active_by_outcome.values())
        if not active:
            continue
        matching = [
            row for row in active
            if _matches_tail(row["outcome_bp"], False, outcome_bp, open_ended)
        ]
        if not matching:
            # An absent bucket is exactly 0 only while a complete meeting
            # distribution is active at this instant (the established
            # comparison semantics); with incomplete coverage there is no
            # value to report, never a fabricated zero.
            active_total = sum(float(row["probability_pct"]) for row in active)
            if abs(active_total - 100.0) > 0.5:
                continue
        total = round(sum(float(row["probability_pct"]) for row in matching), 6)
        if not open_ended and len(matching) == 1 and matching[0]["outcome_bp"] == outcome_bp:
            quality_status = matching[0]["quality_status"]
            observation_count = matching[0]["observation_count"]
            source_observed_at = matching[0]["source_observed_at"]
        else:
            quality_status = "DERIVED_FROM_MEETING_DISTRIBUTION"
            observation_count = max(len(matching), 1)
            source_observed_at = None
        coverage_end = max(row["last_observed_at"] for row in active)
        if (
            points
            and abs(points[-1]["probability_pct"] - total) < 1e-12
            and boundary <= points[-1]["last_observed_at"]
        ):
            # A contiguous equal-value observation extends the covered span;
            # a non-contiguous equal value stays a separate point so a gap in
            # observation is never hidden.
            points[-1]["last_observed_at"] = max(points[-1]["last_observed_at"], coverage_end)
            points[-1]["observation_count"] += observation_count
            continue
        points.append(
            {
                "observed_at": boundary,
                "last_observed_at": coverage_end,
                "probability_pct": total,
                "source_observed_at": source_observed_at,
                "retrieved_at": max(row["retrieved_at"] for row in active),
                "quality_status": quality_status,
                "observation_count": observation_count,
            }
        )
    return points, errors


def _change_entry(reference: dict | None, latest: dict | None, state: str) -> dict:
    if latest is None:
        return {"change_pp": None, "state": "NO_OBSERVATIONS"}
    if reference is None:
        return {"change_pp": None, "state": state}
    return {
        "change_pp": round(latest["probability_pct"] - reference["probability_pct"], 6),
        "reference_probability_pct": reference["probability_pct"],
        "reference_observed_at": reference["observed_at"],
        "reference_quality_status": reference.get("quality_status"),
        "state": None,
    }


def summarize_points(
    points: list[dict],
    as_of: datetime,
    lookback_days: tuple[int, ...] = (1, 7, 30),
) -> dict:
    """The approved per-series calculations with truthful coverage states."""
    eligible = []
    for point in points:
        observed = _parse_instant(point.get("observed_at"))
        if observed is not None and observed <= as_of:
            eligible.append(point)
    if not eligible:
        return {
            "state": "NO_OBSERVATIONS",
            "latest": None,
            "latest_change_from_previous_observation": {"change_pp": None, "state": "NO_OBSERVATIONS"},
            "changes": {
                f"{days}d": {"change_pp": None, "state": "NO_OBSERVATIONS"} for days in lookback_days
            },
            "change_since_first_observation": {"change_pp": None, "state": "NO_OBSERVATIONS"},
            "observed_high": None,
            "observed_low": None,
            "range_width_pp": None,
            "range_position": None,
            "range_position_state": "NO_OBSERVATIONS",
            "percentile_rank": None,
            "episode_count": 0,
            "observation_count": len(points),
            "first_observed_at": None,
            "last_observed_at": None,
        }

    latest = eligible[-1]
    first = eligible[0]
    previous = eligible[-2] if len(eligible) >= 2 else None
    high = max(eligible, key=lambda point: point["probability_pct"])
    low = min(eligible, key=lambda point: point["probability_pct"])
    width = round(high["probability_pct"] - low["probability_pct"], 6)

    changes = {}
    for days in lookback_days:
        target = as_of - timedelta(days=days)
        references = [
            point for point in eligible if _parse_instant(point["observed_at"]) <= target
        ]
        if not references:
            changes[f"{days}d"] = {
                "change_pp": None,
                "state": "INSUFFICIENT_HISTORY",
                "required_lookback_days": days,
                "earliest_observed_at": first["observed_at"],
            }
        else:
            changes[f"{days}d"] = _change_entry(references[-1], latest, "INSUFFICIENT_HISTORY")

    if width > 0.0:
        range_position = round((latest["probability_pct"] - low["probability_pct"]) / width, 6)
        range_state = None
    else:
        range_position = None
        range_state = "ZERO_WIDTH_RANGE"

    percentile_rank = round(
        sum(1 for point in eligible if point["probability_pct"] <= latest["probability_pct"])
        / len(eligible),
        6,
    )
    return {
        "state": "OK",
        "latest": {
            "probability_pct": latest["probability_pct"],
            "observed_at": latest["observed_at"],
            "last_observed_at": latest["last_observed_at"],
            "source_observed_at": latest.get("source_observed_at"),
            "retrieved_at": latest.get("retrieved_at"),
            "quality_status": latest.get("quality_status"),
        },
        "latest_change_from_previous_observation": _change_entry(
            previous, latest, "NO_PREVIOUS_OBSERVATION"
        ),
        "changes": changes,
        "change_since_first_observation": _change_entry(first, latest, "NO_OBSERVATIONS"),
        "observed_high": {
            "probability_pct": high["probability_pct"],
            "observed_at": high["observed_at"],
        },
        "observed_low": {
            "probability_pct": low["probability_pct"],
            "observed_at": low["observed_at"],
        },
        "range_width_pp": width,
        "range_position": range_position,
        "range_position_state": range_state,
        "percentile_rank": percentile_rank,
        "episode_count": len(eligible),
        "observation_count": sum(point.get("observation_count", 1) for point in eligible),
        "first_observed_at": first["observed_at"],
        "last_observed_at": latest["last_observed_at"],
    }


def _day_bounds(day: date) -> tuple[datetime, datetime]:
    start = datetime(day.year, day.month, day.day, tzinfo=timezone.utc)
    return start, start + timedelta(days=1) - timedelta(microseconds=1)


def _value_covering_day(points: list[dict], day: date) -> dict | None:
    """The observed value whose episode covers ``day``; None when unobserved."""
    start, end = _day_bounds(day)
    candidates = []
    for point in points:
        observed = _parse_instant(point["observed_at"])
        last = _parse_instant(point["last_observed_at"]) or observed
        if observed is None:
            continue
        if observed <= end and last >= start:
            candidates.append((observed, point))
    if not candidates:
        return None
    candidates.sort(key=lambda item: item[0])
    return candidates[-1][1]


def divergence_history(
    fed_points: list[dict],
    polymarket_points: list[dict],
    as_of: datetime,
) -> list[dict]:
    """Daily ``Polymarket - Fed-side`` differences over jointly covered UTC days.

    Only days on which both sources have an observation episode covering the
    day appear. A gap on either side remains a gap; it is never filled or
    interpolated.
    """
    if not fed_points or not polymarket_points:
        return []
    first_days = [
        _parse_instant(point["observed_at"]).date()
        for point in fed_points + polymarket_points
        if _parse_instant(point["observed_at"]) is not None
    ]
    if not first_days:
        return []
    day = min(first_days)
    last_day = as_of.date()
    window_start = last_day - timedelta(days=MAX_DIVERGENCE_DAYS - 1)
    if day < window_start:
        day = window_start
    rows: list[dict] = []
    while day <= last_day:
        fed = _value_covering_day(fed_points, day)
        poly = _value_covering_day(polymarket_points, day)
        if fed is not None and poly is not None:
            difference = round(poly["probability_pct"] - fed["probability_pct"], 6)
            if difference == 0.0:
                difference = 0.0
            rows.append(
                {
                    "date": day.isoformat(),
                    "fed_probability_pct": fed["probability_pct"],
                    "polymarket_probability_pct": poly["probability_pct"],
                    "probability_diff_pp": difference,
                    "fed_observed_at": fed["observed_at"],
                    "polymarket_observed_at": poly["observed_at"],
                }
            )
        day += timedelta(days=1)
    return rows


def compute_analytics(
    store: FedwatchHistoryStore,
    meeting_date: str,
    outcome_bp: int,
    open_ended: bool = False,
    fed_method: str = FED_METHOD_LIVE,
    as_of: datetime | None = None,
) -> dict:
    """All approved historical calculations for one meeting/outcome series."""
    if fed_method not in FED_METHODS:
        raise ValueError(
            f"fed_method must be one of {FED_METHODS!r}, not {fed_method!r}"
        )
    as_of = as_of or timeutil.utc_now()
    meeting = store.get_meeting(meeting_date)
    fed_points, fed_errors = change_points(
        store, meeting_date, fed_method, outcome_bp, open_ended
    )
    poly_points, poly_errors = change_points(
        store, meeting_date, POLY_METHOD, outcome_bp, open_ended
    )
    fed_summary = summarize_points(fed_points, as_of)
    poly_summary = summarize_points(poly_points, as_of)

    current_difference = None
    if fed_summary["state"] == "OK" and poly_summary["state"] == "OK":
        current_difference = round(
            poly_summary["latest"]["probability_pct"]
            - fed_summary["latest"]["probability_pct"],
            6,
        )
        if current_difference == 0.0:
            current_difference = 0.0

    history = divergence_history(fed_points, poly_points, as_of)
    return {
        "meeting_date": meeting_date,
        "meeting_status": meeting["status"] if meeting else None,
        "actual_outcome_bp": meeting["actual_outcome_bp"] if meeting else None,
        "outcome_bp": outcome_bp,
        "open_ended": open_ended,
        "as_of": timeutil.iso_z(as_of),
        "time_basis": "UTC",
        "fed_side": {"method": fed_method, **fed_summary},
        "polymarket": {"method": POLY_METHOD, **poly_summary},
        "difference": {
            "sign_convention": "Polymarket - Fed-side",
            "fed_method": fed_method,
            "polymarket_method": POLY_METHOD,
            "current_probability_diff_pp": current_difference,
            "current_fed_probability_pct": (
                fed_summary["latest"]["probability_pct"]
                if fed_summary["state"] == "OK"
                else None
            ),
            "current_polymarket_probability_pct": (
                poly_summary["latest"]["probability_pct"]
                if poly_summary["state"] == "OK"
                else None
            ),
            "history": history,
            "history_basis": (
                "UTC calendar dates on which both sources have accepted observations "
                "covering the day; gaps on either side remain gaps"
            ),
        },
        "errors": fed_errors + poly_errors,
        "method_notes": list(METHOD_NOTES),
    }
