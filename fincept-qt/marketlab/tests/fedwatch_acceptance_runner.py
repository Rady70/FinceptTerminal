"""Run unchanged production interfaces; usable with archived or current code."""
import json
import tempfile
from contextlib import closing
from datetime import date, datetime, timedelta
from pathlib import Path

from fedwatch_acceptance_cases import FIXTURES, NOW, cases


class FixtureTransport:
    def __init__(self, now, event=None, conflict_points=False):
        self.text = {key: (FIXTURES / name).read_text(encoding="utf-8") for key, name in (
            ("DFEDTARU", "fred_DFEDTARU_recent.csv"), ("DFEDTARL", "fred_DFEDTARL_recent.csv"),
            ("fomccalendars", "fomc_calendar_2026-09-28.html"),
            ("fed-rate-monitor", "investing_fed_rate_monitor_2026-09-28.html"))}
        self.events = [event] if event else [json.loads((FIXTURES / name).read_text()) for name in (
            "polymarket_event_606422_october.json", "polymarket_event_770450_december.json")]
        self.points = {}
        for entry in self.events:
            for market in entry.get("markets", []):
                # Stable semantic Yes token, including case/order variations.
                labels = [label.strip().lower() for label in json.loads(market["outcomes"])]
                index = labels.index("yes") if "yes" in labels else 0
                token = json.loads(market["clobTokenIds"])[index]
                probability = float(json.loads(market["outcomePrices"])[index])
                point = {"t": int((now - timedelta(days=1)).timestamp()), "p": probability}
                self.points[token] = [point]
        if conflict_points:
            token = next(iter(self.points))
            self.points[token].append(dict(self.points[token][0], p=.8))

    def get_text(self, url, **kwargs):
        value = next(value for key, value in self.text.items() if key in url)
        if isinstance(value, Exception):
            raise value
        return value

    def get_json(self, url, params=None, **kwargs):
        params = params or {}
        if "prices-history" in url:
            return {"history": self.points.get(params.get("market"), [])}
        if "public-search" in url:
            return {"events": self.events if params.get("q") == "FOMC" else []}
        return self.events if params.get("tag_slug") == "fed-rates" else []


def run_case(case):
    from fedwatch import fomc, fred, investing, monthly_csv, polymarket, published_history, snapshot, zq
    from fedwatch.errors import FedwatchError
    from fedwatch.store import FedwatchHistoryStore

    now = datetime.fromisoformat(NOW)
    clock = lambda: now
    transport = FixtureTransport(now)
    units = {}

    def capture(key, value):
        # Dates and floats retain semantic values; no clocks, source paths,
        # quality labels or new diagnostics are mistaken for financial units.
        units[key] = json.loads(json.dumps(value, default=lambda obj: obj.isoformat(), allow_nan=False))

    def fed_units(result):
        for meeting in result["meetings"]:
            capture("investing:" + meeting["meeting_date"], meeting["normalized_probabilities"])

    kind, payload = case["kind"], case["payload"]
    try:
        if kind == "investing_parse":
            # Isolate the original boundary defect at its public parser plus
            # normalizer interfaces. Original provider coverage does reject
            # this page; do not describe these as provider-accepted units.
            rows = investing.parse_fed_rate_monitor(payload)[0]
            normalized, _ = investing.normalize_cumulative(rows)
            for day in sorted({row["meeting_date"] for row in normalized}):
                capture("investing:" + day, [{key: row[key] for key in ("rate_low", "rate_high", "probability_pct")}
                                            for row in normalized if row["meeting_date"] == day])
        elif kind == "investing":
            transport.text["fed-rate-monitor"] = payload
            fed_units(investing.fetch_distributions(transport, clock=clock))
        elif kind == "calendar":
            for row in fomc.parse_fomc_calendar(payload)[0]:
                capture("calendar:" + row["end_date"].isoformat() + ":" + row["start_date"].isoformat(),
                    {key: row[key] for key in ("start_date", "end_date", "meeting_type", "has_projection_materials")})
        elif kind in ("calendar_fetch", "snapshot_calendar"):
            transport.text["fomccalendars"] = payload
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "fallback.csv"
                # Change capture metadata only; the original archived rows
                # remain the official fallback observations.
                lines = fomc.FALLBACK_PATH.read_text(encoding="utf-8").splitlines()
                lines = [line for line in lines if not line.startswith("#")]
                if case["capture"]:
                    lines.insert(0, "# snapshot_retrieved_at=" + case["capture"])
                path.write_text("\n".join(lines) + "\n", encoding="utf-8")
                if kind == "calendar_fetch":
                    result = fomc.fetch_calendar(transport, fallback_path=path, clock=clock)
                    for row in fomc.upcoming_meetings(result["meetings"], now.date()):
                        capture("calendar:" + row["end_date"].isoformat(),
                                {key: row[key] for key in ("start_date", "end_date", "meeting_type", "has_projection_materials")})
                else:
                    # Isolate mapping authority from the separate age rule:
                    # a two-day paired FRED observation is current either way.
                    transport.text["DFEDTARU"] = "DATE,DFEDTARU\n2026-10-02,4\n"
                    transport.text["DFEDTARL"] = "DATE,DFEDTARL\n2026-10-02,3.75\n"
                    result = snapshot.build_snapshot(transport, clock=clock, fallback_path=path, sleep=lambda _: None)
                    for row in result["data"]["meetings"]:
                        if row.get("fed_side") and row["fed_side"].get("local_probabilities"):
                            capture("local:" + row["meeting_date"], row["fed_side"]["local_probabilities"])
                        if row.get("polymarket") and row["polymarket"]["mapping_status"] == "VALIDATED":
                            capture("mapping:" + row["meeting_date"], row["polymarket"]["event_id"])
                            for outcome in row["polymarket"]["outcomes"]:
                                if outcome["probability_pct"] is not None:
                                    capture("polymarket:" + row["meeting_date"] + ":" + str(outcome["outcome_bp"]),
                                            outcome["probability_pct"])
                        if row.get("comparison"):
                            capture("comparison:" + row["meeting_date"], row["comparison"])
        elif kind == "round5_local":
            from unittest.mock import patch
            from fedwatch.transport import TransportError
            transport.text["fomccalendars"] = payload
            if case.get("fred_failure"):
                transport.text["DFEDTARU"] = TransportError("synthetic FRED provider failure")
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "fallback.csv"
                lines = [line for line in fomc.FALLBACK_PATH.read_text().splitlines() if not line.startswith("#")]
                path.write_text("# snapshot_retrieved_at=2026-01-01T00:00:00Z\n" + "\n".join(lines))
                with patch.object(fomc, "FALLBACK_PATH", path):
                    result = (snapshot.build_snapshot(transport, clock=clock, fallback_path=path, sleep=lambda _: None)["data"]
                              if case["mode"] == "snapshot" else snapshot.build_fed_side_command(transport, clock=clock))
                for row in result["meetings"]:
                    fed = row.get("fed_side") if case["mode"] == "snapshot" else row
                    if fed and fed.get("local_probabilities"):
                        capture("local:" + row["meeting_date"], fed["local_probabilities"])
        elif kind == "fred":
            for row in fred.parse_fred_csv(payload):
                capture(f'fred:{row["date"].isoformat()}:{row["value"]}', row)
            bound = "DFEDTARL" if "DFEDTARL" in payload else "DFEDTARU"
            transport.text[bound] = payload
            result = fred.fetch_target_range(transport, clock=clock)
            capture("target", {"range": result["target_range"], "date": result["latest_observation_date"]})
        elif kind == "fred_history":
            transport.text["DFEDTARU"] = "DATE,DFEDTARU\n2026-09-28,4\n2026-10-05,5\n"
            transport.text["DFEDTARL"] = "DATE,DFEDTARL\n2026-09-28,3.75\n2026-10-05,4.75\n"
            result = fred.fetch_target_history(transport, clock=clock)
            for bound in ("upper", "lower"):
                for row in result[bound]:
                    capture("history:" + bound + ":" + row["date"], row["value"])
        elif kind == "target":
            result = fred.fetch_target_range(transport, clock=clock)
            capture("target", {"range": result["target_range"], "date": result["latest_observation_date"]})
        elif kind == "fed_side":
            result = snapshot.build_fed_side_command(transport, clock=clock)
            for row in result["meetings"]:
                if row.get("local_probabilities"):
                    capture("local:" + row["meeting_date"], row["local_probabilities"])
        elif kind == "snapshot":
            if payload:
                transport.text["fed-rate-monitor"] = payload
            result = snapshot.build_snapshot(transport, clock=clock, sleep=lambda _: None)
            target = result["data"]["current_target_range"]
            if target:
                capture("target", {key: target[key] for key in ("lower", "upper", "latest_observation_date")})
            for row in result["data"]["meetings"]:
                fed = row.get("fed_side")
                if fed and fed.get("local_probabilities"):
                    capture("local:" + row["meeting_date"], fed["local_probabilities"])
                if row.get("comparison"):
                    capture("comparison:" + row["meeting_date"], row["comparison"])
        elif kind == "local_gap":
            raw = [{"meeting_date": day, "rate_low": low, "rate_high": low + .25, "probability_pct": 100}
                   for day, low in (("2026-10-28", 3.75), ("2027-01-27", 4))]
            rows, records = investing.normalize_cumulative(raw)
            result = {"source": "synthetic", "normalized_rows": rows, "meetings": [
                {"meeting_date": record["meeting_date"], "raw_probabilities": [], "normalized_probabilities": [],
                 "normalization": {"normalized_expected_rate": record["normalized_expected_rate"]}} for record in records],
                "parse_report": {"reported_meeting_dates": ["2026-10-28", "2026-12-09", "2027-01-27"]}}
            for row in investing.with_local_probabilities(result, 4, 3.75):
                if row["local_probabilities"]:
                    capture("local:" + row["meeting_date"], row["local_probabilities"])
        elif kind == "polymarket":
            transport = FixtureTransport(now, payload, case.get("conflict_points", False))
            result = polymarket.build_section(transport, [date(2026, 10, 28), date(2026, 12, 9)],
                                              clock=clock, sleep=lambda _: None)
            for row in result["meetings"]:
                if row["mapping_status"] == "VALIDATED":
                    capture("mapping:" + row["meeting_date"], row["event_id"])
                    for outcome in row["outcomes"]:
                        if outcome.get("binary_outcomes_valid") is not False:
                            capture("mapping_market:" + row["meeting_date"] + ":" + str(outcome["outcome_bp"]),
                                {key: outcome[key] for key in ("market_id", "token_id", "open_ended")})
                        if outcome["probability_pct"] is not None:
                            capture("polymarket:" + row["meeting_date"] + ":" + str(outcome["outcome_bp"]),
                                {key: outcome[key] for key in ("outcome_bp", "open_ended", "probability_pct")})
        elif kind in ("monthly", "barchart"):
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "ZQV26.csv"
                path.write_text(payload, encoding="utf-8", newline="")
                rows, _ = (monthly_csv if kind == "monthly" else zq).load_contracts(tmp)
                for row in rows:
                    capture("contract:" + row["date"].isoformat(),
                            {key: row[key] for key in ("contract_symbol", "date", "close_price")})
                buffer = [dict(row, contract_symbol="ZQX26", contract_month=11) for row in rows]
                for watch in (date(2026, 10, 1), date(2026, 10, 4)):
                    try:
                        result = zq.run_deconvolution(watch, [date(2026, 10, 28)], rows + buffer, 4, 3.75,
                                                     selected_meeting=date(2026, 10, 28))
                        if result["rows"]:
                            capture("reconstruction:" + watch.isoformat(), result["rows"])
                    except (FedwatchError, ValueError):
                        pass
        elif kind == "published":
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "published.csv"
                path.write_text(payload, encoding="utf-8", newline="")
                with closing(FedwatchHistoryStore(Path(tmp) / "history.db")) as store:
                    published_history.import_file(store, path, "2026-10-28", "https://www.cmegroup.com/fedwatch", clock=clock)
                    for row in store.observations(meeting_date="2026-10-28"):
                        capture("published:" + row["observed_at"][:10] + ":" + str(row["outcome_bp"]),
                            {key: row[key] for key in ("outcome_bp", "probability_pct", "raw_probability_pct")})
        else:
            raise AssertionError("uncovered kind " + kind)
    except (FedwatchError, ValueError, TypeError, AttributeError, KeyError) as exc:
        return {"units": units, "error": getattr(exc, "code", type(exc).__name__)}
    return {"units": units, "error": None}


if __name__ == "__main__":
    import fedwatch
    print(json.dumps({"package_path": str(Path(fedwatch.__file__).resolve()),
                      "results": {name: run_case(case) for name, case in cases().items()}}, sort_keys=True))
