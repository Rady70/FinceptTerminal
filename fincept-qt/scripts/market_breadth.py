"""Risk appetite over every S&P 500 member (Yahoo daily bars), plus the day's
up / down counts.

Risk appetite asks whether investors are buying risky stocks or retreating
into safe ones. Idea: Kumar & Persaud (2002), "Pure contagion and investors'
shifting risk appetite" — when appetite rises, the assets that were riskiest
do best; and Baker & Wurgler (2006, 2007) — investor mood moves volatile,
speculative stocks most. Each session, over the members with enough history:

  risky_minus_safe          equal-weight return over the last 20 sessions of
                            the most volatile fifth of members minus the
                            least volatile fifth, percentage points
  rank_correlation          Spearman rank correlation, across all members,
                            between that 20-session return and the earlier
                            volatility (Kumar & Persaud's index), -1..+1
  cyclical_minus_defensive  equal-weight 20-session return of the cyclical
                            sectors minus the defensive sectors (GICS), pp

Volatility is the standard deviation of daily returns over the 60 sessions
BEFORE the 20-session window, so a stock's own recent move cannot make it
"risky". Each reading is ranked against its own previous 252 sessions (a
percentile, 0..100); the score is the risky_minus_safe rank.

Known limitation (Misina 2003, Bank of Canada): when stocks move together, a
rise in risk itself can look like a fall in appetite.

Returns use Yahoo's dividend- and split-adjusted close; the day's up / down
counts use the split-adjusted close, as quoted. Members are today's list
(Wikipedia), so the history carries the usual survivorship caveat. Missing
stocks are counted and named, never filled.

Usage: python market_breadth.py sentiment --cache-dir DIR [--force]
"""

from __future__ import annotations

import argparse
import io
import json
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
import pandas as pd

MEMBERS_URL = "https://en.wikipedia.org/wiki/List_of_S%26P_500_companies"
USER_AGENT = "MarketLab/1.0 (personal research terminal)"
MEMBERS_MAX_AGE = timedelta(days=7)
# Two years: 60 + 20 sessions to form the first reading, then a full previous
# 252 sessions to rank it against, then the 60 sessions of the chart.
HISTORY_PERIOD = "2y"
RANK_WINDOW = 252
RETURN_SESSIONS = 20
VOL_SESSIONS = 60
FIFTH = 0.2
MIN_STOCKS = 50
SCORE_HISTORY = 60

CYCLICAL = {"Consumer Discretionary", "Financials", "Industrials", "Materials", "Information Technology"}
DEFENSIVE = {"Consumer Staples", "Utilities", "Health Care"}

COMPONENTS = [
    ("risky_minus_safe", "Riskiest vs safest, 20d", "pp",
     "Return over the last 20 sessions of the most volatile fifth of S&P 500 stocks minus the least volatile "
     "fifth (volatility from the 60 sessions before), percentage points. This is the score."),
    ("rank_correlation", "Return vs earlier risk", "corr",
     "Rank correlation across all members between the 20-session return and the earlier volatility "
     "(Kumar & Persaud's risk-appetite index): +1 = the riskier, the better; −1 = the safer, the better."),
    ("cyclical_minus_defensive", "Cyclical vs defensive, 20d", "pp",
     "Equal-weight 20-session return of consumer discretionary, financials, industrials, materials and "
     "technology stocks minus consumer staples, utilities and health care, percentage points."),
]


def rating_for(score: float | None) -> str | None:
    if score is None:
        return None
    if score < 25:
        return "strongly avoiding risk"
    if score < 45:
        return "avoiding risk"
    if score <= 55:
        return "neutral"
    if score <= 75:
        return "seeking risk"
    return "strongly seeking risk"


# ── members ──────────────────────────────────────────────────────────────────

def load_members(cache: Path, now: datetime) -> tuple[list[str], dict[str, str], str, str | None]:
    """(symbols, GICS sector by symbol, fetched_at, note). Refreshed weekly; a
    failed refresh keeps the cached list and says so. The file also keeps
    each member's SEC CIK (member_ciks), which tells the share classes of one
    company apart (GOOGL / GOOG)."""
    path = cache / "members.json"
    cached = None
    if path.exists():
        try:
            cached = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            cached = None
    if cached and "ciks" in cached and now - datetime.fromisoformat(cached["fetched_at"]) < MEMBERS_MAX_AGE:
        return cached["symbols"], cached["sectors"], cached["fetched_at"], None
    try:
        import requests

        html = requests.get(MEMBERS_URL, headers={"User-Agent": USER_AGENT}, timeout=30).text
        table = pd.read_html(io.StringIO(html), attrs={"id": "constituents"})[0]
        sectors, ciks = {}, {}
        for sym, sector, cik in zip(table["Symbol"], table["GICS Sector"], table["CIK"]):
            s = str(sym).strip().replace(".", "-")
            if s:
                sectors[s] = str(sector).strip()
                ciks[s] = str(cik).strip().split(".")[0].zfill(10)
        symbols = sorted(sectors)
        if len(symbols) < 450:
            raise ValueError(f"only {len(symbols)} members parsed")
        fetched_at = now.isoformat(timespec="seconds")
        path.write_text(json.dumps({"fetched_at": fetched_at, "symbols": symbols, "sectors": sectors,
                                    "ciks": ciks}),
                        encoding="utf-8")
        return symbols, sectors, fetched_at, None
    except Exception as exc:  # noqa: BLE001
        if cached:
            return (cached["symbols"], cached.get("sectors", {}), cached["fetched_at"],
                    f"member list refresh failed ({exc}); using the list of {cached['fetched_at'][:10]}")
        raise


def member_ciks(cache: Path) -> dict[str, str]:
    """The cached members' CIKs by symbol (empty when not stored)."""
    try:
        return json.loads((cache / "members.json").read_text(encoding="utf-8")).get("ciks", {})
    except (OSError, ValueError):
        return {}


# ── bars ─────────────────────────────────────────────────────────────────────

FIELDS = ["Close", "Adj Close", "Volume"]


def _download(symbols: list[str], period: str) -> pd.DataFrame:
    """Long frame (date, symbol, Close, Adj Close, Volume) of daily bars."""
    import yfinance as yf

    if not symbols:
        return pd.DataFrame(columns=["date", "symbol", *FIELDS])
    raw = yf.download(symbols, period=period, interval="1d", auto_adjust=False, group_by="column", threads=True,
                      progress=False)
    if raw is None or raw.empty:
        return pd.DataFrame(columns=["date", "symbol", *FIELDS])
    if not isinstance(raw.columns, pd.MultiIndex):  # one symbol
        raw.columns = pd.MultiIndex.from_product([raw.columns, symbols])
    frames = []
    for field in FIELDS:
        part = raw[field].stack(future_stack=True).rename(field)
        frames.append(part)
    long = pd.concat(frames, axis=1).reset_index()
    long.columns = ["date", "symbol", *FIELDS]
    long["date"] = pd.to_datetime(long["date"]).dt.tz_localize(None).dt.normalize()
    return long.dropna(subset=["Close"])


def update_bars(cache: Path, symbols: list[str], force: bool) -> pd.DataFrame:
    """Cached history plus the latest five sessions; full history for members
    the cache lacks or whose adjusted history was re-scaled."""
    path = cache / "bars.parquet"
    bars = None
    if path.exists() and not force:
        try:
            bars = pd.read_parquet(path)
        except Exception:  # noqa: BLE001 - a broken cache is rebuilt
            bars = None
    # A cache from before the adjusted close was kept is rebuilt once.
    stale = (bars is None or bars.empty or "Adj Close" not in bars.columns
             or (pd.Timestamp.now().normalize() - bars["date"].max()).days > 5)
    if stale:
        bars = _download(symbols, HISTORY_PERIOD)
    if not bars.empty:
        # The latest sessions come from a short read: a long read made in the
        # evening has been seen to leave the day's session out.
        have = set(bars["symbol"].unique())
        new = [s for s in symbols if s not in have]
        recent = _download(symbols, "5d")
        # A transient failure drops symbols: retry those once.
        got = set(recent["symbol"].unique())
        retry = [s for s in symbols if s not in got]
        if retry:
            recent = pd.concat([recent, _download(retry, "5d")], ignore_index=True)
        # A new dividend or split re-scales a stock's whole adjusted history:
        # when its latest stored adjustment factor changed, it is re-read.
        joined = recent.merge(bars, on=["date", "symbol"], suffixes=("", "_old"))
        ratio_now = joined["Adj Close"] / joined["Close"]
        ratio_old = joined["Adj Close_old"] / joined["Close_old"]
        rescaled = sorted(set(joined.loc[(ratio_now / ratio_old - 1).abs() > 1e-6, "symbol"]))
        parts = [bars[~bars.set_index(["date", "symbol"]).index.isin(recent.set_index(["date", "symbol"]).index)
                      & ~bars["symbol"].isin(rescaled)],
                 recent[~recent["symbol"].isin(rescaled)]]
        if new or rescaled:
            parts.append(_download(sorted(set(new) | set(rescaled)), HISTORY_PERIOD))
        bars = pd.concat(parts, ignore_index=True)
    bars = bars[bars["symbol"].isin(symbols)].drop_duplicates(["date", "symbol"], keep="last")
    cutoff = bars["date"].max() - pd.Timedelta(days=760)
    bars = bars[bars["date"] >= cutoff].sort_values(["date", "symbol"]).reset_index(drop=True)
    tmp = path.with_suffix(".tmp")
    bars.to_parquet(tmp, index=False)
    tmp.replace(path)
    return bars


def complete_sessions(bars: pd.DataFrame) -> tuple[pd.DataFrame, list[str]]:
    """Drop trailing sessions with bars for fewer than half the stocks of the
    fullest recent session (a stray early bar), and say so."""
    per_day = bars.groupby("date")["symbol"].nunique().sort_index()
    full = per_day.tail(10).max()
    dropped = []
    while len(per_day) and per_day.iloc[-1] < 0.5 * full:
        dropped.append(f"{per_day.index[-1]:%Y-%m-%d} skipped: bars for only {per_day.iloc[-1]} stocks")
        per_day = per_day.iloc[:-1]
    if dropped:
        bars = bars[bars["date"] <= per_day.index[-1]]
    return bars, dropped


# ── readings ─────────────────────────────────────────────────────────────────

def daily_counts(bars: pd.DataFrame) -> pd.DataFrame:
    """One row per session: stocks up / down / flat / with a move, and the
    equal-weight average % change."""
    close = bars.pivot(index="date", columns="symbol", values="Close").sort_index()
    chg = close.pct_change(fill_method=None) * 100
    out = pd.DataFrame(index=close.index)
    out["up"] = (chg > 0).sum(axis=1)
    out["down"] = (chg < 0).sum(axis=1)
    out["observed"] = chg.notna().sum(axis=1)
    out["flat"] = out["observed"] - out["up"] - out["down"]
    out["today_move"] = chg.mean(axis=1)
    return out


def risk_readings(bars: pd.DataFrame, sectors: dict[str, str]) -> pd.DataFrame:
    """One row per session with the three risk-appetite readings."""
    adj = bars.pivot(index="date", columns="symbol", values="Adj Close").sort_index()
    daily = adj.pct_change(fill_method=None)
    vol = daily.rolling(VOL_SESSIONS, min_periods=VOL_SESSIONS - 10).std().shift(RETURN_SESSIONS)
    ret = (adj / adj.shift(RETURN_SESSIONS) - 1.0) * 100
    both = ret.notna() & vol.notna()
    r = ret.where(both)
    v = vol.where(both)

    vpct = v.rank(axis=1, pct=True)
    risky = r.where(vpct > 1 - FIFTH).mean(axis=1)
    safe = r.where(vpct <= FIFTH).mean(axis=1)

    rr = r.rank(axis=1)
    rv = v.rank(axis=1)
    rr = rr.sub(rr.mean(axis=1), axis=0)
    rv = rv.sub(rv.mean(axis=1), axis=0)
    denom = np.sqrt((rr ** 2).sum(axis=1) * (rv ** 2).sum(axis=1))
    corr = (rr * rv).sum(axis=1) / denom.replace(0, np.nan)

    sector = pd.Series(sectors, dtype=object).reindex(adj.columns)
    cyc = r.loc[:, sector.isin(CYCLICAL).to_numpy()].mean(axis=1)
    dfn = r.loc[:, sector.isin(DEFENSIVE).to_numpy()].mean(axis=1)

    out = pd.DataFrame(index=adj.index)
    out["stocks"] = both.sum(axis=1)
    enough = out["stocks"] >= MIN_STOCKS
    out["risky_return"] = risky.where(enough)
    out["safe_return"] = safe.where(enough)
    out["risky_minus_safe"] = (risky - safe).where(enough)
    out["rank_correlation"] = corr.where(enough)
    out["cyclical_minus_defensive"] = (cyc - dfn).where(enough)
    return out


def percentile_rank(history: pd.Series, value: float) -> float | None:
    """Share of the previous sessions at or below `value`, 0..100."""
    h = history.dropna()
    if len(h) < 60 or value is None or not np.isfinite(value):
        return None
    return float((h <= value).mean() * 100.0)


def ranks_at(readings: pd.DataFrame, i: int) -> dict:
    ranks = {}
    for key, *_ in COMPONENTS:
        prior = readings[key].iloc[max(0, i - RANK_WINDOW):i]
        ranks[key] = percentile_rank(prior, readings[key].iloc[i])
    return ranks


def build(bars: pd.DataFrame, members: list[str], sectors: dict[str, str], retrieved_at: str,
          notes: list[str]) -> dict:
    bars, skipped = complete_sessions(bars)
    notes = [*notes, *skipped]
    present = set(bars.loc[bars["date"] == bars["date"].max(), "symbol"])
    missing = sorted(s for s in members if s not in present)
    counts = daily_counts(bars)
    readings = risk_readings(bars, sectors)
    if readings["risky_minus_safe"].notna().sum() < 70:
        raise ValueError(f"only {len(readings)} sessions of history")
    last = len(readings) - 1
    ranks = ranks_at(readings, last)
    score = ranks["risky_minus_safe"]
    row = readings.iloc[last]
    today = counts.iloc[-1]

    def num(v, places=2):
        return None if v is None or not np.isfinite(v) else round(float(v), places)

    components = []
    for key, label, unit, detail in COMPONENTS:
        components.append({"key": key, "label": label, "unit": unit, "detail": detail,
                           "value": num(row[key], 3 if unit == "corr" else 2),
                           "percentile": None if ranks[key] is None else round(ranks[key], 1)})
    history = []
    for i in range(max(0, last - SCORE_HISTORY + 1), last + 1):
        s = ranks_at(readings, i)["risky_minus_safe"]
        if s is not None:
            history.append({"date": readings.index[i].strftime("%Y-%m-%d"), "score": round(s, 1)})
    if not sectors:
        notes = [*notes, "No sector list: the cyclical vs defensive reading is unavailable."]
    return {
        "source": "Yahoo daily bars of the S&P 500 members",
        "as_of": readings.index[last].strftime("%Y-%m-%d"),
        "retrieved_at": retrieved_at,
        "score": None if score is None else round(score, 1),
        "rating": rating_for(score),
        "members": len(members),
        "stocks_ranked": int(row["stocks"]),
        "risky_return": num(row["risky_return"]),
        "safe_return": num(row["safe_return"]),
        "observed": int(today["observed"]),
        "up": int(today["up"]),
        "down": int(today["down"]),
        "flat": int(today["flat"]),
        "today_avg_move": num(today["today_move"]),
        "missing": missing,
        "components": components,
        "history": history,
        "rank_window": RANK_WINDOW,
        "return_sessions": RETURN_SESSIONS,
        "volatility_sessions": VOL_SESSIONS,
        "notes": notes,
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["sentiment"])
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args(argv[1:])
    now = datetime.now(timezone.utc)
    try:
        cache = Path(args.cache_dir)
        cache.mkdir(parents=True, exist_ok=True)
        members, sectors, members_at, note = load_members(cache, now)
        notes = [note] if note else []
        notes.append(f"Members as listed on {members_at[:10]}; past-year history uses today's members.")
        bars = update_bars(cache, members, args.force)
        data = build(bars, members, sectors, now.isoformat(timespec="seconds"), notes)
    except Exception as exc:  # noqa: BLE001 - reported to the widget, never hidden
        print(json.dumps({"success": False, "error": f"Risk appetite: {type(exc).__name__}: {exc}"[:400]}))
        return 0
    print(json.dumps({"success": True, "data": data}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
