#!/usr/bin/env python3
"""MarketLab FedWatch backend CLI (Batch A current data + Batch B history).

Runtime owner: MarketLab Terminal. This script is launched by the application
through the existing economics Python-runner path and prints exactly one JSON
document on stdout — either an economics success envelope
``{"success": true, "data": ...}`` (optionally with ``partial`` /
``failed_components`` when a composite carried provider failures) or an
explicit provider-attributed error envelope ``{"error": {...}}``.

Current-data commands (Batch A, unchanged):
    snapshot         composite current snapshot (all providers)
    fed_side         Investing distributions + normalized values + local steps
    fred_target      FRED DFEDTARU/DFEDTARL current target range
    fomc_meetings    Federal Reserve FOMC calendar (scrape or fallback snapshot)
    polymarket_fomc  FOMC event discovery, mapping validation, current prices

Durable history commands (Batch B):
    collect             current snapshot + record accepted observations +
                        advance the FOMC meeting lifecycle
    history_meetings    durable meeting lifecycle and observation coverage
    history_series      stored observation episodes for a meeting/outcome
    history_analytics   approved historical calculations for a meeting outcome
    history_backfill    idempotent Polymarket CLOB history backfill for
                        validated mappings
    history_zq_import   optional historical ZQ reconstruction import (the
                        user's raw ZQ dataset is never copied)

The capability is read-only research: no broker, wallet, order or execution
path exists in this script or the package it dispatches to.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from fedwatch import analytics as fedwatch_analytics  # noqa: E402
from fedwatch import history as fedwatch_history  # noqa: E402
from fedwatch import snapshot as fedwatch_snapshot  # noqa: E402
from fedwatch import store as fedwatch_store  # noqa: E402
from fedwatch import timeutil as fedwatch_timeutil  # noqa: E402
from fedwatch.errors import FedwatchError  # noqa: E402
from fedwatch.transport import HttpTransport  # noqa: E402

USAGE = {
    "script": "fedwatch_data.py",
    "description": (
        "MarketLab FedWatch backend: current observations and durable "
        "historical research data (read-only research). Prints one JSON "
        "document per invocation."
    ),
    "commands": {
        "snapshot": "composite current snapshot across all providers",
        "fed_side": "Investing.com distribution, normalization and local-step conversion",
        "fred_target": "current FRED DFEDTARU/DFEDTARL target range",
        "fomc_meetings": "Federal Reserve FOMC calendar",
        "polymarket_fomc": "Polymarket FOMC discovery, mapping validation and current prices",
        "collect": "current snapshot + durable recording + meeting lifecycle",
        "history_meetings": "durable meeting lifecycle and observation coverage",
        "history_series": "stored observations for a meeting outcome",
        "history_analytics": "historical probability calculations for a meeting outcome",
        "history_backfill": "Polymarket CLOB history backfill for validated mappings",
        "history_zq_import": "optional historical ZQ reconstruction import",
    },
    "usage": "python fedwatch_data.py <command> [options]",
    "history_options": {
        "--db": "explicit history database path (default: FINCEPT_DATA_DIR/fedwatch/fedwatch_history.db)",
        "history_series": "--meeting YYYY-MM-DD [--method METHOD] [--outcome-bp N] [--open-ended]",
        "history_analytics": "--meeting YYYY-MM-DD --outcome-bp N [--open-ended] "
        "[--fed-method METHOD] [--as-of ISO-INSTANT]",
        "history_backfill": "[--meeting YYYY-MM-DD ...] [--force] [--refresh-hours HOURS]",
        "history_zq_import": "--data-dir PATH --watch-date YYYY-MM-DD [--watch-date ...]",
    },
}


def _envelope(payload: dict) -> dict:
    errors = payload.get("errors") or []
    if errors:
        providers = sorted({entry.get("provider", "unknown") for entry in errors})
        return {
            "success": True,
            "data": payload,
            "partial": True,
            "failed_components": providers,
        }
    return {"success": True, "data": payload}


class UnknownCommandError(Exception):
    """Raised only for a command that is not in the dispatch table."""


class InvalidArgumentsError(Exception):
    """Raised for a command line that does not match the command's options."""


class _Parser(argparse.ArgumentParser):
    def error(self, message):  # noqa: D401 - argparse override
        raise InvalidArgumentsError(message)


def _parser(prog: str) -> _Parser:
    return _Parser(prog=prog, add_help=False)


def _open_store(args) -> fedwatch_store.FedwatchHistoryStore:
    path = args.db if getattr(args, "db", None) else fedwatch_store.default_db_path()
    return fedwatch_store.FedwatchHistoryStore(path)


def _collect(args) -> dict:
    store = _open_store(args)
    snapshot_envelope = fedwatch_snapshot.build_snapshot()
    data = dict(snapshot_envelope.get("data") or {})
    history_result = fedwatch_history.collect(store, data)
    data["history"] = history_result
    data["errors"] = list(data.get("errors") or []) + list(history_result.get("errors") or [])
    return _envelope(data)


def _history_meetings(args) -> dict:
    store = _open_store(args)
    overview = fedwatch_history.meetings_overview(store)
    return _envelope(overview)


def _history_series(args) -> dict:
    if not args.meeting:
        raise InvalidArgumentsError("--meeting is required")
    store = _open_store(args)
    payload = fedwatch_history.series(
        store,
        args.meeting,
        method=args.method,
        outcome_bp=args.outcome_bp,
        open_ended=args.open_ended if args.open_ended else None,
    )
    return _envelope(payload)


def _history_analytics(args) -> dict:
    if not args.meeting:
        raise InvalidArgumentsError("--meeting is required")
    if args.outcome_bp is None:
        raise InvalidArgumentsError("--outcome-bp is required")
    if args.fed_method not in fedwatch_analytics.FED_METHODS:
        raise InvalidArgumentsError(
            f"--fed-method must be one of {list(fedwatch_analytics.FED_METHODS)!r}"
        )
    as_of = None
    if args.as_of:
        try:
            as_of = fedwatch_timeutil.parse_iso_z(args.as_of)
        except ValueError as exc:
            raise InvalidArgumentsError(f"--as-of is not a valid UTC instant: {args.as_of!r}") from exc
    store = _open_store(args)
    payload = fedwatch_analytics.compute_analytics(
        store,
        args.meeting,
        args.outcome_bp,
        open_ended=args.open_ended,
        fed_method=args.fed_method,
        as_of=as_of,
    )
    return _envelope(payload)


def _history_backfill(args) -> dict:
    store = _open_store(args)
    payload = fedwatch_history.backfill_polymarket(
        store,
        meeting_dates=args.meeting or None,
        force=args.force,
        refresh_hours=args.refresh_hours,
    )
    return _envelope(payload)


def _history_zq_import(args) -> dict:
    if not args.data_dir:
        raise InvalidArgumentsError("--data-dir is required")
    if not args.watch_date:
        raise InvalidArgumentsError("at least one --watch-date is required")
    watch_dates = []
    for value in args.watch_date:
        try:
            watch_dates.append(fedwatch_timeutil.parse_date(value))
        except ValueError as exc:
            raise InvalidArgumentsError(f"--watch-date is not a valid date: {value!r}") from exc
    store = _open_store(args)
    payload = fedwatch_history.import_zq(
        store,
        HttpTransport(),
        Path(args.data_dir),
        watch_dates,
    )
    return _envelope(payload)


def _dispatch(command: str, argv: list[str]) -> dict:
    builders = {
        "snapshot": lambda: fedwatch_snapshot.build_snapshot(),
        "fed_side": lambda: _envelope(fedwatch_snapshot.build_fed_side_command()),
        "fred_target": lambda: _envelope(fedwatch_snapshot.build_fred_target_command()),
        "fomc_meetings": lambda: _envelope(fedwatch_snapshot.build_fomc_meetings_command()),
        "polymarket_fomc": lambda: _envelope(fedwatch_snapshot.build_polymarket_command()),
    }
    if command in builders:
        if argv:
            raise InvalidArgumentsError(f"{command} does not accept arguments: {argv!r}")
        return builders[command]()

    if command == "collect":
        parser = _parser(command)
        parser.add_argument("--db", default=None)
        return _collect(parser.parse_args(argv))

    if command == "history_meetings":
        parser = _parser(command)
        parser.add_argument("--db", default=None)
        return _history_meetings(parser.parse_args(argv))

    if command == "history_series":
        parser = _parser(command)
        parser.add_argument("--db", default=None)
        parser.add_argument("--meeting", default=None)
        parser.add_argument("--method", default=None)
        parser.add_argument("--outcome-bp", type=int, default=None)
        parser.add_argument("--open-ended", action="store_true")
        return _history_series(parser.parse_args(argv))

    if command == "history_analytics":
        parser = _parser(command)
        parser.add_argument("--db", default=None)
        parser.add_argument("--meeting", default=None)
        parser.add_argument("--outcome-bp", type=int, default=None)
        parser.add_argument("--open-ended", action="store_true")
        parser.add_argument("--fed-method", default=fedwatch_history.FED_METHOD_LIVE)
        parser.add_argument("--as-of", default=None)
        return _history_analytics(parser.parse_args(argv))

    if command == "history_backfill":
        parser = _parser(command)
        parser.add_argument("--db", default=None)
        parser.add_argument("--meeting", action="append", default=None)
        parser.add_argument("--force", action="store_true")
        parser.add_argument("--refresh-hours", type=float, default=fedwatch_history.BACKFILL_REFRESH_HOURS)
        return _history_backfill(parser.parse_args(argv))

    if command == "history_zq_import":
        parser = _parser(command)
        parser.add_argument("--db", default=None)
        parser.add_argument("--data-dir", default=None)
        parser.add_argument("--watch-date", action="append", default=None)
        return _history_zq_import(parser.parse_args(argv))

    raise UnknownCommandError(command)


def _error_document(code: str, provider: str, message: str, detail: dict | None = None) -> dict:
    error = {"error": message, "provider": provider, "code": code}
    if detail:
        error["detail"] = detail
    return {"error": error}


def main(args: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if args is None else args)
    if not argv or argv[0] in ("-h", "--help", "help"):
        try:
            print(json.dumps(USAGE, indent=2))
        except ValueError:
            pass
        return 0 if argv else 1

    command = argv[0]
    try:
        payload = _dispatch(command, argv[1:])
    except UnknownCommandError:
        print(
            json.dumps(
                _error_document(
                    "FEDWATCH_UNKNOWN_COMMAND",
                    "fedwatch",
                    f"unknown command {command!r}",
                    {"known_commands": sorted(USAGE["commands"])},
                ),
                indent=2,
            )
        )
        return 1
    except InvalidArgumentsError as exc:
        print(
            json.dumps(
                _error_document(
                    "FEDWATCH_INVALID_ARGUMENTS",
                    "fedwatch",
                    f"invalid arguments for {command!r}: {exc}",
                ),
                indent=2,
            )
        )
        return 1
    except FedwatchError as exc:
        print(json.dumps({"error": exc.to_dict()}, indent=2))
        return 1
    except Exception as exc:  # noqa: BLE001 - the CLI must never traceback
        print(
            json.dumps(
                {
                    "error": {
                        "error": f"unexpected FedWatch failure: {type(exc).__name__}: {exc}",
                        "provider": "fedwatch",
                        "code": "FEDWATCH_INTERNAL_ERROR",
                    }
                },
                indent=2,
            )
        )
        return 1

    try:
        print(json.dumps(payload, indent=2, allow_nan=False))
    except ValueError as exc:
        print(
            json.dumps(
                {
                    "error": {
                        "error": f"FedWatch payload is not JSON-serializable: {exc}",
                        "provider": "fedwatch",
                        "code": "FEDWATCH_SERIALIZATION_ERROR",
                    }
                },
                indent=2,
            )
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
