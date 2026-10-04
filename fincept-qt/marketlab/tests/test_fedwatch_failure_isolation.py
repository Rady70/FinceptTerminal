"""Regression evidence for source-unit isolation; all transports are injected."""
import csv
import tempfile
import unittest
from datetime import date, timedelta
from pathlib import Path

from fedwatch_test_support import (FakeTransport, FixedClock, epoch, make_investing_html,
                                   make_fed_decision_event, make_snapshot_transport, utc)
from fedwatch import acquisition, fomc, fred, history, investing, monthly_csv, polymarket, published_history, snapshot
from fedwatch.store import FedwatchHistoryStore
from fedwatch.transport import TransportError
from test_fedwatch_fomc import synthetic_calendar
from test_fedwatch_polymarket import build_polymarket_transport, token_points_for

NOW = utc(2026, 9, 28, 12)
CLOCK = FixedClock(NOW)
MEETING = "2026-10-28"


class LiveIsolationTests(unittest.TestCase):
    def distributions(self, buckets):
        html = make_investing_html([(f"{label} 02:00PM ET", rows) for label, rows in buckets])
        return investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)

    def test_bad_meeting_preserves_neighbors_without_spanning_missing_meeting(self):
        for bad in ([(3.75, 4, "abc"), (4, 4.25, 100)], [(3.75, 4, 97)], []):
            with self.subTest(bad=bad):
                result = self.distributions([
                    ("Oct 28, 2026", [(3.75, 4, 100)]), ("Dec 09, 2026", bad),
                    ("Jan 27, 2027", [(4, 4.25, 100)]), ("Mar 17, 2027", [(4, 4.25, 100)])])
                self.assertEqual([m["meeting_date"] for m in result["meetings"]],
                                 ["2026-10-28", "2027-01-27", "2027-03-17"])
                sections = investing.with_local_probabilities(result, 4, 3.75)
                self.assertEqual([m["local_status"] for m in sections], ["OK", "PREVIOUS_MEETING_UNAVAILABLE", "OK"])
                self.assertIsNone(sections[1]["local_probabilities"])
                self.assertEqual(sections[2]["local_probabilities"], [{"outcome_bp": 0, "probability_pct": 100}])
                self.assertEqual(result["errors"][0]["detail"]["meeting_date"], "2026-12-09")

    def test_normalizer_itself_isolates_invalid_meeting(self):
        rows = [{"meeting_date": day, "rate_low": 3.75, "rate_high": 4, "probability_pct": p}
                for day, p in [("2026-10-28", 100), ("2026-12-09", 97)]]
        rejected = []
        values, records = investing.normalize_cumulative(rows, rejected=rejected)
        self.assertEqual(len(values), 1)
        self.assertEqual(rows[1]["probability_pct"], 97)
        self.assertEqual(len(records), 1)
        self.assertEqual(rejected[0]["detail"]["meeting_date"], "2026-12-09")

    def test_missing_unused_future_price_does_not_mix_meeting_buckets(self):
        html = make_investing_html([("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100)]),
                                    ("Dec 09, 2026 02:00PM ET", [(4, 4.25, 100)])])
        html = html.replace("Future Price:", "Not Published:", 1)
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertEqual(result["meetings"][0]["raw_probabilities"][0]["rate_low"], 3.75)
        self.assertEqual(result["meetings"][1]["raw_probabilities"][0]["rate_low"], 4)

    def test_bad_sidebar_does_not_contaminate_complete_main_distribution(self):
        result = self.distributions([("Oct 28, 2026", [(3.75, 4, 100)]),
                                     ("Oct 28, 2026", [(3.75, 4, "abc")])])
        self.assertEqual(result["meetings"][0]["raw_probabilities"][0]["probability_pct"], 100)
        self.assertFalse(result["errors"])

    def test_visual_style_and_valid_html_attributes_do_not_determine_probability_identity(self):
        html = make_investing_html([("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100)])])
        html = html.replace('class="infoFed"', "id='meeting' class='extra infoFed'")
        html = html.replace('class="percfedRateItem"', "class='extra percfedRateItem' data-view='rates'")
        html = html.replace('style="width: 100%"', "style='width: 100%;'")
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertEqual(result["meetings"][0]["raw_probabilities"][0]["probability_pct"], 100)
        self.assertFalse(result["errors"])

    def test_fred_rows_are_sorted_finite_and_conflicts_excluded(self):
        report = {}
        values = fred.parse_fred_csv('observation_date,DFEDTARU\n2026-09-28,4\nBROKEN,abc\n'
                                     '2026-09-25,4\n2026-09-26,nan\n2026-09-24,4\n2026-09-24,5\n', report)
        self.assertEqual([r["date"] for r in values], [date(2026, 9, 25), date(2026, 9, 28)])
        self.assertEqual(report["rejected_row_count"], 3)
        self.assertIn("2026-09-24", report["rejected_dates"])

    def test_fred_header_identity_cannot_be_salvaged(self):
        with self.assertRaises(ValueError):
            fred.parse_fred_csv("date,WRONG\n2026-09-28,4\n", series_id="DFEDTARU")

    def test_unclosed_fred_quote_cannot_consume_subsequent_good_rows(self):
        report = {}
        result = fred.parse_fred_csv('date,DFEDTARU\n2026-09-25,4\n2026-09-26,"bad\n2026-09-28,4\n', report)
        self.assertEqual([r["date"] for r in result], [date(2026, 9, 25), date(2026, 9, 28)])
        self.assertEqual(report["rejected_row_count"], 1)

    def test_rejected_latest_fred_and_lagging_bound_retains_labelled_first_step(self):
        for upper_tail in ("2026-09-28,4", "2026-09-28,NaN", "2026-10-01,4"):
            with self.subTest(tail=upper_tail):
                transport = make_snapshot_transport(NOW)
                transport.add_text("DFEDTARU", "DATE,DFEDTARU\n2026-09-25,4\n" + upper_tail)
                transport.add_text("DFEDTARL", "DATE,DFEDTARL\n2026-09-25,3.75\n")
                result = snapshot.build_snapshot(transport, clock=CLOCK, sleep=lambda _: None)
                self.assertEqual(result["data"]["current_target_range"]["status"], "STALE")
                self.assertEqual(result["data"]["current_target_range"]["latest_observation_date"], "2026-09-25")
                for meeting in result["data"]["meetings"]:
                    if meeting.get("fed_side"):
                        self.assertTrue(meeting["fed_side"]["local_probabilities"])
                        self.assertEqual(meeting["fed_side"]["target_range_unverified"],
                                         meeting["fed_side"]["meeting_ordinal"] == 1)

    def test_old_or_unidentifiable_latest_fred_row_does_not_become_current(self):
        for upper, lower in (("2026-09-25,4\nBROKEN,abc", "2026-09-25,3.75"),
                             ("2026-09-24,4", "2026-09-24,3.75")):
            result = fred.fetch_target_range(FakeTransport().add_text("DFEDTARU", "DATE,DFEDTARU\n" + upper)
                                            .add_text("DFEDTARL", "DATE,DFEDTARL\n" + lower), clock=CLOCK)
            self.assertEqual(result["status"], "STALE")
            self.assertEqual(result["target_range"], {"upper": 4, "lower": 3.75})

    def test_historical_bad_fred_row_does_not_erase_current_pair(self):
        transport = make_snapshot_transport(NOW)
        transport.add_text("DFEDTARU", "DATE,DFEDTARU\nBROKEN,abc\n2026-09-28,4\n")
        result = snapshot.build_snapshot(transport, clock=CLOCK, sleep=lambda _: None)
        self.assertTrue(result["partial"])
        self.assertEqual(result["data"]["current_target_range"]["status"], "CURRENT")
        self.assertTrue(result["data"]["meetings"][0]["comparison"])

    def test_rejected_fred_resolution_date_is_not_skipped_to_later_hold(self):
        rows = [{"date": day, "value": value} for day, value in
                [(date(2026, 9, 15), 3.75), (date(2026, 9, 16), 3.75), (date(2026, 9, 18), 3.75)]]
        lower = [dict(row, value=row["value"] - .25) for row in rows]
        result = history.resolve_actual_outcome(date(2026, 9, 16), rows, lower,
                                                rejected_dates=[date(2026, 9, 17)])
        self.assertFalse(result["resolvable"])
        self.assertEqual(result["reason"], "FRED_REJECTED_OBSERVATION_IN_WINDOW")
        self.assertTrue(history.resolve_actual_outcome(date(2026, 9, 16), rows, lower,
                        rejected_dates=[date(2000, 1, 1)])["resolvable"])

    def test_clob_latest_valid_point_survives_all_bad_point_types(self):
        event = make_fed_decision_event()
        points = token_points_for(event, NOW)
        for token, rows in points.items():
            rows[0]["t"] = str(rows[0]["t"])
            rows.extend([None, {"p": .5}, {"t": True, "p": .5}, {"t": 1e18, "p": .5},
                         {"t": epoch(NOW), "p": "NaN"}, {"t": epoch(NOW), "p": 2},
                         {"t": epoch(NOW + timedelta(days=1)), "p": .5}])
        event["markets"][0]["outcomePrices"] = "broken"
        section = polymarket.build_section(build_polymarket_transport([event], points), [date(2026, 10, 28)],
                                           clock=CLOCK, sleep=lambda _: None)
        meeting = section["meetings"][0]
        self.assertEqual(meeting["mapping_status"], "VALIDATED")
        self.assertEqual(meeting["data_status"], "CURRENT")
        self.assertTrue(all(o["probability_pct"] is not None for o in meeting["outcomes"]))
        self.assertTrue(all(o["point_quality"]["rejected"]["future"] == 1 for o in meeting["outcomes"]))

    def test_clob_conflicting_instant_falls_back_to_earlier_valid_point(self):
        points = [{"t": epoch(NOW), "p": .4}, {"t": epoch(NOW), "p": .5},
                  {"t": epoch(NOW - timedelta(days=1)), "p": .3}]
        valid, counts = polymarket.normalize_price_points(points, NOW)
        self.assertEqual([p["probability"] for p in valid], [.3])
        self.assertEqual(counts["conflicting"], 1)
        backfill, _ = history.normalize_backfill_points(points, NOW)
        self.assertEqual(backfill[0]["probability_pct"], 30)

    def test_clob_invalid_sum_retains_outcomes_but_not_current_distribution(self):
        event = make_fed_decision_event()
        points = {token: [{"t": epoch(NOW), "p": .9}] for token in token_points_for(event, NOW)}
        section = polymarket.build_section(build_polymarket_transport([event], points), [date(2026, 10, 28)],
                                           clock=CLOCK, sleep=lambda _: None)
        self.assertEqual(section["meetings"][0]["data_status"], "PARTIAL")
        self.assertTrue(all(o["probability_pct"] == 90 for o in section["meetings"][0]["outcomes"]))

    def test_invalid_clob_document_is_retryable_failure_not_empty_history(self):
        from fedwatch.errors import FedwatchError
        for data in ([], {}, {"history": None}):
            with self.subTest(data=data), self.assertRaises(FedwatchError) as caught:
                polymarket.fetch_price_history(FakeTransport().add_json("prices-history", data), "token", sleep=lambda _: None)
            self.assertEqual(caught.exception.code, "POLYMARKET_MARKET_DATA_INVALID")

    def test_discovery_later_page_failure_preserves_earlier_candidates(self):
        event = make_fed_decision_event()
        for malformed in (TransportError("HTTP 500"), {"wrong": "shape"}):
            with self.subTest(malformed=malformed):
                def tag(url, params):
                    if params["offset"] == 0:
                        return [event]
                    if isinstance(malformed, Exception):
                        raise malformed
                    return malformed
                def search(url, params):
                    if params["page"] == 1:
                        return {"events": [event], "pagination": {"hasMore": True, "totalResults": 2}}
                    if isinstance(malformed, Exception):
                        raise malformed
                    return malformed
                transport = FakeTransport().add_json("/events", tag).add_json("public-search", search)
                events, stats, _ = polymarket.discover_candidate_events(transport, tag_page_size=1, search_page_size=1)
                self.assertEqual([e["id"] for e in events], [event["id"]])
                self.assertFalse(stats["coverage_complete"])
                self.assertTrue(stats["tag"]["page_error"])
                self.assertTrue(stats["searches"]["FOMC"]["page_error"])

    def test_fomc_bad_statement_link_only_rejects_its_row(self):
        html = synthetic_calendar(2026, [("October", "27-28"), ("December", "8-9")])
        html = html.replace("8-9</div>", '8-9</div><a href="monetary20261309a.htm">Statement</a>')
        result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html),
                                     fallback_path=Path("nonexistent-fallback"), clock=CLOCK)
        self.assertEqual([r["end_date"] for r in result["meetings"]], [date(2026, 10, 28)])
        self.assertEqual(result["source_status"], "SCRAPED_PARTIAL")

    def test_calendar_class_quoting_does_not_create_false_partial_coverage(self):
        html = synthetic_calendar(2026, [("October", "27-28")]).replace('"', "'")
        result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html), clock=CLOCK)
        self.assertEqual(result["source_status"], "SCRAPED")
        self.assertTrue(result["coverage_complete"])

    def test_truncated_calendar_keeps_positive_live_identity_without_fallback(self):
        html = synthetic_calendar(2026, [("October", "27-28"), ("December", "8-9")])
        html = html[:html.rindex('<div class="row fomc-meeting">')]
        result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html),
                                     fallback_path=Path("nonexistent-fallback"), clock=CLOCK)
        self.assertEqual(result["meetings"][0]["end_date"], date(2026, 10, 28))
        self.assertFalse(result["coverage_complete"])

    def test_unclosed_fomc_row_does_not_swallow_next_complete_row(self):
        html = synthetic_calendar(2026, [("October", "27-28"), ("December", "8-9")])
        html = html.replace('27-28</div></div>', '27-28</div>', 1)
        result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html),
                                     fallback_path=Path("nonexistent-fallback"), clock=CLOCK)
        self.assertEqual([r["end_date"] for r in result["meetings"]], [date(2026, 12, 9)])
        self.assertFalse(result["coverage_complete"])


class ImportIsolationTests(unittest.TestCase):
    def setUp(self):
        from fedwatch_test_support import frozen_calendar
        frozen_calendar(self)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.store = FedwatchHistoryStore(self.root / "history.db")

    def import_rows(self, rows):
        path = self.root / "published.csv"
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(published_history.COLUMNS)
            writer.writerows(rows)
        return published_history.import_file(self.store, path, MEETING, "https://www.cmegroup.com/fedwatch", clock=CLOCK)

    def test_invalid_day_does_not_erase_other_complete_days_even_if_remaining_total_is_100(self):
        good = [[MEETING, "2026-09-25", 375, 400, 60], [MEETING, "2026-09-25", 400, 425, 40]]
        for bad in ("NaN", -1, "bad"):
            with self.subTest(bad=bad):
                result = self.import_rows(good + [[MEETING, "2026-09-28", 375, 400, 100],
                                                 [MEETING, "2026-09-28", 400, 425, bad]])
                self.assertEqual(result["reporting_dates"], 1)
                self.assertEqual(result["rejected_reporting_dates"], ["2026-09-28"])
                self.assertEqual(self.store.count_observations(), 2)

    def test_bad_revision_does_not_block_independent_new_day(self):
        self.import_rows([[MEETING, "2026-09-25", 375, 400, 60], [MEETING, "2026-09-25", 400, 425, 40]])
        result = self.import_rows([[MEETING, "2026-09-25", 400, 425, 100],
                                  [MEETING, "2026-09-28", 375, 400, 55], [MEETING, "2026-09-28", 400, 425, 45]])
        self.assertEqual(result["counts"], {"inserted": 2})
        self.assertEqual(self.store.count_observations(), 4)

    def test_conflicting_duplicate_day_cannot_be_salvaged_by_first_row(self):
        result = self.import_rows([[MEETING, "2026-09-25", 375, 400, 100],
                                  [MEETING, "2026-09-28", 375, 400, 100], [MEETING, "2026-09-28", 375, 400, 90]])
        self.assertEqual(result["rejected_reporting_dates"], ["2026-09-28"])
        self.assertEqual(result["counts"], {"inserted": 1})

    def test_unclosed_import_quote_cannot_consume_subsequent_valid_day(self):
        path = self.root / "bad-quote.csv"
        path.write_text(",".join(published_history.COLUMNS) + '\n'
            + MEETING + ',2026-09-25,375,400,"bad\n'
            + MEETING + ',2026-09-28,375,400,100\n', encoding="utf-8")
        result = published_history.import_file(self.store, path, MEETING, "https://www.cmegroup.com/fedwatch", clock=CLOCK)
        self.assertEqual(result["reporting_dates"], 1)
        self.assertEqual(result["first_date"], "2026-09-28")
        self.assertEqual(result["rejected_reporting_dates"], ["2026-09-25"])

    def test_monthly_invalid_rows_and_files_keep_clean_contract_days(self):
        for symbol in ("ZQV26", "ZQX26"):
            (self.root / (symbol + ".csv")).write_text(
                f"Symbol: {symbol}\nSource: https://www.investing.com/history?cid=1\n"
                "Date,Price,Open,High,Low,Vol.,Change %\n2026-09-25,96.5,96,96,96,1,0\n"
                '2026-09-26,"bad\nBROKEN,abc,96,96,96,1,0\n2026-09-28,abc,96,96,96,1,0\n', encoding="utf-8")
        (self.root / "ZQZ26.csv").write_text("Symbol: FFc1\n", encoding="utf-8")
        rows, report = monthly_csv.load_contracts(self.root)
        self.assertEqual({r["contract_symbol"] for r in rows}, {"ZQV26", "ZQX26"})
        self.assertEqual(sum(r["close_price"] is not None for r in rows), 2)
        self.assertTrue(all(r["close_price"] is None for r in rows if r["date"] == NOW.date()))
        self.assertEqual(len(report["errors"]), 3)

    def test_scoped_acquisition_keeps_unrelated_meeting_usable_after_bad_meeting(self):
        transport = make_snapshot_transport(NOW)
        transport.add_text("fed-rate-monitor", make_investing_html([
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 30), (4, 4.25, 70)]),
            ("Dec 09, 2026 02:00PM ET", [(3.75, 4, 97)])]))
        acquisition.refresh_current(self.store, transport=transport, clock=CLOCK, sleep=lambda _: None)
        october = acquisition.local_snapshot(self.store, MEETING, clock=CLOCK)
        december = acquisition.local_snapshot(self.store, "2026-12-09", clock=CLOCK)
        self.assertFalse(october["partial"], october["data"]["errors"])
        self.assertTrue(october["data"]["meetings"][0]["comparison"])
        self.assertTrue(december["partial"])
        self.assertIsNone(december["data"]["meetings"][0]["fed_side"])
