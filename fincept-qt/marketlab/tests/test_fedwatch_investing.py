"""Deterministic tests for the MarketLab FedWatch Investing.com Fed-side path.

Pins the behavior that was qualified under the retired reference project:

* the July 2026 qualified fixture's known values (2026-07-29: 87.8/12.2) and
  the sidebar-duplicate dedupe;
* the current 2026-09-28 captured page (2026-10-28: 30.0/70.0; 2026-12-09:
  6.2/38.3/55.4) so the parser is pinned against today's real structure as
  well as the qualified snapshot;
* the rounded-probability normalization regression with its exact values
  (raw sum 100.1 -> factor 0.999000999000999 -> normalized local 72.392008 /
  27.607992), the accepted boundary cases and the rejected malformed cases;
* the whole-table normalization-before-conversion rule (a later meeting's
  local step shifts because an earlier normalized expected rate propagated);
* provider-attributed errors for transport failures, empty parses and
  malformed distributions.

Run locally:
    python -m unittest discover -s marketlab/tests -p "test_fedwatch_*.py"
"""

from __future__ import annotations

import unittest

from fedwatch_test_support import (
    FIXTURE_INVESTING_LIVE,
    FIXTURE_INVESTING_QUALIFIED,
    FakeTransport,
    FixedClock,
    fixture_text,
    make_investing_html as synthetic_html,
    utc,
)

from fedwatch import investing
from fedwatch.errors import FedwatchError
from fedwatch.transport import TransportError


class InvestingParseTests(unittest.TestCase):
    def test_qualified_july_fixture_known_values(self):
        rows, warnings, report = investing.parse_fed_rate_monitor(
            fixture_text(FIXTURE_INVESTING_QUALIFIED)
        )
        self.assertEqual(warnings, [])
        self.assertTrue(report["structurally_complete"])
        meeting_dates = sorted({row["meeting_date"] for row in rows})
        self.assertEqual(len(meeting_dates), 12)
        july = [row for row in rows if row["meeting_date"] == "2026-07-29"]
        self.assertEqual(
            [(row["rate_low"], row["rate_high"], row["probability_pct"]) for row in july],
            [(3.5, 3.75, 87.8), (3.75, 4.0, 12.2)],
        )
        # The sidebar widget repeats the nearest meeting; deduplication must
        # leave exactly the two main-table rows for it.
        self.assertEqual(len(july), 2)
        for meeting_date in meeting_dates:
            total = sum(row["probability_pct"] for row in rows if row["meeting_date"] == meeting_date)
            self.assertLessEqual(abs(total - 100.0), 0.5, meeting_date)

    def test_current_capture_known_values(self):
        rows, warnings, report = investing.parse_fed_rate_monitor(
            fixture_text(FIXTURE_INVESTING_LIVE)
        )
        self.assertEqual(warnings, [])
        self.assertTrue(report["structurally_complete"])
        self.assertEqual(len({row["meeting_date"] for row in rows}), 10)
        october = [row for row in rows if row["meeting_date"] == "2026-10-28"]
        self.assertEqual(
            [(row["rate_low"], row["rate_high"], row["probability_pct"]) for row in october],
            [(3.75, 4.0, 30.0), (4.0, 4.25, 70.0)],
        )
        december = [row for row in rows if row["meeting_date"] == "2026-12-09"]
        self.assertEqual(
            [row["probability_pct"] for row in december], [6.2, 38.3, 55.4]
        )
        rows_by_meeting = {row["meeting_date"]: row["rate_low"] for row in rows}
        self.assertEqual(len(rows_by_meeting), 10)

    def test_empty_html_returns_empty_with_warning(self):
        rows, warnings, report = investing.parse_fed_rate_monitor(
            "<html><body>nope</body></html>"
        )
        self.assertEqual(rows, [])
        self.assertEqual(len(warnings), 1)
        self.assertFalse(report["structurally_complete"])

    def test_unparseable_meeting_time_skips_block(self):
        html = synthetic_html(
            [
                ("not a date", [(3.5, 3.75, 100.0)]),
                ("Oct 28, 2026 02:00PM ET", [(3.75, 4.0, 100.0)]),
            ]
        )
        rows, warnings, report = investing.parse_fed_rate_monitor(html)
        self.assertEqual([row["meeting_date"] for row in rows], ["2026-10-28"])
        self.assertTrue(any("unparseable meeting time" in warning for warning in warnings))
        self.assertFalse(report["structurally_complete"])
        self.assertEqual(len(report["dropped_meeting_blocks"]), 1)

    def test_meeting_without_parseable_buckets_is_skipped_with_warning(self):
        html = synthetic_html([("Oct 28, 2026 02:00PM ET", [])])
        rows, warnings, report = investing.parse_fed_rate_monitor(html)
        self.assertEqual(rows, [])
        self.assertTrue(any("no parseable bucket rows" in warning for warning in warnings))
        self.assertFalse(report["structurally_complete"])

    def test_malformed_percentage_token_skips_row_and_marks_meeting_partial(self):
        # "[0-9.]+" matches strings like "1..2"; float() must be guarded so a
        # malformed provider token cannot escape as a raw ValueError, and the
        # report must record the lost row so the provider can fail closed.
        html = synthetic_html(
            [
                (
                    "Oct 28, 2026 02:00PM ET",
                    [(3.75, 4.00, "1..2"), (4.00, 4.25, 100.0)],
                )
            ]
        )
        rows, warnings, report = investing.parse_fed_rate_monitor(html)
        self.assertEqual([row["probability_pct"] for row in rows], [100.0])
        self.assertTrue(
            any("unparseable target-rate interval or percentage" in warning for warning in warnings)
        )
        self.assertFalse(report["structurally_complete"])
        self.assertEqual(report["dropped_bucket_row_count"], 1)
        self.assertEqual(report["partial_meeting_dates"], ["2026-10-28"])

    def test_bucket_marker_without_matching_structure_is_reported(self):
        # A percentage token that does not match the bucket regex at all (e.g.
        # "abc%") silently disappears from findall; the marker count exposes it.
        html = synthetic_html(
            [
                (
                    "Oct 28, 2026 02:00PM ET",
                    [(3.75, 4.00, "abc"), (4.00, 4.25, 100.0)],
                )
            ]
        )
        rows, warnings, report = investing.parse_fed_rate_monitor(html)
        self.assertEqual([row["probability_pct"] for row in rows], [100.0])
        self.assertFalse(report["structurally_complete"])
        self.assertEqual(report["unmatched_bucket_item_count"], 1)
        self.assertEqual(report["partial_meeting_dates"], ["2026-10-28"])
        self.assertTrue(any("did not contain exactly one range and percentage" in w for w in warnings))

    def test_all_malformed_percentages_is_an_investing_provider_error(self):
        html = synthetic_html(
            [
                (
                    "Oct 28, 2026 02:00PM ET",
                    [(3.75, 4.00, "."), (4.00, 4.25, "1..2")],
                )
            ]
        )
        transport = FakeTransport().add_text("fed-rate-monitor", html)
        with self.assertRaises(FedwatchError) as caught:
            investing.fetch_distributions(transport)
        self.assertEqual(caught.exception.provider, "investing")
        self.assertEqual(caught.exception.code, "INVESTING_PARSE_EMPTY")

    def test_partial_parse_fails_provider_closed(self):
        # One malformed bucket plus a surviving bucket that sums to exactly
        # 100 must not become an ordinary successful distribution: the
        # provider fails closed as a partially parsed source.
        html = synthetic_html(
            [
                (
                    "Oct 28, 2026 02:00PM ET",
                    [(3.75, 4.00, "1..2"), (4.00, 4.25, 100.0)],
                )
            ]
        )
        transport = FakeTransport().add_text("fed-rate-monitor", html)
        with self.assertRaises(FedwatchError) as caught:
            investing.fetch_distributions(transport)
        self.assertEqual(caught.exception.provider, "investing")
        self.assertEqual(caught.exception.code, "INVESTING_PARSE_PARTIAL")
        self.assertEqual(
            caught.exception.detail["parse_report"]["dropped_bucket_row_count"], 1
        )

    def test_bucket_marker_without_matching_regex_fails_provider_closed(self):
        html = synthetic_html(
            [
                (
                    "Oct 28, 2026 02:00PM ET",
                    [(3.75, 4.00, "abc"), (4.00, 4.25, 100.0)],
                )
            ]
        )
        transport = FakeTransport().add_text("fed-rate-monitor", html)
        with self.assertRaises(FedwatchError) as caught:
            investing.fetch_distributions(transport)
        self.assertEqual(caught.exception.provider, "investing")
        self.assertEqual(caught.exception.code, "INVESTING_PARSE_PARTIAL")
        self.assertEqual(
            caught.exception.detail["parse_report"]["unmatched_bucket_item_count"], 1
        )

    def test_unparseable_meeting_block_preserves_valid_meeting(self):
        html = synthetic_html(
            [
                ("not a date", [(3.75, 4.00, 50.0), (4.00, 4.25, 50.0)]),
                ("Nov 18, 2026 02:00PM ET", [(3.75, 4.00, 50.0), (4.00, 4.25, 50.0)]),
            ]
        )
        transport = FakeTransport().add_text("fed-rate-monitor", html)
        result = investing.fetch_distributions(transport)
        self.assertEqual([m["meeting_date"] for m in result["meetings"]], ["2026-11-18"])
        self.assertEqual(result["errors"][0]["code"], "INVESTING_PARSE_PARTIAL")
        self.assertIsNone(investing.with_local_probabilities(result, 4, 3.75)[0]["local_probabilities"])


class NormalizationRegressionTests(unittest.TestCase):
    """The exact regression that was qualified by the retired reference
    project (see qualify_fedwatch_polymarket.py _normalization_regression)."""

    @staticmethod
    def frame(probabilities, meeting_date="2026-10-28"):
        buckets = list(zip([3.50, 3.75, 4.00], [3.75, 4.00, 4.25], probabilities))
        return [
            {
                "meeting_date": meeting_date,
                "rate_low": low,
                "rate_high": high,
                "probability_pct": probability,
            }
            for low, high, probability in buckets
        ]

    def test_captured_october_case_matches_qualified_values(self):
        captured = self.frame([35.6, 64.4], "2026-09-16") + self.frame([25.8, 56.5, 17.8])
        original = [dict(row) for row in captured]

        normalized, records = investing.normalize_cumulative(captured)

        self.assertEqual(captured, original)  # input never mutated
        october = next(record for record in records if record["meeting_date"] == "2026-10-28")
        self.assertAlmostEqual(october["raw_probability_sum_pct"], 100.1, places=9)
        self.assertEqual(october["normalization_factor"], 0.999000999000999)
        self.assertAlmostEqual(october["normalized_probability_sum_pct"], 100.0, places=9)
        self.assertTrue(october["normalization_applied"])
        self.assertAlmostEqual(
            sum(
                row["probability_pct"]
                for row in normalized
                if row["meeting_date"] == "2026-10-28"
            ),
            100.0,
            places=9,
        )

        raw_local = {
            row["local_bp_change"]: row["probability_pct"]
            for row in investing.local_steps_from_cumulative(captured, 3.75, 3.50)
            if row["meeting_date"] == "2026-10-28"
        }
        normalized_local = {
            row["local_bp_change"]: row["probability_pct"]
            for row in investing.local_steps_from_cumulative(normalized, 3.75, 3.50)
            if row["meeting_date"] == "2026-10-28"
        }
        self.assertAlmostEqual(raw_local[0], 70.85, places=6)
        self.assertAlmostEqual(raw_local[25], 29.15, places=6)
        self.assertAlmostEqual(normalized_local[0], 72.392008, places=6)
        self.assertAlmostEqual(normalized_local[25], 27.607992, places=6)

    def test_exact_100_input_is_returned_unchanged(self):
        exact = self.frame([40.0, 60.0])
        normalized, records = investing.normalize_cumulative(exact)
        self.assertEqual(normalized, exact)
        self.assertFalse(records[0]["normalization_applied"])

    def test_boundary_sums_are_accepted_and_normalized(self):
        for probabilities in ([49.95, 49.95], [50.05, 50.05]):
            normalized, records = investing.normalize_cumulative(self.frame(probabilities))
            self.assertAlmostEqual(
                sum(row["probability_pct"] for row in normalized), 100.0, places=9
            )
            self.assertTrue(records[0]["normalization_applied"])

    def test_malformed_distributions_are_rejected(self):
        for probabilities in ([48.5, 48.5], [51.5, 51.5], [50.0, -1.0, 51.0],
                              [50.0, float("nan"), 50.0]):
            with self.assertRaises(ValueError, msg=probabilities):
                investing.normalize_cumulative(self.frame(probabilities))

    def test_malformed_rate_ranges_are_rejected_before_any_conversion(self):
        # The previously qualified input-quality boundary: finite bounds,
        # non-negative lows, an ordered plausible range, and probabilities
        # inside [0, 100] before the expected-rate conversion.
        cases = {
            "equal_bounds": [(3.50, 3.75, 50.0), (4.00, 4.00, 50.0)],
            "reversed_bounds": [(3.50, 3.75, 50.0), (4.25, 4.00, 50.0)],
            "negative_low": [(3.50, 3.75, 50.0), (-0.25, 0.00, 50.0)],
            "implausible_high": [(3.50, 3.75, 50.0), (4.00, 10.5, 50.0)],
            "non_finite_high": [(3.50, 3.75, 50.0), (4.00, float("nan"), 50.0)],
            "probability_above_100": [(3.50, 3.75, 150.0), (4.00, 4.25, -50.0)],
        }
        for label, buckets in cases.items():
            rows = [
                {
                    "meeting_date": "2026-10-28",
                    "rate_low": low,
                    "rate_high": high,
                    "probability_pct": probability,
                }
                for low, high, probability in buckets
            ]
            with self.assertRaises(ValueError, msg=label) as caught:
                investing.normalize_cumulative(rows)
            self.assertEqual(
                getattr(caught.exception, "code", None),
                "INVESTING_DISTRIBUTION_INVALID",
                label,
            )

    def test_empty_distribution_is_rejected(self):
        with self.assertRaises(ValueError):
            investing.normalize_cumulative([])


class LocalStepTests(unittest.TestCase):
    def test_local_step_distribution_preserves_mean_and_sums_to_one(self):
        for change in (0.0, 0.3, 0.6, 1.0, 1.7, -0.4, -2.2):
            distribution = investing.local_step_distribution(change)
            self.assertAlmostEqual(sum(distribution.values()), 1.0, places=12)
            mean_bp = sum(bp * probability for bp, probability in distribution.items())
            self.assertAlmostEqual(mean_bp, change * 25.0, places=9)

    def test_zero_mantissa_collapses_to_single_outcome(self):
        self.assertEqual(investing.local_step_distribution(2.0), {50: 1.0})

    def test_cme_worked_example_change_2_9_gives_10_90(self):
        distribution = investing.local_step_distribution(2.9)
        self.assertAlmostEqual(distribution[50], 0.10, places=12)
        self.assertAlmostEqual(distribution[75], 0.90, places=12)

    def test_first_meeting_matches_cumulative_when_current_range_is_lower_bucket(self):
        cumulative = [
            {"meeting_date": "2026-07-29", "rate_low": 3.50, "rate_high": 3.75,
             "probability_pct": 87.8},
            {"meeting_date": "2026-07-29", "rate_low": 3.75, "rate_high": 4.00,
             "probability_pct": 12.2},
        ]
        local = investing.local_steps_from_cumulative(cumulative, 3.75, 3.50)
        self.assertEqual(
            {(row["local_bp_change"], row["probability_pct"]) for row in local},
            {(0, 87.8), (25, 12.2)},
        )

    def test_identical_consecutive_distributions_produce_a_hold_point_mass(self):
        # Deterministic sequential summation makes an unchanged expected rate
        # an exact point mass. (The retired pandas path could turn this into a
        # 1-ULP two-point distribution; that noise is deliberately not
        # migrated — see the Batch A implementation record.)
        cumulative = []
        for meeting_date in ("2027-09-15", "2027-10-27"):
            cumulative.extend(
                [
                    {"meeting_date": meeting_date, "rate_low": 4.75, "rate_high": 5.00,
                     "probability_pct": 31.4},
                    {"meeting_date": meeting_date, "rate_low": 5.00, "rate_high": 5.25,
                     "probability_pct": 20.6},
                ]
            )
        local = investing.local_steps_from_cumulative(cumulative, 5.0, 4.75)
        second = [row for row in local if row["meeting_date"] == "2027-10-27"]
        self.assertEqual(len(second), 1)
        self.assertEqual(second[0]["local_bp_change"], 0)
        self.assertEqual(second[0]["probability_pct"], 100.0)

    def test_local_probabilities_sum_to_100_per_meeting_on_current_capture(self):
        rows, _, _ = investing.parse_fed_rate_monitor(fixture_text(FIXTURE_INVESTING_LIVE))
        normalized, _ = investing.normalize_cumulative(rows)
        local = investing.local_steps_from_cumulative(normalized, 4.0, 3.75)
        for meeting_date in {row["meeting_date"] for row in local}:
            total = sum(row["probability_pct"] for row in local if row["meeting_date"] == meeting_date)
            self.assertAlmostEqual(total, 100.0, places=6, msg=meeting_date)

    def test_normalization_of_a_later_meeting_chains_through_earlier_meetings(self):
        # Qualified live audit: with current bounds 3.75-4.00 the December 2026
        # local step is 22.45/77.55 from raw values and 20.750751/79.249249
        # after validating/normalizing the whole cumulative table.
        rows, _, _ = investing.parse_fed_rate_monitor(fixture_text(FIXTURE_INVESTING_LIVE))
        normalized, records = investing.normalize_cumulative(rows)
        raw_local = {
            row["local_bp_change"]: row["probability_pct"]
            for row in investing.local_steps_from_cumulative(rows, 4.0, 3.75)
            if row["meeting_date"] == "2026-12-09"
        }
        normalized_local = {
            row["local_bp_change"]: row["probability_pct"]
            for row in investing.local_steps_from_cumulative(normalized, 4.0, 3.75)
            if row["meeting_date"] == "2026-12-09"
        }
        self.assertAlmostEqual(raw_local[0], 22.45, places=6)
        self.assertAlmostEqual(raw_local[25], 77.55, places=6)
        self.assertAlmostEqual(normalized_local[0], 20.750751, places=6)
        self.assertAlmostEqual(normalized_local[25], 79.249249, places=6)
        december = next(record for record in records if record["meeting_date"] == "2026-12-09")
        self.assertTrue(december["normalization_applied"])


class FetchDistributionsTests(unittest.TestCase):
    def test_raw_and_normalized_values_stay_distinct(self):
        transport = FakeTransport().add_text("fed-rate-monitor", fixture_text(FIXTURE_INVESTING_LIVE))
        payload = investing.fetch_distributions(transport, clock=FixedClock(utc(2026, 9, 28, 12)))
        self.assertEqual(payload["method"], "LIVE_INVESTING_DERIVED")
        october = next(
            meeting for meeting in payload["meetings"] if meeting["meeting_date"] == "2026-10-28"
        )
        december = next(
            meeting for meeting in payload["meetings"] if meeting["meeting_date"] == "2026-12-09"
        )
        self.assertEqual([row["probability_pct"] for row in october["raw_probabilities"]],
                         [30.0, 70.0])
        self.assertEqual(october["normalization"]["applied"], False)
        self.assertEqual([row["probability_pct"] for row in december["raw_probabilities"]],
                         [6.2, 38.3, 55.4])
        self.assertEqual(december["normalization"]["applied"], True)
        self.assertAlmostEqual(
            sum(row["probability_pct"] for row in december["normalized_probabilities"]),
            100.0,
            places=9,
        )

    def test_transport_failure_is_an_investing_provider_error(self):
        transport = FakeTransport().add_text(
            "fed-rate-monitor", TransportError("HTTP 503", status_code=503)
        )
        with self.assertRaises(FedwatchError) as caught:
            investing.fetch_distributions(transport)
        self.assertEqual(caught.exception.provider, "investing")
        self.assertEqual(caught.exception.code, "INVESTING_SOURCE_UNAVAILABLE")

    def test_empty_parse_is_an_investing_provider_error(self):
        transport = FakeTransport().add_text("fed-rate-monitor", "<html>changed</html>")
        with self.assertRaises(FedwatchError) as caught:
            investing.fetch_distributions(transport)
        self.assertEqual(caught.exception.provider, "investing")
        self.assertEqual(caught.exception.code, "INVESTING_PARSE_EMPTY")

    def test_malformed_distribution_is_an_investing_provider_error(self):
        html = synthetic_html(
            [
                (
                    "Oct 28, 2026 02:00PM ET",
                    [(3.75, 4.00, 51.5), (4.00, 4.25, 51.5)],
                )
            ]
        )
        transport = FakeTransport().add_text("fed-rate-monitor", html)
        with self.assertRaises(ValueError) as caught:
            investing.fetch_distributions(transport)
        self.assertIsInstance(caught.exception, FedwatchError)
        self.assertEqual(caught.exception.provider, "investing")
        self.assertEqual(caught.exception.code, "INVESTING_DISTRIBUTION_INVALID")

    def test_malformed_rate_range_is_an_investing_provider_error(self):
        html = synthetic_html(
            [
                (
                    "Oct 28, 2026 02:00PM ET",
                    [(4.00, 4.00, 50.0), (4.00, 4.25, 50.0)],
                )
            ]
        )
        transport = FakeTransport().add_text("fed-rate-monitor", html)
        with self.assertRaises(FedwatchError) as caught:
            investing.fetch_distributions(transport)
        self.assertEqual(caught.exception.provider, "investing")
        self.assertEqual(caught.exception.code, "INVESTING_DISTRIBUTION_INVALID")


if __name__ == "__main__":
    unittest.main()
