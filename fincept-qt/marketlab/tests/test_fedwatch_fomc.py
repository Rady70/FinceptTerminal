"""Deterministic tests for the MarketLab FedWatch FOMC calendar path.

The parser is pinned against the captured 2026-09-28 federalreserve.gov page
(57 meetings, October/December 2026 and the 2027 schedule, one notation vote),
against synthetic month-span / notation-vote / projection-material rows, and
against synthetic statement links whose exact decision date overrides the
text parse. The tracked fallback snapshot is exercised for its metadata,
its role as a scoped fallback on scrape failure, and its fail-closed behavior
when it is missing.
"""

from __future__ import annotations

import tempfile
import unittest
from datetime import date
from pathlib import Path

from fedwatch_test_support import (
    FIXTURE_FOMC_CALENDAR,
    FakeTransport,
    FixedClock,
    corrupt_fomc_calendar_december_row,
    fixture_text,
    utc,
)

from fedwatch import fomc
from fedwatch.errors import FedwatchError
from fedwatch.transport import TransportError


def synthetic_calendar(year: int, rows: list[tuple[str, str]]) -> str:
    meetings = "".join(
        f'<div class="row fomc-meeting"><div class="fomc-meeting__month col">'
        f"<strong>{month}</strong></div>"
        f'<div class="fomc-meeting__date col">{days}</div></div>'
        for month, days in rows
    )
    return (
        f'<div class="panel panel-default"><div class="panel-heading">'
        f'<h4><a id="1">{year} FOMC Meetings</a></h4></div>{meetings}</div>'
        "</body></html>"
    )


class FomcParseTests(unittest.TestCase):
    def test_live_calendar_fixture_known_values(self):
        rows, warnings, report = fomc.parse_fomc_calendar(fixture_text(FIXTURE_FOMC_CALENDAR))
        self.assertEqual(warnings, [])
        self.assertEqual(len(rows), 57)
        by_end = {row["end_date"]: row for row in rows}
        october = by_end[date(2026, 10, 28)]
        self.assertEqual(october["start_date"], date(2026, 10, 27))
        self.assertEqual(october["meeting_type"], "regular")
        self.assertFalse(october["has_projection_materials"])
        self.assertEqual(october["source"], "scrape")
        december = by_end[date(2026, 12, 9)]
        self.assertTrue(december["has_projection_materials"])
        self.assertEqual(by_end[date(2027, 1, 27)]["start_date"], date(2027, 1, 26))
        self.assertEqual(
            [row for row in rows if row["meeting_type"] == "notation_vote"],
            [
                {
                    "start_date": date(2025, 8, 22),
                    "end_date": date(2025, 8, 22),
                    "meeting_type": "notation_vote",
                    "has_projection_materials": False,
                    "source": "scrape",
                }
            ],
        )

    def test_upcoming_filter_is_inclusive_and_chronological(self):
        rows, _, _ = fomc.parse_fomc_calendar(fixture_text(FIXTURE_FOMC_CALENDAR))
        upcoming = fomc.upcoming_meetings(rows, date(2026, 9, 28))
        self.assertEqual(len(upcoming), 10)
        self.assertEqual(upcoming[0]["end_date"], date(2026, 10, 28))
        self.assertEqual(upcoming[-1]["end_date"], date(2027, 12, 8))
        on_meeting_day = fomc.upcoming_meetings(rows, date(2026, 10, 28))
        self.assertEqual(on_meeting_day[0]["end_date"], date(2026, 10, 28))

    def test_projection_star_and_notation_vote_synthetic_rows(self):
        rows, warnings, report = fomc.parse_fomc_calendar(
            synthetic_calendar(2026, [("March", "17-18*"), ("August", "22 (notation vote)")])
        )
        self.assertEqual(warnings, [])
        self.assertTrue(report["structurally_complete"])
        self.assertEqual(rows[0]["has_projection_materials"], True)
        self.assertEqual(rows[0]["end_date"], date(2026, 3, 18))
        self.assertEqual(rows[1]["meeting_type"], "notation_vote")
        self.assertEqual(rows[1]["start_date"], rows[1]["end_date"])

    def test_month_spanning_new_year_wraps_end_year(self):
        rows, _, _ = fomc.parse_fomc_calendar(synthetic_calendar(2026, [("December/January", "31-1")]))
        self.assertEqual(rows[0]["start_date"], date(2026, 12, 31))
        self.assertEqual(rows[0]["end_date"], date(2027, 1, 1))

    def test_statement_link_overrides_text_parsed_end_date(self):
        html = (
            '<div class="panel panel-default"><div class="panel-heading">'
            '<h4><a id="1">2026 FOMC Meetings</a></h4></div>'
            '<div class="row fomc-meeting">'
            '<div class="fomc-meeting__month col"><strong>July</strong></div>'
            '<div class="fomc-meeting__date col">28-29*</div>'
            '<div class="col"><a href="/newsevents/pressreleases/monetary20260729a.htm">HTML</a></div>'
            "</div></div></body></html>"
        )
        rows, _, _ = fomc.parse_fomc_calendar(html)
        self.assertEqual(rows[0]["end_date"], date(2026, 7, 29))

        overriding = html.replace("monetary20260729a.htm", "monetary20260730a.htm")
        rows, _, _ = fomc.parse_fomc_calendar(overriding)
        self.assertEqual(rows[0]["end_date"], date(2026, 7, 30))

    def test_empty_or_unstructured_html_raises(self):
        with self.assertRaises(ValueError):
            fomc.parse_fomc_calendar("<html><body>nothing</body></html>")

    def test_html_truncated_between_rows_reports_incomplete(self):
        # A response cut cleanly after a complete row (before the next row
        # marker) still loses a meeting and must not be authoritative: the row
        # counts match, but the document is not closed.
        html = synthetic_calendar(2026, [("March", "17-18"), ("April", "27-28")])
        truncated = html[: html.rindex('<div class="row fomc-meeting">')]
        rows, _, report = fomc.parse_fomc_calendar(truncated)
        self.assertEqual([row["end_date"] for row in rows], [date(2026, 3, 18)])
        self.assertFalse(report["structurally_complete"])
        self.assertFalse(report["document_closed"])
        self.assertEqual(report["row_marker_count"], report["parsed_row_count"])

    def test_truncated_html_drops_incomplete_row_and_reports_incomplete(self):
        html = synthetic_calendar(2026, [("March", "17-18"), ("April", "27-28")])
        cut = html.rindex('<div class="row fomc-meeting">') + len(
            '<div class="row fomc-meeting">'
        )
        truncated = (
            html[:cut] + '<div class="fomc-meeting__month col"><strong>April</strong></div>'
        )
        rows, _, report = fomc.parse_fomc_calendar(truncated)
        self.assertEqual([row["end_date"] for row in rows], [date(2026, 3, 18)])
        self.assertFalse(report["structurally_complete"])
        self.assertTrue(report["open_meeting_at_eof"])

    def test_malformed_live_row_marks_parse_incomplete(self):
        html = corrupt_fomc_calendar_december_row(fixture_text(FIXTURE_FOMC_CALENDAR))
        rows, warnings, report = fomc.parse_fomc_calendar(html)
        self.assertFalse(report["structurally_complete"])
        self.assertEqual(report["skipped_row_count"], 1)
        self.assertNotIn(date(2026, 12, 9), {row["end_date"] for row in rows})
        self.assertEqual(len(rows), 56)
        self.assertTrue(any("row skipped" in warning for warning in warnings))


class FallbackSnapshotTests(unittest.TestCase):
    def test_tracked_fallback_snapshot_loads_with_metadata(self):
        rows, snapshot_retrieved_at = fomc.load_fallback_snapshot()
        self.assertEqual(len(rows), 57)
        self.assertEqual(rows[0]["source"], "fallback_snapshot")
        by_end = {row["end_date"]: row for row in rows}
        self.assertIn(date(2026, 10, 28), by_end)
        self.assertIn(date(2027, 12, 8), by_end)
        self.assertEqual(snapshot_retrieved_at, "2026-09-28T00:00:00Z")

    def test_scrape_failure_falls_back_and_reports_fallback_status(self):
        transport = FakeTransport().add_text(
            "fomccalendars", TransportError("HTTP 403", status_code=403)
        )
        result = fomc.fetch_calendar(transport, clock=FixedClock(utc(2026, 9, 28, 12)))
        self.assertEqual(result["source_status"], "FALLBACK_SNAPSHOT")
        self.assertEqual(result["fallback_snapshot_retrieved_at"], "2026-09-28T00:00:00Z")
        self.assertEqual(len(result["meetings"]), 57)
        self.assertTrue(result["warnings"])

    def test_scrape_parse_failure_falls_back(self):
        transport = FakeTransport().add_text("fomccalendars", "<html>changed</html>")
        result = fomc.fetch_calendar(transport, clock=FixedClock(utc(2026, 9, 28, 12)))
        self.assertEqual(result["source_status"], "FALLBACK_SNAPSHOT")
        self.assertTrue(any("parse failed" in warning for warning in result["warnings"]))

    def test_scrape_success_is_reported_as_scraped(self):
        transport = FakeTransport().add_text(
            "fomccalendars", fixture_text(FIXTURE_FOMC_CALENDAR)
        )
        result = fomc.fetch_calendar(transport, clock=FixedClock(utc(2026, 9, 28, 12)))
        self.assertEqual(result["source_status"], "SCRAPED")
        self.assertIsNone(result["fallback_snapshot_retrieved_at"])
        self.assertEqual(len(result["meetings"]), 57)
        self.assertTrue(result["parse_report"]["structurally_complete"])

    def test_incomplete_live_parse_falls_back_to_tracked_snapshot(self):
        # One malformed official row must not become an authoritative partial
        # scrape: the tracked fallback supplies the complete calendar.
        transport = FakeTransport().add_text(
            "fomccalendars",
            corrupt_fomc_calendar_december_row(fixture_text(FIXTURE_FOMC_CALENDAR)),
        )
        result = fomc.fetch_calendar(transport, clock=FixedClock(utc(2026, 9, 28, 12)))
        self.assertEqual(result["source_status"], "FALLBACK_SNAPSHOT")
        self.assertEqual(result["fallback_snapshot_retrieved_at"], "2026-09-28T00:00:00Z")
        self.assertEqual(len(result["meetings"]), 57)
        self.assertIn(date(2026, 12, 9), {row["end_date"] for row in result["meetings"]})
        self.assertFalse(result["parse_report"]["structurally_complete"])
        self.assertTrue(
            any("structurally incomplete" in warning for warning in result["warnings"])
        )
        self.assertFalse(result["fallback_stale"])

    def test_fresh_fallback_is_not_stale(self):
        transport = FakeTransport().add_text(
            "fomccalendars", TransportError("HTTP 403", status_code=403)
        )
        result = fomc.fetch_calendar(transport, clock=FixedClock(utc(2026, 9, 28, 12)))
        self.assertFalse(result["fallback_stale"])
        self.assertAlmostEqual(result["fallback_age_days"], 0.5, places=6)

    def test_stale_fallback_is_reported_with_age(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fallback.csv"
            path.write_text(
                "# snapshot_retrieved_at=2026-01-01T00:00:00Z\n"
                "start_date,end_date,meeting_type,has_projection_materials\n"
                "2026-10-27,2026-10-28,regular,false\n",
                encoding="utf-8",
            )
            transport = FakeTransport().add_text(
                "fomccalendars", TransportError("HTTP 403", status_code=403)
            )
            result = fomc.fetch_calendar(
                transport, fallback_path=path, clock=FixedClock(utc(2026, 9, 28, 12))
            )
        self.assertEqual(result["source_status"], "FALLBACK_STALE")
        self.assertTrue(result["fallback_stale"])
        self.assertGreater(result["fallback_age_days"], fomc.FALLBACK_MAX_AGE_DAYS)
        self.assertTrue(any("fallback snapshot age" in w for w in result["warnings"]))

    def test_missing_fallback_and_failed_scrape_fails_closed(self):
        transport = FakeTransport().add_text(
            "fomccalendars", TransportError("HTTP 403", status_code=403)
        )
        missing = Path(tempfile.gettempdir()) / "fedwatch-no-such-fallback.csv"
        with self.assertRaises(FedwatchError) as caught:
            fomc.fetch_calendar(transport, fallback_path=missing)
        self.assertEqual(caught.exception.provider, "fomc_calendar")
        self.assertEqual(caught.exception.code, "FOMC_CALENDAR_UNAVAILABLE")

    def test_malformed_fallback_fails_closed(self):
        transport = FakeTransport().add_text(
            "fomccalendars", TransportError("HTTP 403", status_code=403)
        )
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / "fallback.csv"
            bad.write_text("# snapshot_retrieved_at=x\nstart_date,end_date\n2026-01-01,2026-01-02\n",
                           encoding="utf-8")
            with self.assertRaises(FedwatchError) as caught:
                fomc.fetch_calendar(transport, fallback_path=bad)
        self.assertEqual(caught.exception.code, "FOMC_CALENDAR_UNAVAILABLE")

    def test_unreadable_fallback_keeps_fomc_provider_attribution(self):
        transport = FakeTransport().add_text(
            "fomccalendars", TransportError("HTTP 403", status_code=403)
        )
        with tempfile.TemporaryDirectory() as directory:
            unreadable = Path(directory) / "fallback.csv"
            unreadable.mkdir()  # exists, but read_text raises OSError
            with self.assertRaises(FedwatchError) as caught:
                fomc.fetch_calendar(transport, fallback_path=unreadable)
        self.assertEqual(caught.exception.provider, "fomc_calendar")
        self.assertEqual(caught.exception.code, "FOMC_CALENDAR_UNAVAILABLE")


if __name__ == "__main__":
    unittest.main()
