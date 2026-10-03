"""Yahoo (yfinance) observations for the ETF research universe.

Three observation families, each returned with its own status per symbol:

* daily history  - close (split-adjusted, NOT dividend-adjusted), volume,
  cash dividends, capital-gain distributions and split ratios by session date;
  only completed sessions are returned (``sessions.is_completed_session``);
* fund snapshot  - the quote-summary fields Yahoo exposes for a fund at the
  moment of the request (AUM, NAV, metadata), plus top holdings and sector
  weights. Yahoo does not date AUM or NAV; the application records the capture
  time and assigns an effective session by a named rule;
* fundamentals   - quote-summary valuation fields of constituent stocks.

The raw Yahoo field names are kept so the application can apply one explicit
unit table. A field Yahoo did not send is absent, never zero.
"""

from __future__ import annotations

import datetime as _dt
import math

from etf_research import sessions

FUND_FIELDS = (
    "quoteType", "legalType", "longName", "shortName", "fundFamily", "category", "exchange",
    "fullExchangeName", "currency", "exchangeTimezoneName", "totalAssets", "netAssets", "navPrice",
    "previousClose", "regularMarketPreviousClose", "regularMarketPrice", "regularMarketTime",
    "sharesOutstanding", "netExpenseRatio", "annualReportExpenseRatio", "yield", "dividendYield",
    "trailingAnnualDividendYield", "trailingPE", "priceToBook", "beta3Year", "ytdReturn",
    "threeYearAverageReturn", "fiveYearAverageReturn", "trailingThreeMonthReturns",
    "trailingThreeMonthNavReturns", "fundInceptionDate", "averageVolume", "marketState",
)

FUNDAMENTAL_FIELDS = (
    "quoteType", "shortName", "longName", "currency", "sector", "industry", "trailingPE", "forwardPE",
    "trailingPegRatio", "pegRatio", "debtToEquity", "marketCap", "regularMarketTime",
)


def _finite(value):
    """A float, or None for anything that is not a finite number."""
    if value is None or isinstance(value, bool):
        return None
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def _clean_field(value):
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return _finite(value)
    if isinstance(value, str):
        return value
    if value is None:
        return None
    return str(value)


def frame_to_bars(symbol: str, frame, retrieved_at_utc: _dt.datetime) -> dict:
    """Convert one symbol's yfinance OHLCV/actions frame to stored bar rows.

    Rows: [session_date, close, volume|None, dividend, capital_gain, split_ratio].
    A NaN close is not a bar (counted in ``dropped_no_close``). A NaN volume
    stays None (unknown), never 0. Dividend / capital-gain / split columns are
    event columns: an absent event is 0 (no distribution, no split), which is
    what Yahoo's event list means. In-progress sessions are dropped.
    """
    rows = []
    dropped_no_close = 0
    in_progress = 0
    if frame is None or len(frame) == 0:
        return {"rows": rows, "dropped_no_close": 0, "in_progress_excluded": 0}
    cols = set(frame.columns)
    for ts, rec in frame.iterrows():
        session = ts.date() if hasattr(ts, "date") else _dt.date.fromisoformat(str(ts)[:10])
        close = _finite(rec.get("Close")) if "Close" in cols else None
        if close is None or close <= 0:
            dropped_no_close += 1
            continue
        if not sessions.is_completed_session(symbol, session, retrieved_at_utc):
            in_progress += 1
            continue
        volume = _finite(rec.get("Volume")) if "Volume" in cols else None
        div = _finite(rec.get("Dividends")) if "Dividends" in cols else None
        cg = _finite(rec.get("Capital Gains")) if "Capital Gains" in cols else None
        split = _finite(rec.get("Stock Splits")) if "Stock Splits" in cols else None
        rows.append([session.isoformat(), close, volume, div or 0.0, cg or 0.0, split or 0.0])
    return {"rows": rows, "dropped_no_close": dropped_no_close, "in_progress_excluded": in_progress}


def download_history(symbols, period, retrieved_at_utc: _dt.datetime, chunk: int = 40, yf_module=None,
                     start=None):
    """Daily history for ``symbols``; one result entry per requested symbol.

    ``start`` (an ISO date) requests only the sessions from that date: an
    incremental delivery, marked so the store can join it to stored history.
    """
    import pandas as pd  # noqa: F401  (yfinance returns pandas frames)

    if yf_module is None:
        import yfinance as yf_module  # type: ignore[no-redef]

    out = {}
    syms = sorted(set(symbols))
    for offset in range(0, len(syms), chunk):
        batch = syms[offset:offset + chunk]
        try:
            span = {"start": start} if start else {"period": period}
            raw = yf_module.download(batch, interval="1d", auto_adjust=False, actions=True, progress=False,
                                     threads=4, group_by="column", **span)
        except Exception as exc:  # the whole request failed
            for s in batch:
                out[s] = {"status": "FAILED", "detail": f"download failed: {exc}", "rows": []}
            continue
        errors = {}
        try:
            errors = dict(getattr(yf_module, "shared")._ERRORS)
        except Exception:
            errors = {}
        for s in batch:
            try:
                if raw is None or len(raw) == 0:
                    frame = None
                elif hasattr(raw.columns, "levels"):
                    frame = raw.xs(s, axis=1, level=1) if s in raw.columns.get_level_values(1) else None
                else:
                    frame = raw
                conv = frame_to_bars(s, frame, retrieved_at_utc)
            except Exception as exc:
                out[s] = {"status": "FAILED", "detail": f"conversion failed: {exc}", "rows": []}
                continue
            if not conv["rows"]:
                why = errors.get(s) or "no completed session with a close was returned"
                out[s] = {"status": "FAILED", "detail": str(why), "rows": [],
                          "dropped_no_close": conv["dropped_no_close"],
                          "in_progress_excluded": conv["in_progress_excluded"]}
                continue
            out[s] = {"status": "OK", "detail": "", "period": period, **conv}
            if start:
                out[s]["incremental"] = True
                out[s]["requested_start"] = start
    return out


def fund_snapshot(symbol: str, ticker_factory=None) -> dict:
    """Quote-summary fund fields, top holdings and sector weights of one fund."""
    if ticker_factory is None:
        import yfinance as yf

        ticker_factory = yf.Ticker
    out = {"status": "OK", "detail": "", "fields": {}, "holdings": [], "sector_weights": {},
           "holdings_status": "UNAVAILABLE", "holdings_detail": ""}
    try:
        tk = ticker_factory(symbol)
        info = tk.info or {}
    except Exception as exc:
        return {"status": "FAILED", "detail": f"quote summary failed: {exc}", "fields": {}, "holdings": [],
                "sector_weights": {}, "holdings_status": "FAILED", "holdings_detail": ""}
    fields = {k: _clean_field(info.get(k)) for k in FUND_FIELDS if info.get(k) is not None}
    out["fields"] = fields
    if not fields:
        out["status"] = "FAILED"
        out["detail"] = "no quote-summary fields returned"
        return out
    try:
        fd = tk.funds_data
        th = fd.top_holdings
        holdings = []
        if th is not None and len(th) > 0:
            for rank, (hsym, rec) in enumerate(th.iterrows(), start=1):
                weight = _finite(rec.get("Holding Percent"))
                holdings.append([rank, str(hsym), str(rec.get("Name") or ""), weight])
        sw = fd.sector_weightings or {}
        out["holdings"] = holdings
        out["sector_weights"] = {str(k): _finite(v) for k, v in sw.items() if _finite(v) is not None}
        out["holdings_status"] = "OK" if (holdings or out["sector_weights"]) else "UNAVAILABLE"
        if out["holdings_status"] == "UNAVAILABLE":
            out["holdings_detail"] = "Yahoo published no holdings or sector weights for this fund"
    except Exception as exc:
        out["holdings_status"] = "UNAVAILABLE"
        out["holdings_detail"] = f"fund holdings unavailable: {exc}"
    return out


def fundamentals(symbol: str, ticker_factory=None) -> dict:
    if ticker_factory is None:
        import yfinance as yf

        ticker_factory = yf.Ticker
    try:
        info = ticker_factory(symbol).info or {}
    except Exception as exc:
        return {"status": "FAILED", "detail": f"quote summary failed: {exc}", "fields": {}}
    fields = {k: _clean_field(info.get(k)) for k in FUNDAMENTAL_FIELDS if info.get(k) is not None}
    if not fields:
        return {"status": "FAILED", "detail": "no quote-summary fields returned", "fields": {}}
    return {"status": "OK", "detail": "", "fields": fields}
