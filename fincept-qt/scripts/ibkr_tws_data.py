"""MarketLab's thin read-only wrapper over the TRADING_DESK IBKR TWS adapter.

This script is the only MarketLab-owned IBKR connection surface. It:

  * reads a non-secret local JSON configuration (no credentials and no account
    identifiers are requested, copied, or persisted);
  * verifies the configured TRADING_DESK checkout identity (exact commit and a
    clean working tree) before importing anything from it;
  * imports the unchanged ``scripts.ibkr_tws.IBKRTWSReadOnlyAdapter`` from that
    checkout;
  * invokes only the adapter's documented read-only methods; and
  * emits one JSON envelope carrying source, retrieval time, contract identity,
    market-data type/entitlement classification, and the adapter's already
    normalized values.

Commands:
  probe     connect, wait for readiness, report runtime identity, disconnect
  contract  resolve one stock contract's identity details
  snapshot  read one bounded quote snapshot (live attempt, then an explicit
            delayed fallback when entitlement blocks the live feed)
  history   read bounded historical bars for a completed market window

The adapter exposes no order/account/position surface and this wrapper adds
none. Missing values stay missing; entitlement and market-data type are
reported exactly as observed. If the repository pin, official dependency,
local configuration, or output shape cannot be verified, the command fails
closed with a typed failure instead of falling back to another provider.

Configuration (default ``%FINCEPT_DATA_DIR%/ibkr_tws.json``, or ``--config``)::

    {
      "trading_desk_root": "E:\\\\TRADING_DESK",
      "trading_desk_commit": "<40-hex commit>",
      "ibapi_path": "C:\\\\TWS API\\\\source\\\\pythonclient",
      "host": "127.0.0.1",
      "port": 7496,
      "client_id": 71,
      "tws_version": "10.48.1c"
    }

Every failure is reported as a single JSON document. Dependency, pin, and
configuration failures carry ``{"ok": false, "failure": {...}}``; a request
that the adapter itself classified (for example an entitlement block) stays
``ok: true`` with ``classification.usable == false`` so the caller can tell a
subscription fact apart from a transport failure. The top-level ``error`` key
is deliberately never used: the application's bounded Python runner treats it
as a launcher-level error and would discard the structured classification.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import threading
import types
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

SOURCE = "ibkr_tws"
DEFAULT_CONFIG_RELATIVE = "ibkr_tws.json"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 7496
DEFAULT_CLIENT_ID = 71
LOOPBACK_HOSTS = frozenset({"127.0.0.1", "localhost", "::1"})
SNAPSHOT_LIVE_TYPE = 1
SNAPSHOT_DELAYED_TYPE = 3
SNAPSHOT_CONTINUE_STATUSES = frozenset({"NOT_ENTITLED", "NO_VALUE"})
SNAPSHOT_ERROR_STATUS = "ERROR"
ADAPTER_PACKAGE_RELATIVE = Path("scripts") / "ibkr_tws"
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
ENTITLEMENT_HINTS = (
    "not subscribed",
    "not entitled",
    "market data permission",
    "market data permissions",
    "subscription required",
    "entitlement",
    "2188",
    "10089",
    "10090",
    "10167",
    "354",
)
QUOTE_FIELDS = ("bid", "ask", "last", "close", "bid_size", "ask_size", "last_size", "volume")
ATTEMPT_SUMMARY_FIELDS = (
    "market_data_type",
    "market_data_status",
    "market_data_entitlement",
    "usable_market_data",
    "ibkr_error_code",
    "ibkr_error_message",
    "ibkr_error_class",
)


class WrapperError(Exception):
    """A typed, fail-closed wrapper failure with a stage and a message."""

    def __init__(self, kind: str, stage: str, message: str, *, details: Any = None) -> None:
        super().__init__(message)
        self.kind = kind
        self.stage = stage
        self.message = message
        self.details = details

    def to_dict(self) -> dict[str, Any]:
        failure: dict[str, Any] = {"type": self.kind, "stage": self.stage, "message": self.message}
        if self.details is not None:
            failure["details"] = self.details
        return failure


@dataclass
class Config:
    config_path: Path
    trading_desk_root: Path
    trading_desk_commit: str
    ibapi_path: Path
    host: str
    port: int
    client_id: int
    tws_version: str | None


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return digest.upper()


def _require_int(value: Any, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise WrapperError("IBKR_CONFIG_INVALID", "config", f"{name} must be an integer")
    if not minimum <= value <= maximum:
        raise WrapperError("IBKR_CONFIG_INVALID", "config", f"{name} must be between {minimum} and {maximum}")
    return value


def _pick(overrides: dict[str, Any], raw: dict[str, Any], name: str, default: Any = None) -> Any:
    """Explicit override selection: a supplied falsey value is not "absent"."""

    if name in overrides and overrides[name] is not None:
        return overrides[name]
    if name in raw and raw[name] is not None:
        return raw[name]
    return default


def load_config(path: str | Path, overrides: dict[str, Any] | None = None) -> Config:
    """Read and validate the non-secret local configuration file."""

    config_path = Path(path).expanduser()
    try:
        # utf-8-sig accepts both a plain UTF-8 file and the byte-order mark a
        # Windows editor may add; a BOM is not a configuration error.
        raw = json.loads(config_path.read_text(encoding="utf-8-sig"))
    except FileNotFoundError as exc:
        raise WrapperError("IBKR_CONFIG_MISSING", "config", f"IBKR TWS configuration not found at {config_path}") from exc
    except OSError as exc:
        raise WrapperError("IBKR_CONFIG_UNREADABLE", "config", f"IBKR TWS configuration unreadable: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise WrapperError("IBKR_CONFIG_INVALID", "config", f"IBKR TWS configuration is not valid JSON: {exc}") from exc
    if not isinstance(raw, dict):
        raise WrapperError("IBKR_CONFIG_INVALID", "config", "IBKR TWS configuration must be a JSON object")

    overrides = overrides or {}

    root_raw = _pick(overrides, raw, "trading_desk_root")
    if not isinstance(root_raw, str) or not root_raw.strip():
        raise WrapperError("IBKR_CONFIG_INVALID", "config", "trading_desk_root is required")
    root = Path(root_raw).expanduser()

    commit = raw.get("trading_desk_commit")
    if not isinstance(commit, str) or not COMMIT_PATTERN.fullmatch(commit.strip().lower()):
        raise WrapperError(
            "IBKR_CONFIG_INVALID", "config", "trading_desk_commit must be a 40-character lowercase hex commit"
        )

    ibapi_raw = _pick(overrides, raw, "ibapi_path")
    if not isinstance(ibapi_raw, str) or not ibapi_raw.strip():
        raise WrapperError("IBKR_CONFIG_INVALID", "config", "ibapi_path is required")
    ibapi_path = Path(ibapi_raw).expanduser()
    if not ibapi_path.is_dir():
        raise WrapperError(
            "IBKR_IBAPI_PATH_MISSING", "dependency", f"Official TWS API pythonclient directory not found at {ibapi_path}"
        )

    host_raw = _pick(overrides, raw, "host", DEFAULT_HOST)
    if not isinstance(host_raw, str) or not host_raw.strip():
        raise WrapperError("IBKR_CONFIG_INVALID", "config", "host must be a non-empty loopback address")
    host = host_raw.strip()
    if host not in LOOPBACK_HOSTS:
        raise WrapperError(
            "IBKR_CONFIG_INVALID", "config", f"host must be a loopback address; got {host!r}"
        )

    port = _require_int(_pick(overrides, raw, "port", DEFAULT_PORT), "port", 1, 65535)
    client_id = _require_int(_pick(overrides, raw, "client_id", DEFAULT_CLIENT_ID), "client_id", 1, 2**31 - 1)

    tws_version_raw = raw.get("tws_version")
    if tws_version_raw is not None and (not isinstance(tws_version_raw, str) or not tws_version_raw.strip()):
        raise WrapperError("IBKR_CONFIG_INVALID", "config", "tws_version must be a non-empty string when supplied")
    tws_version = tws_version_raw.strip() if isinstance(tws_version_raw, str) else None

    return Config(
        config_path=config_path,
        trading_desk_root=root,
        trading_desk_commit=commit.strip().lower(),
        ibapi_path=ibapi_path,
        host=host,
        port=port,
        client_id=client_id,
        tws_version=tws_version,
    )


def _run_git(root: Path, arguments: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            ["git", "-C", str(root), *arguments],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise WrapperError(
            "IBKR_ADAPTER_IDENTITY_UNAVAILABLE", "pin", f"Unable to inspect the configured TRADING_DESK checkout: {exc}"
        ) from exc


def verify_checkout_identity(config: Config) -> dict[str, Any]:
    """Pin the configured checkout to the exact recorded commit and a clean tree."""

    adapter_dir = config.trading_desk_root / ADAPTER_PACKAGE_RELATIVE
    adapter_file = adapter_dir / "adapter.py"
    init_file = adapter_dir / "__init__.py"
    if not adapter_file.is_file() or not init_file.is_file():
        raise WrapperError(
            "IBKR_ADAPTER_MISSING",
            "pin",
            f"Configured checkout has no {ADAPTER_PACKAGE_RELATIVE / 'adapter.py'} at {config.trading_desk_root}",
        )
    if not config.trading_desk_root.is_dir():
        raise WrapperError(
            "IBKR_ADAPTER_MISSING", "pin", f"Configured TRADING_DESK checkout not found at {config.trading_desk_root}"
        )

    head = _run_git(config.trading_desk_root, ["rev-parse", "HEAD"])
    if head.returncode != 0:
        raise WrapperError(
            "IBKR_ADAPTER_IDENTITY_UNAVAILABLE",
            "pin",
            f"Configured checkout at {config.trading_desk_root} is not a readable git repository",
        )
    observed = head.stdout.strip().lower()
    if observed != config.trading_desk_commit:
        raise WrapperError(
            "IBKR_ADAPTER_PIN_MISMATCH",
            "pin",
            "Configured TRADING_DESK checkout is not at the pinned commit",
            details={"expected": config.trading_desk_commit, "observed": observed or "UNKNOWN"},
        )

    status = _run_git(config.trading_desk_root, ["status", "--porcelain"])
    if status.returncode != 0:
        raise WrapperError(
            "IBKR_ADAPTER_IDENTITY_UNAVAILABLE", "pin", "Unable to read the configured checkout working-tree state"
        )
    if status.stdout.strip():
        raise WrapperError(
            "IBKR_ADAPTER_TREE_DIRTY",
            "pin",
            "Configured TRADING_DESK checkout has uncommitted changes; refusing to import it",
        )

    return {
        "commit": observed,
        "adapter_sha256": _sha256_file(adapter_file),
        "init_sha256": _sha256_file(init_file),
        "checkout_root": str(config.trading_desk_root),
    }


def load_adapter_module(config: Config) -> types.ModuleType:
    """Import IBKRTWSReadOnlyAdapter from the pinned checkout by file location.

    Loading by location keeps the import independent of any unrelated ``scripts``
    package that may already be present on ``sys.path`` while resolving the
    adapter's own relative import from its package directory.
    """

    package_dir = (config.trading_desk_root / ADAPTER_PACKAGE_RELATIVE).resolve()
    init_file = package_dir / "__init__.py"
    package_name = "marketlab_ibkr_tws_producer"
    if package_name in sys.modules:
        return sys.modules[package_name]
    # Importing must not write bytecode into the referenced checkout: that
    # checkout is meant to stay unchanged, and its own clean-tree pin is
    # re-verified on every command.
    previous_dont_write = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location(
            package_name, init_file, submodule_search_locations=[str(package_dir)]
        )
        if spec is None or spec.loader is None:
            raise ImportError("module spec could not be created")
        module = importlib.util.module_from_spec(spec)
        sys.modules[package_name] = module
        spec.loader.exec_module(module)
        return module
    except Exception as exc:
        sys.modules.pop(package_name, None)
        raise WrapperError(
            "IBKR_ADAPTER_IMPORT_FAILED", "dependency", f"Unable to import the pinned TRADING_DESK adapter: {exc}"
        ) from exc
    finally:
        sys.dont_write_bytecode = previous_dont_write


class _StockContract:
    """Minimal project-owned contract descriptor for the official serializer.

    Mirrors only the fields the pinned official ``EClient`` request serializers
    read for contract details, market-data snapshots and historical bars.
    """

    def __init__(self, symbol: str, *, security_type: str = "STK", exchange: str = "SMART", currency: str = "USD") -> None:
        self.symbol = symbol
        self.secType = security_type
        self.exchange = exchange
        self.currency = currency
        self.conId = 0
        self.primaryExchange = ""
        self.tradingClass = ""
        self.lastTradeDateOrContractMonth = ""
        self.lastTradeDate = ""
        self.strike = 0.0
        self.right = ""
        self.multiplier = ""
        self.localSymbol = ""
        self.includeExpired = False
        self.secIdType = ""
        self.secId = ""
        self.description = ""
        self.issuerId = ""
        self.comboLegsDescrip = ""
        self.comboLegs: list[object] = []
        self.deltaNeutralContract = None


def _classify_exception(exc: Exception, stage: str) -> WrapperError:
    name = type(exc).__name__
    message = str(exc)
    if "ConnectionError" in name:
        return WrapperError("IBKR_CONNECTION_FAILED", stage, message)
    if "Timeout" in name:
        return WrapperError("IBKR_TIMEOUT", stage, message)
    if "DataError" in name:
        return WrapperError("IBKR_NO_DATA", stage, message)
    if "RequestError" in name:
        return WrapperError("IBKR_REQUEST_REJECTED", stage, message)
    if any(hint in message.lower() for hint in ENTITLEMENT_HINTS):
        return WrapperError("IBKR_ENTITLEMENT_BLOCKED", stage, message)
    return WrapperError("IBKR_ADAPTER_ERROR", stage, message)


def _connect(config: Config, module: types.ModuleType, readiness_timeout: float, stage: str) -> Any:
    try:
        adapter = module.IBKRTWSReadOnlyAdapter(
            host=config.host,
            port=config.port,
            client_id=config.client_id,
            ibapi_path=str(config.ibapi_path),
            tws_version=config.tws_version,
        )
    except Exception as exc:
        raise _classify_exception(exc, stage) from exc

    # The pinned official client's initial API-version handshake has no bounded
    # form: against an endpoint that accepts the socket but never answers, its
    # connect() parks in the handshake loop. The wrapper therefore performs the
    # connect-and-readiness sequence on a daemon thread and bounds it itself;
    # this process is short-lived, so an abandoned daemon thread cannot outlive
    # the command or keep the interpreter alive.
    outcome: dict[str, Any] = {}

    def handshake() -> None:
        try:
            adapter.connect()
            adapter.wait_until_ready(timeout=readiness_timeout)
            outcome["ok"] = True
        except Exception as exc:  # classified by the caller
            outcome["error"] = exc

    worker = threading.Thread(target=handshake, name="marketlab-ibkr-handshake", daemon=True)
    worker.start()
    budget = max(5.0, float(readiness_timeout) + 3.0)
    worker.join(budget)
    if worker.is_alive():
        raise WrapperError(
            "IBKR_TIMEOUT",
            stage,
            f"TWS did not complete the API handshake within {budget:.1f} seconds",
        )
    error = outcome.get("error")
    if error is not None:
        if isinstance(error, WrapperError):
            raise error
        raise _classify_exception(error, stage)
    return adapter


def _disconnect(adapter: Any) -> bool:
    try:
        adapter.disconnect()
    except Exception:
        return False
    try:
        return not bool(adapter.is_connected)
    except Exception:
        return False


def _identity(config: Config, pin: dict[str, Any], adapter: Any) -> dict[str, Any]:
    runtime: dict[str, Any] = {}
    try:
        raw = adapter.get_runtime_metadata()
        if isinstance(raw, dict):
            runtime = {
                "ibapi_version": raw.get("ibkr_tws_api_version", ""),
                "ibapi_version_source": raw.get("ibkr_tws_api_version_source", ""),
                "ibapi_location": raw.get("ibapi_location", ""),
                "ibapi_runtime_path_verified": raw.get("ibapi_runtime_path_verified", "false"),
                "uses_official_runtime": raw.get("uses_official_runtime", "false"),
                "tws_version": raw.get("tws_version", ""),
                "tws_version_source": raw.get("tws_version_source", ""),
            }
    except Exception:
        runtime = {}
    return {
        "commit": pin["commit"],
        "adapter_sha256": pin["adapter_sha256"],
        "host": config.host,
        "port": config.port,
        "client_id": config.client_id,
        **runtime,
    }


def _envelope(command: str, *, ok: bool, identity: dict[str, Any] | None = None) -> dict[str, Any]:
    envelope: dict[str, Any] = {"source": SOURCE, "command": command, "ok": ok, "retrieved_at": _now_iso()}
    if identity is not None:
        envelope["adapter"] = identity
    return envelope


def _resolve_contract(adapter: Any, contract: _StockContract, symbol: str, timeout: float) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    try:
        rows = adapter.read_contract_details(contract, timeout=timeout)
    except Exception as exc:
        raise _classify_exception(exc, "contract") from exc
    candidates = [dict(row) for row in rows if str(row.get("symbol") or "").upper() == symbol.upper()]
    con_ids = sorted({row.get("con_id") for row in candidates if row.get("con_id")})
    if not candidates:
        raise WrapperError(
            "IBKR_CONTRACT_NOT_RESOLVED", "contract", f"No contract details were returned for {symbol}"
        )
    if len(con_ids) != 1:
        raise WrapperError(
            "IBKR_CONTRACT_AMBIGUOUS",
            "contract",
            f"Contract details for {symbol} resolved to {len(con_ids)} distinct instruments",
            details={"con_ids": con_ids},
        )
    resolved = next(row for row in candidates if row.get("con_id") == con_ids[0])
    return resolved, candidates


def _snapshot_attempt(adapter: Any, contract: _StockContract, market_data_type: int, timeout: float) -> dict[str, Any]:
    try:
        adapter.set_market_data_type(market_data_type)
        result = dict(adapter.read_market_data_snapshot(contract, timeout=timeout, snapshot=True))
    except Exception as exc:
        raise _classify_exception(exc, "snapshot") from exc
    result.pop("_errors", None)
    result.pop("_diagnostics", None)
    result["requested_market_data_type"] = market_data_type
    return result


def _attempt_summary(attempt: dict[str, Any]) -> dict[str, Any]:
    summary = {"requested_market_data_type": attempt.get("requested_market_data_type")}
    for field in ATTEMPT_SUMMARY_FIELDS:
        if attempt.get(field) is not None:
            summary[field] = attempt[field]
    summary["value_present"] = bool(attempt.get("value_present"))
    summary["timed_out"] = bool(attempt.get("_timed_out"))
    return summary


def _quote_values(attempt: dict[str, Any]) -> dict[str, Any]:
    quote: dict[str, Any] = {}
    for field in QUOTE_FIELDS:
        value = attempt.get(field)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            quote[field] = float(value)
    return quote


def _snapshot_classification(deciding: dict[str, Any], *, delayed_fallback: bool) -> dict[str, Any]:
    usable = deciding.get("usable_market_data") == "PASS"
    status = deciding.get("market_data_status") or "NO_VALUE"
    return {
        "usable": usable,
        "feed": status if usable else None,
        "status": status,
        "entitlement": deciding.get("market_data_entitlement") or "UNKNOWN",
        "value_present": bool(deciding.get("value_present")),
        "delayed_fallback": delayed_fallback,
        "timed_out": bool(deciding.get("_timed_out")),
        "error_code": deciding.get("ibkr_error_code"),
        "error_message": deciding.get("ibkr_error_message"),
        "error_class": deciding.get("ibkr_error_class"),
    }


def command_probe(config: Config, timeout: float) -> dict[str, Any]:
    pin = verify_checkout_identity(config)
    module = load_adapter_module(config)
    adapter = _connect(config, module, timeout, "connect")
    identity = _identity(config, pin, adapter)
    clean_disconnect = _disconnect(adapter)
    if not clean_disconnect:
        raise WrapperError("IBKR_DISCONNECT_FAILED", "disconnect", "TWS client did not confirm a clean disconnect")
    envelope = _envelope("probe", ok=True, identity=identity)
    envelope["connected"] = True
    envelope["ready"] = True
    envelope["clean_disconnect"] = True
    return envelope


def command_contract(config: Config, symbol: str, timeout: float, request: dict[str, Any]) -> dict[str, Any]:
    pin = verify_checkout_identity(config)
    module = load_adapter_module(config)
    adapter = _connect(config, module, timeout, "connect")
    try:
        envelope = _envelope("contract", ok=True, identity=_identity(config, pin, adapter))
        contract = _StockContract(symbol, **request)
        resolved, candidates = _resolve_contract(adapter, contract, symbol, timeout)
        envelope["symbol"] = symbol
        envelope["resolution"] = "RESOLVED"
        envelope["resolved"] = resolved
        envelope["candidates"] = candidates
        return envelope
    finally:
        _disconnect(adapter)


def command_snapshot(config: Config, symbol: str, timeout: float, request: dict[str, Any]) -> dict[str, Any]:
    pin = verify_checkout_identity(config)
    module = load_adapter_module(config)
    adapter = _connect(config, module, timeout, "connect")
    try:
        identity = _identity(config, pin, adapter)
        contract = _StockContract(symbol, **request)
        resolved, _candidates = _resolve_contract(adapter, contract, symbol, timeout)
        # Pin the request to the resolved instrument: conId is the identity the
        # consumer reports, so the read and the reported identity are the same.
        if isinstance(resolved.get("con_id"), int):
            contract.conId = resolved["con_id"]

        live = _snapshot_attempt(adapter, contract, SNAPSHOT_LIVE_TYPE, timeout)
        attempts = [_attempt_summary(live)]
        delayed_fallback = False
        deciding = live
        if live.get("usable_market_data") != "PASS" and live.get("market_data_status") != SNAPSHOT_ERROR_STATUS:
            delayed = _snapshot_attempt(adapter, contract, SNAPSHOT_DELAYED_TYPE, timeout)
            attempts.append(_attempt_summary(delayed))
            if delayed.get("usable_market_data") == "PASS":
                deciding = delayed
                delayed_fallback = True
            elif live.get("market_data_status") in SNAPSHOT_CONTINUE_STATUSES:
                deciding = live
            else:
                deciding = delayed

        envelope = _envelope("snapshot", ok=True, identity=identity)
        envelope["symbol"] = symbol
        envelope["contract"] = resolved
        envelope["attempts"] = attempts
        envelope["delayed_fallback"] = delayed_fallback
        envelope["classification"] = _snapshot_classification(deciding, delayed_fallback=delayed_fallback)
        envelope["quote"] = _quote_values(deciding)
        return envelope
    finally:
        _disconnect(adapter)


def _bar_timestamp(value: Any) -> float | None:
    text = str(value or "").strip()
    for fmt in ("%Y%m%d %H:%M:%S", "%Y%m%d"):
        try:
            parsed = datetime.strptime(text, fmt)
        except ValueError:
            continue
        return parsed.replace(tzinfo=timezone.utc).timestamp()
    return None


def _completed_market_end() -> str:
    """A completed US market session, two days back, in IBKR's timestamp format."""

    reference = datetime.now(timezone.utc) - timedelta(days=2)
    while reference.weekday() >= 5:
        reference -= timedelta(days=1)
    return reference.strftime("%Y%m%d 23:59:59 US/Eastern")


def _history_classification(error: Exception | None, bar_count: int) -> dict[str, Any]:
    if error is None and bar_count > 0:
        return {
            "usable": True,
            "feed": "HISTORICAL",
            "status": "OK",
            "entitlement": "AVAILABLE",
            "value_present": True,
            "delayed_fallback": False,
            "timed_out": False,
            "error_code": None,
            "error_message": None,
            "error_class": None,
        }
    message = str(error) if error is not None else "No historical bars were received"
    lowered = message.lower()
    if any(hint in lowered for hint in ENTITLEMENT_HINTS):
        status = "NOT_ENTITLED"
    elif "timeout" in type(error).__name__.lower() or "timed out" in lowered:
        status = "TIMEOUT"
    elif "DataError" in type(error).__name__:
        status = "NO_DATA"
    else:
        status = "ERROR"
    return {
        "usable": False,
        "feed": None,
        "status": status,
        "entitlement": "BLOCKED" if status == "NOT_ENTITLED" else "UNKNOWN",
        "value_present": bar_count > 0,
        "delayed_fallback": False,
        "timed_out": status == "TIMEOUT",
        "error_code": None,
        "error_message": message,
        "error_class": type(error).__name__ if error is not None else None,
    }


def command_history(
    config: Config, symbol: str, timeout: float, request: dict[str, Any], history: dict[str, Any]
) -> dict[str, Any]:
    pin = verify_checkout_identity(config)
    module = load_adapter_module(config)
    adapter = _connect(config, module, timeout, "connect")
    try:
        identity = _identity(config, pin, adapter)
        contract = _StockContract(symbol, **request)
        resolved, _candidates = _resolve_contract(adapter, contract, symbol, timeout)
        if isinstance(resolved.get("con_id"), int):
            contract.conId = resolved["con_id"]

        rows: list[dict[str, Any]] = []
        error: Exception | None = None
        try:
            raw_rows = adapter.read_historical_bars(contract, timeout=timeout, **history)
            rows = [dict(row) for row in raw_rows]
        except Exception as exc:
            error = exc

        bars: list[dict[str, Any]] = []
        for row in rows:
            bar: dict[str, Any] = {"date": str(row.get("date") or "")}
            timestamp = _bar_timestamp(row.get("date"))
            if timestamp is not None:
                bar["timestamp"] = timestamp
            for field in ("open", "high", "low", "close", "volume", "wap"):
                value = row.get(field)
                if isinstance(value, (int, float)) and not isinstance(value, bool):
                    bar[field] = float(value)
            bars.append(bar)

        envelope = _envelope("history", ok=True, identity=identity)
        envelope["symbol"] = symbol
        envelope["contract"] = resolved
        envelope["parameters"] = dict(history)
        envelope["bar_time_basis"] = (
            "IBKR date-only daily bars are mapped to UTC midnight; any intraday "
            "date text is preserved verbatim in the bar's date field"
        )
        envelope["bars"] = bars
        envelope["classification"] = _history_classification(error, len(bars))
        return envelope
    finally:
        _disconnect(adapter)


def _request_options(args: argparse.Namespace) -> dict[str, Any]:
    return {"security_type": args.security_type, "exchange": args.exchange, "currency": args.currency}


def _config_overrides(args: argparse.Namespace) -> dict[str, Any]:
    overrides: dict[str, Any] = {}
    for name in ("host", "port", "client_id"):
        value = getattr(args, name, None)
        if value is not None:
            overrides[name] = value
    return overrides


def _config_path(args: argparse.Namespace) -> Path:
    if args.config:
        return Path(args.config)
    from_env = os.environ.get("FINCEPT_DATA_DIR", "")
    if from_env:
        return Path(from_env) / DEFAULT_CONFIG_RELATIVE
    return Path.cwd() / DEFAULT_CONFIG_RELATIVE


def _add_read_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("symbol")
    parser.add_argument("--timeout", type=float, default=None)
    parser.add_argument("--security-type", default="STK")
    parser.add_argument("--exchange", default="SMART")
    parser.add_argument("--currency", default="USD")


def _add_endpoint_overrides(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--host", dest="host", default=None)
    parser.add_argument("--port", dest="port", type=int, default=None)
    parser.add_argument("--client-id", dest="client_id", type=int, default=None)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="MarketLab read-only IBKR TWS wrapper")
    parser.add_argument("--config", default="", help="path to the local non-secret ibkr_tws.json")
    subparsers = parser.add_subparsers(dest="command", required=True)

    probe = subparsers.add_parser("probe")
    probe.add_argument("--timeout", type=float, default=10.0)
    _add_endpoint_overrides(probe)

    contract = subparsers.add_parser("contract")
    _add_read_arguments(contract)
    _add_endpoint_overrides(contract)

    snapshot = subparsers.add_parser("snapshot")
    _add_read_arguments(snapshot)
    _add_endpoint_overrides(snapshot)

    history = subparsers.add_parser("history")
    _add_read_arguments(history)
    history.add_argument("--duration", default="1 M")
    history.add_argument("--bar-size", default="1 day")
    history.add_argument("--what-to-show", default="TRADES")
    history.add_argument("--end-date-time", default="")
    history.add_argument("--use-rth", default="true")
    _add_endpoint_overrides(history)

    return parser


def _timeout_for(args: argparse.Namespace, default: float) -> float:
    timeout = default if args.timeout is None else float(args.timeout)
    if not (0 < timeout <= 120):
        raise WrapperError("IBKR_CONFIG_INVALID", "config", "timeout must be greater than 0 and at most 120 seconds")
    return timeout


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    command = args.command
    try:
        config = load_config(_config_path(args), _config_overrides(args))
        if command == "probe":
            envelope = command_probe(config, _timeout_for(args, 10.0))
        elif command == "contract":
            envelope = command_contract(config, args.symbol, _timeout_for(args, 20.0), _request_options(args))
        elif command == "snapshot":
            envelope = command_snapshot(config, args.symbol, _timeout_for(args, 15.0), _request_options(args))
        else:
            use_rth = str(args.use_rth).strip().lower() not in {"0", "false", "no"}
            history = {
                "end_date_time": args.end_date_time or _completed_market_end(),
                "duration": args.duration,
                "bar_size": args.bar_size,
                "what_to_show": args.what_to_show,
                "use_rth": use_rth,
            }
            envelope = command_history(
                config, args.symbol, _timeout_for(args, 30.0), _request_options(args), history
            )
    except WrapperError as exc:
        envelope = {
            "source": SOURCE,
            "command": command,
            "ok": False,
            "retrieved_at": _now_iso(),
            "failure": exc.to_dict(),
        }
    except Exception as exc:  # pragma: no cover - unexpected crash path
        envelope = {
            "source": SOURCE,
            "command": command,
            "ok": False,
            "retrieved_at": _now_iso(),
            "failure": {"type": "IBKR_WRAPPER_ERROR", "stage": "wrapper", "message": str(exc)},
        }

    print(json.dumps(envelope, allow_nan=False, default=str))
    return 0 if envelope.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
