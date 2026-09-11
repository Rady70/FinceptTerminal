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
import json
import os
import sys
import types
import unittest
import urllib.parse
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
        "open_interest_all": oi,
        "prod_merc_positions_long": prod_long,
        "prod_merc_positions_short": prod_short,
        "m_money_positions_long_all": money_long,
        "m_money_positions_short_all": money_short,
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
        self.assertEqual(data["overall_sentiment"]["commercial_bias"], "bullish")
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
            self.assertIn("Dealer/Intermediary", result["error"]["error"])

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

    def test_sentiment_zero_net_is_neutral_not_bearish(self):
        row = raw_legacy_row(comm_long="300", comm_short="300", noncomm_long="200", noncomm_short="200")
        self._serve([row])
        result = self.wrapper.analyze_market_sentiment("gold", "legacy")
        data = result["data"]
        self.assertEqual(data["commercial_positions"]["net"], 0)
        self.assertEqual(data["non_commercial_positions"]["net"], 0)
        self.assertEqual(data["overall_sentiment"]["commercial_bias"], "neutral")
        self.assertEqual(data["overall_sentiment"]["non_commercial_bias"], "neutral")

    def test_disaggregated_sentiment_uses_producer_and_money_fields(self):
        self._serve([raw_disaggregated_row()])
        result = self.wrapper.analyze_market_sentiment("gold", "disaggregated")

        self.assertTrue(result.get("success"), result)
        data = result["data"]
        self.assertEqual(data["commercial_positions"]["net"], 300)          # 500 - 200
        self.assertEqual(data["non_commercial_positions"]["net"], 400)      # 700 - 300
        self.assertEqual(result["parameters"]["report_family"], "disaggregated")

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
