"""Permanent no-loss and harmless-variation gate; never runs or needs git."""
import hashlib
import json
import unittest
from pathlib import Path

import fedwatch_test_support  # bootstraps only current production imports
from fedwatch_acceptance_cases import FIXTURES, NOW, cases, fixture_digest
from datetime import date, datetime
from fedwatch_acceptance_runner import run_case

# Narrow identity-level exceptions approved by the owner. No error-code,
# provider-level or arbitrary scenario-wide waiver is permitted.
EXCEPTIONS = {
    "MEETING_BOUNDARY": {"investing:2026-10-28"},
    "DUPLICATE_BUCKET": {"investing:2026-10-28"},
    "OVERLAP": {"investing:2026-10-28"},
    "PREDECESSOR": {"local:2027-01-27"},
    "CALENDAR_REVERSE": {"calendar:2026-10-28:2026-10-29"},
    "CALENDAR_CONFLICT": {"calendar:2026-10-28:2026-10-27", "calendar:2026-10-28:2026-10-26"},
    "FRED_CONFLICT": {"fred:2026-09-28:4.0", "fred:2026-09-28:5.0"},
    "FRED_FUTURE": {"history:upper:2026-10-05", "history:lower:2026-10-05"},
    "NONBINARY": {"polymarket:2026-10-28:-50", "mapping_market:2026-10-28:-50"},
    "CLOB_CONFLICT": {"polymarket:2026-10-28:-50"},
    "FIRST_MEETING_AFTER_DECISION": {"local:2026-10-28"},
}


class AcceptanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((FIXTURES / "acceptance_manifest_65cc9b356.json").read_text(encoding="utf-8"))
        cls.inputs = cases()
        cls.current = {name: run_case(case) for name, case in cls.inputs.items()}

    def test_manifest_identity_covers_every_fixture_and_scenario(self):
        self.assertEqual(self.manifest["original_commit"], "65cc9b35683d740b4eab798b10a59072a5d5e533")
        self.assertEqual(set(self.inputs), set(self.manifest["cases"]))
        self.assertEqual(self.manifest["fixture_hash_convention"], "UTF8_BYTES_CRLF_AND_CR_TO_LF")
        actual = {path.name: fixture_digest(path.read_bytes()) for path in FIXTURES.iterdir()
                  if path.suffix in (".html", ".csv", ".json") and not path.name.startswith("acceptance_manifest_")}
        self.assertEqual(actual, self.manifest["fixture_sha256"])
        for name, case in self.inputs.items():
            with self.subTest(case=name):
                self.assertEqual(hashlib.sha256(json.dumps(case, sort_keys=True).encode()).hexdigest(),
                                 self.manifest["cases"][name]["input_sha256"])

    def test_rule_a_every_original_unit_retains_its_value(self):
        for name, case in self.inputs.items():
            allowed = EXCEPTIONS[case["exception"]] if case["exception"] else set()
            for identity, original in self.manifest["cases"][name]["units"].items():
                if identity in allowed:
                    continue
                with self.subTest(case=name, unit=identity):
                    self.assertIn(identity, self.current[name]["units"])
                    self.assertEqual(original, self.current[name]["units"][identity])

    def test_rule_b_harmless_variations_preserve_all_units_and_values(self):
        for name, case in self.inputs.items():
            if case["base"]:
                with self.subTest(case=name):
                    self.assertEqual(self.current[case["base"]]["units"], self.current[name]["units"])
                    self.assertIsNone(self.current[name]["error"])

    def test_each_exception_proves_an_original_wrong_result(self):
        for name, case in self.inputs.items():
            if not case["exception"]:
                continue
            original = self.manifest["cases"][name]["units"]
            allowed = EXCEPTIONS[case["exception"]]
            with self.subTest(exception=case["exception"]):
                self.assertTrue(set(original) & allowed, "exception must actually exercise an original accepted wrong result")
                self.assertTrue((set(original) & allowed).isdisjoint(self.current[name]["units"]))
        # Independent reasons why these values are wrong, rather than treating
        # disagreement with today's implementation as justification.
        old = self.manifest["cases"]
        self.assertIn("investing:2026-10-28", old["exception/meeting_boundary"]["units"])
        self.assertNotIn("investing:2026-12-09", old["exception/meeting_boundary"]["units"])
        buckets = old["exception/overlap"]["units"]["investing:2026-10-28"]
        self.assertGreater(buckets[0]["rate_high"], buckets[1]["rate_low"])
        reverse = next(iter(old["exception/calendar_reverse"]["units"].values()))
        self.assertGreater(reverse["start_date"], reverse["end_date"])
        identities = list(old["exception/calendar_conflict"]["units"].values())
        self.assertEqual(identities[0]["end_date"], identities[1]["end_date"])
        self.assertNotEqual(identities[0]["start_date"], identities[1]["start_date"])
        self.assertEqual(len([key for key in old["exception/fred_conflict"]["units"] if key.startswith("fred:2026-09-28:")]), 2)
        as_of = datetime.fromisoformat(NOW).date()
        future_original = {key: value for key, value in old["exception/fred_future"]["units"].items()
                           if date.fromisoformat(key.rsplit(":", 1)[1]) > as_of}
        self.assertEqual(set(future_original), EXCEPTIONS["FRED_FUTURE"])
        self.assertTrue(set(future_original).isdisjoint(self.current["exception/fred_future"]["units"]))
        self.assertEqual(json.loads(self.inputs["exception/nonbinary"]["payload"]["markets"][0]["outcomes"]), ["Yes", "Yes"])
        self.assertIn("local:2027-01-27", old["exception/predecessor"]["units"])
        self.assertNotIn("local:2026-12-09", old["exception/predecessor"]["units"])
        duplicate = self.inputs["exception/duplicate_bucket"]["payload"]
        self.assertIn("100%", duplicate)
        self.assertIn("90%", duplicate)
        self.assertTrue(self.inputs["exception/clob_conflict"]["conflict_points"])
        # The original first local is seeded by the 2026-09-28 midpoint despite
        # the explicit 2026-09-30 decision row; later adjacent differences agree.
        first = old["round5/snapshot/after_decision"]["units"]["local:2026-10-28"]
        self.assertEqual(first, old["scenario/snapshot_six_day_pair"]["units"]["local:2026-10-28"])
        # Fixture E[first] = .30*3.875 + .70*4.125 = 4.05. The original
        # subtracts the pre-decision pair midpoint 3.875: +.175 -> 70% +25bp.
        self.assertEqual(first, [{"outcome_bp": 0, "probability_pct": 30.0},
                                 {"outcome_bp": 25, "probability_pct": 70.0}])
        self.assertIn("September</strong></div><div class=\"fomc-meeting__date\">30", self.inputs["round5/snapshot/after_decision"]["payload"])

    def test_required_scenarios_are_positive_and_hidden_bucket_stays_caught(self):
        captured = "fixture/investing_fed_rate_monitor_2026-09-28.html"
        for suffix in ("", "/after_percentage", "/before_label"):
            self.assertEqual(len(self.current[captured + suffix]["units"]), 10)
        snapshot = self.current["scenario/snapshot_six_day_pair"]["units"]
        self.assertEqual(sum(key.startswith("local:") for key in snapshot), 10)
        self.assertEqual(sum(key.startswith("comparison:") for key in snapshot), 2)
        for age in ("fresh", "stale", "unknown"):
            self.assertEqual(len(self.current["scenario/partial_calendar_" + age]["units"]), 10)
            downstream = self.current["scenario/snapshot_partial_calendar_" + age]["units"]
            self.assertEqual(sum(key.startswith("mapping:") for key in downstream), 2)
            self.assertEqual(sum(key.startswith("polymarket:") for key in downstream), 10)
            self.assertEqual(sum(key.startswith("comparison:") for key in downstream), 2)
        self.assertEqual(len(self.current["scenario/fed_side_six_day_pair"]["units"]), 10)
        self.assertEqual(set(self.current["scenario/hidden_bucket"]["units"]), {"investing:2026-10-28"})
        self.assertIn("reconstruction:2026-10-04", self.current["synthetic/monthly/empty"]["units"])
        self.assertIn("published:2026-10-02:400", self.current["synthetic/published/empty"]["units"])

    def test_fixture_identities_survive_lf_and_crlf_checkouts(self):
        for name, expected in self.manifest["fixture_sha256"].items():
            raw = (FIXTURES / name).read_bytes().replace(b"\r\n", b"\n")
            with self.subTest(fixture=name):
                self.assertEqual(fixture_digest(raw), expected)
                self.assertEqual(fixture_digest(raw.replace(b"\n", b"\r\n")), expected)
                self.assertNotEqual(fixture_digest(raw + b"changed observation"), expected)

    def test_round5_first_only_dependency_and_decision_exception(self):
        for mode in ("snapshot", "fed_side"):
            for scenario, count in (("stale_partial", 10), ("fred_failure", 9), ("after_decision", 9)):
                key = "round5/" + mode + "/" + scenario
                with self.subTest(case=key):
                    self.assertEqual(len(self.current[key]["units"]), count)
                    if count == 9:
                        self.assertNotIn("local:2026-10-28", self.current[key]["units"])


if __name__ == "__main__":
    unittest.main()
