"""Deterministic tests for the MarketLab FedWatch Polymarket path.

Pins the previously qualified parsing (bp-outcome phrasings, Gamma string
fields, Yes token mapping) and the Batch A conservative automatic mapping that
replaces the retired manual review table:

* the captured October and December 2026 events validate against their
  meeting dates even though Polymarket's UTC endDate falls on the next day
  (23:59 U.S. Eastern);
* title/month, date, structure, token, price and question checks each reject
  the candidate they are responsible for;
* two passing candidates for one meeting are AMBIGUOUS, never guessed;
* discovery survives a partial source outage, fails closed when every source
  fails, and distinguishes an empty candidate set;
* current CLOB prices produce CURRENT / STALE / PARTIAL / UNAVAILABLE states
  with provider-specific errors and no fabricated values.
"""

from __future__ import annotations

import copy
import json
import unittest
from datetime import date, timedelta

from fedwatch_test_support import (
    FIXTURE_PM_COMBO,
    FIXTURE_PM_DECEMBER,
    FIXTURE_PM_OCTOBER,
    FakeTransport,
    FixedClock,
    epoch,
    fixture_json,
    make_clob_history,
    make_fed_decision_event,
    make_gamma_market,
    utc,
)

from fedwatch import polymarket
from fedwatch.errors import FedwatchError
from fedwatch.transport import TransportError

MEETING_OCTOBER = date(2026, 10, 28)
MEETING_DECEMBER = date(2026, 12, 9)
NOW = utc(2026, 9, 28, 12, 0, 0)
NO_SLEEP = lambda _seconds: None  # noqa: E731


def event_with_market_mutation(mutation) -> dict:
    event = json.loads(json.dumps(fixture_json(FIXTURE_PM_OCTOBER)))
    mutation(event)
    return event


class OutcomeParsingTests(unittest.TestCase):
    def test_known_phrasings(self):
        cases = [
            ("Will there be no change in Fed interest rates after the October 2026 meeting?",
             {"bp_delta": 0, "open_ended": False}),
            ("Will the Fed decrease interest rates by 25 bps after the October 2026 meeting?",
             {"bp_delta": -25, "open_ended": False}),
            ("Will the Fed decrease interest rates by 50+ bps after the October 2026 meeting?",
             {"bp_delta": -50, "open_ended": True}),
            ("Will the Fed increase interest rates by 25 bps after the October 2026 meeting?",
             {"bp_delta": 25, "open_ended": False}),
            ("Will the Fed increase interest rates by 50+ bps after the October 2026 meeting?",
             {"bp_delta": 50, "open_ended": True}),
            ("Will the Fed cut rates by 25 bps?", {"bp_delta": -25, "open_ended": False}),
        ]
        for question, expected in cases:
            self.assertEqual(polymarket.parse_bp_outcome(question), expected, question)

    def test_non_rate_questions_are_rejected(self):
        for question in (
            "Will Powell say 'inflation' more than 10 times?",
            "Will there be a dissent at the October 2026 meeting?",
            "Powell Bingo: will 'transitory' be said?",
            "Will Waller dissent?",
        ):
            self.assertIsNone(polymarket.parse_bp_outcome(question), question)

    def test_captured_october_event_flattens_five_rate_markets(self):
        event = fixture_json(FIXTURE_PM_OCTOBER)
        markets = polymarket.extract_markets(event)
        rate_markets = [market for market in markets if market["bp_delta"] is not None]
        self.assertEqual(len(rate_markets), 5)
        self.assertEqual(
            [(market["bp_delta"], market["open_ended"]) for market in rate_markets],
            [(-50, True), (-25, False), (0, False), (25, False), (50, True)],
        )
        self.assertEqual(
            [market["yes_probability"] for market in rate_markets],
            [0.0025, 0.0045, 0.335, 0.655, 0.0085],
        )
        for market in rate_markets:
            self.assertTrue(market["yes_clob_token_id"])
            self.assertFalse(market["parse_failed"])


class MalformedStructureTests(unittest.TestCase):
    def test_extract_markets_tolerates_malformed_structures(self):
        self.assertEqual(polymarket.extract_markets({"markets": None}), [])
        self.assertEqual(polymarket.extract_markets({"markets": "oops"}), [])
        self.assertEqual(polymarket.extract_markets({}), [])
        valid = make_gamma_market(
            "Will there be no change in Fed interest rates after the October 2026 meeting?",
            "0.5",
            "tok",
        )
        rows = polymarket.extract_markets({"markets": [None, "x", 42, valid]})
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bp_delta"], 0)

    def test_candidate_without_market_list_is_rejected_not_internal(self):
        event = make_fed_decision_event()
        event["markets"] = None
        evidence, reason = polymarket.validate_candidate_event(event, MEETING_OCTOBER)
        self.assertIsNone(evidence)
        self.assertEqual(reason, "INSUFFICIENT_RATE_SUBMARKETS")

    def test_discovery_tolerates_non_list_markets_field(self):
        event = fixture_json(FIXTURE_PM_OCTOBER)
        event["markets"] = {"unexpected": "shape"}
        transport = build_polymarket_transport([event], {})
        events, stats, warnings = polymarket.discover_candidate_events(transport)
        self.assertEqual(len(events), 1)
        self.assertEqual(stats["candidate_market_rows"], 0)
        self.assertEqual(stats["events_without_market_list"], 1)
        self.assertTrue(any("non-list markets field" in warning for warning in warnings))

    def test_section_with_non_list_markets_gives_mapping_state_not_internal_error(self):
        event = make_fed_decision_event()
        event["markets"] = "broken"
        transport = build_polymarket_transport([event], {})
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertEqual(section["meetings"][0]["mapping_status"], "NOT_FOUND")


class MappingValidationTests(unittest.TestCase):
    def test_captured_october_event_validates_with_eastern_date_evidence(self):
        event = fixture_json(FIXTURE_PM_OCTOBER)
        evidence, reason = polymarket.validate_candidate_event(event, MEETING_OCTOBER)
        self.assertIsNone(reason)
        self.assertIsNotNone(evidence)
        self.assertEqual(evidence["event_end_date_utc"], "2026-10-29T03:59:00Z")
        self.assertEqual(evidence["event_end_date_us_eastern"], "2026-10-28")
        self.assertEqual(evidence["fomc_meeting_end_date"], "2026-10-28")
        self.assertEqual(evidence["outcome_bp_values"], [-50, -25, 0, 25, 50])
        self.assertAlmostEqual(evidence["outcome_probability_sum"], 1.0055, places=6)

    def test_captured_december_event_validates_for_december(self):
        event = fixture_json(FIXTURE_PM_DECEMBER)
        evidence, reason = polymarket.validate_candidate_event(event, MEETING_DECEMBER)
        self.assertIsNone(reason)
        self.assertEqual(evidence["event_end_date_us_eastern"], "2026-12-09")

    def test_combo_event_is_rejected_by_title(self):
        combo = fixture_json(FIXTURE_PM_COMBO)
        _, reason = polymarket.validate_candidate_event(combo, MEETING_DECEMBER)
        self.assertEqual(reason, "TITLE_NOT_FED_DECISION")

    def test_end_date_one_day_later_in_eastern_is_rejected(self):
        event = event_with_market_mutation(
            lambda value: value.update({"endDate": "2026-10-30T15:00:00Z"})
        )
        _, reason = polymarket.validate_candidate_event(event, MEETING_OCTOBER)
        self.assertEqual(reason, "END_DATE_MISMATCH")

    def test_closed_or_inactive_event_is_rejected(self):
        closed = event_with_market_mutation(lambda value: value.update({"closed": True}))
        _, reason = polymarket.validate_candidate_event(closed, MEETING_OCTOBER)
        self.assertEqual(reason, "EVENT_NOT_ACTIVE_AND_OPEN")
        inactive = event_with_market_mutation(lambda value: value.update({"active": False}))
        _, reason = polymarket.validate_candidate_event(inactive, MEETING_OCTOBER)
        self.assertEqual(reason, "EVENT_NOT_ACTIVE_AND_OPEN")

    def test_title_month_mismatch_is_rejected(self):
        event = event_with_market_mutation(
            lambda value: value.update({"title": "Fed Decision in September?", "endDate": "2026-10-29T03:59:00Z"})
        )
        _, reason = polymarket.validate_candidate_event(event, MEETING_OCTOBER)
        self.assertEqual(reason, "TITLE_MONTH_MISMATCH")

    def test_title_without_year_still_validates(self):
        event = event_with_market_mutation(
            lambda value: value.update({"title": "Fed Decision in October?"})
        )
        evidence, reason = polymarket.validate_candidate_event(event, MEETING_OCTOBER)
        self.assertIsNone(reason)
        self.assertIsNone(evidence["title_year"])

    def test_insufficient_rate_submarkets_is_rejected(self):
        def drop_three(event):
            event["markets"] = event["markets"][:2]

        _, reason = polymarket.validate_candidate_event(
            event_with_market_mutation(drop_three), MEETING_OCTOBER
        )
        self.assertEqual(reason, "INSUFFICIENT_RATE_SUBMARKETS")

    def test_missing_no_change_outcome_is_rejected(self):
        def drop_no_change(event):
            event["markets"] = [
                market
                for market in event["markets"]
                if "no change" not in market["question"]
            ]

        _, reason = polymarket.validate_candidate_event(
            event_with_market_mutation(drop_no_change), MEETING_OCTOBER
        )
        self.assertEqual(reason, "NO_NO_CHANGE_OUTCOME")

    def test_duplicate_outcomes_are_rejected(self):
        def duplicate(event):
            event["markets"].append(copy.deepcopy(event["markets"][1]))

        _, reason = polymarket.validate_candidate_event(
            event_with_market_mutation(duplicate), MEETING_OCTOBER
        )
        self.assertEqual(reason, "DUPLICATE_OUTCOMES")

    def test_duplicate_token_ids_are_rejected(self):
        def share_token(event):
            first_tokens = json.loads(event["markets"][0]["clobTokenIds"])
            event["markets"][1]["clobTokenIds"] = json.dumps(first_tokens)

        _, reason = polymarket.validate_candidate_event(
            event_with_market_mutation(share_token), MEETING_OCTOBER
        )
        self.assertEqual(reason, "DUPLICATE_TOKEN_IDS")

    def test_missing_token_id_is_rejected(self):
        def strip_tokens(event):
            for market in event["markets"]:
                market["clobTokenIds"] = json.dumps(["", ""])

        _, reason = polymarket.validate_candidate_event(
            event_with_market_mutation(strip_tokens), MEETING_OCTOBER
        )
        self.assertEqual(reason, "MISSING_TOKEN_ID")

    def test_invalid_yes_price_is_rejected(self):
        def corrupt_price(event):
            for market in event["markets"]:
                market["outcomePrices"] = json.dumps(["not-a-number", "0.5"])

        _, reason = polymarket.validate_candidate_event(
            event_with_market_mutation(corrupt_price), MEETING_OCTOBER
        )
        self.assertEqual(reason, "INVALID_YES_PRICE")

    def test_outcome_sum_out_of_range_is_rejected(self):
        def scale_prices(event):
            for market in event["markets"]:
                market["outcomePrices"] = json.dumps(["0.9", "0.1"])

        _, reason = polymarket.validate_candidate_event(
            event_with_market_mutation(scale_prices), MEETING_OCTOBER
        )
        self.assertEqual(reason, "OUTCOME_SUM_OUT_OF_RANGE")

    def test_question_month_mismatch_is_rejected(self):
        def retitle_question(event):
            event["markets"][1]["question"] = (
                "Will the Fed decrease interest rates by 25 bps after the September 2026 meeting?"
            )

        _, reason = polymarket.validate_candidate_event(
            event_with_market_mutation(retitle_question), MEETING_OCTOBER
        )
        self.assertEqual(reason, "QUESTION_MEETING_MISMATCH")

    def test_resolve_mapping_is_ambiguous_when_two_candidates_pass(self):
        first = make_fed_decision_event(event_id="1")
        second = make_fed_decision_event(event_id="2")
        result = polymarket.resolve_mapping([first, second], MEETING_OCTOBER)
        self.assertEqual(result["mapping_status"], "AMBIGUOUS")
        self.assertIsNone(result["event"])
        self.assertEqual(result["ambiguous_event_ids"], ["1", "2"])

    def test_resolve_mapping_reports_not_found_with_same_date_candidates(self):
        unrelated = make_fed_decision_event(
            event_id="99",
            specs=[(-25, False, "0.5"), (0, False, "0.5")],
        )
        result = polymarket.resolve_mapping([unrelated], MEETING_OCTOBER)
        self.assertEqual(result["mapping_status"], "NOT_FOUND")
        self.assertEqual(
            result["same_date_candidates"],
            [
                {
                    "event_id": "99",
                    "event_title": "Fed Decision in October?",
                    "rejected_reason": "INSUFFICIENT_RATE_SUBMARKETS",
                }
            ],
        )


class DiscoveryTests(unittest.TestCase):
    def test_partial_source_failure_survives_with_warning(self):
        event = make_fed_decision_event()
        transport = FakeTransport()
        transport.add_json("gamma-api.polymarket.com/events", TransportError("HTTP 500"))
        transport.add_json(
            "public-search",
            lambda url, params: {"events": [event] if params["q"] == "FOMC" else []},
        )
        events, stats, warnings = polymarket.discover_candidate_events(transport)
        self.assertEqual([item["id"] for item in events], ["606422"])
        self.assertEqual(stats["tag_source_status"], "ERROR")
        self.assertTrue(any("tag listing failed" in warning for warning in warnings))

    def test_all_sources_failing_fails_closed(self):
        transport = FakeTransport()
        transport.add_json("gamma-api.polymarket.com/events", TransportError("HTTP 500"))
        transport.add_json("public-search", TransportError("HTTP 500"))
        with self.assertRaises(FedwatchError) as caught:
            polymarket.discover_candidate_events(transport)
        self.assertEqual(caught.exception.provider, "polymarket")
        self.assertEqual(caught.exception.code, "POLYMARKET_DISCOVERY_UNAVAILABLE")

    def test_empty_discovery_with_no_errors_is_reported_as_empty(self):
        transport = FakeTransport()
        transport.add_json("gamma-api.polymarket.com/events", [])
        transport.add_json("public-search", {"events": []})
        with self.assertRaises(FedwatchError) as caught:
            polymarket.discover_candidate_events(transport)
        self.assertEqual(caught.exception.code, "POLYMARKET_DISCOVERY_EMPTY")


def build_polymarket_transport(events, token_points) -> FakeTransport:
    transport = FakeTransport()
    transport.add_json(
        "gamma-api.polymarket.com/events",
        lambda url, params: events if params.get("tag_slug") == "fed-rates" else [],
    )
    transport.add_json(
        "public-search",
        lambda url, params: {"events": events if params["q"] == "FOMC" else []},
    )
    transport.add_json("prices-history", make_clob_history(token_points))
    return transport


def token_points_for(event: dict, now, age_days: float = 1.0, skip=(), future=(), ages=None):
    ages = dict(ages or {})
    points = {}
    for market in event["markets"]:
        token_ids = json.loads(market["clobTokenIds"])
        yes_token = token_ids[0]
        if yes_token in skip:
            points[yes_token] = []
            continue
        prices = json.loads(market["outcomePrices"])
        probability = float(prices[0])
        if yes_token in future:
            instant = now + timedelta(hours=1)
        else:
            instant = now - timedelta(days=ages.get(yes_token, age_days))
        points[yes_token] = [{"t": epoch(instant), "p": probability}]
    return points


class PriceRetrievalTests(unittest.TestCase):
    def test_build_section_current_mapping_and_prices(self):
        event = make_fed_decision_event()
        transport = build_polymarket_transport([event], token_points_for(event, NOW))
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        self.assertEqual(section["errors"], [])
        entry = section["meetings"][0]
        self.assertEqual(entry["mapping_status"], "VALIDATED")
        self.assertEqual(entry["data_status"], "CURRENT")
        self.assertEqual(entry["latest_observation_date"], "2026-09-27")
        self.assertAlmostEqual(entry["freshness_days"], 1.0, places=6)
        self.assertEqual(
            [outcome["probability_pct"] for outcome in entry["outcomes"]],
            [0.25, 0.45, 33.5, 65.5, 0.85],
        )
        for outcome in entry["outcomes"]:
            self.assertIsNotNone(outcome["source_timestamp"])
            self.assertTrue(outcome["token_id"])

    def test_stale_prices_are_reported_stale_not_current(self):
        event = make_fed_decision_event()
        transport = build_polymarket_transport(
            [event], token_points_for(event, NOW, age_days=10.0)
        )
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["data_status"], "STALE")
        self.assertAlmostEqual(entry["freshness_days"], 10.0, places=6)
        self.assertEqual(entry["freshness"]["status"], "STALE")
        self.assertTrue(any("freshness window" in warning for warning in section["warnings"]))
        self.assertIn(
            "POLYMARKET_MARKET_DATA_STALE", {error.code for error in section["errors"]}
        )

    def test_mixed_age_outcomes_are_stale_not_current(self):
        # One fresh quote must not mask four three-week-old quotes.
        event = make_fed_decision_event()
        fresh_token = "606422-tok-0"
        ages = {
            json.loads(market["clobTokenIds"])[0]: 20.0
            for market in event["markets"]
            if json.loads(market["clobTokenIds"])[0] != fresh_token
        }
        ages[fresh_token] = 1.0
        transport = build_polymarket_transport(
            [event], token_points_for(event, NOW, ages=ages)
        )
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["data_status"], "STALE")
        self.assertAlmostEqual(entry["freshness_days"], 20.0, places=6)
        self.assertEqual(entry["freshness"]["status"], "STALE")
        self.assertEqual(entry["latest_observation_date"], "2026-09-27")
        self.assertIn(
            "POLYMARKET_MARKET_DATA_STALE", {error.code for error in section["errors"]}
        )

    def test_within_tolerance_future_timestamp_is_current_with_skew_reported(self):
        event = make_fed_decision_event()
        points = {}
        for market in event["markets"]:
            token = json.loads(market["clobTokenIds"])[0]
            price = float(json.loads(market["outcomePrices"])[0])
            points[token] = [{"t": epoch(NOW + timedelta(seconds=60)), "p": price}]
        transport = build_polymarket_transport([event], points)
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["data_status"], "CURRENT")
        self.assertLess(entry["freshness_days"], 0.0)  # skew reported truthfully
        self.assertEqual(section["errors"], [])

    def test_malformed_epoch_degrades_to_null_not_crash(self):
        event = make_fed_decision_event()
        points = {
            json.loads(market["clobTokenIds"])[0]: [{"t": 1e18, "p": 0.5}]
            for market in event["markets"]
        }
        transport = build_polymarket_transport([event], points)
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["data_status"], "UNAVAILABLE")
        self.assertTrue(
            any("malformed CLOB price point" in warning for warning in section["warnings"])
        )

    def test_missing_outcome_price_is_partial_with_null(self):
        event = make_fed_decision_event()
        missing_token = "606422-tok-0"
        transport = build_polymarket_transport(
            [event], token_points_for(event, NOW, skip=(missing_token,))
        )
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["data_status"], "PARTIAL")
        missing = next(outcome for outcome in entry["outcomes"] if outcome["outcome_bp"] == 0)
        self.assertIsNone(missing["probability_pct"])
        self.assertTrue(section["warnings"])
        codes = {error.code for error in section["errors"]}
        self.assertIn("POLYMARKET_MARKET_DATA_PARTIAL", codes)

    def test_all_prices_missing_is_unavailable_with_provider_error(self):
        event = make_fed_decision_event()
        all_tokens = tuple(
            json.loads(market["clobTokenIds"])[0] for market in event["markets"]
        )
        transport = build_polymarket_transport(
            [event], token_points_for(event, NOW, skip=all_tokens)
        )
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["data_status"], "UNAVAILABLE")
        self.assertTrue(section["errors"])
        error = section["errors"][0]
        self.assertEqual(error.provider, "polymarket")
        self.assertEqual(error.code, "POLYMARKET_MARKET_DATA_UNAVAILABLE")

    def test_future_timestamp_is_rejected_not_reported_current(self):
        event = make_fed_decision_event()
        all_tokens = tuple(
            json.loads(market["clobTokenIds"])[0] for market in event["markets"]
        )
        transport = build_polymarket_transport(
            [event], token_points_for(event, NOW, future=all_tokens)
        )
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["data_status"], "UNAVAILABLE")
        self.assertTrue(any("future" in warning for warning in section["warnings"]))

    def test_price_history_transport_failure_maps_to_polymarket_provider(self):
        event = make_fed_decision_event()
        transport = FakeTransport()
        transport.add_json(
            "gamma-api.polymarket.com/events",
            lambda url, params: [event] if params.get("tag_slug") == "fed-rates" else [],
        )
        transport.add_json(
            "public-search", {"events": [event]}
        )
        transport.add_json("prices-history", TransportError("HTTP 500"))
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["data_status"], "UNAVAILABLE")
        self.assertEqual(section["errors"][0].provider, "polymarket")

    def test_unverified_meeting_has_no_outcomes_and_no_error(self):
        event = make_fed_decision_event()
        transport = build_polymarket_transport([event], token_points_for(event, NOW))
        section = polymarket.build_section(
            transport, [MEETING_DECEMBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        entry = section["meetings"][0]
        self.assertEqual(entry["mapping_status"], "NOT_FOUND")
        self.assertEqual(entry["outcomes"], [])
        self.assertIsNone(entry["data_status"])
        self.assertEqual(section["errors"], [])

    def test_partial_discovery_source_outage_is_a_provider_error(self):
        event = make_fed_decision_event()
        transport = FakeTransport()
        transport.add_json("gamma-api.polymarket.com/events", TransportError("HTTP 500"))
        transport.add_json(
            "public-search",
            lambda url, params: {"events": [event] if params["q"] == "FOMC" else []},
        )
        transport.add_json("prices-history", make_clob_history(token_points_for(event, NOW)))
        section = polymarket.build_section(
            transport, [MEETING_OCTOBER], clock=FixedClock(NOW), sleep=NO_SLEEP
        )
        codes = {error.code for error in section["errors"]}
        self.assertIn("POLYMARKET_DISCOVERY_PARTIAL", codes)
        self.assertEqual(section["meetings"][0]["mapping_status"], "VALIDATED")


if __name__ == "__main__":
    unittest.main()
