"""FedWatch-specific acquisition: explicit writes, durable and network-free reads."""
from __future__ import annotations

import copy
import time
from datetime import timedelta

from fedwatch import analytics, history, snapshot, timeutil

CURRENT_REUSE_HOURS = 6


def _local_attempt(store, saved, now):
    """Qualify one retained attempt using its own clock and provider metadata."""
    result = copy.deepcopy(saved["envelope"]) if saved else {"success": True, "data": {"meetings": [], "errors": []}}
    data = result["data"]
    age = (now - timeutil.parse_iso_z(saved["acquired_at"])).total_seconds() / 3600 if saved else None
    data["acquisition"] = {"mode": "LOCAL", "acquired_at": saved["acquired_at"] if saved else None,
                           "age_hours": age, "network_requests": 0}
    data["acquisition"]["current_state"] = ("NOT_ACQUIRED" if saved is None else
                                           "STALE_RETAINED" if age < 0 or age > analytics.CURRENT_MAX_AGE_DAYS * 24 else
                                           "RETAINED_ATTEMPT")
    meetings = []
    for meeting in data.get("meetings", []):
        day = meeting["meeting_date"]
        retained_meeting = store.get_meeting(day)
        historical = (timeutil.parse_date(day) < now.date() or
                      (retained_meeting and retained_meeting["status"] in ("RESOLVED", "PENDING")))
        expired = age is None or age < 0 or age > analytics.CURRENT_MAX_AGE_DAYS * 24
        poly = meeting.get("polymarket")
        if poly and poly.get("data_status") == "CURRENT":
            oldest = (poly.get("freshness") or {}).get("oldest_outcome_timestamp")
            try:
                poly_expired = not oldest or (now - timeutil.parse_iso_z(oldest)) > timedelta(days=analytics.CURRENT_MAX_AGE_DAYS)
            except ValueError:
                poly_expired = True
            if expired or historical or poly_expired:
                poly["data_status"] = "STALE"
                meeting["comparison"] = []
        if historical:
            meeting["status"] = retained_meeting["status"] if retained_meeting else "PENDING"
            meeting["fed_side"] = None
            meeting["polymarket"] = None
            meeting["comparison"] = []
        elif expired:
            # Preserve raw accepted history in SQLite; stale cached values cannot
            # populate a consumer's current distribution.
            meeting["fed_side"] = None
            meeting["comparison"] = []
        meetings.append(meeting)
    data["meetings"] = meetings
    if age is None or age < 0 or age > analytics.CURRENT_MAX_AGE_DAYS * 24:
        data["current_target_range"] = None
    return result


def local_snapshot(store, meeting_date=None, clock=timeutil.utc_now):
    """Network-free reads; never infer current quotes from historical rows."""
    now = clock()
    if meeting_date:
        result = _local_attempt(store, store.current_acquisition(meeting_date), now)
    else:
        attempts = store.current_acquisitions()
        result = _local_attempt(store, None, now)
        data = result["data"]
        data.update(retrieved_at=None, sources=[], warnings=[], method_notes=[])
        data["acquisition"]["attempts"] = {}
        qualified = []
        for saved in attempts:
            local = _local_attempt(store, saved, now)
            current = local["data"]
            qualified.append(current)
            data["acquisition"]["attempts"][saved["meeting_date"]] = current["acquisition"]
            for meeting in current.get("meetings", []):
                meeting["acquisition"] = current["acquisition"]
                meeting["current_target_range"] = current.get("current_target_range")
                meeting["retrieved_at"] = current.get("retrieved_at")
                data["meetings"].append(meeting)
            data["errors"].extend(current.get("errors") or [])
            data["sources"].extend(dict(source, meeting_date=saved["meeting_date"]) for source in current.get("sources", []))
            data["warnings"].extend(current.get("warnings", []))
            data["method_notes"] = list(dict.fromkeys(data["method_notes"] + current.get("method_notes", [])))
        reports = [d["aggregate_acquisition"] for d in qualified if d.get("aggregate_acquisition")]
        if reports:
            # Shared last-unscoped diagnostics are not any selected meeting's
            # quote quality, and never participate in selected reuse decisions.
            data["aggregate_acquisition"] = max(reports, key=lambda r: r["acquired_at"])
        if attempts:
            # The overview exposes per-meeting acquisition timestamps. Its age
            # is conservative, so a new meeting cannot freshen an older quote.
            ages = [d["acquisition"]["age_hours"] for d in qualified]
            meta = data["acquisition"]
            meta["age_hours"] = min(ages) if min(ages) < 0 else max(ages)
            meta["current_state"] = ("STALE_RETAINED" if any(d["acquisition"]["current_state"] == "STALE_RETAINED" for d in qualified)
                                     else "RETAINED_ATTEMPT")
            targets = [d.get("current_target_range") for d in qualified]
            data["current_target_range"] = targets[0] if all(t == targets[0] for t in targets) else None
            result["partial"] = any(a["envelope"].get("partial") for a in attempts) or bool(data["errors"])
            result["failed_components"] = sorted({c for a in attempts for c in a["envelope"].get("failed_components", [])})
    result["data"]["history"] = history.meetings_overview(store)
    return result


def refresh_current(store, meeting_date=None, force=False, transport=None, clock=timeutil.utc_now, sleep=time.sleep):
    """Manual Refresh only. Reuse a complete recent acquisition across restarts."""
    now = clock()
    if meeting_date:
        day = timeutil.parse_date(meeting_date)
        meeting = store.get_meeting(meeting_date)
        if day < now.date() or (meeting and meeting["status"] in ("RESOLVED", "PENDING")):
            recorded = None
            if day < now.date() and meeting and meeting["status"] in ("UPCOMING", "PENDING"):
                # Explicit Refresh repairs the selected stale lifecycle using
                # official target history only, never obsolete current quotes.
                recorded = history.collect(store, {"retrieved_at": timeutil.iso_z(now), "meetings": []},
                                           transport=transport, clock=clock, meeting_date=meeting_date)
            result = local_snapshot(store, meeting_date, clock)
            if recorded is not None:
                result["data"]["history_collection"] = recorded
                saved = store.current_acquisition(meeting_date)
                # Lifecycle work must not freshen a retained probability quote.
                store.save_current_acquisition(result, saved["acquired_at"] if saved else timeutil.iso_z(now),
                                               meeting_date=meeting_date)
                # Here lifecycle evaluation IS the explicit operation. Report
                # its errors to the caller after storing independent quote quality.
                operation_errors = recorded.get("errors") or []
                result["data"]["errors"] = list(result["data"].get("errors") or []) + operation_errors
                result["partial"] = bool(result["data"]["errors"])
                result["failed_components"] = sorted({e["provider"] for e in result["data"]["errors"]})
                result["data"]["acquisition"]["operation"] = "LIFECYCLE_REFRESH"
            result["data"]["acquisition"]["reason"] = "HISTORICAL_MEETING"
            return result
    saved = store.current_acquisition(meeting_date)
    local = local_snapshot(store, meeting_date, clock)
    data = local["data"]
    age = data["acquisition"]["age_hours"]
    active_meetings = [m for m in data.get("meetings", []) if m.get("status") == "UPCOMING"]
    wanted_present = bool(active_meetings)
    quoted_usable = all(
        m.get("fed_side") and m["fed_side"].get("local_status") == "OK" and
        (m.get("polymarket") or {}).get("data_status") == "CURRENT" and
        (m.get("polymarket") or {}).get("mapping_status") == "VALIDATED"
        for m in active_meetings
    )
    # A selected attempt proves only that meeting's coverage. An unscoped
    # refresh must discover the complete upcoming inventory explicitly.
    if (meeting_date is not None and not force and saved and not local.get("partial") and not data.get("errors") and
            age is not None and 0 <= age < CURRENT_REUSE_HOURS and wanted_present and quoted_usable):
        data["acquisition"]["reason"] = "RECENT_VALID_ACQUISITION"
        return local
    result = snapshot.build_snapshot(transport, clock=clock, sleep=sleep,
                                     selected_meeting_dates=[timeutil.parse_date(meeting_date)] if meeting_date else None)
    data = result["data"]
    recorded = history.collect(store, data, transport=transport, clock=clock)
    data["history"] = recorded
    # Historical lifecycle failures remain visible and durable, separately
    # from quality errors needed to establish this current distribution.
    data["history_collection"] = recorded
    result["partial"] = bool(data["errors"])
    result["failed_components"] = sorted({e["provider"] for e in data["errors"]})
    store.save_current_acquisition(result, timeutil.iso_z(now), meeting_date=meeting_date)
    data["acquisition"] = {"mode": "MANUAL_REFRESH", "acquired_at": timeutil.iso_z(now), "age_hours": 0}
    return result
