"""Acquisition tolerates provider format drift without losing valid data.

Websites restyle their pages and reword listings. These tests change markup,
date spelling, column order and market wording around the captured fixtures
and require the same accepted values, while values that are actually invalid
(contradictory copies, impossible sums, other months, combinations) are still
rejected. No network is used.
"""

from __future__ import annotations

import copy
import json
import re
import unittest
from datetime import date

from fedwatch_test_support import (
    FIXTURE_INVESTING_LIVE,
    FIXTURE_PM_OCTOBER,
    FakeTransport,
    FixedClock,
    fixture_json,
    fixture_text,
    utc,
)

from fedwatch import investing, polymarket

CAPTURE = utc(2026, 9, 28, 14)


def fetch(html):
    return investing.fetch_distributions(FakeTransport().add_text("fed-rate-monitor", html), clock=FixedClock(CAPTURE))


class InvestingDriftTests(unittest.TestCase):
    def setUp(self):
        self.html = fixture_text(FIXTURE_INVESTING_LIVE)
        self.base = fetch(self.html)

    def distributions(self, result):
        return {m["meeting_date"]: m["normalized_probabilities"] for m in result["meetings"]}

    def test_restyled_cards_and_long_dates_keep_every_meeting(self):
        drifted = self.html.replace("<span>Meeting Time:</span>", '<span class="caption">Meeting time</span>')
        drifted = drifted.replace("<i></i>", '<em class="bar-icon">&#9650;</em>')
        drifted = re.sub(r"(Oct|Dec|Jan|Mar|Apr|Jun|Jul|Sep) (\d\d), (\d{4}) 02:00PM ET",
                         lambda m: {"Oct": "October", "Dec": "December", "Jan": "January", "Mar": "March",
                                    "Apr": "April", "Jun": "June", "Jul": "July", "Sep": "September"}[m.group(1)]
                         + f" {int(m.group(2))}, {m.group(3)} 2:00 PM ET", drifted)
        result = fetch(drifted)
        self.assertEqual(self.distributions(result), self.distributions(self.base))
        self.assertEqual([e["code"] for e in result["errors"]], [e["code"] for e in self.base["errors"]])

    def test_unreadable_bars_are_recovered_from_the_card_table(self):
        december = next(m for m in self.base["meetings"] if m["meeting_date"] == "2026-12-09")
        low = december["raw_probabilities"][0]
        label = f"{low['rate_low']:.2f} - {low['rate_high']:.2f}"
        start = self.html.index("Dec 09, 2026 02:00PM ET")
        position = self.html.index(f"<span>{label}</span>", start)
        broken = self.html[:position] + "<span>range unavailable</span>" + self.html[position + len(label) + 13:]
        result = fetch(broken)
        self.assertIn("2026-12-09", result["parse_report"]["table_recovered_meeting_dates"])
        self.assertEqual(self.distributions(result)["2026-12-09"], self.distributions(self.base)["2026-12-09"])
        self.assertTrue(any("read from the card's table" in w for w in result["warnings"]))

    def test_table_columns_are_found_by_their_headers(self):
        # Reorder the two previous columns (headers and cells) without changing meaning.
        def swap(match):
            return match.group(1) + match.group(3) + match.group(2)
        swapped = re.sub(r"(<th>Current Probability%</th>\s*)(<th>Previous Day Probability%</th>\s*)"
                         r"(<th>Previous Week Probability%</th>)", swap, self.html)
        swapped = re.sub(r"(</td>\s*<td>[^<]*</td>\s*)(<td>[^<]*</td>\s*)(<td>[^<]*</td>)(\s*</tr>)",
                         lambda m: m.group(1) + m.group(3) + " " + m.group(2) + m.group(4), swapped)
        self.assertNotEqual(swapped, self.html)
        base = investing.parse_displayed_context(self.html)["2026-10-28"]["table"]
        moved = investing.parse_displayed_context(swapped)["2026-10-28"]["table"]
        self.assertTrue(base)
        self.assertEqual(base, moved)

    def test_contradictions_and_invalid_sums_are_still_rejected(self):
        # Two intact copies that disagree remain a labelled conflict, never blended.
        html = ('<div class="infoFed"><span>Meeting Time:</span><i>Oct 28, 2026 02:00PM ET</i></div>'
                '<div class="percfedRateWrap"><div class="percfedRateItem"><span>3.75 - 4.00</span><i></i>'
                '<div></div><span>60.0%</span></div></div>')
        bad_sum = fetch(html + html.replace("Oct 28", "Dec 09").replace("60.0%", "100.0%"))
        self.assertNotIn("2026-10-28", self.distributions(bad_sum))
        self.assertIn("2026-12-09", self.distributions(bad_sum))


class PolymarketWordingTests(unittest.TestCase):
    def setUp(self):
        self.event = fixture_json(FIXTURE_PM_OCTOBER)
        self.meeting = date(2026, 10, 28)

    def validate(self, event):
        return polymarket.validate_candidate_event(event, self.meeting)

    def test_reworded_titles_for_one_decision_still_validate(self):
        for title in ("October Fed decision?", "Fed rate decision in October 2026?", "FOMC decision: October"):
            event = dict(self.event, title=title)
            evidence, reason = self.validate(event)
            self.assertIsNotNone(evidence, (title, reason))

    def test_combinations_dissent_and_other_months_are_not_decisions(self):
        for title in ("Fed decisions (Oct-Jan)", "Fed Decision & Dissent Combo in October?",
                      "How many dissent at the October Fed meeting?", "Fed decision in October or December?",
                      "Fed decision in December?"):
            evidence, reason = self.validate(dict(self.event, title=title))
            self.assertIsNone(evidence, title)

    def test_questions_may_omit_the_month_but_never_name_another(self):
        event = copy.deepcopy(self.event)
        for market in event["markets"]:
            market["question"] = re.sub(r"\s*after the October 2026 meeting", "", market["question"])
        self.assertIsNotNone(self.validate(event)[0])
        event["markets"][0]["question"] += " (December meeting)"
        self.assertEqual(self.validate(event)[1], "QUESTION_MEETING_MISMATCH")

    def test_rate_wording_and_units_vary(self):
        self.assertEqual(polymarket.parse_bp_outcome("Fed cuts 25bp in October?"), {"bp_delta": -25, "open_ended": False})
        self.assertEqual(polymarket.parse_bp_outcome("Will the Fed raise rates 50+ basis points?"),
                         {"bp_delta": 50, "open_ended": True})
        self.assertEqual(polymarket.parse_bp_outcome("Fed holds rates in October?"), {"bp_delta": 0, "open_ended": False})
        self.assertIsNone(polymarket.parse_bp_outcome("Will the Fed cut after the October 2026 meeting?"))

    def test_missing_status_flags_do_not_reject_but_explicit_closed_does(self):
        event = {k: v for k, v in self.event.items() if k not in ("active", "closed")}
        self.assertIsNotNone(self.validate(event)[0])
        self.assertEqual(self.validate(dict(self.event, closed=True))[1], "EVENT_NOT_ACTIVE_AND_OPEN")


if __name__ == "__main__":
    unittest.main()
