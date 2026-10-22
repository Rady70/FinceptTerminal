"""Fixture tests for the dashboard sentiment collectors (no network).

cnn_fear_greed.py: CNN's feed is parsed by meaning; a value is dropped only
when it is not a 0..100 score, and a missing index is an error.
market_breadth.py: risk appetite (riskiest vs safest fifth, rank
correlation, cyclical vs defensive) and the day's counts, on a synthetic
market where the risky stocks are known to lead or lag.
finra_short_interest.py: FINRA rows, share classes of one company, the
share count in force on each date, and the ranking.
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
import finra_short_interest as si  # noqa: E402
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


def synthetic_bars(sessions=520, symbols=40, last_day_up=True, risky_lead=0.0, seed=7):
    """Half the stocks volatile (2%/day), half calm (0.5%/day). Over the last
    40 sessions the volatile half drifts by `risky_lead` per session and the
    calm half by minus that."""
    rng = np.random.default_rng(seed)
    dates = pd.bdate_range("2024-08-01", periods=sessions)
    rows = []
    for k in range(symbols):
        risky = k % 2 == 0
        rets = rng.normal(0, 0.02 if risky else 0.005, sessions)
        rets[-40:] += risky_lead if risky else -risky_lead
        if last_day_up:
            rets[-1] = abs(rets[-1]) + 0.01  # every stock up on the last day
        close = 100 * np.exp(np.cumsum(rets))
        for i, d in enumerate(dates):
            rows.append({"date": d, "symbol": f"S{k:02d}", "Close": close[i], "Adj Close": close[i],
                         "Volume": 1_000_000.0})
    return pd.DataFrame(rows)


def synthetic_sectors(symbols=40):
    # Volatile stocks are cyclical, calm ones defensive.
    return {f"S{k:02d}": ("Information Technology" if k % 2 == 0 else "Utilities") for k in range(symbols)}


class RiskAppetite(unittest.TestCase):
    def setUp(self):
        self._min = mb.MIN_STOCKS
        mb.MIN_STOCKS = 20  # the synthetic market has 40 stocks

    def tearDown(self):
        mb.MIN_STOCKS = self._min

    def members(self, bars):
        return sorted(bars["symbol"].unique())

    def test_last_day_counts(self):
        c = mb.daily_counts(synthetic_bars()).iloc[-1]
        self.assertEqual(int(c["up"]), 40)
        self.assertEqual(int(c["down"]), 0)
        self.assertGreater(c["today_move"], 1.0)

    def test_risky_leading_is_risk_seeking(self):
        bars = synthetic_bars(risky_lead=0.004)
        r = mb.risk_readings(bars, synthetic_sectors()).iloc[-1]
        self.assertGreater(r["risky_minus_safe"], 5)
        self.assertGreater(r["rank_correlation"], 0.5)
        self.assertGreater(r["cyclical_minus_defensive"], 5)
        d = mb.build(bars, self.members(bars), synthetic_sectors(), "2026-10-20T23:00:00+00:00", [])
        self.assertGreater(d["score"], 75)
        self.assertEqual(d["rating"], "strongly seeking risk")

    def test_safe_leading_is_risk_avoiding(self):
        bars = synthetic_bars(risky_lead=-0.004)
        d = mb.build(bars, self.members(bars), synthetic_sectors(), "x", [])
        self.assertLess(d["score"], 25)
        self.assertLess(d["risky_return"], d["safe_return"])
        corr = next(c for c in d["components"] if c["key"] == "rank_correlation")
        self.assertLess(corr["value"], -0.5)

    def test_score_is_the_risky_minus_safe_rank(self):
        bars = synthetic_bars()
        d = mb.build(bars, self.members(bars), synthetic_sectors(), "x", [])
        rank = next(c["percentile"] for c in d["components"] if c["key"] == "risky_minus_safe")
        self.assertEqual(d["score"], rank)
        self.assertEqual(d["rating"], mb.rating_for(d["score"]))
        self.assertEqual(d["history"][-1]["score"], d["score"])

    def test_volatility_is_measured_before_the_return_window(self):
        # A calm stock that jumps inside the 20-session window is not made
        # "risky" by its own jump.
        bars = synthetic_bars()
        last = bars["date"].max()
        jump = (bars["symbol"] == "S01") & (bars["date"] >= last - pd.Timedelta(days=14))
        bars.loc[jump, ["Close", "Adj Close"]] *= 1.5
        adj = bars.pivot(index="date", columns="symbol", values="Adj Close")
        vol = adj.pct_change(fill_method=None).rolling(mb.VOL_SESSIONS, min_periods=50).std().shift(
            mb.RETURN_SESSIONS)
        self.assertLess(vol["S01"].iloc[-1], vol["S00"].iloc[-1])
        self.assertTrue(np.isfinite(mb.risk_readings(bars, synthetic_sectors())["risky_minus_safe"].iloc[-1]))

    def test_partial_last_session_is_skipped_and_said(self):
        bars = synthetic_bars()
        stray = bars[bars["date"] == bars["date"].max()].head(1).copy()
        stray["date"] = bars["date"].max() + pd.Timedelta(days=1)
        d = mb.build(pd.concat([bars, stray]), self.members(bars), synthetic_sectors(), "x", [])
        self.assertEqual(d["as_of"], bars["date"].max().strftime("%Y-%m-%d"))
        self.assertTrue(any("skipped" in n for n in d["notes"]))
        self.assertEqual(d["missing"], [])

    def test_a_stock_without_a_bar_is_named_not_filled(self):
        bars = synthetic_bars()
        members = self.members(bars)
        last = bars["date"].max()
        bars = bars[~((bars["symbol"] == "S00") & (bars["date"] == last))]
        d = mb.build(bars, members, synthetic_sectors(), "x", [])
        self.assertEqual(d["missing"], ["S00"])
        self.assertEqual(d["observed"], 39)

    def test_without_sectors_the_sector_reading_is_unavailable(self):
        bars = synthetic_bars()
        d = mb.build(bars, self.members(bars), {}, "x", [])
        sector = next(c for c in d["components"] if c["key"] == "cyclical_minus_defensive")
        self.assertIsNone(sector["value"])
        self.assertIsNotNone(d["score"])

    def test_percentile_rank(self):
        h = pd.Series(range(100), dtype=float)
        self.assertEqual(mb.percentile_rank(h, 49.0), 50.0)
        self.assertEqual(mb.percentile_rank(h, 1000.0), 100.0)
        self.assertIsNone(mb.percentile_rank(h.iloc[:10], 5.0))  # too little history
        self.assertIsNone(mb.percentile_rank(h, float("nan")))

    def test_too_little_history_is_an_error(self):
        with self.assertRaises(ValueError):
            mb.build(synthetic_bars(sessions=60), ["S00"], {}, "x", [])


def finra_row(symbol, day, qty, adv=1_000_000, revised=None):
    return {"symbolCode": symbol, "settlementDate": day, "currentShortPositionQuantity": qty,
            "averageDailyVolumeQuantity": adv, "revisionFlag": revised}


def frame(**cols):
    out = pd.DataFrame(cols)
    for c in ("settlement_date", "date"):
        if c in out:
            out[c] = pd.to_datetime(out[c])
    return out


class ShortInterest(unittest.TestCase):
    def test_finra_symbols(self):
        self.assertEqual(si.finra_symbol("BRK-B"), "BRKB")
        self.assertEqual(si.finra_symbol("AAPL"), "AAPL")

    def test_rows_map_back_and_revisions_replace(self):
        to_yahoo = {"BRKB": "BRK-B", "AAPL": "AAPL"}
        df = si.parse_counts([finra_row("BRKB", "2026-09-30", 100), finra_row("AAPL", "2026-09-30", 50),
                              finra_row("AAPL", "2026-09-30", 60, revised="R"), finra_row("ZZZZ", "2026-09-30", 1),
                              finra_row("AAPL", "2026-09-15", None)], to_yahoo)
        self.assertEqual(sorted(df["symbol"]), ["AAPL", "BRK-B"])
        self.assertEqual(float(df.loc[df["symbol"] == "AAPL", "short_qty"].iloc[0]), 60.0)

    def test_classes_of_one_company_count_once(self):
        counts = frame(symbol=["GOOGL", "GOOG", "AAPL"], settlement_date=["2026-09-30"] * 3,
                       short_qty=[30.0, 20.0, 10.0], adv=[5.0, 5.0, 10.0])
        shares = frame(symbol=["GOOGL", "GOOG", "AAPL"], date=["2026-07-01"] * 3, shares=[1000.0, 1000.0, 100.0])
        ciks = {"GOOGL": "1652044", "GOOG": "1652044", "AAPL": "320193"}
        per = si.company_readings(counts, shares, ciks).set_index("company")
        self.assertEqual(len(per), 2)
        self.assertAlmostEqual(per.loc["1652044", "pct"], 5.0)  # (30 + 20) / 1000
        self.assertAlmostEqual(per.loc["320193", "pct"], 10.0)

    def test_a_company_missing_a_class_or_shares_is_skipped(self):
        counts = frame(symbol=["GOOGL", "GOOG", "AAPL", "MSFT"],
                       settlement_date=["2026-09-30", "2026-09-15", "2026-09-30", "2026-09-30"],
                       short_qty=[30.0, 20.0, 10.0, 5.0], adv=[1.0] * 4)
        shares = frame(symbol=["GOOGL", "AAPL", "MSFT"], date=["2026-07-01", "2026-07-01", "2026-10-05"],
                       shares=[1000.0, 100.0, 100.0])
        ciks = {"GOOGL": "1652044", "GOOG": "1652044"}
        per = si.company_readings(counts, shares, ciks)
        on_30 = per[per["settlement_date"] == pd.Timestamp("2026-09-30")]
        # GOOG has no 30 Sep count; MSFT's only share count is dated after 30 Sep.
        self.assertEqual(list(on_30["company"]), ["AAPL"])

    def test_share_count_in_force_on_the_date(self):
        counts = frame(symbol=["AAPL", "AAPL"], settlement_date=["2026-03-31", "2026-09-30"],
                       short_qty=[10.0, 10.0], adv=[1.0, 1.0])
        shares = frame(symbol=["AAPL", "AAPL"], date=["2026-01-01", "2026-06-01"], shares=[100.0, 200.0])
        per = si.company_readings(counts, shares, {}).set_index("settlement_date")
        self.assertAlmostEqual(per.loc[pd.Timestamp("2026-03-31"), "pct"], 10.0)
        self.assertAlmostEqual(per.loc[pd.Timestamp("2026-09-30"), "pct"], 5.0)

    def test_above_trend_uses_only_earlier_counts(self):
        steady = pd.Series(np.arange(60, dtype=float) * 0.1 + 2.0)
        gaps = si.above_trend(steady)
        self.assertTrue(gaps.iloc[:si.TREND_COUNTS].isna().all())
        self.assertTrue(np.allclose(gaps.iloc[si.TREND_COUNTS:], 0.0))
        jumped = steady.copy()
        jumped.iloc[-1] += 1.0
        later = si.above_trend(jumped)
        self.assertAlmostEqual(later.iloc[-1], 1.0)
        self.assertTrue(np.allclose(later.iloc[si.TREND_COUNTS:-1], 0.0))  # earlier gaps unchanged

    def test_build_ranks_the_gap_to_trend(self):
        # A steady rise ranks as ordinary; only the last count's jump above
        # the trend ranks high.
        dates = pd.date_range("2023-01-15", periods=80, freq="SMS")
        symbols = [f"S{k:03d}" for k in range(si.MIN_COMPANIES)]
        qty = [10.0 + i for i in range(80)]
        qty[-1] += 20.0
        counts = pd.DataFrame([{"symbol": s, "settlement_date": d, "short_qty": qty[i], "adv": 10.0}
                               for i, d in enumerate(dates) for s in symbols])
        shares = pd.DataFrame({"symbol": symbols, "date": pd.Timestamp("2022-01-01"), "shares": 1000.0})
        d = si.build(counts, shares, symbols + ["NEW"], {}, "x", [])
        self.assertEqual(d["as_of"], dates[-1].strftime("%Y-%m-%d"))
        self.assertAlmostEqual(d["average_pct"], 10.9)  # (89 + 20) / 1000
        self.assertAlmostEqual(d["previous_pct"], 8.8)
        self.assertAlmostEqual(d["trend_pct"], 8.9)
        self.assertAlmostEqual(d["above_trend_pp"], 2.0)
        self.assertEqual(d["percentile"], 100.0)
        self.assertEqual(d["counts_ranked"], 80 - 1 - si.TREND_COUNTS)
        self.assertEqual(d["no_count"], ["NEW"])
        self.assertEqual(d["no_shares"], ["NEW"])
        self.assertEqual(d["members"], si.MIN_COMPANIES + 1)

    def test_steady_rise_alone_is_not_ranked_high(self):
        dates = pd.date_range("2023-01-15", periods=80, freq="SMS")
        symbols = [f"S{k:03d}" for k in range(si.MIN_COMPANIES)]
        rng = np.random.default_rng(3)
        qty = 10.0 + np.arange(80) + rng.normal(0, 0.5, 80)
        counts = pd.DataFrame([{"symbol": s, "settlement_date": d, "short_qty": qty[i], "adv": 10.0}
                               for i, d in enumerate(dates) for s in symbols])
        shares = pd.DataFrame({"symbol": symbols, "date": pd.Timestamp("2022-01-01"), "shares": 1000.0})
        d = si.build(counts, shares, symbols, {}, "x", [])
        self.assertEqual(d["level_percentile"], 100.0)  # the plain level is always a record
        self.assertLess(d["percentile"], 100.0)

    def test_too_few_companies_is_an_error(self):
        counts = frame(symbol=["A"], settlement_date=["2026-09-30"], short_qty=[1.0], adv=[1.0])
        shares = frame(symbol=["A"], date=["2026-01-01"], shares=[10.0])
        with self.assertRaises(ValueError):
            si.build(counts, shares, ["A"], {}, "x", [])

    def test_key_file(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "finra_api.json"
            self.assertIsNone(si.read_key(path))  # no file: needs_key, nothing is read
            path.write_text('{"client_id": "abc", "client_secret": ""}', encoding="utf-8")
            self.assertIsNone(si.read_key(path))
            path.write_text('{"client_id": "abc", "client_secret": "xyz"}', encoding="utf-8")
            self.assertEqual(si.read_key(path), ("abc", "xyz"))


if __name__ == "__main__":
    unittest.main()
