"""MarketLab's thin read-only wrapper over the TRADING_DESK IBKR TWS adapter.

This script is the only MarketLab-owned IBKR connection surface. It:

  * reads a non-secret local JSON configuration (no credentials and no account
    identifiers are requested, copied, or persisted);
  * verifies the configured TRADING_DESK checkout identity (exact commit and a
    clean working tree) before importing anything from it;
  * imports the unchanged ``scripts.ibkr_tws.IBKRTWSReadOnlyAdapter`` from that
    checkout;
  * invokes only the adapter's documented read-only methods;
  * re-applies the value and identity validation the reference qualification
    used before it certified a feed (the production adapter reports transport
    and lifecycle facts, not value sanity); and
  * emits one JSON envelope carrying source, retrieval time, contract identity,
    market-data type/entitlement classification, validation outcome, and the
    adapter's normalized values.

Commands:
  probe     connect, wait for readiness, report runtime identity, disconnect
  contract  resolve one stock contract's identity details
  snapshot  read one bounded quote snapshot (live attempt, then an explicit
            delayed fallback when entitlement blocks the live feed)
  history   read bounded historical bars for a completed market window

The adapter exposes no order/account/position surface and this wrapper adds
none. Missing values stay missing; invalid values block the read instead of
being presented as data; entitlement and market-data type are reported exactly
as observed. If the repository pin, official dependency, local configuration,
or output shape cannot be verified, the command fails closed with a typed
failure instead of falling back to another provider.

The value/identity checks below are adapted from the already-qualified
``qualify_ibkr_tws.py`` validators in the pinned checkout (contract identity,
snapshot value sanity, recent-daily-bar sanity and freshness). They are
re-implemented here rather than imported because that qualifier is control
machinery MarketLab deliberately does not consume; the rules are the same.

Configuration (default ``%FINCEPT_DATA_DIR%/ibkr_tws.json``, or ``--config``)::

    {
      "trading_desk_root": "E:\\\\TRADING_DESK",
      "trading_desk_commit": "<40-hex commit>",
      "ibapi_path": "C:\\\\TWS API\\\\source\\\\pythonclient",
      "host": "127.0.0.1",
      "port": 7496,
      "client_id": 71,
      "tws_version": "10.50.1e"
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
import math
import os
import re
import subprocess
import sys
import threading
import types
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
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
SNAPSHOT_FEED_STATUSES = frozenset({"LIVE", "FROZEN", "DELAYED", "DELAYED_FROZEN"})
SNAPSHOT_TERMINAL_STATUSES = frozenset({"NOT_ENTITLED", "NO_VALUE", "ERROR"})
SNAPSHOT_CONTINUE_STATUSES = frozenset({"NOT_ENTITLED", "NO_VALUE"})
SNAPSHOT_ERROR_STATUS = "ERROR"
SNAPSHOT_STATUS_MAP = {1: "LIVE", 2: "FROZEN", 3: "DELAYED", 4: "DELAYED_FROZEN"}
# Subscription/entitlement codes the reference qualifier treats as entitlement
# facts rather than as generic API errors; 10090/10167 precede valid delayed
# data and are never terminal.
ENTITLEMENT_ERROR_CODES = frozenset({354, 2188, 10089, 10090, 10167})
MARKET_DATA_CONTINUE_CODES = frozenset({10090, 10167})
CONTRACT_SECURITY_TYPES = frozenset({"STK", "ETF", "FUND"})
# IBKR's documented unset sentinels.
IBKR_UNSET_DOUBLE = 1.7976931348623157e308
IBKR_UNSET_INTEGER = 2147483647
HISTORY_MAX_AGE_DAYS = 45
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
    "validation_reason",
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


def _finite_number(value: Any) -> bool:
    if isinstance(value, bool) or value is None:
        return False
    try:
        return math.isfinite(float(value))
    except (TypeError, ValueError, OverflowError):
        return False


def _is_unset(value: Any) -> bool:
    """Whether *value* is one of IBKR's documented missing-value sentinels."""

    if value is None or isinstance(value, bool):
        return True
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return True
    if not math.isfinite(number):
        return False
    if number in {-1.0, float(IBKR_UNSET_INTEGER), -float(IBKR_UNSET_INTEGER)}:
        return True
    return number == IBKR_UNSET_DOUBLE


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
        raise WrapperError("IBKR_CONFIG_INVALID", "config", f"host must be a loopback address; got {host!r}")

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
            "IBKR_ADAPTER_IDENTITY_UNAVAILABLE",
            "pin",
            f"Unable to inspect the configured TRADING_DESK checkout: {exc}",
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


def _finish_command(adapter: Any, stage: str, build: Any) -> dict[str, Any]:
    """Run one read and always disconnect; a dirty disconnect is a failure.

    A read that already produced a classified outcome (for example an
    entitlement block) is still a completed read, but it must not claim a clean
    disconnect it did not perform. When the read itself failed, that failure is
    preserved and the disconnect state is not allowed to hide it.
    """

    read_error: Exception | None = None
    envelope: dict[str, Any] | None = None
    try:
        envelope = build()
    except Exception as exc:
        read_error = exc
    clean_disconnect = _disconnect(adapter)
    if read_error is not None:
        if isinstance(read_error, WrapperError):
            raise read_error
        raise _classify_exception(read_error, stage) from read_error
    if not clean_disconnect:
        raise WrapperError(
            "IBKR_DISCONNECT_FAILED", "disconnect", "TWS client did not confirm a clean disconnect after the read"
        )
    if envelope is not None:
        envelope["clean_disconnect"] = True
    return envelope if envelope is not None else {}


def _identity(config: Config, pin: dict[str, Any], adapter: Any) -> dict[str, Any]:
    """Require the observed official runtime identity before any read.

    The acceptance contract fails closed when the official dependency identity
    is missing or unexpected, so runtime metadata is a precondition, not
    optional provenance: a missing or non-official identity refuses the command.
    """

    try:
        raw = adapter.get_runtime_metadata()
    except Exception as exc:
        raise WrapperError(
            "IBKR_OFFICIAL_RUNTIME_UNVERIFIED", "dependency", f"Adapter runtime metadata is unavailable: {exc}"
        ) from exc
    if not isinstance(raw, dict):
        raise WrapperError(
            "IBKR_OFFICIAL_RUNTIME_UNVERIFIED", "dependency", "Adapter runtime metadata is missing"
        )
    runtime = {
        "ibapi_version": raw.get("ibkr_tws_api_version", ""),
        "ibapi_version_source": raw.get("ibkr_tws_api_version_source", ""),
        "ibapi_location": raw.get("ibapi_location", ""),
        "ibapi_runtime_path_verified": raw.get("ibapi_runtime_path_verified", "false"),
        "uses_official_runtime": raw.get("uses_official_runtime", "false"),
        "tws_version": raw.get("tws_version", ""),
        "tws_version_source": raw.get("tws_version_source", ""),
    }
    if str(runtime["uses_official_runtime"]).lower() != "true":
        raise WrapperError(
            "IBKR_OFFICIAL_RUNTIME_UNVERIFIED", "dependency", "Adapter did not report the official ibapi runtime"
        )
    if str(runtime["ibapi_runtime_path_verified"]).lower() != "true":
        raise WrapperError(
            "IBKR_OFFICIAL_RUNTIME_UNVERIFIED", "dependency", "Adapter did not verify the approved ibapi path"
        )
    if not str(runtime["ibapi_version"]).strip():
        raise WrapperError(
            "IBKR_OFFICIAL_RUNTIME_UNVERIFIED", "dependency", "Adapter reported no observed ibapi version"
        )
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


# ── Contract identity (adapted from the reference qualifier) ────────────────


def _contract_row_reason(
    row: Any, expected_symbol: str, expected_currency: str, allowed_types: frozenset[str]
) -> str | None:
    """Return why *row* is not an acceptable identity, or None when it is.

    Same constraints the reference qualification enforced: matching symbol, a
    positive integer conId, the expected currency, a supported security type,
    and a named exchange.
    """

    if not isinstance(row, dict):
        return "CONTRACT_ROW_INVALID"
    if str(row.get("symbol") or "").strip().upper() != expected_symbol:
        return "SYMBOL_MISMATCH"
    con_id = row.get("con_id")
    if con_id is None or isinstance(con_id, bool):
        return "CONID_MISSING"
    try:
        if int(con_id) <= 0 or float(con_id) != int(con_id):
            return "CONID_INVALID"
    except (TypeError, ValueError, OverflowError):
        return "CONID_INVALID"
    if not str(row.get("currency") or "").strip():
        return "CURRENCY_MISSING"
    if str(row.get("currency")).strip().upper() != expected_currency.upper():
        return "CURRENCY_MISMATCH"
    security_type = str(row.get("security_type") or "").strip().upper()
    if not security_type:
        return "SECURITY_TYPE_MISSING"
    if security_type not in allowed_types:
        return "SECURITY_TYPE_UNSUPPORTED"
    if not str(row.get("exchange") or "").strip():
        return "EXCHANGE_MISSING"
    return None


def _resolve_contract(
    adapter: Any,
    contract: _StockContract,
    symbol: str,
    timeout: float,
    currency: str,
    security_type: str,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    try:
        rows = [dict(row) for row in adapter.read_contract_details(contract, timeout=timeout)]
    except Exception as exc:
        raise _classify_exception(exc, "contract") from exc

    expected = symbol.strip().upper()
    if not rows:
        raise WrapperError(
            "IBKR_CONTRACT_NOT_RESOLVED", "contract", f"No contract details were returned for {expected}"
        )
    requested_type = security_type.strip().upper()
    allowed_types = CONTRACT_SECURITY_TYPES if requested_type == "STK" else frozenset({requested_type})

    acceptable: list[dict[str, Any]] = []
    first_failure: str | None = None
    for row in rows:
        reason = _contract_row_reason(row, expected, currency, allowed_types)
        if reason is None:
            acceptable.append(row)
        elif first_failure is None:
            first_failure = reason

    if not acceptable:
        if first_failure == "SYMBOL_MISMATCH":
            raise WrapperError(
                "IBKR_CONTRACT_NOT_RESOLVED",
                "contract",
                f"No contract details matched symbol {expected}",
                details={"candidate_count": len(rows)},
            )
        raise WrapperError(
            "IBKR_CONTRACT_IDENTITY_INVALID",
            "contract",
            f"Contract details for {expected} failed identity validation: {first_failure}",
            details={"reason": first_failure, "candidate_count": len(rows)},
        )

    identities: dict[int, dict[str, Any]] = {}
    for row in acceptable:
        identities.setdefault(int(row["con_id"]), row)
    if len(identities) != 1:
        raise WrapperError(
            "IBKR_CONTRACT_AMBIGUOUS",
            "contract",
            f"Contract details for {expected} resolved to {len(identities)} distinct instruments",
            details={"con_ids": sorted(identities)},
        )
    return next(iter(identities.values())), rows


def _resolve_contract_row(
    adapter: Any, contract: _StockContract, symbol: str, timeout: float, request: dict[str, Any]
) -> dict[str, Any]:
    resolved, _rows = _resolve_contract(
        adapter, contract, symbol, timeout, request.get("currency", "USD"), request.get("security_type", "STK")
    )
    if isinstance(resolved.get("con_id"), int):
        contract.conId = resolved["con_id"]
    return resolved


# ── Snapshot value validation (adapted from the reference qualifier) ────────


def _snapshot_value_error(snapshot: dict[str, Any]) -> bool:
    """Reject values the adapter cannot have meant: non-positive prices, a
    crossed book, non-finite numbers, or negative sizes/volume."""

    prices: list[float] = []
    for name in ("bid", "ask", "last", "close"):
        raw = snapshot.get(name)
        if raw is None or _is_unset(raw):
            continue
        if not _finite_number(raw) or float(raw) <= 0:
            return True
        prices.append(float(raw))
    if not prices:
        return False
    bid = snapshot.get("bid")
    ask = snapshot.get("ask")
    if (
        bid is not None
        and ask is not None
        and not _is_unset(bid)
        and not _is_unset(ask)
        and _finite_number(bid)
        and _finite_number(ask)
        and float(bid) > float(ask)
    ):
        return True
    for name in ("bid_size", "ask_size", "last_size", "volume"):
        raw = snapshot.get(name)
        if raw is None or _is_unset(raw):
            continue
        if not _finite_number(raw) or float(raw) < 0:
            return True
    return False


def _adapter_error_status(snapshot: dict[str, Any]) -> str | None:
    """The adapter's own normalized error fields, excluding continuation codes."""

    error_class = str(snapshot.get("ibkr_error_class") or "").strip().upper()
    if error_class not in {"ERROR", "ENTITLEMENT"}:
        return None
    raw = snapshot.get("ibkr_error_code")
    code: int | None = None
    if raw is not None and not isinstance(raw, bool):
        try:
            code = int(raw)
        except (TypeError, ValueError, OverflowError):
            code = None
    if code in MARKET_DATA_CONTINUE_CODES:
        return None
    return "NOT_ENTITLED" if error_class == "ENTITLEMENT" else "ERROR"


def _validated_snapshot(snapshot: dict[str, Any]) -> dict[str, Any]:
    """Validate one adapter snapshot result and return the effective outcome.

    The adapter certifies transport, lifecycle, and tick presence; it does not
    check value sanity. This function re-applies the reference qualifier's
    gates so a completed request with crossed/zero/non-finite values is an
    explicit failure rather than a usable quote.
    """

    result = dict(snapshot)
    explicit = str(result.get("market_data_status") or result.get("status") or "").strip().upper()
    timed_out = result.get("_timed_out") is True

    if explicit in SNAPSHOT_TERMINAL_STATUSES:
        status = explicit
    elif (
        explicit in {"BLOCKED_FOR_ENTITLEMENT", "ENTITLEMENT_BLOCKED"}
        or result.get("market_data_entitlement") == "BLOCKED"
    ):
        status = "NOT_ENTITLED"
    else:
        adapter_error = _adapter_error_status(result)
        if adapter_error is not None:
            status = adapter_error
        elif _snapshot_value_error(result):
            status = "ERROR"
            result["validation_reason"] = "SNAPSHOT_VALUES_INVALID"
        else:
            has_price = any(
                result.get(name) is not None and not _is_unset(result.get(name))
                for name in ("bid", "ask", "last", "close")
            )
            data_type = explicit if explicit in SNAPSHOT_FEED_STATUSES else None
            if data_type is None:
                data_type = SNAPSHOT_STATUS_MAP.get(result.get("market_data_type"))
            if data_type is not None:
                status = data_type if has_price else "NO_VALUE"
            else:
                status = "ERROR" if has_price else "NO_VALUE"

    if status == "NOT_ENTITLED":
        result.update(
            market_data_status="NOT_ENTITLED",
            usable_market_data="BLOCKED_FOR_ENTITLEMENT",
            market_data_entitlement="BLOCKED",
            value_present=False,
        )
    elif status == "ERROR":
        result.update(
            market_data_status="ERROR",
            usable_market_data="FAIL",
            market_data_entitlement="UNKNOWN",
            validation_reason=result.get("validation_reason") or (
                "SNAPSHOT_LIFECYCLE_INCOMPLETE" if timed_out else "SNAPSHOT_API_OR_ADAPTER_ERROR"
            ),
        )
    elif status == "NO_VALUE":
        result.update(
            market_data_status="NO_VALUE",
            usable_market_data="FAIL",
            market_data_entitlement="UNKNOWN",
            value_present=False,
            validation_reason="SNAPSHOT_NO_USABLE_PRICE",
        )
    else:
        entitlement = "DELAYED" if status in {"DELAYED", "DELAYED_FROZEN"} else "AVAILABLE"
        if timed_out:
            result.update(
                market_data_status="ERROR",
                usable_market_data="FAIL",
                market_data_entitlement=entitlement,
                validation_reason="SNAPSHOT_LIFECYCLE_INCOMPLETE",
            )
        else:
            result.update(
                market_data_status=status,
                usable_market_data="PASS",
                market_data_entitlement=entitlement,
                value_present=True,
                validation_reason="OK",
            )
    return result


def _snapshot_attempt(adapter: Any, contract: _StockContract, market_data_type: int, timeout: float) -> dict[str, Any]:
    try:
        adapter.set_market_data_type(market_data_type)
        result = dict(adapter.read_market_data_snapshot(contract, timeout=timeout, snapshot=True))
    except Exception as exc:
        raise _classify_exception(exc, "snapshot") from exc
    result.pop("_errors", None)
    result.pop("_diagnostics", None)
    result["requested_market_data_type"] = market_data_type
    return _validated_snapshot(result)


def _attempt_summary(attempt: dict[str, Any]) -> dict[str, Any]:
    summary = {"requested_market_data_type": attempt.get("requested_market_data_type")}
    for field in ATTEMPT_SUMMARY_FIELDS:
        if attempt.get(field) is not None:
            summary[field] = attempt[field]
    summary["value_present"] = bool(attempt.get("value_present"))
    summary["timed_out"] = bool(attempt.get("_timed_out"))
    return summary


def _quote_values(attempt: dict[str, Any]) -> dict[str, Any]:
    if attempt.get("usable_market_data") != "PASS":
        return {}
    quote: dict[str, Any] = {}
    for field in QUOTE_FIELDS:
        value = attempt.get(field)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            quote[field] = float(value)
    return quote


def _snapshot_classification(
    deciding: dict[str, Any],
    *,
    delayed_fallback: bool,
    live: dict[str, Any],
    delayed: dict[str, Any] | None,
) -> dict[str, Any]:
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
        "validation_reason": deciding.get("validation_reason"),
        # Both attempts stay visible: the deciding live block must not hide a
        # genuine delayed-attempt failure.
        "live_market_data_status": live.get("market_data_status"),
        "live_usable_market_data": live.get("usable_market_data"),
        "delayed_attempted": delayed is not None,
        "delayed_market_data_status": delayed.get("market_data_status") if delayed else None,
        "delayed_usable_market_data": delayed.get("usable_market_data") if delayed else None,
        "delayed_validation_reason": delayed.get("validation_reason") if delayed else None,
        "delayed_error_code": delayed.get("ibkr_error_code") if delayed else None,
        "delayed_error_message": delayed.get("ibkr_error_message") if delayed else None,
    }


def command_probe(config: Config, timeout: float) -> dict[str, Any]:
    pin = verify_checkout_identity(config)
    module = load_adapter_module(config)
    adapter = _connect(config, module, timeout, "connect")

    def build() -> dict[str, Any]:
        identity = _identity(config, pin, adapter)
        envelope = _envelope("probe", ok=True, identity=identity)
        envelope["connected"] = True
        envelope["ready"] = True
        return envelope

    return _finish_command(adapter, "probe", build)


def command_contract(config: Config, symbol: str, timeout: float, request: dict[str, Any]) -> dict[str, Any]:
    pin = verify_checkout_identity(config)
    module = load_adapter_module(config)
    adapter = _connect(config, module, timeout, "connect")

    def build() -> dict[str, Any]:
        contract = _StockContract(symbol, **request)
        resolved, candidates = _resolve_contract(
            adapter, contract, symbol, timeout, request.get("currency", "USD"),
            request.get("security_type", "STK"),
        )
        envelope = _envelope("contract", ok=True, identity=_identity(config, pin, adapter))
        envelope["symbol"] = symbol
        envelope["resolution"] = "RESOLVED"
        envelope["resolved"] = resolved
        envelope["candidates"] = candidates
        return envelope

    return _finish_command(adapter, "contract", build)


def command_snapshot(config: Config, symbol: str, timeout: float, request: dict[str, Any]) -> dict[str, Any]:
    pin = verify_checkout_identity(config)
    module = load_adapter_module(config)
    adapter = _connect(config, module, timeout, "connect")

    def build() -> dict[str, Any]:
        identity = _identity(config, pin, adapter)
        contract = _StockContract(symbol, **request)
        resolved = _resolve_contract_row(adapter, contract, symbol, timeout, request)

        live = _snapshot_attempt(adapter, contract, SNAPSHOT_LIVE_TYPE, timeout)
        attempts = [_attempt_summary(live)]
        delayed: dict[str, Any] | None = None
        delayed_fallback = False
        deciding = live
        if live.get("usable_market_data") != "PASS" and live.get("market_data_status") != SNAPSHOT_ERROR_STATUS:
            delayed = _snapshot_attempt(adapter, contract, SNAPSHOT_DELAYED_TYPE, timeout)
            attempts.append(_attempt_summary(delayed))
            if delayed.get("usable_market_data") == "PASS":
                deciding = delayed
                delayed_fallback = True
            elif live.get("market_data_status") in SNAPSHOT_CONTINUE_STATUSES:
                # The reference qualifier keeps the live entitlement answer as
                # the deciding one and attaches the delayed facts; the delayed
                # attempt is never silently discarded.
                deciding = live
            else:
                deciding = delayed

        envelope = _envelope("snapshot", ok=True, identity=identity)
        envelope["symbol"] = symbol
        envelope["contract"] = resolved
        envelope["attempts"] = attempts
        envelope["delayed_fallback"] = delayed_fallback
        envelope["classification"] = _snapshot_classification(
            deciding, delayed_fallback=delayed_fallback, live=live, delayed=delayed
        )
        envelope["quote"] = _quote_values(deciding)
        return envelope

    return _finish_command(adapter, "snapshot", build)


# ── Historical-bar validation (adapted from the reference qualifier) ────────


def _bar_timestamp(value: Any) -> float | None:
    text = str(value or "").strip()
    for fmt in ("%Y%m%d %H:%M:%S", "%Y%m%d"):
        try:
            parsed = datetime.strptime(text, fmt)
        except ValueError:
            continue
        return parsed.replace(tzinfo=timezone.utc).timestamp()
    return None


def _bar_date(value: Any) -> date | None:
    text = str(value or "").strip()
    if not text:
        return None
    for fmt in ("%Y%m%d", "%Y%m", "%Y-%m-%d", "%Y/%m/%d"):
        try:
            return datetime.strptime(text[: len(datetime.now().strftime(fmt))], fmt).date()
        except ValueError:
            continue
    return None


def _completed_market_end() -> str:
    """A completed US market session, two days back, in IBKR's timestamp format."""

    reference = datetime.now(timezone.utc) - timedelta(days=2)
    while reference.weekday() >= 5:
        reference -= timedelta(days=1)
    return reference.strftime("%Y%m%d 23:59:59 US/Eastern")


def _history_value_problem(rows: list[dict[str, Any]]) -> tuple[str | None, int | None]:
    """Validate the returned series the way the reference qualification did.

    Returns the first problem reason and row index, or (None, None) when the
    series passes positive-price/finite/OHLC/finite-volume checks, is strictly
    chronological without duplicates, is not in the future, and is fresh enough
    for the bounded completed window.
    """

    parsed_dates: list[date] = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            return "BAR_ROW_INVALID", index
        row_date = _bar_date(row.get("date"))
        if row_date is None:
            return "BAR_DATE_INVALID", index
        parsed_dates.append(row_date)
        values: dict[str, float] = {}
        for name in ("open", "high", "low", "close"):
            raw = row.get(name)
            if raw is None or _is_unset(raw):
                return f"BAR_{name.upper()}_MISSING_OR_UNSET", index
            if not _finite_number(raw) or float(raw) <= 0:
                return f"BAR_{name.upper()}_INVALID", index
            values[name] = float(raw)
        volume = row.get("volume")
        if volume is None or _is_unset(volume):
            return "BAR_VOLUME_MISSING_OR_UNSET", index
        if not _finite_number(volume):
            return "BAR_VOLUME_INVALID", index
        if float(volume) < 0:
            return "BAR_VOLUME_NEGATIVE", index
        if not (
            values["high"] >= values["open"]
            and values["high"] >= values["close"]
            and values["high"] >= values["low"]
            and values["low"] <= values["open"]
            and values["low"] <= values["close"]
        ):
            return "BAR_OHLC_RELATION_INVALID", index

    if any(left >= right for left, right in zip(parsed_dates, parsed_dates[1:])):
        reason = "BAR_DATES_DUPLICATED" if len(set(parsed_dates)) != len(parsed_dates) else "BAR_DATES_NOT_CHRONOLOGICAL"
        return reason, None

    reference_date = datetime.now(timezone.utc).date()
    if parsed_dates[-1] > reference_date:
        return "BAR_DATE_IN_FUTURE", None
    if (reference_date - parsed_dates[-1]).days > HISTORY_MAX_AGE_DAYS:
        return "HISTORY_STALE", None
    return None, None


def _history_classification(
    error: Exception | None,
    bar_count: int,
    value_problem: tuple[str | None, int | None] = (None, None),
) -> dict[str, Any]:
    if error is None and value_problem[0] is None:
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
            "validation_reason": "OK",
        }
    if value_problem[0] is not None:
        reason, row_index = value_problem
        return {
            "usable": False,
            "feed": None,
            "status": "STALE" if reason == "HISTORY_STALE" else "VALUES_INVALID",
            "entitlement": "UNKNOWN",
            "value_present": bar_count > 0,
            "delayed_fallback": False,
            "timed_out": False,
            "error_code": None,
            "error_message": reason,
            "error_class": None,
            "row_index": row_index,
            "validation_reason": reason,
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
        "validation_reason": status,
    }


def command_history(
    config: Config, symbol: str, timeout: float, request: dict[str, Any], history: dict[str, Any]
) -> dict[str, Any]:
    pin = verify_checkout_identity(config)
    module = load_adapter_module(config)
    adapter = _connect(config, module, timeout, "connect")

    def build() -> dict[str, Any]:
        identity = _identity(config, pin, adapter)
        contract = _StockContract(symbol, **request)
        resolved = _resolve_contract_row(adapter, contract, symbol, timeout, request)

        rows: list[dict[str, Any]] = []
        error: Exception | None = None
        try:
            rows = [dict(row) for row in adapter.read_historical_bars(contract, timeout=timeout, **history)]
        except Exception as exc:
            error = exc

        value_problem = _history_value_problem(rows) if error is None else (None, None)
        bars: list[dict[str, Any]] = []
        if error is None and value_problem[0] is None:
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
        envelope["classification"] = _history_classification(error, len(rows), value_problem)
        return envelope

    return _finish_command(adapter, "history", build)


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
