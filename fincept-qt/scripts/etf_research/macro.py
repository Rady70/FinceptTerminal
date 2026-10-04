"""FRED and World Bank observations for the sector and country research models.

FRED goes through MarketLab's existing FedWatch transport and ``fredgraph.csv``
parser (the qualified, key-free FRED path; the default ``requests`` User-Agent
is used deliberately). World Bank uses the public v2 indicator API.

A FRED '.' (no observation that day) and a World Bank ``null`` are counted and
kept distinct from a reported number; neither ever becomes zero.

An uncertain row is rejected on its own, never the unrelated valid rows
(2026-10-04 data-preservation rule): a FRED line whose date or value cannot be
read, or a World Bank value that is not a number, is counted as unparseable,
kept as an example and left out, while the series keeps its other rows. FRED is
parsed here row by row for that reason; FedWatch's own ``parse_fred_csv`` (which
refuses a series with one bad row) is left unchanged for FedWatch. A paginated
World Bank response is followed page by page; a page that fails is named.
"""

from __future__ import annotations

import math

from fedwatch import timeutil
from fedwatch.fred import FRED_CSV_URL
from fedwatch.transport import HttpTransport, Transport, TransportError

WORLD_BANK_URL = "https://api.worldbank.org/v2/country/{codes}/indicator/{indicator}"
REQUEST_TIMEOUT = 25
WORLD_BANK_MAX_PAGES = 50
UNPARSEABLE_EXAMPLES = 3


def parse_fred_rows(text: str) -> dict:
    """``fredgraph.csv`` into rows [iso_date, value], row by row.

    Raises ``ValueError`` only when the response itself is unusable (empty, or
    no value column). A '.' or empty value is a missing observation (counted).
    A line whose date or value cannot be read is unparseable: counted, kept as
    an example, left out; the other rows stand.
    """
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        raise ValueError("FRED response is empty")
    header = [column.strip() for column in lines[0].split(",")]
    if len(header) < 2:
        raise ValueError(f"FRED response has no value column: {lines[0]!r}")
    rows, missing, bad = [], 0, []
    for line in lines[1:]:
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 2:
            bad.append(line)
            continue
        raw_date, raw_value = fields[0], fields[1]
        try:
            day = timeutil.parse_date(raw_date)
        except (ValueError, TypeError):
            bad.append(line)
            continue
        if raw_value in ("", "."):
            missing += 1
            continue
        try:
            value = float(raw_value)
        except ValueError:
            bad.append(line)
            continue
        if not math.isfinite(value):
            bad.append(line)
            continue
        rows.append([day.isoformat() if hasattr(day, "isoformat") else str(day), value])
    return {"rows": rows, "missing_points": missing, "unparseable_points": len(bad),
            "unparseable_examples": bad[:UNPARSEABLE_EXAMPLES]}


def fred_series(series_id: str, transport: Transport | None = None) -> dict:
    transport = transport or HttpTransport()
    url = FRED_CSV_URL.format(series_id=series_id)
    try:
        text = transport.get_text(url, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        return {"status": "FAILED", "detail": f"FRED {series_id} request failed: {exc}", "rows": [], "url": url}
    try:
        parsed = parse_fred_rows(text)
    except ValueError as exc:
        return {"status": "FAILED", "detail": f"FRED {series_id} response could not be parsed: {exc}", "rows": [],
                "url": url}
    if not parsed["rows"]:
        return {"status": "FAILED", "rows": [], "url": url,
                "detail": f"FRED {series_id} response contains no numeric observations"
                          f" ({parsed['unparseable_points']} unparseable line(s))",
                "unparseable_points": parsed["unparseable_points"],
                "unparseable_examples": parsed["unparseable_examples"]}
    detail = ""
    if parsed["unparseable_points"]:
        detail = (f"{parsed['unparseable_points']} unparseable line(s) left out, e.g. "
                  + "; ".join(parsed["unparseable_examples"]))
    return {"status": "OK", "detail": detail, "rows": parsed["rows"], "missing_points": parsed["missing_points"],
            "unparseable_points": parsed["unparseable_points"],
            "unparseable_examples": parsed["unparseable_examples"], "url": url, "text": text}


def parse_world_bank(payload) -> dict:
    """Parse a v2 indicator JSON response into rows [iso2, year, value|None]."""
    if not isinstance(payload, list) or len(payload) < 2:
        raise ValueError("World Bank response is not a [meta, rows] array")
    meta = payload[0] if isinstance(payload[0], dict) else {}
    if "message" in meta:
        raise ValueError(f"World Bank error: {meta.get('message')}")
    rows, bad = [], []
    for rec in payload[1] or []:
        country = (rec.get("country") or {}).get("id") or ""
        year = str(rec.get("date") or "")
        value = rec.get("value")
        if not country or not year.isdigit():
            continue
        if value is None:
            rows.append([country, int(year), None])  # the source's own "no value"
            continue
        try:
            number = float(value)
        except (TypeError, ValueError):
            number = None
        if number is None or not math.isfinite(number):
            bad.append(f"{country} {year}: {value!r}")
            continue
        rows.append([country, int(year), number])
    return {"rows": rows, "source_last_updated": str(meta.get("lastupdated") or ""), "pages": meta.get("pages"),
            "unparseable": bad}


def world_bank_indicator(indicator: str, codes, transport: Transport | None = None) -> dict:
    transport = transport or HttpTransport()
    url = WORLD_BANK_URL.format(codes=";".join(sorted(codes)), indicator=indicator)
    params = {"format": "json", "per_page": 2000, "date": "2005:2035"}
    try:
        payload = transport.get_json(url, params=params, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        return {"status": "FAILED", "detail": f"World Bank {indicator} request failed: {exc}", "rows": [],
                "url": url}
    try:
        parsed = parse_world_bank(payload)
    except ValueError as exc:
        return {"status": "FAILED", "detail": str(exc), "rows": [], "url": url}
    rows, bad = list(parsed["rows"]), list(parsed["unparseable"])
    pages_failed = []
    try:
        pages = int(parsed.get("pages") or 1)
    except (TypeError, ValueError):
        pages = 1
    # A paginated response is followed page by page (within a bound); a page
    # that fails is named, the pages read are kept.
    for page in range(2, min(pages, WORLD_BANK_MAX_PAGES) + 1):
        try:
            more = parse_world_bank(transport.get_json(url, params={**params, "page": page}, timeout=REQUEST_TIMEOUT))
        except (TransportError, ValueError) as exc:
            pages_failed.append(f"page {page}: {exc}")
            continue
        rows.extend(more["rows"])
        bad.extend(more["unparseable"])
    if pages > WORLD_BANK_MAX_PAGES:
        pages_failed.append(f"pages {WORLD_BANK_MAX_PAGES + 1}-{pages} not requested (bound {WORLD_BANK_MAX_PAGES})")
    returned = {r[0] for r in rows}
    absent = sorted(set(codes) - returned)
    notes = []
    if bad:
        notes.append(f"{len(bad)} unparseable value(s) left out, e.g. " + "; ".join(bad[:UNPARSEABLE_EXAMPLES]))
    if pages_failed:
        notes.append("incomplete: " + "; ".join(pages_failed))
    return {"status": "OK", "detail": " ".join(notes), "rows": rows, "url": url,
            "source_last_updated": parsed["source_last_updated"], "countries_absent": absent, "pages": pages,
            "unparseable_points": len(bad), "unparseable_examples": bad[:UNPARSEABLE_EXAMPLES],
            "pages_failed": pages_failed}
