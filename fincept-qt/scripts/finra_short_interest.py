"""Aggregate short interest of the S&P 500 (FINRA consolidated short interest).

Short interest is the number of a stock's shares sold short and not yet
bought back. Rapach, Ringgenberg & Zhou (2016, "Short interest and aggregate
stock returns", Journal of Financial Economics) average it over all stocks,
each as a % of its shares outstanding, and find it among the strongest known
predictors of the market's return: a high reading has been followed by weak
returns. This script builds that average for today's S&P 500 members:

  average_pct    equal-weight mean, across companies, of short interest ÷
                 shares outstanding, %. A company with two listed share
                 classes (GOOGL / GOOG) counts once: the classes' short
                 interest is summed over the company's total shares.
  days_to_cover  all members' short interest ÷ their average daily volume
                 (FINRA's own volume figure), days

Short interest drifts up or down for years for reasons unrelated to mood
(the paper removes a linear trend before using it). Each count is therefore
compared with the straight-line trend of the 48 counts (two years) BEFORE
it — no later count is used — and that gap is ranked against the earlier
counts' gaps (a percentile, 0..100). The plain level's rank is reported too.

Sources:
  FINRA Query API, dataset otcMarket/consolidatedShortInterest, with the
  owner's own FINRA API key (a free Individual account's Public credential;
  FINRA permits non-commercial personal use). Counts are as of FINRA's
  settlement dates (mid-month and month-end) and are published about eight
  business days later. Without a key file the script reports "needs_key" and
  reads nothing.
  Shares outstanding: Yahoo's dated share counts (yfinance get_shares_full),
  the latest count on or before each settlement date.

Members, sectors and CIKs come from market_breadth.py's member cache.
Missing stocks are counted and named, never filled.

Per-stock lookup (`stock --symbol X`): one stock's counts since 2023, read
only when asked for (about 18 KB the first time, then only new counts; the
whole market would be about 4.4 MB per count date and 400 MB for the
history), with its short interest as a % of Yahoo's share count and FINRA's
days to cover. Cached per symbol under <cache-dir>/stocks.

Usage: python finra_short_interest.py summary --cache-dir DIR --key-file PATH
       python finra_short_interest.py stock --symbol TSLA --cache-dir DIR --key-file PATH
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
import pandas as pd

from market_breadth import load_members, member_ciks

TOKEN_URL = "https://ews.fip.finra.org/fip/rest/ews/oauth2/access_token?grant_type=client_credentials"
DATA_URL = "https://api.finra.org/data/group/otcMarket/name/consolidatedShortInterest"
HISTORY_START = "2023-01-01"
HISTORY_AFTER = "2022-12-31"
DATE_ANCHOR = "AAPL"
PAGE = 5000
QUERY_EVERY = timedelta(hours=12)
SHARES_MAX_AGE = timedelta(days=7)
MIN_COMPANIES = 300
MIN_RANK_HISTORY = 24
TREND_COUNTS = 48
FIELDS = ["symbolCode", "settlementDate", "currentShortPositionQuantity", "averageDailyVolumeQuantity",
          "revisionFlag"]


def read_key(path: Path) -> tuple[str, str] | None:
    """(client id, client secret) from the owner's key file, or None."""
    try:
        o = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    cid, secret = str(o.get("client_id", "")).strip(), str(o.get("client_secret", "")).strip()
    return (cid, secret) if cid and secret else None


def finra_symbol(symbol: str) -> str:
    """Yahoo's BRK-B is FINRA's BRKB."""
    return symbol.replace("-", "").replace(".", "")


# ── FINRA ────────────────────────────────────────────────────────────────────

def access_token(key: tuple[str, str]) -> str:
    import requests

    r = requests.post(TOKEN_URL, auth=key, timeout=30)
    if r.status_code != 200:
        raise RuntimeError(f"FINRA refused the API key (HTTP {r.status_code})")
    token = r.json().get("access_token")
    if not token:
        raise RuntimeError("FINRA returned no access token")
    return token


def _post(token: str, payload: dict, url: str = DATA_URL) -> list[dict]:
    """One FINRA Query API page. `url` is one of the module constants of the
    FINRA scripts (this one's DATA_URL, finra_bonds.py's dataset URLs)."""
    import requests

    r = requests.post(url, json=payload, timeout=120,
                      headers={"Authorization": f"Bearer {token}", "Accept": "application/json"})
    if r.status_code == 204 or (r.status_code == 200 and not r.content.strip()):
        return []
    if r.status_code != 200:
        raise RuntimeError(f"FINRA: HTTP {r.status_code}")
    return r.json()


def post_all(token: str, payload: dict, url: str = DATA_URL) -> list[dict]:
    """Every page of a FINRA query."""
    rows, offset = [], 0
    while True:
        page = _post(token, {**payload, "limit": PAGE, "offset": offset}, url)
        rows.extend(page)
        if len(page) < PAGE:
            return rows
        offset += PAGE


def settlement_dates(token: str, after: str) -> list[str]:
    """FINRA's settlement dates after `after`, read from one stock that has a
    count on every date (one small request instead of a whole-market read,
    which FINRA's gateway times out on)."""
    rows = _post(token, {"limit": PAGE, "fields": ["settlementDate"],
                         "compareFilters": [{"compareType": "EQUAL", "fieldName": "symbolCode",
                                             "fieldValue": DATE_ANCHOR},
                                            {"compareType": "GREATER", "fieldName": "settlementDate",
                                             "fieldValue": after}]})
    return sorted({str(r["settlementDate"]) for r in rows if r.get("settlementDate")})


def fetch_counts(token: str, symbols: list[str], day: str) -> pd.DataFrame:
    """The members' counts of one settlement date."""
    to_yahoo = {finra_symbol(s): s for s in symbols}
    rows, offset = [], 0
    while True:
        page = _post(token, {"limit": PAGE, "offset": offset, "fields": FIELDS,
                             "compareFilters": [{"compareType": "EQUAL", "fieldName": "settlementDate",
                                                 "fieldValue": day}],
                             "domainFilters": [{"fieldName": "symbolCode", "values": sorted(to_yahoo)}]})
        rows.extend(page)
        if len(page) < PAGE:
            break
        offset += PAGE
    return parse_counts(rows, to_yahoo)


def parse_counts(rows: list[dict], to_yahoo: dict[str, str]) -> pd.DataFrame:
    """FINRA rows → (symbol, settlement_date, short_qty, adv). A revised row
    replaces the original; rows without a count are dropped."""
    out = []
    for row in rows:
        sym = to_yahoo.get(str(row.get("symbolCode", "")).strip())
        day = pd.to_datetime(row.get("settlementDate"), errors="coerce")
        qty = row.get("currentShortPositionQuantity")
        if sym is None or pd.isna(day) or not isinstance(qty, (int, float)) or qty < 0:
            continue
        adv = row.get("averageDailyVolumeQuantity")
        out.append({"symbol": sym, "settlement_date": day.normalize(), "short_qty": float(qty),
                    "adv": float(adv) if isinstance(adv, (int, float)) and adv > 0 else np.nan,
                    "revised": bool(row.get("revisionFlag"))})
    df = pd.DataFrame(out, columns=["symbol", "settlement_date", "short_qty", "adv", "revised"])
    df = df.sort_values("revised").drop_duplicates(["symbol", "settlement_date"], keep="last")
    return df.drop(columns="revised").reset_index(drop=True)


def update_counts(cache: Path, key: tuple[str, str], symbols: list[str], now: datetime) -> tuple[pd.DataFrame, bool]:
    """Cached counts plus every settlement date FINRA published since; FINRA
    is asked at most every 12 hours, and a member's history is read once
    (when it is first seen). Returns (counts, asked FINRA)."""
    path, meta_path = cache / "short_interest.parquet", cache / "short_interest_meta.json"
    counts = pd.read_parquet(path) if path.exists() else pd.DataFrame(
        columns=["symbol", "settlement_date", "short_qty", "adv"])
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        meta = {}
    asked_for = set(meta.get("symbols", []))
    new_members = [s for s in symbols if s not in asked_for]
    asked_at = meta.get("asked_at")
    due = not asked_at or now - datetime.fromisoformat(asked_at) >= QUERY_EVERY
    if not due and not new_members:
        return counts, False
    token = access_token(key)
    stored = sorted({d.strftime("%Y-%m-%d") for d in pd.to_datetime(counts["settlement_date"])}) if len(counts) else []
    parts = [counts]
    if stored and new_members:
        for day in stored:
            parts.append(fetch_counts(token, new_members, day))
    after = stored[-1] if stored else HISTORY_AFTER
    for day in settlement_dates(token, after):
        parts.append(fetch_counts(token, symbols, day))
    nonempty = [p for p in parts if len(p)]
    if nonempty:
        counts = pd.concat(nonempty, ignore_index=True)
    counts = counts.drop_duplicates(["symbol", "settlement_date"], keep="last")
    counts = counts.sort_values(["settlement_date", "symbol"]).reset_index(drop=True)
    tmp = path.with_suffix(".tmp")
    counts.to_parquet(tmp, index=False)
    tmp.replace(path)
    meta_path.write_text(json.dumps({"asked_at": now.isoformat(timespec="seconds"),
                                     "symbols": sorted(asked_for | set(symbols))}), encoding="utf-8")
    return counts, True


# ── shares outstanding ───────────────────────────────────────────────────────

def update_shares(cache: Path, symbols: list[str], now: datetime) -> pd.DataFrame:
    """Yahoo's dated share counts per member, re-read weekly."""
    import yfinance as yf

    path, meta_path = cache / "shares.parquet", cache / "shares_meta.json"
    shares = pd.read_parquet(path) if path.exists() else pd.DataFrame(columns=["symbol", "date", "shares"])
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        meta = {}
    start = (pd.Timestamp(HISTORY_START) - pd.Timedelta(days=365)).strftime("%Y-%m-%d")
    fresh = []
    for s in symbols:
        got = meta.get(s)
        if got and now - datetime.fromisoformat(got) < SHARES_MAX_AGE:
            continue
        try:
            series = yf.Ticker(s).get_shares_full(start=start)
        except Exception:  # noqa: BLE001 - a stock without counts is reported, not fatal
            series = None
        if series is None or len(series) == 0:
            continue
        idx = pd.to_datetime(series.index)
        idx = idx.tz_localize(None) if idx.tz is not None else idx
        fresh.append(pd.DataFrame({"symbol": s, "date": idx.normalize(), "shares": series.to_numpy(float)}))
        meta[s] = now.isoformat(timespec="seconds")
    if fresh:
        done = {f["symbol"].iloc[0] for f in fresh}
        shares = pd.concat([shares[~shares["symbol"].isin(done)], *fresh], ignore_index=True)
        shares = shares[shares["shares"] > 0].drop_duplicates(["symbol", "date"], keep="last")
        tmp = path.with_suffix(".tmp")
        shares.to_parquet(tmp, index=False)
        tmp.replace(path)
        meta_path.write_text(json.dumps(meta), encoding="utf-8")
    return shares


# ── the reading ──────────────────────────────────────────────────────────────

def company_readings(counts: pd.DataFrame, shares: pd.DataFrame, ciks: dict[str, str]) -> pd.DataFrame:
    """One row per (settlement date, company): short interest % of shares and
    the summed short interest and volume. A company is skipped on a date when
    one of its classes has no count or it has no share count yet."""
    if counts.empty:
        return pd.DataFrame(columns=["settlement_date", "company", "pct", "short_qty", "adv"])
    c = counts.copy()
    c["settlement_date"] = pd.to_datetime(c["settlement_date"]).astype("datetime64[ns]")
    c["company"] = c["symbol"].map(lambda s: ciks.get(s, s))
    classes = c.groupby("company")["symbol"].nunique()
    grouped = c.groupby(["settlement_date", "company"]).agg(
        short_qty=("short_qty", "sum"), adv=("adv", "sum"), n=("symbol", "nunique"), first=("symbol", "min"))
    grouped = grouped.reset_index()
    grouped = grouped[grouped["n"] == grouped["company"].map(classes)]

    # Latest share count on or before each settlement date (any class of the
    # company: Yahoo reports the company total for each class).
    sh = shares.copy()
    sh["date"] = pd.to_datetime(sh["date"]).astype("datetime64[ns]")
    sh["company"] = sh["symbol"].map(lambda s: ciks.get(s, s))
    sh = sh.sort_values("date").drop_duplicates(["company", "date"], keep="last")
    grouped = grouped.sort_values("settlement_date")
    merged = pd.merge_asof(grouped, sh[["company", "date", "shares"]].sort_values("date"),
                           left_on="settlement_date", right_on="date", by="company", direction="backward")
    merged["pct"] = merged["short_qty"] / merged["shares"] * 100
    merged = merged[merged["pct"].notna() & (merged["pct"] <= 100)]
    return merged[["settlement_date", "company", "pct", "short_qty", "adv"]].reset_index(drop=True)


def percentile_rank(history: pd.Series, value: float) -> float | None:
    """Share of the previous counts at or below `value`, 0..100."""
    h = history.dropna()
    if len(h) < MIN_RANK_HISTORY or value is None or not np.isfinite(value):
        return None
    return float((h <= value).mean() * 100.0)


def above_trend(series: pd.Series) -> pd.Series:
    """Each value minus the straight-line trend of the TREND_COUNTS values
    before it, extended to its own position (NaN until there are enough)."""
    out = pd.Series(np.nan, index=series.index)
    values = series.to_numpy(float)
    x = np.arange(TREND_COUNTS)
    for i in range(TREND_COUNTS, len(values)):
        window = values[i - TREND_COUNTS:i]
        if not np.isfinite(window).all() or not np.isfinite(values[i]):
            continue
        slope, intercept = np.polyfit(x, window, 1)
        out.iloc[i] = values[i] - (intercept + slope * TREND_COUNTS)
    return out


def build(counts: pd.DataFrame, shares: pd.DataFrame, members: list[str], ciks: dict[str, str],
          retrieved_at: str, notes: list[str]) -> dict:
    per = company_readings(counts, shares, ciks)
    by_date = per.groupby("settlement_date").agg(
        average_pct=("pct", "mean"), companies=("company", "nunique"),
        short_qty=("short_qty", "sum"), adv=("adv", "sum"))
    by_date = by_date[by_date["companies"] >= MIN_COMPANIES].sort_index()
    if by_date.empty:
        raise ValueError("no settlement date with enough companies")
    by_date["days_to_cover"] = by_date["short_qty"] / by_date["adv"].replace(0, np.nan)
    by_date["pct_gap"] = above_trend(by_date["average_pct"])
    by_date["dtc_gap"] = above_trend(by_date["days_to_cover"])
    last = by_date.iloc[-1]
    prior = by_date.iloc[:-1]
    as_of = by_date.index[-1]

    def num(v, places=2):
        return None if v is None or not np.isfinite(v) else round(float(v), places)

    pct_rank = percentile_rank(prior["pct_gap"], last["pct_gap"])
    dtc_rank = percentile_rank(prior["dtc_gap"], last["dtc_gap"])
    level_rank = percentile_rank(prior["average_pct"], last["average_pct"])
    on_date = set(counts.loc[counts["settlement_date"] == as_of, "symbol"])
    with_shares = set(shares["symbol"].unique())
    return {
        "status": "ok",
        "source": "FINRA consolidated short interest (Query API); shares outstanding from Yahoo",
        "as_of": as_of.strftime("%Y-%m-%d"),
        "retrieved_at": retrieved_at,
        "average_pct": num(last["average_pct"]),
        "previous_pct": num(prior["average_pct"].iloc[-1]) if len(prior) else None,
        "trend_pct": num(last["average_pct"] - last["pct_gap"]),
        "above_trend_pp": num(last["pct_gap"]),
        "percentile": None if pct_rank is None else round(pct_rank, 1),
        "level_percentile": None if level_rank is None else round(level_rank, 1),
        "days_to_cover": num(last["days_to_cover"]),
        "days_to_cover_trend": num(last["days_to_cover"] - last["dtc_gap"]),
        "days_to_cover_percentile": None if dtc_rank is None else round(dtc_rank, 1),
        "companies": int(last["companies"]),
        "members": len({ciks.get(s, s) for s in members}),
        "counts_ranked": int(prior["pct_gap"].notna().sum()),
        "trend_counts": TREND_COUNTS,
        "history_start": by_date.index[0].strftime("%Y-%m-%d"),
        "no_count": sorted(s for s in members if s not in on_date),
        "no_shares": sorted(s for s in members if s not in with_shares),
        "history": [{"date": d.strftime("%Y-%m-%d"), "pct": num(v, 3)} for d, v in by_date["average_pct"].items()],
        "notes": notes,
    }


# ── one stock, on request ────────────────────────────────────────────────────

STOCK_FIELDS = ["symbolCode", "issueName", "settlementDate", "currentShortPositionQuantity",
                "averageDailyVolumeQuantity", "daysToCoverQuantity", "revisionFlag"]
SYMBOL_RE = re.compile(r"^[A-Z][A-Z0-9.\-]{0,9}$")


def merge_stock_rows(stored: list[dict], rows: list[dict]) -> list[dict]:
    """Stored counts plus new FINRA rows, one per settlement date (a revised
    row replaces the original), oldest first."""
    by_date = {r["date"]: r for r in stored}
    for row in sorted(rows, key=lambda r: bool(r.get("revisionFlag"))):
        day = str(row.get("settlementDate") or "")[:10]
        qty = row.get("currentShortPositionQuantity")
        if not day or not isinstance(qty, (int, float)) or qty < 0:
            continue
        adv = row.get("averageDailyVolumeQuantity")
        dtc = row.get("daysToCoverQuantity")
        by_date[day] = {"date": day, "short_qty": float(qty),
                        "adv": float(adv) if isinstance(adv, (int, float)) and adv > 0 else None,
                        "days_to_cover": float(dtc) if isinstance(dtc, (int, float)) and dtc >= 0 else None,
                        "name": str(row.get("issueName") or "").strip()}
    return [by_date[d] for d in sorted(by_date)]


def stock_view(symbol: str, rows: list[dict], shares: list[list]) -> dict:
    """The lookup's answer: each count with % of shares where a share count
    dated on or before it exists, the latest count and its change. Days to
    cover = shares short ÷ FINRA's average daily volume: FINRA's own
    daysToCoverQuantity never goes below 1 (e.g. TSLA, 13 Jan 2023: 1.0 where
    the ratio is 0.48), so it is used only when the volume is missing."""
    if not rows:
        return {"status": "no_data", "symbol": symbol}
    share_points = sorted((pd.Timestamp(d), float(v)) for d, v in shares if v and v > 0)
    history = []
    for r in rows:
        day = pd.Timestamp(r["date"])
        in_force = [v for d, v in share_points if d <= day]
        pct = r["short_qty"] / in_force[-1] * 100 if in_force else None
        adv = r.get("adv")
        dtc = round(r["short_qty"] / adv, 2) if adv else r.get("days_to_cover")
        history.append({"date": r["date"], "short_qty": r["short_qty"], "days_to_cover": dtc,
                        "pct": None if pct is None or pct > 100 else round(pct, 3)})
    last, prev = history[-1], (history[-2] if len(history) > 1 else None)
    change = None
    if prev and prev["short_qty"] > 0:
        change = round((last["short_qty"] / prev["short_qty"] - 1) * 100, 2)
    return {"status": "ok", "symbol": symbol, "name": rows[-1].get("name", ""), "as_of": last["date"],
            "short_qty": last["short_qty"], "pct": last["pct"], "days_to_cover": last["days_to_cover"],
            "previous_date": prev["date"] if prev else None, "change_pct": change, "counts": len(history),
            "history": history}


def stock_lookup(cache: Path, key: tuple[str, str], symbol: str, now: datetime) -> dict:
    sym = symbol.strip().upper()
    if not SYMBOL_RE.match(sym):
        raise ValueError(f"not a stock symbol: {symbol!r}")
    fsym = finra_symbol(sym)
    folder = cache / "stocks"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{fsym}.json"
    try:
        stored = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        stored = {"rows": [], "asked_at": None, "shares": [], "shares_at": None}
    asked_at = stored.get("asked_at")
    asked = False
    if not asked_at or now - datetime.fromisoformat(asked_at) >= QUERY_EVERY:
        after = stored["rows"][-1]["date"] if stored["rows"] else HISTORY_AFTER
        rows = post_all(access_token(key), {
            "fields": STOCK_FIELDS,
            "compareFilters": [{"compareType": "EQUAL", "fieldName": "symbolCode", "fieldValue": fsym},
                               {"compareType": "GREATER", "fieldName": "settlementDate", "fieldValue": after}]})
        stored["rows"] = merge_stock_rows(stored["rows"], rows)
        stored["asked_at"] = now.isoformat(timespec="seconds")
        asked = True
    shares_at = stored.get("shares_at")
    if stored["rows"] and (not shares_at or now - datetime.fromisoformat(shares_at) >= SHARES_MAX_AGE):
        try:
            import yfinance as yf

            start = (pd.Timestamp(HISTORY_START) - pd.Timedelta(days=365)).strftime("%Y-%m-%d")
            series = yf.Ticker(sym).get_shares_full(start=start)
            if series is not None and len(series):
                idx = pd.to_datetime(series.index)
                idx = idx.tz_localize(None) if idx.tz is not None else idx
                stored["shares"] = [[d.strftime("%Y-%m-%d"), float(v)] for d, v in zip(idx, series.to_numpy(float))]
            stored["shares_at"] = now.isoformat(timespec="seconds")
        except Exception:  # noqa: BLE001 - % of shares is then unavailable, the counts still show
            pass
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(stored), encoding="utf-8")
    tmp.replace(path)
    view = stock_view(sym, stored["rows"], stored.get("shares", []))
    view["asked_finra"] = asked
    view["retrieved_at"] = now.isoformat(timespec="seconds")
    return view


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["summary", "stock"])
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--key-file", required=True)
    ap.add_argument("--symbol", default="")
    args = ap.parse_args(argv[1:])
    now = datetime.now(timezone.utc)
    try:
        key = read_key(Path(args.key_file))
        if key is None:
            print(json.dumps({"success": True, "data": {"status": "needs_key", "key_file": args.key_file}}))
            return 0
        if args.command == "stock":
            data = stock_lookup(Path(args.cache_dir), key, args.symbol, now)
            print(json.dumps({"success": True, "data": data}))
            return 0
        cache = Path(args.cache_dir)
        cache.mkdir(parents=True, exist_ok=True)
        members_cache = cache.parent / "market_breadth"
        members_cache.mkdir(parents=True, exist_ok=True)
        members, _, members_at, note = load_members(members_cache, now)
        ciks = member_ciks(members_cache)
        notes = [note] if note else []
        notes.append(f"Members as listed on {members_at[:10]}; earlier counts use today's members.")
        counts, asked = update_counts(cache, key, members, now)
        shares = update_shares(cache, members, now)
        data = build(counts, shares, members, ciks, now.isoformat(timespec="seconds"), notes)
        data["asked_finra"] = asked
    except Exception as exc:  # noqa: BLE001 - reported to the widget, never hidden
        print(json.dumps({"success": False, "error": f"Short interest: {type(exc).__name__}: {exc}"[:400]}))
        return 0
    print(json.dumps({"success": True, "data": data}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
