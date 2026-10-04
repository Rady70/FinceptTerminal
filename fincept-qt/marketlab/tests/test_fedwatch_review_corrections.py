"""Adversarial regressions from the independent failure-isolation review."""
import csv
import json
import tempfile
import unittest
from datetime import date
from pathlib import Path

from fedwatch_test_support import (FIXTURE_FOMC_CALENDAR, FIXTURE_INVESTING_LIVE,
    FakeTransport, FixedClock, fixture_text, make_investing_html, make_snapshot_transport,
    make_fed_decision_event, utc)
from fedwatch import fomc, fred, history, investing, monthly_csv, polymarket, published_history, snapshot, zq
from fedwatch.errors import FedwatchError
from fedwatch.store import FedwatchHistoryStore
from test_fedwatch_fomc import synthetic_calendar

NOW = utc(2026, 10, 4, 12)
CLOCK = FixedClock(NOW)


class CalendarReviewTests(unittest.TestCase):
    def test_partial_live_uses_fresh_backup_to_preserve_all_upcoming_dates(self):
        html = fixture_text(FIXTURE_FOMC_CALENDAR)
        html = html[:html.index("2027 FOMC Meetings")]
        result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html), clock=CLOCK)
        self.assertEqual(len(fomc.upcoming_meetings(result["meetings"], NOW.date())), 10)
        self.assertTrue(result["coverage_complete"])
        self.assertEqual(result["source_status"], "SCRAPED_WITH_FALLBACK")
        self.assertEqual(result["fallback_snapshot_retrieved_at"], "2026-09-28T00:00:00Z")
        self.assertFalse(result["parse_report"]["structurally_complete"])

    def test_live_date_revision_wins_without_preserving_the_old_meeting(self):
        html = synthetic_calendar(2026, [("October", "27-29")])
        result = snapshot.build_fomc_meetings_command(
            FakeTransport().add_text("fomccalendars", html.removesuffix("</html>")), clock=CLOCK)
        by_date = {row["end_date"]: row for row in result["meetings"]}
        self.assertNotIn("2026-10-28", by_date)
        self.assertEqual(by_date["2026-10-29"]["source"], "scrape")
        self.assertEqual(by_date["2026-12-09"]["source"], "fallback_snapshot")
        self.assertEqual(result["merge_conflicts"][0]["resolution"], "LIVE_ROW_WINS")
        self.assertEqual(result["merge_conflicts"][0]["fallback"]["end_date"], "2026-10-28")

    def test_conflicting_live_identity_is_not_resurrected_by_backup(self):
        html = synthetic_calendar(2026, [("October", "27-28"), ("October", "26-28"), ("December", "8-9")])
        result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html), clock=CLOCK)
        self.assertFalse(result["coverage_complete"])
        self.assertNotIn(date(2026, 10, 28), {row["end_date"] for row in result["meetings"]})

    def test_partial_live_with_stale_or_malformed_backup_keeps_positive_rows(self):
        html = synthetic_calendar(2026, [("October", "27-28")]).removesuffix("</html>")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "backup.csv"
            for captured in ("2026-01-01T00:00:00Z", "2026-12-01T00:00:00Z", "BAD"):
                with self.subTest(captured=captured):
                    path.write_text(f"# snapshot_retrieved_at={captured}\n"
                        "start_date,end_date,meeting_type,has_projection_materials\n"
                        "2026-12-08,2026-12-09,regular,false\n", encoding="utf-8")
                    result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html),
                                                 fallback_path=path, clock=CLOCK)
                    self.assertEqual(result["source_status"], "SCRAPED_PARTIAL")
                    self.assertFalse(result["fallback_used"])
                    self.assertFalse(result["coverage_complete"])
                    self.assertFalse(result["fallback_stale"])
                    self.assertEqual([r["end_date"] for r in result["meetings"]], [date(2026, 10, 28)])
            path.write_text("invalid calendar", encoding="utf-8")
            result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html), fallback_path=path, clock=CLOCK)
            self.assertEqual(result["source_status"], "SCRAPED_PARTIAL")
            path.write_bytes(b"\xff\xfeinvalid utf-8")
            result = fomc.fetch_calendar(FakeTransport().add_text("fomccalendars", html), fallback_path=path, clock=CLOCK)
            self.assertEqual(result["source_status"], "SCRAPED_PARTIAL")
            self.assertEqual([r["end_date"] for r in result["meetings"]], [date(2026, 10, 28)])

    def test_recovered_calendar_preserves_snapshot_mapping_and_comparison(self):
        transport = make_snapshot_transport(NOW)
        html = fixture_text(FIXTURE_FOMC_CALENDAR)
        transport.add_text("fomccalendars", html[:html.index("2027 FOMC Meetings")])
        transport.add_text("DFEDTARU", "DATE,DFEDTARU\n2026-10-02,4\n")
        transport.add_text("DFEDTARL", "DATE,DFEDTARL\n2026-10-02,3.75\n")
        result = snapshot.build_snapshot(transport, clock=CLOCK, sleep=lambda _: None)
        meetings = {m["meeting_date"]: m for m in result["data"]["meetings"]}
        self.assertEqual(len(meetings), 10)
        self.assertEqual(meetings["2027-01-27"]["fomc_calendar"]["source"], "fallback_snapshot")
        for day in ("2026-10-28", "2026-12-09"):
            self.assertEqual(meetings[day]["polymarket"]["mapping_status"], "VALIDATED")
            self.assertTrue(meetings[day]["comparison"])


class InvestingReviewTests(unittest.TestCase):
    def test_missing_bucket_cannot_be_hidden_by_second_match_in_last_fragment(self):
        html = make_investing_html([
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100)]),
            ("Dec 09, 2026 02:00PM ET", [(3.5, 3.75, "abc"), (3.75, 4, 70)])])
        html += '<aside><span>4.0 - 4.25</span><div></div><span>30%</span></aside>'
        rows, _, report = investing.parse_fed_rate_monitor(html)
        self.assertFalse(report["structurally_complete"])
        self.assertIn("2026-12-09", report["partial_meeting_dates"])
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertEqual([m["meeting_date"] for m in result["meetings"]], ["2026-10-28"])

    def test_trailing_lookalike_does_not_poison_a_complete_last_bucket(self):
        html = make_investing_html([("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100)])])
        html += '<aside><span>4.0 - 4.25</span><div></div><span>30%</span></aside>'
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertFalse(result["errors"])
        self.assertEqual(result["meetings"][0]["raw_probabilities"][0]["probability_pct"], 100)

    def test_two_matches_in_one_bucket_cannot_complete_a_missing_other_bucket(self):
        html = make_investing_html([("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100)]),
            ("Dec 09, 2026 02:00PM ET", [(3.5, 3.75, "bad"), (3.75, 4, 70)])])
        html = html.replace('<span>70%</span>', '<span>70%</span><span>4.0 - 4.25</span><div></div><span>30%</span>')
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertEqual([m["meeting_date"] for m in result["meetings"]], ["2026-10-28"])
        self.assertEqual(result["parse_report"]["unmatched_bucket_item_count"], 2)

    def test_unclosed_bucket_cannot_certify_a_complete_distribution(self):
        html = make_investing_html([("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100)]),
            ("Dec 09, 2026 02:00PM ET", [(3.75, 4, 100)])])
        html = html[:html.rindex("</div></div>")]
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertEqual([m["meeting_date"] for m in result["meetings"]], ["2026-10-28"])
        self.assertIn("2026-12-09", result["parse_report"]["partial_meeting_dates"])

    def test_intact_same_meeting_copy_can_replace_a_broken_main_copy(self):
        html = make_investing_html([
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, "abc"), (4, 4.25, 70)]),
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 30), (4, 4.25, 70)])])
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertEqual([r["probability_pct"] for r in result["meetings"][0]["raw_probabilities"]], [30, 70])
        self.assertIn("2026-10-28", result["parse_report"]["recovered_meeting_dates"])

    def test_numerically_invalid_copy_can_be_replaced_without_blending(self):
        html = make_investing_html([("Oct 28, 2026 02:00PM ET", [(3.75, 4, 97)]),
            ("Oct 28, 2026 02:00PM ET", [(3.5, 3.75, 60), (3.75, 4, 40)])])
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        self.assertEqual([r["probability_pct"] for r in result["meetings"][0]["raw_probabilities"]], [60, 40])
        self.assertIn("2026-10-28", result["parse_report"]["recovered_meeting_dates"])

    def test_default_normalizer_is_strict_and_partial_mode_separates_records(self):
        rows = [{"meeting_date": day, "rate_low": 3.75, "rate_high": 4, "probability_pct": p}
                for day, p in [("2026-10-28", 100), ("2026-12-09", 97)]]
        with self.assertRaises(FedwatchError):
            investing.normalize_cumulative(rows)
        rejected = []
        accepted, records = investing.normalize_cumulative(rows, rejected=rejected)
        self.assertEqual(len(accepted), 1)
        self.assertEqual(len(records), 1)
        self.assertIn("normalized_expected_rate", records[0])
        self.assertEqual(rejected[0]["detail"]["meeting_date"], "2026-12-09")

    def test_production_conversion_retains_qualified_capture_values(self):
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor",
                                               fixture_text(FIXTURE_INVESTING_LIVE)), clock=CLOCK)
        sections = {m["meeting_date"]: m for m in investing.with_local_probabilities(result, 4, 3.75)}
        self.assertEqual(sections["2026-10-28"]["local_probabilities"],
                         [{"outcome_bp": 0, "probability_pct": 30}, {"outcome_bp": 25, "probability_pct": 70}])
        self.assertEqual(sections["2026-12-09"]["local_probabilities"],
                         [{"outcome_bp": 0, "probability_pct": 20.750751},
                          {"outcome_bp": 25, "probability_pct": 79.249249}])

    def test_production_normalization_regression_rounding_and_chaining(self):
        html = make_investing_html([
            ("Sep 16, 2026 02:00PM ET", [(3.5, 3.75, 35.6), (3.75, 4, 64.4)]),
            ("Oct 28, 2026 02:00PM ET", [(3.5, 3.75, 25.8), (3.75, 4, 56.5), (4, 4.25, 17.8)]),
            ("Dec 09, 2026 02:00PM ET", [(3.5, 3.75, 25.8), (3.75, 4, 56.5), (4, 4.25, 17.8)])])
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=CLOCK)
        sections = investing.with_local_probabilities(result, 3.75, 3.5)
        self.assertEqual(sections[1]["local_probabilities"],
                         [{"outcome_bp": 0, "probability_pct": 72.392008},
                          {"outcome_bp": 25, "probability_pct": 27.607992}])
        self.assertEqual(sections[2]["local_probabilities"], [{"outcome_bp": 0, "probability_pct": 100}])
        self.assertEqual([s["meeting_ordinal"] for s in sections], [1, 2, 3])

    def test_off_schedule_meeting_has_identity_error_not_predecessor_error(self):
        result = investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor",
            make_investing_html([("Oct 29, 2026 02:00PM ET", [(3.75, 4, 100)])])), clock=CLOCK)
        section = investing.with_local_probabilities(result, 4, 3.75, meeting_dates=["2026-10-28"])[0]
        self.assertEqual(section["local_status"], "MEETING_DATE_MISMATCH")
        self.assertEqual(section["local_error"]["code"], "INVESTING_MEETING_DATE_MISMATCH")


class FieldIdentityReviewTests(unittest.TestCase):
    def test_fred_uses_named_requested_series_despite_unrelated_extra_column(self):
        rows, _ = fred.fetch_series(FakeTransport().add_text("DFEDTARU",
            "DATE,UNRELATED,DFEDTARU\n2026-10-02,bad,4\n"), "DFEDTARU")
        self.assertEqual(rows, [{"date": date(2026, 10, 2), "value": 4}])
        for header in ("DATE,OTHER,VALUE", "DATE,DFEDTARU,DFEDTARU"):
            with self.subTest(header=header), self.assertRaises(ValueError):
                fred.parse_fred_csv(header + "\n2026-10-02,4,4\n", series_id="DFEDTARU")

    def test_binary_yes_identity_accepts_case_whitespace_and_reordered_labels(self):
        event = make_fed_decision_event()
        market = event["markets"][0]
        market.update(outcomes=json.dumps([" no ", " YES "]),
                      outcomePrices=json.dumps([.7, .3]), clobTokenIds=json.dumps(["no-token", "yes-token"]))
        result = polymarket.extract_markets(event)[0]
        self.assertTrue(result["binary_outcomes_valid"])
        self.assertEqual(result["yes_clob_token_id"], "yes-token")
        self.assertEqual(result["yes_probability"], .3)
        for outcomes in (["Yes", "Yes"], ["Yes", "Maybe"], ["Yes", "No", "Maybe"], [True, "No"]):
            market["outcomes"] = json.dumps(outcomes)
            self.assertFalse(polymarket.extract_markets(event)[0]["binary_outcomes_valid"])


class ImportReviewTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def monthly(self, observations, symbol="ZQV26"):
        path = self.root / (symbol + ".csv")
        with path.open("w", encoding="utf-8", newline="") as handle:
            handle.write(f"Symbol: {symbol}\nSource: https://www.investing.com/history?cid=1\n")
            writer = csv.writer(handle)
            writer.writerow(["Date", "Price", "Open", "High", "Low", "Vol.", "Change %"])
            writer.writerows([day, price, 96, 96, 96, 1, "0%"] for day, price in observations)
        return monthly_csv.load_contracts(self.root)

    def test_valid_date_whitespace_does_not_substitute_an_older_price(self):
        rows, report = self.monthly([("10/02/2026", 96.1), ("10/03/2026 ", 96.2)])
        self.assertEqual(zq.month_avg_price(rows, 2026, 10, date(2026, 10, 3)), 96.2)
        self.assertFalse(report["errors"])

    def test_undated_row_preserves_source_values_but_blocks_ambiguous_contract_lookup(self):
        rows, report = self.monthly([("10/02/2026", 96.1), ("BROKEN", 96.2), ("10/04/2026", 96.3)])
        self.assertEqual([r["close_price"] for r in rows], [96.1, 96.3])
        for day in (date(2026, 10, 3), date(2026, 10, 4)):
            with self.subTest(day=day), self.assertRaisesRegex(ValueError, "undated"):
                zq.month_avg_price(rows, 2026, 10, day)
        self.assertEqual(report["files"][0]["undated_rejected_row_count"], 1)

    def test_provider_missing_and_rejected_close_are_distinct(self):
        rows, report = self.monthly([("10/01/2026", 96.1), ("10/02/2026", "-"), ("10/03/2026", "bad")])
        self.assertEqual([r["close_status"] for r in rows], ["OBSERVED", "SOURCE_MISSING", "REJECTED"])
        self.assertEqual(report["files"][0]["missing_close_rows"], 1)
        self.assertEqual(report["files"][0]["rejected_close_rows"], 1)
        with self.assertRaisesRegex(ValueError, "rejected"):
            zq.month_avg_price(rows, 2026, 10, date(2026, 10, 3))

    def test_monthly_quality_survives_storage_for_an_independently_valid_watch_date(self):
        self.monthly([("10/01/2026", 96.1), ("10/02/2026", "-"),
                      ("10/03/2026", "bad"), ("10/04/2026", 96.2)])
        self.monthly([("10/04/2026", 96.3)], symbol="ZQX26")
        store = FedwatchHistoryStore(self.root / "history.db")
        html = fixture_text(FIXTURE_FOMC_CALENDAR)
        transport = make_snapshot_transport(NOW).add_text("fomccalendars", html[:html.index("2027 FOMC Meetings")])
        history.import_zq(store, transport, self.root, [NOW.date()], clock=CLOCK,
                          input_format="investing", meeting_date="2026-10-28")
        observations = store.observations(meeting_date="2026-10-28", method=history.FED_METHOD_ZQ)
        self.assertTrue(observations)
        inputs = observations[0]["detail"]["contract_inputs"]
        october = next(file for file in inputs["files"] if file["symbol"] == "ZQV26")
        self.assertEqual(october["close_quality"], [{"date": "2026-10-02", "status": "SOURCE_MISSING"},
                                                  {"date": "2026-10-03", "status": "REJECTED"}])

    def test_undated_required_contract_cannot_create_durable_reconstruction(self):
        self.monthly([("10/02/2026", 96.1), ("BROKEN", 96.2), ("10/04/2026", 96.3)])
        self.monthly([("10/04/2026", 96.4)], symbol="ZQX26")
        store = FedwatchHistoryStore(self.root / "history.db")
        result = history.import_zq(store, make_snapshot_transport(NOW), self.root, [NOW.date()], clock=CLOCK,
                                   input_format="investing", meeting_date="2026-10-28")
        self.assertIn("FEDWATCH_ZQ_DATE_UNCERTAIN", {error["code"] for error in result["errors"]})
        self.assertFalse(store.observations(meeting_date="2026-10-28", method=history.FED_METHOD_ZQ))

    def test_undated_unneeded_contract_does_not_block_independent_reconstruction(self):
        self.monthly([("10/04/2026", 96.2)])
        self.monthly([("10/04/2026", 96.3)], symbol="ZQX26")
        self.monthly([("BROKEN", 96.4), ("10/04/2026", 96.4)], symbol="ZQZ26")
        store = FedwatchHistoryStore(self.root / "history.db")
        result = history.import_zq(store, make_snapshot_transport(NOW), self.root, [NOW.date()], clock=CLOCK,
                                   input_format="investing", meeting_date="2026-10-28")
        self.assertNotIn("FEDWATCH_ZQ_DATE_UNCERTAIN", {error["code"] for error in result["errors"]})
        self.assertTrue(store.observations(meeting_date="2026-10-28", method=history.FED_METHOD_ZQ))

    def test_undated_small_cme_bucket_cannot_certify_incomplete_distribution(self):
        path = self.root / "published.csv"
        meeting = "2026-10-28"
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(published_history.COLUMNS)
            writer.writerows([[meeting, "2026-10-02", 375, 400, 99.5],
                              [meeting, "BROKEN", 400, 425, .5]])
        store = FedwatchHistoryStore(self.root / "history.db")
        with self.assertRaises(FedwatchError) as caught:
            published_history.import_file(store, path, meeting, "https://www.cmegroup.com/fedwatch", clock=CLOCK)
        self.assertEqual(caught.exception.code, "FEDWATCH_PUBLISHED_HISTORY_DATE_UNCERTAIN")
        self.assertEqual(store.count_observations(), 0)

    def test_undated_cme_import_preserves_existing_history_exactly(self):
        path = self.root / "published.csv"
        meeting = "2026-10-28"
        path.write_text(",".join(published_history.COLUMNS) + f"\n{meeting},2026-10-01,375,400,100\n", encoding="utf-8")
        store = FedwatchHistoryStore(self.root / "history.db")
        published_history.import_file(store, path, meeting, "https://www.cmegroup.com/fedwatch", clock=CLOCK)
        before = store.observations(meeting_date=meeting)
        path.write_text(",".join(published_history.COLUMNS) +
                        f"\n{meeting},2026-10-02,375,400,100\n{meeting},BROKEN,400,425,0\n", encoding="utf-8")
        with self.assertRaises(FedwatchError):
            published_history.import_file(store, path, meeting, "https://www.cmegroup.com/fedwatch", clock=CLOCK)
        self.assertEqual(store.observations(meeting_date=meeting), before)
