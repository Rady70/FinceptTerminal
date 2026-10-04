"""Deterministic tests for the MarketLab FedWatch FRED target-range path."""

from __future__ import annotations

import unittest
from datetime import date

from fedwatch_test_support import (
    FIXTURE_FRED_LOWER,
    FIXTURE_FRED_UPPER,
    FakeTransport,
    FixedClock,
    fixture_text,
    utc,
)

from fedwatch import fred
from fedwatch.errors import FedwatchError
from fedwatch.transport import TransportError


def csv_text(rows) -> str:
    return "DATE,VALUE\n" + "\n".join(rows) + "\n"


class FredParseTests(unittest.TestCase):
    def test_recent_fixtures_parse_to_latest_values(self):
        upper = fred.parse_fred_csv(fixture_text(FIXTURE_FRED_UPPER))
        lower = fred.parse_fred_csv(fixture_text(FIXTURE_FRED_LOWER))
        self.assertEqual(upper[-1], {"date": date(2026, 9, 28), "value": 4.0})
        self.assertEqual(lower[-1], {"date": date(2026, 9, 28), "value": 3.75})
        self.assertEqual(len(upper), 12)
        self.assertEqual(len(lower), 12)

    def test_missing_observation_markers_are_skipped(self):
        parsed = fred.parse_fred_csv(csv_text(["2026-09-25,4.00", "2026-09-26,.", "2026-09-28,4.00"]))
        self.assertEqual([row["value"] for row in parsed], [4.0, 4.0])

    def test_empty_or_headerless_response_raises(self):
        with self.assertRaises(ValueError):
            fred.parse_fred_csv("")
        with self.assertRaises(ValueError):
            fred.parse_fred_csv("VALUE\n")
        with self.assertRaises(ValueError):
            fred.parse_fred_csv("DATE,VALUE\n2026-09-28,.\n")


class FredTargetRangeTests(unittest.TestCase):
    def build_transport(self):
        return FakeTransport().add_text(
            "DFEDTARU", fixture_text(FIXTURE_FRED_UPPER)
        ).add_text("DFEDTARL", fixture_text(FIXTURE_FRED_LOWER))

    def test_current_target_range(self):
        payload = fred.fetch_target_range(
            self.build_transport(), clock=FixedClock(utc(2026, 9, 28, 12))
        )
        self.assertEqual(payload["target_range"], {"lower": 3.75, "upper": 4.0})
        self.assertEqual(payload["latest_observation_date"], "2026-09-28")
        self.assertEqual(payload["retrieved_at"], "2026-09-28T12:00:00Z")
        self.assertTrue(payload["same_latest_date"])
        self.assertEqual(payload["series"]["upper"]["series_id"], "DFEDTARU")
        self.assertEqual(payload["series"]["lower"]["series_id"], "DFEDTARL")
        self.assertEqual(len(payload["recent_observations"]), 5)

    def test_transport_failure_is_a_fred_provider_error(self):
        transport = FakeTransport().add_text("DFEDTARU", TransportError("HTTP 500"))
        with self.assertRaises(FedwatchError) as caught:
            fred.fetch_target_range(transport)
        self.assertEqual(caught.exception.provider, "fred")
        self.assertEqual(caught.exception.code, "FRED_SOURCE_UNAVAILABLE")

    def test_inconsistent_bounds_are_rejected(self):
        transport = FakeTransport().add_text(
            "DFEDTARU", csv_text(["2026-09-28,3.50"])
        ).add_text("DFEDTARL", csv_text(["2026-09-28,3.75"]))
        with self.assertRaises(FedwatchError) as caught:
            fred.fetch_target_range(transport)
        self.assertEqual(caught.exception.code, "FRED_TARGET_RANGE_INVALID")

    def test_equal_bounds_are_rejected(self):
        transport = FakeTransport().add_text(
            "DFEDTARU", csv_text(["2026-09-28,3.75"])
        ).add_text("DFEDTARL", csv_text(["2026-09-28,3.75"]))
        with self.assertRaises(FedwatchError) as caught:
            fred.fetch_target_range(transport)
        self.assertEqual(caught.exception.provider, "fred")
        self.assertEqual(caught.exception.code, "FRED_TARGET_RANGE_INVALID")

    def test_negative_bounds_are_rejected(self):
        transport = FakeTransport().add_text(
            "DFEDTARU", csv_text(["2026-09-28,0.00"])
        ).add_text("DFEDTARL", csv_text(["2026-09-28,-0.25"]))
        with self.assertRaises(FedwatchError) as caught:
            fred.fetch_target_range(transport)
        self.assertEqual(caught.exception.code, "FRED_TARGET_RANGE_INVALID")

    def test_non_finite_bounds_are_rejected(self):
        transport = FakeTransport().add_text(
            "DFEDTARU", csv_text(["2026-09-28,nan"])
        ).add_text("DFEDTARL", csv_text(["2026-09-28,3.75"]))
        with self.assertRaises(FedwatchError) as caught:
            fred.fetch_target_range(transport)
        self.assertEqual(caught.exception.code, "FRED_TARGET_RANGE_INVALID")

    def test_different_latest_observation_dates_retain_stale_common_pair(self):
        # A lagging lower series must not let MarketLab synthesize a
        # point-in-time range from two different dates.
        transport = FakeTransport().add_text(
            "DFEDTARU", csv_text(["2026-09-24,4.00", "2026-09-28,4.00"])
        ).add_text("DFEDTARL", csv_text(["2026-09-24,3.75", "2026-09-25,3.75"]))
        result = fred.fetch_target_range(transport, clock=FixedClock(utc(2026, 9, 28, 12)))
        self.assertEqual(result["status"], "STALE")
        self.assertEqual(result["latest_observation_date"], "2026-09-24")
        self.assertEqual(result["target_range"], {"upper": 4, "lower": 3.75})
        self.assertFalse(result["same_latest_date"])

    def test_future_dated_observation_is_rejected(self):
        transport = FakeTransport().add_text(
            "DFEDTARU", csv_text(["2026-10-01,4.00"])
        ).add_text("DFEDTARL", csv_text(["2026-10-01,3.75"]))
        with self.assertRaises(FedwatchError) as caught:
            fred.fetch_target_range(transport, clock=FixedClock(utc(2026, 9, 28, 12)))
        self.assertEqual(caught.exception.code, "FRED_TARGET_RANGE_INVALID")

    def test_html_error_page_is_reported_as_invalid(self):
        transport = FakeTransport().add_text(
            "DFEDTARU", "<html><body>maintenance</body></html>"
        ).add_text("DFEDTARL", "<html><body>maintenance</body></html>")
        with self.assertRaises(FedwatchError) as caught:
            fred.fetch_target_range(transport)
        self.assertEqual(caught.exception.code, "FRED_TARGET_RANGE_INVALID")


if __name__ == "__main__":
    unittest.main()
