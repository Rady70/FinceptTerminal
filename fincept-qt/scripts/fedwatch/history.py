"""Durable FedWatch history: collection, lifecycle and Polymarket backfill.

This module turns the finalized Batch A current-observation contract into
durable MarketLab history (FEDWATCH_INTEGRATION_PLAN.md section 9) without
changing the current contract:

* :func:`collect` records an accepted snapshot's Fed-side and Polymarket
  observations into the SQLite history store, then advances the durable FOMC
  meeting lifecycle from FRED's official target-range series.
* :func:`backfill_polymarket` imports the qualified CLOB ``prices-history``
  (``interval=max``, ``fidelity=1440``) for already-validated mappings,
  idempotently and with an intentional refresh cadence.
* :func:`record_zq_observations` persists reconstructed historical ZQ
  observations; the user's raw ZQ dataset is read but never copied.

Only accepted observations become history. Every unattempted or skipped
recording is reported with an explicit machine-readable reason so a provider
failure or a stale value can never appear as a stored zero-probability
observation.
"""

from __future__ import annotations

import math
import time
from datetime import date, datetime, timedelta, timezone

from fedwatch import fomc, fred, polymarket, timeutil
from fedwatch import zq as fedwatch_zq
from fedwatch.errors import (
    PROVIDER_FOMC_CALENDAR,
    PROVIDER_HISTORY,
    PROVIDER_ZQ,
    FedwatchError,
    HistoryStoreError,
)
from fedwatch.store import (
    MEETING_STATUS_PENDING,
    MEETING_STATUS_RESOLVED,
    MEETING_STATUS_UPCOMING,
    FedwatchHistoryStore,
    content_digest,
)
from fedwatch.transport import HttpTransport, Transport

FED_METHOD_LIVE = "LIVE_INVESTING_DERIVED"
FED_METHOD_ZQ = "HISTORICAL_ZQ_RECONSTRUCTED"
POLY_METHOD = "POLYMARKET_CLOB"
POLY_SOURCE = "polymarket"
INVESTING_SOURCE = "investing"
ZQ_SOURCE = "zq"

BACKFILL_REFRESH_HOURS = 24
BACKFILL_FIDELITY_MINUTES = 1440
FUTURE_TOLERANCE_SECONDS = polymarket.FUTURE_TOLERANCE_SECONDS

# Resolution uses FRED's official target-range series, exactly the qualified
# FedWatch convention (decision = post-meeting range minus pre-meeting range).
# Two effective-date conventions are evaluated and must agree: the qualified
# inclusive convention (a change effective on the decision day) and the strict
# next-day convention (the Fed's normal implementation). Disagreement leaves
# the meeting PENDING rather than guessing an outcome.
RESOLUTION_LOOKAHEAD_DAYS = 3


def _finite_number(value) -> float | None:
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


# ── recording the accepted current snapshot ────────────────────────────────


def _count_record(report: dict, result: str) -> None:
    report["records"].append(result)
    report["counts"][result] = report["counts"].get(result, 0) + 1


def _record_fed_side(
    store: FedwatchHistoryStore,
    meeting: dict,
    retrieved_at: str,
    recorded_at: datetime,
    report: dict,
) -> None:
    meeting_date = meeting["meeting_date"]
    fed = meeting.get("fed_side")
    if not isinstance(fed, dict):
        report["skipped"].append(
            {"meeting_date": meeting_date, "source": INVESTING_SOURCE, "reason": "FED_SIDE_UNAVAILABLE"}
        )
        return
    local_rows = fed.get("local_probabilities")
    if fed.get("local_status") != "OK" or not local_rows:
        report["skipped"].append(
            {
                "meeting_date": meeting_date,
                "source": INVESTING_SOURCE,
                "reason": "FED_SIDE_LOCAL_PROBABILITY_UNAVAILABLE",
                "detail": {"local_status": fed.get("local_status")},
            }
        )
        return

    method = fed.get("method") or FED_METHOD_LIVE
    freshness = fed.get("freshness") or {}
    observed_at = retrieved_at
    source_observed_at = fed.get("source_timestamp")
    detail = {
        "origin": "live_collect",
        "source": fed.get("source"),
        "method": method,
        "source_timestamp": source_observed_at,
        "timestamp_note": fed.get("timestamp_note"),
        "meeting_ordinal": fed.get("meeting_ordinal"),
        "raw_probabilities": fed.get("raw_probabilities"),
        "normalized_probabilities": fed.get("normalized_probabilities"),
        "normalization": fed.get("normalization"),
        "local_probabilities": local_rows,
        "freshness": freshness,
    }
    for row in local_rows:
        probability = _finite_number(row.get("probability_pct"))
        outcome_bp = row.get("outcome_bp")
        if probability is None or isinstance(outcome_bp, bool) or not isinstance(outcome_bp, int):
            report["skipped"].append(
                {
                    "meeting_date": meeting_date,
                    "source": INVESTING_SOURCE,
                    "reason": "MALFORMED_FED_SIDE_OUTCOME",
                    "detail": {"outcome_bp": outcome_bp, "probability_pct": row.get("probability_pct")},
                }
            )
            continue
        digest = content_digest(
            {
                "method": method,
                "outcome_bp": outcome_bp,
                "probability_pct": probability,
                "raw_probabilities": fed.get("raw_probabilities"),
                "normalized_probabilities": fed.get("normalized_probabilities"),
                "normalization": fed.get("normalization"),
            }
        )
        result = store.record_observation(
            meeting_date=meeting_date,
            source=INVESTING_SOURCE,
            method=method,
            outcome_bp=outcome_bp,
            open_ended=False,
            probability_pct=probability,
            raw_probability_pct=None,
            normalized_probability_pct=None,
            observed_at=observed_at,
            source_observed_at=source_observed_at,
            retrieved_at=retrieved_at,
            quality_status="OK",
            freshness_status=freshness.get("status"),
            digest=digest,
            detail=detail,
            recorded_at=recorded_at,
        )
        report["fed_side_observations"] += 1
        _count_record(report, result)


def _record_polymarket(
    store: FedwatchHistoryStore,
    meeting: dict,
    retrieved_at: str,
    recorded_at: datetime,
    report: dict,
) -> None:
    meeting_date = meeting["meeting_date"]
    poly = meeting.get("polymarket")
    if not isinstance(poly, dict):
        report["skipped"].append(
            {"meeting_date": meeting_date, "source": POLY_SOURCE, "reason": "POLYMARKET_SECTION_UNAVAILABLE"}
        )
        return
    if poly.get("mapping_status") != "VALIDATED":
        store.mark_mapping_revalidation(
            meeting_date,
            POLY_SOURCE,
            POLY_METHOD,
            str(poly.get("mapping_status") or "UNKNOWN"),
            now=recorded_at,
        )
        report["skipped"].append(
            {
                "meeting_date": meeting_date,
                "source": POLY_SOURCE,
                "reason": "POLYMARKET_MAPPING_NOT_VALIDATED",
                "detail": {"mapping_status": poly.get("mapping_status")},
            }
        )
        return

    event_id = poly.get("event_id")
    event_title = poly.get("event_title")
    evidence = poly.get("mapping_evidence")
    outcomes = poly.get("outcomes")
    if not isinstance(outcomes, list) or not outcomes:
        report["skipped"].append(
            {"meeting_date": meeting_date, "source": POLY_SOURCE, "reason": "POLYMARKET_NO_OUTCOMES"}
        )
        return

    data_status = poly.get("data_status")
    freshness = poly.get("freshness") or {}
    for outcome in outcomes:
        outcome_bp = outcome.get("outcome_bp")
        open_ended = bool(outcome.get("open_ended"))
        if isinstance(outcome_bp, bool) or not isinstance(outcome_bp, int):
            report["skipped"].append(
                {
                    "meeting_date": meeting_date,
                    "source": POLY_SOURCE,
                    "reason": "MALFORMED_POLYMARKET_OUTCOME",
                    "detail": {"outcome_bp": outcome_bp},
                }
            )
            continue
        store.upsert_mapping_outcome(
            meeting_date=meeting_date,
            source=POLY_SOURCE,
            method=POLY_METHOD,
            outcome_bp=outcome_bp,
            open_ended=open_ended,
            mapping_status="VALIDATED",
            external_event_id=str(event_id) if event_id is not None else None,
            external_event_title=event_title,
            external_market_id=outcome.get("market_id"),
            external_token_id=outcome.get("token_id"),
            question=outcome.get("question"),
            mapping_evidence=evidence,
            now=recorded_at,
        )

        probability = _finite_number(outcome.get("probability_pct"))
        if probability is None:
            report["skipped"].append(
                {
                    "meeting_date": meeting_date,
                    "source": POLY_SOURCE,
                    "reason": "POLYMARKET_OUTCOME_WITHOUT_PROBABILITY",
                    "detail": {"outcome_bp": outcome_bp, "data_status": data_status},
                }
            )
            continue

        observed_at = outcome.get("source_timestamp") or retrieved_at
        source_observed_at = outcome.get("source_timestamp")
        if _parse_instant(observed_at) is None:
            report["skipped"].append(
                {
                    "meeting_date": meeting_date,
                    "source": POLY_SOURCE,
                    "reason": "MALFORMED_POLYMARKET_TIMESTAMP",
                    "detail": {"outcome_bp": outcome_bp, "source_timestamp": source_observed_at},
                }
            )
            continue
        if source_observed_at is not None and _parse_instant(source_observed_at) is None:
            observed_at = retrieved_at
            source_observed_at = None
        detail = {
            "origin": "live_collect",
            "event_id": event_id,
            "event_title": event_title,
            "event_end_date": poly.get("event_end_date"),
            "market_id": outcome.get("market_id"),
            "token_id": outcome.get("token_id"),
            "question": outcome.get("question"),
            "open_ended": open_ended,
            "data_status": data_status,
            "freshness": freshness,
            "mapping_evidence": evidence,
        }
        digest = content_digest(
            {
                "event_id": event_id,
                "market_id": outcome.get("market_id"),
                "token_id": outcome.get("token_id"),
                "outcome_bp": outcome_bp,
                "open_ended": open_ended,
                "probability_pct": probability,
            }
        )
        result = store.record_observation(
            meeting_date=meeting_date,
            source=POLY_SOURCE,
            method=POLY_METHOD,
            outcome_bp=outcome_bp,
            open_ended=open_ended,
            probability_pct=probability,
            raw_probability_pct=probability,
            normalized_probability_pct=None,
            observed_at=observed_at,
            source_observed_at=source_observed_at,
            retrieved_at=retrieved_at,
            quality_status=data_status or "UNKNOWN",
            freshness_status=freshness.get("status"),
            digest=digest,
            detail=detail,
            recorded_at=recorded_at,
        )
        report["polymarket_observations"] += 1
        _count_record(report, result)


def record_snapshot(
    store: FedwatchHistoryStore,
    data: dict,
    clock=timeutil.utc_now,
) -> dict:
    """Record an accepted Batch A snapshot's observations into durable history.

    Resolved, past-awaiting-resolution and past-dated meetings are never given
    new live observations. ``collect`` evaluates the lifecycle *before* calling
    this function; the past-date guard here is defense in depth so a snapshot
    that still carries a past meeting cannot open a live-collection hole. The
    returned report counts each recorded observation and lists every skip with
    its reason.
    """
    recorded_at = clock()
    retrieved_at = data.get("retrieved_at")
    if _parse_instant(retrieved_at) is None:
        raise HistoryStoreError(
            "FEDWATCH_HISTORY_WRITE_FAILED",
            f"snapshot retrieved_at is not a valid UTC instant: {retrieved_at!r}",
        )
    as_of_date = recorded_at.date() if isinstance(recorded_at, datetime) else date.today()
    report = {
        "fed_side_observations": 0,
        "polymarket_observations": 0,
        "counts": {},
        "records": [],
        "skipped": [],
    }
    for meeting in data.get("meetings") or []:
        if not isinstance(meeting, dict):
            report["skipped"].append({"meeting_date": None, "source": None, "reason": "MALFORMED_MEETING"})
            continue
        meeting_date = meeting.get("meeting_date")
        if not meeting_date:
            report["skipped"].append({"meeting_date": None, "source": None, "reason": "MISSING_MEETING_DATE"})
            continue
        if not isinstance(meeting_date, str):
            report["skipped"].append(
                {"meeting_date": None, "source": None, "reason": "MALFORMED_MEETING_DATE",
                 "detail": {"meeting_date_type": type(meeting_date).__name__}}
            )
            continue
        try:
            meeting_day = timeutil.parse_date(meeting_date)
        except (AttributeError, ValueError):
            report["skipped"].append(
                {"meeting_date": meeting_date, "source": None, "reason": "MALFORMED_MEETING_DATE"}
            )
            continue

        store.upsert_meeting(
            meeting_date,
            calendar=meeting.get("fomc_calendar"),
            default_status=MEETING_STATUS_UPCOMING,
            now=recorded_at,
        )
        stored = store.get_meeting(meeting_date)
        if stored is not None and stored["status"] == MEETING_STATUS_RESOLVED:
            report["skipped"].append(
                {"meeting_date": meeting_date, "source": None, "reason": "MEETING_RESOLVED"}
            )
            continue
        if stored is not None and stored["status"] == MEETING_STATUS_PENDING:
            report["skipped"].append(
                {
                    "meeting_date": meeting_date,
                    "source": None,
                    "reason": "MEETING_AWAITING_RESOLUTION",
                    "detail": {"status_reason": stored["status_reason"]},
                }
            )
            continue
        if meeting_day < as_of_date:
            report["skipped"].append(
                {
                    "meeting_date": meeting_date,
                    "source": None,
                    "reason": "MEETING_DATE_IN_PAST",
                    "detail": {"status": stored["status"] if stored else None},
                }
            )
            continue

        _record_fed_side(store, meeting, retrieved_at, recorded_at, report)
        _record_polymarket(store, meeting, retrieved_at, recorded_at, report)
    return report


# ── durable meeting lifecycle ──────────────────────────────────────────────


def _last_at_or_before(rows: list[dict], day: date) -> dict | None:
    candidates = [row for row in rows if row["date"] <= day]
    return candidates[-1] if candidates else None


def _delta_bp(before: dict, after: dict) -> int | None:
    raw = (float(after["value"]) - float(before["value"])) * 100.0
    if not math.isfinite(raw):
        return None
    rounded = int(round(raw))
    if abs(raw - rounded) > 1e-6:
        return None
    return rounded


def _convention_delta(before: dict | None, after: dict | None) -> tuple[int, int] | None:
    if before is None or after is None:
        return None
    upper_delta = _delta_bp(before["upper"], after["upper"])
    lower_delta = _delta_bp(before["lower"], after["lower"])
    if upper_delta is None or lower_delta is None:
        return None
    if upper_delta != lower_delta:
        return None
    return upper_delta, lower_delta


def _first_change(rows: list[dict], start: date, end: date, baseline_value: float) -> dict | None:
    """First observation in ``[start, end]`` whose value differs from baseline."""
    for row in rows:
        if start <= row["date"] <= end and float(row["value"]) != float(baseline_value):
            return row
    return None


def resolve_actual_outcome(
    end_date: date,
    upper_rows: list[dict],
    lower_rows: list[dict],
    lookahead_days: int = RESOLUTION_LOOKAHEAD_DAYS,
) -> dict:
    """Establish a meeting's actual target-rate decision from FRED, or refuse.

    ``upper_rows``/``lower_rows`` are ``[{"date": date, "value": float}, ...]``.
    Duplicate dates collapse to the last supplied row. The decision is the
    *first* target-range change observed after the meeting inside the lookahead
    window (the meeting's effective move); a later intermeeting change inside
    the same window is never accumulated onto the meeting. It is only accepted
    when the qualified inclusive convention and the strict next-day convention
    agree, so a same-day or missing FRED update can never be misread as a hold.

    Returns ``{"resolvable": True, "outcome_bp": int, "detail": {...}}`` or
    ``{"resolvable": False, "reason": code, "detail": {...}}``.
    """
    lookahead_days = max(1, min(int(lookahead_days), 7))
    upper_by_date = {row["date"]: row for row in upper_rows}
    lower_by_date = {row["date"]: row for row in lower_rows}
    upper = [{"date": day, "value": upper_by_date[day]["value"]} for day in sorted(upper_by_date)]
    lower = [{"date": day, "value": lower_by_date[day]["value"]} for day in sorted(lower_by_date)]

    if not upper or not lower:
        return {
            "resolvable": False,
            "reason": "FRED_COVERAGE_INSUFFICIENT",
            "detail": {"upper_rows": len(upper), "lower_rows": len(lower)},
        }
    if any(not math.isfinite(row["value"]) for row in upper + lower):
        return {"resolvable": False, "reason": "FRED_VALUE_INVALID", "detail": {}}

    def pair(day: date) -> dict | None:
        if day in upper_by_date and day in lower_by_date:
            return {"upper": upper_by_date[day], "lower": lower_by_date[day]}
        return None

    def range_snapshot(day: date) -> dict | None:
        if day in upper_by_date and day in lower_by_date:
            return {
                "date": day.isoformat(),
                "upper": upper_by_date[day]["value"],
                "lower": lower_by_date[day]["value"],
            }
        return None

    def convention(before_day: date | None, window_start: date, window_end: date) -> dict:
        if before_day is None:
            return {"status": "missing_before"}
        before_pair = pair(before_day)
        if before_pair is None:
            return {"status": "missing_pair"}
        window_rows = [row for row in upper if window_start <= row["date"] <= window_end]
        if not window_rows:
            return {"status": "missing_window"}
        changed = _first_change(
            upper, window_start, window_end, before_pair["upper"]["value"]
        )
        if changed is None:
            after_day = window_rows[-1]["date"]
            after_pair = before_pair
        else:
            after_day = changed["date"]
            after_pair = pair(after_day)
            if after_pair is None:
                return {"status": "missing_pair", "after_date": after_day.isoformat()}
        delta = _convention_delta(before_pair, after_pair)
        if delta is None:
            return {"status": "bounds_inconsistent"}
        return {
            "status": "ok",
            "delta": delta[0],
            "before_day": before_day,
            "after_day": after_day,
            "before_pair": before_pair,
            "after_pair": after_pair,
        }

    inclusive_before_day = _last_at_or_before(upper, end_date)
    strict_rows = [row for row in upper if row["date"] < end_date]
    strict_before_day = strict_rows[-1]["date"] if strict_rows else None
    suffix_end = end_date + timedelta(days=lookahead_days)
    inclusive = convention(inclusive_before_day["date"] if inclusive_before_day else None,
                           end_date, suffix_end)
    strict = convention(strict_before_day, end_date, suffix_end)
    has_any_before = inclusive_before_day is not None

    if not has_any_before or (
        inclusive["status"] != "ok" and strict["status"] != "ok"
    ):
        if inclusive["status"] == "missing_window" or strict["status"] == "missing_window":
            reason = "FRED_COVERAGE_INSUFFICIENT"
        elif inclusive["status"] == "bounds_inconsistent" and strict["status"] == "bounds_inconsistent":
            reason = "FRED_BOUNDS_INCONSISTENT"
        else:
            reason = "FRED_COVERAGE_INSUFFICIENT"
        return {
            "resolvable": False,
            "reason": reason,
            "detail": {
                "end_date": end_date.isoformat(),
                "has_pre_meeting_observation": has_any_before,
                "has_post_meeting_observation": any(row["date"] > end_date for row in upper),
                "inclusive_status": inclusive["status"],
                "strict_status": strict["status"],
                "upper_latest_date": upper[-1]["date"].isoformat() if upper else None,
                "lookahead_days": lookahead_days,
            },
        }
    if inclusive["status"] != "ok" or strict["status"] != "ok" or inclusive["delta"] != strict["delta"]:
        return {
            "resolvable": False,
            "reason": "FRED_EFFECTIVE_DATE_AMBIGUOUS",
            "detail": {
                "end_date": end_date.isoformat(),
                "inclusive_convention_bp": inclusive.get("delta"),
                "strict_convention_bp": strict.get("delta"),
                "inclusive_status": inclusive["status"],
                "strict_status": strict["status"],
            },
        }

    outcome_bp = inclusive["delta"]
    before_day = inclusive["before_day"]
    after_day = inclusive["after_day"]
    return {
        "resolvable": True,
        "outcome_bp": outcome_bp,
        "detail": {
            "end_date": end_date.isoformat(),
            "method": "FRED DFEDTARU/DFEDTARL target-range change",
            "inclusive_convention_bp": inclusive["delta"],
            "strict_convention_bp": strict["delta"],
            "upper_delta_bp": _delta_bp(inclusive["before_pair"]["upper"], inclusive["after_pair"]["upper"]),
            "lower_delta_bp": _delta_bp(inclusive["before_pair"]["lower"], inclusive["after_pair"]["lower"]),
            "rate_before_date": before_day.isoformat() if before_day else None,
            "rate_after_date": after_day.isoformat() if after_day else None,
            "rate_before": range_snapshot(before_day) if before_day else None,
            "rate_after": range_snapshot(after_day) if after_day else None,
            "lookahead_days": lookahead_days,
        },
    }


def evaluate_lifecycle(
    store: FedwatchHistoryStore,
    fred_history: dict | None,
    clock=timeutil.utc_now,
) -> dict:
    """Advance unresolved past meetings using the official FRED series.

    A meeting whose decision day has passed is marked RESOLVED only when FRED
    establishes the actual target-range change; otherwise it stays PENDING with
    the exact reason. Resolved meetings are left untouched and receive no
    further live collection.
    """
    now = clock()
    result = {"evaluated": 0, "resolved": [], "pending": [], "errors": []}
    pending = store.meetings_awaiting_resolution(now.date())
    if not pending:
        return result
    if fred_history is None:
        result["errors"].append(
            {
                "error": "FRED target-range history unavailable; meeting outcomes cannot be established",
                "provider": "fred",
                "code": "FRED_SOURCE_UNAVAILABLE",
            }
        )
        return result

    upper_rows = [
        {"date": timeutil.parse_date(row["date"]), "value": float(row["value"])}
        for row in fred_history.get("upper", [])
    ]
    lower_rows = [
        {"date": timeutil.parse_date(row["date"]), "value": float(row["value"])}
        for row in fred_history.get("lower", [])
    ]

    for meeting in pending:
        meeting_date = meeting["meeting_date"]
        result["evaluated"] += 1
        verdict = resolve_actual_outcome(
            timeutil.parse_date(meeting_date), upper_rows, lower_rows
        )
        if verdict["resolvable"]:
            store.mark_resolved(
                meeting_date,
                verdict["outcome_bp"],
                "FRED DFEDTARU/DFEDTARL target-range change",
                detail=verdict["detail"],
                now=now,
            )
            result["resolved"].append(
                {"meeting_date": meeting_date, "actual_outcome_bp": verdict["outcome_bp"]}
            )
        else:
            store.mark_pending(
                meeting_date, verdict["reason"], detail=verdict["detail"], now=now
            )
            result["pending"].append(
                {"meeting_date": meeting_date, "reason": verdict["reason"]}
            )
    return result


def collect(
    store: FedwatchHistoryStore,
    data: dict,
    transport: Transport | None = None,
    clock=timeutil.utc_now,
) -> dict:
    """Record an accepted snapshot and advance the meeting lifecycle.

    The lifecycle is advanced *first*: a pending or newly resolved meeting is
    never given fresh live observations in the same run, so a meeting that has
    already passed cannot remain a live-collection target even for one cycle.
    """
    transport = transport or HttpTransport()
    now = clock()
    lifecycle = None
    errors: list[dict] = []
    pending = store.meetings_awaiting_resolution(now.date())
    if pending:
        fred_history = None
        try:
            fred_history = fred.fetch_target_history(transport, clock=clock)
        except FedwatchError as exc:
            errors.append(exc.to_dict())
        lifecycle = evaluate_lifecycle(store, fred_history, clock=clock)
        errors.extend(lifecycle.get("errors", []))
    report = record_snapshot(store, data, clock=clock)
    return {
        "db_path": str(store.path),
        "recorded": report,
        "lifecycle": lifecycle,
        "errors": errors,
    }


# ── Polymarket historical backfill ─────────────────────────────────────────


def normalize_backfill_points(points: list, now: datetime) -> tuple[list[dict], dict]:
    """Normalize CLOB history points to at most one accepted point per UTC day.

    Each kept point retains its exact source instant and its probability in
    percent; malformed, future-dated and out-of-range points are counted and
    dropped, never repaired. Same-day duplicates keep the latest instant, the
    same rule the qualified implementation used for the daily series.
    """
    by_day: dict[str, dict] = {}
    counts = {"malformed": 0, "future": 0, "out_of_range": 0}
    for point in points if isinstance(points, list) else []:
        if not isinstance(point, dict):
            counts["malformed"] += 1
            continue
        raw_t = point.get("t")
        raw_p = point.get("p")
        if isinstance(raw_t, bool) or isinstance(raw_p, bool):
            counts["malformed"] += 1
            continue
        try:
            timestamp = float(raw_t)
            probability = float(raw_p)
        except (TypeError, ValueError):
            counts["malformed"] += 1
            continue
        if not math.isfinite(timestamp) or not math.isfinite(probability) or timestamp < 0:
            counts["malformed"] += 1
            continue
        try:
            instant = datetime.fromtimestamp(timestamp, tz=timezone.utc)
        except (OverflowError, OSError, ValueError):
            counts["malformed"] += 1
            continue
        if instant > now + timedelta(seconds=FUTURE_TOLERANCE_SECONDS):
            counts["future"] += 1
            continue
        if not 0.0 <= probability <= 1.0:
            counts["out_of_range"] += 1
            continue
        day = instant.date().isoformat()
        previous = by_day.get(day)
        if previous is None or instant > previous["instant"]:
            by_day[day] = {"instant": instant, "probability": probability}
    normalized = [
        {
            "observed_at": timeutil.iso_z(entry["instant"]),
            "probability_pct": round(entry["probability"] * 100.0, 4),
        }
        for entry in sorted(by_day.values(), key=lambda entry: entry["instant"])
    ]
    return normalized, counts


def _backfill_state_is_fresh(state: dict | None, now: datetime, refresh_hours: float) -> bool:
    if state is None:
        return False
    last = _parse_instant(state.get("last_backfill_at"))
    if last is None:
        return False
    return (now - last).total_seconds() < refresh_hours * 3600.0


def backfill_polymarket(
    store: FedwatchHistoryStore,
    transport: Transport | None = None,
    clock=timeutil.utc_now,
    sleep=time.sleep,
    meeting_dates: list[str] | None = None,
    force: bool = False,
    refresh_hours: float = BACKFILL_REFRESH_HOURS,
) -> dict:
    """Backfill validated mappings' full CLOB history idempotently.

    A mapping already backfilled within ``refresh_hours`` is skipped; a
    resolved meeting is skipped permanently unless ``force`` is set, so
    reopening an old meeting never re-downloads its history. Deduplication is
    enforced both by the source-observation key and the unique per-point key, so a
    forced re-run inserts nothing new.
    """
    transport = transport or HttpTransport()
    now = clock()
    retrieved_at = timeutil.iso_z(now)
    mappings = store.validated_mappings(meeting_dates)
    result = {
        "retrieved_at": retrieved_at,
        "db_path": str(store.path),
        "backfills": [],
        "errors": [],
        "warnings": [],
    }
    if not mappings:
        result["warnings"].append(
            "no validated Polymarket mappings are stored; nothing to backfill"
        )
        return result

    meeting_cache: dict[str, dict | None] = {}
    for mapping in mappings:
        meeting_date = mapping["meeting_date"]
        if meeting_date not in meeting_cache:
            meeting_cache[meeting_date] = store.get_meeting(meeting_date)
        meeting = meeting_cache[meeting_date]
        token_id = mapping.get("external_token_id")
        entry_base = {
            "meeting_date": meeting_date,
            "event_id": mapping.get("external_event_id"),
            "outcome_bp": mapping["outcome_bp"],
            "open_ended": mapping["open_ended"],
            "token_id": token_id,
        }
        state = store.get_backfill_state(
            meeting_date, mapping["source"], mapping["method"],
            mapping["outcome_bp"], mapping["open_ended"],
        )
        resolution = meeting["status"] if meeting is not None else None
        state_matches_token = state is not None and state.get("token_id") == token_id
        state_is_terminal_success = (
            state_matches_token
            and state.get("status") in ("OK", "PARTIAL", "EMPTY")
        )
        if (
            not force
            and resolution != MEETING_STATUS_RESOLVED
            and mapping.get("last_revalidation_status") in ("NOT_FOUND", "AMBIGUOUS")
        ):
            result["backfills"].append({**entry_base, "status": "MAPPING_NOT_CURRENT"})
            continue
        if not force and resolution == MEETING_STATUS_RESOLVED and state_is_terminal_success:
            result["backfills"].append({**entry_base, "status": "SKIPPED_RESOLVED"})
            continue
        if (
            not force
            and state_matches_token
            and state is not None
            and state.get("status") not in ("PROVIDER_ERROR", "NO_TOKEN")
            and _backfill_state_is_fresh(state, now, refresh_hours)
        ):
            result["backfills"].append({**entry_base, "status": "SKIPPED_FRESH"})
            continue
        if not token_id:
            store.upsert_backfill_state(
                meeting_date, mapping["source"], mapping["method"], mapping["outcome_bp"],
                mapping["open_ended"], mapping.get("external_event_id"), None,
                mapping.get("external_market_id"), mapping.get("question"), None, 0, 0, 0,
                "NO_TOKEN", detail={"reason": "validated mapping has no token id"}, now=now,
            )
            result["backfills"].append({**entry_base, "status": "NO_TOKEN"})
            continue

        try:
            points = polymarket.fetch_price_history(
                transport,
                token_id,
                interval="max",
                fidelity=BACKFILL_FIDELITY_MINUTES,
                sleep=sleep,
            )
        except FedwatchError as exc:
            store.upsert_backfill_state(
                meeting_date, mapping["source"], mapping["method"], mapping["outcome_bp"],
                mapping["open_ended"], mapping.get("external_event_id"), token_id,
                mapping.get("external_market_id"), mapping.get("question"), None, 0, 0, 0,
                "PROVIDER_ERROR", detail={"error": exc.code}, now=now,
            )
            result["errors"].append(exc.to_dict())
            result["backfills"].append(
                {**entry_base, "status": "PROVIDER_ERROR", "error_code": exc.code}
            )
            continue

        normalized, counts = normalize_backfill_points(points, now)
        inserted = revised = duplicates = 0
        detail = {
            "origin": "clob_prices_history_backfill",
            "event_id": mapping.get("external_event_id"),
            "event_title": mapping.get("external_event_title"),
            "market_id": mapping.get("external_market_id"),
            "token_id": token_id,
            "question": mapping.get("question"),
            "open_ended": mapping["open_ended"],
        }
        for point in normalized:
            point_digest = content_digest(
                {
                    "event_id": mapping.get("external_event_id"),
                    "market_id": mapping.get("external_market_id"),
                    "token_id": token_id,
                    "outcome_bp": mapping["outcome_bp"],
                    "open_ended": mapping["open_ended"],
                    "probability_pct": point["probability_pct"],
                }
            )
            outcome = store.record_observation(
                meeting_date=meeting_date,
                source=mapping["source"],
                method=mapping["method"],
                outcome_bp=mapping["outcome_bp"],
                open_ended=mapping["open_ended"],
                probability_pct=point["probability_pct"],
                raw_probability_pct=point["probability_pct"],
                normalized_probability_pct=None,
                observed_at=point["observed_at"],
                source_observed_at=point["observed_at"],
                retrieved_at=retrieved_at,
                quality_status="BACKFILLED",
                freshness_status=None,
                digest=point_digest,
                detail=detail,
                recorded_at=now,
            )
            if outcome == "inserted":
                inserted += 1
            elif outcome == "revised":
                revised += 1
            else:
                duplicates += 1
        status = "OK" if normalized else "EMPTY"
        if counts["malformed"] or counts["future"] or counts["out_of_range"]:
            status = "PARTIAL"
            result["warnings"].append(
                f"meeting {meeting_date} outcome {mapping['outcome_bp']}bp: dropped "
                f"{counts['malformed']} malformed, {counts['future']} future-dated and "
                f"{counts['out_of_range']} out-of-range CLOB point(s)"
            )
            if not normalized:
                status = "EMPTY"
        store.upsert_backfill_state(
            meeting_date, mapping["source"], mapping["method"], mapping["outcome_bp"],
            mapping["open_ended"], mapping.get("external_event_id"), token_id,
            mapping.get("external_market_id"), mapping.get("question"),
            normalized[-1]["observed_at"] if normalized else None,
            len(normalized), inserted, duplicates + revised, status,
            detail={
                "point_counts": counts,
                "observations_inserted": inserted,
                "observations_revised": revised,
                "duplicates": duplicates,
            }, now=now,
        )
        result["backfills"].append(
            {
                **entry_base,
                "status": status,
                "points_seen": len(points) if isinstance(points, list) else 0,
                "points_accepted": len(normalized),
                "observations_inserted": inserted,
                "observations_revised": revised,
                "duplicates": duplicates,
                "first_point_observed_at": normalized[0]["observed_at"] if normalized else None,
                "last_point_observed_at": normalized[-1]["observed_at"] if normalized else None,
                "malformed_counts": counts,
            }
        )
    return result


# ── reconstructed ZQ observations ──────────────────────────────────────────


def record_zq_observations(
    store: FedwatchHistoryStore,
    watch_date: date,
    local_rows: list[dict],
    detail_base: dict | None = None,
    clock=timeutil.utc_now,
) -> dict:
    """Persist reconstructed local ZQ distributions as historical observations.

    Only the reconstructed observations are written; the user's raw ZQ files
    are never copied into MarketLab history. ``observed_at`` is the
    reconstruction's watch date, so multiple watch dates for one meeting form
    the historical evolution of the reconstructed method.
    """
    now = clock()
    retrieved_at = timeutil.iso_z(now)
    observed_at = f"{watch_date.isoformat()}T00:00:00Z"
    report = {"processed": 0, "counts": {}, "skipped": [], "records": []}
    for row in local_rows:
        meeting_date = row.get("meeting_date")
        outcome_bp = row.get("local_bp_change", row.get("outcome_bp"))
        probability = _finite_number(row.get("probability_pct"))
        if not meeting_date or probability is None or isinstance(outcome_bp, bool) or not isinstance(outcome_bp, int):
            report["skipped"].append(
                {
                    "meeting_date": meeting_date,
                    "reason": "MALFORMED_ZQ_OBSERVATION",
                    "detail": {"outcome_bp": outcome_bp, "probability_pct": row.get("probability_pct")},
                }
            )
            continue
        meeting_status = (
            MEETING_STATUS_PENDING
            if timeutil.parse_date(meeting_date) < now.date()
            else MEETING_STATUS_UPCOMING
        )
        store.upsert_meeting(meeting_date, default_status=meeting_status, now=now)
        digest = content_digest(
            {
                "method": FED_METHOD_ZQ,
                "watch_date": watch_date.isoformat(),
                "meeting_date": meeting_date,
                "outcome_bp": outcome_bp,
                "probability_pct": probability,
                "meeting_ordinal": row.get("meeting_ordinal"),
                "multi_meeting_month": row.get("multi_meeting_month"),
                "approximated_month_split": row.get("approximated_month_split"),
            }
        )
        detail = {
            "origin": "zq_reconstruction",
            "method": FED_METHOD_ZQ,
            "watch_date": watch_date.isoformat(),
            "meeting_ordinal": row.get("meeting_ordinal"),
            "multi_meeting_month": row.get("multi_meeting_month"),
            "approximated_month_split": row.get("approximated_month_split"),
        }
        if detail_base:
            detail.update(detail_base)
        result = store.record_observation(
            meeting_date=meeting_date,
            source=ZQ_SOURCE,
            method=FED_METHOD_ZQ,
            outcome_bp=outcome_bp,
            open_ended=False,
            probability_pct=probability,
            raw_probability_pct=None,
            normalized_probability_pct=None,
            observed_at=observed_at,
            source_observed_at=None,
            retrieved_at=retrieved_at,
            quality_status="RECONSTRUCTED",
            freshness_status="HISTORICAL",
            digest=digest,
            detail=detail,
            recorded_at=now,
        )
        report["processed"] += 1
        report["records"].append(result)
        report["counts"][result] = report["counts"].get(result, 0) + 1
    return report


# ── optional historical ZQ import ──────────────────────────────────────────


def import_zq(
    store: FedwatchHistoryStore,
    transport: Transport,
    data_dir,
    watch_dates: list[date],
    clock=timeutil.utc_now,
) -> dict:
    """Reconstruct ZQ observations for the watch dates and persist them.

    The user's ZQ files are read in place and never copied. The live FOMC
    calendar is the meeting reference (a stale fallback is refused) and FRED's
    full target-range history supplies each watch date's current range. Each
    watch date is independent: one insufficient date is reported without
    blocking the others.
    """
    contracts, contract_report = fedwatch_zq.load_contracts(data_dir)
    fomc_result = fomc.fetch_calendar(transport, clock=clock)
    if fomc_result.get("fallback_stale"):
        raise FedwatchError(
            PROVIDER_FOMC_CALENDAR,
            "FOMC_CALENDAR_FALLBACK_STALE",
            "live FOMC calendar unavailable and the tracked fallback snapshot is too "
            "old to establish the meeting schedule; ZQ reconstruction would use an "
            "unreliable meeting list",
            detail={
                "fallback_snapshot_retrieved_at": fomc_result.get(
                    "fallback_snapshot_retrieved_at"
                )
            },
        )
    meeting_end_dates = sorted(row["end_date"] for row in fomc_result["meetings"])
    warnings: list[str] = list(fomc_result.get("warnings") or [])
    fred_history = fred.fetch_target_history(transport, clock=clock)
    upper_rows = fred_history["upper"]
    lower_rows = fred_history["lower"]

    errors: list[dict] = []
    watch_results = []
    for watch_date in sorted(watch_dates):
        upper_before = [
            row for row in upper_rows
            if timeutil.parse_date(row["date"]) <= watch_date
        ]
        lower_before = [
            row for row in lower_rows
            if timeutil.parse_date(row["date"]) <= watch_date
        ]
        if not upper_before or not lower_before:
            errors.append(
                FedwatchError(
                    PROVIDER_ZQ,
                    "FEDWATCH_ZQ_RECONSTRUCTION_INCOMPLETE",
                    f"no FRED target-range observation exists on or before watch date "
                    f"{watch_date.isoformat()}",
                    detail={"watch_date": watch_date.isoformat()},
                ).to_dict()
            )
            continue
        current_range = {
            "upper": float(upper_before[-1]["value"]),
            "lower": float(lower_before[-1]["value"]),
            "observation_date": upper_before[-1]["date"],
        }
        try:
            deconvolution = fedwatch_zq.run_deconvolution(
                watch_date,
                meeting_end_dates,
                contracts,
                current_rate_upper=current_range["upper"],
                current_rate_lower=current_range["lower"],
            )
        except FedwatchError as exc:
            errors.append(exc.to_dict())
            continue
        local_rows = [row for row in deconvolution["rows"] if row["row_type"] == "local"]
        try:
            recorded = record_zq_observations(
                store,
                watch_date,
                local_rows,
                detail_base={
                    "current_target_range": current_range,
                    "contract_count": len(contracts),
                },
                clock=clock,
            )
        except FedwatchError as exc:
            errors.append(exc.to_dict())
            continue
        watch_results.append(
            {
                "watch_date": watch_date.isoformat(),
                "current_target_range": current_range,
                "deconvolution": deconvolution["report"],
                "local_row_count": len(local_rows),
                "cumulative_row_count": len(deconvolution["rows"]) - len(local_rows),
                "recorded": recorded,
            }
        )
        warnings.extend(deconvolution["report"].get("warnings") or [])

    lifecycle = evaluate_lifecycle(store, fred_history, clock=clock)
    errors.extend(lifecycle.get("errors") or [])
    return {
        "db_path": str(store.path),
        "data_dir": str(data_dir),
        "contract_report": contract_report,
        "watch_dates": watch_results,
        "lifecycle": lifecycle,
        "errors": errors,
        "warnings": warnings,
    }


# ── read surfaces ──────────────────────────────────────────────────────────


def series(
    store: FedwatchHistoryStore,
    meeting_date: str,
    method: str | None = None,
    outcome_bp: int | None = None,
    open_ended: bool | None = None,
) -> dict:
    """Stored accepted observations for one meeting (optionally filtered)."""
    rows = store.observations(
        meeting_date=meeting_date, method=method, outcome_bp=outcome_bp, open_ended=open_ended
    )
    errors: list[dict] = []
    valid: list[dict] = []
    for row in rows:
        if _finite_number(row["probability_pct"]) is None:
            errors.append(
                {
                    "error": "stored observation probability is not a finite number",
                    "provider": PROVIDER_HISTORY,
                    "code": "FEDWATCH_HISTORY_ROW_INVALID",
                    "detail": {"observation_id": row["id"]},
                }
            )
            continue
        valid.append(row)
    return {
        "meeting_date": meeting_date,
        "method": method,
        "outcome_bp": outcome_bp,
        "open_ended": open_ended,
        "observation_count": len(valid),
        "observations": valid,
        "errors": errors,
    }


def meetings_overview(store: FedwatchHistoryStore) -> dict:
    """Durable meeting lifecycle plus per-method observation coverage."""
    meetings = store.list_meetings()
    summaries = store.method_summary()
    by_meeting: dict[str, dict] = {}
    for summary in summaries:
        by_meeting.setdefault(summary["meeting_date"], {})[summary["method"]] = {
            "observations": summary["observations"],
            "accepted_count": summary["accepted_count"],
            "first_observed_at": summary["first_observed_at"],
            "last_observed_at": summary["last_observed_at"],
            "source": summary["source"],
        }
    mappings = store.validated_mappings()
    mapping_by_meeting: dict[str, dict] = {}
    for mapping in mappings:
        entry = mapping_by_meeting.setdefault(
            mapping["meeting_date"],
            {
                "mapping_status": "VALIDATED",
                "event_id": mapping.get("external_event_id"),
                "event_title": mapping.get("external_event_title"),
                "outcome_count": 0,
                "last_seen_at": mapping.get("last_seen_at"),
                "last_revalidation_status": mapping.get("last_revalidation_status"),
                "last_revalidated_at": mapping.get("last_revalidated_at"),
            },
        )
        entry["outcome_count"] += 1
    backfills = store.backfill_states()
    backfill_by_meeting: dict[str, dict] = {}
    for state in backfills:
        entry = backfill_by_meeting.setdefault(
            state["meeting_date"],
            {
                "outcomes": 0,
                "points": 0,
                "last_backfill_at": state.get("last_backfill_at"),
                "statuses": set(),
            },
        )
        entry["outcomes"] += 1
        entry["points"] += state.get("point_count") or 0
        if state.get("status"):
            entry["statuses"].add(state["status"])
    for entry in backfill_by_meeting.values():
        entry["statuses"] = sorted(entry["statuses"])

    out_meetings = []
    for meeting in meetings:
        meeting_date = meeting["meeting_date"]
        out_meetings.append(
            {
                **meeting,
                "observations": by_meeting.get(meeting_date, {}),
                "polymarket_mapping": mapping_by_meeting.get(meeting_date),
                "polymarket_backfill": backfill_by_meeting.get(meeting_date),
            }
        )
    return {
        "meeting_count": len(out_meetings),
        "db_path": str(store.path),
        "meetings": out_meetings,
    }
