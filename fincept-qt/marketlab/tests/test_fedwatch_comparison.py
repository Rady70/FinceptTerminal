"""Deterministic tests for the Fed-side vs Polymarket comparison semantics."""

from __future__ import annotations

import unittest

import fedwatch_test_support  # noqa: F401  (puts scripts/ on sys.path)
from fedwatch import comparison


class ComparisonTests(unittest.TestCase):
    def test_exact_bucket_match_computes_polymarket_minus_fed(self):
        fed = [
            {"outcome_bp": 0, "probability_pct": 63.0},
            {"outcome_bp": 25, "probability_pct": 37.0},
        ]
        polymarket = [
            {"outcome_bp": 0, "open_ended": False, "probability_pct": 65.0},
            {"outcome_bp": 25, "open_ended": False, "probability_pct": 30.0},
        ]
        rows = comparison.compare(fed, polymarket)
        self.assertAlmostEqual(rows[0]["probability_diff_pp"], 2.0)
        self.assertAlmostEqual(rows[1]["probability_diff_pp"], -7.0)
        self.assertEqual(rows[0]["fed_probability_pct"], 63.0)
        self.assertEqual(rows[0]["polymarket_probability_pct"], 65.0)

    def test_open_ended_positive_bucket_sums_the_fed_tail(self):
        fed = [
            {"outcome_bp": 25, "probability_pct": 20.0},
            {"outcome_bp": 50, "probability_pct": 5.0},
        ]
        polymarket = [{"outcome_bp": 25, "open_ended": True, "probability_pct": 30.0}]
        rows = comparison.compare(fed, polymarket)
        self.assertEqual(rows[0]["fed_probability_pct"], 25.0)
        self.assertAlmostEqual(rows[0]["probability_diff_pp"], 5.0)

    def test_open_ended_negative_bucket_sums_the_fed_tail(self):
        fed = [
            {"outcome_bp": -50, "probability_pct": 5.0},
            {"outcome_bp": -25, "probability_pct": 20.0},
        ]
        polymarket = [{"outcome_bp": -25, "open_ended": True, "probability_pct": 30.0}]
        rows = comparison.compare(fed, polymarket)
        self.assertEqual(rows[0]["fed_probability_pct"], 25.0)
        self.assertAlmostEqual(rows[0]["probability_diff_pp"], 5.0)

    def test_missing_fed_series_yields_none_not_zero(self):
        polymarket = [{"outcome_bp": 0, "open_ended": False, "probability_pct": 65.0}]
        rows = comparison.compare(None, polymarket)
        self.assertIsNone(rows[0]["fed_probability_pct"])
        self.assertIsNone(rows[0]["probability_diff_pp"])
        self.assertEqual(rows[0]["polymarket_probability_pct"], 65.0)

    def test_missing_exact_bucket_is_zero_when_a_fed_series_exists(self):
        fed = [{"outcome_bp": 0, "probability_pct": 100.0}]
        polymarket = [
            {"outcome_bp": 25, "open_ended": False, "probability_pct": 10.0},
            {"outcome_bp": 50, "open_ended": True, "probability_pct": 2.0},
        ]
        rows = comparison.compare(fed, polymarket)
        self.assertEqual(rows[0]["fed_probability_pct"], 0.0)
        self.assertAlmostEqual(rows[0]["probability_diff_pp"], 10.0)
        self.assertEqual(rows[1]["fed_probability_pct"], 0.0)
        self.assertAlmostEqual(rows[1]["probability_diff_pp"], 2.0)

    def test_missing_polymarket_price_yields_none_difference(self):
        fed = [{"outcome_bp": 0, "probability_pct": 100.0}]
        polymarket = [{"outcome_bp": 0, "open_ended": False, "probability_pct": None}]
        rows = comparison.compare(fed, polymarket)
        self.assertEqual(rows[0]["fed_probability_pct"], 100.0)
        self.assertIsNone(rows[0]["polymarket_probability_pct"])
        self.assertIsNone(rows[0]["probability_diff_pp"])


if __name__ == "__main__":
    unittest.main()
