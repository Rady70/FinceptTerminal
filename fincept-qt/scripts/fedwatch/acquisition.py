"""FedWatch-specific acquisition: explicit writes, durable and network-free reads."""
from __future__ import annotations

import copy
import time
from datetime import timedelta

from fedwatch import analytics, history, snapshot, timeutil

CURRENT_REUSE_HOURS = 6


def local_snapshot(store, meeting_date=None, clock=timeutil.utc_now):
    """Opening saved research never constructs a provider transport."""
    now = clock()
    saved = store.current_acquisition()
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
        if meeting_date and day != meeting_date:
            continue
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
    data["history"] = history.meetings_overview(store)
    if age is None or age < 0 or age > analytics.CURRENT_MAX_AGE_DAYS * 24:
        data["current_target_range"] = None
    return result


def refresh_current(store, meeting_date=None, force=False, transport=None, clock=timeutil.utc_now, sleep=time.sleep):
    """Manual Refresh only. Reuse a complete recent acquisition across restarts."""
    now = clock()
    if meeting_date:
        day = timeutil.parse_date(meeting_date)
        meeting = store.get_meeting(meeting_date)
        if day < now.date() or (meeting and meeting["status"] in ("RESOLVED", "PENDING")):
            result = local_snapshot(store, meeting_date, clock)
            result["data"]["acquisition"]["reason"] = "HISTORICAL_MEETING"
            return result
    saved = store.current_acquisition()
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
    if (not force and saved and not local.get("partial") and not data.get("errors") and
            age is not None and 0 <= age < CURRENT_REUSE_HOURS and wanted_present and quoted_usable):
        data["acquisition"]["reason"] = "RECENT_VALID_ACQUISITION"
        return local
    result = snapshot.build_snapshot(transport, clock=clock, sleep=sleep,
                                     selected_meeting_dates=[timeutil.parse_date(meeting_date)] if meeting_date else None)
    data = result["data"]
    recorded = history.collect(store, data, transport=transport, clock=clock)
    data["history"] = recorded
    data["errors"] = list(data.get("errors") or []) + list(recorded.get("errors") or [])
    result["partial"] = bool(data["errors"])
    result["failed_components"] = sorted({e["provider"] for e in data["errors"]})
    store.save_current_acquisition(result, timeutil.iso_z(now))
    data["acquisition"] = {"mode": "MANUAL_REFRESH", "acquired_at": timeutil.iso_z(now), "age_hours": 0}
    return result
