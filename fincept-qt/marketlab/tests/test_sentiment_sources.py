"""Fixture tests for the dashboard sentiment collectors (no network).

cnn_fear_greed.py: CNN's feed is parsed by meaning; a value is dropped only
when it is not a 0..100 score, and a missing index is an error.
market_breadth.py: the five breadth readings, their past-year percentile and
the score, on a synthetic market.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np
import pandas as pd

_SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

import cnn_fear_greed as cnn  # noqa: E402
import market_breadth as mb  # noqa: E402


def cnn_payload(**over):
    payload = {
        "fear_and_greed": {"score": 27.2, "rating": "fear", "timestamp": "2026-10-20T22:39:24+00:00",
                           "previous_close": 23.1071, "previous_1_week": 44.11, "previous_1_month": 30.43,
                           "previous_1_year": 29.86},
        "fear_and_greed_historical": {"data": [
            {"x": 1760918400000.0, "y": 29.86, "rating": "fear"},
            {"x": 1760950000000.0, "y": 31.0, "rating": "fear"},  # same UTC day: the later wins
            {"x": 1761004800000.0, "y": 140, "rating": "?"},  # invalid: dropped
        ]},
    }
    for key, *_ in cnn.COMPONENTS:
        payload[key] = {"score": 50, "rating": "neutral", "timestamp": 1792527301000.0,
                        "data": [{"x": 1, "y": 1.5}, {"x": 2, "y": 17.31}]}
    payload.update(over)
    return payload


class CnnFearGreedParse(unittest.TestCase):
    def test_index_previous_and_components(self):
        d = cnn.parse(cnn_payload(), "2026-10-20T23:00:00+00:00")
        self.assertEqual(d["score"], 27.2)
        self.assertEqual(d["rating"], "fear")
        self.assertEqual(d["updated_at"], "2026-10-20T22:39:24+00:00")
        self.assertEqual([p["score"] for p in d["previous"]], [23.1, 44.1, 30.4, 29.9])
        self.assertEqual(len(d["components"]), 7)
        self.assertEqual(d["components"][4]["value"], 17.31)  # the latest data point
        self.assertEqual(d["components"][0]["updated_at"], "2026-10-20T20:15:01+00:00")
        self.assertEqual(d["missing"], [])

    def test_each_indicator_carries_its_series(self):
        p = cnn_payload(market_momentum_sp125={"data": [{"x": 1792527301000.0, "y": 7549.7}]},
                        market_volatility_vix_50={"data": []})
        p["market_momentum_sp500"]["data"] = [{"x": 1792527301000.0, "y": 7712.57}, {"x": None, "y": 1},
                                              {"x": 1792527302000.0, "y": "bad"}]
        d = cnn.parse(p, "x")
        by = {c["key"]: c for c in d["components"]}
        mom = by["market_momentum_sp500"]
        self.assertEqual(mom["series"], [{"date": "2026-10-20", "value": 7712.57}])
        self.assertEqual(mom["companion_label"], "125-day average")
        self.assertEqual(mom["companion"], [{"date": "2026-10-20", "value": 7549.7}])
        vix = by["market_volatility_vix"]
        self.assertIsNone(vix["companion_label"])  # no companion points: no second line
        self.assertEqual(by["put_call_options"]["series_label"], "Put/call ratio, 5-day average")

    def test_history_one_per_day_and_invalid_dropped(self):
        d = cnn.parse(cnn_payload(), "x")
        self.assertEqual(d["history"], [{"date": "2025-10-20", "score": 31.0}])

    def test_missing_or_invalid_component_is_reported_not_zero(self):
        p = cnn_payload(put_call_options={"score": "n/a"}, junk_bond_demand=None)
        d = cnn.parse(p, "x")
        by = {c["key"]: c for c in d["components"]}
        self.assertIsNone(by["put_call_options"]["score"])
        self.assertIsNone(by["junk_bond_demand"]["score"])
        self.assertIn("put_call_options", d["missing"])
        self.assertIn("junk_bond_demand", d["missing"])

    def test_rating_falls_back_to_cnn_bands(self):
        p = cnn_payload()
        p["fear_and_greed"] = {"score": 80}
        d = cnn.parse(p, "x")
        self.assertEqual(d["rating"], "extreme greed")
        self.assertEqual(cnn.rating_for(24.9), "extreme fear")
        self.assertEqual(cnn.rating_for(50), "neutral")

    def test_missing_index_is_an_error(self):
        with self.assertRaises(ValueError):
            cnn.parse({"fear_and_greed": {"score": None}}, "x")
        with self.assertRaises(ValueError):
            cnn.parse([], "x")


def synthetic_bars(sessions=520, symbols=40, last_day_up=True, seed=7):
    rng = np.random.default_rng(seed)
    dates = pd.bdate_range("2024-08-01", periods=sessions)
    rows = []
    for k in range(symbols):
        rets = rng.normal(0, 0.01, sessions)
        if last_day_up:
            rets[-1] = abs(rets[-1]) + 0.01  # every stock up on the last day
        close = 100 * np.exp(np.cumsum(rets))
        for i, d in enumerate(dates):
            rows.append({"date": d, "symbol": f"S{k:02d}", "High": close[i] * 1.005, "Low": close[i] * 0.995,
                         "Close": close[i], "Volume": 1_000_000.0})
    return pd.DataFrame(rows)


class MarketBreadth(unittest.TestCase):
    def test_last_day_counts_and_readings(self):
        bars = synthetic_bars()
        r = mb.daily_readings(bars)
        last = r.iloc[-1]
        self.assertEqual(int(last["up"]), 40)
        self.assertEqual(int(last["down"]), 0)
        self.assertGreater(last["today_move"], 1.0)
        self.assertTrue(0 <= last["above_50d"] <= 100)

    def test_percentile_rank(self):
        h = pd.Series(range(100), dtype=float)
        self.assertEqual(mb.percentile_rank(h, 49.0), 50.0)
        self.assertEqual(mb.percentile_rank(h, 1000.0), 100.0)
        self.assertIsNone(mb.percentile_rank(h.iloc[:10], 5.0))  # too little history
        self.assertIsNone(mb.percentile_rank(h, float("nan")))

    def test_score_is_the_average_percentile_and_rated(self):
        bars = synthetic_bars()
        members = sorted(bars["symbol"].unique())
        d = mb.build(bars, members, ["MISSING"], "2026-10-20T23:00:00+00:00", ["note"])
        pcts = [c["percentile"] for c in d["components"] if c["percentile"] is not None]
        self.assertGreaterEqual(len(pcts), 3)
        self.assertAlmostEqual(d["score"], round(float(np.mean(pcts)), 1), places=1)
        self.assertEqual(d["rating"], mb.rating_for(d["score"]))
        self.assertEqual(d["up"], 40)
        self.assertEqual(d["missing"], ["MISSING"])  # reported, never filled
        self.assertEqual(d["as_of"], bars["date"].max().strftime("%Y-%m-%d"))
        self.assertTrue(d["history"])

    def test_a_stock_without_a_bar_is_not_counted(self):
        bars = synthetic_bars()
        last = bars["date"].max()
        bars = bars[~((bars["symbol"] == "S00") & (bars["date"] == last))]
        r = mb.daily_readings(bars)
        self.assertEqual(int(r.iloc[-1]["observed"]), 39)
        self.assertEqual(int(r.iloc[-1]["up"]), 39)

    def test_too_little_history_is_an_error(self):
        with self.assertRaises(ValueError):
            mb.build(synthetic_bars(sessions=40), ["S00"], [], "x", [])


if __name__ == "__main__":
    unittest.main()
