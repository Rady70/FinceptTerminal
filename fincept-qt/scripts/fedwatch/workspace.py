"""One network-free read model for the FedWatch research workspace.

The panel opens with everything it needs from local storage in a single call:
every stored meeting, the latest accepted Fed-side target-range distribution
and Polymarket outcome prices with explicit CURRENT / STALE / UNAVAILABLE
states, daily history for both sources, the approved per-outcome change
calculations, and source/as-of provenance.

Rules kept from the acquisition and analytics layers:

* "current" values come only from the retained acquisition through
  :func:`acquisition.local_snapshot`; anything older is the latest *stored*
  observation and is labelled STALE with its own timestamp, never current;
* per-outcome Polymarket points and Fed-side local changes come from
  :func:`analytics.compute_analytics` (token generations and tails resolved
  there, never reconstructed here);
* history is reported by UTC day using the latest accepted observation of the
  day; missing days stay missing and nothing is interpolated;
* nothing derived is written back to storage.
"""
from __future__ import annotations

from datetime import date

from fedwatch import acquisition, analytics, history, polymarket, timeutil

CME_NOTE = (
    "CME FedWatch probabilities are not collected: CME's website and data terms prohibit scripts, "
    "robots and other automated access to cmegroup.com data, and the licensed FedWatch API is paid "
    "(outside the owner's free-data policy). The Fed-side distribution here comes from Investing.com's "
    "Fed Rate Monitor, which is computed from CME 30-Day Fed Funds futures; the futures price it "
    "displays for each meeting is shown as published. Open the official CME tool in a browser for "
    "personal viewing."
)


METHOD_NOTES = [
    "Fed-side: Investing.com's Fed Rate Monitor publishes rounded probabilities of each target range by meeting, "
    "computed from CME 30-Day Fed Funds futures. MarketLab normalizes each meeting to 100% and derives the "
    "meeting's local step (change at that meeting) with the CME integer/mantissa split. This is not official CME "
    "FedWatch output.",
    "Expected rate = probability-weighted midpoint of the target ranges after a meeting. Previous-day and "
    "previous-week values are as displayed by Investing.com, not MarketLab observations.",
    "The Fed-side local step is binary (at most two adjacent outcomes) while Polymarket prices broader tails, so "
    "Polymarket - Fed-side gaps can reflect both disagreement and model shape. They are descriptive, never "
    "trading signals.",
    "Values older than the current-freshness window are shown with their own timestamp and marked stale; they are "
    "never presented as current. History uses the latest accepted observation of each UTC day; missing days stay "
    "empty and nothing is interpolated. A 1D/7D/30D change needs an observation at or before the lookback.",
    "Read-only research: no broker, wallet, order or execution path exists in FedWatch.",
]


def _band_key(row):
    return (round(float(row["rate_low"]), 6), round(float(row["rate_high"]), 6))


def _expected(distribution):
    total = sum(item["probability_pct"] for item in distribution)
    if not distribution or total <= 0:
        return None
    return sum((item["rate_low"] + item["rate_high"]) / 2.0 * item["probability_pct"] / total
               for item in distribution)


def _distribution_from_section(fed):
    """Normalized bands with raw and Investing-displayed previous values."""
    raw = {_band_key(row): row.get("probability_pct") for row in fed.get("raw_probabilities") or []}
    previous = ((fed.get("displayed_context") or {}).get("previous") or {})
    prior = {}
    for horizon in ("previous_day", "previous_week"):
        for row in (previous.get(horizon) or {}).get("probabilities") or []:
            prior.setdefault(_band_key(row), {})[horizon + "_pct"] = row.get("probability_pct")
    rows = []
    for row in fed.get("normalized_probabilities") or []:
        key = _band_key(row)
        rows.append({"rate_low": key[0], "rate_high": key[1], "probability_pct": row["probability_pct"],
                     "raw_probability_pct": raw.get(key),
                     "previous_day_pct": prior.get(key, {}).get("previous_day_pct"),
                     "previous_week_pct": prior.get(key, {}).get("previous_week_pct")})
    # Buckets listed only in the table's previous columns are not part of the
    # current distribution; they stay in the previous-horizon expected rates.
    rows.sort(key=lambda row: row["rate_low"])
    return rows, previous


def _fed_entry(fed, state, observed_at):
    context = fed.get("displayed_context") or {}
    distribution, previous = _distribution_from_section(fed)
    normalization = fed.get("normalization") or {}
    expected = normalization.get("normalized_expected_rate")
    return {
        "state": state,
        "observed_at": observed_at,
        "source_updated_at": fed.get("source_timestamp"),
        "freshness_status": (fed.get("freshness") or {}).get("status"),
        "distribution": distribution,
        "expected_rate": expected if expected is not None else _expected(distribution),
        "expected_rate_previous_day": (previous.get("previous_day") or {}).get("normalized_expected_rate"),
        "expected_rate_previous_week": (previous.get("previous_week") or {}).get("normalized_expected_rate"),
        "previous_status": previous.get("status", "UNAVAILABLE"),
        "futures_price": context.get("futures_price"),
        "futures_implied_rate": context.get("futures_implied_rate"),
        "local": fed.get("local_probabilities"),
        "local_status": fed.get("local_status"),
        "copy_conflict": bool(fed.get("copy_conflict")),
        "target_range_unverified": bool(fed.get("target_range_unverified")),
    }


def _stored_fed_groups(store, meeting_date):
    """Accepted target-range distributions keyed by their effective instant."""
    groups = {}
    for row in store.observations(meeting_date=meeting_date, method=history.FED_METHOD_LIVE_TARGET):
        detail = row.get("detail") or {}
        instant = row["observed_at"]
        group = groups.setdefault(instant, {"observed_at": row["retrieved_at"], "source_updated_at":
                                            row.get("source_observed_at"), "bands": {}, "detail": detail,
                                            "freshness_status": row.get("freshness_status")})
        group["observed_at"] = max(group["observed_at"], row.get("last_retrieved_at") or row["retrieved_at"])
        group["bands"][_band_key(detail)] = {
            "rate_low": detail["rate_low"], "rate_high": detail["rate_high"],
            "probability_pct": row["probability_pct"], "raw_probability_pct": row.get("raw_probability_pct"),
            "previous_day_pct": detail.get("previous_day_pct"), "previous_week_pct": detail.get("previous_week_pct")}
    # Observations recorded before target-range rows existed keep the same
    # distribution in each local row's detail; use it where no band rows exist.
    for row in store.observations(meeting_date=meeting_date, method=history.FED_METHOD_LIVE):
        detail = row.get("detail") or {}
        instant = detail.get("source_timestamp") or row["observed_at"]
        if instant in groups or not detail.get("normalized_probabilities"):
            continue
        distribution, _ = _distribution_from_section(detail)
        groups[instant] = {"observed_at": row["retrieved_at"], "source_updated_at": detail.get("source_timestamp"),
                           "bands": {(r["rate_low"], r["rate_high"]): r for r in distribution}, "detail": detail,
                           "freshness_status": row.get("freshness_status"),
                           "expected_rate": (detail.get("normalization") or {}).get("normalized_expected_rate"),
                           "local": detail.get("local_probabilities")}
    out = []
    for instant, group in groups.items():
        distribution = sorted(group["bands"].values(), key=lambda r: r["rate_low"])
        expected = group.get("expected_rate")
        if expected is None:
            expected = group["detail"].get("normalized_expected_rate")
        out.append({"instant": instant, "observed_at": group["observed_at"],
                    "source_updated_at": group["source_updated_at"], "distribution": distribution,
                    "expected_rate": expected if expected is not None else _expected(distribution),
                    "futures_price": group["detail"].get("futures_price"),
                    "freshness_status": group["freshness_status"]})
    out.sort(key=lambda g: g["instant"])
    return out


def _daily(groups):
    by_day = {}
    for group in groups:
        instant = timeutil.parse_iso_z(group["instant"])
        by_day[instant.date().isoformat()] = dict(group, date=instant.date().isoformat())
    return [by_day[day] for day in sorted(by_day)]


def _outcome_key(row):
    return (int(row["outcome_bp"]), bool(row.get("open_ended")))


def _trim_side(side):
    latest = side.get("latest") or {}
    changes = side.get("changes") or {}
    return {
        "state": side.get("state"),
        "latest_pct": latest.get("probability_pct"),
        "latest_observed_at": latest.get("observed_at"),
        "change_1d_pp": (changes.get("1d") or {}).get("change_pp"),
        "change_7d_pp": (changes.get("7d") or {}).get("change_pp"),
        "change_30d_pp": (changes.get("30d") or {}).get("change_pp"),
        # The approved lookback uses the latest observation at or before the
        # lookback instant, which can be older; its time is reported with it.
        "reference_1d_at": (changes.get("1d") or {}).get("reference_observed_at"),
        "reference_7d_at": (changes.get("7d") or {}).get("reference_observed_at"),
        "reference_30d_at": (changes.get("30d") or {}).get("reference_observed_at"),
        "change_since_first_pp": (side.get("change_since_first_observation") or {}).get("change_pp"),
        "observed_high_pct": (side.get("observed_high") or {}).get("probability_pct"),
        "observed_low_pct": (side.get("observed_low") or {}).get("probability_pct"),
        "first_observed_at": side.get("first_observed_at"),
        "observation_count": side.get("observation_count"),
    }


def _polymarket_entry(poly, stored_mapping):
    mapping_status = (poly or {}).get("mapping_status") or ((stored_mapping or {}).get("mapping_status"))
    entry = {"mapping_status": mapping_status or "NOT_VERIFIED",
             "event_id": (poly or {}).get("event_id") or (stored_mapping or {}).get("event_id"),
             "event_title": (poly or {}).get("event_title") or (stored_mapping or {}).get("event_title"),
             "state": "UNAVAILABLE", "observed_at": None, "outcomes": []}
    if poly and poly.get("data_status") == "CURRENT" and poly.get("mapping_status") == "VALIDATED":
        entry.update(state="CURRENT", observed_at=poly.get("source_timestamp"),
                     outcomes=[{"outcome_bp": o["outcome_bp"], "open_ended": bool(o["open_ended"]),
                                "probability_pct": o.get("probability_pct"), "observed_at": o.get("source_timestamp")}
                               for o in poly.get("outcomes") or [] if o.get("probability_pct") is not None])
    return entry


def build(store, clock=timeutil.utc_now) -> dict:
    now = clock()
    today = timeutil.eastern_date_from_utc(now)
    local = acquisition.local_snapshot(store, clock=clock)
    data = local["data"]
    overview = data.get("history") or history.meetings_overview(store)
    current_by_date = {m["meeting_date"]: m for m in data.get("meetings") or []}
    meetings = []
    for stored in overview["meetings"]:
        day = stored["meeting_date"]
        current = current_by_date.get(day) or {}
        calendar = stored.get("calendar") or current.get("fomc_calendar") or {}
        meeting_day = timeutil.parse_date(day)
        upcoming = stored["status"] == "UPCOMING" and meeting_day >= today
        entry = {
            "meeting_date": day,
            "status": stored["status"],
            "start_date": calendar.get("start_date"),
            "has_projection_materials": calendar.get("has_projection_materials"),
            "days_until": (meeting_day - today).days,
            "actual_outcome_bp": stored.get("actual_outcome_bp"),
            "resolved_at": stored.get("resolved_at"),
            "status_reason": stored.get("status_reason"),
        }
        groups = _stored_fed_groups(store, day)
        fed_current = current.get("fed_side")
        if fed_current and upcoming and fed_current.get("normalized_probabilities"):
            freshness = (fed_current.get("freshness") or {}).get("status")
            state = "STALE" if freshness == "STALE" else "CURRENT"
            entry["fed"] = _fed_entry(fed_current, state, current.get("retrieved_at"))
        elif groups:
            latest = groups[-1]
            entry["fed"] = {"state": "STALE" if upcoming else "HISTORICAL", "observed_at": latest["observed_at"],
                            "source_updated_at": latest["source_updated_at"],
                            "freshness_status": latest["freshness_status"], "distribution": latest["distribution"],
                            "expected_rate": latest["expected_rate"], "expected_rate_previous_day": None,
                            "expected_rate_previous_week": None, "previous_status": "NOT_CURRENT",
                            "futures_price": latest.get("futures_price"),
                            "futures_implied_rate": (round(100 - latest["futures_price"], 6)
                                                     if latest.get("futures_price") is not None else None),
                            "local": None, "local_status": None, "copy_conflict": False,
                            "target_range_unverified": False}
        else:
            entry["fed"] = {"state": "UNAVAILABLE", "distribution": [], "local": None}
        if entry["fed"].get("observed_at"):
            age = now - timeutil.parse_iso_z(entry["fed"]["observed_at"])
            entry["fed"]["age_days"] = round(age.total_seconds() / 86400.0, 6)

        poly = _polymarket_entry(current.get("polymarket") if upcoming else None, stored.get("polymarket_mapping"))
        outcomes = set()
        for row in (entry["fed"].get("local") or []):
            outcomes.add(_outcome_key(row))
        for row in ((stored.get("polymarket_mapping") or {}).get("outcomes") or []):
            outcomes.add(_outcome_key(row))
        for row in poly["outcomes"]:
            outcomes.add(_outcome_key(row))
        observations = stored.get("observations") or {}
        if history.FED_METHOD_LIVE in observations:
            # Every stored local change at this meeting, not only today's pair.
            for row in store.observations(meeting_date=day, method=history.FED_METHOD_LIVE):
                outcomes.add((int(row["outcome_bp"]), False))
        changes, poly_points = [], {}
        for outcome_bp, open_ended in sorted(outcomes, key=lambda k: (k[0], k[1])):
            result = analytics.compute_analytics(store, day, outcome_bp, open_ended=open_ended, as_of=now)
            fed_side, poly_side, difference = result["fed_side"], result["polymarket"], result["difference"]
            changes.append({"outcome_bp": outcome_bp, "open_ended": open_ended,
                            "fed": _trim_side(fed_side), "polymarket": _trim_side(poly_side),
                            "current_difference_pp": difference.get("current_probability_diff_pp"),
                            "difference_state": difference.get("current_state")})
            for point in poly_side["history"]:
                instant = timeutil.parse_iso_z(point["observed_at"])
                key = instant.date().isoformat()
                bucket = poly_points.setdefault(key, {})
                previous = bucket.get((outcome_bp, open_ended))
                if previous is None or point["observed_at"] > previous["observed_at"]:
                    bucket[(outcome_bp, open_ended)] = {"outcome_bp": outcome_bp, "open_ended": open_ended,
                                                        "probability_pct": point["probability_pct"],
                                                        "observed_at": point["observed_at"],
                                                        "quality_status": point.get("quality_status")}
        if poly["state"] != "CURRENT" and poly_points:
            latest_day = max(poly_points)
            latest = sorted(poly_points[latest_day].values(), key=lambda r: (r["outcome_bp"], r["open_ended"]))
            poly.update(state="STALE" if upcoming else "HISTORICAL",
                        observed_at=max(r["observed_at"] for r in latest), outcomes=latest)
        mapped = {_outcome_key(row) for row in ((stored.get("polymarket_mapping") or {}).get("outcomes") or [])}
        entry["polymarket"] = poly
        entry["comparison"] = current.get("comparison") or [] if upcoming else []
        entry["history"] = {
            "fed_days": [{"date": g["date"], "observed_at": g["observed_at"], "source_updated_at": g["source_updated_at"],
                          "distribution": [{k: r[k] for k in ("rate_low", "rate_high", "probability_pct")}
                                           for r in g["distribution"]],
                          "expected_rate": g["expected_rate"]} for g in _daily(groups)],
            "polymarket_days": [{"date": d, "outcomes": sorted(points.values(), key=lambda r: (r["outcome_bp"], r["open_ended"])),
                                 "complete": bool(mapped) and mapped <= set(points)}
                                for d, points in sorted(poly_points.items())],
            "changes": changes,
        }
        meetings.append(entry)

    upcoming_days = [m for m in meetings if m["status"] == "UPCOMING" and m["days_until"] >= 0]
    return {
        "generated_at": timeutil.iso_z(now),
        "network_requests": 0,
        "today": today.isoformat(),
        "current_target_range": data.get("current_target_range"),
        "acquisition": data.get("acquisition"),
        "last_refresh_at": max((a.get("acquired_at") for a in ((data.get("acquisition") or {}).get("attempts") or {}).values()
                                if a.get("acquired_at")), default=None),
        "next_meeting_date": upcoming_days[0]["meeting_date"] if upcoming_days else None,
        "meetings": meetings,
        "sources": data.get("sources") or [],
        "errors": data.get("errors") or [],
        "warnings": data.get("warnings") or [],
        "method_notes": list(METHOD_NOTES),
        "cme": {"collected": False, "note": CME_NOTE, "public_tool_url": "https://www.cmegroup.com/fedwatch"},
        "polymarket_source": polymarket.SOURCE_LABEL,
    }
