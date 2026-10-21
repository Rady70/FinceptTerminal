"""CNN Fear & Greed Index collector.

Reads the JSON feed the CNN Fear & Greed page draws from
(production.dataviz.cnn.io/index/fearandgreed/graphdata) and returns CNN's own
values: the index, its earlier readings and its seven components. Nothing is
recomputed here. The owner reviewed CNN's terms before this collector was added
(2026-10-21). The feed is undocumented, so parsing is by meaning and tolerant:
a value is dropped (reported as missing) only when it is not a number in 0..100.

Usage: python cnn_fear_greed.py index
"""

from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from typing import Any

URL = "https://production.dataviz.cnn.io/index/fearandgreed/graphdata"
# The feed answers browser requests only (HTTP 418 otherwise).
HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0 Safari/537.36",
    "Accept": "application/json",
    "Referer": "https://edition.cnn.com/",
}

# CNN's seven components, in CNN's order, with plain names.
COMPONENTS = [
    ("market_momentum_sp500", "Market momentum", "S&P 500 vs its 125-day average"),
    ("stock_price_strength", "Stock price strength", "NYSE 52-week highs vs lows"),
    ("stock_price_breadth", "Stock price breadth", "McClellan volume summation"),
    ("put_call_options", "Put and call options", "5-day put/call ratio"),
    ("market_volatility_vix", "Market volatility", "VIX vs its 50-day average"),
    ("safe_haven_demand", "Safe-haven demand", "Stocks vs bonds, 20 days"),
    ("junk_bond_demand", "Junk-bond demand", "Junk vs investment-grade yield spread"),
]

# What each component's chart plots (CNN's own series), and the second line
# CNN draws with it where there is one.
SERIES = {
    "market_momentum_sp500": ("S&P 500", "market_momentum_sp125", "125-day average"),
    "stock_price_strength": ("Net new 52-week highs, %", None, None),
    "stock_price_breadth": ("McClellan volume summation", None, None),
    "put_call_options": ("Put/call ratio, 5-day average", None, None),
    "market_volatility_vix": ("VIX", "market_volatility_vix_50", "50-day average"),
    "safe_haven_demand": ("Stocks minus bonds, 20-day return, pp", None, None),
    "junk_bond_demand": ("Junk minus investment-grade yield, pp", None, None),
}

PREVIOUS = [
    ("previous_close", "Previous close"),
    ("previous_1_week", "1 week ago"),
    ("previous_1_month", "1 month ago"),
    ("previous_1_year", "1 year ago"),
]


def _score(value: Any) -> float | None:
    """A 0..100 score, or None when absent or not a valid score."""
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    if v != v or v < 0 or v > 100:  # NaN or out of range
        return None
    return round(v, 1)


def _number(value: Any) -> float | None:
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    return None if v != v else v


def _when(value: Any) -> str | None:
    """ISO 8601 UTC from an ISO string or epoch milliseconds."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        try:
            return datetime.fromtimestamp(float(value) / 1000.0, tz=timezone.utc).isoformat(timespec="seconds")
        except (OverflowError, OSError, ValueError):
            return None
    text = str(value).strip()
    try:
        dt = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc).isoformat(timespec="seconds")


def rating_for(score: float | None) -> str | None:
    """CNN's bands, used only when the feed omits the rating."""
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


def _series(block: Any) -> list[dict]:
    """A component's plotted series: one {date, value} per day (latest wins);
    points without a time or a number are dropped."""
    data = block.get("data") if isinstance(block, dict) else None
    by_day: dict[str, float] = {}
    if isinstance(data, list):
        for p in data:
            if not isinstance(p, dict):
                continue
            when = _when(p.get("x"))
            v = _number(p.get("y"))
            if when and v is not None:
                by_day[when[:10]] = v
    return [{"date": d, "value": by_day[d]} for d in sorted(by_day)]


def _rating(raw: Any, score: float | None) -> str | None:
    text = str(raw).strip().lower() if isinstance(raw, str) and raw.strip() else None
    return text or rating_for(score)


def parse(payload: Any, retrieved_at: str) -> dict:
    """CNN feed JSON -> the widget's shape. Raises ValueError when the index
    itself is missing (no usable reading)."""
    if not isinstance(payload, dict):
        raise ValueError("feed is not a JSON object")
    head = payload.get("fear_and_greed")
    if not isinstance(head, dict):
        raise ValueError("feed has no fear_and_greed block")
    score = _score(head.get("score"))
    if score is None:
        raise ValueError(f"index score missing or invalid: {head.get('score')!r}")

    missing: list[str] = []
    previous = []
    for key, label in PREVIOUS:
        v = _score(head.get(key))
        if v is None:
            missing.append(key)
        previous.append({"key": key, "label": label, "score": v, "rating": rating_for(v)})

    components = []
    for key, label, detail in COMPONENTS:
        block = payload.get(key)
        block = block if isinstance(block, dict) else {}
        s = _score(block.get("score"))
        if s is None:
            missing.append(key)
        latest_value = None
        data = block.get("data")
        if isinstance(data, list) and data and isinstance(data[-1], dict):
            latest_value = _number(data[-1].get("y"))
        series_label, companion_key, companion_label = SERIES[key]
        companion = _series(payload.get(companion_key)) if companion_key else []
        components.append({
            "series_label": series_label,
            "series": _series(block),
            "companion_label": companion_label if companion else None,
            "companion": companion,
            "key": key,
            "label": label,
            "detail": detail,
            "score": s,
            "rating": _rating(block.get("rating"), s),
            "updated_at": _when(block.get("timestamp")),
            "value": latest_value,
        })

    history = []
    hist = payload.get("fear_and_greed_historical")
    points = hist.get("data") if isinstance(hist, dict) else None
    if isinstance(points, list):
        by_day: dict[str, float] = {}
        for p in points:
            if not isinstance(p, dict):
                continue
            when = _when(p.get("x"))
            s = _score(p.get("y"))
            if when and s is not None:
                by_day[when[:10]] = s  # one reading per day; the latest wins
        history = [{"date": d, "score": by_day[d]} for d in sorted(by_day)]

    return {
        "source": "CNN Fear & Greed Index",
        "source_url": "https://edition.cnn.com/markets/fear-and-greed",
        "retrieved_at": retrieved_at,
        "score": score,
        "rating": _rating(head.get("rating"), score),
        "updated_at": _when(head.get("timestamp")),
        "previous": previous,
        "components": components,
        "history": history,
        "missing": missing,
    }


def fetch() -> Any:
    import requests

    response = requests.get(URL, headers=HEADERS, timeout=30)
    response.raise_for_status()
    return response.json()


def main(argv: list[str]) -> int:
    command = argv[1] if len(argv) > 1 else "index"
    if command != "index":
        print(json.dumps({"success": False, "error": f"unknown command: {command}"}))
        return 1
    retrieved_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    try:
        data = parse(fetch(), retrieved_at)
    except Exception as exc:  # noqa: BLE001 - reported to the widget, never hidden
        print(json.dumps({"success": False, "error": f"CNN Fear & Greed: {type(exc).__name__}: {exc}"[:400]}))
        return 0
    print(json.dumps({"success": True, "data": data}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
