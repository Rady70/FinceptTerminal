"""Fixture tests for the dashboard quote rules in yfinance_data.py (no network).

Covers the daily-change references: forex from 00:00 Europe/Helsinki
(DST-aware), futures from the current contract's previous settlement (with the
unverified prior-bar fallback), and everything else from the previous daily
close; plus full precision and one row per requested symbol.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

import pandas as pd

_SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

import yfinance_data as yd  # noqa: E402


def hourly(start_utc: str, closes: list[float]) -> pd.DataFrame:
    idx = pd.date_range(start_utc, periods=len(closes), freq="1h", tz="UTC")
    return pd.DataFrame({"Open": closes, "High": [c + 0.001 for c in closes], "Low": [c - 0.001 for c in closes],
                         "Close": closes, "Volume": [0] * len(closes)}, index=idx)


def daily(dates: list[str], closes: list[float], tz="America/New_York") -> pd.DataFrame:
    idx = pd.DatetimeIndex([pd.Timestamp(d, tz=tz) for d in dates])
    return pd.DataFrame({"Open": closes, "High": closes, "Low": closes, "Close": closes,
                         "Volume": [100] * len(closes)}, index=idx)


class HelsinkiDayTests(unittest.TestCase):
    def test_summer_time_midnight_is_21_utc(self):
        # 2026-10-20 is EEST (UTC+3): Helsinki midnight = 2026-10-19 21:00 UTC.
        # Bars start 19:00..23:00 UTC; the 20:00 bar ends at 21:00 = midnight.
        f = hourly("2026-10-19 19:00", [1.10, 1.20, 1.30, 1.40, 1.50])
        day = yd.helsinki_day_change(f)
        self.assertEqual(day["session_date"], "2026-10-20")
        self.assertAlmostEqual(day["previous_close"], 1.20)
        self.assertAlmostEqual(day["price"], 1.50)
        self.assertAlmostEqual(day["open"], 1.30)

    def test_winter_time_midnight_is_22_utc(self):
        # 2026-11-03 is EET (UTC+2): Helsinki midnight = 2026-11-02 22:00 UTC.
        f = hourly("2026-11-02 20:00", [2.0, 2.1, 2.2, 2.3])
        day = yd.helsinki_day_change(f)
        self.assertEqual(day["session_date"], "2026-11-03")
        self.assertAlmostEqual(day["previous_close"], 2.1)  # the 21:00 bar ends at 22:00

    def test_monday_compares_with_friday_close_and_ignores_weekend_bars(self):
        fri = hourly("2026-11-06 20:00", [3.0, 3.1])  # Fri 22:00/23:00 Helsinki (EET)
        stray = hourly("2026-11-07 12:00", [9.9])    # a stray Saturday bar
        mon = hourly("2026-11-08 22:00", [3.2, 3.3])  # Mon 00:00/01:00 Helsinki
        day = yd.helsinki_day_change(pd.concat([fri, stray, mon]))
        self.assertEqual(day["session_date"], "2026-11-09")
        self.assertAlmostEqual(day["previous_close"], 3.1)
        self.assertAlmostEqual(day["price"], 3.3)

    def test_no_bar_before_midnight_leaves_reference_missing(self):
        f = hourly("2026-10-19 21:00", [1.0, 1.1])
        day = yd.helsinki_day_change(f)
        self.assertIsNone(day["previous_close"])
        row = yd._quote_row("EURUSD=X", day["price"], day["previous_close"], yd.DAY_BASIS_HELSINKI,
                            day["session_date"])
        self.assertIsNone(row["change"])
        self.assertIsNone(row["change_percent"])

    def test_empty_frame(self):
        self.assertIsNone(yd.helsinki_day_change(None))
        self.assertIsNone(yd.helsinki_day_change(hourly("2026-10-19 21:00", [float("nan")])))


class FuturesTests(unittest.TestCase):
    def setUp(self):
        yd._settlement_cache.clear()

    def test_change_uses_current_contract_settlement(self):
        # Prior bar 103.66 belongs to the expired contract; settlement 98.54 is the current one.
        hist = daily(["2026-10-19", "2026-10-20"], [103.66, 100.68])
        reader = lambda s: {"regularMarketPreviousClose": 98.54, "underlyingSymbol": "CLZ26.NYM"}
        row = yd._daily_quote("CL=F", hist, reader)
        self.assertEqual(row["day_basis"], yd.DAY_BASIS_SETTLEMENT)
        self.assertEqual(row["contract"], "CLZ26.NYM")
        self.assertAlmostEqual(row["change"], 2.14)
        self.assertGreater(row["change_percent"], 0)

    def test_unreadable_settlement_falls_back_and_says_so(self):
        hist = daily(["2026-10-19", "2026-10-20"], [57.0, 57.4])
        row = yd._daily_quote("SI=F", hist, lambda s: {})
        self.assertEqual(row["day_basis"], yd.DAY_BASIS_PRIOR_BAR)
        self.assertAlmostEqual(row["previous_close"], 57.0)

    def test_settlement_is_cached(self):
        calls = []

        def reader(s):
            calls.append(s)
            return {"previousClose": 10.0}
        yd._previous_settlement("NG=F", reader)
        yd._previous_settlement("NG=F", reader)
        self.assertEqual(calls, ["NG=F"])


class PrecisionAndShapeTests(unittest.TestCase):
    def test_small_prices_keep_their_digits(self):
        row = yd._quote_row("DOGE-USD", 0.07926, 0.0797, yd.DAY_BASIS_SESSION, "2026-10-20")
        self.assertAlmostEqual(row["price"], 0.07926)
        self.assertAlmostEqual(row["change"], -0.00044)
        self.assertNotEqual(row["change"], 0)

    def test_stock_uses_previous_session_and_reports_its_date(self):
        hist = daily(["2026-10-16", "2026-10-19"], [343.46, 342.94])
        row = yd._daily_quote("AAPL", hist)
        self.assertEqual(row["day_basis"], yd.DAY_BASIS_SESSION)
        self.assertEqual(row["session_date"], "2026-10-19")
        self.assertAlmostEqual(row["previous_close"], 343.46)

    def test_single_bar_has_no_change(self):
        row = yd._daily_quote("NEW", daily(["2026-10-20"], [5.0]))
        self.assertIsNone(row["change"])

    def test_every_requested_symbol_gets_a_row(self):
        hist = daily(["2026-10-19", "2026-10-20"], [1.0, 2.0])
        frame = pd.concat({"AAA": hist}, axis=1)
        with mock.patch.object(yd.yf, "download", return_value=frame):
            rows = yd.get_batch_quotes(["AAA", "MISSING"])
        self.assertEqual([r["symbol"] for r in rows], ["AAA", "MISSING"])
        self.assertIn("error", rows[1])
        self.assertAlmostEqual(rows[0]["change_percent"], 100.0)


if __name__ == "__main__":
    unittest.main()
