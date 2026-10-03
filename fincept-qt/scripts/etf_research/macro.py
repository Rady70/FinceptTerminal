"""FRED and World Bank observations for the sector and country research models.

FRED goes through MarketLab's existing FedWatch transport and ``fredgraph.csv``
parser (the qualified, key-free FRED path; the default ``requests`` User-Agent
is used deliberately). World Bank uses the public v2 indicator API.

A FRED '.' (no observation that day) and a World Bank ``null`` are counted and
kept distinct from a reported number; neither ever becomes zero.
"""

from __future__ import annotations

from fedwatch.fred import FRED_CSV_URL, parse_fred_csv
from fedwatch.transport import HttpTransport, Transport, TransportError

WORLD_BANK_URL = "https://api.worldbank.org/v2/country/{codes}/indicator/{indicator}"
REQUEST_TIMEOUT = 25


def _count_fred_missing(text: str) -> int:
    missing = 0
    for line in text.splitlines()[1:]:
        fields = [f.strip() for f in line.split(",")]
        if len(fields) >= 2 and fields[1] in ("", "."):
            missing += 1
    return missing


def fred_series(series_id: str, transport: Transport | None = None) -> dict:
    transport = transport or HttpTransport()
    url = FRED_CSV_URL.format(series_id=series_id)
    try:
        text = transport.get_text(url, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        return {"status": "FAILED", "detail": f"FRED {series_id} request failed: {exc}", "rows": [], "url": url}
    try:
        obs = parse_fred_csv(text)
    except ValueError as exc:
        return {"status": "FAILED", "detail": f"FRED {series_id} response could not be parsed: {exc}", "rows": [],
                "url": url}
    rows = [[o["date"].isoformat() if hasattr(o["date"], "isoformat") else str(o["date"]), float(o["value"])]
            for o in obs]
    return {"status": "OK", "detail": "", "rows": rows, "missing_points": _count_fred_missing(text), "url": url,
            "text": text}


def parse_world_bank(payload) -> dict:
    """Parse a v2 indicator JSON response into rows [iso2, year, value|None]."""
    if not isinstance(payload, list) or len(payload) < 2:
        raise ValueError("World Bank response is not a [meta, rows] array")
    meta = payload[0] if isinstance(payload[0], dict) else {}
    if "message" in meta:
        raise ValueError(f"World Bank error: {meta.get('message')}")
    rows = []
    for rec in payload[1] or []:
        country = (rec.get("country") or {}).get("id") or ""
        year = str(rec.get("date") or "")
        value = rec.get("value")
        if not country or not year.isdigit():
            continue
        rows.append([country, int(year), None if value is None else float(value)])
    return {"rows": rows, "source_last_updated": str(meta.get("lastupdated") or ""), "pages": meta.get("pages")}


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
    if parsed.get("pages") not in (None, 1):
        return {"status": "FAILED", "detail": "World Bank response was paginated; refusing a partial series",
                "rows": [], "url": url}
    returned = {r[0] for r in parsed["rows"]}
    absent = sorted(set(codes) - returned)
    return {"status": "OK", "detail": "", "rows": parsed["rows"], "url": url,
            "source_last_updated": parsed["source_last_updated"], "countries_absent": absent}
