"""Owner-authorized first-only uncertainty and durable calendar-copy regressions."""
import json
import tempfile
import unittest
from contextlib import closing
from pathlib import Path
from unittest.mock import patch

from fedwatch_test_support import (FixedClock, FakeTransport, FIXTURE_FOMC_CALENDAR,
    fixture_text, make_snapshot_transport, utc)
from fedwatch import acquisition, fomc, history, snapshot, timeutil
from fedwatch.store import FedwatchHistoryStore
from fedwatch.transport import TransportError

NOW = utc(2026, 10, 4, 12)
CLOCK = FixedClock(NOW)
HTML = fixture_text(FIXTURE_FOMC_CALENDAR)
PARTIAL = HTML[:HTML.index("2027 FOMC Meetings")]


class FirstOnlyTests(unittest.TestCase):
    def test_snapshot_and_standalone_stale_labels_failure_and_positive_decision(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "backup.csv"
            path.write_text(fomc.FALLBACK_PATH.read_text().replace(
                fomc.FALLBACK_PATH.read_text().splitlines()[0], "# snapshot_retrieved_at=2026-01-01T00:00:00Z"))
            for mode in ("snapshot", "standalone"):
                for scenario in ("stale", "failure", "decision"):
                    with self.subTest(mode=mode, scenario=scenario), patch.object(fomc, "FALLBACK_PATH", path):
                        transport = make_snapshot_transport(NOW).add_text("fomccalendars", PARTIAL)
                        if scenario == "failure":
                            transport.add_text("DFEDTARU", TransportError("injected FRED failure"))
                        if scenario == "decision":
                            rows, _ = fomc.load_fallback_snapshot(path)
                            rows.append(dict(start_date=utc(2026, 9, 30).date(), end_date=utc(2026, 9, 30).date(),
                                             meeting_type="regular", has_projection_materials=False, source="fallback_snapshot"))
                            path.write_text(fomc.snapshot_csv(rows, "2026-01-01T00:00:00Z"))
                        result = (snapshot.build_snapshot(transport, clock=CLOCK, fallback_path=path, sleep=lambda _: None)["data"]
                                  if mode == "snapshot" else snapshot.build_fed_side_command(transport, clock=CLOCK))
                        fed = [(row["fed_side"] if mode == "snapshot" else row) for row in result["meetings"]]
                        self.assertEqual(sum(bool(row["local_probabilities"]) for row in fed), 10 if scenario == "stale" else 9)
                        self.assertEqual(fed[1]["local_status"], "OK")
                        self.assertFalse(fed[1]["target_range_unverified"])
                        if scenario == "stale":
                            self.assertTrue(fed[0]["target_range_unverified"])
                            self.assertEqual(fed[0]["target_range_pair_date"], "2026-09-28")
                            self.assertEqual(fed[0]["local_status"], "OK")
                            if mode == "snapshot":
                                self.assertTrue(result["meetings"][0]["comparison"])
                                with closing(FedwatchHistoryStore(Path(tmp) / "history.db")) as store:
                                    history.record_snapshot(store, result, clock=CLOCK)
                                    rows = store.observations(meeting_date="2026-10-28")
                                    retained = [row for row in rows if row["method"] == "LIVE_INVESTING_DERIVED"]
                                    self.assertTrue(retained)
                                    # The archived target-range distribution needs no FRED pair.
                                    bands = [row for row in rows if row["method"] == "LIVE_INVESTING_TARGET_RANGE"]
                                    self.assertTrue(bands)
                                    self.assertNotIn("target_range_unverified", bands[0]["detail"])
                                    self.assertTrue(all(row["detail"]["target_range_unverified"] for row in retained))
                                    self.assertTrue(all(row["detail"]["target_range_pair_date"] == "2026-09-28" for row in retained))
                        elif scenario == "decision":
                            self.assertEqual(fed[0]["local_status"], "FIRST_MEETING_AFTER_DECISION")
                        # Remove the synthetic decision before the next mode.
                        if scenario == "decision":
                            path.write_text(fomc.snapshot_csv([row for row in rows if row["end_date"].isoformat() != "2026-09-30"], "2026-01-01T00:00:00Z"))

    def test_decision_bounds_include_pair_day_exclude_today_and_future(self):
        transport = make_snapshot_transport(NOW)
        from fedwatch import fred, investing
        target = fred.fetch_target_range(transport, clock=CLOCK)
        target["status"] = "STALE"
        distributions = investing.fetch_distributions(transport, clock=CLOCK)
        for day, withheld in (("2026-09-27", False), ("2026-09-28", True),
                              ("2026-10-03", True), ("2026-10-04", False), ("2026-10-05", False)):
            rows = [{"start_date": timeutil.parse_date(day), "end_date": timeutil.parse_date(day),
                     "meeting_type": "regular", "has_projection_materials": False, "source": "fallback_snapshot", "stale": True}]
            sections = snapshot.local_sections(distributions, target, {"meetings": rows}, NOW.date())
            with self.subTest(day=day):
                self.assertEqual(sections[0]["local_probabilities"] is None, withheld)
                self.assertTrue(all(row["local_status"] == "OK" for row in sections[1:]))


class CalendarCopyTests(unittest.TestCase):
    def test_collect_writes_complete_only_and_readonly_never_writes(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict("os.environ", {"FINCEPT_DATA_DIR": tmp}):
            with closing(FedwatchHistoryStore(Path(tmp) / "fedwatch" / "history.db")) as store:
                path = fomc.profile_calendar_path(store.path)
                data = snapshot.build_snapshot(make_snapshot_transport(NOW), clock=CLOCK, sleep=lambda _: None)["data"]
                self.assertFalse(path.exists())
                collected = history.collect(store, data, transport=FakeTransport(), clock=CLOCK)
                self.assertEqual(collected["calendar_copy"]["status"], "WRITTEN")
                before = path.read_bytes()
                partial = snapshot.build_snapshot(make_snapshot_transport(NOW).add_text("fomccalendars", PARTIAL),
                    clock=CLOCK, profile_calendar_path=path, sleep=lambda _: None)["data"]
                self.assertTrue(partial["calendar_snapshot"]["coverage_complete"])
                collected = history.collect(store, partial, transport=FakeTransport(), clock=CLOCK)
                self.assertEqual(collected["calendar_copy"]["reason"], "NO_COMPLETE_LIVE_CAPTURE")
                self.assertEqual(path.read_bytes(), before)
                for builder in (snapshot.build_fed_side_command, snapshot.build_fomc_meetings_command,
                                snapshot.build_polymarket_command):
                    builder(make_snapshot_transport(NOW), clock=CLOCK)
                self.assertEqual(path.read_bytes(), before)

    def test_newest_readable_copy_wins_bad_copy_warns_and_no_timestamp_is_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundled, profile = Path(tmp) / "bundled.csv", Path(tmp) / "profile.csv"
            rows, _ = fomc.load_fallback_snapshot()
            for newest in ("profile", "bundle"):
                bundled.write_text(fomc.snapshot_csv(rows, "2026-10-03T00:00:00Z" if newest == "bundle" else "2026-09-28T00:00:00Z"))
                profile.write_text(fomc.snapshot_csv(rows, "2026-10-03T00:00:00Z" if newest == "profile" else "2026-09-28T00:00:00Z"))
                result = fomc.fetch_calendar(FakeTransport(), fallback_path=bundled, profile_path=profile, clock=CLOCK)
                self.assertEqual(result["fallback_origin"], "PROFILE_COPY" if newest == "profile" else "BUNDLED_FILE")
            for bad in ("not,csv\nbroken,row", fomc.snapshot_csv(rows, "2026-10-03T00:00:00Z").split("\n", 1)[1],
                        fomc.snapshot_csv(rows, "2027-10-03T00:00:00Z")):
                profile.write_text(bad)
                result = fomc.fetch_calendar(FakeTransport(), fallback_path=bundled, profile_path=profile, clock=CLOCK)
                self.assertEqual(result["fallback_origin"], "BUNDLED_FILE")
                self.assertTrue(any("Unreadable profile calendar copy ignored" in warning for warning in result["warnings"]))

    def test_explicit_db_refresh_reads_and_writes_beside_its_own_db(self):
        with tempfile.TemporaryDirectory() as tmp:
            with closing(FedwatchHistoryStore(Path(tmp) / "explicit" / "history.db")) as store:
                result = acquisition.refresh_current(store, force=True, transport=make_snapshot_transport(NOW),
                                                     clock=CLOCK, sleep=lambda _: None)
                path = fomc.profile_calendar_path(store.path)
                self.assertTrue(path.exists())
                self.assertEqual(result["data"]["history"]["calendar_copy"]["status"], "WRITTEN")
                result = acquisition.refresh_current(store, force=True,
                    transport=make_snapshot_transport(NOW).add_text("fomccalendars", TransportError("offline")),
                    clock=CLOCK, sleep=lambda _: None)
                self.assertEqual(result["data"]["calendar_snapshot"]["source_status"], "FALLBACK_SNAPSHOT")


if __name__ == "__main__":
    unittest.main()
