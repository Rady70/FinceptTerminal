"""Deterministic tests for the optional historical ZQ reconstruction path.

The port must preserve the qualified deconvolution algebra (CME's day-weight
convention, price propagation, the integer+mantissa binary split and the
multi-meeting-month approximation) while remaining optional: these tests also
prove that the live archive and the store never require ZQ data and that the
user's raw dataset is never copied. No test uses the network.
"""

from __future__ import annotations

import tempfile
import unittest
from datetime import date
from pathlib import Path

import fedwatch_test_support  # noqa: F401  (bootstraps the scripts path)

from fedwatch.errors import ZqDataError
from fedwatch import zq as fedwatch_zq
from fedwatch.history import FED_METHOD_ZQ, record_zq_observations
from fedwatch.investing import local_step_distribution
from fedwatch.store import FedwatchHistoryStore
from fedwatch.zq import (
    MonthRecord,
    _solve_month,
    load_contract_file,
    load_contracts,
    month_avg_price,
    propagate_prices,
    run_deconvolution,
)

HEADER = "Date Time,Open,High,Low,Close,Change,Volume,Open Interest\n"


def write_contract(directory: Path, filename: str, rows, symbol: str) -> Path:
    text = f"Symbol: {symbol}\n" + HEADER + "\n".join(rows) + "\n"
    path = directory / filename
    path.write_text(text, encoding="utf-8")
    return path


def contract_rows(year, month, pairs):
    return [
        {
            "contract_symbol": f"ZQ-{year}-{month}",
            "contract_month": month,
            "contract_year": year,
            "date": day,
            "close_price": close,
            "volume": 10.0,
            "open_interest": 5000.0,
            "low_confidence": False,
        }
        for day, close in pairs
    ]


class IngestTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.directory = Path(self._tmp.name)

    def test_filename_parsing(self):
        symbol, month, year = fedwatch_zq.parse_contract_filename(Path("ZQZ26.csv"))
        self.assertEqual((symbol, month, year), ("ZQZ26", 12, 2026))
        symbol, month, year = fedwatch_zq.parse_contract_filename(Path("zqf22.CSV"))
        self.assertEqual((symbol, month, year), ("ZQF22", 1, 2022))
        with self.assertRaises(ZqDataError) as caught:
            fedwatch_zq.parse_contract_filename(Path("ESZ26.csv"))
        self.assertEqual(caught.exception.code, "FEDWATCH_ZQ_DATA_INVALID")

    def test_contract_file_loading_skips_footnotes_and_flags_low_confidence(self):
        path = write_contract(
            self.directory,
            "ZQZ26.csv",
            [
                "2026-12-01,95.5,95.6,95.4,95.55,0.05,100,5000",
                "Downloaded from Barchart.com",
                "2026-12-02,95.5,95.7,95.4,95.60,0.05,0,100",
            ],
            symbol="ZQZ26",
        )
        rows, report = load_contract_file(path)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["contract_year"], 2026)
        self.assertEqual(rows[0]["contract_month"], 12)
        self.assertEqual(rows[0]["date"], date(2026, 12, 1))
        self.assertFalse(rows[0]["low_confidence"])
        self.assertTrue(rows[1]["low_confidence"])
        self.assertEqual(report["skipped_rows"], 1)
        self.assertEqual(report["low_confidence_rows"], 1)

    def test_wrong_header_is_invalid(self):
        path = self.directory / "ZQZ26.csv"
        path.write_text("Symbol: ZQZ26\nNot,A,Header\n", encoding="utf-8")
        with self.assertRaises(ZqDataError) as caught:
            load_contract_file(path)
        self.assertEqual(caught.exception.code, "FEDWATCH_ZQ_DATA_INVALID")

    def test_directory_loading_reports_and_fails_closed(self):
        with self.assertRaises(ZqDataError):
            load_contracts(self.directory / "does-not-exist")
        with self.assertRaises(ZqDataError):
            load_contracts(self.directory)

        write_contract(
            self.directory, "ZQZ26.csv",
            ["2026-12-01,95.5,95.6,95.4,95.55,0.05,100,5000"], symbol="ZQZ26",
        )
        (self.directory / "ZQF27.csv").write_text("Symbol: ZQF27\nbad\n", encoding="utf-8")
        contracts, report = load_contracts(self.directory)
        self.assertEqual(len(contracts), 1)
        self.assertEqual(len(report["skipped_files"]), 1)
        self.assertEqual(report["skipped_files"][0]["file"], "ZQF27.csv")

        bad_dir = self.directory / "all-bad"
        bad_dir.mkdir()
        (bad_dir / "ZQG27.csv").write_text("Symbol: ZQG27\nbad\n", encoding="utf-8")
        with self.assertRaises(ZqDataError):
            load_contracts(bad_dir)


class PriceConstructionTests(unittest.TestCase):
    def test_month_avg_price_active_and_expired(self):
        contracts = contract_rows(
            2027, 1,
            [(date(2027, 1, 5), 98.0), (date(2027, 1, 10), 98.1), (date(2027, 1, 20), 98.2)],
        )
        self.assertAlmostEqual(month_avg_price(contracts, 2027, 1, date(2027, 1, 12)), 98.1)

        expired = contract_rows(
            2027, 1,
            [(date(2027, 1, 20), 98.0), (date(2027, 1, 29), 98.1), (date(2027, 2, 15), 999.0)],
        )
        self.assertAlmostEqual(month_avg_price(expired, 2027, 1, date(2027, 3, 1)), 98.1)
        with self.assertRaises(ValueError):
            month_avg_price(contracts, 2027, 2, date(2027, 1, 12))

    def test_month_avg_price_refuses_a_missing_latest_close(self):
        contracts = contract_rows(
            2027, 1,
            [(date(2027, 1, 5), 98.0), (date(2027, 1, 10), 98.1)],
        )
        contracts[-1]["close_price"] = None
        with self.assertRaises(ValueError):
            month_avg_price(contracts, 2027, 1, date(2027, 1, 12))

    def test_solve_month_matches_cme_published_worked_example(self):
        record = MonthRecord(
            year=2022, month=9, p_avg=97.4475, p_start=None, p_end=96.9400,
            meeting_end_dates=[date(2022, 9, 21)],
        )
        _solve_month(record)
        self.assertAlmostEqual(record.p_start, 97.6650, places=3)
        change_steps = (record.p_start - record.p_end) / 25 * 100
        self.assertAlmostEqual(change_steps, 2.9, places=3)
        local = local_step_distribution(change_steps)
        self.assertAlmostEqual(local[50], 0.10, places=3)
        self.assertAlmostEqual(local[75], 0.90, places=3)

    def test_propagate_single_fomc_month_between_non_fomc_months(self):
        january = MonthRecord(year=2027, month=1, p_avg=98.00)
        february = MonthRecord(
            year=2027, month=2, meeting_end_dates=[date(2027, 2, 15)], p_avg=97.814286
        )
        march = MonthRecord(year=2027, month=3, p_avg=97.60)
        months, warnings = propagate_prices([january, february, march])
        self.assertEqual(warnings, [])
        self.assertAlmostEqual(months[0].p_start, 98.00)
        self.assertAlmostEqual(months[1].p_start, 98.00)
        self.assertAlmostEqual(months[1].p_end, 97.60)
        self.assertAlmostEqual(months[2].p_end, 97.60)
        reconstructed = (15 * months[1].p_start + 13 * months[1].p_end) / 28
        self.assertAlmostEqual(reconstructed, february.p_avg, places=5)

    def test_propagate_consecutive_fomc_months_chain(self):
        may = MonthRecord(year=2026, month=5, p_avg=96.40)
        june = MonthRecord(year=2026, month=6, meeting_end_dates=[date(2026, 6, 17)], p_avg=96.38)
        july = MonthRecord(year=2026, month=7, meeting_end_dates=[date(2026, 7, 29)], p_avg=96.37)
        august = MonthRecord(year=2026, month=8, p_avg=96.28)
        months, _warnings = propagate_prices([may, june, july, august])
        self.assertAlmostEqual(months[1].p_start, 96.40)
        self.assertAlmostEqual(months[2].p_end, 96.28)
        self.assertAlmostEqual(months[1].p_end, months[2].p_start)
        reconstructed = (29 * months[2].p_start + 2 * months[2].p_end) / 31
        self.assertAlmostEqual(reconstructed, july.p_avg, places=5)

    def test_index_zero_fomc_month_resolves_via_backward_solve(self):
        january = MonthRecord(
            year=2025, month=1, meeting_end_dates=[date(2025, 1, 29)], p_avg=95.675
        )
        february = MonthRecord(year=2025, month=2, p_avg=95.69)
        months, _warnings = propagate_prices([january, february])
        self.assertIsNotNone(months[0].p_start)
        self.assertAlmostEqual(months[0].p_end, february.p_avg)
        reconstructed = (29 * months[0].p_start + 2 * months[0].p_end) / 31
        self.assertAlmostEqual(reconstructed, january.p_avg, places=6)

    def test_multi_meeting_month_is_flagged_and_conserves_average(self):
        january = MonthRecord(year=2027, month=1, p_avg=98.00)
        february = MonthRecord(
            year=2027, month=2,
            meeting_end_dates=[date(2027, 2, 5), date(2027, 2, 20)],
            p_avg=97.50,
        )
        march = MonthRecord(year=2027, month=3, p_avg=97.00)
        months, warnings = propagate_prices([january, february, march])
        resolved = months[1]
        self.assertTrue(resolved.multi_meeting_month)
        self.assertTrue(resolved.resolved_via_approximation)
        self.assertEqual(len(resolved.segment_rates), 2)
        self.assertTrue(any("approximated" in warning for warning in warnings))
        boundaries = [resolved.p_start] + resolved.segment_rates
        segment_days = [5, 15, 28 - 20]
        weighted = sum(days * rate for days, rate in zip(segment_days, boundaries)) / 28
        self.assertAlmostEqual(weighted, february.p_avg, places=9)


class DeconvolutionEngineTests(unittest.TestCase):
    def test_run_deconvolution_produces_binary_local_and_full_cumulative(self):
        watch_date = date(2026, 7, 14)
        meetings = [date(2026, 7, 29), date(2026, 12, 9)]
        contracts = []
        for month, close in ((7, 96.5), (8, 96.4), (9, 96.4), (10, 96.3), (11, 96.3), (12, 96.3)):
            contracts += contract_rows(
                2026, month,
                [(date(2026, 7, 10), close - 0.05), (date(2026, 7, 13), close)],
            )
        contracts += contract_rows(2027, 1, [(date(2026, 7, 13), 96.3)])
        result = run_deconvolution(
            watch_date, meetings, contracts,
            current_rate_upper=3.75, current_rate_lower=3.50,
        )
        local = [row for row in result["rows"] if row["row_type"] == "local"]
        cumulative = [row for row in result["rows"] if row["row_type"] == "cumulative"]
        self.assertGreaterEqual(len(local), 2)
        self.assertEqual(len(local) % 2, 0)
        local_sums = {}
        for row in local:
            local_sums.setdefault(row["meeting_date"], 0.0)
            local_sums[row["meeting_date"]] += row["probability_pct"]
        for total in local_sums.values():
            self.assertAlmostEqual(total, 100.0, places=6)
        cumulative_sums = {}
        for row in cumulative:
            cumulative_sums.setdefault(row["meeting_date"], 0.0)
            cumulative_sums[row["meeting_date"]] += row["probability_pct"]
        for total in cumulative_sums.values():
            self.assertAlmostEqual(total, 100.0, places=6)
        self.assertTrue(all(row["rate_low"] is None for row in local))
        self.assertTrue(all(row["rate_low"] is not None for row in cumulative))
        report = result["report"]
        self.assertGreaterEqual(report["meeting_count"], 2)
        self.assertEqual(
            [skip["meeting_date"] for skip in report["skipped_meetings"]],
            ["2026-12-09"],
        )
        self.assertEqual(report["buffer_skipped_meetings"], [])

    def test_meeting_without_a_following_contract_month_is_excluded(self):
        watch_date = date(2026, 7, 14)
        contracts = contract_rows(
            2026, 7, [(date(2026, 7, 10), 96.4), (date(2026, 7, 13), 96.5)]
        ) + contract_rows(2026, 8, [(date(2026, 7, 10), 96.3), (date(2026, 7, 13), 96.3)])
        with self.assertRaises(ZqDataError):
            run_deconvolution(
                watch_date, [date(2026, 8, 26)], contracts,
                current_rate_upper=3.75, current_rate_lower=3.50,
            )

    def test_missing_latest_close_is_not_priced_from_a_stale_close(self):
        watch_date = date(2026, 7, 14)
        meetings = [date(2026, 7, 29), date(2026, 12, 9)]
        contracts = []
        for month, close in ((7, 96.5), (8, 96.4), (9, 96.4), (10, 96.3), (11, 96.3), (12, 96.3)):
            contracts += contract_rows(
                2026, month,
                [(date(2026, 7, 10), close - 0.05), (date(2026, 7, 13), close)],
            )
        contracts += contract_rows(2027, 1, [(date(2026, 7, 13), 96.3)])
        july_latest = [
            row for row in contracts
            if row["contract_year"] == 2026 and row["contract_month"] == 7
            and row["date"] == date(2026, 7, 13)
        ][0]
        july_latest["close_price"] = None
        result = run_deconvolution(
            watch_date, meetings, contracts,
            current_rate_upper=3.75, current_rate_lower=3.50,
        )
        self.assertIn(
            "2026-07-29",
            [skip["meeting_date"] for skip in result["report"]["skipped_meetings"]],
        )


class ZqPersistenceTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.store = FedwatchHistoryStore(Path(self._tmp.name) / "history.db")

    def local_rows(self):
        return [
            {
                "meeting_date": "2026-07-29",
                "meeting_ordinal": 1,
                "local_bp_change": 0,
                "probability_pct": 20.0,
                "multi_meeting_month": False,
                "approximated_month_split": False,
            },
            {
                "meeting_date": "2026-07-29",
                "meeting_ordinal": 1,
                "local_bp_change": 25,
                "probability_pct": 80.0,
                "multi_meeting_month": False,
                "approximated_month_split": False,
            },
        ]

    def test_reconstructed_observations_persist_with_a_distinct_method(self):
        report = record_zq_observations(
            self.store, date(2026, 7, 14), self.local_rows(),
            detail_base={"current_target_range": {"upper": 3.75, "lower": 3.50}},
        )
        self.assertEqual(report["processed"], 2)
        self.assertEqual(report["counts"], {"inserted": 2})
        rows = self.store.observations(meeting_date="2026-07-29")
        self.assertEqual({row["method"] for row in rows}, {FED_METHOD_ZQ})
        self.assertEqual({row["source"] for row in rows}, {"zq"})
        self.assertEqual({row["quality_status"] for row in rows}, {"RECONSTRUCTED"})
        self.assertEqual(rows[0]["observed_at"], "2026-07-14T00:00:00Z")
        self.assertEqual(rows[0]["detail"]["origin"], "zq_reconstruction")
        self.assertEqual(rows[0]["detail"]["watch_date"], "2026-07-14")

    def test_repeated_import_is_idempotent_and_a_new_watch_date_adds_history(self):
        record_zq_observations(self.store, date(2026, 7, 14), self.local_rows())
        count = self.store.count_observations()
        again = record_zq_observations(self.store, date(2026, 7, 14), self.local_rows())
        self.assertEqual(self.store.count_observations(), count)
        self.assertEqual(again["counts"], {"duplicate": 2})
        moved = record_zq_observations(self.store, date(2026, 7, 15), self.local_rows())
        self.assertEqual(self.store.count_observations(), count + 2)
        self.assertTrue(all(result == "inserted" for result in moved["records"]))

    def test_zq_data_is_optional_and_never_copied(self):
        source = Path(self._tmp.name) / "zq-source"
        source.mkdir()
        write_contract(
            source, "ZQN26.csv",
            ["2026-07-13,96.4,96.5,96.3,96.5,0.1,10,5000"], symbol="ZQN26",
        )
        before = sorted(path.name for path in source.iterdir())
        contracts, _report = load_contracts(source)
        self.assertEqual(len(contracts), 1)
        record_zq_observations(
            self.store, date(2026, 7, 14),
            [row for row in self.local_rows() if row["meeting_date"] == "2026-07-29"],
        )
        self.assertEqual(sorted(path.name for path in source.iterdir()), before)
        self.assertEqual(
            sorted(path.name for path in Path(self._tmp.name).iterdir()),
            ["history.db", "zq-source"],
        )
        with self.assertRaises(ZqDataError) as caught:
            load_contracts(Path(self._tmp.name) / "missing")
        self.assertEqual(caught.exception.provider, "zq")


if __name__ == "__main__":
    unittest.main()
