"""Explicit local import of CME-published TARGET RANGE probabilities.

This is a small CSV interchange, not a CME downloader or a parser claiming
native workbook qualification. Save permitted downloaded Excel data as the
documented five-column CSV; preserve the original file outside Git.
Published cumulative target bands are never called local meeting changes.
"""
from __future__ import annotations

import csv
import hashlib
import io
import math
from pathlib import Path
from urllib.parse import urlparse

from fedwatch import fomc, history, timeutil
from fedwatch.errors import FedwatchError, PROVIDER_HISTORY
from fedwatch.store import content_digest

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
    """Validate the entire input before writing. Reimports use store revision rules."""
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
    reader = csv.DictReader(io.StringIO(text))
    if reader.fieldnames != COLUMNS:
        raise invalid(f"expected columns {COLUMNS!r}; native XLS/XLSX is not accepted")
    days = {}
    for index, row in enumerate(reader):
        if index >= MAX_ROWS:
            raise invalid("CSV exceeds the bounded row count")
        try:
            day = timeutil.parse_date(row["observation_date"])
            low, high = int(row["rate_low_bp"]), int(row["rate_high_bp"])
            probability = float(row["probability_pct"])
            if (row["meeting_date"] != meeting_date or None in row or
                    day > min(meeting_day, now.date()) or low < 0 or high - low != 25 or low % 25 or
                    not math.isfinite(probability) or not 0 <= probability <= 100):
                raise ValueError("invalid identity, date, target band or percent probability")
        except (ValueError, TypeError, KeyError) as exc:
            raise invalid(f"row {index + 2}: {exc}") from exc
        by_band = days.setdefault(day.isoformat(), {})
        if high in by_band and by_band[high] != (low, probability):
            raise invalid("conflicting duplicate date/band inside one input; supply an unambiguous revision")
        by_band[high] = (low, probability)
    if not days:
        raise invalid("CSV contains no observations; history remains missing")
    for day, bands in days.items():
        total = sum(p for _, p in bands.values())
        if abs(total - 100) > TOTAL_TOLERANCE_PP:
            raise invalid(f"{day}: incomplete or invalid distribution (total {total} percent)")
    retained = store.observations(meeting_date=meeting_date, method=METHOD, source=SOURCE)
    retained_bands = {}
    for row in retained:
        retained_bands.setdefault(row["observed_at"][:10], set()).add(row["outcome_bp"])
    for day, bands in days.items():
        if day in retained_bands and retained_bands[day] != set(bands):
            raise invalid(f"{day}: revision changes the bucket set; retained history preserved for review")
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
            "counts": counts, "network_requests": 0, "errors": []}


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
