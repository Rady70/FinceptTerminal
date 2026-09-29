"""Deterministic tests for the approved historical FedWatch calculations.

The analytics module computes from the stored observation chronology only:
previous-observation change, 1/7/30-day changes, change since the first
retained observation, observed high/low, range position, percentile, the
Fed-side versus Polymarket difference (``Polymarket - Fed-side``) and its
daily evolution. Insufficient coverage must be an explicit state, never a
fabricated value. No test uses the network.
"""

from __future__ import annotations

import sqlite3
import tempfile
import unittest
from datetime import timedelta
from pathlib import Path

from fedwatch_test_support import utc

from fedwatch import analytics as fedwatch_analytics
from fedwatch.history import (
    FED_METHOD_LIVE,
    FED_METHOD_ZQ,
    POLY_METHOD,
    POLY_SOURCE,
)
from fedwatch.store import FedwatchHistoryStore

NOW = utc(2026, 9, 28, 12, 0, 0)
LIVE = FED_METHOD_LIVE


def new_store(testcase) -> FedwatchHistoryStore:
    tmp = tempfile.TemporaryDirectory()
    testcase.addCleanup(tmp.cleanup)
    return FedwatchHistoryStore(Path(tmp.name) / "history.db")


def seed(
    store: FedwatchHistoryStore,
    points,
    meeting="2026-10-28",
    method=LIVE,
    source="investing",
    outcome_bp=25,
    open_ended=False,
    quality="OK",
) -> None:
    for observed_at, value in points:
        store.record_observation(
            meeting_date=meeting,
            source=source,
            method=method,
            outcome_bp=outcome_bp,
            open_ended=open_ended,
            probability_pct=value,
            raw_probability_pct=None,
            normalized_probability_pct=None,
            observed_at=observed_at,
            source_observed_at=None,
            retrieved_at=observed_at,
            quality_status=quality,
            freshness_status=None,
            digest=f"{method}|{meeting}|{outcome_bp}|{open_ended}|{value}|{observed_at}",
            detail={"origin": "test"},
        )


def change(entry):
    return entry["change_pp"]


class FullCalculationTests(unittest.TestCase):
    def setUp(self):
        self.store = new_store(self)
        seed(
            self.store,
            [
                ("2026-08-01T00:00:00Z", 30.0),
                ("2026-08-25T00:00:00Z", 32.0),
                ("2026-09-21T00:00:00Z", 33.0),
                ("2026-09-26T00:00:00Z", 35.0),
                ("2026-09-27T00:00:00Z", 45.0),
                ("2026-09-28T00:00:00Z", 40.0),
            ],
        )

    def test_all_required_calculations(self):
        result = fedwatch_analytics.compute_analytics(
            self.store, "2026-10-28", 25, as_of=NOW
        )
        fed = result["fed_side"]
        self.assertEqual(fed["state"], "OK")
        self.assertEqual(fed["method"], LIVE)
        self.assertEqual(fed["latest"]["probability_pct"], 40.0)
        self.assertEqual(fed["latest"]["observed_at"], "2026-09-28T00:00:00Z")

        previous = fed["latest_change_from_previous_observation"]
        self.assertEqual(change(previous), -5.0)
        self.assertEqual(previous["reference_probability_pct"], 45.0)
        self.assertEqual(previous["reference_observed_at"], "2026-09-27T00:00:00Z")

        self.assertEqual(change(fed["changes"]["1d"]), -5.0)
        self.assertEqual(fed["changes"]["1d"]["reference_observed_at"], "2026-09-27T00:00:00Z")
        self.assertEqual(change(fed["changes"]["7d"]), 7.0)
        self.assertEqual(fed["changes"]["7d"]["reference_observed_at"], "2026-09-21T00:00:00Z")
        self.assertEqual(change(fed["changes"]["30d"]), 8.0)
        self.assertEqual(fed["changes"]["30d"]["reference_observed_at"], "2026-08-25T00:00:00Z")
        self.assertEqual(change(fed["change_since_first_observation"]), 10.0)

        self.assertEqual(fed["observed_high"], {"probability_pct": 45.0, "observed_at": "2026-09-27T00:00:00Z"})
        self.assertEqual(fed["observed_low"], {"probability_pct": 30.0, "observed_at": "2026-08-01T00:00:00Z"})
        self.assertEqual(fed["range_width_pp"], 15.0)
        self.assertAlmostEqual(fed["range_position"], round(10.0 / 15.0, 6), places=9)
        self.assertIsNone(fed["range_position_state"])
        self.assertAlmostEqual(fed["percentile_rank"], round(5.0 / 6.0, 6), places=9)
        self.assertEqual(fed["episode_count"], 6)

    def test_analytics_does_not_persist_derived_values(self):
        before = self.store.count_observations()
        fedwatch_analytics.compute_analytics(self.store, "2026-10-28", 25, as_of=NOW)
        self.assertEqual(self.store.count_observations(), before)

    def test_as_of_excludes_later_observations(self):
        result = fedwatch_analytics.compute_analytics(
            self.store, "2026-10-28", 25, as_of=utc(2026, 9, 27, 12, 0, 0)
        )
        self.assertEqual(result["fed_side"]["latest"]["probability_pct"], 45.0)

    def test_unknown_fed_method_is_rejected(self):
        with self.assertRaises(ValueError):
            fedwatch_analytics.compute_analytics(
                self.store, "2026-10-28", 25, fed_method="NOT_A_METHOD", as_of=NOW
            )


class InsufficientHistoryTests(unittest.TestCase):
    def setUp(self):
        self.store = new_store(self)
        seed(
            self.store,
            [
                ("2026-09-26T00:00:00Z", 30.0),
                ("2026-09-27T00:00:00Z", 31.0),
                ("2026-09-28T00:00:00Z", 32.0),
            ],
        )

    def test_short_coverage_never_fabricates_long_lookbacks(self):
        fed = fedwatch_analytics.compute_analytics(
            self.store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(change(fed["changes"]["1d"]), 1.0)
        for window in ("7d", "30d"):
            entry = fed["changes"][window]
            self.assertIsNone(entry["change_pp"])
            self.assertEqual(entry["state"], "INSUFFICIENT_HISTORY")
            self.assertEqual(entry["required_lookback_days"], int(window[:-1]))
            self.assertEqual(entry["earliest_observed_at"], "2026-09-26T00:00:00Z")

    def test_no_observations_state(self):
        empty = new_store(self)
        result = fedwatch_analytics.compute_analytics(
            empty, "2026-10-28", 25, as_of=NOW
        )
        self.assertEqual(result["fed_side"]["state"], "NO_OBSERVATIONS")
        self.assertIsNone(result["difference"]["current_probability_diff_pp"])
        self.assertEqual(result["difference"]["history"], [])

    def test_single_observation_has_no_range_position(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 50.0)])
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(fed["range_width_pp"], 0.0)
        self.assertIsNone(fed["range_position"])
        self.assertEqual(fed["range_position_state"], "ZERO_WIDTH_RANGE")
        self.assertEqual(change(fed["change_since_first_observation"]), 0.0)
        self.assertIsNone(change(fed["latest_change_from_previous_observation"]))
        self.assertEqual(
            fed["latest_change_from_previous_observation"]["state"], "NO_PREVIOUS_OBSERVATION"
        )

    def test_observations_after_as_of_alone_are_no_observations(self):
        store = new_store(self)
        seed(store, [("2026-09-29T00:00:00Z", 50.0)])
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(fed["state"], "NO_OBSERVATIONS")


class IsolationTests(unittest.TestCase):
    def test_meeting_outcome_and_method_series_do_not_leak(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 40.0)])
        seed(store, [("2026-09-28T00:00:00Z", 60.0)], outcome_bp=0)
        seed(store, [("2026-09-28T00:00:00Z", 99.0)], meeting="2026-12-09")
        seed(store, [("2026-09-28T00:00:00Z", 11.0)], method=FED_METHOD_ZQ, source="zq")
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )
        self.assertEqual(result["fed_side"]["latest"]["probability_pct"], 40.0)
        self.assertEqual(result["fed_side"]["episode_count"], 1)

    def test_zq_method_is_selected_explicitly(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 40.0)])
        seed(store, [("2026-09-20T00:00:00Z", 55.0)], method=FED_METHOD_ZQ, source="zq")
        zq = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, fed_method=FED_METHOD_ZQ, as_of=NOW
        )
        self.assertEqual(zq["fed_side"]["method"], FED_METHOD_ZQ)
        self.assertEqual(zq["fed_side"]["latest"]["probability_pct"], 55.0)


class DerivedOutcomeTests(unittest.TestCase):
    def test_open_ended_tails_use_the_comparison_semantics(self):
        store = new_store(self)
        seed(store, [("2026-09-27T00:00:00Z", 30.0)], outcome_bp=0)
        seed(store, [("2026-09-27T00:00:00Z", 70.0)], outcome_bp=25)
        seed(store, [("2026-09-28T00:00:00Z", 20.0)], outcome_bp=0)
        seed(store, [("2026-09-28T00:00:00Z", 80.0)], outcome_bp=25)
        tail = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, open_ended=True, as_of=NOW
        )["fed_side"]
        self.assertEqual(tail["latest"]["probability_pct"], 80.0)
        self.assertEqual(change(tail["latest_change_from_previous_observation"]), 10.0)
        self.assertEqual(
            tail["latest"]["quality_status"], "DERIVED_FROM_MEETING_DISTRIBUTION"
        )

    def test_absent_bucket_is_zero_only_while_the_distribution_is_complete(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 30.0)], outcome_bp=0)
        seed(store, [("2026-09-28T00:00:00Z", 70.0)], outcome_bp=25)
        absent = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 50, open_ended=True, as_of=NOW
        )["fed_side"]
        self.assertEqual(absent["latest"]["probability_pct"], 0.0)
        self.assertEqual(
            absent["latest"]["quality_status"], "DERIVED_FROM_MEETING_DISTRIBUTION"
        )
        exact_absent = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", -25, as_of=NOW
        )["fed_side"]
        self.assertEqual(exact_absent["latest"]["probability_pct"], 0.0)

        incomplete = new_store(self)
        seed(incomplete, [("2026-09-28T00:00:00Z", 70.0)], outcome_bp=25)
        gated = fedwatch_analytics.compute_analytics(
            incomplete, "2026-10-28", 0, as_of=NOW
        )["fed_side"]
        self.assertEqual(gated["state"], "NO_OBSERVATIONS")

    def test_stored_exact_outcome_keeps_its_quality(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 70.0)], outcome_bp=25, quality="STALE")
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(fed["latest"]["quality_status"], "STALE")


class DivergenceTests(unittest.TestCase):
    def _seed_both(self, store, fed_points, poly_points):
        seed(store, fed_points)
        seed(store, poly_points, method=POLY_METHOD, source=POLY_SOURCE)

    def _seed_with_digest(self, store, observed_at, value, digest, method=LIVE,
                          source="investing", outcome_bp=25, open_ended=False):
        store.record_observation(
            meeting_date="2026-10-28",
            source=source,
            method=method,
            outcome_bp=outcome_bp,
            open_ended=open_ended,
            probability_pct=value,
            raw_probability_pct=None,
            normalized_probability_pct=None,
            observed_at=observed_at,
            source_observed_at=None,
            retrieved_at=observed_at,
            quality_status="OK",
            freshness_status=None,
            digest=digest,
            detail={"origin": "test"},
        )

    def test_conflicting_episode_does_not_fabricate_a_summed_probability(self):
        store = new_store(self)
        self._seed_with_digest(store, "2026-07-14T00:00:00Z", 80.0, "zq-80")
        self._seed_with_digest(store, "2026-07-15T00:00:00Z", 80.0, "zq-80")
        self._seed_with_digest(store, "2026-07-15T00:00:00Z", 20.0, "zq-20")
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(fed["latest"]["probability_pct"], 20.0)
        self.assertEqual(change(fed["latest_change_from_previous_observation"]), -60.0)
        self.assertEqual(fed["observed_high"]["probability_pct"], 80.0)

    def test_conflict_strictly_inside_coverage_never_fabricates_a_zero(self):
        store = new_store(self)
        for observed_at in ("2026-07-14T00:00:00Z", "2026-07-15T00:00:00Z"):
            self._seed_with_digest(store, observed_at, 50.0, "bucket-0", outcome_bp=0)
            self._seed_with_digest(store, observed_at, 50.0, "bucket-25", outcome_bp=25)
        self._seed_with_digest(store, "2026-07-14T12:00:00Z", 20.0, "bucket-25-new")
        points, errors = fedwatch_analytics.change_points(
            store, "2026-10-28", LIVE, 25, False
        )
        self.assertEqual(errors, [])
        self.assertEqual([point["probability_pct"] for point in points], [50.0, 20.0])
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(fed["latest"]["probability_pct"], 20.0)
        self.assertNotIn(0.0, [point["probability_pct"] for point in points])

    def test_divergence_does_not_forward_fill_across_an_unobserved_gap(self):
        store = new_store(self)
        for observed_at in ("2026-01-01T00:00:00Z", "2026-03-01T00:00:00Z"):
            self._seed_with_digest(store, observed_at, 70.0, "fed-70")
            self._seed_with_digest(
                store, observed_at, 75.0, "poly-75", method=POLY_METHOD, source=POLY_SOURCE
            )
        history = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=utc(2026, 3, 2, 0, 0, 0)
        )["difference"]["history"]
        self.assertEqual(
            [row["date"] for row in history], ["2026-01-01", "2026-03-01"]
        )
        self.assertEqual([row["probability_diff_pp"] for row in history], [5.0, 5.0])

    def test_current_difference_and_daily_history(self):
        store = new_store(self)
        self._seed_both(
            store,
            [
                ("2026-09-20T00:00:00Z", 30.0),
                ("2026-09-21T00:00:00Z", 31.0),
                ("2026-09-22T00:00:00Z", 32.0),
            ],
            [
                ("2026-09-20T00:00:00Z", 35.0),
                ("2026-09-22T00:00:00Z", 34.0),
            ],
        )
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )
        difference = result["difference"]
        self.assertEqual(difference["sign_convention"], "Polymarket - Fed-side")
        self.assertEqual(difference["current_probability_diff_pp"], 2.0)
        self.assertEqual(difference["current_fed_probability_pct"], 32.0)
        self.assertEqual(difference["current_polymarket_probability_pct"], 34.0)
        self.assertEqual(
            [(row["date"], row["probability_diff_pp"]) for row in difference["history"]],
            [("2026-09-20", 5.0), ("2026-09-22", 2.0)],
        )

    def test_utc_day_boundaries_use_the_correct_observation(self):
        store = new_store(self)
        self._seed_both(
            store,
            [
                ("2026-09-21T23:59:00Z", 30.0),
                ("2026-09-22T00:01:00Z", 40.0),
            ],
            [
                ("2026-09-21T23:59:00Z", 35.0),
                ("2026-09-22T00:01:00Z", 45.0),
            ],
        )
        history = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["difference"]["history"]
        self.assertEqual(
            [(row["date"], row["fed_probability_pct"], row["polymarket_probability_pct"]) for row in history],
            [("2026-09-21", 30.0, 35.0), ("2026-09-22", 40.0, 45.0)],
        )

    def test_missing_polymarket_coverage_creates_a_gap_not_a_fill(self):
        store = new_store(self)
        self._seed_both(
            store,
            [
                ("2026-09-20T00:00:00Z", 30.0),
                ("2026-09-21T00:00:00Z", 31.0),
                ("2026-09-22T00:00:00Z", 32.0),
            ],
            [
                ("2026-09-22T00:00:00Z", 34.0),
            ],
        )
        difference = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["difference"]
        self.assertEqual([row["date"] for row in difference["history"]], ["2026-09-22"])
        self.assertEqual(difference["current_probability_diff_pp"], 2.0)


class MalformedStoredRowTests(unittest.TestCase):
    def test_malformed_rows_are_excluded_with_an_error(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        db = Path(tmp.name) / "history.db"
        store = FedwatchHistoryStore(db)
        seed(store, [("2026-09-28T00:00:00Z", 40.0)])
        connection = sqlite3.connect(str(db))
        try:
            connection.execute(
                "UPDATE fedwatch_probability_observations SET probability_pct = 'NaN'"
            )
            connection.commit()
        finally:
            connection.close()
        result = fedwatch_analytics.compute_analytics(store, "2026-10-28", 25, as_of=NOW)
        self.assertEqual(result["fed_side"]["state"], "NO_OBSERVATIONS")
        self.assertEqual(len(result["errors"]), 1)
        self.assertEqual(result["errors"][0]["provider"], "fedwatch_history")
        self.assertEqual(result["errors"][0]["code"], "FEDWATCH_HISTORY_ROW_INVALID")


if __name__ == "__main__":
    unittest.main()
