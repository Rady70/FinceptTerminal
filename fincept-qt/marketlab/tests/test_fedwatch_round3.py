"""Date-bounded uncertainty and labelled retention, including durable detail."""
import csv
import tempfile
import unittest
from datetime import date
from pathlib import Path

from fedwatch_test_support import FakeTransport, FixedClock, make_snapshot_transport, make_investing_html, make_fed_decision_event, utc
import json
from fedwatch import fred, history, monthly_csv, polymarket, published_history, snapshot, zq
from fedwatch.errors import FedwatchError
from fedwatch.store import FedwatchHistoryStore
from test_fedwatch_fomc import synthetic_calendar

NOW = utc(2026, 10, 4, 12)
CLOCK = FixedClock(NOW)


class CarryForwardTests(unittest.TestCase):
    def test_complete_calendar_carries_six_day_pair_into_all_locals_and_comparisons(self):
        result = snapshot.build_snapshot(make_snapshot_transport(NOW), clock=CLOCK, sleep=lambda _: None)
        target = result["data"]["current_target_range"]
        self.assertEqual(target["status"], "CURRENT")
        self.assertTrue(target["carried_forward"])
        self.assertEqual(target["status_reason"], "CARRIED_FORWARD_NO_FOMC_DECISION")
        self.assertEqual(sum(bool(m["fed_side"]["local_probabilities"]) for m in result["data"]["meetings"]), 10)
        self.assertEqual(sum(bool(m["comparison"]) for m in result["data"]["meetings"]), 2)

    def test_decision_on_or_after_pair_before_today_blocks_carry_forward(self):
        for end in (date(2026, 9, 28), date(2026, 10, 3)):
            with self.subTest(end=end):
                result = fred.fetch_target_range(make_snapshot_transport(NOW), clock=CLOCK,
                    calendar={"coverage_complete": True, "meetings": [{"end_date": end}]})
                self.assertEqual(result["status"], "STALE")
                self.assertEqual(result["status_reason"], "FOMC_DECISION_SINCE_PAIR")
                self.assertFalse(result["carried_forward"])

    def test_missing_partial_or_stale_calendar_keeps_three_day_limit(self):
        for calendar in (None, {"coverage_complete": False, "meetings": []},
                         {"coverage_complete": True, "fallback_stale": True, "meetings": []}):
            with self.subTest(calendar=calendar):
                result = fred.fetch_target_range(make_snapshot_transport(NOW), clock=CLOCK, calendar=calendar)
                self.assertEqual(result["status"], "STALE")
                self.assertEqual(result["status_reason"], "PAIR_TOO_OLD")


class BoundedRowsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.store = FedwatchHistoryStore(self.root / "history.db")
        self.addCleanup(self.store.close)

    def monthly(self, observations, symbol="ZQV26"):
        with (self.root / (symbol + ".csv")).open("w", encoding="utf-8", newline="") as stream:
            stream.write(f"Symbol: {symbol}\nSource: https://www.investing.com/history?cid=1\n")
            writer = csv.writer(stream)
            writer.writerow(["Date", "Price", "Open", "High", "Low", "Vol.", "Change %"])
            writer.writerows([day, close, 96, 96, 96, 1, 0] for day, close in observations)
        return monthly_csv.load_contracts(self.root)

    def published(self, observations):
        path = self.root / "published.csv"
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(published_history.COLUMNS)
            writer.writerows(["2026-10-28", day, 375, 400, 100] for day in observations)
        return published_history.import_file(self.store, path, "2026-10-28", "https://www.cmegroup.com/fedwatch", clock=CLOCK)

    def test_ascending_descending_and_consecutive_bad_rows_bound_only_latest_close_window(self):
        observations = [("10/01/2026", 96.1), ("10/02/2026", 96.2), ("BAD", 96.9),
                        ("ALSO BAD", 96.8), ("10/03/2026", 96.3), ("10/04/2026", 96.4)]
        for values in (observations, list(reversed(observations))):
            with self.subTest(descending=values != observations):
                rows, report = self.monthly(values)
                self.assertTrue(report["files"][0]["date_ordered"])
                self.assertEqual(zq.month_avg_price(rows, 2026, 10, date(2026, 10, 1)), 96.1)
                self.assertEqual(zq.month_avg_price(rows, 2026, 10, date(2026, 10, 4)), 96.4)
                for day in (2, 3):
                    self.assertTrue(zq.close_date_uncertain(rows, date(2026, 10, day)))
                    with self.assertRaisesRegex(ValueError, "undated"):
                        zq.month_avg_price(rows, 2026, 10, date(2026, 10, day))
                self.assertEqual({(w["older_date"], w["newer_date"]) for w in rows[0]["date_uncertainty_windows"]},
                                 {("2026-10-02", "2026-10-03")})

    def test_open_ended_windows_in_both_orders(self):
        dated = [("10/01/2026", 96.1), ("10/02/2026", 96.2), ("10/04/2026", 96.4)]
        for values, safe, unsafe in (([("BAD", 97)] + dated, 2, 1),
                                    (dated + [("BAD", 97)], 2, 4),
                                    ([("BAD", 97)] + list(reversed(dated)), 2, 4),
                                    (list(reversed(dated)) + [("BAD", 97)], 2, 1)):
            with self.subTest(values=values):
                rows, _ = self.monthly(values)
                self.assertFalse(zq.close_date_uncertain(rows, date(2026, 10, safe)))
                self.assertTrue(zq.close_date_uncertain(rows, date(2026, 10, unsafe)))

    def test_unordered_monthly_file_keeps_whole_contract_guard(self):
        rows, report = self.monthly([("10/02/2026", 96.2), ("BAD", 97), ("10/01/2026", 96.1), ("10/04/2026", 96.4)])
        self.assertFalse(report["files"][0]["date_ordered"])
        for day in (1, 2, 4, 10):
            self.assertTrue(zq.close_date_uncertain(rows, date(2026, 10, day)))

    def test_deconvolution_guard_and_storage_use_the_same_bounded_window(self):
        self.monthly([("10/01/2026", 96.1), ("10/02/2026", 96.2), ("BAD", 97),
                      ("10/03/2026", 96.3), ("10/04/2026", 96.4)])
        self.monthly([("10/01/2026", 96.1), ("10/04/2026", 96.4)], symbol="ZQX26")
        result = history.import_zq(self.store, make_snapshot_transport(NOW), self.root,
            [date(2026, 10, day) for day in (1, 2, 3, 4)], clock=CLOCK, input_format="investing", meeting_date="2026-10-28")
        uncertain = [error["detail"]["watch_date"] for error in result["errors"] if error["code"] == "FEDWATCH_ZQ_DATE_UNCERTAIN"]
        self.assertEqual(uncertain, ["2026-10-02", "2026-10-03"])
        stored = self.store.observations(meeting_date="2026-10-28", method=history.FED_METHOD_ZQ)
        self.assertEqual({row["detail"]["watch_date"] for row in stored}, {"2026-10-01", "2026-10-04"})
        quality = next(file for file in stored[0]["detail"]["contract_inputs"]["files"] if file["symbol"] == "ZQV26")
        self.assertEqual(quality["date_uncertainty_windows"][0]["older_date"], "2026-10-02")

    def test_grouped_published_dates_reject_only_neighbours_in_both_orders(self):
        values = ["2026-10-01", "2026-10-02", "BAD", "ALSO BAD", "2026-10-03", "2026-10-04"]
        for dates in (values, list(reversed(values))):
            with self.subTest(descending=dates != values):
                result = self.published(dates)
                self.assertEqual(result["rejected_reporting_dates"], ["2026-10-02", "2026-10-03"])
                self.assertEqual((result["first_date"], result["last_date"], result["reporting_dates"]),
                                 ("2026-10-01", "2026-10-04", 2))

    def test_published_end_rows_affect_only_the_one_neighbour(self):
        for values, rejected in ((["BAD", "2026-10-01", "2026-10-02", "2026-10-03"], "2026-10-01"),
                                 (["2026-10-03", "2026-10-02", "2026-10-01", "BAD"], "2026-10-01")):
            with self.subTest(values=values):
                result = self.published(values)
                self.assertEqual(result["rejected_reporting_dates"], [rejected])
                self.assertEqual(result["reporting_dates"], 2)

    def test_bad_row_inside_a_group_quarantines_only_that_reporting_group(self):
        path = self.root / "published.csv"
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(published_history.COLUMNS)
            writer.writerows([["2026-10-28", day, low, low + 25, probability] for day, low, probability in (
                ("2026-10-01", 375, 100), ("2026-10-02", 375, 60), ("BAD", 400, 10),
                ("2026-10-02", 400, 40), ("2026-10-03", 375, 100))])
        result = published_history.import_file(self.store, path, "2026-10-28", "https://www.cmegroup.com/fedwatch", clock=CLOCK)
        self.assertEqual(result["rejected_reporting_dates"], ["2026-10-02"])
        self.assertEqual(result["reporting_dates"], 2)

    def test_unordered_published_file_preserves_history_before_any_write(self):
        self.published(["2026-10-01"])
        before = self.store.observations()
        with self.assertRaises(FedwatchError) as caught:
            self.published(["2026-10-03", "BAD", "2026-10-01", "2026-10-04"])
        self.assertEqual(caught.exception.code, "FEDWATCH_PUBLISHED_HISTORY_DATE_UNCERTAIN")
        self.assertEqual(self.store.observations(), before)

    def test_copy_conflict_is_retained_in_locals_comparisons_and_durable_detail(self):
        html = make_investing_html([
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 97)]),
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 30), (4, 4.25, 70)])])
        transport = make_snapshot_transport(NOW).add_text("fed-rate-monitor", html)
        result = snapshot.build_snapshot(transport, clock=CLOCK, sleep=lambda _: None)
        october = next(row for row in result["data"]["meetings"] if row["meeting_date"] == "2026-10-28")
        self.assertTrue(october["fed_side"]["copy_conflict"])
        self.assertTrue(october["fed_side"]["local_probabilities"])
        self.assertTrue(october["comparison"])
        history.record_snapshot(self.store, result["data"], clock=CLOCK)
        rows = self.store.observations(meeting_date="2026-10-28", method=history.FED_METHOD_LIVE)
        self.assertTrue(rows)
        self.assertTrue(all(row["detail"]["copy_conflict"] for row in rows))
        self.assertEqual(rows[0]["detail"]["copy_conflicts"][0]["earlier_probability_pct"], 97)

    def test_conflicting_broken_copies_do_not_prevent_a_later_intact_copy(self):
        html = make_investing_html([
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 20), (4, 4.25, "bad")]),
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 30), (4, 4.25, "bad")]),
            ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 30), (4, 4.25, 70)])])
        result = snapshot.build_snapshot(make_snapshot_transport(NOW).add_text("fed-rate-monitor", html),
                                         clock=CLOCK, sleep=lambda _: None)
        fed = result["data"]["meetings"][0]["fed_side"]
        self.assertTrue(fed["copy_conflict"])
        self.assertEqual([row["probability_pct"] for row in fed["raw_probabilities"]], [30, 70])

    def test_each_nonbinary_market_preserves_other_mappings_quotes_and_history(self):
        for bad_index in range(5):
            with self.subTest(bad_index=bad_index):
                event = make_fed_decision_event(event_id=str(700 + bad_index))
                event["markets"][bad_index]["outcomes"] = json.dumps(["Yes", "Yes"])
                transport = make_snapshot_transport(NOW, events=[event])
                result = snapshot.build_snapshot(transport, clock=CLOCK, sleep=lambda _: None)
                section = result["data"]["meetings"][0]["polymarket"]
                self.assertEqual(section["mapping_status"], "VALIDATED")
                self.assertEqual(section["data_status"], "PARTIAL")
                self.assertEqual(sum(row["probability_pct"] is not None for row in section["outcomes"]), 4)
                self.assertEqual(section["mapping_evidence"]["validated_binary_market_count"], 4)
                self.assertEqual(len([call for call in transport.json_calls if "prices-history" in call["url"]]), 4)
                history.record_snapshot(self.store, result["data"], clock=CLOCK)
                mappings = self.store.validated_mappings(meeting_dates=["2026-10-28"])
                self.assertFalse(any(row["external_market_id"] == event["markets"][bad_index]["id"] for row in mappings))


if __name__ == "__main__":
    unittest.main()
