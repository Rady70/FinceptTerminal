"""Bounded acquisition tests. All providers are injected; no internet calls."""
import contextlib
import csv
import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from datetime import timedelta
from pathlib import Path
from unittest.mock import patch

from fedwatch_test_support import FIXTURE_PM_DECEMBER, FixedClock, FakeTransport, fixture_json, make_snapshot_transport, utc
from fedwatch import acquisition, analytics, history, monthly_csv, published_history, sources
from fedwatch.errors import FedwatchError
from fedwatch.store import FedwatchHistoryStore, HISTORY_SCHEMA_VERSION

NOW = utc(2026, 9, 28, 12)
MEETING = "2026-10-28"
CME_URL = "https://www.cmegroup.com/fedwatch"
CLI = Path(__file__).resolve().parents[2] / "scripts" / "fedwatch_data.py"


class AcquisitionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.store = FedwatchHistoryStore(self.root / "history.db")
        self.clock = FixedClock(NOW)
        self.transport = make_snapshot_transport(NOW)

    def refresh(self, **kwargs):
        return acquisition.refresh_current(self.store, MEETING, transport=self.transport,
                                           clock=self.clock, sleep=lambda _: None, **kwargs)

    def cme_file(self, rows=None):
        path = self.root / "history.csv"
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(published_history.COLUMNS)
            writer.writerows(rows if rows is not None else [
                [MEETING, "2026-09-25", 375, 400, 60], [MEETING, "2026-09-25", 400, 425, 40],
                [MEETING, "2026-09-28", 375, 400, 55], [MEETING, "2026-09-28", 400, 425, 45]])
        return path

    def import_cme(self, path):
        return published_history.import_file(self.store, path, MEETING, CME_URL, clock=self.clock)

    def monthly_files(self, missing_last=False):
        directory = self.root / "contracts"
        directory.mkdir(exist_ok=True)
        for code, price in (("U", 96.5), ("V", 96.4), ("X", 96.35)):
            with (directory / f"ZQ{code}26.csv").open("w", encoding="utf-8", newline="") as handle:
                handle.write(f"Symbol: ZQ{code}26\nSource: https://www.investing.com/rates-bonds/cbot-30-day-federal-funds-comp-c1-futures-historical-data?cid={ord(code)}\n")
                writer = csv.writer(handle)
                writer.writerow(["Date", "Price", "Open", "High", "Low", "Vol.", "Change %"])
                writer.writerow(["Sep 25, 2026", price, price, price, price, "1K", "0%"])
                writer.writerow(["Sep 28, 2026", "-" if missing_last and code == "X" else price, price, price, price, "1K", "0%"])
        return directory

    def import_monthly(self, directory, **kwargs):
        return history.import_zq(self.store, self.transport, directory, [NOW.date()],
                                 clock=self.clock, input_format="investing", meeting_date=MEETING, **kwargs)

    def test_open_and_navigation_never_acquire(self):
        with patch("fedwatch.transport.HttpTransport._request", side_effect=AssertionError("network")):
            self.assertEqual(acquisition.local_snapshot(self.store, clock=self.clock)["data"]["acquisition"]["network_requests"], 0)
            self.import_cme(self.cme_file())
            for meeting in (MEETING, "2026-12-09", MEETING):
                for method, outcome in ((history.FED_METHOD_LIVE, 0), (history.FED_METHOD_ZQ, 25), (history.FED_METHOD_LIVE, -25)):
                    history.series(self.store, meeting, method=method, outcome_bp=outcome)
                    analytics.compute_analytics(self.store, meeting, outcome, fed_method=method, as_of=NOW)
                sources.describe(self.store, meeting)
                published_history.compare_reconstruction(self.store, meeting)
                acquisition.local_snapshot(self.store, meeting, self.clock)
        self.assertEqual(self.transport.text_calls + self.transport.json_calls, [])

    def test_manual_refresh_persists_and_recent_repeat_survives_restart(self):
        result = self.refresh()
        self.assertEqual(result["data"]["acquisition"]["mode"], "MANUAL_REFRESH")
        self.assertFalse(result["partial"], result["data"]["errors"])
        self.assertGreater(len(self.transport.text_calls + self.transport.json_calls), 0)
        count = self.store.count_observations()
        new_store = FedwatchHistoryStore(self.store.path)
        untouched = FakeTransport()
        reused = acquisition.refresh_current(new_store, MEETING, transport=untouched, clock=self.clock)
        self.assertEqual(reused["data"]["acquisition"]["reason"], "RECENT_VALID_ACQUISITION")
        self.assertEqual(untouched.text_calls + untouched.json_calls, [])
        self.assertEqual(self.store.count_observations(), count)

    def test_expired_saved_current_is_gated_without_fetch(self):
        self.refresh()
        later = FixedClock(NOW + timedelta(days=4))
        saved = acquisition.local_snapshot(self.store, MEETING, later)["data"]
        self.assertIsNone(saved["meetings"][0]["fed_side"])
        self.assertEqual(saved["meetings"][0]["polymarket"]["data_status"], "STALE")
        self.assertEqual(saved["meetings"][0]["comparison"], [])
        self.assertIsNone(saved["current_target_range"])

    def test_quote_freshness_expires_before_reuse_window(self):
        self.refresh()
        envelope = self.store.current_acquisition()["envelope"]
        poly = envelope["data"]["meetings"][0]["polymarket"]
        poly["freshness"]["oldest_outcome_timestamp"] = "2026-09-25T12:01:00Z"
        self.store.save_current_acquisition(envelope, "2026-09-28T12:00:00Z")
        self.clock.value += timedelta(minutes=2)
        calls = len(self.transport.json_calls)
        self.refresh()
        self.assertGreater(len(self.transport.json_calls), calls)

    def test_resolved_and_pending_meeting_refresh_never_requests_current(self):
        self.store.upsert_meeting(MEETING, now=NOW)
        self.store.mark_resolved(MEETING, 0, "fixture", now=NOW)
        self.assertEqual(self.refresh(force=True)["data"]["acquisition"]["reason"], "HISTORICAL_MEETING")
        acquisition.refresh_current(self.store, "2026-09-16", transport=self.transport, clock=self.clock, force=True)
        self.assertEqual(self.transport.text_calls + self.transport.json_calls, [])

    def test_failure_preserves_accepted_history_and_does_not_replay_current_success(self):
        self.refresh()
        before = self.store.count_observations()
        failed = acquisition.refresh_current(self.store, MEETING, force=True, transport=FakeTransport(),
                                              clock=self.clock, sleep=lambda _: None)
        self.assertTrue(failed["partial"])
        self.assertEqual(self.store.count_observations(), before)
        saved = acquisition.local_snapshot(self.store, MEETING, self.clock)
        self.assertTrue(saved["partial"])
        self.assertGreater(len(history.series(self.store, MEETING)["observations"]), 0)

    def test_selected_refresh_only_quotes_selected_meeting(self):
        result = self.refresh()
        self.assertEqual([m["meeting_date"] for m in result["data"]["meetings"]], [MEETING])
        december_tokens = {json.loads(m["clobTokenIds"])[0] for m in fixture_json(FIXTURE_PM_DECEMBER)["markets"]}
        self.assertTrue(december_tokens)
        self.assertFalse(any(c["params"].get("market") in december_tokens for c in self.transport.json_calls))

    def test_cme_import_incremental_duplicate_revision_and_restart(self):
        path = self.cme_file()
        first = self.import_cme(path)
        self.assertEqual(first["counts"], {"inserted": 4})
        second = self.import_cme(path)
        self.assertEqual(second["counts"], {"duplicate": 4})
        path = self.cme_file([[MEETING, "2026-09-28", 375, 400, 57], [MEETING, "2026-09-28", 400, 425, 43]])
        self.assertEqual(self.import_cme(path)["counts"], {"revised": 2})
        restarted = FedwatchHistoryStore(self.store.path)
        rows = restarted.observations(method=published_history.METHOD)
        self.assertEqual(len(rows), 4)
        self.assertTrue(all(r["source"] == "cme_published" and r["detail"]["financial_object"] == published_history.OBJECT for r in rows))
        self.assertTrue(all(r["quality_status"] == "PUBLISHED_USER_IMPORT" for r in rows))

    def test_invalid_or_partial_published_input_never_zero_fills_or_destroys_history(self):
        self.import_cme(self.cme_file())
        before = self.store.count_observations()
        for rows in ([], [[MEETING, "2026-09-28", 375, 400, 55]],
                     [["2026-12-09", "2026-09-28", 375, 400, 100]],
                     [[MEETING, "2026-09-28", 375, 400, "NaN"]],
                     [[MEETING, "2026-10-01", 375, 400, 100]],
                     [[MEETING, "2026-09-28", 375, 400, 100]]):
            with self.subTest(rows=rows), self.assertRaises(FedwatchError):
                self.import_cme(self.cme_file(rows))
        self.assertEqual(self.store.count_observations(), before)

    def test_published_rounding_retains_raw_and_normalized_values(self):
        self.import_cme(self.cme_file([[MEETING, "2026-09-28", 375, 400, 66.6], [MEETING, "2026-09-28", 400, 425, 33.3]]))
        rows = self.store.observations(method=published_history.METHOD)
        self.assertAlmostEqual(sum(r["probability_pct"] for r in rows), 100)
        self.assertAlmostEqual(sum(r["raw_probability_pct"] for r in rows), 99.9)

    def test_monthly_contract_reconstruction_keeps_method_and_reuses_retained_input(self):
        directory = self.monthly_files()
        result = self.import_monthly(directory)
        self.assertFalse(result["errors"], result)
        rows = self.store.observations(method=history.FED_METHOD_ZQ)
        self.assertEqual(len(rows), 2)
        self.assertTrue(all(r["meeting_date"] == MEETING and r["quality_status"] == "RECONSTRUCTED" for r in rows))
        self.assertTrue(all(r["detail"]["cumulative_distribution"] for r in rows))
        calls = len(self.transport.text_calls + self.transport.json_calls)
        self.assertEqual(self.import_monthly(directory)["status"], "REUSED_LOCAL")
        self.assertEqual(len(self.transport.text_calls + self.transport.json_calls), calls)
        self.assertEqual(self.store.count_observations(), 2)
        with (directory / "ZQV26.csv").open("a", encoding="utf-8") as f:
            f.write("2026-09-27,96.35,96,96,96,1K,0%\n")
        self.import_monthly(directory)
        self.assertGreater(len(self.transport.text_calls + self.transport.json_calls), calls)

    def test_monthly_missing_close_stays_missing_and_insufficient_buffer_fails_closed(self):
        directory = self.monthly_files(missing_last=True)
        rows, report = monthly_csv.load_contracts(directory)
        self.assertIsNone(next(r for r in rows if r["contract_symbol"] == "ZQX26" and r["date"] == NOW.date())["close_price"])
        self.assertTrue(all(r["open_interest"] is None for r in rows))
        result = self.import_monthly(directory)
        self.assertEqual(self.store.count_observations(), 0)
        self.assertTrue(result["errors"])
        self.assertTrue(result["watch_dates"][0]["deconvolution"]["skipped_meetings"])

    def test_generic_or_wrong_month_csv_rejected(self):
        directory = self.monthly_files()
        path = directory / "ZQV26.csv"
        path.write_text(path.read_text().replace("Symbol: ZQV26", "Symbol: FFc1"))
        with self.assertRaises(FedwatchError):
            monthly_csv.load_contracts(directory)

    def test_direct_and_reconstructed_objects_are_distinct_and_comparison_is_local(self):
        self.import_monthly(self.monthly_files())
        self.import_cme(self.cme_file())
        result = published_history.compare_reconstruction(self.store, MEETING)
        self.assertEqual(result["status"], "OVERLAP")
        self.assertTrue(any(r["difference_pp"] is not None for r in result["comparisons"]))
        self.assertEqual({r["method"] for r in self.store.observations()}, {history.FED_METHOD_ZQ, published_history.METHOD})
        self.assertNotIn(published_history.METHOD, analytics.FED_METHODS)
        self.assertEqual(history.series(self.store, MEETING, method=published_history.METHOD)["financial_object"], published_history.OBJECT)

    def test_polymarket_explicit_bounded_backfill_then_local_reuse(self):
        self.refresh()
        before = len(self.transport.json_calls)
        result = history.backfill_polymarket(self.store, self.transport, clock=self.clock,
                                             meeting_dates=[MEETING], sleep=lambda _: None)
        mappings = len(self.store.validated_mappings([MEETING]))
        self.assertEqual(len(self.transport.json_calls) - before, mappings)
        self.assertFalse(result["errors"])
        calls = len(self.transport.json_calls)
        history.backfill_polymarket(FedwatchHistoryStore(self.store.path), self.transport, clock=self.clock, meeting_dates=[MEETING])
        self.assertEqual(len(self.transport.json_calls), calls)
        self.assertTrue(all(c["params"]["fidelity"] == 1440 for c in self.transport.json_calls if "prices-history" in c["url"]))

    def test_schema_v2_upgrade_preserves_history(self):
        self.import_cme(self.cme_file())
        with contextlib.closing(sqlite3.connect(self.store.path)) as conn:
            conn.execute("DROP TABLE fedwatch_current_acquisition")
            conn.execute("PRAGMA user_version=2")
            conn.commit()
        reopened = FedwatchHistoryStore(self.store.path)
        self.assertEqual(reopened.count_observations(), 4)
        self.assertIsNone(reopened.current_acquisition())
        with contextlib.closing(sqlite3.connect(self.store.path)) as conn:
            self.assertEqual(conn.execute("PRAGMA user_version").fetchone()[0], HISTORY_SCHEMA_VERSION)

    def test_local_cli_survives_fresh_process(self):
        self.import_cme(self.cme_file())
        for command in ("local_snapshot", "history_sources", "history_compare_cme", "history_series"):
            result = subprocess.run([sys.executable, str(CLI), command, "--db", str(self.store.path), "--meeting", MEETING], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(json.loads(result.stdout)["success"])


if __name__ == "__main__":
    unittest.main()
