#!/usr/bin/env python3
"""Batch 3 CFTC historical-validation acquisition (network side only).

This script is deliberately outside the pure replay core. It downloads, for the
Batch 3 validation run only:

  * the full official CFTC COT history per market/report family through the
    existing provider path (scripts/cftc_data.py, the same code the R3 workspace
    uses), futures-only;
  * the Socrata row metadata (`:created_at`) for each market/family so the
    replay can use genuine publication timestamps where the CFTC Public
    Reporting Environment actually recorded them (from the report dated
    2022-09-13 onward; everything earlier carries the 2022-09-13 bulk-migration
    timestamp and is not a publication time);
  * the retained free Yahoo daily price history for the R3 proxy symbols through
    yfinance (same provider path as the app, continuous/spot proxies labelled).

Downloaded histories are large and are written outside Git (the caller passes
--data-dir; build/win-dev/cftc-validation/data is the documented default). The
script performs no rule evaluation: it never computes a research state.

Run it with the app-managed Python that has requests/pandas/yfinance
(%LOCALAPPDATA%\\com.marketlab.terminal\\venv-numpy2\\Scripts\\python.exe), e.g.

  python marketlab/cftc_validation_acquire.py \
      --data-dir build/win-dev/cftc-validation/data

Existing files are reused unless --refresh is given, so a rerun is cheap.
"""

from __future__ import annotations

import argparse
import json
import math
import platform
import re
import sys
import time
import urllib.parse
from datetime import datetime, timezone
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent.parent / "scripts"
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from cftc_data import CFTCDataWrapper  # noqa: E402

# The R3 supported market set (CftcPanel.cpp kMarkets + cot_codes + price_specs).
# key, label, asset class, CFTC contract code, Yahoo proxy symbol, proxy type.
# proxy_type: "continuous_front_month" (a rolling proxy, not a deliverable
# contract) or "spot_index" (an index level, not the futures contract).
UNIVERSE = [
    {"key": "gold", "label": "Gold", "asset_class": "metals", "code": "088691",
     "symbol": "GC=F", "proxy_type": "continuous_front_month"},
    {"key": "silver", "label": "Silver", "asset_class": "metals", "code": "084691",
     "symbol": "SI=F", "proxy_type": "continuous_front_month"},
    {"key": "copper", "label": "Copper", "asset_class": "metals", "code": "085692",
     "symbol": "HG=F", "proxy_type": "continuous_front_month"},
    {"key": "platinum", "label": "Platinum", "asset_class": "metals", "code": "076651",
     "symbol": "PL=F", "proxy_type": "continuous_front_month"},
    {"key": "palladium", "label": "Palladium", "asset_class": "metals", "code": "075651",
     "symbol": "PA=F", "proxy_type": "continuous_front_month"},
    {"key": "crude_oil", "label": "Crude Oil (WTI)", "asset_class": "energy", "code": "067651",
     "symbol": "CL=F", "proxy_type": "continuous_front_month"},
    {"key": "natural_gas", "label": "Natural Gas", "asset_class": "energy", "code": "023651",
     "symbol": "NG=F", "proxy_type": "continuous_front_month"},
    {"key": "gasoline", "label": "Gasoline", "asset_class": "energy", "code": "111659",
     "symbol": "RB=F", "proxy_type": "continuous_front_month"},
    {"key": "heating_oil", "label": "Heating Oil", "asset_class": "energy", "code": "022651",
     "symbol": "HO=F", "proxy_type": "continuous_front_month"},
    {"key": "corn", "label": "Corn", "asset_class": "agriculture", "code": "002602",
     "symbol": "ZC=F", "proxy_type": "continuous_front_month"},
    {"key": "wheat", "label": "Wheat", "asset_class": "agriculture", "code": "001602",
     "symbol": "ZW=F", "proxy_type": "continuous_front_month"},
    {"key": "soybeans", "label": "Soybeans", "asset_class": "agriculture", "code": "005602",
     "symbol": "ZS=F", "proxy_type": "continuous_front_month"},
    {"key": "cotton", "label": "Cotton", "asset_class": "agriculture", "code": "033661",
     "symbol": "CT=F", "proxy_type": "continuous_front_month"},
    {"key": "coffee", "label": "Coffee", "asset_class": "agriculture", "code": "083731",
     "symbol": "KC=F", "proxy_type": "continuous_front_month"},
    {"key": "sugar", "label": "Sugar", "asset_class": "agriculture", "code": "080732",
     "symbol": "SB=F", "proxy_type": "continuous_front_month"},
    {"key": "cocoa", "label": "Cocoa", "asset_class": "agriculture", "code": "073732",
     "symbol": "CC=F", "proxy_type": "continuous_front_month"},
    {"key": "live_cattle", "label": "Live Cattle", "asset_class": "agriculture", "code": "057642",
     "symbol": "LE=F", "proxy_type": "continuous_front_month"},
    {"key": "lean_hogs", "label": "Lean Hogs", "asset_class": "agriculture", "code": "054642",
     "symbol": "HE=F", "proxy_type": "continuous_front_month"},
    {"key": "euro", "label": "Euro (EUR/USD)", "asset_class": "fx", "code": "099741",
     "symbol": "6E=F", "proxy_type": "continuous_front_month"},
    {"key": "jpy", "label": "Japanese Yen", "asset_class": "fx", "code": "097741",
     "symbol": "6J=F", "proxy_type": "continuous_front_month"},
    {"key": "british_pound", "label": "British Pound", "asset_class": "fx", "code": "096742",
     "symbol": "6B=F", "proxy_type": "continuous_front_month"},
    {"key": "swiss_franc", "label": "Swiss Franc", "asset_class": "fx", "code": "092741",
     "symbol": "6S=F", "proxy_type": "continuous_front_month"},
    {"key": "canadian_dollar", "label": "Canadian Dollar", "asset_class": "fx", "code": "090741",
     "symbol": "6C=F", "proxy_type": "continuous_front_month"},
    {"key": "australian_dollar", "label": "Australian Dollar", "asset_class": "fx", "code": "232741",
     "symbol": "6A=F", "proxy_type": "continuous_front_month"},
    {"key": "s&p_500", "label": "S&P 500", "asset_class": "equity_indexes", "code": "13874A",
     "symbol": "ES=F", "proxy_type": "continuous_front_month"},
    {"key": "nasdaq_100", "label": "Nasdaq 100", "asset_class": "equity_indexes", "code": "209742",
     "symbol": "NQ=F", "proxy_type": "continuous_front_month"},
    {"key": "dow_jones", "label": "Dow Jones", "asset_class": "equity_indexes", "code": "124603",
     "symbol": "YM=F", "proxy_type": "continuous_front_month"},
    {"key": "nikkei", "label": "Nikkei 225", "asset_class": "equity_indexes", "code": "240741",
     "symbol": "NIY=F", "proxy_type": "continuous_front_month"},
    {"key": "vix", "label": "VIX", "asset_class": "volatility", "code": "1170E1",
     "symbol": "^VIX", "proxy_type": "spot_index"},
    {"key": "treasury_bonds", "label": "T-Bonds (30Y)", "asset_class": "rates", "code": "020601",
     "symbol": "ZB=F", "proxy_type": "continuous_front_month"},
    {"key": "treasury_notes_10y", "label": "T-Notes (10Y)", "asset_class": "rates", "code": "043602",
     "symbol": "ZN=F", "proxy_type": "continuous_front_month"},
    {"key": "treasury_notes_5y", "label": "T-Notes (5Y)", "asset_class": "rates", "code": "044601",
     "symbol": "ZF=F", "proxy_type": "continuous_front_month"},
    {"key": "treasury_notes_2y", "label": "T-Notes (2Y)", "asset_class": "rates", "code": "042601",
     "symbol": "ZT=F", "proxy_type": "continuous_front_month"},
    {"key": "fed_funds", "label": "Fed Funds", "asset_class": "rates", "code": "045601",
     "symbol": "ZQ=F", "proxy_type": "continuous_front_month"},
    {"key": "bitcoin", "label": "Bitcoin", "asset_class": "crypto", "code": "133741",
     "symbol": "BTC=F", "proxy_type": "continuous_front_month"},
    {"key": "ether", "label": "Ethereum", "asset_class": "crypto", "code": "146021",
     "symbol": "ETH=F", "proxy_type": "continuous_front_month"},
    {"key": "us_dollar_index", "label": "US Dollar Index", "asset_class": "fx", "code": "098662",
     "symbol": "DX-Y.NYB", "proxy_type": "spot_index"},
]

FAMILIES = ["legacy", "disaggregated", "tff"]

# Socrata bulk migration timestamp observed for every row loaded before the
# Public Reporting Environment began recording real weekly publications. The
# first genuine PRE publication is the report dated 2022-09-13 (created
# 2022-09-16T19:30:04Z). Anything created on/before 2022-09-13 is migration
# metadata, not a publication time.
PRE_MIGRATION_CUTOFF = "2022-09-14"


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _slug(symbol: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", symbol).strip("_") or "symbol"


def _write_json(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=1)
        handle.write("\n")


def _read_json(path: Path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def _fetch_created_at(wrapper: CFTCDataWrapper, resource_id: str, search_query: str):
    """Return {report_date: created_at} from Socrata row metadata.

    This is the retained publication metadata: Socrata stores the ingestion
    timestamp per row. For rows loaded in the 2022-09-13 bulk migration it is
    the migration time; from the 2022-09-13 report onward it is the actual
    weekly publication time. The replay applies that distinction, not this
    script, so the raw metadata is preserved verbatim.
    """
    encoded = urllib.parse.quote(search_query)
    created = {}
    offset = 0
    page_size = 5000
    while True:
        url = (
            f"{wrapper.base_url}/resource/{resource_id}.json?"
            f"$select=report_date_as_yyyy_mm_dd,:created_at"
            f"&$where={encoded}&$order=report_date_as_yyyy_mm_dd,id%20ASC"
            f"&$limit={page_size}&$offset={offset}"
        )
        page = wrapper._make_request(url)
        if page is None or not isinstance(page, list):
            return None, f"unreadable :created_at page at offset {offset}"
        for row in page:
            date_value = row.get("report_date_as_yyyy_mm_dd")
            stamp = row.get(":created_at")
            if isinstance(date_value, str) and "T" in date_value:
                date_value = date_value.split("T")[0]
            if date_value:
                created[date_value] = stamp
        if len(page) < page_size:
            break
        offset += len(page)
    return created, None


def acquire_cftc(wrapper, market, family, data_dir: Path, refresh: bool):
    out = data_dir / "cftc" / f"{market['key']}_{family}.json"
    if out.exists() and not refresh:
        return {"status": "cached", "file": str(out)}

    result = wrapper.get_cot_history(
        identifier=market["key"],
        report_type=family,
        futures_only=True,
        max_rows=20000,
    )
    if "error" in result:
        error = result["error"]
        message = error.get("error", str(error)) if isinstance(error, dict) else str(error)
        payload = {
            "success": False,
            "error": message,
            "validation": {
                "market_key": market["key"],
                "label": market["label"],
                "asset_class": market["asset_class"],
                "cftc_contract_code": market["code"],
                "report_family": family,
                "futures_only": True,
                "retrieved_at": _utc_now(),
            },
        }
        _write_json(out, payload)
        return {"status": "no_history", "file": str(out), "reason": message}

    resource_id = result["parameters"]["dataset"]
    created, created_error = _fetch_created_at(wrapper, resource_id, wrapper._build_search_query(market["key"]))
    if created_error:
        payload = {
            "success": False,
            "error": f"CFTC history downloaded but :created_at metadata failed: {created_error}",
            "validation": {
                "market_key": market["key"],
                "report_family": family,
                "retrieved_at": _utc_now(),
            },
        }
        _write_json(out, payload)
        return {"status": "metadata_failed", "file": str(out), "reason": created_error}

    genuine = sum(
        1
        for date_value, stamp in created.items()
        if isinstance(stamp, str) and stamp[:10] >= PRE_MIGRATION_CUTOFF and date_value >= "2022-09-14"
    )
    result["publication"] = {
        "source": "Socrata :created_at row metadata",
        "migration_cutoff": PRE_MIGRATION_CUTOFF,
        "created_at_by_report_date": created,
        "genuine_publication_count": genuine,
    }
    result["validation"] = {
        "market_key": market["key"],
        "label": market["label"],
        "asset_class": market["asset_class"],
        "cftc_contract_code": market["code"],
        "report_family": family,
        "futures_only": True,
        "retrieved_at": _utc_now(),
    }
    _write_json(out, result)
    return {
        "status": "ok",
        "file": str(out),
        "rows": result["parameters"]["count"],
        "first": result["parameters"]["first_report_date"],
        "last": result["parameters"]["last_report_date"],
        "genuine_publication_count": genuine,
    }


def acquire_prices(market, data_dir: Path, refresh: bool):
    import yfinance as yf

    slug = _slug(market["symbol"])
    out = data_dir / "prices" / f"{slug}.json"
    if out.exists() and not refresh:
        return {"status": "cached", "file": str(out), "price_file": out.name}
    try:
        ticker = yf.Ticker(market["symbol"])
        history = ticker.history(period="max", interval="1d", auto_adjust=False, timeout=30)
    except Exception as exc:  # provider failure is recorded, never fabricated
        payload = {
            "success": False,
            "error": f"yfinance failed for {market['symbol']}: {exc}",
            "market_key": market["key"],
            "symbol": market["symbol"],
            "retrieved_at": _utc_now(),
        }
        _write_json(out, payload)
        return {"status": "failed", "file": str(out), "price_file": out.name, "reason": str(exc)}

    points = []
    seen = set()
    for index, row in history.iterrows():
        try:
            close = float(row["Close"])
        except Exception:
            continue
        if not math.isfinite(close) or close <= 0.0:
            continue
        date_value = index.date().isoformat()
        if date_value in seen:
            continue
        seen.add(date_value)
        points.append({"date": date_value, "close": close})
    points.sort(key=lambda item: item["date"])
    payload = {
        "success": len(points) > 0,
        "market_key": market["key"],
        "label": market["label"],
        "symbol": market["symbol"],
        "proxy_type": market["proxy_type"],
        "source": "Yahoo Finance via yfinance (retained free public path)",
        "interval": "1d",
        "adjustment": "none (Close, auto_adjust=False)",
        "retrieved_at": _utc_now(),
        "first_date": points[0]["date"] if points else None,
        "last_date": points[-1]["date"] if points else None,
        "points": points,
    }
    _write_json(out, payload)
    return {
        "status": "ok" if points else "no_points",
        "file": str(out),
        "price_file": out.name,
        "points": len(points),
        "first": payload["first_date"],
        "last": payload["last_date"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Batch 3 CFTC validation acquisition")
    parser.add_argument(
        "--data-dir",
        default=str(Path(__file__).resolve().parent.parent / "build" / "win-dev" / "cftc-validation" / "data"),
        help="Directory for downloaded histories (must stay outside Git).",
    )
    parser.add_argument("--markets", nargs="*", default=None, help="Subset of market keys (default: all).")
    parser.add_argument("--families", nargs="*", default=FAMILIES, help="Report families (default: all three).")
    parser.add_argument("--refresh", action="store_true", help="Re-download even when a file already exists.")
    parser.add_argument("--skip-cftc", action="store_true")
    parser.add_argument("--skip-prices", action="store_true")
    parser.add_argument("--app-sha", default="")
    parser.add_argument("--control-sha", default="")
    args = parser.parse_args()

    data_dir = Path(args.data_dir).resolve()
    data_dir.mkdir(parents=True, exist_ok=True)

    markets = UNIVERSE
    if args.markets:
        wanted = set(args.markets)
        markets = [m for m in UNIVERSE if m["key"] in wanted]
        if not markets:
            print(f"No known market matches {args.markets}", file=sys.stderr)
            return 2

    import yfinance

    manifest = {
        "purpose": "Batch 3 CFTC historical validation acquisition (network side)",
        "app_sha": args.app_sha,
        "control_sha": args.control_sha,
        "started_at": _utc_now(),
        "python": sys.executable,
        "python_version": platform.python_version(),
        "yfinance": getattr(yfinance, "__version__", "unknown"),
        "futures_only": True,
        "families": list(args.families),
        "pairs": [],
        "prices": [],
        "market_universe": markets,
    }

    if not args.skip_cftc:
        wrapper = CFTCDataWrapper()
        for market in markets:
            for family in args.families:
                outcome = acquire_cftc(wrapper, market, family, data_dir, args.refresh)
                outcome.update({"market_key": market["key"], "report_family": family})
                manifest["pairs"].append(outcome)
                print(f"[cftc] {market['key']:<20} {family:<14} {outcome['status']}")
                time.sleep(0.2)

    if not args.skip_prices:
        for market in markets:
            outcome = acquire_prices(market, data_dir, args.refresh)
            outcome.update({"market_key": market["key"], "symbol": market["symbol"]})
            manifest["prices"].append(outcome)
            print(f"[price] {market['key']:<20} {market['symbol']:<10} {outcome['status']}")
            time.sleep(0.2)

    manifest["finished_at"] = _utc_now()
    ok_pairs = sum(1 for p in manifest["pairs"] if p["status"] in ("ok", "cached"))
    ok_prices = sum(1 for p in manifest["prices"] if p["status"] in ("ok", "cached"))
    manifest["summary"] = {
        "pairs_ok": ok_pairs,
        "pairs_total": len(manifest["pairs"]),
        "prices_ok": ok_prices,
        "prices_total": len(manifest["prices"]),
    }
    _write_json(data_dir / "acquisition_manifest.json", manifest)
    print(
        f"Done: {ok_pairs}/{len(manifest['pairs'])} market/family histories, "
        f"{ok_prices}/{len(manifest['prices'])} price series -> {data_dir}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
