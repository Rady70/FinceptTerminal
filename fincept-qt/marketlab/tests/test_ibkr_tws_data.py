"""Offline tests for MarketLab's thin IBKR TWS wrapper.

These tests run the real ``scripts/ibkr_tws_data.py`` CLI as a subprocess, but
the pinned checkout they import contains a deterministic fake adapter instead of
the real TRADING_DESK package. No TWS connection, no ``ibapi`` install and no
network access are required: the fake adapter reproduces the adapter's
documented read-only result shapes and failure modes, and the tests assert that
the wrapper preserves those shapes, classifications, and missing values.

The checkout is a genuine temporary git repository so the wrapper's commit pin
and clean-tree verification are exercised for real rather than bypassed.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

FINCEPT_QT = Path(__file__).resolve().parents[2]
WRAPPER = FINCEPT_QT / "scripts" / "ibkr_tws_data.py"
IBAPI_PLACEHOLDER = "ibapi-placeholder"

FAKE_ADAPTER = '''
"""Deterministic offline stand-in for the pinned TRADING_DESK adapter."""

from __future__ import annotations

import os
import time


class IBKRConnectionError(RuntimeError):
    pass


class IBKRTimeoutError(RuntimeError):
    pass


class IBKRRequestError(RuntimeError):
    pass


class IBKRDataError(RuntimeError):
    pass


def _scenario() -> str:
    return os.environ.get("FAKE_IBKR_SCENARIO", "ok")


def _contract_row(con_id: int, symbol: str) -> dict:
    return {
        "con_id": con_id,
        "symbol": symbol,
        "local_symbol": symbol,
        "security_type": "STK",
        "exchange": "SMART",
        "primary_exchange": "NASDAQ",
        "currency": "USD",
        "trading_class": symbol,
        "multiplier": "",
        "last_trade_date_or_contract_month": "",
        "strike": None,
        "right": None,
    }


def _blocked(market_data_type: int) -> dict:
    return {
        "request_transport": "PASS",
        "market_data_type": market_data_type,
        "market_data_entitlement": "BLOCKED",
        "usable_market_data": "BLOCKED_FOR_ENTITLEMENT",
        "market_data_status": "NOT_ENTITLED",
        "value_present": False,
        "ibkr_error_req_id": 10000,
        "ibkr_error_code": 10089,
        "ibkr_error_message": "Requested market data is not subscribed.",
        "ibkr_error_class": "ENTITLEMENT",
    }


def _delayed_pass() -> dict:
    return {
        "request_transport": "PASS",
        "market_data_type": 3,
        "market_data_entitlement": "DELAYED",
        "usable_market_data": "PASS",
        "market_data_status": "DELAYED",
        "value_present": True,
        "bid": 226.10,
        "ask": 226.14,
        "last": 226.12,
        "close": 225.00,
        "bid_size": 3.0,
        "ask_size": 4.0,
        "last_size": 1.0,
        "volume": 123456.0,
    }


class IBKRTWSReadOnlyAdapter:
    def __init__(self, host="127.0.0.1", port=7496, client_id=71, *, app=None,
                 ibapi_path=None, request_id_start=10000, tws_version=None):
        self.host = host
        self.port = port
        self.client_id = client_id
        self.tws_version = tws_version
        self._connected = False
        self._market_data_type = 1

    @property
    def uses_official_runtime(self) -> bool:
        return True

    @property
    def is_connected(self) -> bool:
        return self._connected

    def connect(self) -> None:
        if _scenario() == "connect_hang":
            # Mimics the pinned official client's unbounded API-version
            # handshake against an endpoint that accepts but never answers.
            time.sleep(30)
        if _scenario() == "refuse":
            raise IBKRConnectionError(
                "TWS did not open at %s:%s for client_id=%s." % (self.host, self.port, self.client_id)
            )
        self._connected = True

    def wait_until_ready(self, timeout: float = 10.0) -> bool:
        if _scenario() == "ready_timeout":
            raise IBKRTimeoutError(
                "Timed out waiting for TWS readiness signal nextValidId after %.1f seconds." % timeout
            )
        if not self._connected:
            raise IBKRConnectionError("TWS client is not connected")
        return True

    def disconnect(self) -> None:
        if _scenario() == "disconnect_fail":
            return
        self._connected = False

    def get_runtime_metadata(self) -> dict:
        return {
            "ibkr_tws_api_source": "official Interactive Brokers TWS API distribution",
            "ibkr_tws_api_version": "10.45.01",
            "ibkr_tws_api_version_source": "OBSERVED_RUNTIME_FACT",
            "ibapi_package_version": "10.45.1",
            "ibapi_location": "C:\\\\TWS API\\\\source\\\\pythonclient",
            "ibapi_observed_location": "C:\\\\TWS API\\\\source\\\\pythonclient\\\\ibapi",
            "ibapi_runtime_path_verified": "true",
            "uses_official_runtime": "true",
            "tws_version": self.tws_version or "NOT_PROVEN",
            "tws_version_source": "WORKSTATION_INVENTORY" if self.tws_version else "NOT_PROVEN",
        }

    def read_contract_details(self, contract, timeout: float = 20.0) -> list:
        scenario = _scenario()
        if scenario == "contract_empty":
            return []
        if scenario == "contract_ambiguous":
            return [_contract_row(1, contract.symbol), _contract_row(2, contract.symbol)]
        if scenario == "contract_wrong_currency":
            row = _contract_row(265598, contract.symbol)
            row["currency"] = "EUR"
            return [row]
        if scenario == "contract_wrong_type":
            row = _contract_row(265598, contract.symbol)
            row["security_type"] = "OPT"
            return [row]
        if scenario == "contract_zero_conid":
            return [_contract_row(0, contract.symbol)]
        if scenario == "contract_missing_exchange":
            row = _contract_row(265598, contract.symbol)
            row["exchange"] = ""
            return [row]
        return [_contract_row(265598, contract.symbol)]

    def set_market_data_type(self, market_data_type: int) -> None:
        self._market_data_type = market_data_type

    def read_market_data_snapshot(self, contract, timeout: float = 15.0,
                                  generic_tick_list: str = "", snapshot: bool = True) -> dict:
        scenario = _scenario()
        if scenario == "snapshot_raise":
            raise IBKRConnectionError("snapshot transport failed")
        if scenario == "conid_check" and getattr(contract, "conId", 0) != 265598:
            raise IBKRRequestError("snapshot did not use the resolved conId: %r" % getattr(contract, "conId", None))
        if scenario == "live":
            result = _delayed_pass()
            result.update({"market_data_type": 1, "market_data_entitlement": "AVAILABLE",
                           "market_data_status": "LIVE"})
            return result
        if scenario == "live_blocked_delayed_pass":
            if self._market_data_type == 1:
                return _blocked(1)
            return _delayed_pass()
        if scenario == "all_blocked":
            return _blocked(self._market_data_type)
        if scenario == "api_error":
            return {
                "request_transport": "PASS",
                "market_data_type": 1,
                "market_data_entitlement": "UNKNOWN",
                "usable_market_data": "FAIL",
                "market_data_status": "ERROR",
                "value_present": False,
                "ibkr_error_req_id": 10000,
                "ibkr_error_code": 322,
                "ibkr_error_message": "Error processing request",
                "ibkr_error_class": "ERROR",
            }
        if scenario == "crossed_book":
            result = _delayed_pass()
            result.update({"bid": 227.0, "ask": 226.0})
            return result
        if scenario == "zero_price":
            result = _delayed_pass()
            result.update({"last": 0.0})
            return result
        if scenario == "negative_price":
            result = _delayed_pass()
            result.update({"bid": -2.0})
            return result
        if scenario == "non_finite":
            result = _delayed_pass()
            result["last"] = float("nan")
            return result
        if scenario == "negative_size":
            result = _delayed_pass()
            result.update({"volume": -5.0})
            return result
        if scenario == "error_with_values":
            result = _delayed_pass()
            result.update({"market_data_status": "ERROR", "usable_market_data": "FAIL",
                           "ibkr_error_code": 322, "ibkr_error_message": "Error processing request",
                           "ibkr_error_class": "ERROR"})
            return result
        if scenario == "live_blocked_delayed_error":
            if self._market_data_type == 1:
                return _blocked(1)
            return {
                "request_transport": "PASS",
                "market_data_type": 3,
                "market_data_entitlement": "UNKNOWN",
                "usable_market_data": "FAIL",
                "market_data_status": "ERROR",
                "value_present": False,
                "ibkr_error_code": 322,
                "ibkr_error_message": "Error processing request",
                "ibkr_error_class": "ERROR",
            }
        if scenario == "missing_fields":
            return {
                "request_transport": "PASS",
                "market_data_type": 3,
                "market_data_entitlement": "DELAYED",
                "usable_market_data": "PASS",
                "market_data_status": "DELAYED",
                "value_present": True,
                "bid": 226.10,
                "ask": 226.14,
            }
        if self._market_data_type == 1:
            result = _delayed_pass()
            result.update({"market_data_type": 1, "market_data_entitlement": "AVAILABLE",
                           "market_data_status": "LIVE"})
            return result
        return _delayed_pass()

    def read_historical_bars(self, contract, *, end_date_time: str, duration: str = "1 M",
                             bar_size: str = "1 day", what_to_show: str = "TRADES",
                             use_rth: bool = True, timeout: float = 30.0) -> list:
        scenario = _scenario()
        if scenario == "history_empty":
            raise IBKRDataError("No historical bars were received from TWS.")
        if scenario == "history_entitlement":
            raise IBKRRequestError(
                "TWS rejected historical bars: 2188: Requested market data is not subscribed."
            )
        if scenario == "conid_check" and getattr(contract, "conId", 0) != 265598:
            raise IBKRRequestError("history did not use the resolved conId: %r" % getattr(contract, "conId", None))
        rows = [
            {"date": "20260806", "open": 224.5, "high": 227.0, "low": 223.9,
             "close": 226.0, "volume": 1000.5},
            {"date": "20260807", "open": 226.0, "high": 228.4, "low": 225.1,
             "close": 227.3, "volume": 1100.0},
        ]
        if scenario == "history_zero_close":
            rows[1]["close"] = 0.0
        elif scenario == "history_crossed_ohlc":
            rows[1]["high"] = 224.0
        elif scenario == "history_duplicate_dates":
            rows[1]["date"] = rows[0]["date"]
        elif scenario == "history_future":
            rows[1]["date"] = "20991231"
        elif scenario == "history_negative_volume":
            rows[1]["volume"] = -1.0
        return rows
'''


class IbkrWrapperTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="marketlab_ibkr_")
        cls.root = Path(cls._tmp.name)
        checkout = cls.root / "trading_desk"
        package = checkout / "scripts" / "ibkr_tws"
        package.mkdir(parents=True)
        (package / "__init__.py").write_text(
            "from .adapter import IBKRTWSReadOnlyAdapter\n\n__all__ = [\"IBKRTWSReadOnlyAdapter\"]\n",
            encoding="utf-8",
        )
        (package / "adapter.py").write_text(FAKE_ADAPTER, encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(checkout)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(checkout), "add", "-A"], check=True, capture_output=True)
        subprocess.run(
            ["git", "-C", str(checkout), "-c", "user.email=test@marketlab.invalid",
             "-c", "user.name=MarketLab Test", "commit", "-q", "-m", "fixture"],
            check=True,
            capture_output=True,
        )
        cls.commit = subprocess.run(
            ["git", "-C", str(checkout), "rev-parse", "HEAD"], check=True, capture_output=True, text=True
        ).stdout.strip()
        cls.ibapi_dir = cls.root / IBAPI_PLACEHOLDER
        cls.ibapi_dir.mkdir()
        cls.checkout = checkout

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def _write_config(self, **overrides) -> Path:
        config = {
            "trading_desk_root": str(self.checkout),
            "trading_desk_commit": self.commit,
            "ibapi_path": str(self.ibapi_dir),
            "host": "127.0.0.1",
            "port": 7496,
            "client_id": 71,
            "tws_version": "10.48.1c",
        }
        config.update(overrides)
        path = self.root / f"config_{len(list(self.root.glob('config_*.json')))}.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        return path

    def _run(self, config: Path, *arguments: str, scenario: str = "ok") -> tuple[int, dict]:
        env = dict(os.environ)
        env["FAKE_IBKR_SCENARIO"] = scenario
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        result = subprocess.run(
            [sys.executable, str(WRAPPER), "--config", str(config), *arguments],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        lines = [line for line in result.stdout.splitlines() if line.strip()]
        self.assertTrue(lines, f"wrapper produced no JSON output; stderr={result.stderr}")
        return result.returncode, json.loads(lines[-1])

    def test_probe_reports_identity_and_clean_disconnect(self) -> None:
        code, payload = self._run(self._write_config(), "probe")
        self.assertEqual(code, 0, payload)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["source"], "ibkr_tws")
        self.assertEqual(payload["adapter"]["commit"], self.commit)
        self.assertEqual(payload["adapter"]["ibapi_version"], "10.45.01")
        self.assertEqual(payload["adapter"]["tws_version"], "10.48.1c")
        self.assertTrue(payload["clean_disconnect"])

    def test_endpoint_overrides_are_typed_and_effective(self) -> None:
        # The C++ consumer always passes --host/--port/--client-id after the
        # subcommand; a string port must bind to an integer override (the
        # application once failed every command with "port must be an integer").
        code, payload = self._run(
            self._write_config(), "probe", "--host", "127.0.0.1", "--port", "7497", "--client-id", "72"
        )
        self.assertEqual(code, 0, payload)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["adapter"]["port"], 7497)
        self.assertEqual(payload["adapter"]["client_id"], 72)

    def test_probe_refused_connection_is_a_typed_failure(self) -> None:
        code, payload = self._run(self._write_config(), "probe", scenario="refuse")
        self.assertEqual(code, 1)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["failure"]["type"], "IBKR_CONNECTION_FAILED")
        self.assertEqual(payload["failure"]["stage"], "connect")

    def test_probe_readiness_timeout_is_a_typed_failure(self) -> None:
        code, payload = self._run(self._write_config(), "probe", scenario="ready_timeout")
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_TIMEOUT")
        self.assertEqual(payload["failure"]["stage"], "connect")

    def test_probe_bounds_a_hanging_handshake(self) -> None:
        # A server that accepts the socket but never completes the IBKR
        # handshake must not park the command: the wrapper bounds the handshake
        # and reports a typed timeout instead of running until the caller's
        # process watchdog kills it.
        started = time.monotonic()
        code, payload = self._run(self._write_config(), "probe", "--timeout", "1", scenario="connect_hang")
        elapsed = time.monotonic() - started
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_TIMEOUT")
        self.assertEqual(payload["failure"]["stage"], "connect")
        self.assertLess(elapsed, 20.0)

    def test_contract_resolves_a_single_instrument(self) -> None:
        code, payload = self._run(self._write_config(), "contract", "AAPL")
        self.assertEqual(code, 0, payload)
        self.assertEqual(payload["resolution"], "RESOLVED")
        self.assertEqual(payload["resolved"]["con_id"], 265598)
        self.assertEqual(payload["resolved"]["currency"], "USD")

    def test_contract_not_found_is_explicit(self) -> None:
        code, payload = self._run(self._write_config(), "contract", "AAPL", scenario="contract_empty")
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_CONTRACT_NOT_RESOLVED")

    def test_contract_ambiguity_is_explicit_with_candidate_ids(self) -> None:
        code, payload = self._run(self._write_config(), "contract", "AAPL", scenario="contract_ambiguous")
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_CONTRACT_AMBIGUOUS")
        self.assertEqual(payload["failure"]["details"]["con_ids"], [1, 2])

    def test_contract_identity_constraints_are_enforced(self) -> None:
        for scenario, reason in (
            ("contract_wrong_currency", "CURRENCY_MISMATCH"),
            ("contract_wrong_type", "SECURITY_TYPE_UNSUPPORTED"),
            ("contract_zero_conid", "CONID_INVALID"),
            ("contract_missing_exchange", "EXCHANGE_MISSING"),
        ):
            with self.subTest(scenario=scenario):
                code, payload = self._run(self._write_config(), "contract", "AAPL", scenario=scenario)
                self.assertEqual(code, 1)
                self.assertEqual(payload["failure"]["type"], "IBKR_CONTRACT_IDENTITY_INVALID")
                self.assertEqual(payload["failure"]["details"]["reason"], reason)

    def test_snapshot_live_pass_preserves_live_classification(self) -> None:
        code, payload = self._run(self._write_config(), "snapshot", "AAPL", scenario="live")
        self.assertEqual(code, 0, payload)
        self.assertTrue(payload["classification"]["usable"])
        self.assertEqual(payload["classification"]["feed"], "LIVE")
        self.assertFalse(payload["classification"]["delayed_fallback"])
        self.assertEqual(len(payload["attempts"]), 1)

    def test_snapshot_live_block_falls_back_to_delayed_and_keeps_both_attempts(self) -> None:
        code, payload = self._run(
            self._write_config(), "snapshot", "AAPL", scenario="live_blocked_delayed_pass"
        )
        self.assertEqual(code, 0, payload)
        self.assertTrue(payload["classification"]["usable"])
        self.assertEqual(payload["classification"]["feed"], "DELAYED")
        self.assertTrue(payload["classification"]["delayed_fallback"])
        self.assertEqual(len(payload["attempts"]), 2)
        self.assertEqual(payload["attempts"][0]["market_data_status"], "NOT_ENTITLED")
        self.assertEqual(payload["attempts"][1]["usable_market_data"], "PASS")

    def test_snapshot_all_blocked_is_explicit_and_not_usable(self) -> None:
        code, payload = self._run(self._write_config(), "snapshot", "AAPL", scenario="all_blocked")
        self.assertEqual(code, 0, payload)
        self.assertFalse(payload["classification"]["usable"])
        self.assertEqual(payload["classification"]["status"], "NOT_ENTITLED")
        self.assertEqual(payload["classification"]["entitlement"], "BLOCKED")
        self.assertIsNone(payload["classification"]["feed"])
        self.assertEqual(payload["quote"], {})

    def test_snapshot_api_error_stops_the_delayed_fallback(self) -> None:
        code, payload = self._run(self._write_config(), "snapshot", "AAPL", scenario="api_error")
        self.assertEqual(code, 0, payload)
        self.assertFalse(payload["classification"]["usable"])
        self.assertEqual(len(payload["attempts"]), 1)
        self.assertEqual(payload["classification"]["error_code"], 322)

    def test_snapshot_transport_exception_keeps_its_typing(self) -> None:
        code, payload = self._run(self._write_config(), "snapshot", "AAPL", scenario="snapshot_raise")
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_CONNECTION_FAILED")
        self.assertEqual(payload["failure"]["stage"], "snapshot")

    def test_snapshot_and_history_pin_the_resolved_contract(self) -> None:
        # The fake adapter rejects a request whose contract is not the resolved
        # conId, so a successful run proves the wrapper pins the identity it
        # reports.
        code, payload = self._run(self._write_config(), "snapshot", "AAPL", scenario="conid_check")
        self.assertEqual(code, 0, payload)
        self.assertTrue(payload["classification"]["usable"])
        code, payload = self._run(self._write_config(), "history", "AAPL", scenario="conid_check")
        self.assertEqual(code, 0, payload)
        self.assertTrue(payload["classification"]["usable"])

    def test_snapshot_missing_fields_stay_absent_and_never_become_zero(self) -> None:
        code, payload = self._run(self._write_config(), "snapshot", "AAPL", scenario="missing_fields")
        self.assertEqual(code, 0, payload)
        self.assertTrue(payload["classification"]["usable"])
        self.assertIn("bid", payload["quote"])
        self.assertNotIn("last", payload["quote"])
        self.assertNotIn("volume", payload["quote"])
        self.assertNotIn("last", payload.get("quote", {}))

    def test_snapshot_value_validation_blocks_bad_values(self) -> None:
        # The adapter reports transport/lifecycle; it does not reject crossed
        # books, zero/negative/non-finite prices, or negative sizes. The wrapper
        # must, exactly as the reference qualification did.
        for scenario in ("crossed_book", "zero_price", "negative_price", "non_finite",
                         "negative_size", "error_with_values"):
            with self.subTest(scenario=scenario):
                code, payload = self._run(self._write_config(), "snapshot", "AAPL", scenario=scenario)
                self.assertEqual(code, 0, payload)
                self.assertFalse(payload["classification"]["usable"])
                self.assertEqual(payload["classification"]["status"], "ERROR")
                self.assertEqual(payload["quote"], {})
                self.assertEqual(len(payload["attempts"]), 1)

    def test_snapshot_blocked_live_still_exposes_a_delayed_failure(self) -> None:
        # A genuine delayed-attempt ERROR must not be hidden behind the live
        # NOT_ENTITLED decision.
        code, payload = self._run(
            self._write_config(), "snapshot", "AAPL", scenario="live_blocked_delayed_error"
        )
        self.assertEqual(code, 0, payload)
        self.assertFalse(payload["classification"]["usable"])
        self.assertEqual(payload["classification"]["status"], "NOT_ENTITLED")
        self.assertTrue(payload["classification"]["delayed_attempted"])
        self.assertEqual(payload["classification"]["delayed_market_data_status"], "ERROR")
        self.assertEqual(payload["classification"]["delayed_error_code"], 322)
        self.assertEqual(payload["classification"]["live_market_data_status"], "NOT_ENTITLED")
        self.assertEqual(len(payload["attempts"]), 2)

    def test_disconnect_failure_after_a_successful_read_is_visible(self) -> None:
        code, payload = self._run(self._write_config(), "snapshot", "AAPL", scenario="disconnect_fail")
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_DISCONNECT_FAILED")
        self.assertEqual(payload["failure"]["stage"], "disconnect")

    def test_history_returns_bars_with_utc_timestamps(self) -> None:
        code, payload = self._run(self._write_config(), "history", "AAPL")
        self.assertEqual(code, 0, payload)
        self.assertTrue(payload["classification"]["usable"])
        self.assertEqual(len(payload["bars"]), 2)
        self.assertEqual(payload["bars"][0]["date"], "20260806")
        self.assertAlmostEqual(payload["bars"][0]["timestamp"], 1785974400.0, places=0)
        self.assertEqual(payload["bars"][1]["close"], 227.3)

    def test_history_empty_is_classified_not_a_success(self) -> None:
        code, payload = self._run(self._write_config(), "history", "AAPL", scenario="history_empty")
        self.assertEqual(code, 0, payload)
        self.assertFalse(payload["classification"]["usable"])
        self.assertEqual(payload["classification"]["status"], "NO_DATA")
        self.assertEqual(payload["bars"], [])

    def test_history_entitlement_message_is_classified_not_subscribed(self) -> None:
        code, payload = self._run(
            self._write_config(), "history", "AAPL", scenario="history_entitlement"
        )
        self.assertEqual(code, 0, payload)
        self.assertFalse(payload["classification"]["usable"])
        self.assertEqual(payload["classification"]["status"], "NOT_ENTITLED")
        self.assertEqual(payload["classification"]["entitlement"], "BLOCKED")

    def test_history_value_validation_blocks_bad_series(self) -> None:
        for scenario in ("history_zero_close", "history_crossed_ohlc", "history_duplicate_dates",
                         "history_future", "history_negative_volume"):
            with self.subTest(scenario=scenario):
                code, payload = self._run(self._write_config(), "history", "AAPL", scenario=scenario)
                self.assertEqual(code, 0, payload)
                self.assertFalse(payload["classification"]["usable"])
                self.assertEqual(payload["classification"]["status"], "VALUES_INVALID")
                self.assertEqual(payload["bars"], [])
                self.assertTrue(payload["classification"]["validation_reason"])

    def test_pin_mismatch_fails_closed(self) -> None:
        config = self._write_config(trading_desk_commit="0" * 40)
        code, payload = self._run(config, "probe")
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_ADAPTER_PIN_MISMATCH")
        self.assertEqual(payload["failure"]["details"]["observed"], self.commit)

    def test_dirty_checkout_fails_closed(self) -> None:
        dirty_file = self.checkout / "scripts" / "ibkr_tws" / "scratch.txt"
        dirty_file.write_text("uncommitted", encoding="utf-8")
        try:
            code, payload = self._run(self._write_config(), "probe")
        finally:
            dirty_file.unlink()
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_ADAPTER_TREE_DIRTY")

    def test_non_loopback_host_is_rejected(self) -> None:
        config = self._write_config(host="192.0.2.10")
        code, payload = self._run(config, "probe")
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_CONFIG_INVALID")

    def test_zero_or_empty_endpoint_values_fail_closed(self) -> None:
        # A configured zero/empty value must reach validation, not be replaced
        # by a default that could point at a different TWS endpoint.
        for overrides in ({"port": 0}, {"client_id": 0}, {"host": ""}):
            with self.subTest(overrides=overrides):
                config = self._write_config(**overrides)
                code, payload = self._run(config, "probe")
                self.assertEqual(code, 1)
                self.assertEqual(payload["failure"]["type"], "IBKR_CONFIG_INVALID")
                self.assertEqual(payload["failure"]["stage"], "config")

    def test_missing_config_fails_closed(self) -> None:
        code, payload = self._run(self.root / "does_not_exist.json", "probe")
        self.assertEqual(code, 1)
        self.assertEqual(payload["failure"]["type"], "IBKR_CONFIG_MISSING")

    def test_wrapper_source_has_no_order_or_account_surface(self) -> None:
        source = WRAPPER.read_text(encoding="utf-8").lower()
        forbidden = (
            "placeorder",
            "cancelorder",
            "reqopenorders",
            "reqexecutions",
            "reqpositions",
            "reqaccountsummary",
            "accountsummary",
            "reqaccountupdates",
            "reqallopenorders",
            "reqcompletedorders",
        )
        for name in forbidden:
            self.assertNotIn(name, source, f"wrapper must not reference {name}")


if __name__ == "__main__":
    unittest.main()
