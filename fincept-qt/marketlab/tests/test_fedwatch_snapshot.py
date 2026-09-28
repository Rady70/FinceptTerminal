"""Deterministic tests for the MarketLab FedWatch snapshot contract and CLI.

The full snapshot is assembled from captured provider fixtures over a fake
transport with a fixed clock:

* raw Investing values and validated/normalized Fed-side values stay distinct;
* the current FRED target range is present with its latest observation date;
* the FOMC calendar supplies meeting metadata;
* Polymarket validation and current prices produce the comparison with
  ``probability_diff_pp = Polymarket - Fed-side``;
* every provider carries source and retrieval timing;
* each provider's failure is attributed to that provider only and turns the
  envelope into an explicit partial result without dropping truthful data;
* the CLI emits one JSON document per invocation, fail-closed.
"""

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import unittest
from datetime import timedelta
from pathlib import Path

from fedwatch_test_support import (
    FIXTURE_FRED_LOWER,
    FIXTURE_FRED_UPPER,
    FIXTURE_FOMC_CALENDAR,
    FIXTURE_INVESTING_LIVE,
    FIXTURE_PM_DECEMBER,
    FIXTURE_PM_OCTOBER,
    FakeTransport,
    FixedClock,
    epoch,
    fixture_json,
    fixture_text,
    make_clob_history,
    make_fed_decision_event,
    make_investing_html,
    utc,
)

from fedwatch import snapshot as fedwatch_snapshot
from fedwatch.errors import FedwatchError
from fedwatch.transport import TransportError

NOW = utc(2026, 9, 28, 12, 0, 0)
NO_SLEEP = lambda _seconds: None  # noqa: E731
SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "fedwatch_data.py"


def build_snapshot_transport(events=None) -> FakeTransport:
    transport = FakeTransport()
    transport.add_text("DFEDTARU", fixture_text(FIXTURE_FRED_UPPER))
    transport.add_text("DFEDTARL", fixture_text(FIXTURE_FRED_LOWER))
    transport.add_text("fomccalendars", fixture_text(FIXTURE_FOMC_CALENDAR))
    transport.add_text("fed-rate-monitor", fixture_text(FIXTURE_INVESTING_LIVE))
    events = events if events is not None else [
        fixture_json(FIXTURE_PM_OCTOBER),
        fixture_json(FIXTURE_PM_DECEMBER),
    ]
    transport.add_json(
        "gamma-api.polymarket.com/events",
        lambda url, params: events if params.get("tag_slug") == "fed-rates" else [],
    )
    transport.add_json(
        "public-search",
        lambda url, params: {"events": events if params["q"] == "FOMC" else []},
    )
    points = {}
    for event in events:
        for market in event.get("markets", []):
            token = json.loads(market["clobTokenIds"])[0]
            price = float(json.loads(market["outcomePrices"])[0])
            points[token] = [{"t": epoch(NOW - timedelta(days=1)), "p": price}]
    transport.add_json("prices-history", make_clob_history(points))
    return transport


class SnapshotContractTests(unittest.TestCase):
    def setUp(self):
        self.snapshot = fedwatch_snapshot.build_snapshot(
            build_snapshot_transport(), clock=FixedClock(NOW), sleep=NO_SLEEP
        )

    def test_envelope_is_complete_and_not_partial(self):
        self.assertTrue(self.snapshot["success"])
        self.assertFalse(self.snapshot["partial"])
        self.assertEqual(self.snapshot["failed_components"], [])
        self.assertEqual(self.snapshot["data"]["retrieved_at"], "2026-09-28T12:00:00Z")
        self.assertEqual(self.snapshot["data"]["errors"], [])
        # Ordinary absence of a listed future event is a mapping state, not a
        # provider failure: NOT_FOUND meetings must not make the snapshot partial.
        mapping_statuses = [
            meeting["polymarket"]["mapping_status"]
            for meeting in self.snapshot["data"]["meetings"]
        ]
        self.assertIn("NOT_FOUND", mapping_statuses)
        self.assertEqual(mapping_statuses.count("VALIDATED"), 2)

    def test_current_target_range(self):
        target = self.snapshot["data"]["current_target_range"]
        self.assertEqual(target["lower"], 3.75)
        self.assertEqual(target["upper"], 4.0)
        self.assertEqual(target["latest_observation_date"], "2026-09-28")
        self.assertIn("DFEDTARU", target["source"])

    def test_ten_upcoming_meetings_with_calendar_and_fed_side(self):
        meetings = self.snapshot["data"]["meetings"]
        self.assertEqual(len(meetings), 10)
        october = meetings[0]
        self.assertEqual(october["meeting_date"], "2026-10-28")
        self.assertEqual(october["status"], "UPCOMING")
        self.assertEqual(october["fomc_calendar"]["start_date"], "2026-10-27")
        self.assertEqual(october["fed_side"]["local_status"], "OK")
        self.assertEqual(
            [(row["outcome_bp"], row["probability_pct"]) for row in october["fed_side"]["local_probabilities"]],
            [(0, 30.0), (25, 70.0)],
        )
        self.assertEqual(
            [row["probability_pct"] for row in october["fed_side"]["raw_probabilities"]],
            [30.0, 70.0],
        )
        self.assertEqual(october["fed_side"]["normalization"]["applied"], False)
        self.assertEqual(
            october["fed_side"]["freshness"]["status"], "SOURCE_TIMESTAMP_UNAVAILABLE"
        )
        self.assertIsNone(october["fed_side"]["source_timestamp"])

    def test_polymarket_validation_and_comparison_semantics(self):
        meetings = {entry["meeting_date"]: entry for entry in self.snapshot["data"]["meetings"]}
        october = meetings["2026-10-28"]
        self.assertEqual(october["polymarket"]["mapping_status"], "VALIDATED")
        self.assertEqual(october["polymarket"]["data_status"], "CURRENT")
        self.assertEqual(october["polymarket"]["event_id"], "606422")
        comparison = october["comparison"]
        by_outcome = {row["outcome_bp"]: row for row in comparison}
        self.assertAlmostEqual(by_outcome[0]["probability_diff_pp"], 3.5, places=6)
        self.assertAlmostEqual(by_outcome[25]["probability_diff_pp"], -4.5, places=6)
        self.assertAlmostEqual(by_outcome[-50]["probability_diff_pp"], 0.25, places=6)
        self.assertAlmostEqual(by_outcome[50]["probability_diff_pp"], 0.85, places=6)
        self.assertAlmostEqual(by_outcome[-25]["probability_diff_pp"], 0.45, places=6)
        for row in comparison:
            self.assertAlmostEqual(
                row["probability_diff_pp"],
                round(row["polymarket_probability_pct"] - row["fed_probability_pct"], 6),
                places=9,
            )

    def test_december_normalization_chains_and_comparison_uses_normalized_values(self):
        meetings = {entry["meeting_date"]: entry for entry in self.snapshot["data"]["meetings"]}
        december = meetings["2026-12-09"]
        self.assertEqual(december["fed_side"]["normalization"]["applied"], True)
        local = {row["outcome_bp"]: row["probability_pct"] for row in december["fed_side"]["local_probabilities"]}
        self.assertAlmostEqual(local[0], 20.750751, places=6)
        self.assertAlmostEqual(local[25], 79.249249, places=6)
        comparison = {row["outcome_bp"]: row for row in december["comparison"]}
        self.assertAlmostEqual(comparison[0]["fed_probability_pct"], 20.750751, places=6)

    def test_sources_carry_provider_timing_and_status(self):
        sources = {entry["provider"]: entry for entry in self.snapshot["data"]["sources"]}
        self.assertEqual(set(sources), {"fred", "fomc_calendar", "investing", "polymarket"})
        for entry in sources.values():
            self.assertEqual(entry["status"], "OK")
            self.assertEqual(entry["retrieved_at"], "2026-09-28T12:00:00Z")
        self.assertEqual(sources["investing"]["method"], "LIVE_INVESTING_DERIVED")
        self.assertEqual(sources["polymarket"]["method"], "POLYMARKET_CLOB")
        self.assertEqual(sources["fred"]["latest_observation_date"], "2026-09-28")

    def test_method_notes_preserve_the_binary_vs_tails_limitation(self):
        notes = " ".join(self.snapshot["data"]["method_notes"])
        self.assertIn("not automatically a mispricing", notes)
        self.assertIn("probability_diff_pp = Polymarket - Fed-side", notes)
        self.assertIn("not raw CME/ZQ observations", notes)

    def test_fed_side_raw_and_normalized_are_not_the_same_object(self):
        meetings = {entry["meeting_date"]: entry for entry in self.snapshot["data"]["meetings"]}
        december = meetings["2026-12-09"]["fed_side"]
        raw = [row["probability_pct"] for row in december["raw_probabilities"]]
        normalized = [row["probability_pct"] for row in december["normalized_probabilities"]]
        self.assertEqual(raw, [6.2, 38.3, 55.4])
        self.assertAlmostEqual(sum(normalized), 100.0, places=9)
        self.assertNotEqual(raw, normalized)


class SnapshotFailureTests(unittest.TestCase):
    def test_investing_failure_blames_investing_only(self):
        transport = build_snapshot_transport()
        transport.add_text("fed-rate-monitor", TransportError("HTTP 503", status_code=503))
        snapshot = fedwatch_snapshot.build_snapshot(transport, clock=FixedClock(NOW), sleep=NO_SLEEP)
        self.assertTrue(snapshot["partial"])
        self.assertEqual(snapshot["failed_components"], ["investing"])
        self.assertEqual([error["provider"] for error in snapshot["data"]["errors"]], ["investing"])
        meetings = {entry["meeting_date"]: entry for entry in snapshot["data"]["meetings"]}
        self.assertIsNone(meetings["2026-10-28"]["fed_side"])
        self.assertEqual(meetings["2026-10-28"]["polymarket"]["mapping_status"], "VALIDATED")
        self.assertIsNotNone(snapshot["data"]["current_target_range"])
        sources = {entry["provider"]: entry for entry in snapshot["data"]["sources"]}
        self.assertEqual(sources["investing"]["status"], "ERROR")
        self.assertEqual(sources["fred"]["status"], "OK")
        self.assertEqual(sources["polymarket"]["status"], "OK")

    def test_fred_failure_keeps_raw_fed_side_without_local_conversion(self):
        transport = build_snapshot_transport()
        transport.add_text("DFEDTARU", TransportError("HTTP 500"))
        snapshot = fedwatch_snapshot.build_snapshot(transport, clock=FixedClock(NOW), sleep=NO_SLEEP)
        self.assertEqual(snapshot["failed_components"], ["fred"])
        self.assertIsNone(snapshot["data"]["current_target_range"])
        meetings = {entry["meeting_date"]: entry for entry in snapshot["data"]["meetings"]}
        october = meetings["2026-10-28"]["fed_side"]
        self.assertEqual(october["local_status"], "CURRENT_TARGET_RANGE_UNAVAILABLE")
        self.assertIsNone(october["local_probabilities"])
        self.assertEqual([row["probability_pct"] for row in october["raw_probabilities"]],
                         [30.0, 70.0])
        self.assertEqual(meetings["2026-10-28"]["comparison"], [])
        sources = {entry["provider"]: entry for entry in snapshot["data"]["sources"]}
        self.assertEqual(sources["investing"]["status"], "OK")

    def test_polymarket_discovery_failure_blames_polymarket_only(self):
        transport = build_snapshot_transport()
        transport.add_json("gamma-api.polymarket.com/events", TransportError("HTTP 500"))
        transport.add_json("public-search", TransportError("HTTP 500"))
        snapshot = fedwatch_snapshot.build_snapshot(transport, clock=FixedClock(NOW), sleep=NO_SLEEP)
        self.assertEqual(snapshot["failed_components"], ["polymarket"])
        meetings = {entry["meeting_date"]: entry for entry in snapshot["data"]["meetings"]}
        self.assertIsNone(meetings["2026-10-28"]["polymarket"])
        self.assertEqual(meetings["2026-10-28"]["fed_side"]["local_status"], "OK")
        self.assertIsNotNone(snapshot["data"]["current_target_range"])

    def test_fomc_failure_skips_polymarket_mapping_and_keeps_fed_side(self):
        transport = build_snapshot_transport()
        transport.add_text("fomccalendars", TransportError("HTTP 403", status_code=403))
        missing = Path(__file__).resolve().parent / "no-such-fallback.csv"
        snapshot = fedwatch_snapshot.build_snapshot(
            transport, clock=FixedClock(NOW), fallback_path=missing, sleep=NO_SLEEP
        )
        self.assertEqual(snapshot["failed_components"], ["fomc_calendar"])
        meetings = {entry["meeting_date"]: entry for entry in snapshot["data"]["meetings"]}
        self.assertIsNone(meetings["2026-10-28"]["fomc_calendar"])
        self.assertIsNone(meetings["2026-10-28"]["polymarket"])
        self.assertEqual(meetings["2026-10-28"]["fed_side"]["local_status"], "OK")
        sources = {entry["provider"]: entry for entry in snapshot["data"]["sources"]}
        self.assertEqual(sources["polymarket"]["status"], "SKIPPED")
        self.assertEqual(sources["fomc_calendar"]["status"], "ERROR")

    def test_fomc_fallback_is_visible_not_hidden(self):
        transport = build_snapshot_transport()
        transport.add_text("fomccalendars", TransportError("HTTP 403", status_code=403))
        snapshot = fedwatch_snapshot.build_snapshot(transport, clock=FixedClock(NOW))
        sources = {entry["provider"]: entry for entry in snapshot["data"]["sources"]}
        self.assertEqual(sources["fomc_calendar"]["status"], "FALLBACK_SNAPSHOT")
        self.assertEqual(
            sources["fomc_calendar"]["fallback_snapshot_retrieved_at"], "2026-09-28T00:00:00Z"
        )
        self.assertFalse(snapshot["partial"])


def single_event_transport(event: dict, token_points: dict) -> FakeTransport:
    transport = build_snapshot_transport(events=[event])
    transport.add_json("prices-history", make_clob_history(token_points))
    return transport


def event_token_points(event: dict, age_days: float = 1.0, empty: set | None = None) -> dict:
    empty = empty or set()
    points = {}
    for market in event["markets"]:
        token = json.loads(market["clobTokenIds"])[0]
        if token in empty:
            points[token] = []
            continue
        price = float(json.loads(market["outcomePrices"])[0])
        points[token] = [{"t": epoch(NOW - timedelta(days=age_days)), "p": price}]
    return points


class FomcAuthorityTests(unittest.TestCase):
    """The official FOMC calendar is authoritative for meeting identity."""

    def test_investing_only_date_cannot_produce_a_comparison(self):
        investing_html = make_investing_html(
            [
                ("Oct 28, 2026 02:00PM ET", [(3.75, 4.00, 50.0), (4.00, 4.25, 50.0)]),
                ("Nov 18, 2026 02:00PM ET", [(3.75, 4.00, 40.0), (4.00, 4.25, 60.0)]),
            ]
        )
        # A structure that would validate if it were ever offered as a candidate:
        # 2026-11-19T04:59Z is 2026-11-18 23:59 U.S. Eastern.
        november_event = make_fed_decision_event(
            event_id="777",
            title="Fed Decision in November?",
            end_date="2026-11-19T04:59:00Z",
            month="November",
            year=2026,
        )
        transport = FakeTransport()
        transport.add_text("DFEDTARU", fixture_text(FIXTURE_FRED_UPPER))
        transport.add_text("DFEDTARL", fixture_text(FIXTURE_FRED_LOWER))
        transport.add_text("fomccalendars", fixture_text(FIXTURE_FOMC_CALENDAR))
        transport.add_text("fed-rate-monitor", investing_html)
        transport.add_json(
            "gamma-api.polymarket.com/events",
            lambda url, params: [november_event] if params.get("tag_slug") == "fed-rates" else [],
        )
        transport.add_json(
            "public-search",
            lambda url, params: {"events": [november_event] if params["q"] == "FOMC" else []},
        )
        points = {}
        for market in november_event["markets"]:
            token = json.loads(market["clobTokenIds"])[0]
            price = float(json.loads(market["outcomePrices"])[0])
            points[token] = [{"t": epoch(NOW - timedelta(days=1)), "p": price}]
        transport.add_json("prices-history", make_clob_history(points))

        snapshot = fedwatch_snapshot.build_snapshot(
            transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertTrue(snapshot["partial"])
        self.assertIn("investing", snapshot["failed_components"])
        mismatch = next(
            error
            for error in snapshot["data"]["errors"]
            if error["code"] == "INVESTING_MEETING_DATE_MISMATCH"
        )
        self.assertEqual(mismatch["provider"], "investing")
        self.assertEqual(mismatch["detail"]["meeting_dates"], ["2026-11-18"])

        sources = {entry["provider"]: entry for entry in snapshot["data"]["sources"]}
        self.assertEqual(sources["investing"]["status"], "PARTIAL")

        meetings = {m["meeting_date"]: m for m in snapshot["data"]["meetings"]}
        self.assertNotIn("2026-11-18", meetings)
        self.assertIn("2026-10-28", meetings)
        for meeting in snapshot["data"]["meetings"]:
            self.assertNotEqual(meeting["meeting_date"], "2026-11-18")
            self.assertEqual(meeting["comparison"], [])

        # The non-official date was never offered to Polymarket validation, so
        # its outcome tokens were never fetched.
        self.assertFalse(
            any(
                str(call.get("params", {}).get("market", "")).startswith("777-")
                for call in transport.json_calls
                if call["url"].endswith("prices-history")
            )
        )


class SnapshotPolymarketQualityTests(unittest.TestCase):
    """A validated mapping whose CLOB quality is not CURRENT must propagate."""

    def _snapshot_for(self, event: dict, token_points: dict) -> dict:
        transport = single_event_transport(event, token_points)
        return fedwatch_snapshot.build_snapshot(
            transport, clock=FixedClock(NOW), sleep=NO_SLEEP
        )

    def _assert_snapshot_partial_for_polymarket(self, snapshot: dict, expected_code: str,
                                                expected_status: str) -> dict:
        self.assertTrue(snapshot["partial"])
        self.assertEqual(snapshot["failed_components"], ["polymarket"])
        errors = snapshot["data"]["errors"]
        self.assertEqual({error["provider"] for error in errors}, {"polymarket"})
        self.assertIn(expected_code, {error["code"] for error in errors})
        sources = {entry["provider"]: entry for entry in snapshot["data"]["sources"]}
        self.assertEqual(sources["polymarket"]["status"], "PARTIAL")
        meetings = {m["meeting_date"]: m for m in snapshot["data"]["meetings"]}
        october = meetings["2026-10-28"]
        self.assertEqual(october["polymarket"]["data_status"], expected_status)
        self.assertEqual(october["comparison"], [])
        self.assertEqual(october["fed_side"]["local_status"], "OK")
        return october

    def test_partial_validated_mapping_makes_snapshot_partial_without_comparison(self):
        event = fixture_json(FIXTURE_PM_OCTOBER)
        missing_token = json.loads(event["markets"][2]["clobTokenIds"])[0]
        snapshot = self._snapshot_for(
            event, event_token_points(event, empty={missing_token})
        )
        self._assert_snapshot_partial_for_polymarket(
            snapshot, "POLYMARKET_MARKET_DATA_PARTIAL", "PARTIAL"
        )

    def test_stale_validated_mapping_makes_snapshot_partial_without_comparison(self):
        event = fixture_json(FIXTURE_PM_OCTOBER)
        snapshot = self._snapshot_for(event, event_token_points(event, age_days=10.0))
        self._assert_snapshot_partial_for_polymarket(
            snapshot, "POLYMARKET_MARKET_DATA_STALE", "STALE"
        )

    def test_unavailable_validated_mapping_makes_snapshot_partial_without_comparison(self):
        event = fixture_json(FIXTURE_PM_OCTOBER)
        all_tokens = {
            json.loads(market["clobTokenIds"])[0] for market in event["markets"]
        }
        snapshot = self._snapshot_for(
            event, event_token_points(event, empty=all_tokens)
        )
        self._assert_snapshot_partial_for_polymarket(
            snapshot, "POLYMARKET_MARKET_DATA_UNAVAILABLE", "UNAVAILABLE"
        )


class CommandTests(unittest.TestCase):
    def test_fed_side_command_reports_both_target_range_and_distributions(self):
        payload = fedwatch_snapshot.build_fed_side_command(
            build_snapshot_transport(), clock=FixedClock(NOW)
        )
        self.assertEqual(payload["current_target_range"]["upper"], 4.0)
        self.assertEqual(payload["method"], "LIVE_INVESTING_DERIVED")
        self.assertEqual(payload["quality"]["parsed_meeting_count"], 10)
        self.assertEqual(payload["quality"]["normalization_applied_meeting_count"], 3)
        for meeting in payload["meetings"]:
            total = sum(row["probability_pct"] for row in meeting["normalized_probabilities"])
            self.assertAlmostEqual(total, 100.0, places=9, msg=meeting["meeting_date"])

    def test_fomc_meetings_command_serializes_dates(self):
        payload = fedwatch_snapshot.build_fomc_meetings_command(
            build_snapshot_transport(), clock=FixedClock(NOW)
        )
        self.assertEqual(payload["source_status"], "SCRAPED")
        self.assertEqual(len(payload["meetings"]), 57)
        self.assertEqual(payload["meetings"][0]["start_date"], "2021-01-26")

    def test_polymarket_command_carries_mapping_and_errors(self):
        payload = fedwatch_snapshot.build_polymarket_command(
            build_snapshot_transport(), clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertEqual(payload["errors"], [])
        meetings = {entry["meeting_date"]: entry for entry in payload["meetings"]}
        self.assertEqual(meetings["2026-10-28"]["mapping_status"], "VALIDATED")
        self.assertEqual(meetings["2026-12-09"]["mapping_status"], "VALIDATED")


class CliTests(unittest.TestCase):
    def test_cli_main_fail_closed_commands(self):
        import fedwatch_data

        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            exit_code = fedwatch_data.main(["definitely-not-a-command"])
        self.assertEqual(exit_code, 1)
        payload = json.loads(stdout.getvalue())
        self.assertEqual(payload["error"]["code"], "FEDWATCH_UNKNOWN_COMMAND")

        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            exit_code = fedwatch_data.main([])
        self.assertEqual(exit_code, 1)
        usage = json.loads(stdout.getvalue())
        self.assertIn("commands", usage)

        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            exit_code = fedwatch_data.main(["help"])
        self.assertEqual(exit_code, 0)
        self.assertIn("commands", json.loads(stdout.getvalue()))

    def test_cli_reports_provider_attributed_error_envelope(self):
        import fedwatch_data

        original = fedwatch_snapshot.build_fred_target_command

        def failing_command(*args, **kwargs):
            raise FedwatchError("fred", "FRED_SOURCE_UNAVAILABLE", "fixture failure")

        fedwatch_snapshot.build_fred_target_command = failing_command
        try:
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                exit_code = fedwatch_data.main(["fred_target"])
        finally:
            fedwatch_snapshot.build_fred_target_command = original
        self.assertEqual(exit_code, 1)
        payload = json.loads(stdout.getvalue())
        self.assertEqual(payload["error"]["provider"], "fred")
        self.assertEqual(payload["error"]["code"], "FRED_SOURCE_UNAVAILABLE")

    def test_cli_snapshot_partial_envelope_shape(self):
        import fedwatch_data

        synthetic = {
            "success": True,
            "data": {
                "retrieved_at": "2026-09-28T12:00:00Z",
                "current_target_range": None,
                "meetings": [],
                "sources": [],
                "errors": [{"provider": "investing", "code": "X", "error": "y"}],
                "warnings": [],
                "method_notes": [],
            },
            "partial": True,
            "failed_components": ["investing"],
        }
        original = fedwatch_snapshot.build_snapshot
        fedwatch_snapshot.build_snapshot = lambda: synthetic
        try:
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                exit_code = fedwatch_data.main(["snapshot"])
        finally:
            fedwatch_snapshot.build_snapshot = original
        self.assertEqual(exit_code, 0)
        payload = json.loads(stdout.getvalue())
        self.assertTrue(payload["success"])
        self.assertTrue(payload["partial"])
        self.assertEqual(payload["failed_components"], ["investing"])

    def test_cli_starts_with_a_plain_interpreter(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "help"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertIn("commands", payload)


if __name__ == "__main__":
    unittest.main()
