"""Market-wide breadth sentiment over every S&P 500 member (Yahoo daily bars).

Five readings of how the whole market is behaving, each ranked against its own
previous 252 sessions (a percentile, 0..100), and the score is their average:

  advancers   (stocks up - stocks down) / stocks with a move, %, averaged
              over the last 5 sessions
  up_volume   (volume of rising - falling stocks) / both, %, last 5 sessions
  avg_move    equal-weight average % change over the last 5 sessions
  above_50d   % of stocks above their 50-day average close
  net_highs   (new 52-week highs - new lows) / stocks, %

The three daily-flow readings use 5 sessions so one day's swing does not flip
the score; today's own up / down counts are reported alongside.

A percentile says how today compares with the past year, so no fixed
threshold has to be guessed. Members are today's list (Wikipedia), so the
past-year history carries the usual survivorship caveat. Missing stocks are
counted and named, never filled.

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
# Two years: every reading (the 52-week window needs ~200 sessions of warm-up)
# is then ranked against a full previous 252 sessions.
HISTORY_PERIOD = "2y"
RANK_WINDOW = 252
FLOW_SESSIONS = 5
SCORE_HISTORY = 60

COMPONENTS = [
    ("advancers", "Rising vs falling, 5d", "% net",
     "(stocks up − stocks down) ÷ stocks with a move, averaged over the last 5 sessions"),
    ("up_volume", "Up vs down volume, 5d", "% net",
     "(volume of rising − falling stocks) ÷ both, summed over the last 5 sessions"),
    ("avg_move", "Average move, 5d", "%", "Equal-weight average % change of all members over 5 sessions"),
    ("above_50d", "Above 50-day average", "%", "Share of members closing above their 50-day average"),
    ("net_highs", "52-wk highs − lows", "% net", "(new 52-week highs − new lows) ÷ members"),
]


def rating_for(score: float | None) -> str | None:
    if score is None:
        return None
    if score < 25:
        return "extreme fear"
    if score < 45:
        return "fear"
    if score <= 55:
        return "neutral"
    if score <= 75:
        return "greed"
    return "extreme greed"


# ── members ──────────────────────────────────────────────────────────────────

def load_members(cache: Path, now: datetime) -> tuple[list[str], str, str | None]:
    """(symbols, fetched_at, note). Refreshed weekly; a failed refresh keeps
    the cached list and says so."""
    path = cache / "members.json"
    cached = None
    if path.exists():
        try:
            cached = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            cached = None
    if cached and now - datetime.fromisoformat(cached["fetched_at"]) < MEMBERS_MAX_AGE:
        return cached["symbols"], cached["fetched_at"], None
    try:
        import requests

        html = requests.get(MEMBERS_URL, headers={"User-Agent": USER_AGENT}, timeout=30).text
        table = pd.read_html(io.StringIO(html), attrs={"id": "constituents"})[0]
        symbols = sorted({str(s).strip().replace(".", "-") for s in table["Symbol"] if str(s).strip()})
        if len(symbols) < 450:
            raise ValueError(f"only {len(symbols)} members parsed")
        fetched_at = now.isoformat(timespec="seconds")
        path.write_text(json.dumps({"fetched_at": fetched_at, "symbols": symbols}), encoding="utf-8")
        return symbols, fetched_at, None
    except Exception as exc:  # noqa: BLE001
        if cached:
            return cached["symbols"], cached["fetched_at"], f"member list refresh failed ({exc}); using the list of {cached['fetched_at'][:10]}"
        raise


# ── bars ─────────────────────────────────────────────────────────────────────

FIELDS = ["High", "Low", "Close", "Volume"]


def _download(symbols: list[str], period: str) -> pd.DataFrame:
    """Long frame (date, symbol, High, Low, Close, Volume) of unadjusted daily bars."""
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


def update_bars(cache: Path, symbols: list[str], force: bool) -> tuple[pd.DataFrame, list[str]]:
    """Cached history plus the latest five sessions; full history for members
    the cache lacks. Returns (bars, symbols that returned no data)."""
    path = cache / "bars.parquet"
    bars = None
    if path.exists() and not force:
        try:
            bars = pd.read_parquet(path)
        except Exception:  # noqa: BLE001 - a broken cache is rebuilt
            bars = None
    stale = bars is None or bars.empty or (pd.Timestamp.now().normalize() - bars["date"].max()).days > 5
    if stale:
        bars = _download(symbols, HISTORY_PERIOD)
    else:
        have = set(bars["symbol"].unique())
        new = [s for s in symbols if s not in have]
        recent = _download(symbols, "5d")
        # A transient failure drops symbols: retry those once.
        got = set(recent["symbol"].unique())
        retry = [s for s in symbols if s not in got]
        if retry:
            recent = pd.concat([recent, _download(retry, "5d")], ignore_index=True)
        parts = [bars[~bars.set_index(["date", "symbol"]).index.isin(recent.set_index(["date", "symbol"]).index)],
                 recent]
        if new:
            parts.append(_download(new, HISTORY_PERIOD))
        bars = pd.concat(parts, ignore_index=True)
    bars = bars[bars["symbol"].isin(symbols)].drop_duplicates(["date", "symbol"], keep="last")
    cutoff = bars["date"].max() - pd.Timedelta(days=760)
    bars = bars[bars["date"] >= cutoff].sort_values(["date", "symbol"]).reset_index(drop=True)
    tmp = path.with_suffix(".tmp")
    bars.to_parquet(tmp, index=False)
    tmp.replace(path)
    latest = bars["date"].max()
    present = set(bars.loc[bars["date"] == latest, "symbol"])
    return bars, sorted(s for s in symbols if s not in present)


# ── readings ─────────────────────────────────────────────────────────────────

def daily_readings(bars: pd.DataFrame) -> pd.DataFrame:
    """One row per session with the five readings and the counts."""
    close = bars.pivot(index="date", columns="symbol", values="Close").sort_index()
    high = bars.pivot(index="date", columns="symbol", values="High").reindex_like(close)
    low = bars.pivot(index="date", columns="symbol", values="Low").reindex_like(close)
    vol = bars.pivot(index="date", columns="symbol", values="Volume").reindex_like(close)

    chg = close.pct_change(fill_method=None) * 100
    moved = chg.notna()
    up = (chg > 0).sum(axis=1)
    down = (chg < 0).sum(axis=1)
    observed = moved.sum(axis=1)
    upvol = vol.where(chg > 0).sum(axis=1)
    downvol = vol.where(chg < 0).sum(axis=1)
    ma50 = close.rolling(50, min_periods=50).mean()
    has_ma = ma50.notna() & close.notna()
    hi = high.rolling(252, min_periods=200).max()
    lo = low.rolling(252, min_periods=200).min()
    has_hl = hi.notna() & lo.notna()

    out = pd.DataFrame(index=close.index)
    net_adv = ((up - down) / observed.replace(0, np.nan)) * 100
    out["advancers"] = net_adv.rolling(FLOW_SESSIONS, min_periods=FLOW_SESSIONS).mean()
    up5 = upvol.rolling(FLOW_SESSIONS, min_periods=FLOW_SESSIONS).sum()
    down5 = downvol.rolling(FLOW_SESSIONS, min_periods=FLOW_SESSIONS).sum()
    out["up_volume"] = ((up5 - down5) / (up5 + down5).replace(0, np.nan)) * 100
    out["avg_move"] = (close.pct_change(FLOW_SESSIONS, fill_method=None) * 100).mean(axis=1)
    out["today_move"] = chg.mean(axis=1)
    out["above_50d"] = ((close > ma50) & has_ma).sum(axis=1) / has_ma.sum(axis=1).replace(0, np.nan) * 100
    out["net_highs"] = (((high >= hi) & has_hl).sum(axis=1) - ((low <= lo) & has_hl).sum(axis=1)) / \
        has_hl.sum(axis=1).replace(0, np.nan) * 100
    out["up"] = up
    out["down"] = down
    out["flat"] = observed - up - down
    out["observed"] = observed
    return out


def percentile_rank(history: pd.Series, value: float) -> float | None:
    """Share of the previous sessions at or below `value`, 0..100."""
    h = history.dropna()
    if len(h) < 60 or value is None or not np.isfinite(value):
        return None
    return float((h <= value).mean() * 100.0)


def score_at(readings: pd.DataFrame, i: int) -> tuple[float | None, dict]:
    ranks = {}
    for key, *_ in COMPONENTS:
        prior = readings[key].iloc[max(0, i - RANK_WINDOW):i]
        ranks[key] = percentile_rank(prior, readings[key].iloc[i])
    usable = [v for v in ranks.values() if v is not None]
    return (float(np.mean(usable)) if len(usable) >= 3 else None), ranks


def build(bars: pd.DataFrame, members: list[str], missing: list[str], retrieved_at: str, notes: list[str]) -> dict:
    readings = daily_readings(bars)
    if len(readings) < 70:
        raise ValueError(f"only {len(readings)} sessions of history")
    last = len(readings) - 1
    score, ranks = score_at(readings, last)
    row = readings.iloc[last]

    def num(v):
        return None if v is None or not np.isfinite(v) else round(float(v), 2)

    components = []
    for key, label, unit, detail in COMPONENTS:
        components.append({"key": key, "label": label, "unit": unit, "detail": detail, "value": num(row[key]),
                           "percentile": None if ranks[key] is None else round(ranks[key], 1)})
    history = []
    for i in range(max(RANK_WINDOW // 4, last - SCORE_HISTORY + 1), last + 1):
        s, _ = score_at(readings, i)
        if s is not None:
            history.append({"date": readings.index[i].strftime("%Y-%m-%d"), "score": round(s, 1)})
    return {
        "source": "Yahoo daily bars of the S&P 500 members",
        "as_of": readings.index[last].strftime("%Y-%m-%d"),
        "retrieved_at": retrieved_at,
        "score": None if score is None else round(score, 1),
        "rating": rating_for(score),
        "members": len(members),
        "observed": int(row["observed"]),
        "up": int(row["up"]),
        "down": int(row["down"]),
        "flat": int(row["flat"]),
        "today_avg_move": num(row["today_move"]),
        "missing": missing,
        "components": components,
        "history": history,
        "rank_window": RANK_WINDOW,
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
        members, members_at, note = load_members(cache, now)
        notes = [note] if note else []
        notes.append(f"Members as listed on {members_at[:10]}; past-year history uses today's members.")
        bars, missing = update_bars(cache, members, args.force)
        data = build(bars, members, missing, now.isoformat(timespec="seconds"), notes)
    except Exception as exc:  # noqa: BLE001 - reported to the widget, never hidden
        print(json.dumps({"success": False, "error": f"Market breadth: {type(exc).__name__}: {exc}"[:400]}))
        return 0
    print(json.dumps({"success": True, "data": data}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
