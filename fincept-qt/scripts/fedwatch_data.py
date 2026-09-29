#!/usr/bin/env python3
"""MarketLab FedWatch backend CLI (Batch A).

Runtime owner: MarketLab Terminal. This script is launched by the application
through the existing economics Python-runner path and prints exactly one JSON
document on stdout — either an economics success envelope
``{"success": true, "data": ...}`` (optionally with ``partial`` /
``failed_components`` when a composite carried provider failures) or an
explicit provider-attributed error envelope ``{"error": {...}}``.

Commands:
    snapshot         composite current snapshot (all providers)
    fed_side         Investing distributions + normalized values + local steps
    fred_target      FRED DFEDTARU/DFEDTARL current target range
    fomc_meetings    Federal Reserve FOMC calendar (scrape or fallback snapshot)
    polymarket_fomc  FOMC event discovery, mapping validation, current prices

The capability is read-only research: no broker, wallet, order or execution
path exists in this script or the package it dispatches to.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from fedwatch import snapshot as fedwatch_snapshot  # noqa: E402
from fedwatch.errors import FedwatchError  # noqa: E402

USAGE = {
    "script": "fedwatch_data.py",
    "description": (
        "MarketLab FedWatch current-observation backend (read-only research). "
        "Prints one JSON document per invocation."
    ),
    "commands": {
        "snapshot": "composite current snapshot across all providers",
        "fed_side": "Investing.com distribution, normalization and local-step conversion",
        "fred_target": "current FRED DFEDTARU/DFEDTARL target range",
        "fomc_meetings": "Federal Reserve FOMC calendar",
        "polymarket_fomc": "Polymarket FOMC discovery, mapping validation and current prices",
    },
    "usage": "python fedwatch_data.py <command>",
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


def _dispatch(command: str) -> dict:
    builders = {
        "snapshot": lambda: fedwatch_snapshot.build_snapshot(),
        "fed_side": lambda: _envelope(fedwatch_snapshot.build_fed_side_command()),
        "fred_target": lambda: _envelope(fedwatch_snapshot.build_fred_target_command()),
        "fomc_meetings": lambda: _envelope(fedwatch_snapshot.build_fomc_meetings_command()),
        "polymarket_fomc": lambda: _envelope(fedwatch_snapshot.build_polymarket_command()),
    }
    if command not in builders:
        raise UnknownCommandError(command)
    return builders[command]()


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
        payload = _dispatch(command)
    except UnknownCommandError:
        print(
            json.dumps(
                {
                    "error": {
                        "error": f"unknown command {command!r}",
                        "provider": "fedwatch",
                        "code": "FEDWATCH_UNKNOWN_COMMAND",
                        "detail": {"known_commands": sorted(USAGE["commands"])},
                    }
                },
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
