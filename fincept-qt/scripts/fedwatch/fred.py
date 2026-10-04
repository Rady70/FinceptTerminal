"""FRED target-range context (DFEDTARU / DFEDTARL).

The Federal Funds target range is retrieved from FRED's public CSV endpoint
(``fredgraph.csv``) — no API key, no credentials. This is the same source and
the same parsing behavior the qualified component used, and it supplies the
current target-range midpoint the Fed-side local-step conversion is seeded
with.

FRED's bot protection tarpits browser-shaped requests from non-browser TLS
fingerprints, so the default ``requests`` User-Agent is used deliberately; do
not add a spoofed browser UA here.
"""

from __future__ import annotations

import csv
import math

from fedwatch import timeutil
from fedwatch.errors import PROVIDER_FRED, FedwatchError
from fedwatch.transport import Transport, TransportError

FRED_CSV_URL = "https://fred.stlouisfed.org/graph/fredgraph.csv?id={series_id}"
SOURCE_LABEL = "fred.stlouisfed.org DFEDTARU/DFEDTARL"
REQUEST_TIMEOUT = 15
FRESHNESS_MAX_AGE_DAYS = 3


def parse_fred_csv(text: str, report: dict | None = None, series_id: str | None = None) -> list[dict]:
    """Parse a FRED ``fredgraph.csv`` response into ``[{date, value}, ...]``.

    FRED writes ``.`` for missing observations (weekends and holidays in these
    daily series); those rows are counted separately. Malformed rows and
    conflicting dates are rejected independently and recorded in ``report``.
    Invalid document identity or no usable rows raises ``ValueError``.
    """
    lines = text.lstrip("\ufeff").splitlines()
    header_index = next((index for index, line in enumerate(lines) if line.strip()), len(lines))
    header = next(csv.reader(lines[header_index:header_index + 1]), [])
    header = [column.strip() for column in header]
    if len(header) < 2 or header[0].lower() not in ("date", "observation_date"):
        raise ValueError("FRED response has an invalid date/series header")
    if series_id and header.count(series_id) == 1:
        value_index = header.index(series_id)
    elif len(header) == 2 and header[1] and (not series_id or header[1] == "VALUE"):
        value_index = 1
    else:
        raise ValueError("FRED response does not identify one unambiguous requested series")
    quality = report if report is not None else {}
    quality.update(missing_row_count=0, rejected_rows=[], rejected_dates=[], latest_source_date=None,
                   trailing_undated_rejection=False)
    by_date, conflicts = {}, set()
    for line_number, line in enumerate(lines[header_index + 1:], start=header_index + 2):
        if not line.strip():
            continue
        quality["trailing_undated_rejection"] = False
        try:
            fields = next(csv.reader([line], strict=True), [])
        except csv.Error as exc:
            prefix = next(csv.reader([line.split(",", 1)[0]]), [])
            try:
                bad_date = timeutil.parse_date(prefix[0])
            except (ValueError, IndexError):
                bad_date = None
            iso = bad_date.isoformat() if bad_date else None
            quality["trailing_undated_rejection"] = iso is None
            quality["rejected_rows"].append({"row": line_number, "date": iso, "reason": str(exc)})
            if iso:
                quality["rejected_dates"].append(iso)
                if quality["latest_source_date"] is None or iso > quality["latest_source_date"]:
                    quality["latest_source_date"] = iso
            continue
        if not fields or not any(fields):
            continue
        day = None
        try:
            day = timeutil.parse_date(fields[0].strip())
            if len(fields) != len(header):
                raise ValueError("row columns do not match the declared header")
            value_text = fields[value_index].strip()
            if value_text in ("", "."):
                quality["missing_row_count"] += 1
                continue
            iso = day.isoformat()
            if quality["latest_source_date"] is None or iso > quality["latest_source_date"]:
                quality["latest_source_date"] = iso
            value = float(value_text)
            if not math.isfinite(value):
                raise ValueError("non-finite value")
            if day in by_date and by_date[day] != value:
                conflicts.add(day)
                raise ValueError("conflicting duplicate observation")
            by_date[day] = value
        except (ValueError, TypeError, IndexError) as exc:
            quality["trailing_undated_rejection"] = day is None
            quality["rejected_rows"].append({"row": line_number, "date": day.isoformat() if day else None,
                                             "reason": str(exc)})
            if day:
                quality["rejected_dates"].append(day.isoformat())
                iso = day.isoformat()
                if quality["latest_source_date"] is None or iso > quality["latest_source_date"]:
                    quality["latest_source_date"] = iso
    # A bad dated row makes that date ambiguous, even if another row is numeric.
    rejected_dates = set(quality["rejected_dates"])
    observations = [{"date": day, "value": value} for day, value in sorted(by_date.items())
                    if day not in conflicts and day.isoformat() not in rejected_dates]
    quality["rejected_row_count"] = len(quality["rejected_rows"])
    quality["accepted_row_count"] = len(observations)

    if not observations:
        raise ValueError("FRED response contains no numeric observations")
    return observations


def fetch_series(transport: Transport, series_id: str, report: dict | None = None) -> tuple[list[dict], str]:
    url = FRED_CSV_URL.format(series_id=series_id)
    try:
        text = transport.get_text(url, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_SOURCE_UNAVAILABLE",
            f"FRED {series_id} request failed: {exc}",
            detail=exc.detail(),
        ) from exc
    try:
        return parse_fred_csv(text, report=report, series_id=series_id), url
    except (ValueError, csv.Error) as exc:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            f"FRED {series_id} response could not be parsed: {exc}",
            detail={"series_id": series_id, "url": url, "parse_report": report or {}},
        ) from exc


def valid_target_range(upper, lower) -> bool:
    """The qualified target-range invariant ``upper > lower >= 0``, finite.

    Batch A's current-range read enforces this; the historical resolution and
    the ZQ import use the same check so a malformed provider pair can never
    become a durable decision or reconstruction input.
    """
    try:
        upper_value = float(upper)
        lower_value = float(lower)
    except (TypeError, ValueError):
        return False
    if not math.isfinite(upper_value) or not math.isfinite(lower_value):
        return False
    return upper_value > lower_value >= 0


def fetch_target_history(transport: Transport, clock=timeutil.utc_now) -> dict:
    """Fetch the full DFEDTARU/DFEDTARL daily series for historical decisions.

    Batch B uses this to establish resolved FOMC meetings' actual target-range
    change (the qualified FedWatch decision convention) without guessing from
    the meeting date. The series are returned chronologically with ISO dates
    and validated finite values; a malformed series is an explicit provider
    failure, never a silently empty history.
    """
    reports = {"upper": {}, "lower": {}}
    upper_rows, upper_url = fetch_series(transport, "DFEDTARU", reports["upper"])
    lower_rows, _lower_url = fetch_series(transport, "DFEDTARL", reports["lower"])
    today = clock().date()
    for bound, rows in (("upper", upper_rows), ("lower", lower_rows)):
        future = [row for row in rows if row["date"] > today]
        reports[bound]["rejected_rows"].extend({"date": row["date"].isoformat(), "reason": "future observation"} for row in future)
        reports[bound]["rejected_dates"].extend(row["date"].isoformat() for row in future)
        reports[bound]["rejected_row_count"] += len(future)
        rows[:] = [row for row in rows if row["date"] <= today]
        reports[bound]["accepted_row_count"] = len(rows)
    combined = upper_rows + lower_rows
    if not combined:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            "FRED target-range history is empty",
            detail={"url": upper_url},
        )
    invalid = [
        row for row in combined
        if not math.isfinite(row["value"])
    ]
    if invalid:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            "FRED target-range history contains a non-finite value",
            detail={"invalid_row_count": len(invalid)},
        )
    upper_rows.sort(key=lambda row: row["date"])
    lower_rows.sort(key=lambda row: row["date"])
    return {
        "retrieved_at": timeutil.iso_z(clock()),
        "source": SOURCE_LABEL,
        "method": "FRED target-range series (DFEDTARU/DFEDTARL)",
        "parse_reports": reports,
        "errors": _parse_errors(reports),
        "upper": [
            {"date": row["date"].isoformat(), "value": row["value"]} for row in upper_rows
        ],
        "lower": [
            {"date": row["date"].isoformat(), "value": row["value"]} for row in lower_rows
        ],
    }


def _parse_errors(reports: dict) -> list[dict]:
    return [FedwatchError(PROVIDER_FRED, "FRED_PARSE_PARTIAL", "FRED observations were rejected",
                         detail={"bound": bound, "parse_report": report}).to_dict()
            for bound, report in reports.items() if report.get("rejected_row_count")]


def fetch_target_range(transport: Transport, clock=timeutil.utc_now, calendar: dict | None = None) -> dict:
    """Fetch the current target range from DFEDTARU/DFEDTARL.

    Return the latest valid non-future common-date pair. Newer unpaired or
    rejected bounds leave this pair STALE. With a complete official calendar,
    a valid pair carries forward until the next decision. Otherwise the
    conservative three-day age limit applies.
    Consumers label an unverified stale seed and withhold the first local step
    if any readable calendar row establishes an intervening decision.
    """
    reports = {"upper": {}, "lower": {}}
    upper_rows, upper_url = fetch_series(transport, "DFEDTARU", reports["upper"])
    lower_rows, _lower_url = fetch_series(transport, "DFEDTARL", reports["lower"])
    retrieved_at = clock()

    upper_by_date = {row["date"]: row["value"] for row in upper_rows}
    lower_by_date = {row["date"]: row["value"] for row in lower_rows}
    paired = sorted(day for day in upper_by_date.keys() & lower_by_date.keys()
                    if day <= retrieved_at.date() and valid_target_range(upper_by_date[day], lower_by_date[day]))
    if not paired:
        raise FedwatchError(PROVIDER_FRED, "FRED_TARGET_RANGE_INVALID", "no valid non-future paired FRED range",
                            detail={"parse_reports": reports})
    latest_day = paired[-1]
    latest_upper = {"date": latest_day, "value": upper_by_date[latest_day]}
    latest_lower = {"date": latest_day, "value": lower_by_date[latest_day]}
    upper_value = latest_upper["value"]
    lower_value = latest_lower["value"]

    same_latest = upper_rows[-1]["date"] == lower_rows[-1]["date"]
    age_days = (retrieved_at.date() - latest_day).days
    calendar_complete = bool(calendar and calendar.get("coverage_complete", False) and
                             not calendar.get("fallback_stale", False))
    decision_since_pair = bool(calendar_complete and any(
        latest_day <= row["end_date"] < retrieved_at.date() for row in calendar["meetings"]))
    age_current = not decision_since_pair if calendar_complete else age_days <= FRESHNESS_MAX_AGE_DAYS
    status = "CURRENT" if (same_latest and latest_day == upper_rows[-1]["date"] and
                           age_current and
                           not any(r["trailing_undated_rejection"] for r in reports.values()) and
                           all(not r["latest_source_date"] or r["latest_source_date"] <= latest_day.isoformat()
                               for r in reports.values())) else "STALE"

    recent_observations = [
        {
            "date": upper_row["date"].isoformat(),
            "upper": upper_row["value"],
            "lower": lower_by_date[upper_row["date"]],
        }
        for upper_row in [{"date": day, "value": upper_by_date[day]} for day in paired[-5:]]
    ]

    return {
        "retrieved_at": timeutil.iso_z(retrieved_at),
        "source": SOURCE_LABEL,
        "method": "FRED target-range bounds",
        "target_range": {
            "lower": lower_value,
            "upper": upper_value,
        },
        "latest_observation_date": latest_upper["date"].isoformat(),
        "same_latest_date": same_latest,
        "status": status,
        "carried_forward": status == "CURRENT" and calendar_complete and age_days > 0,
        "calendar_coverage_complete": calendar_complete,
        "status_reason": ("CARRIED_FORWARD_NO_FOMC_DECISION" if calendar_complete and age_days > 0 else None)
                          if status == "CURRENT" else "FOMC_DECISION_SINCE_PAIR" if decision_since_pair else
                          "PAIR_TOO_OLD" if not age_current else "LAGGING_OR_REJECTED_BOUND",
        "age_days": age_days,
        "latest_available_bounds": {
            "upper": {"date": upper_rows[-1]["date"].isoformat(), "value": upper_rows[-1]["value"]},
            "lower": {"date": lower_rows[-1]["date"].isoformat(), "value": lower_rows[-1]["value"]},
        },
        "parse_reports": reports,
        "errors": _parse_errors(reports) + ([FedwatchError(PROVIDER_FRED, "FRED_TARGET_RANGE_STALE",
                    "retained latest valid common-date range does not meet currentness requirements",
                    detail={"latest_observation_date": latest_day.isoformat(), "age_days": age_days}).to_dict()] if status == "STALE" else []),
        "series": {
            "upper": {
                "series_id": "DFEDTARU",
                "observation_date": latest_upper["date"].isoformat(),
                "value": upper_value,
                "url": upper_url,
            },
            "lower": {
                "series_id": "DFEDTARL",
                "observation_date": latest_lower["date"].isoformat(),
                "value": lower_value,
                "url": FRED_CSV_URL.format(series_id="DFEDTARL"),
            },
        },
        "recent_observations": recent_observations,
    }
