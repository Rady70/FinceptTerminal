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
    A NaN close is not a bar (counted in ``dropped_no_close``); no price is
    ever made up for it. A corporate action on such a row (a distribution or a
    split) is not dropped with it: it is returned in ``actions_without_close``
    [session_date, dividend, capital_gain, split_ratio] so it stays visible,
    though it is not applied to a return without a close. A NaN volume stays
    None (unknown), never 0. Dividend / capital-gain / split columns are event
    columns: an absent event is 0 (no distribution, no split), which is what
    Yahoo's event list means. In-progress sessions are dropped.
    """
    rows = []
    actions_without_close = []
    dropped_no_close = 0
    in_progress = 0
    if frame is None or len(frame) == 0:
        return {"rows": rows, "dropped_no_close": 0, "in_progress_excluded": 0, "actions_without_close": []}
    cols = set(frame.columns)
    for ts, rec in frame.iterrows():
        session = ts.date() if hasattr(ts, "date") else _dt.date.fromisoformat(str(ts)[:10])
        close = _finite(rec.get("Close")) if "Close" in cols else None
        if close is None or close <= 0:
            dropped_no_close += 1
            div = _finite(rec.get("Dividends")) if "Dividends" in cols else None
            cg = _finite(rec.get("Capital Gains")) if "Capital Gains" in cols else None
            split = _finite(rec.get("Stock Splits")) if "Stock Splits" in cols else None
            if any(x for x in (div, cg, split)):
                actions_without_close.append([session.isoformat(), div or 0.0, cg or 0.0, split or 0.0])
            continue
        if not sessions.is_completed_session(symbol, session, retrieved_at_utc):
            in_progress += 1
            continue
        volume = _finite(rec.get("Volume")) if "Volume" in cols else None
        div = _finite(rec.get("Dividends")) if "Dividends" in cols else None
        cg = _finite(rec.get("Capital Gains")) if "Capital Gains" in cols else None
        split = _finite(rec.get("Stock Splits")) if "Stock Splits" in cols else None
        rows.append([session.isoformat(), close, volume, div or 0.0, cg or 0.0, split or 0.0])
    return {"rows": rows, "dropped_no_close": dropped_no_close, "in_progress_excluded": in_progress,
            "actions_without_close": actions_without_close}


# yfinance errors (as ``repr`` in ``shared._ERRORS``) that say "no data for this
# symbol". Usually asking again cannot cure them, but not always: yfinance first
# looks up the symbol's time zone and, when that request times out or the
# connection drops, hides the cause and reports "possibly delisted; no timezone
# found". So they are retried once, but never counted as provider-wide failures.
_NO_DATA_ERRORS = ("YFPricesMissingError", "YFTzMissingError", "YFTickerMissingError", "YFInvalidPeriodError")


def download_history(symbols, period, retrieved_at_utc: _dt.datetime, chunk: int = 40, yf_module=None,
                     start=None):
    """Daily history for ``symbols``; one result entry per requested symbol.

    ``start`` (an ISO date) requests only the sessions from that date: an
    incremental delivery, marked so the store can join it to stored history.

    yfinance fetches each symbol of a batch on its own and does not raise for a
    symbol that fails: it records the error in ``shared._ERRORS`` and returns no
    bars for it. Such a symbol (no bars and a provider error), and every symbol
    of a batch request that raised as a whole, is retried alone once, so a
    transient error (throttling, a timeout, a connection error) does not cost it
    this refresh. An error that says the symbol has no data (yfinance's
    ``YFPricesMissingError`` / ``YFTzMissingError`` family, "possibly delisted",
    or an invalid period) is retried too, because yfinance also reports a timed-
    out time-zone lookup that way, but it is not counted below: delisted symbols
    must not stop the retries of others. Two retried symbols failing in a row
    with any other error are taken as a provider-wide failure: the retries stop,
    and the symbols not retried keep their first error. A symbol that answered
    without a completed bar and without an error is not retried.
    """
    import pandas as pd  # noqa: F401  (yfinance returns pandas frames)

    if yf_module is None:
        import yfinance as yf_module  # type: ignore[no-redef]

    span = {"start": start} if start else {"period": period}

    def fetch(batch):
        raw = yf_module.download(batch, interval="1d", auto_adjust=False, actions=True, progress=False,
                                 threads=4, group_by="column", **span)
        try:
            errors = dict(getattr(yf_module, "shared")._ERRORS)
        except Exception:
            errors = {}
        return raw, errors

    def provider_error(s, errors):
        return errors.get(s) or errors.get(s.upper())

    def counts_as_provider_failure(s, errors):
        """A provider error other than "no data for this symbol"."""
        why = str(provider_error(s, errors) or "")
        return bool(why) and not why.startswith(_NO_DATA_ERRORS)

    out = {}

    def convert(s, raw, errors):
        """Store one symbol's result in ``out``; True when it has bars."""
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
            return False
        if not conv["rows"]:
            why = provider_error(s, errors) or "no completed session with a close was returned"
            out[s] = {"status": "FAILED", "detail": str(why), "rows": [],
                      "dropped_no_close": conv["dropped_no_close"],
                      "in_progress_excluded": conv["in_progress_excluded"],
                      "actions_without_close": conv["actions_without_close"]}
            return False
        out[s] = {"status": "OK", "detail": "", "period": period, **conv}
        if start:
            out[s]["incremental"] = True
            out[s]["requested_start"] = start
        return True

    def fail(s, why, note=""):
        out[s] = {"status": "FAILED", "detail": f"download failed: {why}{note}", "rows": []}

    syms = sorted(set(symbols))
    for offset in range(0, len(syms), chunk):
        batch = syms[offset:offset + chunk]
        try:
            raw, errors = fetch(batch)
        except Exception as exc:  # the whole request failed
            for s in batch:
                fail(s, exc)
            retry = list(batch)
        else:
            retry = [s for s in batch if not convert(s, raw, errors) and provider_error(s, errors)]
        failed_in_row = 0
        for i, s in enumerate(retry):
            if failed_in_row == 2:
                for rest in retry[i:]:
                    out[rest]["detail"] += " (not retried alone: two single retries failed in a row)"
                break
            try:
                raw, errors = fetch([s])
            except Exception as exc:
                failed_in_row += 1
                fail(s, exc, " (retried alone)")
                continue
            if convert(s, raw, errors):
                failed_in_row = 0
            elif counts_as_provider_failure(s, errors):
                failed_in_row += 1
                out[s]["detail"] += " (retried alone)"
            else:
                # "No data" again, or an answer without a completed bar: not
                # counted toward the stop (and not taken as a sign of health).
                out[s]["detail"] += " (retried alone)"
    return out


def fund_snapshot(symbol: str, ticker_factory=None) -> dict:
    """Quote-summary fund fields, top holdings and sector weights of one fund."""
    if ticker_factory is None:
        import yfinance as yf

        ticker_factory = yf.Ticker
    # The quote summary (``info``) and the fund holdings (``funds_data``) are
    # separate Yahoo surfaces: each is attempted and classified on its own, so
    # a failed quote never hides holdings that are available, and vice versa.
    out = {"status": "OK", "detail": "", "fields": {}, "holdings": [], "sector_weights": {},
           "holdings_status": "UNAVAILABLE", "holdings_detail": "", "quote_status": "OK"}
    try:
        tk = ticker_factory(symbol)
    except Exception as exc:
        return {"status": "FAILED", "detail": f"ticker unavailable: {exc}", "fields": {}, "holdings": [],
                "sector_weights": {}, "holdings_status": "FAILED", "holdings_detail": "", "quote_status": "FAILED"}
    try:
        info = tk.info or {}
    except Exception as exc:
        info = {}
        out["detail"] = f"quote summary failed: {exc}"
    fields = {k: _clean_field(info.get(k)) for k in FUND_FIELDS if info.get(k) is not None}
    out["fields"] = fields
    if not fields:
        out["quote_status"] = "FAILED"
        out["detail"] = out["detail"] or "no quote-summary fields returned"
    # Holdings and sector weights come from one yfinance parse of Yahoo's fund
    # profile. yfinance gives up on the whole profile when it cannot read it
    # (e.g. one listed holding without a symbol raises "No Fund data found"; other
    # errors are logged and leave ``top_holdings`` as None). That is a failed
    # read, not a fund that publishes nothing: it is FAILED and the item PARTIAL
    # (MarketLab cannot read the rest without modifying yfinance; a recorded
    # limitation). Only a successful, empty parse is UNAVAILABLE.
    try:
        fd = tk.funds_data
        th = fd.top_holdings
        if th is None:
            raise ValueError("yfinance returned no holdings table (the fund profile could not be parsed)")
        holdings = []
        for rank, (hsym, rec) in enumerate(th.iterrows(), start=1):
            weight = _finite(rec.get("Holding Percent"))
            holdings.append([rank, str(hsym), str(rec.get("Name") or ""), weight])
        sw = fd.sector_weightings or {}
        out["holdings"] = holdings
        out["sector_weights"] = {str(k): _finite(v) for k, v in sw.items() if _finite(v) is not None}
        # A sector weight that is not a number is left out alone, counted.
        out["sector_weights_unparseable"] = sum(1 for v in sw.values() if _finite(v) is None)
        out["holdings_status"] = "OK" if (holdings or out["sector_weights"]) else "UNAVAILABLE"
        if out["holdings_status"] == "UNAVAILABLE":
            out["holdings_detail"] = "Yahoo published no holdings or sector weights for this fund"
    except Exception as exc:
        out["holdings_status"] = "FAILED"
        out["holdings_detail"] = f"fund holdings could not be read: {exc}"
    if out["quote_status"] == "FAILED" and out["holdings_status"] != "OK":
        out["status"] = "FAILED"  # nothing of the fund was obtained
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
