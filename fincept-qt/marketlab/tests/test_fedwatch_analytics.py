"""Deterministic tests for the approved historical FedWatch calculations.

The analytics module computes from the stored accepted-observation chronology
only: previous-observation change, 1/7/30-day changes, change since the first
retained observation, observed high/low, range position, percentile, the
Fed-side versus Polymarket difference (``Polymarket - Fed-side``) and its
daily evolution over actually observed days. Insufficient coverage must be an
explicit state, never a fabricated value. No test uses the network.
"""

from __future__ import annotations

import sqlite3
import tempfile
import unittest
from pathlib import Path

from fedwatch_test_support import utc

from fedwatch import analytics as fedwatch_analytics
from fedwatch.history import (
    FED_METHOD_LIVE,
    FED_METHOD_ZQ,
    POLY_METHOD,
    POLY_SOURCE,
)
from fedwatch.store import FedwatchHistoryStore, content_digest

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
    quality=None,
    instrument_key="",
) -> None:
    """Record production-like observations: the digest covers content only.

    The default quality is the finalized producer's current marker for the
    series: "OK" for live Fed-side observations, "CURRENT" for live Polymarket
    observations and "RECONSTRUCTED" for ZQ watch-date reconstructions.
    """
    if quality is None:
        quality = {
            POLY_METHOD: "CURRENT",
            FED_METHOD_ZQ: "RECONSTRUCTED",
        }.get(method, "OK")
    for observed_at, value in points:
        digest = content_digest(
            {
                "method": method,
                "source": source,
                "outcome_bp": outcome_bp,
                "open_ended": open_ended,
                "instrument_key": instrument_key,
                "probability_pct": value,
            }
        )
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
            digest=digest,
            instrument_key=instrument_key,
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
        self.assertEqual(fed["observation_count"], 6)

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


class PreviousObservationTests(unittest.TestCase):
    def test_previous_observation_is_the_immediately_preceding_accepted_observation(self):
        store = new_store(self)
        seed(store, [("2026-09-26T00:00:00Z", 50.0)])
        seed(store, [("2026-09-27T00:00:00Z", 60.0)])
        seed(store, [("2026-09-28T00:00:00Z", 60.0)])
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(fed["latest"]["probability_pct"], 60.0)
        previous = fed["latest_change_from_previous_observation"]
        self.assertEqual(change(previous), 0.0)
        self.assertEqual(previous["reference_probability_pct"], 60.0)
        self.assertEqual(previous["reference_observed_at"], "2026-09-27T00:00:00Z")
        self.assertEqual(change(fed["change_since_first_observation"]), 10.0)
        self.assertEqual(fed["observation_count"], 3)

    def test_repeated_values_do_not_change_the_lookback_reference(self):
        store = new_store(self)
        seed(store, [("2026-09-20T00:00:00Z", 40.0)])
        seed(store, [("2026-09-21T00:00:00Z", 40.0)])
        seed(store, [("2026-09-27T00:00:00Z", 45.0)])
        seed(store, [("2026-09-28T00:00:00Z", 50.0)])
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(change(fed["changes"]["1d"]), 5.0)
        self.assertEqual(fed["changes"]["1d"]["reference_observed_at"], "2026-09-27T00:00:00Z")
        self.assertEqual(change(fed["changes"]["7d"]), 10.0)
        self.assertEqual(fed["changes"]["7d"]["reference_observed_at"], "2026-09-21T00:00:00Z")


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
        self.assertEqual(result["fed_side"]["observation_count"], 1)

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

    def test_derived_fed_tail_from_current_observations_remains_current(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 20.0)], outcome_bp=0)
        seed(store, [("2026-09-28T00:00:00Z", 80.0)], outcome_bp=25)
        seed(
            store, [("2026-09-28T00:00:00Z", 75.0)],
            method=POLY_METHOD, source=POLY_SOURCE, open_ended=True,
        )
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, open_ended=True, as_of=NOW
        )
        fed = result["fed_side"]
        self.assertEqual(fed["latest"]["probability_pct"], 80.0)
        self.assertEqual(
            fed["latest"]["quality_status"], "DERIVED_FROM_MEETING_DISTRIBUTION"
        )
        self.assertTrue(fed["latest"]["current_eligible"])
        self.assertEqual(result["difference"]["current_state"], "OK")
        self.assertEqual(result["difference"]["current_probability_diff_pp"], -5.0)

    def test_derived_fed_tail_from_reconstructed_observations_is_not_current(self):
        store = new_store(self)
        seed(
            store, [("2026-09-28T00:00:00Z", 40.0)],
            method=FED_METHOD_ZQ, source="zq", outcome_bp=0,
        )
        seed(
            store, [("2026-09-28T00:00:00Z", 60.0)],
            method=FED_METHOD_ZQ, source="zq", outcome_bp=25,
        )
        seed(
            store, [("2026-09-28T00:00:00Z", 65.0)],
            method=POLY_METHOD, source=POLY_SOURCE, open_ended=True,
        )
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, open_ended=True, fed_method=FED_METHOD_ZQ, as_of=NOW
        )
        fed = result["fed_side"]
        self.assertEqual(fed["latest"]["probability_pct"], 60.0)
        self.assertEqual(
            fed["latest"]["quality_status"], "DERIVED_FROM_MEETING_DISTRIBUTION"
        )
        self.assertFalse(fed["latest"]["current_eligible"])
        difference = result["difference"]
        self.assertEqual(difference["current_state"], "NON_CURRENT_LATEST_OBSERVATION")
        self.assertIsNone(difference["current_probability_diff_pp"])

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

    def test_incomplete_distribution_is_never_used_for_a_derived_tail(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 70.0)], outcome_bp=25)
        tail = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, open_ended=True, as_of=NOW
        )["fed_side"]
        self.assertEqual(tail["state"], "NO_OBSERVATIONS")
        absent = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 50, open_ended=True, as_of=NOW
        )["fed_side"]
        self.assertEqual(absent["state"], "NO_OBSERVATIONS")

    def test_exact_stored_outcome_is_readable_with_an_incomplete_distribution(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 70.0)], outcome_bp=25)
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(fed["latest"]["probability_pct"], 70.0)


class DivergenceTests(unittest.TestCase):
    def _seed_both(self, store, fed_points, poly_points):
        seed(store, fed_points)
        seed(store, poly_points, method=POLY_METHOD, source=POLY_SOURCE)

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
            store, "2026-10-28", 25, as_of=utc(2026, 9, 22, 12, 0, 0)
        )
        difference = result["difference"]
        self.assertEqual(difference["sign_convention"], "Polymarket - Fed-side")
        self.assertEqual(difference["current_state"], "OK")
        self.assertEqual(difference["current_probability_diff_pp"], 2.0)
        self.assertEqual(difference["current_fed_probability_pct"], 32.0)
        self.assertEqual(difference["current_polymarket_probability_pct"], 34.0)
        self.assertEqual(difference["fed_latest_quality_status"], "OK")
        self.assertEqual(difference["polymarket_latest_quality_status"], "CURRENT")
        self.assertEqual(
            [(row["date"], row["probability_diff_pp"]) for row in difference["history"]],
            [("2026-09-20", 5.0), ("2026-09-22", 2.0)],
        )

    def test_unchanged_values_do_not_create_divergence_days_without_an_observation(self):
        # Same values on 09-20 and 09-22 with no observation on 09-21: the
        # calendar day in between must not be presented as covered.
        store = new_store(self)
        self._seed_both(
            store,
            [("2026-09-20T00:00:00Z", 70.0), ("2026-09-22T00:00:00Z", 70.0)],
            [("2026-09-20T00:00:00Z", 75.0), ("2026-09-22T00:00:00Z", 75.0)],
        )
        history = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=utc(2026, 9, 23)
        )["difference"]["history"]
        self.assertEqual([row["date"] for row in history], ["2026-09-20", "2026-09-22"])
        self.assertEqual([row["probability_diff_pp"] for row in history], [5.0, 5.0])

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
            store, "2026-10-28", 25, as_of=utc(2026, 9, 22, 12, 0, 0)
        )["difference"]
        self.assertEqual([row["date"] for row in difference["history"]], ["2026-09-22"])
        self.assertEqual(difference["current_probability_diff_pp"], 2.0)

    def test_current_difference_is_gated_on_latest_observation_age(self):
        store = new_store(self)
        self._seed_both(
            store,
            [("2026-09-28T00:00:00Z", 50.0)],
            [("2026-09-20T00:00:00Z", 55.0)],
        )
        difference = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["difference"]
        self.assertIsNone(difference["current_probability_diff_pp"])
        self.assertEqual(difference["current_state"], "STALE_LATEST_OBSERVATION")
        self.assertGreater(difference["polymarket_latest_age_days"], 3.0)
        self.assertLessEqual(difference["fed_latest_age_days"], 3.0)

    def test_partial_polymarket_observation_is_readable_but_not_current(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 40.0)])
        seed(
            store,
            [("2026-09-27T00:00:00Z", 35.0), ("2026-09-28T00:00:00Z", 36.0)],
            method=POLY_METHOD, source=POLY_SOURCE, quality="PARTIAL",
        )
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )
        # The stored series stays historically readable ...
        polymarket = result["polymarket"]
        self.assertEqual(polymarket["state"], "OK")
        self.assertEqual(polymarket["latest"]["probability_pct"], 36.0)
        self.assertEqual(polymarket["latest"]["quality_status"], "PARTIAL")
        self.assertEqual(change(polymarket["latest_change_from_previous_observation"]), 1.0)
        difference = result["difference"]
        self.assertEqual([row["date"] for row in difference["history"]], ["2026-09-28"])
        # ... but a partially covered observation is not current provider data.
        self.assertEqual(difference["current_state"], "NON_CURRENT_LATEST_OBSERVATION")
        self.assertIsNone(difference["current_probability_diff_pp"])
        self.assertEqual(difference["polymarket_latest_quality_status"], "PARTIAL")

    def test_stale_polymarket_observation_is_readable_but_not_current(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 40.0)])
        seed(
            store,
            [("2026-09-27T00:00:00Z", 35.0), ("2026-09-28T00:00:00Z", 36.0)],
            method=POLY_METHOD, source=POLY_SOURCE, quality="STALE",
        )
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )
        self.assertEqual(result["polymarket"]["latest"]["probability_pct"], 36.0)
        difference = result["difference"]
        self.assertEqual(difference["current_state"], "NON_CURRENT_LATEST_OBSERVATION")
        self.assertIsNone(difference["current_probability_diff_pp"])
        self.assertEqual(difference["polymarket_latest_quality_status"], "STALE")

    def test_backfilled_polymarket_history_is_readable_but_not_current(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 40.0)])
        seed(
            store,
            [("2026-09-27T00:00:00Z", 35.0), ("2026-09-28T00:00:00Z", 36.0)],
            method=POLY_METHOD, source=POLY_SOURCE, quality="BACKFILLED",
        )
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )
        self.assertEqual(result["polymarket"]["latest"]["probability_pct"], 36.0)
        difference = result["difference"]
        self.assertEqual(difference["current_state"], "NON_CURRENT_LATEST_OBSERVATION")
        self.assertIsNone(difference["current_probability_diff_pp"])

    def test_reconstructed_fed_observations_are_readable_but_not_current(self):
        store = new_store(self)
        seed(
            store,
            [("2026-09-28T00:00:00Z", 55.0)],
            method=FED_METHOD_ZQ, source="zq", quality="RECONSTRUCTED",
        )
        seed(store, [("2026-09-28T00:00:00Z", 60.0)], method=POLY_METHOD, source=POLY_SOURCE)
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, fed_method=FED_METHOD_ZQ, as_of=NOW
        )
        fed = result["fed_side"]
        self.assertEqual(fed["latest"]["probability_pct"], 55.0)
        self.assertEqual(fed["latest"]["quality_status"], "RECONSTRUCTED")
        self.assertFalse(fed["latest"]["current_eligible"])
        difference = result["difference"]
        self.assertEqual(difference["current_state"], "NON_CURRENT_LATEST_OBSERVATION")
        self.assertIsNone(difference["current_probability_diff_pp"])
        self.assertEqual(difference["fed_latest_quality_status"], "RECONSTRUCTED")

    def test_conflicting_observation_never_fabricates_a_summed_probability(self):
        store = new_store(self)
        seed(store, [("2026-07-14T00:00:00Z", 50.0)])
        seed(store, [("2026-07-16T00:00:00Z", 50.0)])
        seed(store, [("2026-07-15T00:00:00Z", 20.0)])
        points, errors = fedwatch_analytics.change_points(
            store, "2026-10-28", LIVE, 25, False
        )
        self.assertEqual(errors, [])
        self.assertEqual([point["probability_pct"] for point in points], [50.0, 20.0, 50.0])
        fed = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["fed_side"]
        self.assertEqual(fed["latest"]["probability_pct"], 50.0)
        self.assertEqual(change(fed["latest_change_from_previous_observation"]), 30.0)
        self.assertEqual(fed["observed_high"]["probability_pct"], 50.0)
        self.assertNotIn(0.0, [point["probability_pct"] for point in points])


class InstrumentGenerationTests(unittest.TestCase):
    def test_recreated_token_series_uses_the_current_validated_generation(self):
        store = new_store(self)
        seed(
            store,
            [("2026-09-20T00:00:00Z", 30.0), ("2026-09-27T00:00:00Z", 35.0)],
            method=POLY_METHOD, source=POLY_SOURCE, instrument_key="tok-old",
        )
        seed(
            store,
            [("2026-09-26T00:00:00Z", 60.0), ("2026-09-28T00:00:00Z", 65.0)],
            method=POLY_METHOD, source=POLY_SOURCE, instrument_key="tok-new",
        )
        store.upsert_mapping_outcome(
            "2026-10-28", POLY_SOURCE, POLY_METHOD, 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "m-new", "tok-new", "question",
            {"validation_method": "test"},
        )
        poly = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )["polymarket"]
        self.assertEqual(poly["latest"]["probability_pct"], 65.0)
        self.assertEqual(poly["latest"]["instrument_key"], "tok-new")
        self.assertEqual(poly["observation_count"], 2)

    def test_validated_mapping_without_rows_does_not_fall_back_to_an_obsolete_generation(self):
        store = new_store(self)
        seed(
            store,
            [("2026-09-27T00:00:00Z", 35.0), ("2026-09-28T00:00:00Z", 36.0)],
            method=POLY_METHOD, source=POLY_SOURCE, instrument_key="tok-old",
        )
        store.upsert_mapping_outcome(
            "2026-10-28", POLY_SOURCE, POLY_METHOD, 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "m-new", "tok-new", "question",
            {"validation_method": "test"},
        )
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )
        self.assertEqual(result["polymarket"]["state"], "NO_OBSERVATIONS")
        self.assertEqual(result["difference"]["current_state"], "MISSING_SIDE")
        self.assertIsNone(result["difference"]["current_probability_diff_pp"])

    def test_negative_revalidation_fails_the_current_comparison_closed(self):
        store = new_store(self)
        seed(
            store,
            [("2026-09-27T00:00:00Z", 40.0), ("2026-09-28T00:00:00Z", 45.0)],
        )
        seed(
            store,
            [("2026-09-27T00:00:00Z", 35.0), ("2026-09-28T00:00:00Z", 36.0)],
            method=POLY_METHOD, source=POLY_SOURCE, instrument_key="tok-old",
        )
        store.upsert_mapping_outcome(
            "2026-10-28", POLY_SOURCE, POLY_METHOD, 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "m-old", "tok-old", "question",
            {"validation_method": "test"},
        )
        store.mark_mapping_revalidation(
            "2026-10-28", POLY_SOURCE, POLY_METHOD, "NOT_FOUND", now=NOW
        )
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, as_of=NOW
        )
        # The stored history stays inspectable ...
        self.assertEqual(result["polymarket"]["latest"]["probability_pct"], 36.0)
        # ... but the current cross-source comparison fails closed.
        difference = result["difference"]
        self.assertEqual(difference["current_state"], "MAPPING_NOT_CURRENT")
        self.assertEqual(difference["mapping_revalidation_status"], "NOT_FOUND")
        self.assertIsNone(difference["current_probability_diff_pp"])
        self.assertIsNone(difference["current_polymarket_probability_pct"])


class ApplicationHistoryContractTests(unittest.TestCase):
    def test_history_keeps_real_instants_and_applies_the_summary_as_of(self):
        store = new_store(self)
        seed(store, [
            ("2026-09-21T00:00:00Z", 40.0),
            ("2026-09-23T00:00:00Z", 40.0),
            ("2026-09-29T00:00:00Z", 60.0),
        ])
        result = fedwatch_analytics.compute_analytics(store, "2026-10-28", 25, as_of=NOW)
        points = result["fed_side"]["history"]
        self.assertEqual([p["observed_at"] for p in points], [
            "2026-09-21T00:00:00Z", "2026-09-23T00:00:00Z",
        ])
        self.assertEqual([p["probability_pct"] for p in points], [40.0, 40.0])
        self.assertEqual(points[-1]["probability_pct"], result["fed_side"]["latest"]["probability_pct"])
        self.assertEqual(result["polymarket"]["history"], [])
        self.assertTrue(points[-1]["current_eligible"])
        points[-1]["probability_pct"] = 99.0
        again = fedwatch_analytics.compute_analytics(store, "2026-10-28", 25, as_of=NOW)
        self.assertEqual(again["fed_side"]["history"][-1]["probability_pct"], 40.0)

    def test_history_exposes_backend_derived_tails_and_preserves_incomplete_gaps(self):
        store = new_store(self)
        seed(store, [("2026-09-21T00:00:00Z", 20.0)], outcome_bp=0)
        seed(store, [
            ("2026-09-21T00:00:00Z", 80.0),
            ("2026-09-23T00:00:00Z", 60.0),
        ])
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, open_ended=True, as_of=NOW,
        )
        points = result["fed_side"]["history"]
        self.assertEqual(len(points), 1)
        self.assertEqual(points[0]["probability_pct"], 80.0)
        self.assertEqual(points[0]["quality_status"], "DERIVED_FROM_MEETING_DISTRIBUTION")
        self.assertEqual(points[0]["observed_at"], "2026-09-21T00:00:00Z")
        absent = fedwatch_analytics.compute_analytics(store, "2026-10-28", -25, as_of=NOW)
        self.assertEqual([p["probability_pct"] for p in absent["fed_side"]["history"]], [0.0])

    def test_history_exposes_only_the_validated_token_generation(self):
        store = new_store(self)
        seed(store, [("2026-09-27T00:00:00Z", 30.0)],
             method=POLY_METHOD, source=POLY_SOURCE, instrument_key="old")
        seed(store, [("2026-09-28T00:00:00Z", 65.0)],
             method=POLY_METHOD, source=POLY_SOURCE, instrument_key="new")
        store.upsert_mapping_outcome(
            "2026-10-28", POLY_SOURCE, POLY_METHOD, 25, False, "VALIDATED",
            "event", "October decision", "market", "new", "question", {},
        )
        result = fedwatch_analytics.compute_analytics(store, "2026-10-28", 25, as_of=NOW)
        self.assertEqual([p["instrument_key"] for p in result["polymarket"]["history"]], ["new"])
        store.upsert_mapping_outcome(
            "2026-10-28", POLY_SOURCE, POLY_METHOD, 25, False, "VALIDATED",
            "event", "October decision", "market-next", "next", "question", {},
        )
        empty = fedwatch_analytics.compute_analytics(store, "2026-10-28", 25, as_of=NOW)
        self.assertEqual(empty["polymarket"]["history"], [])

    def test_zq_history_retains_its_method_and_non_current_quality(self):
        store = new_store(self)
        seed(store, [("2026-09-28T00:00:00Z", 70.0)], method=FED_METHOD_ZQ)
        result = fedwatch_analytics.compute_analytics(
            store, "2026-10-28", 25, fed_method=FED_METHOD_ZQ, as_of=NOW,
        )
        self.assertEqual(result["fed_side"]["method"], FED_METHOD_ZQ)
        self.assertEqual(result["fed_side"]["history"][0]["quality_status"], "RECONSTRUCTED")
        self.assertFalse(result["fed_side"]["history"][0]["current_eligible"])


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
