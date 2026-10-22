"""Bond Pulse: daily breadth and customer flows of US corporate bonds (FINRA
TRACE aggregates, Query API, the owner's FINRA key).

Two FINRA datasets, for investment-grade and high-yield (junk) bonds:

  fixedIncomeMarket/corporateMarketBreadth    bonds traded that day that rose,
                                              fell or were unchanged in price;
                                              new 52-week highs and lows
  fixedIncomeMarket/corporateMarketSentiment  traded volume by side: customer
                                              buys and customer sells

Readings per category, for the latest trade date FINRA has published:

  net_advances  (advancing − declining bonds) ÷ both, %
  net_highs     new 52-week highs − new 52-week lows (count)
  net_buying    (customer buy − customer sell volume) ÷ both, %

Daily values swing, so each is also averaged over the last 10 trade dates
and that average is ranked against its previous 252 (a percentile, 0..100).
Nothing is filled: a date FINRA has not published is simply absent.

Credit markets often show stress before stocks do; high-yield bonds react
first. Read with the CNN junk-bond indicator in mind (that one is a yield
spread; this is trading breadth and flows).

Data: one trade date ≈ 2 breadth rows + 4 flow rows; the first read asks for
about 400 calendar days (well under 1 MB), later reads only for new dates,
at most every 2 hours. Cached in <cache-dir>.

Usage: python finra_bonds.py pulse --cache-dir DIR --key-file PATH
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
import pandas as pd

from finra_short_interest import access_token, post_all, read_key

BREADTH_URL = "https://api.finra.org/data/group/fixedIncomeMarket/name/corporateMarketBreadth"
FLOWS_URL = "https://api.finra.org/data/group/fixedIncomeMarket/name/corporateMarketSentiment"
CATEGORIES = {"investment grade": "investment_grade", "high yield": "high_yield"}
FIRST_READ_DAYS = 400
QUERY_EVERY = timedelta(hours=2)
AVERAGE_DAYS = 10
RANK_WINDOW = 252
MIN_RANK_HISTORY = 60
HISTORY_DAYS = 60


# ── FINRA ────────────────────────────────────────────────────────────────────

def parse_breadth(rows: list[dict]) -> pd.DataFrame:
    out = []
    for r in rows:
        cat = CATEGORIES.get(str(r.get("productCategory", "")).strip().lower())
        day = pd.to_datetime(r.get("tradeReportDate"), errors="coerce")
        if cat is None or pd.isna(day):
            continue
        vals = [r.get(k) for k in ("advances", "declines", "unchanged", "fiftyTwoWeekHigh", "fiftyTwoWeekLow")]
        if not all(isinstance(v, (int, float)) and v >= 0 for v in vals):
            continue
        out.append({"date": day.normalize(), "category": cat, "advances": float(vals[0]), "declines": float(vals[1]),
                    "unchanged": float(vals[2]), "highs": float(vals[3]), "lows": float(vals[4])})
    return pd.DataFrame(out, columns=["date", "category", "advances", "declines", "unchanged", "highs", "lows"])


def parse_flows(rows: list[dict]) -> pd.DataFrame:
    """(date, category, customer_buy, customer_sell) volumes."""
    out = {}
    for r in rows:
        cat = CATEGORIES.get(str(r.get("tradeType", "")).strip().lower())
        side = str(r.get("productCategory", "")).strip().lower()
        day = pd.to_datetime(r.get("tradeReportDate"), errors="coerce")
        vol = r.get("totalVolume")
        if cat is None or side not in ("customer buy", "customer sell") or pd.isna(day):
            continue
        if not isinstance(vol, (int, float)) or vol < 0:
            continue
        key = (day.normalize(), cat)
        out.setdefault(key, {"date": key[0], "category": cat, "customer_buy": np.nan, "customer_sell": np.nan})
        out[key]["customer_buy" if side == "customer buy" else "customer_sell"] = float(vol)
    return pd.DataFrame(list(out.values()), columns=["date", "category", "customer_buy", "customer_sell"])


def update(cache: Path, key: tuple[str, str], now: datetime) -> tuple[pd.DataFrame, pd.DataFrame, bool]:
    """Cached breadth and flows plus any trade date FINRA published since;
    FINRA is asked at most every 2 hours. Returns (breadth, flows, asked)."""
    bpath, fpath, mpath = cache / "bond_breadth.parquet", cache / "bond_flows.parquet", cache / "bond_meta.json"
    breadth = pd.read_parquet(bpath) if bpath.exists() else parse_breadth([])
    flows = pd.read_parquet(fpath) if fpath.exists() else parse_flows([])
    try:
        meta = json.loads(mpath.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        meta = {}
    asked_at = meta.get("asked_at")
    if asked_at and now - datetime.fromisoformat(asked_at) < QUERY_EVERY and len(breadth):
        return breadth, flows, False
    token = access_token(key)
    first = (now - timedelta(days=FIRST_READ_DAYS)).strftime("%Y-%m-%d")

    def since(df):
        return pd.Timestamp(df["date"].max()).strftime("%Y-%m-%d") if len(df) else first

    new_b = parse_breadth(post_all(token, {
        "compareFilters": [{"compareType": "GREATER" if len(breadth) else "GTE", "fieldName": "tradeReportDate",
                            "fieldValue": since(breadth)}],
        "domainFilters": [{"fieldName": "productCategory", "values": sorted(CATEGORIES)}]}, BREADTH_URL))
    new_f = parse_flows(post_all(token, {
        "compareFilters": [{"compareType": "GREATER" if len(flows) else "GTE", "fieldName": "tradeReportDate",
                            "fieldValue": since(flows)}],
        "domainFilters": [{"fieldName": "tradeType", "values": sorted(CATEGORIES)},
                          {"fieldName": "productCategory", "values": ["customer buy", "customer sell"]}]},
        FLOWS_URL))
    breadth = pd.concat([d for d in (breadth, new_b) if len(d)] or [breadth], ignore_index=True)
    flows = pd.concat([d for d in (flows, new_f) if len(d)] or [flows], ignore_index=True)
    breadth = breadth.drop_duplicates(["date", "category"], keep="last").sort_values(["date", "category"])
    flows = flows.drop_duplicates(["date", "category"], keep="last").sort_values(["date", "category"])
    for df, path in ((breadth, bpath), (flows, fpath)):
        tmp = path.with_suffix(".tmp")
        df.to_parquet(tmp, index=False)
        tmp.replace(path)
    mpath.write_text(json.dumps({"asked_at": now.isoformat(timespec="seconds")}), encoding="utf-8")
    return breadth, flows, True


# ── the reading ──────────────────────────────────────────────────────────────

def percentile_rank(history: pd.Series, value: float) -> float | None:
    h = history.dropna()
    if len(h) < MIN_RANK_HISTORY or value is None or not np.isfinite(value):
        return None
    return float((h <= value).mean() * 100.0)


def category_readings(breadth: pd.DataFrame, flows: pd.DataFrame, category: str) -> pd.DataFrame:
    """One row per trade date for one category: the three daily readings and
    their 10-date averages."""
    b = breadth[breadth["category"] == category].set_index("date").sort_index()
    f = flows[flows["category"] == category].set_index("date").sort_index()
    out = pd.DataFrame(index=b.index.union(f.index))
    moved = (b["advances"] + b["declines"]).replace(0, np.nan)
    out["advances"] = b["advances"]
    out["declines"] = b["declines"]
    out["unchanged"] = b["unchanged"]
    out["highs"] = b["highs"]
    out["lows"] = b["lows"]
    out["net_advances"] = (b["advances"] - b["declines"]) / moved * 100
    out["net_highs"] = b["highs"] - b["lows"]
    both = (f["customer_buy"] + f["customer_sell"]).replace(0, np.nan)
    out["customer_buy"] = f["customer_buy"]
    out["customer_sell"] = f["customer_sell"]
    out["net_buying"] = (f["customer_buy"] - f["customer_sell"]) / both * 100
    for col in ("net_advances", "net_highs", "net_buying"):
        out[col + "_avg"] = out[col].rolling(AVERAGE_DAYS, min_periods=AVERAGE_DAYS).mean()
    return out


def build(breadth: pd.DataFrame, flows: pd.DataFrame, retrieved_at: str) -> dict:
    if breadth.empty:
        raise ValueError("FINRA returned no corporate bond breadth")

    def num(v, places=2):
        return None if v is None or not np.isfinite(v) else round(float(v), places)

    categories = {}
    for name, cat in CATEGORIES.items():
        r = category_readings(breadth, flows, cat)
        if r.empty:
            continue
        i = len(r) - 1
        last = r.iloc[i]
        readings = {}
        for col in ("net_advances", "net_highs", "net_buying"):
            avg = col + "_avg"
            prior = r[avg].iloc[max(0, i - RANK_WINDOW):i]
            rank = percentile_rank(prior, last[avg])
            readings[col] = {"today": num(last[col]), "average": num(last[avg]),
                             "percentile": None if rank is None else round(rank, 1)}
        tail = r.iloc[-HISTORY_DAYS:]
        categories[cat] = {
            "label": name,
            "as_of": r.index[i].strftime("%Y-%m-%d"),
            "advances": num(last["advances"], 0), "declines": num(last["declines"], 0),
            "unchanged": num(last["unchanged"], 0), "highs": num(last["highs"], 0), "lows": num(last["lows"], 0),
            "customer_buy": num(last["customer_buy"]), "customer_sell": num(last["customer_sell"]),
            "readings": readings,
            "history": [{"date": d.strftime("%Y-%m-%d"), "net_advances_avg": num(v)}
                        for d, v in tail["net_advances_avg"].items() if np.isfinite(v)],
            "trade_dates": int(r["net_advances"].notna().sum()),
        }
    return {"status": "ok", "source": "FINRA TRACE corporate bond market breadth and sentiment (Query API)",
            "as_of": max(c["as_of"] for c in categories.values()), "retrieved_at": retrieved_at,
            "average_days": AVERAGE_DAYS, "rank_window": RANK_WINDOW, "categories": categories}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["pulse"])
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--key-file", required=True)
    args = ap.parse_args(argv[1:])
    now = datetime.now(timezone.utc)
    try:
        key = read_key(Path(args.key_file))
        if key is None:
            print(json.dumps({"success": True, "data": {"status": "needs_key", "key_file": args.key_file}}))
            return 0
        cache = Path(args.cache_dir)
        cache.mkdir(parents=True, exist_ok=True)
        breadth, flows, asked = update(cache, key, now)
        data = build(breadth, flows, now.isoformat(timespec="seconds"))
        data["asked_finra"] = asked
    except Exception as exc:  # noqa: BLE001 - reported to the widget, never hidden
        print(json.dumps({"success": False, "error": f"Bond Pulse: {type(exc).__name__}: {exc}"[:400]}))
        return 0
    print(json.dumps({"success": True, "data": data}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
