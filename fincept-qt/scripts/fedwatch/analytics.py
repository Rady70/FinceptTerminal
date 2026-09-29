"""Historical FedWatch analytics computed from durable stored observations.

Batch B must let Batch C consume the approved historical calculations without
inventing financial semantics in the UI layer (FEDWATCH_INTEGRATION_PLAN.md
section 11). Every value here is computed on demand from the stored accepted
observations; nothing derived is persisted.

Guarantees:

* the actual stored observation chronology is used — each stored row is one
  accepted observation instant, and no coverage is inferred between
  observations (no interval, no interpolation, no forward-fill);
* "previous observation" is the immediately preceding accepted observation of
  the same series, not the previous distinct value;
* a lookback (1/7/30-day) change is only produced when an accepted observation
  exists at or before the lookback instant, and the reference observation's
  timestamp is reported with it;
* insufficient coverage is an explicit state, never a fabricated number;
* the *current* Fed-side versus Polymarket difference is only produced when
  both latest observations are inside the established current-freshness window;
  stale observations stay inspectable historically but never silently become a
  current cross-source comparison;
* series are isolated by meeting, method, outcome and open-endedness;
* all dates and instants are UTC, reported as such;
* ``probability_diff_pp = Polymarket - Fed-side`` (the established sign
  convention); differences are descriptive only and are never labeled
  mispricing, hawkish/dovish, buy/sell or a trading signal.
"""

from __future__ import annotations

import math
from datetime import date, datetime, timedelta

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

# The established current-freshness window (the qualified Polymarket source
# window). A current cross-source comparison needs both latest observations
# inside it; older observations remain historical and are never presented as
# current.
CURRENT_MAX_AGE_DAYS = 3.0

# The divergence history is bounded to the most recent window so a pathological
# stored date can never make the day-by-day scan unbounded. Polymarket's public
# history does not usefully predate this window for FedWatch.
MAX_DIVERGENCE_DAYS = 3700

METHOD_NOTES = [
    "probability_diff_pp = Polymarket - Fed-side.",
    "All historical calculations use the stored accepted-observation chronology on a "
    "UTC basis; no value is interpolated or forward-filled and no observation is "
    "inferred between stored instants.",
    "A change is reported only when an accepted observation exists at or before the "
    "lookback instant; the reference observation timestamp is part of the result.",
    "The current cross-source difference is only produced when both sources' latest "
    "observations are within the current-freshness window; a stale observation stays "
    "historical and does not become a current comparison.",
    "A probability difference can reflect both genuine market disagreement and the "
    "structural Fed-side binary versus Polymarket broad-tail methodology difference; it "
    "is descriptive research data, not a mispricing or trading signal.",
]


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


def _point_from_row(row: dict) -> dict:
    return {
        "observed_at": row["observed_at"],
        "probability_pct": float(row["probability_pct"]),
        "source_observed_at": row["source_observed_at"],
        "retrieved_at": row["retrieved_at"],
        "quality_status": row["quality_status"],
        "observation_count": row["observation_count"],
    }


def change_points(
    store: FedwatchHistoryStore,
    meeting_date: str,
    method: str,
    outcome_bp: int,
    open_ended: bool,
) -> tuple[list[dict], list[dict]]:
    """Return one point per observed instant for one outcome series.

    For a Fed-side method the value at each observed instant is derived from
    that instant's stored meeting-level distribution exactly like the
    established comparison semantics: an open-ended request is the tail sum on
    the same side, and an outcome the binary Fed-side distribution does not
    carry is exactly 0 while a complete distribution is stored at that
    instant. A Polymarket open-ended outcome is its own stored market. Returns
    ``(points, errors)`` ordered by the observation instant; no coverage is
    inferred between instants.
    """
    rows = store.observations(
        meeting_date=meeting_date, method=method, source=POLY_SOURCE if method == POLY_METHOD else None
    )
    errors: list[dict] = []
    valid: list[dict] = []
    for row in rows:
        probability = _finite(row["probability_pct"])
        if probability is None or not 0.0 <= probability <= 100.0 or _parse_instant(row["observed_at"]) is None:
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
            _point_from_row(row)
            for row in sorted(matching, key=lambda row: (row["observed_at"], row["id"]))
        ]
        return points, errors

    by_instant: dict[str, list[dict]] = {}
    for row in valid:
        by_instant.setdefault(row["observed_at"], []).append(row)
    points: list[dict] = []
    for instant in sorted(by_instant):
        rows_at = by_instant[instant]
        matching = [
            row for row in rows_at
            if _matches_tail(row["outcome_bp"], False, outcome_bp, open_ended)
        ]
        if not matching:
            # An absent bucket is exactly 0 only while a complete meeting
            # distribution is stored at this instant (the established
            # comparison semantics); otherwise there is no value to report,
            # never a fabricated zero.
            active_total = sum(float(row["probability_pct"]) for row in rows_at)
            if abs(active_total - 100.0) > 0.5:
                continue
        total = round(sum(float(row["probability_pct"]) for row in matching), 6)
        if not open_ended and len(matching) == 1 and matching[0]["outcome_bp"] == outcome_bp:
            point = _point_from_row(matching[0])
            point["probability_pct"] = total
        else:
            point = {
                "observed_at": instant,
                "probability_pct": total,
                "source_observed_at": None,
                "retrieved_at": max(row["retrieved_at"] for row in rows_at),
                "quality_status": "DERIVED_FROM_MEETING_DISTRIBUTION",
                "observation_count": max(len(matching), 1),
            }
        points.append(point)
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
    eligible.sort(key=lambda point: (point["observed_at"], 0))
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
            "observation_count": 0,
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
        "observation_count": sum(point.get("observation_count", 1) for point in eligible),
        "first_observed_at": first["observed_at"],
        "last_observed_at": latest["observed_at"],
    }


def _latest_by_day(points: list[dict], last_day: date) -> dict[str, dict]:
    """Latest actual observation per UTC calendar day, up to ``last_day``."""
    by_day: dict[str, dict] = {}
    for point in points:
        observed = _parse_instant(point.get("observed_at"))
        if observed is None:
            continue
        day = observed.date()
        if day > last_day:
            continue
        key = day.isoformat()
        previous = by_day.get(key)
        if previous is None or observed > _parse_instant(previous["observed_at"]):
            by_day[key] = point
    return by_day


def divergence_history(
    fed_points: list[dict],
    polymarket_points: list[dict],
    as_of: datetime,
) -> list[dict]:
    """Daily ``Polymarket - Fed-side`` differences over actually observed days.

    A UTC day appears only when each source has at least one *actual*
    observation whose instant falls on that day; the day's value is the latest
    observation of that day for each source. No day is inferred between
    observations, and a gap on either side remains a gap.
    """
    if not fed_points or not polymarket_points:
        return []
    last_day = as_of.date()
    fed_by_day = _latest_by_day(fed_points, last_day)
    poly_by_day = _latest_by_day(polymarket_points, last_day)
    window_start = last_day - timedelta(days=MAX_DIVERGENCE_DAYS - 1)
    rows: list[dict] = []
    for day in sorted(set(fed_by_day) & set(poly_by_day)):
        day_value = date.fromisoformat(day)
        if day_value < window_start:
            continue
        fed = fed_by_day[day]
        poly = poly_by_day[day]
        difference = round(poly["probability_pct"] - fed["probability_pct"], 6)
        if difference == 0.0:
            difference = 0.0
        rows.append(
            {
                "date": day,
                "fed_probability_pct": fed["probability_pct"],
                "polymarket_probability_pct": poly["probability_pct"],
                "probability_diff_pp": difference,
                "fed_observed_at": fed["observed_at"],
                "polymarket_observed_at": poly["observed_at"],
            }
        )
    return rows


def _current_difference(fed_summary: dict, poly_summary: dict, as_of: datetime) -> dict:
    """The current cross-source difference, gated by latest-observation age."""
    base = {
        "sign_convention": "Polymarket - Fed-side",
        "fed_method": None,
        "polymarket_method": POLY_METHOD,
        "current_probability_diff_pp": None,
        "current_fed_probability_pct": None,
        "current_polymarket_probability_pct": None,
        "current_state": None,
        "current_max_age_days": CURRENT_MAX_AGE_DAYS,
        "fed_latest_age_days": None,
        "polymarket_latest_age_days": None,
    }
    if fed_summary["state"] != "OK" or poly_summary["state"] != "OK":
        base["current_state"] = "MISSING_SIDE"
        return base
    fed_latest = fed_summary["latest"]
    poly_latest = poly_summary["latest"]
    fed_age = (as_of - _parse_instant(fed_latest["observed_at"])).total_seconds() / 86400.0
    poly_age = (as_of - _parse_instant(poly_latest["observed_at"])).total_seconds() / 86400.0
    base["fed_latest_age_days"] = round(fed_age, 6)
    base["polymarket_latest_age_days"] = round(poly_age, 6)
    base["current_fed_probability_pct"] = fed_latest["probability_pct"]
    base["current_polymarket_probability_pct"] = poly_latest["probability_pct"]
    if fed_age > CURRENT_MAX_AGE_DAYS or poly_age > CURRENT_MAX_AGE_DAYS:
        base["current_state"] = "STALE_LATEST_OBSERVATION"
        return base
    difference = round(poly_latest["probability_pct"] - fed_latest["probability_pct"], 6)
    if difference == 0.0:
        difference = 0.0
    base["current_state"] = "OK"
    base["current_probability_diff_pp"] = difference
    return base


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

    difference = _current_difference(fed_summary, poly_summary, as_of)
    difference["fed_method"] = fed_method
    difference["history"] = divergence_history(fed_points, poly_points, as_of)
    difference["history_basis"] = (
        "UTC calendar days on which each source has an actual accepted observation; "
        "the day's value is that day's latest observation per source and gaps on "
        "either side remain gaps"
    )

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
        "difference": difference,
        "errors": fed_errors + poly_errors,
        "method_notes": list(METHOD_NOTES),
    }
