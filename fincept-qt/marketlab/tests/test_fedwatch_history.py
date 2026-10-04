"""Deterministic tests for FedWatch durable collection, lifecycle and backfill.

These tests exercise the real Batch B paths — ``history.record_snapshot``,
``history.collect``, ``history.evaluate_lifecycle``,
``history.backfill_polymarket`` and the ``fedwatch_data.py`` CLI — with the
captured Batch A provider fixtures over fake transports and temporary SQLite
stores. No test opens a network connection.
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import subprocess
import sys
import tempfile
import unittest
from datetime import timedelta
from pathlib import Path

from fedwatch_test_support import (
    FIXTURE_PM_DECEMBER,
    FIXTURE_PM_OCTOBER,
    FakeTransport,
    FixedClock,
    epoch,
    fixture_json,
    make_clob_history,
    make_investing_html,
    make_snapshot_transport,
    fred_series_rows,
    utc,
)

from fedwatch import analytics as fedwatch_analytics
from fedwatch import history as fedwatch_history
from fedwatch import snapshot as fedwatch_snapshot
from fedwatch.store import FedwatchHistoryStore
from fedwatch.transport import TransportError

NOW = utc(2026, 9, 28, 12, 0, 0)
NO_SLEEP = lambda _seconds: None  # noqa: E731
SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "fedwatch_data.py"


def fred_csv(series_id: str, pairs) -> str:
    lines = ["observation_date," + series_id]
    lines.extend(f"{day.isoformat()},{value}" for day, value in pairs)
    return "\n".join(lines) + "\n"


def synthetic_fed_meeting(meeting_date: str) -> dict:
    """A minimal accepted fed-side meeting section for lifecycle tests."""
    buckets = [
        {"rate_low": 3.75, "rate_high": 4.00, "probability_pct": 50.0},
        {"rate_low": 4.00, "rate_high": 4.25, "probability_pct": 50.0},
    ]
    return {
        "meeting_date": meeting_date,
        "status": "UPCOMING",
        "fomc_calendar": None,
        "fed_side": {
            "meeting_date": meeting_date,
            "method": "LIVE_INVESTING_DERIVED",
            "source": "test",
            "raw_probabilities": [dict(bucket) for bucket in buckets],
            "normalized_probabilities": [dict(bucket) for bucket in buckets],
            "normalization": {
                "applied": False,
                "raw_probability_sum_pct": 100.0,
                "normalization_factor": 1.0,
                "normalized_probability_sum_pct": 100.0,
                "raw_expected_rate": 4.0,
                "normalized_expected_rate": 4.0,
                "expected_rate_difference": 0.0,
            },
            "local_probabilities": [
                {"outcome_bp": 0, "probability_pct": 50.0},
                {"outcome_bp": 25, "probability_pct": 50.0},
            ],
            "local_status": "OK",
            "source_timestamp": None,
            "freshness": {"status": "SOURCE_TIMESTAMP_UNAVAILABLE"},
        },
        "polymarket": None,
        "comparison": [],
    }


def build_snapshot_at(now, transport=None, events=None) -> dict:
    transport = transport or make_snapshot_transport(now, events)
    return fedwatch_snapshot.build_snapshot(
        transport, clock=FixedClock(now), sleep=NO_SLEEP
    )


def new_store(testcase) -> FedwatchHistoryStore:
    tmp = tempfile.TemporaryDirectory()
    testcase.addCleanup(tmp.cleanup)
    return FedwatchHistoryStore(Path(tmp.name) / "fedwatch" / "fedwatch_history.db")


class RecordingTests(unittest.TestCase):
    def setUp(self):
        self.store = new_store(self)
        self.snapshot = build_snapshot_at(NOW)

    def test_accepted_fed_side_and_polymarket_observations_are_persisted(self):
        report = fedwatch_history.record_snapshot(
            self.store, self.snapshot["data"], clock=FixedClock(NOW)
        )
        self.assertGreater(report["fed_side_observations"], 0)
        self.assertGreater(report["polymarket_observations"], 0)
        rows = self.store.observations(meeting_date="2026-10-28")
        methods = {row["method"] for row in rows}
        self.assertEqual(methods, {"LIVE_INVESTING_DERIVED", "POLYMARKET_CLOB"})
        fed = [
            row for row in rows if row["method"] == "LIVE_INVESTING_DERIVED"
        ]
        self.assertEqual(
            [(row["outcome_bp"], row["probability_pct"]) for row in fed],
            [(0, 30.0), (25, 70.0)],
        )
        self.assertEqual(fed[0]["source"], "investing")
        self.assertIsNone(fed[0]["source_observed_at"])
        self.assertIsNone(fed[0]["normalized_probability_pct"])
        self.assertEqual(fed[0]["freshness_status"], "SOURCE_TIMESTAMP_UNAVAILABLE")
        self.assertEqual(fed[0]["detail"]["origin"], "live_collect")
        self.assertEqual(
            fed[0]["detail"]["normalization"]["applied"], False
        )
        poly = [
            row for row in rows if row["method"] == "POLYMARKET_CLOB"
        ]
        self.assertTrue(all(row["source_observed_at"] for row in poly))
        self.assertTrue(all(row["quality_status"] == "CURRENT" for row in poly))
        for row in poly:
            self.assertEqual(row["raw_probability_pct"], row["probability_pct"])
            self.assertIsNone(row["normalized_probability_pct"])

        mappings = self.store.validated_mappings(["2026-10-28"])
        self.assertEqual(len(mappings), 5)
        self.assertEqual(mappings[0]["external_event_id"], "606422")

    def test_partial_polymarket_collection_is_stored_but_not_a_current_comparison(self):
        meeting = synthetic_fed_meeting("2026-10-28")
        meeting["polymarket"] = {
            "meeting_date": "2026-10-28",
            "mapping_status": "VALIDATED",
            "event_id": "606422",
            "event_title": "Fed Decision in October?",
            "event_end_date": "2026-10-29T03:59:00Z",
            "mapping_evidence": {"validation_method": "test"},
            "data_status": "PARTIAL",
            "freshness": {"status": "PARTIAL", "age_days": None, "basis": "test"},
            "outcomes": [
                {
                    "outcome_bp": 25,
                    "open_ended": False,
                    "probability_pct": 33.5,
                    "source_timestamp": "2026-09-28T11:00:00Z",
                    "market_id": "606422-1",
                    "token_id": "606422-tok-25",
                    "question": "Will the Fed increase interest rates by 25 bps?",
                }
            ],
        }
        report = fedwatch_history.record_snapshot(
            self.store,
            {"retrieved_at": "2026-09-28T12:00:00Z", "meetings": [meeting]},
            clock=FixedClock(NOW),
        )
        self.assertEqual(report["polymarket_observations"], 1)
        poly_rows = [
            row for row in self.store.observations(meeting_date="2026-10-28")
            if row["method"] == "POLYMARKET_CLOB"
        ]
        self.assertEqual([row["quality_status"] for row in poly_rows], ["PARTIAL"])

        # The stored partial observation remains historically readable ...
        result = fedwatch_analytics.compute_analytics(
            self.store, "2026-10-28", 25, as_of=NOW
        )
        self.assertEqual(result["fed_side"]["state"], "OK")
        self.assertEqual(result["fed_side"]["latest"]["probability_pct"], 50.0)
        self.assertEqual(result["polymarket"]["latest"]["probability_pct"], 33.5)
        # ... but it must not become a current cross-source comparison.
        difference = result["difference"]
        self.assertEqual(difference["current_state"], "NON_CURRENT_LATEST_OBSERVATION")
        self.assertIsNone(difference["current_probability_diff_pp"])
        self.assertEqual(difference["polymarket_latest_quality_status"], "PARTIAL")

    def test_replaying_the_same_snapshot_is_a_duplicate(self):
        first = fedwatch_history.record_snapshot(
            self.store, self.snapshot["data"], clock=FixedClock(NOW)
        )
        count_after_first = self.store.count_observations()
        second = fedwatch_history.record_snapshot(
            self.store, self.snapshot["data"], clock=FixedClock(NOW)
        )
        self.assertGreater(first["fed_side_observations"], 0)
        self.assertEqual(
            self.store.count_observations(), count_after_first,
            "replaying the same snapshot must not add observation rows",
        )
        self.assertEqual(
            second["counts"], {"duplicate": first["fed_side_observations"] + first["polymarket_observations"]}
        )

    def test_a_later_collect_records_one_row_per_accepted_observation(self):
        first = fedwatch_history.record_snapshot(
            self.store, self.snapshot["data"], clock=FixedClock(NOW)
        )
        count_after_first = self.store.count_observations()
        later = NOW + timedelta(minutes=10)
        second_snapshot = build_snapshot_at(later)
        second = fedwatch_history.record_snapshot(
            self.store, second_snapshot["data"], clock=FixedClock(later)
        )
        accepted = second["fed_side_observations"] + second["polymarket_observations"]
        self.assertEqual(
            self.store.count_observations(), count_after_first + accepted,
            "every accepted observation instant is its own durable row",
        )
        self.assertTrue(all(result == "inserted" for result in second["records"]))
        feed = [
            row for row in self.store.observations(meeting_date="2026-10-28", outcome_bp=25)
            if row["method"] == "LIVE_INVESTING_DERIVED"
        ]
        self.assertEqual(
            [row["observed_at"] for row in feed],
            ["2026-09-28T12:00:00Z", "2026-09-28T12:10:00Z"],
        )
        self.assertGreater(first["fed_side_observations"], 0)

    def test_changed_probability_creates_a_distinct_observation(self):
        fedwatch_history.record_snapshot(
            self.store, self.snapshot["data"], clock=FixedClock(NOW)
        )
        changed_html = make_investing_html(
            [("Oct 28, 2026 02:00PM ET", [(3.75, 4.00, 40.0), (4.00, 4.25, 60.0)])]
        )
        transport = make_snapshot_transport(NOW)
        transport.add_text("fed-rate-monitor", changed_html)
        changed = build_snapshot_at(NOW + timedelta(minutes=30), transport)
        fedwatch_history.record_snapshot(
            self.store, changed["data"], clock=FixedClock(NOW + timedelta(minutes=30))
        )
        feed = [
            row for row in self.store.observations(meeting_date="2026-10-28")
            if row["method"] == "LIVE_INVESTING_DERIVED" and row["outcome_bp"] == 25
        ]
        self.assertEqual([row["probability_pct"] for row in feed], [70.0, 60.0])
        self.assertEqual(feed[0]["observed_at"], "2026-09-28T12:00:00Z")
        self.assertEqual(feed[1]["observed_at"], "2026-09-28T12:30:00Z")

    def test_provider_failures_are_not_stored_as_zero_probabilities(self):
        transport = make_snapshot_transport(NOW)
        transport.add_text("fed-rate-monitor", TransportError("HTTP 503", status_code=503))
        transport.add_text("DFEDTARU", TransportError("HTTP 500"))
        snapshot = build_snapshot_at(NOW, transport)
        report = fedwatch_history.record_snapshot(
            self.store, snapshot["data"], clock=FixedClock(NOW)
        )
        self.assertEqual(report["fed_side_observations"], 0)
        reasons = {skip["reason"] for skip in report["skipped"]}
        self.assertIn("FED_SIDE_UNAVAILABLE", reasons)
        fed_rows = [
            row for row in self.store.observations(meeting_date="2026-10-28")
            if row["method"] == "LIVE_INVESTING_DERIVED"
        ]
        self.assertEqual(fed_rows, [])
        self.assertGreater(report["polymarket_observations"], 0)

    def test_fred_failure_records_later_adjacent_locals_only(self):
        transport = make_snapshot_transport(NOW)
        transport.add_text("DFEDTARU", TransportError("HTTP 500"))
        snapshot = build_snapshot_at(NOW, transport)
        report = fedwatch_history.record_snapshot(
            self.store, snapshot["data"], clock=FixedClock(NOW)
        )
        self.assertGreater(report["fed_side_observations"], 0)
        reasons = {skip["reason"] for skip in report["skipped"]}
        self.assertIn("FED_SIDE_LOCAL_PROBABILITY_UNAVAILABLE", reasons)
        self.assertEqual(
            [row for row in self.store.observations(method="LIVE_INVESTING_DERIVED")
             if row["meeting_date"] == "2026-10-28"],
            [],
        )

    def test_restart_reload_preserves_history(self):
        fedwatch_history.record_snapshot(
            self.store, self.snapshot["data"], clock=FixedClock(NOW)
        )
        count = self.store.count_observations()
        reopened = FedwatchHistoryStore(self.store.path)
        self.assertEqual(reopened.count_observations(), count)
        meetings = reopened.list_meetings()
        self.assertIn("2026-10-28", [meeting["meeting_date"] for meeting in meetings])

    def test_collect_records_and_advances_lifecycle(self):
        store = self.store
        store.upsert_meeting("2026-09-16", default_status="UPCOMING")
        transport = make_snapshot_transport(NOW)
        upper_pairs = [
            (utc(2026, 9, 10).date(), 3.75),
            (utc(2026, 9, 16).date(), 3.75),
            (utc(2026, 9, 17).date(), 4.00),
            (utc(2026, 9, 18).date(), 4.00),
        ]
        lower_pairs = [
            (utc(2026, 9, 10).date(), 3.50),
            (utc(2026, 9, 16).date(), 3.50),
            (utc(2026, 9, 17).date(), 3.75),
            (utc(2026, 9, 18).date(), 3.75),
        ]
        transport.add_text("DFEDTARU", fred_csv("DFEDTARU", upper_pairs))
        transport.add_text("DFEDTARL", fred_csv("DFEDTARL", lower_pairs))
        result = fedwatch_history.collect(
            store, self.snapshot["data"], transport=transport, clock=FixedClock(NOW),
        )
        self.assertIn("recorded", result)
        self.assertIsNotNone(result["lifecycle"])
        resolved = result["lifecycle"]["resolved"]
        self.assertEqual(resolved, [{"meeting_date": "2026-09-16", "actual_outcome_bp": 25}])
        meeting = store.get_meeting("2026-09-16")
        self.assertEqual(meeting["status"], "RESOLVED")
        self.assertEqual(meeting["actual_outcome_bp"], 25)
        self.assertEqual(meeting["resolved_at"], "2026-09-28T12:00:00Z")
        self.assertEqual(
            meeting["actual_outcome_detail"]["method"],
            "FRED DFEDTARU/DFEDTARL first post-meeting target range",
        )


class LifecycleTests(unittest.TestCase):
    def test_resolve_actual_outcome_requires_agreeing_conventions(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 10).date(), "value": 3.75},
            {"date": utc(2026, 9, 16).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
            {"date": utc(2026, 9, 18).date(), "value": 4.00},
        ]
        lower = [
            {"date": utc(2026, 9, 10).date(), "value": 3.50},
            {"date": utc(2026, 9, 16).date(), "value": 3.50},
            {"date": utc(2026, 9, 17).date(), "value": 3.75},
            {"date": utc(2026, 9, 18).date(), "value": 3.75},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertTrue(verdict["resolvable"])
        self.assertEqual(verdict["outcome_bp"], 25)
        self.assertEqual(verdict["detail"]["rate_before_date"], "2026-09-16")
        self.assertEqual(verdict["detail"]["rate_after_date"], "2026-09-17")
        self.assertEqual(verdict["detail"]["rate_before"], {"date": "2026-09-16", "upper": 3.75, "lower": 3.50})
        self.assertEqual(verdict["detail"]["rate_after"], {"date": "2026-09-17", "upper": 4.00, "lower": 3.75})

    def test_two_changes_inside_the_window_attribute_only_the_first(self):
        # A second move inside the lookahead (an intermeeting change) must not
        # be accumulated onto the meeting.
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 16).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
            {"date": utc(2026, 9, 18).date(), "value": 4.25},
        ]
        lower = [
            {"date": utc(2026, 9, 15).date(), "value": 3.50},
            {"date": utc(2026, 9, 16).date(), "value": 3.50},
            {"date": utc(2026, 9, 17).date(), "value": 3.75},
            {"date": utc(2026, 9, 18).date(), "value": 4.00},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertTrue(verdict["resolvable"])
        self.assertEqual(verdict["outcome_bp"], 25)
        self.assertEqual(verdict["detail"]["rate_after_date"], "2026-09-17")
        self.assertEqual(verdict["detail"]["rate_after"]["upper"], 4.00)

    def test_meeting_day_only_series_is_not_a_hold(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 10).date(), "value": 3.75},
            {"date": utc(2026, 9, 16).date(), "value": 3.75},
        ]
        lower = [
            {"date": utc(2026, 9, 10).date(), "value": 3.50},
            {"date": utc(2026, 9, 16).date(), "value": 3.50},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertFalse(verdict["resolvable"])
        self.assertEqual(verdict["reason"], "FRED_COVERAGE_INSUFFICIENT")

    def test_first_post_meeting_hold_ignores_a_later_intermeeting_move(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 16).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 3.75},
            {"date": utc(2026, 9, 18).date(), "value": 4.00},
        ]
        lower = [
            {"date": utc(2026, 9, 15).date(), "value": 3.50},
            {"date": utc(2026, 9, 16).date(), "value": 3.50},
            {"date": utc(2026, 9, 17).date(), "value": 3.50},
            {"date": utc(2026, 9, 18).date(), "value": 3.75},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertTrue(verdict["resolvable"])
        self.assertEqual(verdict["outcome_bp"], 0)
        self.assertEqual(verdict["detail"]["rate_after_date"], "2026-09-17")
        self.assertEqual(verdict["detail"]["rate_after"]["upper"], 3.75)

    def test_lower_bound_only_change_is_rejected(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 3.75},
        ]
        lower = [
            {"date": utc(2026, 9, 15).date(), "value": 3.50},
            {"date": utc(2026, 9, 17).date(), "value": 3.25},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertFalse(verdict["resolvable"])
        self.assertEqual(verdict["reason"], "FRED_BOUNDS_INCONSISTENT")

    def test_resolve_missing_post_meeting_coverage_is_not_a_hold(self):
        end_date = utc(2026, 9, 16).date()
        upper = [{"date": utc(2026, 9, 15).date(), "value": 3.75}]
        lower = [{"date": utc(2026, 9, 15).date(), "value": 3.50}]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertFalse(verdict["resolvable"])
        self.assertEqual(verdict["reason"], "FRED_COVERAGE_INSUFFICIENT")

    def test_inverted_before_range_is_rejected(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 15).date(), "value": 3.50},
            {"date": utc(2026, 9, 17).date(), "value": 3.75},
        ]
        lower = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertFalse(verdict["resolvable"])
        self.assertEqual(verdict["reason"], "FRED_TARGET_RANGE_INVALID")

    def test_inverted_after_range_is_rejected(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 3.75},
        ]
        lower = [
            {"date": utc(2026, 9, 15).date(), "value": 3.50},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertFalse(verdict["resolvable"])
        self.assertEqual(verdict["reason"], "FRED_TARGET_RANGE_INVALID")

    def test_equal_bounds_range_is_rejected(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
        ]
        lower = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertFalse(verdict["resolvable"])
        self.assertEqual(verdict["reason"], "FRED_TARGET_RANGE_INVALID")

    def test_upper_only_post_date_does_not_block_a_later_paired_date(self):
        # A valid paired range exists on 09-18 even though the upper series has
        # an unpaired 09-17 row; the paired date must be selected.
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
            {"date": utc(2026, 9, 18).date(), "value": 4.00},
        ]
        lower = [
            {"date": utc(2026, 9, 15).date(), "value": 3.50},
            {"date": utc(2026, 9, 18).date(), "value": 3.75},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertTrue(verdict["resolvable"])
        self.assertEqual(verdict["outcome_bp"], 25)
        self.assertEqual(verdict["detail"]["rate_after_date"], "2026-09-18")

    def test_resolve_ambiguous_effective_date_is_not_guessed(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 10).date(), "value": 3.75},
            {"date": utc(2026, 9, 16).date(), "value": 4.00},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
        ]
        lower = [
            {"date": utc(2026, 9, 10).date(), "value": 3.50},
            {"date": utc(2026, 9, 16).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 3.75},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertFalse(verdict["resolvable"])
        self.assertEqual(verdict["reason"], "FRED_EFFECTIVE_DATE_AMBIGUOUS")

    def test_resolve_inconsistent_bounds_is_rejected(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 10).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 4.00},
        ]
        lower = [
            {"date": utc(2026, 9, 10).date(), "value": 3.50},
            {"date": utc(2026, 9, 17).date(), "value": 3.25},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertFalse(verdict["resolvable"])
        self.assertEqual(verdict["reason"], "FRED_BOUNDS_INCONSISTENT")

    def test_hold_is_resolved_once_post_meeting_data_exists(self):
        end_date = utc(2026, 9, 16).date()
        upper = [
            {"date": utc(2026, 9, 15).date(), "value": 3.75},
            {"date": utc(2026, 9, 17).date(), "value": 3.75},
        ]
        lower = [
            {"date": utc(2026, 9, 15).date(), "value": 3.50},
            {"date": utc(2026, 9, 17).date(), "value": 3.50},
        ]
        verdict = fedwatch_history.resolve_actual_outcome(end_date, upper, lower)
        self.assertTrue(verdict["resolvable"])
        self.assertEqual(verdict["outcome_bp"], 0)

    def test_evaluate_lifecycle_marks_pending_without_guessing(self):
        store = new_store(self)
        store.upsert_meeting("2026-09-16", default_status="UPCOMING")
        store.upsert_meeting("2026-10-28", default_status="UPCOMING")
        fred_history = {
            "retrieved_at": "2026-09-28T12:00:00Z",
            "source": "test",
            "upper": fred_series_rows([(utc(2026, 9, 10).date(), 3.75)]),
            "lower": fred_series_rows([(utc(2026, 9, 10).date(), 3.50)]),
        }
        result = fedwatch_history.evaluate_lifecycle(
            store, fred_history, clock=FixedClock(NOW)
        )
        self.assertEqual(result["evaluated"], 1)
        self.assertEqual(result["resolved"], [])
        self.assertEqual(
            result["pending"], [{"meeting_date": "2026-09-16", "reason": "FRED_COVERAGE_INSUFFICIENT"}]
        )
        meeting = store.get_meeting("2026-09-16")
        self.assertEqual(meeting["status"], "PENDING")
        self.assertEqual(meeting["status_reason"], "FRED_COVERAGE_INSUFFICIENT")
        self.assertIsNone(meeting["actual_outcome_bp"])
        self.assertEqual(store.get_meeting("2026-10-28")["status"], "UPCOMING")

    def test_resolved_and_pending_meetings_stop_receiving_live_observations(self):
        store = new_store(self)
        store.upsert_meeting("2026-09-16", default_status="UPCOMING")
        store.upsert_meeting("2026-09-15", default_status="UPCOMING")
        store.mark_resolved("2026-09-16", 25, "test", now=NOW)
        store.mark_pending("2026-09-15", "FRED_COVERAGE_INSUFFICIENT", now=NOW)
        data = {
            "retrieved_at": "2026-09-28T12:00:00Z",
            "meetings": [
                {"meeting_date": "2026-09-16", "status": "UPCOMING", "fed_side": None,
                 "polymarket": None, "comparison": [], "fomc_calendar": None},
                {"meeting_date": "2026-09-15", "status": "UPCOMING", "fed_side": None,
                 "polymarket": None, "comparison": [], "fomc_calendar": None},
            ],
        }
        report = fedwatch_history.record_snapshot(store, data, clock=FixedClock(NOW))
        reasons = {skip["meeting_date"]: skip["reason"] for skip in report["skipped"]}
        self.assertEqual(reasons["2026-09-16"], "MEETING_RESOLVED")
        self.assertEqual(reasons["2026-09-15"], "MEETING_AWAITING_RESOLUTION")
        self.assertEqual(store.count_observations(), 0)

    def test_collect_resolves_a_past_meeting_before_recording_it(self):
        store = new_store(self)
        store.upsert_meeting("2026-09-16", default_status="UPCOMING")
        transport = make_snapshot_transport(NOW)
        transport.add_text("DFEDTARU", fred_csv("DFEDTARU", [
            (utc(2026, 9, 10).date(), 3.75),
            (utc(2026, 9, 16).date(), 3.75),
            (utc(2026, 9, 17).date(), 4.00),
        ]))
        transport.add_text("DFEDTARL", fred_csv("DFEDTARL", [
            (utc(2026, 9, 10).date(), 3.50),
            (utc(2026, 9, 16).date(), 3.50),
            (utc(2026, 9, 17).date(), 3.75),
        ]))
        data = {
            "retrieved_at": "2026-09-28T12:00:00Z",
            "meetings": [synthetic_fed_meeting("2026-09-16")],
        }
        result = fedwatch_history.collect(
            store, data, transport=transport, clock=FixedClock(NOW)
        )
        self.assertEqual(
            result["lifecycle"]["resolved"],
            [{"meeting_date": "2026-09-16", "actual_outcome_bp": 25}],
        )
        self.assertEqual(
            {skip["meeting_date"]: skip["reason"] for skip in result["recorded"]["skipped"]},
            {"2026-09-16": "MEETING_RESOLVED"},
        )
        self.assertEqual(store.count_observations(), 0)

    def test_an_unseen_past_meeting_is_never_recorded_and_becomes_pending(self):
        store = new_store(self)
        data = {
            "retrieved_at": "2026-09-28T12:00:00Z",
            "meetings": [synthetic_fed_meeting("2026-09-01")],
        }
        report = fedwatch_history.record_snapshot(store, data, clock=FixedClock(NOW))
        self.assertEqual(
            {skip["meeting_date"]: skip["reason"] for skip in report["skipped"]},
            {"2026-09-01": "MEETING_DATE_IN_PAST"},
        )
        self.assertEqual(store.count_observations(), 0)
        meeting = store.get_meeting("2026-09-01")
        self.assertEqual(meeting["status"], "PENDING")
        self.assertEqual(meeting["status_reason"], "MEETING_DATE_IN_PAST")

    def test_fred_unavailable_marks_past_meetings_pending(self):
        store = new_store(self)
        store.upsert_meeting("2026-09-16", default_status="UPCOMING")
        result = fedwatch_history.evaluate_lifecycle(
            store, None, clock=FixedClock(NOW)
        )
        self.assertEqual(
            result["pending"],
            [{"meeting_date": "2026-09-16", "reason": "FRED_SOURCE_UNAVAILABLE"}],
        )
        self.assertEqual(result["errors"][0]["code"], "FRED_SOURCE_UNAVAILABLE")
        meeting = store.get_meeting("2026-09-16")
        self.assertEqual(meeting["status"], "PENDING")
        self.assertEqual(meeting["status_reason"], "FRED_SOURCE_UNAVAILABLE")

    def test_overview_exposes_validated_outcomes_without_observations(self):
        store = new_store(self)
        store.upsert_meeting("2026-10-28")
        store.upsert_mapping_outcome(
            "2026-10-28", fedwatch_history.POLY_SOURCE, fedwatch_history.POLY_METHOD,
            -50, True, "VALIDATED", "event", "October decision", "market",
            "token", "50 bp or more cut", {},
        )
        overview = fedwatch_history.meetings_overview(store)
        meeting = overview["meetings"][0]
        self.assertEqual(meeting["observations"], {})
        self.assertEqual(meeting["polymarket_mapping"]["outcome_count"], 1)
        self.assertEqual(meeting["polymarket_mapping"]["outcomes"], [{
            "outcome_bp": -50, "open_ended": True,
            "external_market_id": "market", "external_token_id": "token",
            "question": "50 bp or more cut",
        }])

    def test_resolved_meeting_history_remains_readable(self):
        store = new_store(self)
        store.upsert_meeting("2026-09-16", default_status="UPCOMING")
        store.record_observation(
            meeting_date="2026-09-16", source="investing",
            method="LIVE_INVESTING_DERIVED", outcome_bp=25, open_ended=False,
            probability_pct=64.0, raw_probability_pct=None, normalized_probability_pct=None,
            observed_at="2026-09-15T12:00:00Z", source_observed_at=None,
            retrieved_at="2026-09-15T12:00:00Z", quality_status="OK",
            freshness_status="SOURCE_TIMESTAMP_UNAVAILABLE",
            digest="digest-a", detail={"origin": "test"},
        )
        store.mark_resolved("2026-09-16", 25, "test", now=NOW)
        series = fedwatch_history.series(store, "2026-09-16")
        self.assertEqual(len(series["observations"]), 1)
        overview = fedwatch_history.meetings_overview(store)
        meeting = overview["meetings"][0]
        self.assertEqual(meeting["status"], "RESOLVED")
        self.assertEqual(meeting["actual_outcome_bp"], 25)
        self.assertIn("LIVE_INVESTING_DERIVED", meeting["observations"])


class BackfillTests(unittest.TestCase):
    def setUp(self):
        self.store = new_store(self)
        self.store.upsert_meeting("2026-10-28", default_status="UPCOMING")
        self.store.upsert_mapping_outcome(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "606422-3", "tok-25", "question",
            {"validation_method": "test"},
        )

    def transport_with(self, points_by_token):
        transport = FakeTransport()
        transport.add_json("prices-history", make_clob_history(points_by_token))
        return transport

    def test_backfill_imports_daily_points_and_dedupes_same_day(self):
        transport = self.transport_with(
            {
                "tok-25": [
                    {"t": epoch(utc(2026, 8, 1)), "p": 0.30},
                    {"t": epoch(utc(2026, 8, 2)), "p": 0.35},
                    {"t": epoch(utc(2026, 8, 3)), "p": 0.32},
                    {"t": epoch(utc(2026, 8, 3, 18, 0)), "p": 0.33},
                ]
            }
        )
        result = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertEqual(result["errors"], [])
        entry = result["backfills"][0]
        self.assertEqual(entry["status"], "OK")
        self.assertEqual(entry["points_accepted"], 3)
        self.assertEqual(entry["observations_inserted"], 3)
        rows = self.store.observations(
            meeting_date="2026-10-28", method="POLYMARKET_CLOB", outcome_bp=25
        )
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[-1]["probability_pct"], 33.0)
        self.assertEqual(rows[-1]["quality_status"], "BACKFILLED")
        self.assertEqual(rows[-1]["source_observed_at"], "2026-08-03T18:00:00Z")

    def test_live_and_backfill_share_the_clob_observation_identity(self):
        # The same CLOB point (token, timestamp, probability) reaching the store
        # through the live path and then the backfill path must be one accepted
        # observation, with the live provenance retained.
        snapshot = build_snapshot_at(NOW)
        fedwatch_history.record_snapshot(
            self.store, snapshot["data"], clock=FixedClock(NOW)
        )
        live_rows = self.store.observations(method="POLYMARKET_CLOB")
        self.assertTrue(live_rows)
        live_point_epoch = epoch(NOW - timedelta(days=1))
        prices = {}
        for fixture_name in (FIXTURE_PM_OCTOBER, FIXTURE_PM_DECEMBER):
            for market in fixture_json(fixture_name)["markets"]:
                token = json.loads(market["clobTokenIds"])[0]
                prices[token] = float(json.loads(market["outcomePrices"])[0])

        def route(url, params):
            token = params["market"]
            return {"history": [{"t": live_point_epoch, "p": prices[token]}]}

        transport = FakeTransport()
        transport.add_json("prices-history", route)
        result = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertTrue(all(entry["status"] == "OK" for entry in result["backfills"]))
        self.assertEqual(
            sum(entry["observations_inserted"] for entry in result["backfills"]), 0
        )
        self.assertEqual(sum(entry["duplicates"] for entry in result["backfills"]), 10)
        rows = self.store.observations(method="POLYMARKET_CLOB")
        self.assertEqual(len(rows), len(live_rows))
        for row in rows:
            self.assertEqual(row["quality_status"], "CURRENT")
            self.assertEqual(row["detail"]["origin"], "live_collect")
            self.assertEqual(row["observation_count"], 2)

    def test_repeat_backfill_is_skipped_fresh_and_idempotent_when_forced(self):
        transport = self.transport_with(
            {
                "tok-25": [
                    {"t": epoch(utc(2026, 8, 1)), "p": 0.30},
                    {"t": epoch(utc(2026, 8, 2)), "p": 0.35},
                ]
            }
        )
        fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        count = self.store.count_observations()
        skipped = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW + timedelta(hours=1)), sleep=NO_SLEEP
        )
        self.assertEqual(skipped["backfills"][0]["status"], "SKIPPED_FRESH")
        forced = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW + timedelta(hours=1)),
            sleep=NO_SLEEP, force=True,
        )
        self.assertEqual(forced["backfills"][0]["status"], "OK")
        self.assertEqual(forced["backfills"][0]["observations_inserted"], 0)
        self.assertEqual(forced["backfills"][0]["duplicates"], 2)
        self.assertEqual(self.store.count_observations(), count)

    def test_resolved_meeting_backfill_is_skipped_unless_forced(self):
        self.store.mark_resolved("2026-10-28", 25, "test", now=NOW)
        transport = self.transport_with({"tok-25": [{"t": 1785715200, "p": 0.30}]})
        fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        again = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW + timedelta(days=30)), sleep=NO_SLEEP
        )
        self.assertEqual(again["backfills"][0]["status"], "SKIPPED_RESOLVED")
        forced = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW + timedelta(days=30)),
            sleep=NO_SLEEP, force=True,
        )
        self.assertEqual(forced["backfills"][0]["status"], "OK")

    def test_empty_history_is_recorded_truthfully(self):
        result = fedwatch_history.backfill_polymarket(
            self.store, self.transport_with({"tok-25": []}),
            clock=FixedClock(NOW), sleep=NO_SLEEP,
        )
        self.assertEqual(result["backfills"][0]["status"], "EMPTY")
        state = self.store.get_backfill_state(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False
        )
        self.assertEqual(state["point_count"], 0)
        self.assertEqual(self.store.count_observations(), 0)

    def test_provider_error_is_attributed_and_recorded(self):
        transport = FakeTransport()
        transport.add_json("prices-history", TransportError("HTTP 500", status_code=500))
        result = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertEqual(result["backfills"][0]["status"], "PROVIDER_ERROR")
        self.assertEqual(result["errors"][0]["provider"], "polymarket")
        self.assertEqual(result["errors"][0]["code"], "POLYMARKET_MARKET_DATA_UNAVAILABLE")
        state = self.store.get_backfill_state(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False
        )
        self.assertEqual(state["status"], "PROVIDER_ERROR")

    def test_malformed_points_are_dropped_and_partial(self):
        transport = self.transport_with(
            {
                "tok-25": [
                    {"t": "not-a-time", "p": 0.30},
                    {"t": 1785715200, "p": 2.5},
                    {"t": 4102444800, "p": 0.5},
                    {"t": 1785801600, "p": 0.35},
                ]
            }
        )
        result = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = result["backfills"][0]
        self.assertEqual(entry["status"], "PARTIAL")
        self.assertEqual(entry["points_accepted"], 1)
        self.assertEqual(entry["malformed_counts"], {"malformed": 1, "future": 1, "out_of_range": 1, "conflicting": 0})
        self.assertTrue(result["warnings"])

    def test_missing_token_and_no_mappings_are_reported(self):
        self.store.upsert_mapping_outcome(
            "2026-12-09", "polymarket", "POLYMARKET_CLOB", 0, False, "VALIDATED",
            "770450", "Fed Decision in December?", "770450-2", None, "question", None,
        )
        transport = self.transport_with({"tok-25": [{"t": 1785715200, "p": 0.30}]})
        result = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        statuses = {entry["meeting_date"]: entry["status"] for entry in result["backfills"]}
        self.assertEqual(statuses["2026-12-09"], "NO_TOKEN")
        empty_store = new_store(self)
        empty = fedwatch_history.backfill_polymarket(
            empty_store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertEqual(empty["backfills"], [])
        self.assertTrue(empty["warnings"])

    def test_resolved_meeting_retries_after_a_provider_error(self):
        error_transport = FakeTransport()
        error_transport.add_json("prices-history", TransportError("HTTP 500", status_code=500))
        first = fedwatch_history.backfill_polymarket(
            self.store, error_transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertEqual(first["backfills"][0]["status"], "PROVIDER_ERROR")
        self.store.mark_resolved("2026-10-28", 25, "test", now=NOW)
        healthy = self.transport_with({"tok-25": [{"t": epoch(utc(2026, 8, 1)), "p": 0.30}]})
        retry = fedwatch_history.backfill_polymarket(
            self.store, healthy, clock=FixedClock(NOW + timedelta(days=32)), sleep=NO_SLEEP
        )
        self.assertEqual(retry["backfills"][0]["status"], "OK")
        self.assertGreater(self.store.count_observations(), 0)

    def test_demoted_mapping_blocks_backfill_for_a_current_meeting(self):
        snapshot = build_snapshot_at(NOW)
        data = copy.deepcopy(snapshot["data"])
        october = next(m for m in data["meetings"] if m["meeting_date"] == "2026-10-28")
        october["polymarket"]["mapping_status"] = "NOT_FOUND"
        fedwatch_history.record_snapshot(self.store, data, clock=FixedClock(NOW))
        mappings = self.store.validated_mappings(["2026-10-28"])
        self.assertEqual(
            mappings[0]["last_revalidation_status"], "NOT_FOUND"
        )
        transport = self.transport_with({"tok-25": [{"t": epoch(utc(2026, 8, 1)), "p": 0.30}]})
        blocked = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertEqual(blocked["backfills"][0]["status"], "MAPPING_NOT_CURRENT")
        self.assertEqual(
            self.store.observations(meeting_date="2026-10-28", method="POLYMARKET_CLOB"),
            [],
        )

        self.store.mark_resolved("2026-10-28", 25, "test", now=NOW)
        allowed = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW + timedelta(days=32)), sleep=NO_SLEEP
        )
        self.assertEqual(allowed["backfills"][0]["status"], "OK")

    def test_changed_token_on_a_resolved_meeting_is_refetched(self):
        transport = self.transport_with(
            {"tok-25": [{"t": epoch(utc(2026, 8, 1)), "p": 0.30}], "tok-new": [{"t": epoch(utc(2026, 8, 2)), "p": 0.40}]}
        )
        fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.store.upsert_mapping_outcome(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "606422-3", "tok-new", "question",
            {"validation_method": "test"},
        )
        self.store.mark_resolved("2026-10-28", 25, "test", now=NOW)
        refetched = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW + timedelta(days=32)), sleep=NO_SLEEP
        )
        self.assertEqual(refetched["backfills"][0]["status"], "OK")
        self.assertEqual(refetched["backfills"][0]["token_id"], "tok-new")

    def test_backfill_after_live_collection_preserves_the_live_observation(self):
        snapshot = build_snapshot_at(NOW)
        fedwatch_history.record_snapshot(
            self.store, snapshot["data"], clock=FixedClock(NOW)
        )
        live_rows = self.store.observations(
            meeting_date="2026-10-28", method="POLYMARKET_CLOB"
        )
        self.assertTrue(live_rows)
        live_by_key = {(row["outcome_bp"], row["observed_at"]): row for row in live_rows}
        transport = FakeTransport()
        transport.add_json(
            "prices-history",
            lambda url, params: {"history": [{"t": epoch(utc(2026, 8, 1)), "p": 0.30}]},
        )
        result = fedwatch_history.backfill_polymarket(
            self.store, transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertTrue(all(entry["status"] == "OK" for entry in result["backfills"]))
        rows = self.store.observations(meeting_date="2026-10-28", method="POLYMARKET_CLOB")
        self.assertEqual(len(rows), len(live_rows) * 2)
        for row in rows:
            if (row["outcome_bp"], row["observed_at"]) in live_by_key:
                original = live_by_key[(row["outcome_bp"], row["observed_at"])]
                self.assertEqual(row["probability_pct"], original["probability_pct"])
                self.assertEqual(row["quality_status"], "CURRENT")

    def test_recreated_token_at_the_same_timestamp_preserves_old_observations(self):
        self.store.upsert_mapping_outcome(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "606422-3", "tok-old", "question",
            {"validation_method": "test"},
        )
        first_transport = FakeTransport()
        first_transport.add_json(
            "prices-history",
            lambda url, params: {"history": [{"t": epoch(utc(2026, 8, 1)), "p": 0.30}]},
        )
        fedwatch_history.backfill_polymarket(
            self.store, first_transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.store.upsert_mapping_outcome(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "606422-3", "tok-new", "question",
            {"validation_method": "test"},
        )
        recreated = FakeTransport()
        recreated.add_json(
            "prices-history",
            lambda url, params: {"history": [{"t": epoch(utc(2026, 8, 1)), "p": 0.40}]},
        )
        result = fedwatch_history.backfill_polymarket(
            self.store, recreated, clock=FixedClock(NOW), sleep=NO_SLEEP, force=True
        )
        self.assertEqual(result["backfills"][0]["status"], "OK")
        rows = self.store.observations(
            meeting_date="2026-10-28", method="POLYMARKET_CLOB", outcome_bp=25
        )
        self.assertEqual(
            {(row["instrument_key"], row["probability_pct"]) for row in rows},
            {("tok-old", 30.0), ("tok-new", 40.0)},
        )
        old = next(row for row in rows if row["instrument_key"] == "tok-old")
        self.assertEqual(old["detail"]["origin"], "clob_prices_history_backfill")

    def test_backfill_only_uses_validated_mappings_for_the_requested_meeting(self):
        self.store.upsert_mapping_outcome(
            "2026-12-09", "polymarket", "POLYMARKET_CLOB", 0, False, "VALIDATED",
            "770450", "Fed Decision in December?", "770450-2", "tok-0", "question", None,
        )
        transport = self.transport_with(
            {"tok-25": [{"t": 1785715200, "p": 0.30}], "tok-0": [{"t": 1785715200, "p": 0.55}]}
        )
        result = fedwatch_history.backfill_polymarket(
            self.store, transport, meeting_dates=["2026-12-09"],
            clock=FixedClock(NOW), sleep=NO_SLEEP,
        )
        self.assertEqual(len(result["backfills"]), 1)
        self.assertEqual(result["backfills"][0]["meeting_date"], "2026-12-09")
        self.assertEqual(self.store.count_observations(), 1)


class RecordingRobustnessTests(unittest.TestCase):
    def test_malformed_polymarket_timestamp_is_skipped_not_fatal(self):
        store = new_store(self)
        data = {
            "retrieved_at": "2026-09-28T12:00:00Z",
            "meetings": [
                {
                    "meeting_date": "2026-10-28",
                    "status": "UPCOMING",
                    "fomc_calendar": None,
                    "fed_side": None,
                    "comparison": [],
                    "polymarket": {
                        "mapping_status": "VALIDATED",
                        "event_id": "606422",
                        "outcomes": [
                            {"outcome_bp": 0, "open_ended": False, "market_id": "m0",
                             "token_id": "t0", "question": "q", "probability_pct": 30.0,
                             "source_timestamp": "not-a-time"},
                            {"outcome_bp": 25, "open_ended": False, "market_id": "m1",
                             "token_id": "t1", "question": "q", "probability_pct": 70.0,
                             "source_timestamp": "2026-09-27T12:00:00Z"},
                        ],
                        "data_status": "CURRENT",
                        "freshness": {"status": "CURRENT"},
                    },
                }
            ],
        }
        report = fedwatch_history.record_snapshot(store, data, clock=FixedClock(NOW))
        self.assertEqual(report["polymarket_observations"], 1)
        self.assertIn(
            "MALFORMED_POLYMARKET_TIMESTAMP",
            {skip["reason"] for skip in report["skipped"]},
        )

    def test_non_string_meeting_date_is_skipped_not_fatal(self):
        store = new_store(self)
        data = {
            "retrieved_at": "2026-09-28T12:00:00Z",
            "meetings": [{"meeting_date": 20261028, "fed_side": None, "polymarket": None}],
        }
        report = fedwatch_history.record_snapshot(store, data, clock=FixedClock(NOW))
        self.assertIn(
            "MALFORMED_MEETING_DATE", {skip["reason"] for skip in report["skipped"]}
        )
        self.assertEqual(store.count_observations(), 0)


class ImportZqTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.store = FedwatchHistoryStore(self.root / "history.db")
        self.zq_dir = self.root / "zq"
        self.zq_dir.mkdir()

    def _write_contracts(self):
        header = "Date Time,Open,High,Low,Close,Change,Volume,Open Interest\n"
        for month, close in (
            (9, 96.5), (10, 96.4), (11, 96.4), (12, 96.3),
        ):
            code = {9: "U", 10: "V", 11: "X", 12: "Z"}[month]
            (self.zq_dir / f"ZQ{code}26.csv").write_text(
                f"Symbol: ZQ{code}26\n" + header
                + f"2026-07-10,0,0,0,{close},0,10,5000\n"
                + f"2026-09-10,0,0,0,{close},0,10,5000\n"
                + f"2026-09-28,0,0,0,{close},0,10,5000\n",
                encoding="utf-8",
            )
        (self.zq_dir / "ZQF27.csv").write_text(
            "Symbol: ZQF27\n" + header
            + "2026-07-10,0,0,0,96.3,0,10,5000\n"
            + "2026-09-28,0,0,0,96.3,0,10,5000\n",
            encoding="utf-8",
        )

    def test_import_zq_offline_with_fixture_transport(self):
        self._write_contracts()
        transport = make_snapshot_transport(NOW)
        payload = fedwatch_history.import_zq(
            self.store, transport, self.zq_dir,
            [utc(2026, 9, 28).date()], clock=FixedClock(NOW),
        )
        self.assertEqual(len(payload["watch_dates"]), 1)
        watched = payload["watch_dates"][0]
        self.assertGreater(watched["local_row_count"], 0)
        self.assertEqual(watched["recorded"]["processed"], watched["local_row_count"])
        self.assertEqual(
            watched["current_target_range"],
            {"upper": 4.0, "lower": 3.75, "observation_date": "2026-09-28"},
        )
        stored = self.store.observations(method=fedwatch_history.FED_METHOD_ZQ)
        self.assertGreater(len(stored), 0)
        self.assertTrue(all(row["quality_status"] == "RECONSTRUCTED" for row in stored))
        self.assertTrue(
            all(row["detail"]["watch_date"] == "2026-09-28" for row in stored)
        )

    def test_import_zq_uses_only_a_paired_range_date(self):
        self._write_contracts()
        transport = make_snapshot_transport(NOW)
        transport.add_text("DFEDTARU", fred_csv("DFEDTARU", [
            (utc(2026, 6, 30).date(), 3.50),
            (utc(2026, 7, 10).date(), 3.75),
            (utc(2026, 9, 28).date(), 4.00),
        ]))
        transport.add_text("DFEDTARL", fred_csv("DFEDTARL", [
            (utc(2026, 6, 30).date(), 3.25),
            (utc(2026, 9, 28).date(), 3.75),
        ]))
        payload = fedwatch_history.import_zq(
            self.store, transport, self.zq_dir,
            [utc(2026, 7, 14).date()], clock=FixedClock(NOW),
        )
        self.assertEqual(
            payload["watch_dates"][0]["current_target_range"],
            {"upper": 3.50, "lower": 3.25, "observation_date": "2026-06-30"},
        )

    def test_import_zq_fails_closed_without_a_paired_range_date(self):
        self._write_contracts()
        transport = make_snapshot_transport(NOW)
        transport.add_text("DFEDTARU", fred_csv("DFEDTARU", [
            (utc(2026, 7, 10).date(), 3.75),
        ]))
        transport.add_text("DFEDTARL", fred_csv("DFEDTARL", [
            (utc(2026, 6, 30).date(), 3.25),
        ]))
        payload = fedwatch_history.import_zq(
            self.store, transport, self.zq_dir,
            [utc(2026, 7, 14).date()], clock=FixedClock(NOW),
        )
        self.assertEqual(payload["watch_dates"], [])
        self.assertEqual(
            [error["code"] for error in payload["errors"]],
            ["FEDWATCH_ZQ_RECONSTRUCTION_INCOMPLETE"],
        )
        self.assertEqual(self.store.observations(method=fedwatch_history.FED_METHOD_ZQ), [])

    def test_import_zq_rejects_an_invalid_paired_range(self):
        self._write_contracts()
        transport = make_snapshot_transport(NOW)
        transport.add_text("DFEDTARU", fred_csv("DFEDTARU", [
            (utc(2026, 7, 10).date(), 3.50),
        ]))
        transport.add_text("DFEDTARL", fred_csv("DFEDTARL", [
            (utc(2026, 7, 10).date(), 3.75),
        ]))
        payload = fedwatch_history.import_zq(
            self.store, transport, self.zq_dir,
            [utc(2026, 7, 14).date()], clock=FixedClock(NOW),
        )
        self.assertEqual(payload["watch_dates"], [])
        self.assertEqual(
            [error["code"] for error in payload["errors"]],
            ["FRED_TARGET_RANGE_INVALID"],
        )
        self.assertEqual(self.store.observations(method=fedwatch_history.FED_METHOD_ZQ), [])

    def test_import_zq_uses_the_historical_watch_date_rate_and_reloads(self):
        # A historical watch date must use the target range in effect then, not
        # the latest range, and the reconstructed rows must survive a reload.
        self._write_contracts()
        transport = make_snapshot_transport(NOW)
        transport.add_text("DFEDTARU", fred_csv("DFEDTARU", [
            (utc(2026, 7, 10).date(), 3.75),
            (utc(2026, 9, 28).date(), 4.00),
        ]))
        transport.add_text("DFEDTARL", fred_csv("DFEDTARL", [
            (utc(2026, 7, 10).date(), 3.50),
            (utc(2026, 9, 28).date(), 3.75),
        ]))
        payload = fedwatch_history.import_zq(
            self.store, transport, self.zq_dir,
            [utc(2026, 7, 14).date()], clock=FixedClock(NOW),
        )
        watched = payload["watch_dates"][0]
        self.assertEqual(
            watched["current_target_range"],
            {"upper": 3.75, "lower": 3.50, "observation_date": "2026-07-10"},
        )
        self.assertGreater(watched["local_row_count"], 0)
        reopened = FedwatchHistoryStore(self.store.path)
        stored = reopened.observations(method=fedwatch_history.FED_METHOD_ZQ)
        self.assertEqual(len(stored), watched["local_row_count"])
        self.assertTrue(all(row["observed_at"] == "2026-07-14T00:00:00Z" for row in stored))
        self.assertTrue(
            all(row["detail"]["current_target_range"]["upper"] == 3.75 for row in stored)
        )


class StorageGrowthTests(unittest.TestCase):
    def test_replaying_the_same_snapshot_never_grows_history(self):
        store = new_store(self)
        snapshot = build_snapshot_at(NOW)
        fedwatch_history.record_snapshot(store, snapshot["data"], clock=FixedClock(NOW))
        initial_rows = store.count_observations()
        initial_size = store.path.stat().st_size
        for _ in range(25):
            fedwatch_history.record_snapshot(
                store, snapshot["data"], clock=FixedClock(NOW)
            )
        self.assertEqual(store.count_observations(), initial_rows)
        self.assertLessEqual(store.path.stat().st_size - initial_size, 4096)

    def test_each_accepted_instant_adds_exactly_one_row_per_observation(self):
        store = new_store(self)
        snapshot = build_snapshot_at(NOW)
        first = fedwatch_history.record_snapshot(
            store, snapshot["data"], clock=FixedClock(NOW)
        )
        accepted_fed = first["fed_side_observations"]
        accepted_poly = first["polymarket_observations"]
        for index in range(1, 11):
            moment = NOW + timedelta(minutes=10 * index)
            data = dict(snapshot["data"])
            data["retrieved_at"] = moment.strftime("%Y-%m-%dT%H:%M:%SZ")
            report = fedwatch_history.record_snapshot(store, data, clock=FixedClock(moment))
            self.assertEqual(report["fed_side_observations"], accepted_fed)
            self.assertEqual(report["counts"].get("duplicate", 0), accepted_poly)
        self.assertEqual(
            store.count_observations(), accepted_fed + accepted_poly + 10 * accepted_fed
        )

    def test_repeated_collect_with_changing_values_records_each_instant(self):
        store = new_store(self)
        rows_per_collect = []
        for index in range(4):
            moment = NOW + timedelta(minutes=10 * index)
            html = make_investing_html(
                [("Oct 28, 2026 02:00PM ET",
                  [(3.75, 4.00, 30.0 + index), (4.00, 4.25, 70.0 - index)])]
            )
            transport = make_snapshot_transport(moment)
            transport.add_text("fed-rate-monitor", html)
            snapshot = build_snapshot_at(moment, transport)
            fedwatch_history.record_snapshot(store, snapshot["data"], clock=FixedClock(moment))
            fed_rows = [
                row for row in store.observations(
                    meeting_date="2026-10-28", method="LIVE_INVESTING_DERIVED"
                ) if row["outcome_bp"] == 0
            ]
            rows_per_collect.append(len(fed_rows))
        self.assertEqual(rows_per_collect, [1, 2, 3, 4])


class CliTests(unittest.TestCase):
    def seed(self) -> FedwatchHistoryStore:
        store = new_store(self)
        snapshot = build_snapshot_at(NOW)
        fedwatch_history.record_snapshot(store, snapshot["data"], clock=FixedClock(NOW))
        store.upsert_meeting("2026-09-16", default_status="UPCOMING")
        store.mark_resolved("2026-09-16", 25, "test", now=NOW)
        return store

    def run_cli(self, *args) -> tuple[int, dict, str]:
        completed = subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            capture_output=True, text=True, timeout=120,
        )
        payload = json.loads(completed.stdout) if completed.stdout.strip() else None
        return completed.returncode, payload, completed.stderr

    def test_history_commands_read_a_persisted_database_in_a_fresh_process(self):
        store = self.seed()
        code, payload, stderr = self.run_cli(
            "history_meetings", "--db", str(store.path)
        )
        self.assertEqual(code, 0, stderr)
        self.assertTrue(payload["success"])
        dates = [meeting["meeting_date"] for meeting in payload["data"]["meetings"]]
        self.assertIn("2026-10-28", dates)
        self.assertIn("2026-09-16", dates)
        resolved = next(
            meeting for meeting in payload["data"]["meetings"]
            if meeting["meeting_date"] == "2026-09-16"
        )
        self.assertEqual(resolved["status"], "RESOLVED")
        self.assertEqual(resolved["actual_outcome_bp"], 25)

        code, payload, stderr = self.run_cli(
            "history_series", "--db", str(store.path), "--meeting", "2026-10-28"
        )
        self.assertEqual(code, 0, stderr)
        self.assertGreater(payload["data"]["observation_count"], 0)

        code, payload, stderr = self.run_cli(
            "history_analytics", "--db", str(store.path),
            "--meeting", "2026-10-28", "--outcome-bp", "25",
            "--as-of", "2026-09-28T12:00:00Z",
        )
        self.assertEqual(code, 0, stderr)
        difference = payload["data"]["difference"]
        self.assertEqual(difference["sign_convention"], "Polymarket - Fed-side")
        self.assertEqual(difference["current_state"], "OK")
        self.assertIsNotNone(difference["current_probability_diff_pp"])
        self.assertEqual(
            payload["data"]["fed_side"]["changes"]["30d"]["state"], "INSUFFICIENT_HISTORY"
        )

    def test_cli_argument_validation_fails_closed(self):
        code, payload, _ = self.run_cli("history_series", "--outcome-bp", "abc")
        self.assertEqual(code, 1)
        self.assertEqual(payload["error"]["code"], "FEDWATCH_INVALID_ARGUMENTS")

        code, payload, _ = self.run_cli("history_series")
        self.assertEqual(code, 1)
        self.assertIn("--meeting", payload["error"]["error"])

        store = self.seed()
        code, payload, _ = self.run_cli(
            "history_analytics", "--db", str(store.path),
            "--meeting", "2026-10-28", "--outcome-bp", "25",
            "--fed-method", "NOT_A_METHOD",
        )
        self.assertEqual(code, 1)
        self.assertEqual(payload["error"]["code"], "FEDWATCH_INVALID_ARGUMENTS")

    def test_cli_help_lists_history_commands(self):
        code, payload, stderr = self.run_cli("help")
        self.assertEqual(code, 0, stderr)
        for command in (
            "collect", "history_meetings", "history_series",
            "history_analytics", "history_backfill", "history_zq_import",
        ):
            self.assertIn(command, payload["commands"])

    def test_cli_collect_persists_through_the_history_store(self):
        store = new_store(self)
        fixture = build_snapshot_at(NOW)
        original = fedwatch_snapshot.build_snapshot
        fedwatch_snapshot.build_snapshot = lambda *args, **kwargs: fixture
        try:
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                import fedwatch_data

                code = fedwatch_data.main(["collect", "--db", str(store.path)])
        finally:
            fedwatch_snapshot.build_snapshot = original
        self.assertEqual(code, 0, stdout.getvalue())
        payload = json.loads(stdout.getvalue())
        self.assertTrue(payload["success"])
        self.assertIn("history", payload["data"])
        self.assertGreater(payload["data"]["history"]["recorded"]["fed_side_observations"], 0)
        self.assertGreater(store.count_observations(), 0)


if __name__ == "__main__":
    unittest.main()
