"""Workspace read model and the Investing page context it surfaces.

Covers the values the Investing Fed Rate Monitor card already publishes but
the qualified parser previously discarded (Updated time, futures price,
Previous Day / Previous Week table), the target-range archive that no longer
depends on the local-step conversion, and the network-free `workspace` read.
All inputs are captured fixtures or synthetic pages; no network is used.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from datetime import timedelta
from pathlib import Path

from fedwatch_test_support import (
    FIXTURE_INVESTING_LIVE,
    FakeTransport,
    FixedClock,
    fixture_text,
    make_investing_html,
    make_snapshot_transport,
    utc,
)

from fedwatch import history, investing, snapshot, workspace
from fedwatch.store import FedwatchHistoryStore

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "fedwatch_data.py"
CAPTURE = utc(2026, 9, 28, 14)  # after the fixture card's "Updated: Sep 28, 2026 08:55AM EDT"


def new_store(testcase):
    tmp = tempfile.TemporaryDirectory()
    testcase.addCleanup(tmp.cleanup)
    return FedwatchHistoryStore(Path(tmp.name) / "fedwatch" / "fedwatch_history.db")


def collect(store, now):
    envelope = snapshot.build_snapshot(make_snapshot_transport(now), clock=FixedClock(now), sleep=lambda _: None)
    history.record_snapshot(store, envelope["data"], clock=FixedClock(now))
    store.save_current_acquisition(envelope, now.strftime("%Y-%m-%dT%H:%M:%SZ"))
    return envelope


class DisplayedContextTests(unittest.TestCase):
    def setUp(self):
        self.html = fixture_text(FIXTURE_INVESTING_LIVE)

    def test_captured_card_publishes_update_time_futures_price_and_previous_values(self):
        context = investing.parse_displayed_context(self.html)
        october = context["2026-10-28"]
        self.assertEqual(october["source_updated_text"], "Sep 28, 2026 08:55AM EDT")
        self.assertEqual(october["source_updated_at"].isoformat(), "2026-09-28T12:55:00+00:00")
        self.assertIsNotNone(october["futures_price"])
        self.assertTrue(0 < october["futures_price"] <= 100)
        self.assertTrue(october["table"])
        for row in october["table"]:
            self.assertIsNotNone(row["current_pct"])
            self.assertIsNotNone(row["previous_day_pct"])
            self.assertIsNotNone(row["previous_week_pct"])
        # Every meeting card carries its own context.
        rows, _, report = investing.parse_fed_rate_monitor(self.html)
        self.assertEqual(set(context), set(report["reported_meeting_dates"]))

    def test_previous_values_require_the_table_to_repeat_the_accepted_bars(self):
        rows, _, _ = investing.parse_fed_rate_monitor(self.html)
        context = investing.parse_displayed_context(self.html)["2026-10-28"]
        raw = [r for r in rows if r["meeting_date"] == "2026-10-28"]
        accepted = investing._displayed_previous(context, raw)
        self.assertEqual(accepted["status"], "OK")
        self.assertTrue(accepted["previous_week"]["complete"])
        self.assertIsNotNone(accepted["previous_week"]["normalized_expected_rate"])
        # Display rounding is absorbed; a real disagreement withholds the previous values.
        rounded = [dict(r, probability_pct=r["probability_pct"] + 0.1) for r in raw]
        self.assertEqual(investing._displayed_previous(context, rounded)["status"], "OK")
        changed = [dict(r, probability_pct=r["probability_pct"] + 2.0) for r in raw]
        self.assertEqual(investing._displayed_previous(context, changed)["status"], "MISMATCH")
        # A bucket the table does not repeat is not a conflict; it only has no previous value.
        self.assertEqual(investing._displayed_previous(context, raw + [
            {"meeting_date": "2026-10-28", "rate_low": 9.0, "rate_high": 9.25, "probability_pct": 0.0}])["status"],
            "PARTIAL")
        self.assertEqual(investing._displayed_previous({"table": []}, raw)["status"], "UNAVAILABLE")

    def test_extra_table_row_is_accepted_only_as_explicit_zero(self):
        raw = [{"rate_low": 3.75, "rate_high": 4.0, "probability_pct": 100.0}]
        table = [{"rate_low": 3.75, "rate_high": 4.0, "current_pct": 100.0, "previous_day_pct": 90.0,
                  "previous_week_pct": 80.0, "invalid_cells": 0},
                 {"rate_low": 4.0, "rate_high": 4.25, "current_pct": 0.0, "previous_day_pct": 10.0,
                  "previous_week_pct": 20.0, "invalid_cells": 0}]
        result = investing._displayed_previous({"table": table}, raw)
        self.assertEqual(result["status"], "OK")
        self.assertAlmostEqual(result["previous_week"]["normalized_expected_rate"], 3.875 * 0.8 + 4.125 * 0.2)
        table[1]["current_pct"] = 0.4
        self.assertEqual(investing._displayed_previous({"table": table}, raw)["status"], "MISMATCH")

    def test_dash_cells_mean_not_listed_and_unreadable_cells_are_only_missing(self):
        # Shape observed live on 2026-10-05: a far meeting's table lists extra
        # buckets with the page's dash where a horizon has no value.
        raw = [{"rate_low": 3.75, "rate_high": 4.0, "probability_pct": 70.0},
               {"rate_low": 4.0, "rate_high": 4.25, "probability_pct": 30.0}]
        html = ('<div class="infoFed"><div><span>Meeting Time:</span><i>Oct 27, 2027 02:00PM ET</i></div></div>'
                '<table class="genTbl openTbl fedRateTbl"><tbody>'
                '<tr><td class="left">3.50 - 3.75 <span></span></td><td>&mdash;</td><td>&mdash;</td><td>0.0%</td></tr>'
                '<tr><td class="left">3.75 - 4.00</td><td>70.0%</td><td>60.0%</td><td>50.0%</td></tr>'
                '<tr><td class="left">4.00 - 4.25</td><td>30.0%</td><td>40.0%</td><td>50.0%</td></tr>'
                '<tr><td class="left">4.25 - 4.50</td><td>0.0%</td><td>0.0%</td><td>&mdash;</td></tr>'
                '</tbody></table><div class="fedUpdate">Updated: Oct 05, 2026 08:15AM EDT </div>')
        context = investing.parse_displayed_context(html)["2027-10-27"]
        result = investing._displayed_previous(context, raw)
        self.assertEqual(result["status"], "OK")
        self.assertEqual([r["rate_low"] for r in result["previous_week"]["probabilities"]], [3.5, 3.75, 4.0])
        self.assertTrue(result["previous_day"]["complete"])
        self.assertAlmostEqual(result["previous_week"]["normalized_expected_rate"], 3.875 * 0.5 + 4.125 * 0.5)
        garbled = html.replace("<td>60.0%</td>", "<td>n/a</td>")
        context = investing.parse_displayed_context(garbled)["2027-10-27"]
        partial = investing._displayed_previous(context, raw)
        self.assertEqual(partial["status"], "PARTIAL")
        self.assertEqual(partial["unreadable_cells"], 1)
        self.assertNotIn(3.75, [r["rate_low"] for r in partial["previous_day"]["probabilities"]])
        self.assertTrue(partial["previous_week"]["complete"])
        # One unreadable current cell leaves the remaining aligned rows usable.
        garbled = html.replace("<td>70.0%</td>", "<td>n/a</td>")
        context = investing.parse_displayed_context(garbled)["2027-10-27"]
        self.assertEqual(investing._displayed_previous(context, raw)["status"], "PARTIAL")

    def test_freshness_counts_us_weekdays_since_the_published_update(self):
        friday = utc(2026, 10, 2, 11, 35)
        self.assertEqual(investing.source_freshness(friday, utc(2026, 10, 5, 13))["status"], "CURRENT")
        wednesday = utc(2026, 9, 30, 11, 35)
        self.assertEqual(investing.source_freshness(wednesday, utc(2026, 10, 5, 13))["status"], "STALE")
        future = investing.source_freshness(utc(2026, 10, 5, 14), utc(2026, 10, 5, 13))
        self.assertEqual(future["status"], "SOURCE_TIMESTAMP_UNAVAILABLE")
        self.assertEqual(future["reason"], "FUTURE_SOURCE_TIMESTAMP")
        self.assertEqual(investing.source_freshness(None, utc(2026, 10, 5))["status"], "SOURCE_TIMESTAMP_UNAVAILABLE")

    def test_updated_time_parses_both_eastern_abbreviations(self):
        self.assertEqual(investing._parse_updated("Jan 05, 2027 07:35AM EST").isoformat(), "2027-01-05T12:35:00+00:00")
        self.assertEqual(investing._parse_updated("Oct 05, 2026 07:35PM EDT").isoformat(), "2026-10-05T23:35:00+00:00")
        self.assertIsNone(investing._parse_updated("Oct 05, 2026 07:35AM CET"))
        self.assertIsNone(investing._parse_updated("yesterday"))

    def test_fetched_sections_carry_source_time_freshness_and_context(self):
        transport = FakeTransport()
        transport.add_text("fed-rate-monitor", self.html)
        result = investing.fetch_distributions(transport, clock=FixedClock(CAPTURE))
        october = next(m for m in result["meetings"] if m["meeting_date"] == "2026-10-28")
        self.assertEqual(october["source_timestamp"], "2026-09-28T12:55:00Z")
        self.assertEqual(october["freshness"]["status"], "CURRENT")
        self.assertEqual(october["displayed_context"]["previous"]["status"], "OK")
        sections = investing.with_local_probabilities(result, 4.0, 3.75)
        first = next(s for s in sections if s["meeting_date"] == "2026-10-28")
        self.assertEqual(first["source_timestamp"], "2026-09-28T12:55:00Z")
        self.assertIn("Updated", first["timestamp_note"])
        self.assertIs(first["displayed_context"], october["displayed_context"])

    def test_page_without_context_keeps_the_previous_contract(self):
        html = make_investing_html([("Oct 28, 2026 02:00PM ET", [("3.75", "4.00", "60.0"), ("4.00", "4.25", "40.0")])])
        transport = FakeTransport()
        transport.add_text("fed-rate-monitor", html)
        result = investing.fetch_distributions(transport, clock=FixedClock(CAPTURE))
        meeting = result["meetings"][0]
        self.assertIsNone(meeting["source_timestamp"])
        self.assertEqual(meeting["freshness"]["status"], "SOURCE_TIMESTAMP_UNAVAILABLE")
        self.assertEqual(meeting["displayed_context"]["previous"]["status"], "UNAVAILABLE")
        self.assertEqual(meeting["displayed_context"]["futures_price"], 96.0)


class TargetRangeArchiveTests(unittest.TestCase):
    def test_distribution_is_archived_when_the_local_step_is_unavailable(self):
        store = new_store(self)
        html = make_investing_html([("Oct 28, 2026 02:00PM ET", [("3.75", "4.00", "60.0"), ("4.00", "4.25", "40.0")])])
        transport = FakeTransport()
        transport.add_text("fed-rate-monitor", html)
        distributions = investing.fetch_distributions(transport, clock=FixedClock(CAPTURE))
        # No FRED pair: the first local step cannot be derived.
        sections = investing.without_local_probabilities(distributions)
        data = {"retrieved_at": "2026-09-28T14:00:00Z",
                "meetings": [{"meeting_date": "2026-10-28", "status": "UPCOMING", "fed_side": sections[0],
                              "polymarket": None, "comparison": []}]}
        report = history.record_snapshot(store, data, clock=FixedClock(CAPTURE))
        bands = store.observations(meeting_date="2026-10-28", method=history.FED_METHOD_LIVE_TARGET)
        self.assertEqual([(row["outcome_bp"], row["probability_pct"]) for row in bands], [(400, 60.0), (425, 40.0)])
        self.assertEqual(store.observations(meeting_date="2026-10-28", method=history.FED_METHOD_LIVE), [])
        self.assertEqual(report["target_range_counts"], {"inserted": 2})
        self.assertTrue(any(s["reason"] == "FED_SIDE_LOCAL_PROBABILITY_UNAVAILABLE" for s in report["skipped"]))

    def test_rereading_the_same_published_update_adds_no_rows(self):
        store = new_store(self)
        collect(store, CAPTURE)
        before = len(store.observations(method=history.FED_METHOD_LIVE_TARGET))
        self.assertGreater(before, 0)
        collect(store, CAPTURE + timedelta(minutes=20))
        self.assertEqual(len(store.observations(method=history.FED_METHOD_LIVE_TARGET)), before)
        row = store.observations(method=history.FED_METHOD_LIVE_TARGET)[0]
        self.assertEqual(row["observed_at"], "2026-09-28T12:55:00Z")
        self.assertEqual(row["source_observed_at"], "2026-09-28T12:55:00Z")


class WorkspaceTests(unittest.TestCase):
    def test_current_read_has_every_meeting_history_and_no_network(self):
        store = new_store(self)
        collect(store, CAPTURE)
        result = workspace.build(store, clock=FixedClock(CAPTURE + timedelta(hours=1)))
        self.assertEqual(result["network_requests"], 0)
        self.assertEqual(result["next_meeting_date"], "2026-10-28")
        self.assertEqual(result["current_target_range"]["upper"], 4.0)
        october = next(m for m in result["meetings"] if m["meeting_date"] == "2026-10-28")
        self.assertEqual(october["fed"]["state"], "CURRENT")
        self.assertAlmostEqual(sum(b["probability_pct"] for b in october["fed"]["distribution"]), 100.0, places=9)
        self.assertEqual(october["fed"]["previous_status"], "OK")
        self.assertIsNotNone(october["fed"]["expected_rate_previous_week"])
        self.assertIsNotNone(october["fed"]["futures_price"])
        self.assertEqual(october["polymarket"]["state"], "CURRENT")
        self.assertTrue(october["comparison"])
        self.assertTrue(october["history"]["fed_days"])
        self.assertTrue(october["history"]["changes"])
        upcoming = [m for m in result["meetings"] if m["status"] == "UPCOMING"]
        self.assertTrue(all(m["fed"]["distribution"] for m in upcoming))
        self.assertIn("CME", result["cme"]["note"])
        self.assertFalse(result["cme"]["collected"])

    def test_old_values_are_shown_as_stale_never_current(self):
        store = new_store(self)
        collect(store, CAPTURE)
        later = CAPTURE + timedelta(days=5)
        result = workspace.build(store, clock=FixedClock(later))
        october = next(m for m in result["meetings"] if m["meeting_date"] == "2026-10-28")
        self.assertEqual(october["fed"]["state"], "STALE")
        self.assertTrue(october["fed"]["distribution"])
        self.assertGreater(october["fed"]["age_days"], 4)
        self.assertEqual(october["polymarket"]["state"], "STALE")
        self.assertEqual(october["comparison"], [])

    def test_observations_recorded_before_band_rows_still_give_history(self):
        store = new_store(self)
        collect(store, CAPTURE)
        conn = store._connect()
        try:
            conn.execute("DELETE FROM fedwatch_probability_observations WHERE method = ?",
                         (history.FED_METHOD_LIVE_TARGET,))
            conn.commit()
        finally:
            conn.close()
        result = workspace.build(store, clock=FixedClock(CAPTURE + timedelta(days=5)))
        october = next(m for m in result["meetings"] if m["meeting_date"] == "2026-10-28")
        self.assertTrue(october["history"]["fed_days"])
        self.assertTrue(october["fed"]["distribution"])

    def test_empty_store_is_an_explicit_empty_workspace(self):
        store = new_store(self)
        result = workspace.build(store, clock=FixedClock(CAPTURE))
        self.assertEqual(result["meetings"], [])
        self.assertIsNone(result["next_meeting_date"])

    def test_cli_workspace_command_is_local_only(self):
        store = new_store(self)
        collect(store, CAPTURE)
        completed = subprocess.run([sys.executable, str(SCRIPT), "workspace", "--db", str(store.path)],
                                   capture_output=True, text=True, timeout=120, env={"PYTHONIOENCODING": "utf-8",
                                   "SYSTEMROOT": __import__("os").environ.get("SYSTEMROOT", "")})
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        envelope = json.loads(completed.stdout)
        self.assertTrue(envelope["success"])
        self.assertEqual(envelope["data"]["network_requests"], 0)
        self.assertTrue(envelope["data"]["meetings"])


if __name__ == "__main__":
    unittest.main()
