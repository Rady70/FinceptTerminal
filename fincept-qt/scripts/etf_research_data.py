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
    for parent in cons.get("holding_parents", []):
        for rank, hsym, _name, _w in fund_items.get(parent, {}).get("holdings", [])[:max_per]:
            if hsym and hsym.upper() not in ("CASH", "USD"):
                constituent_set.add(hsym)
    # Symbols whose history the request already fetched at a longer depth are
    # not fetched again at the constituent depth (one consistent window each).
    constituent_set -= set(cons.get("exclude", []))
    constituent_list = sorted(constituent_set)
    t_cons = _now()
    cons_hist = {}
    if constituent_list and cons.get("history_period"):
        _progress("yahoo_constituent_history", 0, len(constituent_list))
        cons_hist = yahoo.download_history(constituent_list, cons["history_period"], t_cons)
        for v in cons_hist.values():
            v["retrieved_at"] = _iso(_now())
        _progress("yahoo_constituent_history", len(constituent_list), len(constituent_list))
    stages["yahoo_constituent_history"] = _stage("yahoo_constituent_history", t_cons, cons_hist)

    t_fund = _now()
    fundamentals = {}
    fundamental_list = sorted(constituent_set | set(cons.get("fundamental_extra", [])))
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
