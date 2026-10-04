"""Remaining independent-review findings: downstream retention and labels."""
import tempfile
import unittest
from pathlib import Path

from fedwatch_test_support import (FixedClock, FakeTransport, FIXTURE_FOMC_CALENDAR,
    fixture_text, make_snapshot_transport, make_investing_html, utc)
from fedwatch import fomc, investing, snapshot

NOW = utc(2026, 10, 4, 12)
CLOCK = FixedClock(NOW)


class FollowupTests(unittest.TestCase):
    def test_stale_or_unknown_backup_cannot_hide_live_confirmed_snapshot_mappings(self):
        html = fixture_text(FIXTURE_FOMC_CALENDAR)
        partial = html[:html.index("2027 FOMC Meetings")]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fallback.csv"
            rows = [line for line in fomc.FALLBACK_PATH.read_text().splitlines() if not line.startswith("#")]
            for captured in ("2026-01-01T00:00:00Z", None):
                with self.subTest(captured=captured):
                    path.write_text(("# snapshot_retrieved_at=" + captured + "\n" if captured else "") + "\n".join(rows))
                    transport = make_snapshot_transport(NOW).add_text("fomccalendars", partial)
                    transport.add_text("DFEDTARU", "DATE,DFEDTARU\n2026-10-02,4\n")
                    transport.add_text("DFEDTARL", "DATE,DFEDTARL\n2026-10-02,3.75\n")
                    result = snapshot.build_snapshot(transport, clock=CLOCK, fallback_path=path, sleep=lambda _: None)
                    meetings = {row["meeting_date"]: row for row in result["data"]["meetings"]}
                    for day in ("2026-10-28", "2026-12-09"):
                        self.assertEqual(meetings[day]["polymarket"]["mapping_status"], "VALIDATED")
                        self.assertTrue(meetings[day]["comparison"])
                        self.assertEqual(meetings[day]["fomc_calendar"]["source"], "scrape")
                    self.assertIsNone(meetings["2027-01-27"]["polymarket"])
                    self.assertTrue(result["partial"])
                    self.assertEqual(len([call for call in transport.json_calls if "prices-history" in call["url"]]), 10)
                    standalone = snapshot.build_polymarket_command(transport, clock=CLOCK, fallback_path=path, sleep=lambda _: None)
                    self.assertEqual([row["meeting_date"] for row in standalone["meetings"]], ["2026-10-28", "2026-12-09"])

    def test_standalone_fed_side_carries_forward_under_the_same_complete_calendar(self):
        result = snapshot.build_fed_side_command(make_snapshot_transport(NOW), clock=CLOCK)
        self.assertEqual(sum(bool(row["local_probabilities"]) for row in result["meetings"]), 10)
        self.assertTrue(result["current_target_range"]["carried_forward"])

    def test_single_copy_duplicate_is_not_mislabelled_as_copy_disagreement(self):
        html = make_investing_html([
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100)]),
            ("Dec 09, 2026 02:00PM ET", [(3.75, 4, 100), (3.75, 4, 90)])])
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertNotIn("2026-12-09", result["parse_report"]["conflicting_copy_meeting_dates"])
        self.assertEqual(result["errors"][0]["code"], "INVESTING_PARSE_PARTIAL")

    def test_intact_main_and_disagreeing_sidebar_keep_main_values_and_label(self):
        html = make_investing_html([
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 30), (4, 4.25, 70)]),
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 60), (4, 4.25, 40)])])
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertEqual([row["probability_pct"] for row in result["meetings"][0]["raw_probabilities"]], [30, 70])
        self.assertTrue(result["meetings"][0]["copy_conflict"])
        self.assertEqual(result["errors"][0]["code"], "INVESTING_COPY_CONFLICT")


if __name__ == "__main__":
    unittest.main()
