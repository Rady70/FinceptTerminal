"""Explicit local import of CME-published TARGET RANGE probabilities.

Provisional: only synthetic fixtures have been consumed; zero native files.
This is a small CSV interchange, not a CME downloader or a parser claiming
native workbook qualification. Save permitted downloaded Excel data as the
documented five-column CSV; preserve the original file outside Git.
Published cumulative target bands are never called local meeting changes.
"""
from __future__ import annotations

import csv
import hashlib
import math
from pathlib import Path
from urllib.parse import urlparse

from fedwatch import fomc, history, timeutil
from fedwatch.errors import FedwatchError, PROVIDER_HISTORY
from fedwatch.store import content_digest
from fedwatch.row_dates import uncertainty_windows

METHOD = "HISTORICAL_CME_PUBLISHED_TARGET_RANGE"
SOURCE = "cme_published"
OBJECT = "TARGET_RANGE_UPPER_BP"
COLUMNS = ["meeting_date", "observation_date", "rate_low_bp", "rate_high_bp", "probability_pct"]
MAX_BYTES = 5_000_000
MAX_ROWS = 50_000
TOTAL_TOLERANCE_PP = 0.5


def invalid(message):
    return FedwatchError(PROVIDER_HISTORY, "FEDWATCH_PUBLISHED_HISTORY_INVALID", message)


def import_file(store, path, meeting_date, source_url, clock=timeutil.utc_now):
    """Validate independently by reporting date; retain complete valid dates."""
    now = clock()
    meeting_day = timeutil.parse_date(meeting_date)
    url = urlparse(source_url)
    if url.scheme != "https" or url.hostname not in ("www.cmegroup.com", "cmegroup.com", "cmegroup-tools.quikstrike.net"):
        raise invalid("source URL must identify the CME public FedWatch page/download")
    # A filename or a chosen UI date alone cannot establish meeting identity.
    calendar, captured = fomc.load_fallback_snapshot()
    known = {r["end_date"] for r in calendar}
    if meeting_day not in known and not (store.get_meeting(meeting_date) or {}).get("calendar"):
        raise invalid("meeting is absent from the retained official calendar")
    if meeting_day >= now.date() and meeting_day in known:
        age = (now - timeutil.parse_iso_z(captured)).total_seconds() / 86400 if captured else math.inf
        if not -1 <= age <= fomc.FALLBACK_MAX_AGE_DAYS and not (store.get_meeting(meeting_date) or {}).get("calendar"):
            raise invalid("future meeting identity needs a fresh retained official calendar")
    path = Path(path)
    try:
        with path.open("rb") as handle:
            raw = handle.read(MAX_BYTES + 1)
        if len(raw) > MAX_BYTES:
            raise invalid("CSV exceeds the bounded import size")
        text = raw.decode("utf-8-sig")
    except (OSError, UnicodeError) as exc:
        raise invalid(f"cannot read UTF-8 CSV: {exc}") from exc
    lines = text.splitlines()
    if next(csv.reader(lines[:1]), []) != COLUMNS:
        raise invalid(f"expected columns {COLUMNS!r}; native XLS/XLSX is not accepted")
    days, rejected_days, errors = {}, set(), []
    undated_rejections = []
    row_dates = []
    for index, line in enumerate(lines[1:]):
        if index >= MAX_ROWS:
            raise invalid("CSV exceeds the bounded row count")
        if not line.strip():
            continue
        try:
            fields = next(csv.reader([line], strict=True))
        except csv.Error as exc:
            # These date-only numeric rows have no supported multiline fields.
            # Recover a separately parseable date prefix to quarantine its day.
            prefix = next(csv.reader([",".join(line.split(",")[:2])]), [])
            try:
                bad_day = timeutil.parse_date(prefix[1].strip())
                rejected_days.add(bad_day.isoformat())
            except (ValueError, IndexError):
                bad_day = None
            if bad_day is None:
                undated_rejections.append(index + 2)
            row_dates.append((index + 2, bad_day))
            errors.append(FedwatchError(PROVIDER_HISTORY, "FEDWATCH_PUBLISHED_HISTORY_INVALID",
                f"row {index + 2}: {exc}", detail={"row": index + 2,
                "reporting_date": bad_day.isoformat() if bad_day else None}).to_dict())
            continue
        if not any(field.strip() for field in fields):
            continue
        row = dict(zip(COLUMNS, (field.strip() for field in fields)))
        day = None
        try:
            day = timeutil.parse_date(row["observation_date"])
            row_dates.append((index + 2, day))
            low, high = int(row["rate_low_bp"]), int(row["rate_high_bp"])
            probability = float(row["probability_pct"])
            if (row["meeting_date"] != meeting_date or len(fields) != len(COLUMNS) or
                    day > min(meeting_day, now.date()) or low < 0 or high - low != 25 or low % 25 or
                    not math.isfinite(probability) or not 0 <= probability <= 100):
                raise ValueError("invalid identity, date, target band or percent probability")
        except (ValueError, TypeError, KeyError) as exc:
            if day is not None:
                rejected_days.add(day.isoformat())
            else:
                undated_rejections.append(index + 2)
                row_dates.append((index + 2, None))
            errors.append(FedwatchError(PROVIDER_HISTORY, "FEDWATCH_PUBLISHED_HISTORY_INVALID",
                                       f"row {index + 2}: {exc}",
                                        detail={"row": index + 2, "reporting_date": day.isoformat() if day else None,
                                                **({"source_fields": fields} if day is None else {})}).to_dict())
            continue
        by_band = days.setdefault(day.isoformat(), {})
        if high in by_band and by_band[high] != (low, probability):
            rejected_days.add(day.isoformat())
            errors.append(invalid(f"{day}: conflicting duplicate date/band").to_dict())
        by_band[high] = (low, probability)
    if undated_rejections:
        ordered, windows = uncertainty_windows(row_dates)
        if not ordered:
            raise FedwatchError(PROVIDER_HISTORY, "FEDWATCH_PUBLISHED_HISTORY_DATE_UNCERTAIN",
                "unordered reporting dates cannot bound undated rows; retained history preserved",
                detail={"rows": undated_rejections, "errors": errors,
                        "input_sha256": hashlib.sha256(raw).hexdigest()})
        # Grouped date-ordered rows can lose buckets only in neighbour groups.
        neighbours = {day for window in windows for day in window["neighbour_dates"]}
        rejected_days.update(neighbours)
        errors.append(FedwatchError(PROVIDER_HISTORY, "FEDWATCH_PUBLISHED_HISTORY_DATE_UNCERTAIN",
            "undated rows quarantined to their neighbouring reporting dates",
            detail={"rows": undated_rejections, "rejected_reporting_dates": sorted(neighbours),
                    "date_uncertainty_windows": windows}).to_dict())
    if not days:
        raise invalid("CSV contains no observations; history remains missing")
    for day, bands in days.items():
        total = sum(p for _, p in bands.values())
        if abs(total - 100) > TOTAL_TOLERANCE_PP:
            rejected_days.add(day)
            errors.append(invalid(f"{day}: incomplete or invalid distribution (total {total} percent)").to_dict())
    retained = store.observations(meeting_date=meeting_date, method=METHOD, source=SOURCE)
    retained_bands = {}
    for row in retained:
        retained_bands.setdefault(row["observed_at"][:10], set()).add(row["outcome_bp"])
    for day, bands in days.items():
        if day in retained_bands and retained_bands[day] != set(bands):
            rejected_days.add(day)
            errors.append(invalid(f"{day}: revision changes the bucket set; retained history preserved for review").to_dict())
    days = {day: bands for day, bands in days.items() if day not in rejected_days}
    if not days:
        raise FedwatchError(PROVIDER_HISTORY, "FEDWATCH_PUBLISHED_HISTORY_INVALID",
                            "no complete valid reporting dates; retained history preserved",
                            detail={"errors": errors})
    # Every accepted source row retains its raw percent; only documented
    # rounding correction (<= 0.5 pp) produces the normalized plotting value.
    imported = timeutil.iso_z(now)
    input_digest = hashlib.sha256(raw).hexdigest()
    store.upsert_meeting(meeting_date, default_status="PENDING" if meeting_day < now.date() else "UPCOMING", now=now)
    counts = {}
    for day, bands in sorted(days.items()):
        total = sum(p for _, p in bands.values())
        # Same-band revisions use the existing store's digest/revision semantics.
        rows = []
        for high, (low, raw_probability) in sorted(bands.items()):
            probability = raw_probability * 100 / total
            detail = {"financial_object": OBJECT, "rate_low_bp": low, "rate_high_bp": high,
                      "source_url": source_url, "input_sha256": input_digest, "file_name": path.name,
                      "reporting_date": day, "timestamp_precision": "DATE_ONLY",
                      "timestamp_note": "UTC midnight is a date key, not a claimed publication instant",
                      "availability_timing": "UNKNOWN_INTRADAY; unsuitable for point-in-time backtesting",
                      "origin": "user_import", "provenance_status": "USER_DECLARED_CME_PUBLISHED",
                      "raw_total_pct": total, "normalization_factor": 100 / total}
            rows.append(dict(meeting_date=meeting_date, source=SOURCE, method=METHOD,
                             outcome_bp=high, open_ended=False, probability_pct=probability,
                             raw_probability_pct=raw_probability, normalized_probability_pct=probability,
                             observed_at=day + "T00:00:00Z", source_observed_at=None, retrieved_at=imported,
                             quality_status="PUBLISHED_USER_IMPORT", freshness_status="HISTORICAL",
                             digest=content_digest({"low": low, "high": high, "raw": raw_probability, "total": total}),
                             detail=detail, recorded_at=now))
        for row in rows:
            status = store.record_observation(**row)
            counts[status] = counts.get(status, 0) + 1
    return {"meeting_date": meeting_date, "method": METHOD, "source": SOURCE,
            "financial_object": OBJECT, "input_sha256": input_digest,
            "reporting_dates": len(days), "first_date": min(days), "last_date": max(days),
            "counts": counts, "network_requests": 0, "errors": errors,
            "rejected_reporting_dates": sorted(rejected_days), "partial": bool(errors)}


def compare_reconstruction(store, meeting_date):
    """Date/band matched published vs reconstructed cumulative probabilities.

    Never compare absolute rate buckets with local-change outcomes. Missing
    buckets remain None; date-only inputs cannot establish simultaneous quotes.
    """
    direct = store.observations(meeting_date=meeting_date, method=METHOD, source=SOURCE)
    reconstructed = store.observations(meeting_date=meeting_date, method=history.FED_METHOD_ZQ, source=history.ZQ_SOURCE)
    by_day = {}
    for row in direct:
        detail = row.get("detail") or {}
        by_day.setdefault(detail.get("reporting_date"), {})[(detail.get("rate_low_bp"), detail.get("rate_high_bp"))] = row["probability_pct"]
    cumulative = {}
    for row in reconstructed:
        detail = row.get("detail") or {}
        bands = detail.get("cumulative_distribution") or []
        if bands:
            cumulative[detail["watch_date"]] = {(round(b["rate_low"] * 100), round(b["rate_high"] * 100)): b["probability_pct"] for b in bands}
    comparisons = []
    for day in sorted(set(by_day) & set(cumulative)):
        for band in sorted(set(by_day[day]) | set(cumulative[day])):
            published = by_day[day].get(band)
            zq = cumulative[day].get(band)
            comparisons.append({"reporting_date": day, "rate_low_bp": band[0], "rate_high_bp": band[1],
                                "cme_published_pct": published, "zq_reconstructed_pct": zq,
                                "difference_pp": zq - published if published is not None and zq is not None else None})
    return {"meeting_date": meeting_date, "status": "OVERLAP" if comparisons else "NO_OVERLAP",
            "comparisons": comparisons, "timing_note": "Date matched, not necessarily simultaneous; publication timing unknown",
            "network_requests": 0, "errors": []}
