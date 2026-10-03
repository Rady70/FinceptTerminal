#!/usr/bin/env python3
"""MarketLab ETF Flow & Sector Rotation research acquisition (manual refresh).

Runtime owner: MarketLab Terminal. The application starts this script only
when the user asks for **Refresh ETF Research Data** (or runs the headless
``--etf-research refresh`` command). It never schedules or polls anything.

    python etf_research_data.py fetch --request <request.json> --out <payload.json>

The request (written by the application) names the symbols per history depth,
the funds to snapshot, the constituent policy, and the FRED/World Bank series.
The payload is a JSON document of source observations grouped by stage, each
stage and each symbol/series with its own status. Progress lines go to stderr
as ``ETFR_PROGRESS {json}``. stdout carries exactly one JSON summary.

Read-only research: no broker, account, order or execution path exists here.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from etf_research import macro, yahoo  # noqa: E402

SCRIPT_VERSION = "etf_research_fetch_v1"


def _now() -> _dt.datetime:
    return _dt.datetime.now(_dt.timezone.utc)


def _iso(t: _dt.datetime) -> str:
    return t.astimezone(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.") + f"{t.microsecond // 1000:03d}Z"


def _progress(stage: str, done: int, total: int, note: str = "") -> None:
    sys.stderr.write("ETFR_PROGRESS " + json.dumps({"stage": stage, "done": done, "total": total, "note": note})
                     + "\n")
    sys.stderr.flush()


def _digest(obj) -> str:
    return hashlib.sha256(json.dumps(obj, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()


def _stage(name: str, requested_at: _dt.datetime, items: dict) -> dict:
    statuses = [v.get("status") for v in items.values()]
    ok = sum(1 for s in statuses if s == "OK")
    if not statuses:
        status = "UNAVAILABLE"
    elif ok == len(statuses):
        status = "OK"
    elif ok == 0:
        status = "FAILED"
    else:
        status = "PARTIAL"
    return {"stage": name, "status": status, "requested_at": _iso(requested_at), "retrieved_at": _iso(_now()),
            "items_requested": len(statuses), "items_ok": ok, "sha256": _digest(items), "items": items}


def _num(value):
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    return v if v == v else None


def fetch_cftc(markets, report: str, futures_only: bool, wrapper=None) -> dict:
    """Weekly open interest and non-commercial long/short per market.

    The CFTC tool's own monitor read is reused: it refreshes MarketLab's CFTC
    archive incrementally (as the CFTC workspace does) and returns its
    canonical rows. A missing cell stays None, never 0.
    """
    if wrapper is None:
        import cftc_data  # MarketLab's CFTC tool (scripts/cftc_data.py)

        wrapper = cftc_data.CFTCDataWrapper()
    try:
        res = wrapper.get_cot_monitor(report, futures_only, list(markets), 25000)
    except Exception as exc:  # the tool failed as a whole
        res = {"error": {"message": str(exc)}}
    if not res.get("success"):
        why = (res.get("error") or {}).get("message", "CFTC monitor failed")
        return {m: {"status": "FAILED", "detail": str(why), "rows": [], "retrieved_at": _iso(_now())} for m in markets}
    items = {}
    for mk in res.get("data", {}).get("markets", []):
        rows = []
        for r in mk.get("rows", []):
            day = str(r.get("report_date_as_yyyy_mm_dd") or "")[:10]
            if not day:
                continue
            rows.append([day, _num(r.get("open_interest_all")), _num(r.get("non_commercial_long")),
                         _num(r.get("non_commercial_short"))])
        ok = any(row[1] is not None for row in rows)
        items[mk.get("market_key")] = {
            "status": "OK" if ok else "FAILED",
            "detail": mk.get("refresh_error") or ("" if ok else "no CFTC rows for this market"),
            "source_status": mk.get("status", ""),
            "rows": rows,
            "retrieved_at": _iso(_now()),
        }
    for m in markets:
        items.setdefault(m, {"status": "FAILED", "detail": "market missing from the CFTC tool's answer", "rows": [],
                             "retrieved_at": _iso(_now())})
    return items


def run_fetch(request: dict) -> dict:
    started = _now()
    stages = {}

    # ── Daily history: long, standard and constituent depths ──────────────────
    history_items = {}
    for depth, period in (("long", "max"), ("standard", "2y")):
        syms = request.get("history", {}).get(depth, [])
        if not syms:
            continue
        t0 = _now()
        _progress("yahoo_history", 0, len(syms), depth)
        got = yahoo.download_history(syms, period, t0)
        for s, v in got.items():
            v["retrieved_at"] = _iso(_now())
            history_items[s] = v
        _progress("yahoo_history", len(syms), len(syms), depth)
    inc = request.get("history", {}).get("incremental", {})
    if inc.get("symbols"):
        syms = inc["symbols"]
        t0 = _now()
        _progress("yahoo_history", 0, len(syms), "incremental")
        for s, v in yahoo.download_history(syms, None, t0, start=inc["start"]).items():
            v["retrieved_at"] = _iso(_now())
            history_items[s] = v
        _progress("yahoo_history", len(syms), len(syms), "incremental")
    stages["yahoo_history"] = _stage("yahoo_history", started, history_items)

    # ── Fund snapshots, holdings and sector weights ─────────────────────────────
    funds = request.get("funds", [])
    t_funds = _now()
    fund_items = {}
    if funds:
        done = 0
        with ThreadPoolExecutor(max_workers=6) as pool:
            for sym, res in zip(funds, pool.map(yahoo.fund_snapshot, funds)):
                res["captured_at"] = _iso(_now())
                fund_items[sym] = res
                done += 1
                if done % 10 == 0 or done == len(funds):
                    _progress("yahoo_funds", done, len(funds))
    stages["yahoo_funds"] = _stage("yahoo_funds", t_funds, fund_items)

    # ── Constituents: top holdings of the requested parents + fixed stock lists ─
    cons = request.get("constituents", {})
    max_per = int(cons.get("max_per_parent", 10))
    constituent_set = set(cons.get("extra_symbols", []))
    # Holdings are fetched under their research symbol: the reviewed map adds a
    # missing exchange suffix (e.g. 00939 -> 0939.HK); non-equity holdings are skipped.
    symbol_map = cons.get("symbol_map", {})
    excluded = set(cons.get("exclude_holdings", []))
    for parent in cons.get("holding_parents", []):
        for rank, hsym, _name, _w in fund_items.get(parent, {}).get("holdings", [])[:max_per]:
            if hsym and hsym.upper() not in ("CASH", "USD") and hsym not in excluded:
                constituent_set.add(symbol_map.get(hsym, hsym))
    # Symbols whose history the request already fetched at a longer depth are
    # not fetched again at the constituent depth (one consistent window each).
    constituent_set -= set(cons.get("exclude", []))
    constituent_list = sorted(constituent_set)
    t_cons = _now()
    cons_hist = {}
    cons_inc = cons.get("incremental", {})
    inc_set = set(cons_inc.get("symbols", []))
    full_list = [x for x in constituent_list if x not in inc_set]
    inc_list = [x for x in constituent_list if x in inc_set]
    if constituent_list and cons.get("history_period"):
        _progress("yahoo_constituent_history", 0, len(constituent_list))
        if full_list:
            cons_hist.update(yahoo.download_history(full_list, cons["history_period"], t_cons))
        if inc_list:
            cons_hist.update(yahoo.download_history(inc_list, None, t_cons, start=cons_inc["start"]))
        for v in cons_hist.values():
            v["retrieved_at"] = _iso(_now())
        _progress("yahoo_constituent_history", len(constituent_list), len(constituent_list))
    stages["yahoo_constituent_history"] = _stage("yahoo_constituent_history", t_cons, cons_hist)

    t_fund = _now()
    fundamentals = {}
    # Fundamentals captured within the last few days are reused, not re-fetched.
    fresh = set(cons.get("fundamentals_fresh", []))
    wanted = constituent_set | set(cons.get("fundamental_extra", []))
    fundamental_list = sorted(wanted - fresh)
    skipped_fresh = len(wanted & fresh)
    if fundamental_list and cons.get("fundamentals", False):
        done = 0
        with ThreadPoolExecutor(max_workers=6) as pool:
            for sym, res in zip(fundamental_list, pool.map(yahoo.fundamentals, fundamental_list)):
                res["captured_at"] = _iso(_now())
                fundamentals[sym] = res
                done += 1
                if done % 20 == 0 or done == len(fundamental_list):
                    _progress("yahoo_fundamentals", done, len(fundamental_list))
    stages["yahoo_fundamentals"] = _stage("yahoo_fundamentals", t_fund, fundamentals)
    stages["yahoo_fundamentals"]["skipped_fresh"] = skipped_fresh
    stages["yahoo_fundamentals"]["fresh_days"] = int(cons.get("fundamentals_fresh_days", 0))

    # ── FRED ─────────────────────────────────────────────────────────────────
    t_fred = _now()
    fred_items = {}
    series = request.get("fred", [])
    for i, sid in enumerate(series, start=1):
        res = macro.fred_series(sid)
        text = res.pop("text", None)
        res["response_sha256"] = hashlib.sha256(text.encode("utf-8")).hexdigest() if text else ""
        res["retrieved_at"] = _iso(_now())
        fred_items[sid] = res
        _progress("fred", i, len(series), sid)
    stages["fred"] = _stage("fred", t_fred, fred_items)

    # ── World Bank ────────────────────────────────────────────────────────────
    t_wb = _now()
    wb_items = {}
    wb = request.get("world_bank", {})
    codes = wb.get("countries", [])
    for i, ind in enumerate(wb.get("indicators", []), start=1):
        res = macro.world_bank_indicator(ind, codes)
        res["retrieved_at"] = _iso(_now())
        wb_items[ind] = res
        _progress("world_bank", i, len(wb.get("indicators", [])), ind)
    stages["world_bank"] = _stage("world_bank", t_wb, wb_items)

    # ── CFTC positioning (through MarketLab's CFTC tool and its archive) ───────
    t_cftc = _now()
    cftc_items = {}
    cftc_req = request.get("cftc", {})
    markets = cftc_req.get("markets", [])
    if markets:
        _progress("cftc", 0, len(markets))
        cftc_items = fetch_cftc(markets, cftc_req.get("report", "legacy"), bool(cftc_req.get("futures_only", True)))
        _progress("cftc", len(markets), len(markets))
    stages["cftc"] = _stage("cftc", t_cftc, cftc_items)

    return {
        "script_version": SCRIPT_VERSION,
        "started_at": _iso(started),
        "finished_at": _iso(_now()),
        "constituents": constituent_list,
        "stages": stages,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="etf_research_data.py")
    sub = parser.add_subparsers(dest="command")
    fetch = sub.add_parser("fetch")
    fetch.add_argument("--request", required=True)
    fetch.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    if args.command != "fetch":
        print(json.dumps({"error": {"code": "USAGE", "message": "usage: etf_research_data.py fetch --request R --out O"}}))
        return 2
    try:
        request = json.loads(Path(args.request).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(json.dumps({"error": {"code": "BAD_REQUEST", "message": str(exc)}}))
        return 2
    payload = run_fetch(request)
    out_path = Path(args.out)
    tmp = out_path.with_suffix(out_path.suffix + ".part")
    tmp.write_text(json.dumps(payload, allow_nan=False), encoding="utf-8")
    tmp.replace(out_path)
    summary = {name: {"status": st["status"], "items_requested": st["items_requested"], "items_ok": st["items_ok"]}
               for name, st in payload["stages"].items()}
    print(json.dumps({"ok": True, "script_version": SCRIPT_VERSION, "out": str(out_path), "stages": summary}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
