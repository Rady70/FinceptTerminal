"""Fixture-based tests for scripts/cftc_data.py (Phase 4, plan section 11.1/11.3).

No network access: every test replaces CFTCDataWrapper._make_request with a
fixture shaped like the Socrata payload. What is pinned down is the data
integrity contract the COT panel depends on:

* a mapped market selects one contract by CFTC contract-market code;
* a specific market's default window reaches the weekly report instead of
  stopping at seven days;
* numeric columns Socrata returns as strings are coerced before arithmetic;
* missing/malformed/empty responses stay errors, and a missing position is
  never silently reported as zero — the legacy field names are read, the
  disaggregated family reads Producer/Merchant + Managed Money, and the TFF
  family fails closed rather than mislabelling a trader class.

Run locally:
    python -m unittest discover -s marketlab/tests -p "test_*.py" -v
The module is also registered as the CTest `marketlab_cftc_fixtures`.
"""

import copy
import csv
import io
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import types
import unittest
import urllib.parse
import zipfile
from datetime import datetime, timedelta

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPTS = os.path.abspath(os.path.join(_HERE, "..", "..", "scripts"))
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)


def _ensure_import_deps():
    """cftc_data imports requests/pandas at module load; the fixture tests
    never exercise them. Provide minimal stubs only when the packages are
    genuinely absent, so the CTest runs on a plain Python 3 install."""
    if "requests" not in sys.modules:
        try:
            import requests  # noqa: F401
        except ImportError:
            requests = types.ModuleType("requests")

            class _RequestException(Exception):
                pass

            requests.exceptions = types.SimpleNamespace(RequestException=_RequestException)
            requests.Session = lambda: types.SimpleNamespace(headers={})
            sys.modules["requests"] = requests
    if "pandas" not in sys.modules:
        try:
            import pandas  # noqa: F401
        except ImportError:
            sys.modules["pandas"] = types.ModuleType("pandas")


_ensure_import_deps()
import cftc_data  # noqa: E402


def raw_legacy_row(report_date="2026-09-01", oi="1000", comm_long="300", comm_short="100",
                   noncomm_long="400", noncomm_short="150", nonrept_long="200", nonrept_short="250",
                   market="GOLD - COMMODITY EXCHANGE INC.", code="088691"):
    """A raw Socrata legacy row with string numerics, exactly as observed."""
    return {
        "report_date_as_yyyy_mm_dd": report_date,
        "market_and_exchange_names": market,
        "commodity": "GOLD",
        "cftc_contract_market_code": code,
        "futonly_or_combined": "FutOnly",
        "open_interest_all": oi,
        "comm_positions_long_all": comm_long,
        "comm_positions_short_all": comm_short,
        "noncomm_positions_long_all": noncomm_long,
        "noncomm_positions_short_all": noncomm_short,
        "nonrept_positions_long_all": nonrept_long,
        "nonrept_positions_short_all": nonrept_short,
    }


def raw_disaggregated_row(report_date="2026-09-01", oi="2000", prod_long="500", prod_short="200",
                          money_long="700", money_short="300", nonrept_long="400", nonrept_short="450"):
    return {
        "report_date_as_yyyy_mm_dd": report_date,
        "market_and_exchange_names": "GOLD - COMMODITY EXCHANGE INC.",
        "cftc_contract_market_code": "088691",
        "futonly_or_combined": "FutOnly",
        "open_interest_all": oi,
        "prod_merc_positions_long": prod_long,
        "prod_merc_positions_short": prod_short,
        "m_money_positions_long_all": money_long,
        "m_money_positions_short_all": money_short,
        "nonrept_positions_long_all": nonrept_long,
        "nonrept_positions_short_all": nonrept_short,
    }


def raw_tff_row(report_date="2026-09-01", oi="5000", dealer_long="1000", dealer_short="900",
                asset_long="1200", asset_short="300", lev_long="800", lev_short="1400",
                other_long="250", other_short="150", nonrept_long="400", nonrept_short="900",
                market="EURO FX - CHICAGO MERCANTILE EXCHANGE", code="099741"):
    """A raw Socrata Traders-in-Financial-Futures row with string numerics."""
    return {
        "report_date_as_yyyy_mm_dd": report_date,
        "market_and_exchange_names": market,
        "contract_market_name": market,
        "cftc_contract_market_code": code,
        "commodity": "EURO FX",
        "contract_units": "EURO (125,000 EURO)",
        "futonly_or_combined": "FutOnly",
        "open_interest_all": oi,
        "dealer_positions_long_all": dealer_long,
        "dealer_positions_short_all": dealer_short,
        "asset_mgr_positions_long": asset_long,
        "asset_mgr_positions_short": asset_short,
        "lev_money_positions_long": lev_long,
        "lev_money_positions_short": lev_short,
        "other_rept_positions_long": other_long,
        "other_rept_positions_short": other_short,
        "nonrept_positions_long_all": nonrept_long,
        "nonrept_positions_short_all": nonrept_short,
    }


class CftcFixtureTest(unittest.TestCase):
    def setUp(self):
        self.wrapper = cftc_data.CFTCDataWrapper()
        self.captured_urls = []

    def _serve(self, rows):
        def fake_make_request(url):
            self.captured_urls.append(url)
            return copy.deepcopy(rows)

        self.wrapper._make_request = fake_make_request

    def test_default_window_for_named_market_reaches_weekly_report(self):
        self._serve([raw_legacy_row()])
        result = self.wrapper.get_cot_data("gold", "legacy")

        self.assertTrue(result.get("success"))
        where = urllib.parse.unquote(self.captured_urls[-1])
        self.assertIn("cftc_contract_market_code = '088691'", where)
        expected_start = (datetime.now() - timedelta(days=365)).strftime("%Y-%m-%d")
        self.assertIn(expected_start, where, "named market must not use the 7-day window")
        self.assertEqual(result["parameters"]["source"], "https://publicreporting.cftc.gov")
        self.assertEqual(result["parameters"]["report_family"], "legacy")
        self.assertIn("retrieved_at", result["parameters"])

    def test_all_identifier_keeps_short_window_and_no_contract_filter(self):
        self._serve([raw_legacy_row()])
        result = self.wrapper.get_cot_data("all", "legacy")

        self.assertTrue(result.get("success"))
        where = urllib.parse.unquote(self.captured_urls[-1])
        self.assertNotIn("cftc_contract_market_code =", where)
        expected_start = (datetime.now() - timedelta(days=7)).strftime("%Y-%m-%d")
        self.assertIn(expected_start, where)

    def test_string_numerics_are_coerced_and_missing_stays_absent(self):
        row = raw_legacy_row()
        del row["comm_positions_short_all"]  # provider omitted the cell entirely
        row["noncomm_positions_long_all"] = None
        self._serve([row])

        result = self.wrapper.get_cot_data("gold", "legacy")
        self.assertTrue(result.get("success"))
        clean = result["data"][0]
        self.assertEqual(clean["open_interest_all"], 1000)
        self.assertIsInstance(clean["open_interest_all"], int)
        self.assertNotIn("comm_positions_short_all", clean, "missing cell must not become zero")
        self.assertNotIn("noncomm_positions_long_all", clean, "null cell must not become zero")

    def test_identifier_fields_keep_leading_zero_contract_code(self):
        # "088691" is an identifier; converting it to a number would drop the
        # leading zero and change the contract identity.
        self._serve([raw_legacy_row()])
        result = self.wrapper.get_cot_data("gold", "legacy")
        clean = result["data"][0]
        self.assertEqual(clean["cftc_contract_market_code"], "088691")
        self.assertIsInstance(clean["cftc_contract_market_code"], str)
        self.assertEqual(clean["report_date_as_yyyy_mm_dd"], "2026-09-01")
        self.assertEqual(clean["commodity"], "GOLD")

    def test_empty_response_is_an_error_not_empty_success(self):
        self._serve([])
        result = self.wrapper.get_cot_data("gold", "legacy")
        self.assertNotIn("success", result)
        self.assertIn("error", result)
        self.assertIn("No COT data found", result["error"]["error"])

    def test_legacy_sentiment_reads_the_real_field_names(self):
        self._serve([raw_legacy_row()])
        result = self.wrapper.analyze_market_sentiment("gold", "legacy")

        self.assertTrue(result.get("success"), result)
        data = result["data"]
        self.assertEqual(data["commercial_positions"]["long"], 300)
        self.assertEqual(data["commercial_positions"]["short"], 100)
        self.assertEqual(data["commercial_positions"]["net"], 200)
        self.assertEqual(data["non_commercial_positions"]["long"], 400)
        self.assertEqual(data["non_commercial_positions"]["net"], 250)
        # The direction is the sign of the reported net position, never a
        # bullish/bearish sentiment label.
        self.assertEqual(data["overall_sentiment"]["commercial_bias"], "net_long")
        self.assertEqual(data["overall_sentiment"]["non_commercial_bias"], "net_long")
        serialized = json.dumps(result).lower()
        self.assertNotIn("bullish", serialized)
        self.assertNotIn("bearish", serialized)
        self.assertEqual(result["parameters"]["report_family"], "legacy")

    def test_sentiment_missing_field_fails_closed(self):
        row = raw_legacy_row()
        del row["comm_positions_short_all"]
        self._serve([row])
        result = self.wrapper.analyze_market_sentiment("gold", "legacy")

        self.assertNotIn("success", result)
        self.assertIn("commercial_short", result["error"]["error"])
        self.assertIn("missing position is not zero", result["error"]["error"])

    def test_tff_sentiment_and_trend_fail_closed(self):
        self._serve([raw_legacy_row()])
        sent = self.wrapper.analyze_market_sentiment("s&p_500", "financial")
        trend = self.wrapper.get_cot_historical_trend("s&p_500", "financial")
        summary = self.wrapper.get_position_summary("s&p_500", "financial")
        for result, view in ((sent, "sentiment"), (trend, "trend"), (summary, "summary")):
            self.assertNotIn("success", result, view)
        # The commercial/non-commercial views name the Legacy-only split; the
        # trend names the TFF classes it cannot mislabel.
        self.assertIn("Only the Legacy report", sent["error"]["error"])
        self.assertIn("Only the Legacy report", summary["error"]["error"])
        self.assertIn("Dealer/Intermediary", trend["error"]["error"])

    def test_position_summary_picks_the_latest_row_not_the_oldest(self):
        # get_cot_data orders ASC; the summary must still be the latest row.
        older = raw_legacy_row(report_date="2026-08-18", oi="900", comm_long="200", comm_short="100")
        newer = raw_legacy_row(report_date="2026-09-01", oi="1000", comm_long="300", comm_short="100")
        self._serve([older, newer])
        result = self.wrapper.get_position_summary("gold", "legacy")
        self.assertTrue(result.get("success"), result)
        self.assertEqual(result["data"]["report_date"], "2026-09-01")
        self.assertEqual(result["data"]["open_interest"], 1000)
        self.assertEqual(result["data"]["positions"]["commercial"]["net"], 200)

    def test_sentiment_unavailable_comparison_stays_unavailable(self):
        # A single row cannot support a week-over-week comparison: the change
        # must stay absent and the trend/activity that depend on it unavailable.
        self._serve([raw_legacy_row()])
        result = self.wrapper.analyze_market_sentiment("gold", "legacy")
        self.assertTrue(result.get("success"), result)
        data = result["data"]
        self.assertNotIn("change_in_oi", data)
        self.assertNotIn("oi_change_pct", data)
        self.assertEqual(data["overall_sentiment"]["oi_trend"], "unavailable")
        self.assertEqual(data["overall_sentiment"]["activity_level"], "unavailable")

    def test_sentiment_zero_oi_change_is_unchanged_not_decreasing(self):
        older = raw_legacy_row(report_date="2026-08-25", oi="1000")
        newer = raw_legacy_row(report_date="2026-09-01", oi="1000")
        self._serve([older, newer])
        result = self.wrapper.analyze_market_sentiment("gold", "legacy")
        data = result["data"]
        self.assertEqual(data["change_in_oi"], 0)
        self.assertEqual(data["overall_sentiment"]["oi_trend"], "unchanged")
        self.assertEqual(data["overall_sentiment"]["activity_level"], "low")

    def test_sentiment_zero_net_is_flat_not_short(self):
        row = raw_legacy_row(comm_long="300", comm_short="300", noncomm_long="200", noncomm_short="200")
        self._serve([row])
        result = self.wrapper.analyze_market_sentiment("gold", "legacy")
        data = result["data"]
        self.assertEqual(data["commercial_positions"]["net"], 0)
        self.assertEqual(data["non_commercial_positions"]["net"], 0)
        self.assertEqual(data["overall_sentiment"]["commercial_bias"], "flat")
        self.assertEqual(data["overall_sentiment"]["non_commercial_bias"], "flat")

    def test_disaggregated_sentiment_and_summary_fail_closed(self):
        # Producer/Merchant and Managed Money are the Disaggregated report's own
        # categories; relabelling them "commercial"/"non-commercial" would
        # reconstruct a Legacy split the report does not publish.
        self._serve([raw_disaggregated_row()])
        sentiment = self.wrapper.analyze_market_sentiment("gold", "disaggregated")
        summary = self.wrapper.get_position_summary("gold", "disaggregated")
        for result in (sentiment, summary):
            self.assertNotIn("success", result)
            self.assertIn("Only the Legacy report", result["error"]["error"])

    def test_nikkei_maps_to_the_reporting_yen_contract(self):
        # 240741 (USD Nikkei) stopped reporting on 2026-03-03; 240743 is the
        # yen-denominated contract the NIY=F price proxy trades.
        self.assertEqual(self.wrapper.cot_codes["nikkei"], "240743")
        self.assertEqual(self.wrapper._build_search_query("nikkei"),
                         "cftc_contract_market_code = '240743'")

    def test_trend_is_numeric_ordered_and_null_preserving(self):
        newer = raw_legacy_row(report_date="2026-09-01", oi="1100")
        older = raw_legacy_row(report_date="2026-08-25", oi="1000", comm_long="250",
                               comm_short=None)
        self._serve([newer, older])
        result = self.wrapper.get_cot_historical_trend("gold", "legacy", 8)

        self.assertTrue(result.get("success"), result)
        points = result["data"]
        self.assertEqual([p["date"] for p in points], ["2026-08-25", "2026-09-01"])
        self.assertEqual(points[0]["open_interest"], 1000)
        self.assertEqual(points[0]["commercial_long"], 250)
        self.assertIsNone(points[0]["commercial_short"], "a missing leg must stay null")
        self.assertIsNone(points[0]["commercial_net"], "a net with a missing leg must stay null")
        self.assertEqual(points[1]["commercial_net"], 200)
        self.assertIn("retrieved_at", result["parameters"])

    def test_disaggregated_trend_uses_family_terminology(self):
        self._serve([raw_disaggregated_row(prod_long="500", prod_short="200",
                                           money_long="700", money_short="300")])
        result = self.wrapper.get_cot_historical_trend("gold", "disaggregated", 8)

        self.assertTrue(result.get("success"), result)
        point = result["data"][0]
        # Disaggregated trend points must name Producer/Merchant and Managed
        # Money fields; reusing the legacy commercial names would mislabel the
        # very data they carry.
        self.assertEqual(point["prod_merc_long"], 500)
        self.assertEqual(point["prod_merc_short"], 200)
        self.assertEqual(point["prod_merc_net"], 300)
        self.assertEqual(point["m_money_long"], 700)
        self.assertEqual(point["m_money_short"], 300)
        self.assertEqual(point["m_money_net"], 400)
        self.assertNotIn("commercial_net", point)
        self.assertNotIn("non_commercial_net", point)

    def test_search_query_builder_mapping_and_wildcard(self):
        self.assertEqual(self.wrapper._build_search_query("all"), "")
        self.assertEqual(self.wrapper._build_search_query("gold"),
                         "cftc_contract_market_code = '088691'")
        self.assertEqual(self.wrapper._build_search_query("13874A"),
                         "cftc_contract_market_code = '13874A'")
        wildcard = self.wrapper._build_search_query("some market")
        self.assertIn("like", wildcard)
        self.assertNotIn("cftc_contract_market_code =", wildcard)

    def test_malformed_market_row_keeps_parser_from_inventing_a_number(self):
        row = raw_legacy_row(oi="not-a-number")
        row["comm_positions_long_all"] = "300x"
        self._serve([row])
        result = self.wrapper.get_cot_data("gold", "legacy")
        clean = result["data"][0]
        self.assertEqual(clean["open_interest_all"], "not-a-number")
        self.assertEqual(clean["comm_positions_long_all"], "300x")

    # ── R3 workspace history (cot_history) ────────────────────────────────────

    def test_cot_history_selects_combined_dataset_and_has_no_date_bound(self):
        self._serve([raw_legacy_row()])
        result = self.wrapper.get_cot_history("gold", "legacy", futures_only=False)

        self.assertTrue(result.get("success"), result)
        url = urllib.parse.unquote(self.captured_urls[-1])
        self.assertIn("/resource/jun7-fc8e.json", url)
        self.assertIn("cftc_contract_market_code = '088691'", url)
        self.assertNotIn("between", url.lower(), "full history must not carry a date bound")
        self.assertIn("$order=report_date_as_yyyy_mm_dd,id", url)
        params = result["parameters"]
        self.assertEqual(params["report_family"], "legacy")
        self.assertEqual(params["dataset"], "jun7-fc8e")
        self.assertEqual(params["count"], 1)
        self.assertEqual(params["first_report_date"], "2026-09-01")
        self.assertEqual(params["last_report_date"], "2026-09-01")

    def test_cot_history_futures_only_selects_futures_only_dataset(self):
        self._serve([raw_legacy_row()])
        result = self.wrapper.get_cot_history("gold", "legacy", futures_only=True)

        self.assertTrue(result.get("success"), result)
        self.assertIn("/resource/6dca-aqww.json", urllib.parse.unquote(self.captured_urls[-1]))
        self.assertTrue(result["parameters"]["futures_only"])

    def test_cot_history_emits_exact_participant_fields_and_metadata(self):
        self._serve([raw_legacy_row()])
        row = self.wrapper.get_cot_history("gold", "legacy")["data"][0]

        self.assertEqual(row["commercial_long"], 300)
        self.assertEqual(row["commercial_short"], 100)
        self.assertEqual(row["non_commercial_long"], 400)
        self.assertEqual(row["non_commercial_short"], 150)
        self.assertEqual(row["non_reportable_long"], 200)
        self.assertEqual(row["non_reportable_short"], 250)
        self.assertEqual(row["open_interest_all"], 1000)
        self.assertIsInstance(row["commercial_long"], int)
        self.assertEqual(row["cftc_contract_market_code"], "088691")
        self.assertEqual(row["market_and_exchange_names"], "GOLD - COMMODITY EXCHANGE INC.")
        # The raw provider field names must not leak into the normalized keys.
        self.assertNotIn("comm_positions_long_all", row)

    def test_cot_history_disaggregated_reads_the_double_underscore_swap_short(self):
        row = raw_disaggregated_row()
        row["swap_positions_long_all"] = "111"
        row["swap__positions_short_all"] = "222"
        row["other_rept_positions_long"] = "333"
        row["other_rept_positions_short"] = "444"
        self._serve([row])

        result = self.wrapper.get_cot_history("gold", "disaggregated")
        self.assertTrue(result.get("success"), result)
        point = result["data"][0]
        self.assertEqual(point["producer_merchant_long"], 500)
        self.assertEqual(point["swap_dealer_long"], 111)
        self.assertEqual(point["swap_dealer_short"], 222,
                         "the real provider field is swap__positions_short_all")
        self.assertEqual(point["managed_money_long"], 700)
        self.assertEqual(point["other_reportable_long"], 333)
        self.assertEqual(point["non_reportable_short"], 450)

    def test_cot_history_tff_uses_financial_trader_classes(self):
        self._serve([raw_tff_row()])
        result = self.wrapper.get_cot_history("euro", "financial", futures_only=True)

        self.assertTrue(result.get("success"), result)
        self.assertIn("/resource/gpe5-46if.json", urllib.parse.unquote(self.captured_urls[-1]))
        point = result["data"][0]
        self.assertEqual(point["dealer_long"], 1000)
        self.assertEqual(point["dealer_short"], 900)
        self.assertEqual(point["asset_manager_long"], 1200)
        self.assertEqual(point["leveraged_funds_short"], 1400)
        self.assertEqual(point["other_reportable_long"], 250)
        self.assertEqual(point["non_reportable_short"], 900)
        # TFF must never be relabelled as legacy commercial/non-commercial.
        self.assertNotIn("commercial_long", point)
        self.assertNotIn("managed_money_long", point)

    def test_cot_history_missing_cell_stays_null(self):
        row = raw_legacy_row()
        del row["comm_positions_short_all"]
        self._serve([row])
        point = self.wrapper.get_cot_history("gold", "legacy")["data"][0]

        self.assertIsNone(point["commercial_short"], "a missing leg must stay null, never 0")
        self.assertEqual(point["commercial_long"], 300)

    def test_cot_history_retains_trader_counts_and_concentration(self):
        # Field names verified against all six authoritative Socrata resources
        # (legacy/disaggregated/TFF, combined and futures-only). Counts are
        # whole traders; concentration cells are decimals and must not be
        # truncated to ints.
        row = raw_legacy_row()
        row["traders_tot_all"] = "208"
        row["traders_tot_rept_long_all"] = "65"
        row["traders_tot_rept_short_all"] = 70
        row["conc_gross_le_4_tdr_long"] = "12.5"
        row["conc_gross_le_4_tdr_short"] = "19.3"
        row["conc_gross_le_8_tdr_long"] = 23.1
        row["conc_gross_le_8_tdr_short"] = "30.3"
        row["conc_net_le_4_tdr_long_all"] = "12.5"
        row["conc_net_le_4_tdr_short_all"] = 18.6
        row["conc_net_le_8_tdr_long_all"] = "21.7"
        row["conc_net_le_8_tdr_short_all"] = 27.1
        self._serve([row])

        point = self.wrapper.get_cot_history("gold", "legacy")["data"][0]
        self.assertEqual(point["traders_total"], 208)
        self.assertIsInstance(point["traders_total"], int)
        self.assertEqual(point["traders_reportable_long"], 65)
        self.assertEqual(point["traders_reportable_short"], 70)
        self.assertEqual(point["concentration_gross_4_long"], 12.5)
        self.assertIsInstance(point["concentration_gross_4_long"], float)
        self.assertEqual(point["concentration_gross_4_short"], 19.3)
        self.assertEqual(point["concentration_gross_8_long"], 23.1)
        self.assertEqual(point["concentration_gross_8_short"], 30.3)
        self.assertEqual(point["concentration_net_4_short"], 18.6)
        self.assertEqual(point["concentration_net_8_long"], 21.7)
        self.assertEqual(point["concentration_net_8_short"], 27.1)

    def test_cot_history_trader_context_missing_or_junk_stays_null(self):
        row = raw_legacy_row()
        row["traders_tot_all"] = None
        row["conc_gross_le_4_tdr_long"] = "not-a-number"
        row["conc_gross_le_4_tdr_short"] = "nan"
        row["conc_gross_le_8_tdr_long"] = float("inf")
        self._serve([row])
        point = self.wrapper.get_cot_history("gold", "legacy")["data"][0]

        self.assertIsNone(point["traders_total"], "a null count is not zero")
        self.assertIsNone(point["concentration_gross_4_long"], "junk stays absent")
        self.assertIsNone(point["concentration_gross_4_short"], "nan is not a measurement")
        self.assertIsNone(point["concentration_gross_8_long"], "infinity is not a measurement")
        self.assertIsNone(point["concentration_gross_8_short"], "an absent cell stays null")
        self.assertIsNone(point["concentration_net_4_long"])

    def test_cot_history_sorts_ascending_and_paginates_offsets(self):
        rows = [raw_legacy_row(report_date="2026-09-01"),
                raw_legacy_row(report_date="2026-08-25"),
                raw_legacy_row(report_date="2026-09-08")]
        pages = []

        def fake_make_request(url):
            parsed = urllib.parse.urlparse(url)
            query = urllib.parse.parse_qs(parsed.query)
            limit = int(query["$limit"][0])
            offset = int(query["$offset"][0])
            pages.append((offset, limit))
            return copy.deepcopy(rows[offset:offset + limit])

        self.wrapper._make_request = fake_make_request
        result = self.wrapper.get_cot_history("gold", "legacy", page_size=1)

        self.assertTrue(result.get("success"), result)
        self.assertEqual([p["report_date_as_yyyy_mm_dd"] for p in result["data"]],
                         ["2026-08-25", "2026-09-01", "2026-09-08"])
        self.assertEqual(pages[0], (0, 1))
        self.assertEqual(pages[1], (1, 1))
        self.assertEqual(pages[2], (2, 1))

    def test_cot_history_cap_is_an_error_not_a_truncated_success(self):
        self._serve([raw_legacy_row(report_date="2026-08-25"),
                     raw_legacy_row(report_date="2026-09-01")])
        result = self.wrapper.get_cot_history("gold", "legacy", max_rows=1)

        self.assertNotIn("success", result)
        self.assertIn("cap", result["error"]["error"])

    def test_cot_history_empty_response_is_an_error(self):
        self._serve([])
        result = self.wrapper.get_cot_history("gold", "legacy")

        self.assertNotIn("success", result)
        self.assertIn("No COT history found", result["error"]["error"])

    def test_cot_history_rejects_invalid_family_and_market_sweep(self):
        self._serve([raw_legacy_row()])
        bad_family = self.wrapper.get_cot_history("gold", "supplemental")
        sweep = self.wrapper.get_cot_history("all", "legacy")
        for result in (bad_family, sweep):
            self.assertNotIn("success", result)
            self.assertIn("error", result)


ANNUAL_LEGACY_HEADER = [
    "Market and Exchange Names",
    "As of Date in Form YYMMDD",
    "As of Date in Form YYYY-MM-DD",
    "CFTC Contract Market Code",
    "Open Interest (All)",
    "Noncommercial Positions-Long (All)",
    "Noncommercial Positions-Short (All)",
    "Noncommercial Positions-Spreading (All)",
    "Commercial Positions-Long (All)",
    "Commercial Positions-Short (All)",
    "Nonreportable Positions-Long (All)",
    "Nonreportable Positions-Short (All)",
    "Traders-Total (All)",
    "Traders-Total Reportable-Long (All)",
    "Traders-Total Reportable-Short (All)",
    "Traders-Noncommercial-Long (All)",
    "Traders-Noncommercial-Short (All)",
    "Traders-Noncommercial-Spreading (All)",
    "Traders-Commercial-Long (All)",
    "Traders-Commercial-Short (All)",
    "Concentration-Gross LT = 4 TDR-Long (All)",
    "Concentration-Gross LT =4 TDR-Short (All)",
    "Concentration-Gross LT =8 TDR-Long (All)",
    "Concentration-Gross LT =8 TDR-Short (All)",
    "Concentration-Net LT =4 TDR-Long (All)",
    "Concentration-Net LT =4 TDR-Short (All)",
    "Concentration-Net LT =8 TDR-Long (All)",
    "Concentration-Net LT =8 TDR-Short (All)",
    "Contract Units",
]

ANNUAL_DISAGG_HEADER = [
    "Market_and_Exchange_Names",
    "As_of_Date_In_Form_YYMMDD",
    "Report_Date_as_MM_DD_YYYY",
    "CFTC_Contract_Market_Code",
    "Open_Interest_All",
    "Prod_Merc_Positions_Long_All",
    "Prod_Merc_Positions_Short_All",
    "Swap_Positions_Long_All",
    "Swap__Positions_Short_All",
    "Swap__Positions_Spread_All",
    "M_Money_Positions_Long_All",
    "M_Money_Positions_Short_All",
    "M_Money_Positions_Spread_All",
    "Other_Rept_Positions_Long_All",
    "Other_Rept_Positions_Short_All",
    "Other_Rept_Positions_Spread_All",
    "NonRept_Positions_Long_All",
    "NonRept_Positions_Short_All",
    "Traders_Tot_All",
    "Traders_Tot_Rept_Long_All",
    "Traders_Tot_Rept_Short_All",
    "Traders_Prod_Merc_Long_All",
    "Traders_Prod_Merc_Short_All",
    "Traders_Swap_Long_All",
    "Traders_Swap_Short_All",
    "Traders_Swap_Spread_All",
    "Traders_M_Money_Long_All",
    "Traders_M_Money_Short_All",
    "Traders_M_Money_Spread_All",
    "Traders_Other_Rept_Long_All",
    "Traders_Other_Rept_Short_All",
    "Traders_Other_Rept_Spread_All",
    "Conc_Gross_LE_4_TDR_Long_All",
    "Contract_Units",
]

ANNUAL_TFF_HEADER = [
    "Market_and_Exchange_Names",
    "As_of_Date_In_Form_YYMMDD",
    "Report_Date_as_YYYY-MM-DD",
    "CFTC_Contract_Market_Code",
    "Open_Interest_All",
    "Dealer_Positions_Long_All",
    "Dealer_Positions_Short_All",
    "Dealer_Positions_Spread_All",
    "Asset_Mgr_Positions_Long_All",
    "Asset_Mgr_Positions_Short_All",
    "Asset_Mgr_Positions_Spread_All",
    "Lev_Money_Positions_Long_All",
    "Lev_Money_Positions_Short_All",
    "Lev_Money_Positions_Spread_All",
    "Other_Rept_Positions_Long_All",
    "Other_Rept_Positions_Short_All",
    "Other_Rept_Positions_Spread_All",
    "NonRept_Positions_Long_All",
    "NonRept_Positions_Short_All",
    "Traders_Tot_All",
    "Traders_Tot_Rept_Long_All",
    "Traders_Tot_Rept_Short_All",
    "Traders_Dealer_Long_All",
    "Traders_Dealer_Short_All",
    "Traders_Dealer_Spread_All",
    "Traders_Asset_Mgr_Long_All",
    "Traders_Asset_Mgr_Short_All",
    "Traders_Asset_Mgr_Spread_All",
    "Traders_Lev_Money_Long_All",
    "Traders_Lev_Money_Short_All",
    "Traders_Lev_Money_Spread_All",
    "Traders_Other_Rept_Long_All",
    "Traders_Other_Rept_Short_All",
    "Traders_Other_Rept_Spread_All",
    "Conc_Net_LE_4_TDR_Long_All",
    "Contract_Units",
]

ANNUAL_FILES = {
    "legacy": ("annual.txt", ANNUAL_LEGACY_HEADER),
    "disaggregated": ("f_year.txt", ANNUAL_DISAGG_HEADER),
    "tff": ("FinFutYY.txt", ANNUAL_TFF_HEADER),
}

_CATALOG_HEADER = os.path.abspath(
    os.path.join(_HERE, "..", "..", "src", "services", "economics", "CftcMarketCatalog.h"))


def annual_zip(family, rows, entry=None, header=None):
    """Build an official-format annual ZIP in memory from a list of records.

    `rows` are dicts keyed by the exact header names; unknown columns are left
    empty so a test can exercise missing-cell behavior explicitly.
    """
    entry_name, default_header = ANNUAL_FILES[family]
    header = header or default_header
    buffer = io.BytesIO()
    text = io.StringIO()
    writer = csv.writer(text)
    writer.writerow(header)
    for record in rows:
        writer.writerow([record.get(column, "") for column in header])
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr(entry or entry_name, text.getvalue())
    return buffer.getvalue()


def legacy_annual_record(report_date="2024-06-25", code="088691", oi="452190",
                         market="GOLD - COMMODITY EXCHANGE INC."):
    return {
        "Market and Exchange Names": market,
        "As of Date in Form YYMMDD": "240625",
        "As of Date in Form YYYY-MM-DD": report_date,
        "CFTC Contract Market Code": code,
        "Open Interest (All)": oi,
        "Noncommercial Positions-Long (All)": "284885",
        "Noncommercial Positions-Short (All)": "38656",
        "Commercial Positions-Long (All)": "86551",
        "Commercial Positions-Short (All)": "358039",
        "Nonreportable Positions-Long (All)": "48436",
        "Nonreportable Positions-Short (All)": "23177",
        "Traders-Total (All)": "300",
        "Traders-Total Reportable-Long (All)": "243",
        "Traders-Total Reportable-Short (All)": "158",
        "Concentration-Gross LT = 4 TDR-Long (All)": "17.7",
        "Concentration-Gross LT =4 TDR-Short (All)": "42.6",
        "Concentration-Gross LT =8 TDR-Long (All)": "27.9",
        "Concentration-Gross LT =8 TDR-Short (All)": "58.7",
        "Concentration-Net LT =4 TDR-Long (All)": "17.5",
        "Concentration-Net LT =4 TDR-Short (All)": "40.3",
        "Concentration-Net LT =8 TDR-Long (All)": "26.5",
        "Concentration-Net LT =8 TDR-Short (All)": "56.4",
        "Contract Units": "(CONTRACTS OF 100 TROY OUNCES)",
    }


class CftcBackfillMonitorTest(unittest.TestCase):
    """Batch 5: the annual-ZIP archive, canonical convergence and the monitor.

    No network access: the annual download and the monitor's per-market Socrata
    page are replaced with fixtures.
    """

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cftc-cot-batch5-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.wrapper = cftc_data.CFTCDataWrapper(archive_dir=self.tmp)
        self.captured_urls = []

    # ── helpers ─────────────────────────────────────────────────────────────

    def _serve_annual(self, payload):
        def fake_download(url, timeout=180):
            self.captured_urls.append(url)
            return payload
        self.captured_urls = []
        self.wrapper._download_annual = fake_download

    def _serve_monitor(self, rows, fail=None):
        def fake_get(url):
            self.captured_urls.append(url)
            if fail is not None:
                raise RuntimeError(fail)
            return copy.deepcopy(rows)
        self.wrapper._monitor_get = fake_get

    def _archive_rows(self):
        conn = sqlite3.connect(os.path.join(self.tmp, "cot_archive.db"))
        conn.row_factory = sqlite3.Row
        try:
            return [dict(row) for row in conn.execute(
                "SELECT report_date, contract_code, report_family, report_basis, row_json, "
                "first_source, last_source FROM cot_observations ORDER BY report_date")]
        finally:
            conn.close()

    def _monitor_rows(self, rows):
        """Merge legacy Socrata rows through the fixture stubbed page."""
        self._serve_monitor(rows)
        result = self.wrapper.get_cot_monitor("legacy", True, ["gold"], 25000, refresh=True)
        self.assertTrue(result.get("success"), result)
        return result["data"]["markets"][0]

    # ── annual backfill ─────────────────────────────────────────────────────

    def test_annual_legacy_zip_maps_to_canonical_rows_exactly(self):
        micro = legacy_annual_record(code="999999", market="MICRO GOLD - COMEX")
        corn = legacy_annual_record(code="002602", market="CORN - CBOT")
        malformed = legacy_annual_record(report_date="not-a-date")
        missing_cell = legacy_annual_record(report_date="2024-06-18")
        missing_cell.pop("Nonreportable Positions-Long (All)")
        self._serve_annual(annual_zip(
            "legacy", [legacy_annual_record(), micro, corn, malformed, missing_cell]))

        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        self.assertTrue(result.get("success"), result)
        self.assertEqual(result["data"]["years"][0]["status"], "ok")
        self.assertEqual(result["data"]["rows_inserted"], 2)
        self.assertEqual(result["data"]["rows_rejected"], 1)
        # Rows for other CFTC contracts (micro gold) and for curated markets
        # outside the requested subset (corn) are filtered, not ingested.
        self.assertEqual(result["data"]["years"][0]["rows_filtered"], 2)
        rows = self._archive_rows()
        self.assertEqual([row["contract_code"] for row in rows], ["088691", "088691"])
        self.assertEqual([row["report_date"] for row in rows], ["2024-06-18", "2024-06-25"])
        first = json.loads(rows[0]["row_json"])
        self.assertIsNone(first["non_reportable_long"])
        self.assertEqual(first["futonly_or_combined"], "FutOnly")
        self.assertEqual(first["open_interest_all"], 452190)
        self.assertEqual(first["concentration_gross_4_long"], 17.7)
        self.assertTrue(rows[0]["first_source"].startswith("cftc-annual:https://www.cftc.gov/"))

    def test_annual_disaggregated_and_tff_headers_are_supported(self):
        disagg = annual_zip("disaggregated", [{
            "Market_and_Exchange_Names": "GOLD - COMMODITY EXCHANGE INC.",
            "As_of_Date_In_Form_YYMMDD": "100601",
            "Report_Date_as_MM_DD_YYYY": "06/01/2010",
            "CFTC_Contract_Market_Code": "088691",
            "Open_Interest_All": "1000",
            "Prod_Merc_Positions_Long_All": "300",
            "Prod_Merc_Positions_Short_All": "100",
            "Swap_Positions_Long_All": "100",
            "Swap__Positions_Short_All": "50",
            "M_Money_Positions_Long_All": "400",
            "M_Money_Positions_Short_All": "200",
            "Other_Rept_Positions_Long_All": "50",
            "Other_Rept_Positions_Short_All": "50",
            "NonRept_Positions_Long_All": "150",
            "NonRept_Positions_Short_All": "200",
            "Traders_Tot_All": "100",
            "Traders_Tot_Rept_Long_All": "80",
            "Traders_Tot_Rept_Short_All": "70",
            "Swap__Positions_Spread_All": "40",
            "M_Money_Positions_Spread_All": "60",
            "Traders_Swap_Spread_All": "5",
            "Traders_M_Money_Spread_All": "7",
            "Conc_Gross_LE_4_TDR_Long_All": "20.0",
            "Contract_Units": "OUNCES",
        }])
        self._serve_annual(disagg)
        result = self.wrapper.cot_backfill("disaggregated", True, 2010, 2010, ["gold"])
        self.assertTrue(result.get("success"), result)
        rows = self._archive_rows()
        self.assertEqual(len(rows), 1)
        row = json.loads(rows[0]["row_json"])
        self.assertEqual(row["report_date_as_yyyy_mm_dd"], "2010-06-01")
        self.assertEqual(row["producer_merchant_long"], 300)
        self.assertEqual(row["swap_dealer_long"], 100)
        self.assertEqual(row["swap_dealer_short"], 50)
        self.assertEqual(row["managed_money_long"], 400)
        self.assertEqual(row["other_reportable_long"], 50)
        self.assertEqual(row["non_reportable_long"], 150)
        self.assertEqual(row["swap_dealer_spread"], 40)
        self.assertEqual(row["managed_money_spread"], 60)
        self.assertEqual(row["swap_dealer_traders_spread"], 5)
        self.assertEqual(row["managed_money_traders_spread"], 7)
        # Disaggregated rows are never relabelled into the legacy classes.
        self.assertNotIn("commercial_long", row)

    def test_annual_tff_zip_maps_to_tff_participants(self):
        tff = annual_zip("tff", [{
            "Market_and_Exchange_Names": "EURO FX - CHICAGO MERCANTILE EXCHANGE",
            "As_of_Date_In_Form_YYMMDD": "240625",
            "Report_Date_as_YYYY-MM-DD": "2024-06-25",
            "CFTC_Contract_Market_Code": "099741",
            "Open_Interest_All": "5000",
            "Dealer_Positions_Long_All": "1000",
            "Dealer_Positions_Short_All": "900",
            "Asset_Mgr_Positions_Long_All": "1200",
            "Asset_Mgr_Positions_Short_All": "300",
            "Lev_Money_Positions_Long_All": "800",
            "Lev_Money_Positions_Short_All": "1400",
            "Other_Rept_Positions_Long_All": "250",
            "Other_Rept_Positions_Short_All": "150",
            "NonRept_Positions_Long_All": "400",
            "NonRept_Positions_Short_All": "900",
            "Traders_Tot_All": "140",
            "Traders_Tot_Rept_Long_All": "100",
            "Traders_Tot_Rept_Short_All": "90",
            "Dealer_Positions_Spread_All": "11",
            "Traders_Lev_Money_Spread_All": "9",
            "Conc_Net_LE_4_TDR_Long_All": "35.0",
            "Contract_Units": "EURO (125,000 EURO)",
        }])
        self._serve_annual(tff)
        result = self.wrapper.cot_backfill("tff", True, 2024, 2024, ["euro"])
        self.assertTrue(result.get("success"), result)
        rows = self._archive_rows()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["report_family"], "tff")
        self.assertEqual(rows[0]["report_basis"], "futures_only")
        row = json.loads(rows[0]["row_json"])
        self.assertEqual(row["leveraged_funds_short"], 1400)
        self.assertEqual(row["dealer_spread"], 11)
        self.assertEqual(row["leveraged_funds_traders_spread"], 9)
        self.assertEqual(row["futonly_or_combined"], "FutOnly")
        # TFF is never reconstructed into legacy commercial/non-commercial.
        self.assertNotIn("commercial_long", row)

    def test_repeated_backfill_is_idempotent_and_adopts_revisions(self):
        self._serve_annual(annual_zip("legacy", [legacy_annual_record()]))
        first = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        second = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        self.assertEqual(first["data"]["rows_inserted"], 1)
        self.assertEqual(second["data"]["rows_inserted"], 0)
        self.assertEqual(second["data"]["rows_updated"], 0)
        self.assertEqual(len(self._archive_rows()), 1)

        revised = legacy_annual_record(oi="999999")
        self._serve_annual(annual_zip("legacy", [revised]))
        third = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        self.assertEqual(third["data"]["rows_inserted"], 0)
        self.assertEqual(third["data"]["rows_updated"], 1)
        rows = self._archive_rows()
        self.assertEqual(len(rows), 1)
        self.assertEqual(json.loads(rows[0]["row_json"])["open_interest_all"], 999999)
        # The first writer's provenance is retained; the latest writer is tracked.
        self.assertIn("first_source", rows[0])

    def test_backfill_year_without_an_official_file_is_reported(self):
        def not_published(url, timeout=180):
            raise cftc_data.CFTCAnnualNotPublished(url)
        self.wrapper._download_annual = not_published
        result = self.wrapper.cot_backfill("disaggregated", True, 2010, 2010, ["gold"])
        self.assertTrue(result.get("success"), result)
        self.assertEqual(result["data"]["years"][0]["status"], "not_published")
        self.assertEqual(result["data"]["skipped_years"], [2010])
        self.assertEqual(self._archive_rows(), [])

    def test_combined_annual_urls_and_entries_are_exact(self):
        combined_legacy = annual_zip(
            "legacy", [legacy_annual_record()], entry="annualof.txt")
        self._serve_annual(combined_legacy)
        result = self.wrapper.cot_backfill("legacy", False, 2024, 2024, ["gold"])
        self.assertTrue(result.get("success"), result)
        self.assertIn("deahistfo2024.zip", self.captured_urls[-1])
        row = json.loads(self._archive_rows()[0]["row_json"])
        self.assertEqual(row["futonly_or_combined"], "Combined")

        disagg = annual_zip("disaggregated", [{
            "Market_and_Exchange_Names": "GOLD - COMMODITY EXCHANGE INC.",
            "As_of_Date_In_Form_YYMMDD": "240625",
            "Report_Date_as_MM_DD_YYYY": "06/25/2024",
            "CFTC_Contract_Market_Code": "088691",
            "Open_Interest_All": "1000",
            "Prod_Merc_Positions_Long_All": "300",
            "Prod_Merc_Positions_Short_All": "100",
            "Swap_Positions_Long_All": "100",
            "Swap__Positions_Short_All": "50",
            "M_Money_Positions_Long_All": "400",
            "M_Money_Positions_Short_All": "200",
            "Other_Rept_Positions_Long_All": "50",
            "Other_Rept_Positions_Short_All": "50",
            "NonRept_Positions_Long_All": "150",
            "NonRept_Positions_Short_All": "200",
            "Traders_Tot_All": "100",
            "Traders_Tot_Rept_Long_All": "80",
            "Traders_Tot_Rept_Short_All": "70",
            "Conc_Gross_LE_4_TDR_Long_All": "20.0",
            "Contract_Units": "OUNCES",
        }], entry="c_year.txt")
        self._serve_annual(disagg)
        disagg_result = self.wrapper.cot_backfill("disaggregated", False, 2024, 2024, ["gold"])
        self.assertTrue(disagg_result.get("success"), disagg_result)
        self.assertIn("com_disagg_txt_2024.zip", self.captured_urls[0])

        tff = annual_zip("tff", [{
            "Market_and_Exchange_Names": "EURO FX - CHICAGO MERCANTILE EXCHANGE",
            "As_of_Date_In_Form_YYMMDD": "240625",
            "Report_Date_as_YYYY-MM-DD": "2024-06-25",
            "CFTC_Contract_Market_Code": "099741",
            "Open_Interest_All": "5000",
            "Dealer_Positions_Long_All": "1000",
            "Dealer_Positions_Short_All": "900",
            "Asset_Mgr_Positions_Long_All": "1200",
            "Asset_Mgr_Positions_Short_All": "300",
            "Lev_Money_Positions_Long_All": "800",
            "Lev_Money_Positions_Short_All": "1400",
            "Other_Rept_Positions_Long_All": "250",
            "Other_Rept_Positions_Short_All": "150",
            "NonRept_Positions_Long_All": "400",
            "NonRept_Positions_Short_All": "900",
            "Traders_Tot_All": "140",
            "Traders_Tot_Rept_Long_All": "100",
            "Traders_Tot_Rept_Short_All": "90",
            "Conc_Net_LE_4_TDR_Long_All": "35.0",
            "Contract_Units": "EURO (125,000 EURO)",
        }], entry="FinComYY.txt")
        self._serve_annual(tff)
        tff_result = self.wrapper.cot_backfill("tff", False, 2024, 2024, ["euro"])
        self.assertTrue(tff_result.get("success"), tff_result)
        self.assertIn("com_fin_txt_2024.zip", self.captured_urls[0])

    def test_cross_path_metadata_does_not_churn_the_archive(self):
        # The annual path stores null display names; the Socrata path carries
        # commodity/contract_market_name. A monitor refresh with identical
        # analytical values must count as unchanged and keep the first writer's
        # stored row and provenance.
        self._serve_annual(annual_zip("legacy", [legacy_annual_record(report_date="2024-06-25")]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        matching_socrata = raw_legacy_row(
            report_date="2024-06-25", oi="452190", comm_long="86551", comm_short="358039",
            noncomm_long="284885", noncomm_short="38656", nonrept_long="48436", nonrept_short="23177")
        matching_socrata.update({
            "contract_units": "(CONTRACTS OF 100 TROY OUNCES)",
            "contract_market_name": "GOLD",
            "traders_tot_all": "300",
            "traders_tot_rept_long_all": "243",
            "traders_tot_rept_short_all": "158",
            "conc_gross_le_4_tdr_long": "17.7",
            "conc_gross_le_4_tdr_short": "42.6",
            "conc_gross_le_8_tdr_long": "27.9",
            "conc_gross_le_8_tdr_short": "58.7",
            "conc_net_le_4_tdr_long_all": "17.5",
            "conc_net_le_4_tdr_short_all": "40.3",
            "conc_net_le_8_tdr_long_all": "26.5",
            "conc_net_le_8_tdr_short_all": "56.4",
        })
        self._serve_monitor([matching_socrata])
        market = self._monitor_rows([matching_socrata])
        self.assertEqual(market["inserted"], 0)
        self.assertEqual(market["updated"], 0)
        self.assertEqual(len(self._archive_rows()), 1)
        stored = self._archive_rows()[0]
        self.assertEqual(json.loads(stored["row_json"])["commodity"], None)
        self.assertTrue(stored["first_source"].startswith("cftc-annual:"))

    def test_sparse_recent_history_expands_the_transport_window(self):
        rows = []
        report_date = datetime(2019, 1, 1)
        for i in range(400):
            row = raw_legacy_row(report_date=report_date.strftime("%Y-%m-%d"))
            row["conc_gross_le_4_tdr_long"] = str(10.0 + 0.02 * i)
            if i % 2 == 1 and i >= 200:
                # The most recent 200 reports carry every other market's legs
                # missing; only the older dense block holds the full reference.
                row["noncomm_positions_long_all"] = None
                row["noncomm_positions_short_all"] = None
            rows.append(row)
            report_date += timedelta(days=7)
        market = self._monitor_rows(rows)
        # 200 rows would leave only 100 valid points; the window must expand
        # past 200 so every trailing engine reference is exactly reproducible.
        self.assertGreater(len(market["rows"]), 200)
        self.assertLessEqual(len(market["rows"]), 400)

    def test_transport_window_covers_disjoint_sparse_references(self):
        # Adversarial sparsity: in the last 400 reports of 1000, leg-valid rows
        # and Open-Interest-valid rows are disjoint. A window that only counts
        # legs would declare itself sufficient while the engine's Net %OI
        # reference is under-populated.
        rows = []
        report_date = datetime(2014, 1, 7)
        for i in range(1000):
            row = raw_legacy_row(report_date=report_date.strftime("%Y-%m-%d"))
            row["conc_gross_le_4_tdr_long"] = str(10.0 + 0.01 * i)
            if i >= 600 and i % 2 == 1:
                row["noncomm_positions_long_all"] = None
                row["noncomm_positions_short_all"] = None
            if i >= 600 and i % 2 == 0:
                row["open_interest_all"] = None
            rows.append(row)
            report_date += timedelta(days=7)
        market = self._monitor_rows(rows)
        recent = market["rows"]
        valid_points = [row for row in recent
                        if row.get("open_interest_all") and row.get("non_commercial_long") is not None]
        # Either the whole history is carried, or the window carries at least
        # the 157 valid Net %OI points the engine's reference needs.
        self.assertTrue(len(recent) == 1000 or len(valid_points) >= 157)

    def test_duplicate_provider_rows_fail_the_market_not_the_scan(self):
        duplicate = raw_legacy_row(report_date="2026-09-01")
        second = raw_legacy_row(report_date="2026-09-01", oi="2000")
        self._serve_monitor([duplicate, second])
        result = self.wrapper.get_cot_monitor("legacy", True, ["gold"], 25000, refresh=True)
        self.assertTrue(result.get("success"), result)
        market = result["data"]["markets"][0]
        self.assertIn(market["status"], ("unavailable", "archive_only"))
        self.assertIn("two rows for the same report date", market["refresh_error"])

    def test_archive_upsert_collapses_identical_duplicates_and_rejects_conflicts(self):
        archive = self.wrapper._open_archive()
        row = {
            "report_date_as_yyyy_mm_dd": "2024-06-25",
            "cftc_contract_market_code": "088691",
            "open_interest_all": 100,
        }
        inserted, updated, _unchanged = archive.upsert_observations(
            [row, dict(row)], "legacy", "futures_only", "test", "2024-06-26T00:00:00+00:00")
        self.assertEqual((inserted, updated), (1, 0))
        conflicting = dict(row)
        conflicting["open_interest_all"] = 999
        with self.assertRaises(ValueError):
            archive.upsert_observations(
                [row, conflicting], "legacy", "futures_only", "test", "2024-06-26T00:00:00+00:00")

    def test_provider_empty_read_keeps_stored_history_current(self):
        # An empty full read for a contract whose archive already holds rows
        # (for example a delisted market) is not a fabricated success: the
        # stored history is shown as current, and the engine's report-age
        # freshness flag marks a genuinely stale contract as outdated.
        self._serve_annual(annual_zip("legacy", [legacy_annual_record(report_date="2024-06-25")]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        self._serve_monitor([])
        result = self.wrapper.get_cot_monitor("legacy", True, ["gold"], 25000, refresh=True)
        market = result["data"]["markets"][0]
        self.assertEqual(market["status"], "current")
        self.assertEqual(market["refresh_error"], "")
        self.assertEqual(len(market["rows"]), 1)

    def test_backfill_rejects_years_before_the_family_starts(self):
        result = self.wrapper.cot_backfill("disaggregated", True, 2005, 2005, ["gold"])
        self.assertNotIn("success", result)
        self.assertIn("begin in 2010", result["error"]["error"])

    def test_malformed_annual_data_fails_closed_without_writing(self):
        for payload, expected in (
                (b"not a zip", "not a ZIP"),
                (annual_zip("legacy", [legacy_annual_record()], header=["wrong", "columns"]), "missing required")):
            self._serve_annual(payload)
            result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
            self.assertTrue(result.get("success"), result)
            self.assertEqual(result["data"]["years"][0]["status"], "malformed")
            self.assertIn(expected, result["data"]["years"][0]["error"])
            self.assertEqual(self._archive_rows(), [])

    def test_conflicting_annual_duplicates_fail_the_year_without_writing(self):
        first = legacy_annual_record(report_date="2024-06-25", oi="452190")
        conflicting = legacy_annual_record(report_date="2024-06-25", oi="999999")
        self._serve_annual(annual_zip("legacy", [first, conflicting]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        self.assertTrue(result.get("success"), result)
        year = result["data"]["years"][0]
        self.assertEqual(year["status"], "malformed")
        self.assertIn("conflicting duplicate rows", year["error"])
        self.assertEqual(self._archive_rows(), [])

    def test_identical_annual_duplicates_collapse_and_are_counted(self):
        duplicate = legacy_annual_record(report_date="2024-06-25")
        self._serve_annual(annual_zip("legacy", [duplicate, dict(duplicate)]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        self.assertEqual(result["data"]["years"][0]["status"], "ok")
        self.assertEqual(result["data"]["rows_inserted"], 1)
        self.assertEqual(result["data"]["years"][0]["rows_collapsed"], 1)
        self.assertEqual(len(self._archive_rows()), 1)

    def test_all_rejected_records_are_malformed_not_no_market_data(self):
        bad = legacy_annual_record(report_date="not-a-date")
        self._serve_annual(annual_zip("legacy", [bad]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        year = result["data"]["years"][0]
        self.assertEqual(year["status"], "malformed")
        self.assertIn("could not be ingested", year["error"])
        self.assertEqual(year["rows_rejected"], 1)
        self.assertEqual(self._archive_rows(), [])

    def test_year_without_selected_market_rows_is_no_market_data(self):
        other = legacy_annual_record(code="999999", market="MICRO GOLD - COMEX")
        self._serve_annual(annual_zip("legacy", [other]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        year = result["data"]["years"][0]
        self.assertEqual(year["status"], "no_market_data")
        self.assertEqual(year["rows_rejected"], 0)
        self.assertEqual(year["rows_filtered"], 1)

    def test_every_backfill_year_status_is_recorded_durably(self):
        self._serve_annual(annual_zip("legacy", [legacy_annual_record()]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        def not_published(url, timeout=180):
            raise cftc_data.CFTCAnnualNotPublished(url)
        self.wrapper._download_annual = not_published
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        def failed(url, timeout=180):
            raise RuntimeError("provider offline")
        self.wrapper._download_annual = failed
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        self._serve_annual(b"not a zip")
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        self._serve_annual(annual_zip(
            "legacy", [legacy_annual_record(code="999999", market="MICRO GOLD - COMEX")]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])

        runs = self.wrapper.cot_archive_status()["data"]["recent_backfill_runs"]
        statuses = {run["status"] for run in runs}
        self.assertEqual(statuses, {"ok", "not_published", "failed", "malformed", "no_market_data"})
        self.assertEqual(next(run for run in runs if run["status"] == "failed")["detail"]["error"],
                         "provider offline")
        self.assertIn("not a ZIP",
                      next(run for run in runs if run["status"] == "malformed" and
                           run["years"] == "2024")["detail"]["error"])

    def test_legacy_combined_starts_in_1995_with_both_official_url_patterns(self):
        rejected = self.wrapper.cot_backfill("legacy", False, 1986, 1986, ["gold"])
        self.assertNotIn("success", rejected)
        self.assertIn("begin in 1995", rejected["error"]["error"])

        calls = []

        def download(url, timeout=180):
            calls.append(url)
            if url.endswith("deahistfo1995.zip"):
                raise cftc_data.CFTCAnnualNotPublished(url)
            return annual_zip(
                "legacy", [legacy_annual_record(report_date="1995-03-21")], entry="annualof.txt")

        self.wrapper._download_annual = download
        result = self.wrapper.cot_backfill("legacy", False, 1995, 1995, ["gold"])
        self.assertEqual(result["data"]["years"][0]["status"], "ok")
        self.assertTrue(calls[0].endswith("deahistfo1995.zip"), calls)
        self.assertTrue(calls[-1].endswith("deahistfo_1995.zip"), calls)
        self.assertEqual(calls.count(calls[0]), 1, "the alternate is only tried when the primary 404s")

        self._serve_annual(annual_zip("legacy", [legacy_annual_record(report_date="1986-01-15")]))
        futonly = self.wrapper.cot_backfill("legacy", True, 1986, 1986, ["gold"])
        self.assertEqual(futonly["data"]["years"][0]["status"], "ok")
        self.assertIn("deacot1986.zip", self.captured_urls[0])

    def test_spreading_and_participant_trader_counts_survive_both_ingestion_paths(self):
        annual = legacy_annual_record(report_date="2024-06-25")
        annual.update({
            "Noncommercial Positions-Spreading (All)": "1234",
            "Traders-Noncommercial-Long (All)": "111",
            "Traders-Noncommercial-Short (All)": "222",
            "Traders-Noncommercial-Spreading (All)": "333",
            "Traders-Commercial-Long (All)": "444",
            "Traders-Commercial-Short (All)": "555",
        })
        self._serve_annual(annual_zip("legacy", [annual]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        self.assertEqual(result["data"]["years"][0]["status"], "ok")

        stored = json.loads(self._archive_rows()[0]["row_json"])
        self.assertEqual(stored["non_commercial_spread"], 1234)
        self.assertEqual(stored["non_commercial_traders_long"], 111)
        self.assertEqual(stored["non_commercial_traders_short"], 222)
        self.assertEqual(stored["non_commercial_traders_spread"], 333)
        self.assertEqual(stored["commercial_traders_long"], 444)
        self.assertEqual(stored["commercial_traders_short"], 555)
        self.assertIsNone(stored["commercial_traders_spread"],
                          "the legacy commercial count has no published spread column")
        self.assertNotIn("managed_money_spread", stored)

        # The current path must produce the identical row for the same report,
        # so the two ingestion paths do not churn each other's new fields.
        current = raw_legacy_row(
            report_date="2024-06-25", oi="452190", comm_long="86551", comm_short="358039",
            noncomm_long="284885", noncomm_short="38656", nonrept_long="48436", nonrept_short="23177")
        current.update({
            "contract_units": "(CONTRACTS OF 100 TROY OUNCES)",
            "contract_market_name": "GOLD",
            "traders_tot_all": "300",
            "traders_tot_rept_long_all": "243",
            "traders_tot_rept_short_all": "158",
            "conc_gross_le_4_tdr_long": "17.7",
            "conc_gross_le_4_tdr_short": "42.6",
            "conc_gross_le_8_tdr_long": "27.9",
            "conc_gross_le_8_tdr_short": "58.7",
            "conc_net_le_4_tdr_long_all": "17.5",
            "conc_net_le_4_tdr_short_all": "40.3",
            "conc_net_le_8_tdr_long_all": "26.5",
            "conc_net_le_8_tdr_short_all": "56.4",
            "noncomm_postions_spread_all": "1234",
            "traders_noncomm_long_all": "111",
            "traders_noncomm_short_all": "222",
            "traders_noncomm_spread_all": "333",
            "traders_comm_long_all": "444",
            "traders_comm_short_all": "555",
        })
        market = self._monitor_rows([current])
        self.assertEqual(market["inserted"], 0)
        self.assertEqual(market["updated"], 0)
        persisted = json.loads(self._archive_rows()[0]["row_json"])
        self.assertEqual(persisted["non_commercial_spread"], 1234)
        self.assertEqual(persisted["non_commercial_traders_spread"], 333)
        self.assertEqual(persisted["commercial_traders_short"], 555)

    def test_subset_backfill_ignores_unrequested_supported_rows(self):
        # A supported-but-unrequested market with malformed data is filtered,
        # not validated: it must not turn a legitimate no-data year into
        # `malformed`, nor inflate the requested market's rejected count.
        corn_bad = legacy_annual_record(report_date="not-a-date", code="002602", market="CORN - CBOT")
        self._serve_annual(annual_zip("legacy", [corn_bad]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        year = result["data"]["years"][0]
        self.assertEqual(year["status"], "no_market_data")
        self.assertEqual(year["rows_rejected"], 0)
        self.assertEqual(year["rows_filtered"], 1)

        gold = legacy_annual_record()
        self._serve_annual(annual_zip("legacy", [gold, corn_bad]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        year = result["data"]["years"][0]
        self.assertEqual(year["status"], "ok")
        self.assertEqual(result["data"]["rows_inserted"], 1)
        self.assertEqual(year["rows_rejected"], 0)
        self.assertEqual(year["rows_filtered"], 1)

    def test_quoted_old_contract_units_normalize_and_do_not_churn(self):
        # The pre-2011 TFF Socrata resource wraps Contract Units in literal
        # quotes while the annual file does not. Both paths must store the same
        # canonical text, and neither may rewrite the other's row.
        annual = {
            "Market_and_Exchange_Names": "EURO FX - CHICAGO MERCANTILE EXCHANGE",
            "As_of_Date_In_Form_YYMMDD": "100720",
            "Report_Date_as_YYYY-MM-DD": "2010-07-20",
            "CFTC_Contract_Market_Code": "099741",
            "Open_Interest_All": "5000",
            "Dealer_Positions_Long_All": "1000",
            "Dealer_Positions_Short_All": "900",
            "Dealer_Positions_Spread_All": "10",
            "Asset_Mgr_Positions_Long_All": "1200",
            "Asset_Mgr_Positions_Short_All": "300",
            "Lev_Money_Positions_Long_All": "800",
            "Lev_Money_Positions_Short_All": "1400",
            "Other_Rept_Positions_Long_All": "250",
            "Other_Rept_Positions_Short_All": "150",
            "NonRept_Positions_Long_All": "400",
            "NonRept_Positions_Short_All": "900",
            "Traders_Tot_All": "140",
            "Traders_Tot_Rept_Long_All": "100",
            "Traders_Tot_Rept_Short_All": "90",
            "Traders_Dealer_Spread_All": "4",
            "Conc_Net_LE_4_TDR_Long_All": "35.0",
            "Contract_Units": "(CONTRACTS OF EUR 125,000)",
        }
        self._serve_annual(annual_zip("tff", [annual]))
        first = self.wrapper.cot_backfill("tff", True, 2010, 2010, ["euro"])
        self.assertEqual(first["data"]["years"][0]["status"], "ok")
        stored = json.loads(self._archive_rows()[0]["row_json"])
        self.assertEqual(stored["contract_units"], "(CONTRACTS OF EUR 125,000)")

        current = raw_tff_row(report_date="2010-07-20")
        current.update({
            "contract_units": "'(CONTRACTS OF EUR 125,000)'",
            "traders_tot_all": "140",
            "traders_tot_rept_long_all": "100",
            "traders_tot_rept_short_all": "90",
            "traders_dealer_spread_all": "4",
            "dealer_positions_spread_all": "10",
            "conc_net_le_4_tdr_long_all": "35.0",
        })
        self._serve_monitor([current])
        current_result = self.wrapper.get_cot_monitor("tff", True, ["euro"], 25000, refresh=True)
        market = current_result["data"]["markets"][0]
        self.assertEqual(market["inserted"], 0)
        self.assertEqual(market["updated"], 0)
        row = json.loads(self._archive_rows()[0]["row_json"])
        self.assertEqual(row["contract_units"], "(CONTRACTS OF EUR 125,000)")

        # An annual re-ingestion after the current path must also be a no-op.
        second = self.wrapper.cot_backfill("tff", True, 2010, 2010, ["euro"])
        self.assertEqual(second["data"]["rows_inserted"], 0)
        self.assertEqual(second["data"]["rows_updated"], 0)
        self.assertEqual(len(self._archive_rows()), 1)

    def test_monitor_wrong_contract_code_fails_the_market_and_writes_nothing(self):
        wrong = raw_legacy_row(report_date="2026-09-01", code="999999")
        self._serve_monitor([wrong])
        result = self.wrapper.get_cot_monitor("legacy", True, ["gold"], 25000, refresh=True)

        self.assertTrue(result.get("success"), result)
        market = result["data"]["markets"][0]
        self.assertEqual(market["status"], "unavailable")
        self.assertIn("while 088691 was requested", market["refresh_error"])
        self.assertEqual(market["rows"], [])
        self.assertEqual(self._archive_rows(), [])

    def test_backfill_unknown_market_fails_closed(self):
        self._serve_annual(annual_zip("legacy", [legacy_annual_record()]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["micro_gold"])
        self.assertNotIn("success", result)
        self.assertIn("Unknown market", result["error"]["error"])

    def test_futures_only_and_combined_are_separate_archive_identities(self):
        self._serve_annual(annual_zip("legacy", [legacy_annual_record()]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        combined = legacy_annual_record()
        self._serve_annual(annual_zip("legacy", [combined]))
        self.wrapper.cot_backfill("legacy", False, 2024, 2024, ["gold"])
        rows = self._archive_rows()
        self.assertEqual(sorted(row["report_basis"] for row in rows),
                         ["futures_and_options_combined", "futures_only"])
        self.assertEqual({json.loads(row["row_json"])["futonly_or_combined"] for row in rows},
                         {"FutOnly", "Combined"})

    # ── monitor ─────────────────────────────────────────────────────────────

    def test_monitor_merges_and_persists_incremental_rows(self):
        self._serve_annual(annual_zip("legacy", [legacy_annual_record(report_date="2024-06-18")]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        # The next monitor refresh returns the full current history, including
        # the already-archived week and a new one.
        market = self._monitor_rows([
            raw_legacy_row(report_date="2024-06-18"),
            raw_legacy_row(report_date="2024-06-25"),
        ])
        self.assertEqual(market["status"], "updated")
        self.assertEqual(market["inserted"], 1)
        rows = self._archive_rows()
        self.assertEqual([row["report_date"] for row in rows], ["2024-06-18", "2024-06-25"])
        # The same full-history response twice must not duplicate anything.
        again = self._monitor_rows([
            raw_legacy_row(report_date="2024-06-18"),
            raw_legacy_row(report_date="2024-06-25"),
        ])
        # Nothing new on the second, incremental read: the stored history is
        # current and is not rewritten.
        self.assertEqual(again["status"], "current")
        self.assertEqual(again["inserted"], 0)
        self.assertEqual(len(self._archive_rows()), 2)
        # The second read is a bounded increment from the stored latest date,
        # not another full-history download.
        self.assertIn(">= '2024-06-25'", urllib.parse.unquote(self.captured_urls[-1]))

    def test_monitor_refresh_failure_uses_archive_with_explicit_status(self):
        self._serve_annual(annual_zip("legacy", [legacy_annual_record()]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        self._serve_monitor([], fail="provider offline")
        result = self.wrapper.get_cot_monitor("legacy", True, ["gold"], 25000, refresh=True)
        market = result["data"]["markets"][0]
        self.assertEqual(market["status"], "archive_only")
        self.assertIn("provider offline", market["refresh_error"])
        self.assertEqual(len(market["rows"]), 1)
        self.assertEqual(market["rows"][0]["report_date_as_yyyy_mm_dd"], "2024-06-25")

    def test_monitor_without_archive_and_failed_refresh_is_unavailable(self):
        self._serve_monitor([], fail="provider offline")
        result = self.wrapper.get_cot_monitor("legacy", True, ["gold"], 25000, refresh=True)
        market = result["data"]["markets"][0]
        self.assertEqual(market["status"], "unavailable")
        self.assertEqual(market["rows"], [])

    def test_monitor_no_data_and_unknown_market_are_distinct(self):
        self._serve_monitor([])
        result = self.wrapper.get_cot_monitor("legacy", True, ["gold", "micro_gold"], 25000, refresh=True)
        by_key = {market["market_key"]: market for market in result["data"]["markets"]}
        self.assertEqual(by_key["gold"]["status"], "no_data")
        self.assertEqual(by_key["micro_gold"]["status"], "unknown_market")
        self.assertEqual(by_key["micro_gold"]["rows"], [])

    def test_monitor_bounds_full_history_and_transports_the_concentration_series(self):
        # 320 weekly rows: the monitor must return the last 200 full rows and the
        # older primary-concentration values, not a silently truncated history.
        rows = []
        report_date = datetime(2020, 1, 7)
        for i in range(320):
            row = raw_legacy_row(report_date=report_date.strftime("%Y-%m-%d"))
            row["conc_gross_le_4_tdr_long"] = str(10.0 + 0.05 * i)
            rows.append(row)
            report_date += timedelta(days=7)
        market = self._monitor_rows(rows)
        self.assertEqual(len(market["rows"]), 200)
        history = market["concentration_history"]
        self.assertEqual(history["field"], "concentration_gross_4_long")
        self.assertEqual(len(history["dates"]), 120)
        self.assertEqual(history["dates"][0], "2020-01-07")
        self.assertEqual(history["dates"][-1], rows[119]["report_date_as_yyyy_mm_dd"])
        self.assertAlmostEqual(history["values"][-1], 10.0 + 0.05 * 119)
        # The recent rows and the older series must not overlap.
        self.assertLess(history["dates"][-1], market["rows"][0]["report_date_as_yyyy_mm_dd"])

    def test_monitor_rows_are_canonical_for_the_family_and_basis(self):
        self._serve_monitor([raw_tff_row(report_date="2026-09-01"),
                             raw_tff_row(report_date="2026-09-08")])
        result = self.wrapper.get_cot_monitor("tff", True, ["euro"], 25000, refresh=True)
        self.assertTrue(result.get("success"), result)
        market = result["data"]["markets"][0]
        row = market["rows"][-1]
        self.assertEqual(market["status"], "updated")
        self.assertEqual(row["futonly_or_combined"], "FutOnly")
        self.assertEqual(row["cftc_contract_market_code"], "099741")
        self.assertEqual(row["leveraged_funds_long"], 800)
        self.assertEqual(row["leveraged_funds_short"], 1400)
        self.assertEqual(row["dealer_long"], 1000)
        self.assertEqual(result["data"]["report_basis"], "futures_only")

    def test_monitor_missing_cells_stay_missing(self):
        incomplete = raw_legacy_row(report_date="2026-09-01")
        incomplete["noncomm_positions_long_all"] = None
        self._serve_monitor([incomplete])
        market = self._monitor_rows([incomplete])
        row = market["rows"][0]
        self.assertIsNone(row["non_commercial_long"])
        self.assertIsNotNone(row["non_commercial_short"])

    def test_annual_text_metadata_is_trimmed_to_match_the_current_path(self):
        # The older annual files pad Contract Units / market names with trailing
        # spaces; the Socrata resources do not. Both canonical paths must store
        # the same identity text.
        padded = legacy_annual_record()
        padded["Market and Exchange Names"] = "GOLD - COMMODITY EXCHANGE INC. "
        padded["Contract Units"] = "(CONTRACTS OF 100 TROY OUNCES) "
        self._serve_annual(annual_zip("legacy", [padded]))
        result = self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        self.assertTrue(result.get("success"), result)
        row = json.loads(self._archive_rows()[0]["row_json"])
        self.assertEqual(row["market_and_exchange_names"], "GOLD - COMMODITY EXCHANGE INC.")
        self.assertEqual(row["contract_units"], "(CONTRACTS OF 100 TROY OUNCES)")

        spaced = raw_legacy_row(report_date="2026-09-01")
        spaced["market_and_exchange_names"] = "GOLD - COMMODITY EXCHANGE INC. "
        self._serve_monitor([spaced])
        market = self._monitor_rows([spaced])
        self.assertEqual(market["rows"][0]["market_and_exchange_names"], "GOLD - COMMODITY EXCHANGE INC.")

    def test_monitor_refresh_disabled_reports_stored_only(self):
        self._serve_annual(annual_zip("legacy", [legacy_annual_record()]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        result = self.wrapper.get_cot_monitor("legacy", True, ["gold"], 25000, refresh=False)
        market = result["data"]["markets"][0]
        self.assertEqual(market["status"], "archive_only")
        self.assertEqual(len(market["rows"]), 1)

    def test_archive_status_reports_coverage_and_provenance(self):
        self._serve_annual(annual_zip("legacy", [legacy_annual_record()]))
        self.wrapper.cot_backfill("legacy", True, 2024, 2024, ["gold"])
        status = self.wrapper.cot_archive_status()
        self.assertTrue(status.get("success"), status)
        self.assertEqual(status["data"]["total_observations"], 1)
        self.assertEqual(status["data"]["groups"][0]["report_family"], "legacy")
        self.assertEqual(status["data"]["groups"][0]["report_basis"], "futures_only")
        runs = status["data"]["recent_backfill_runs"]
        self.assertTrue(any(run["status"] == "ok" for run in runs))

    # ── catalog consistency ─────────────────────────────────────────────────

    @unittest.skipUnless(os.path.exists(_CATALOG_HEADER), "application market catalog not present")
    def test_market_keys_match_the_cpp_catalog_header(self):
        with open(_CATALOG_HEADER, encoding="utf-8") as handle:
            text = handle.read()
        entries = re.findall(
            r'\{\s*QStringLiteral\("([^"]+)"\)\s*,\s*QStringLiteral\("([^"]+)"\)\s*,\s*'
            r'QStringLiteral\("([^"]+)"\)\s*\}',
            text)
        self.assertEqual(len(entries), 37)
        self.assertEqual({key for key, _label, _asset in entries}, set(self.wrapper.cot_codes))
        for _key, label, asset_class in entries:
            self.assertTrue(label)
            self.assertTrue(asset_class)


if __name__ == "__main__":
    unittest.main(verbosity=2)
