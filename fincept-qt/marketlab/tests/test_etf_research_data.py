"""Fixture tests for the ETF research acquisition script (no network).

Every Yahoo, FRED and World Bank call is replaced by an in-memory fake, so the
tests exercise the project-owned conversion rules only: completed-session
filtering, missing versus zero, per-item and per-stage status, the constituent
policy, the paginated-response refusal and the atomic payload write.
"""

from __future__ import annotations

import datetime as dt
import io
import json
import math
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

_SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

import etf_research_data  # noqa: E402
from etf_research import macro, sessions, yahoo  # noqa: E402
from fedwatch.transport import Transport, TransportError  # noqa: E402

UTC = dt.timezone.utc
NAN = float("nan")


class _Stamp:
    """A pandas-Timestamp stand-in: only ``date()`` is used."""

    def __init__(self, day: dt.date):
        self._day = day

    def date(self) -> dt.date:
        return self._day


class _Row(dict):
    pass


class _Frame:
    """The slice of a pandas DataFrame that ``frame_to_bars`` touches."""

    def __init__(self, rows, columns=("Close", "Volume", "Dividends", "Capital Gains", "Stock Splits")):
        self._rows = rows
        self.columns = list(columns)

    def __len__(self):
        return len(self._rows)

    def iterrows(self):
        for day, rec in self._rows:
            yield _Stamp(day), _Row(rec)


class _FakeYf:
    def __init__(self, frame=None, error=None, errors=None):
        self._frame = frame
        self._error = error
        self.shared = type("S", (), {"_ERRORS": errors or {}})()
        self.calls = []

    def download(self, batch, **kwargs):
        self.calls.append((list(batch), kwargs))
        if self._error:
            raise self._error
        return self._frame


class _FakeTransport(Transport):
    def __init__(self, text=None, payload=None, error=None):
        self.text, self.payload, self.error = text, payload, error
        self.urls = []

    def get_text(self, url, headers=None, timeout=20):
        self.urls.append(url)
        if self.error:
            raise self.error
        return self.text

    def get_json(self, url, params=None, timeout=20):
        self.urls.append(url)
        if self.error:
            raise self.error
        return self.payload


class CompletedSessionTests(unittest.TestCase):
    def test_us_bar_is_in_progress_until_close_plus_buffer(self):
        day = dt.date(2026, 10, 2)  # Friday; 16:00 ET close = 20:00 UTC (EDT)
        self.assertFalse(sessions.is_completed_session("SPY", day, dt.datetime(2026, 10, 2, 17, 8, tzinfo=UTC)))
        self.assertFalse(sessions.is_completed_session("SPY", day, dt.datetime(2026, 10, 2, 20, 29, tzinfo=UTC)))
        self.assertTrue(sessions.is_completed_session("SPY", day, dt.datetime(2026, 10, 2, 20, 30, tzinfo=UTC)))
        self.assertTrue(sessions.is_completed_session("SPY", dt.date(2026, 10, 1),
                                                      dt.datetime(2026, 10, 2, 17, 8, tzinfo=UTC)))

    def test_exchange_suffixes_use_their_own_close(self):
        day = dt.date(2026, 10, 2)
        at = dt.datetime(2026, 10, 2, 10, 0, tzinfo=UTC)  # 17:00 Bangkok, 06:00 New York
        self.assertTrue(sessions.is_completed_session("PTT.BK", day, at))  # 16:30 + 30 min
        self.assertTrue(sessions.is_completed_session("^SET.BK", day, at))  # index carries the suffix
        self.assertFalse(sessions.is_completed_session("^VIX", day, at))  # bare caret is U.S.
        self.assertTrue(sessions.is_completed_session("005930.KQ", day, at))  # 15:30 Seoul + 30 min
        self.assertEqual(sessions.exchange_rule("005930.KQ")[0], "Asia/Seoul")

    def test_unknown_suffix_is_conservative(self):
        zone, _, _, known = sessions.exchange_rule("ABC.XX")
        self.assertFalse(known)
        at = dt.datetime(2026, 10, 2, 23, 59, tzinfo=UTC)
        self.assertFalse(sessions.is_completed_session("ABC.XX", dt.date(2026, 10, 2), at))
        self.assertTrue(sessions.is_completed_session("ABC.XX", dt.date(2026, 10, 1), at))


class FrameToBarsTests(unittest.TestCase):
    AT = dt.datetime(2026, 10, 3, 12, 0, tzinfo=UTC)

    def test_missing_volume_is_none_and_absent_events_are_zero(self):
        frame = _Frame([
            (dt.date(2026, 9, 30), {"Close": 10.0, "Volume": NAN, "Dividends": NAN, "Capital Gains": NAN,
                                    "Stock Splits": NAN}),
            (dt.date(2026, 10, 1), {"Close": 10.5, "Volume": 0.0, "Dividends": 0.12, "Capital Gains": 0.0,
                                    "Stock Splits": 2.0}),
        ])
        out = yahoo.frame_to_bars("XLB", frame, self.AT)
        self.assertEqual(out["rows"][0], ["2026-09-30", 10.0, None, 0.0, 0.0, 0.0])
        # A reported zero volume stays a zero; it is an observation, not a gap.
        self.assertEqual(out["rows"][1], ["2026-10-01", 10.5, 0.0, 0.12, 0.0, 2.0])

    def test_missing_or_non_positive_close_is_not_a_bar(self):
        frame = _Frame([
            (dt.date(2026, 9, 29), {"Close": NAN, "Volume": 5.0}),
            (dt.date(2026, 9, 30), {"Close": 0.0, "Volume": 5.0}),
            (dt.date(2026, 10, 1), {"Close": math.inf, "Volume": 5.0}),
            (dt.date(2026, 10, 2), {"Close": 11.0, "Volume": 5.0}),
        ], columns=("Close", "Volume"))
        out = yahoo.frame_to_bars("XLB", frame, self.AT)
        self.assertEqual(out["dropped_no_close"], 3)
        self.assertEqual([r[0] for r in out["rows"]], ["2026-10-02"])
        self.assertEqual(out["rows"][0][3:], [0.0, 0.0, 0.0])  # no event columns at all: no events

    def test_in_progress_session_is_dropped_and_counted(self):
        at = dt.datetime(2026, 10, 2, 17, 8, tzinfo=UTC)  # before the U.S. close
        frame = _Frame([(dt.date(2026, 10, 1), {"Close": 10.0}), (dt.date(2026, 10, 2), {"Close": 10.2})],
                       columns=("Close",))
        out = yahoo.frame_to_bars("SPY", frame, at)
        self.assertEqual(out["in_progress_excluded"], 1)
        self.assertEqual([r[0] for r in out["rows"]], ["2026-10-01"])

    def test_empty_frame(self):
        self.assertEqual(yahoo.frame_to_bars("SPY", None, self.AT)["rows"], [])
        self.assertEqual(yahoo.frame_to_bars("SPY", _Frame([]), self.AT)["rows"], [])


class DownloadHistoryTests(unittest.TestCase):
    AT = dt.datetime(2026, 10, 3, 12, 0, tzinfo=UTC)

    def test_whole_request_failure_marks_every_symbol_failed(self):
        fake = _FakeYf(error=RuntimeError("HTTP 429"))
        out = yahoo.download_history(["XLB", "XLE"], "2y", self.AT, yf_module=fake)
        self.assertEqual({k: v["status"] for k, v in out.items()}, {"XLB": "FAILED", "XLE": "FAILED"})
        self.assertIn("HTTP 429", out["XLB"]["detail"])

    def test_requests_are_unadjusted_daily_with_actions(self):
        fake = _FakeYf(frame=_Frame([(dt.date(2026, 10, 1), {"Close": 10.0})], columns=("Close",)))
        out = yahoo.download_history(["XLB"], "max", self.AT, yf_module=fake)
        self.assertEqual(out["XLB"]["status"], "OK")
        _, kwargs = fake.calls[0]
        self.assertEqual((kwargs["interval"], kwargs["auto_adjust"], kwargs["actions"], kwargs["period"]),
                         ("1d", False, True, "max"))

    def test_symbol_without_completed_bars_fails_with_provider_reason(self):
        fake = _FakeYf(frame=_Frame([]), errors={"INTUCH.BK": "possibly delisted; no price data found"})
        out = yahoo.download_history(["INTUCH.BK"], "2y", self.AT, yf_module=fake)
        self.assertEqual(out["INTUCH.BK"]["status"], "FAILED")
        self.assertIn("delisted", out["INTUCH.BK"]["detail"])

    def test_batches_are_chunked_and_deduplicated(self):
        fake = _FakeYf(frame=_Frame([(dt.date(2026, 10, 1), {"Close": 1.0})], columns=("Close",)))
        yahoo.download_history(["B", "A", "C", "A"], "2y", self.AT, chunk=2, yf_module=fake)
        self.assertEqual([c[0] for c in fake.calls], [["A", "B"], ["C"]])


class _Holdings:
    def __init__(self, rows):
        self._rows = rows

    def __len__(self):
        return len(self._rows)

    def iterrows(self):
        for sym, rec in self._rows:
            yield sym, _Row(rec)


class _FundsData:
    def __init__(self, holdings, weights, fail=False):
        self._h, self._w, self._fail = holdings, weights, fail

    @property
    def top_holdings(self):
        if self._fail:
            raise RuntimeError("404 fundsData")
        return self._h

    @property
    def sector_weightings(self):
        return self._w


class _Ticker:
    def __init__(self, info=None, funds=None, fail=False):
        self._info, self.funds_data, self._fail = info, funds, fail

    @property
    def info(self):
        if self._fail:
            raise RuntimeError("quoteSummary 401")
        return self._info


class FundSnapshotTests(unittest.TestCase):
    def test_fields_holdings_and_weights(self):
        info = {"totalAssets": 8.75e9, "navPrice": 89.1, "sharesOutstanding": None, "longName": "Materials",
                "yield": NAN, "unrelated": 1, "beta3Year": True}
        funds = _FundsData(_Holdings([("LIN", {"Name": "Linde PLC", "Holding Percent": 0.1313}),
                                      ("NEM", {"Name": "Newmont", "Holding Percent": NAN})]),
                           {"basic_materials": 0.84, "consumer_cyclical": 0.16, "energy": 0.0, "tech": NAN})
        out = yahoo.fund_snapshot("XLB", ticker_factory=lambda s: _Ticker(info, funds))
        self.assertEqual(out["status"], "OK")
        self.assertEqual(out["fields"]["totalAssets"], 8.75e9)
        self.assertNotIn("sharesOutstanding", out["fields"])  # absent stays absent
        self.assertNotIn("unrelated", out["fields"])
        self.assertIsNone(out["fields"]["yield"])  # NaN is not a number, and never zero
        self.assertEqual(out["fields"]["beta3Year"], "True")  # a non-numeric value is kept as text
        self.assertEqual(out["holdings"], [[1, "LIN", "Linde PLC", 0.1313], [2, "NEM", "Newmont", None]])
        self.assertEqual(out["sector_weights"], {"basic_materials": 0.84, "consumer_cyclical": 0.16, "energy": 0.0})
        self.assertEqual(out["holdings_status"], "OK")

    def test_holdings_failure_keeps_the_quote_fields(self):
        out = yahoo.fund_snapshot("GLD", ticker_factory=lambda s: _Ticker({"totalAssets": 1.0},
                                                                          _FundsData(None, {}, fail=True)))
        self.assertEqual(out["status"], "OK")
        self.assertEqual(out["holdings_status"], "UNAVAILABLE")
        self.assertIn("404", out["holdings_detail"])

    def test_quote_failure_and_empty_quote(self):
        failed = yahoo.fund_snapshot("X", ticker_factory=lambda s: _Ticker(fail=True))
        self.assertEqual((failed["status"], failed["holdings_status"]), ("FAILED", "FAILED"))
        empty = yahoo.fund_snapshot("X", ticker_factory=lambda s: _Ticker({}))
        self.assertEqual(empty["status"], "FAILED")

    def test_fundamentals(self):
        ok = yahoo.fundamentals("LIN", ticker_factory=lambda s: _Ticker({"trailingPE": 30.8, "currency": "USD"}))
        self.assertEqual(ok, {"status": "OK", "detail": "", "fields": {"trailingPE": 30.8, "currency": "USD"}})
        self.assertEqual(yahoo.fundamentals("X", ticker_factory=lambda s: _Ticker(fail=True))["status"], "FAILED")


class MacroTests(unittest.TestCase):
    def test_fred_missing_points_are_counted_not_zeroed(self):
        text = "observation_date,VIXCLS\n2026-09-30,16.1\n2026-10-01,.\n2026-10-02,15.8\n"
        out = macro.fred_series("VIXCLS", transport=_FakeTransport(text=text))
        self.assertEqual(out["status"], "OK")
        self.assertEqual(out["rows"], [["2026-09-30", 16.1], ["2026-10-02", 15.8]])
        self.assertEqual(out["missing_points"], 1)
        self.assertTrue(out["url"].endswith("id=VIXCLS"))

    def test_fred_transport_and_parse_failures(self):
        failed = macro.fred_series("X", transport=_FakeTransport(error=TransportError("HTTP 503")))
        self.assertEqual((failed["status"], failed["rows"]), ("FAILED", []))
        bad = macro.fred_series("X", transport=_FakeTransport(text="<html>blocked</html>"))
        self.assertEqual(bad["status"], "FAILED")

    def test_world_bank_null_is_kept_distinct(self):
        payload = [{"page": 1, "pages": 1, "lastupdated": "2026-07-13"},
                   [{"country": {"id": "TH"}, "date": "2025", "value": 2.5},
                    {"country": {"id": "TH"}, "date": "2024", "value": None},
                    {"country": {"id": ""}, "date": "2024", "value": 1.0}]]
        out = macro.world_bank_indicator("NY.GDP.MKTP.KD.ZG", ["TH", "TW"], transport=_FakeTransport(payload=payload))
        self.assertEqual(out["status"], "OK")
        self.assertEqual(out["rows"], [["TH", 2025, 2.5], ["TH", 2024, None]])
        self.assertEqual(out["countries_absent"], ["TW"])
        self.assertEqual(out["source_last_updated"], "2026-07-13")

    def test_world_bank_refuses_partial_and_error_responses(self):
        paged = [{"page": 1, "pages": 2}, [{"country": {"id": "TH"}, "date": "2025", "value": 1.0}]]
        self.assertEqual(macro.world_bank_indicator("I", ["TH"], transport=_FakeTransport(payload=paged))["status"],
                         "FAILED")
        err = [{"message": [{"id": "120", "value": "Invalid value"}]}]
        self.assertEqual(macro.world_bank_indicator("I", ["TH"], transport=_FakeTransport(payload=err))["status"],
                         "FAILED")
        with self.assertRaises(ValueError):
            macro.parse_world_bank({"not": "a list"})


class _FakeCftc:
    def __init__(self, answer=None, error=None):
        self.answer, self.error, self.calls = answer, error, []

    def get_cot_monitor(self, report, futures_only, markets, max_rows):
        self.calls.append((report, futures_only, list(markets)))
        if self.error:
            raise self.error
        return self.answer


class CftcStageTests(unittest.TestCase):
    def test_rows_fields_and_missing_cells(self):
        answer = {"success": True, "data": {"markets": [
            {"market_key": "gold", "status": "current", "refresh_error": "", "rows": [
                {"report_date_as_yyyy_mm_dd": "2026-09-22", "open_interest_all": 500000,
                 "non_commercial_long": 250000, "non_commercial_short": None},
                {"report_date_as_yyyy_mm_dd": "2026-09-29T00:00:00", "open_interest_all": 510000,
                 "non_commercial_long": 255000, "non_commercial_short": 90000}]},
            {"market_key": "bitcoin", "status": "unavailable", "refresh_error": "HTTP 503", "rows": []}]}}
        fake = _FakeCftc(answer)
        items = etf_research_data.fetch_cftc(["gold", "bitcoin", "ether"], "legacy", True, wrapper=fake)
        self.assertEqual(fake.calls, [("legacy", True, ["gold", "bitcoin", "ether"])])
        self.assertEqual(items["gold"]["status"], "OK")
        self.assertEqual(items["gold"]["rows"], [["2026-09-22", 500000.0, 250000.0, None],
                                                 ["2026-09-29", 510000.0, 255000.0, 90000.0]])
        self.assertEqual(items["bitcoin"]["status"], "FAILED")
        self.assertIn("503", items["bitcoin"]["detail"])
        self.assertEqual(items["ether"]["status"], "FAILED")  # never silently dropped

    def test_tool_failure_fails_every_market(self):
        items = etf_research_data.fetch_cftc(["gold"], "legacy", True, wrapper=_FakeCftc(error=RuntimeError("boom")))
        self.assertEqual(items["gold"]["status"], "FAILED")
        self.assertIn("boom", items["gold"]["detail"])


class RunFetchTests(unittest.TestCase):
    REQUEST = {
        "history": {"long": ["SPY", "XLB"], "standard": ["THD"]},
        "funds": ["XLB", "GLD"],
        "constituents": {"holding_parents": ["XLB"], "max_per_parent": 2, "extra_symbols": ["PTT.BK"],
                         "exclude": ["SPY"], "history_period": "1y", "fundamentals": True,
                         "fundamental_extra": ["ASML.AS"], "symbol_map": {"LIN": "LIN.TEST"},
                         "exclude_holdings": ["CASHFUND"]},
        "fred": ["VIXCLS", "BAD"],
        "world_bank": {"countries": ["TH"], "indicators": ["NY.GDP.MKTP.KD.ZG"]},
    }

    def _patches(self):
        def history(symbols, period, at, **_):
            return {s: ({"status": "FAILED", "detail": "x", "rows": []} if s == "THD" else
                        {"status": "OK", "detail": "", "rows": [["2026-10-01", 1.0, None, 0.0, 0.0, 0.0]]})
                    for s in symbols}

        def snapshot(sym):
            holdings = [[1, "LIN", "Linde", 0.13], [2, "CASHFUND", "Cash fund", 0.01], [3, "NEM", "Newmont", 0.07]]
            return {"status": "OK", "fields": {"totalAssets": 1.0}, "holdings": holdings if sym == "XLB" else []}

        def fred(sid):
            if sid == "BAD":
                return {"status": "FAILED", "detail": "HTTP 500", "rows": [], "url": "u"}
            return {"status": "OK", "detail": "", "rows": [["2026-10-01", 1.0]], "text": "a,b\n2026-10-01,1\n"}

        return [mock.patch.object(yahoo, "download_history", side_effect=history),
                mock.patch.object(yahoo, "fund_snapshot", side_effect=snapshot),
                mock.patch.object(yahoo, "fundamentals", side_effect=lambda s: {"status": "OK", "fields": {}}),
                mock.patch.object(macro, "fred_series", side_effect=fred),
                mock.patch.object(macro, "world_bank_indicator",
                                  side_effect=lambda i, c: {"status": "OK", "rows": [["TH", 2025, 1.0]]})]

    def _run(self):
        patches = self._patches()
        for p in patches:
            p.start()
        try:
            with mock.patch.object(sys, "stderr", io.StringIO()) as err:
                payload = etf_research_data.run_fetch(self.REQUEST)
            return payload, err.getvalue()
        finally:
            for p in patches:
                p.stop()

    def test_stage_statuses_and_constituent_policy(self):
        payload, progress = self._run()
        st = payload["stages"]
        self.assertEqual(st["yahoo_history"]["status"], "PARTIAL")  # THD failed, SPY/XLB ok
        self.assertEqual((st["yahoo_history"]["items_ok"], st["yahoo_history"]["items_requested"]), (2, 3))
        self.assertEqual(st["yahoo_funds"]["status"], "OK")
        self.assertEqual(st["fred"]["status"], "PARTIAL")
        self.assertEqual(st["world_bank"]["status"], "OK")
        # Top-2 holdings of XLB without CASH, plus extras, minus the excluded long-history symbols.
        # The reviewed symbol map renames a holding before it is fetched.
        self.assertEqual(payload["constituents"], ["LIN.TEST", "PTT.BK"])
        self.assertEqual(sorted(st["yahoo_fundamentals"]["items"]), ["ASML.AS", "LIN.TEST", "PTT.BK"])
        # The response body is reduced to its digest; the text itself is not carried.
        self.assertNotIn("text", st["fred"]["items"]["VIXCLS"])
        self.assertEqual(len(st["fred"]["items"]["VIXCLS"]["response_sha256"]), 64)
        self.assertEqual(st["fred"]["items"]["BAD"]["response_sha256"], "")
        lines = [ln for ln in progress.splitlines() if ln.startswith("ETFR_PROGRESS ")]
        self.assertTrue(lines)
        self.assertTrue(all(json.loads(ln[len("ETFR_PROGRESS "):])["stage"] for ln in lines))

    def test_empty_request_stages_are_unavailable(self):
        with mock.patch.object(sys, "stderr", io.StringIO()):
            payload = etf_research_data.run_fetch({})
        self.assertEqual({k: v["status"] for k, v in payload["stages"].items()},
                         {k: "UNAVAILABLE" for k in payload["stages"]})

    def test_stage_rollup(self):
        t = dt.datetime(2026, 10, 3, tzinfo=UTC)
        self.assertEqual(etf_research_data._stage("s", t, {"a": {"status": "FAILED"}})["status"], "FAILED")
        self.assertEqual(etf_research_data._stage("s", t, {"a": {"status": "OK"}})["status"], "OK")

    def test_main_writes_atomically_and_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            req = Path(tmp) / "req.json"
            out = Path(tmp) / "out.json"
            req.write_text(json.dumps({}), encoding="utf-8")
            buf = io.StringIO()
            with mock.patch.object(sys, "stderr", io.StringIO()), redirect_stdout(buf):
                code = etf_research_data.main(["fetch", "--request", str(req), "--out", str(out)])
            self.assertEqual(code, 0)
            summary = json.loads(buf.getvalue())
            self.assertTrue(summary["ok"])
            self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["script_version"],
                             etf_research_data.SCRIPT_VERSION)
            self.assertFalse((Path(tmp) / "out.json.part").exists())

    def test_usage_and_bad_request(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(etf_research_data.main([]), 2)
        self.assertEqual(json.loads(buf.getvalue())["error"]["code"], "USAGE")
        with tempfile.TemporaryDirectory() as tmp:
            buf = io.StringIO()
            with redirect_stdout(buf):
                code = etf_research_data.main(["fetch", "--request", str(Path(tmp) / "missing.json"),
                                               "--out", str(Path(tmp) / "o.json")])
            self.assertEqual(code, 2)
            self.assertEqual(json.loads(buf.getvalue())["error"]["code"], "BAD_REQUEST")


if __name__ == "__main__":
    unittest.main()
