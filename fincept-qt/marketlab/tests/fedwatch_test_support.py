"""Shared offline harness for the MarketLab FedWatch fixture tests.

Every test exercises the real MarketLab-owned parsing, validation and
comparison functions with a fake HTTP transport and captured source fixtures.
No test opens a network connection; live-provider qualification is performed
separately and reported distinctly (see FEDWATCH_BATCH_A_IMPLEMENTATION.md in
the Rady70/Market_Lab control repository).
"""

from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_SCRIPTS = _HERE.parents[1] / "scripts"
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from fedwatch.transport import Transport, TransportError  # noqa: E402

FIXTURES = _HERE / "fixtures" / "fedwatch"

FIXTURE_INVESTING_LIVE = "investing_fed_rate_monitor_2026-09-28.html"
FIXTURE_INVESTING_QUALIFIED = "investing_fed_rate_monitor_2026-07-16_qualified.html"
FIXTURE_FOMC_CALENDAR = "fomc_calendar_2026-09-28.html"
FIXTURE_PM_OCTOBER = "polymarket_event_606422_october.json"
FIXTURE_PM_DECEMBER = "polymarket_event_770450_december.json"
FIXTURE_PM_COMBO = "polymarket_event_955999_combo.json"
FIXTURE_FRED_UPPER = "fred_DFEDTARU_recent.csv"
FIXTURE_FRED_LOWER = "fred_DFEDTARL_recent.csv"


def fixture_path(name: str) -> Path:
    return FIXTURES / name


def fixture_text(name: str) -> str:
    return fixture_path(name).read_text(encoding="utf-8")


def fixture_json(name: str):
    return json.loads(fixture_text(name))


def utc(year: int, month: int, day: int, hour: int = 0, minute: int = 0, second: int = 0):
    return datetime(year, month, day, hour, minute, second, tzinfo=timezone.utc)


class FixedClock:
    """A deterministic clock for retrieved_at / freshness assertions."""

    def __init__(self, value: datetime):
        self.value = value

    def __call__(self) -> datetime:
        return self.value


class FakeTransport(Transport):
    """Deterministic transport: URL-substring routes returning fixtures.

    Route values may be a payload, a ``TransportError`` to raise, or a callable
    ``(url, params) -> payload`` for token-specific CLOB histories.
    """

    def __init__(self):
        self.text_routes: list = []
        self.json_routes: list = []
        self.text_calls: list = []
        self.json_calls: list = []

    def add_text(self, substring: str, value):
        # Most recently added route wins, so a test can override a default.
        self.text_routes.insert(0, (substring, value))
        return self

    def add_json(self, substring: str, value):
        self.json_routes.insert(0, (substring, value))
        return self

    @staticmethod
    def _resolve(value, url, params):
        if isinstance(value, BaseException):
            raise value
        if callable(value):
            return value(url, params)
        return value

    def get_text(self, url: str, headers=None, timeout: int = 20) -> str:
        self.text_calls.append({"url": url, "headers": dict(headers or {})})
        for substring, value in self.text_routes:
            if substring in url:
                return self._resolve(value, url, None)
        raise TransportError(f"no text fixture route for {url}", url=url)

    def get_json(self, url: str, params=None, timeout: int = 20):
        self.json_calls.append({"url": url, "params": dict(params or {})})
        for substring, value in self.json_routes:
            if substring in url:
                return self._resolve(value, url, params)
        raise TransportError(f"no JSON fixture route for {url}", url=url)


def make_gamma_market(question: str, yes_price, token_id: str, market_id: str = "1",
                      outcome_prices=None, token_ids=None):
    prices = outcome_prices
    if prices is None:
        no_price = round(1.0 - float(yes_price), 4)
        prices = [str(yes_price), str(no_price)]
    tokens = token_ids if token_ids is not None else [token_id, f"{token_id}-no"]
    return {
        "id": market_id,
        "question": question,
        "outcomes": json.dumps(["Yes", "No"]),
        "outcomePrices": json.dumps([str(value) for value in prices]),
        "clobTokenIds": json.dumps(tokens),
        "endDate": "2026-10-29T03:59:00Z",
    }


DEFAULT_OUTCOME_SPECS = [
    (-50, True, "0.0025"),
    (-25, False, "0.0045"),
    (0, False, "0.335"),
    (25, False, "0.655"),
    (50, True, "0.0085"),
]


def _rate_question(month: str, year: int, bp_delta: int, open_ended: bool) -> str:
    if bp_delta == 0:
        return f"Will there be no change in Fed interest rates after the {month} {year} meeting?"
    direction = "increase" if bp_delta > 0 else "decrease"
    plus = "+" if open_ended else ""
    return (
        f"Will the Fed {direction} interest rates by {abs(bp_delta)}{plus} bps "
        f"after the {month} {year} meeting?"
    )


def make_fed_decision_event(
    event_id: str = "606422",
    title: str = "Fed Decision in October?",
    end_date: str = "2026-10-29T03:59:00Z",
    month: str = "October",
    year: int = 2026,
    specs=None,
    active: bool = True,
    closed: bool = False,
) -> dict:
    specs = list(DEFAULT_OUTCOME_SPECS if specs is None else specs)
    markets = []
    for index, (bp_delta, open_ended, price) in enumerate(specs):
        markets.append(
            make_gamma_market(
                _rate_question(month, year, bp_delta, open_ended),
                price,
                token_id=f"{event_id}-tok-{bp_delta}{'o' if open_ended else ''}",
                market_id=f"{event_id}-{index}",
            )
        )
    return {
        "id": event_id,
        "title": title,
        "endDate": end_date,
        "active": active,
        "closed": closed,
        "markets": markets,
    }


def make_clob_history(token_points: dict):
    """CLOB route callable: maps the request's market token to its history."""

    def route(url, params):
        token = (params or {}).get("market")
        points = token_points.get(token, [])
        return {"history": points}

    return route


def epoch(value: datetime) -> int:
    return int(value.timestamp())
