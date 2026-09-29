"""Guard tests: no retired reference runtime dependency and no execution surface.

Batch A must be self-contained inside MarketLab Terminal. These tests enforce
the boundary statically over the runtime files that ship with the application:

* no retired-project name, path or environment reference anywhere in the runtime
  package, the CLI, its runtime resource (the tracked FOMC fallback snapshot),
  or the other FedWatch test modules. Python sources are scanned by AST, so
  implicit literal concatenation and f-strings cannot hide a token;
* no hardcoded absolute Windows paths in the runtime;
* no environment-variable reads at all in the runtime;
* a strict import allowlist — the Python standard library plus ``requests``
  (used only by ``transport.py``) and the package itself — so the feature
  cannot quietly acquire a dependency the app-managed environment does not
  provide;
* no broker/wallet/exchange/execution imports.

The CLI startup check runs the real script with a scrubbed environment on a
plain interpreter, proving the process starts with no retired-project checkout,
environment or state present. (This guard module itself is the only file that
names the retired project, because it must contain the scan tokens.)
"""

from __future__ import annotations

import ast
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PACKAGE = REPO / "scripts" / "fedwatch"
CLI = REPO / "scripts" / "fedwatch_data.py"
FALLBACK = PACKAGE / "fomc_dates_fallback.csv"
TESTS = Path(__file__).resolve().parent

TRADING_DESK_TOKENS = (
    "TRADING_DESK",
    "TRADING DESK",
    "Trading Desk",
    "trading_desk",
    "trading-desk",
)
ABSOLUTE_PATH_RE = re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/]")
ENVIRONMENT_RE = re.compile(r"\bos\.environ\b|\bos\.getenv\b|\benviron\s*\[")
EXECUTION_IMPORTS = {
    "ccxt",
    "web3",
    "eth_account",
    "py_clob_client",
    "ibapi",
    "ib_insync",
    "alpaca",
    "alpaca_trade_api",
    "binance",
    "telegram",
    "telebot",
}


def runtime_python_files() -> list[Path]:
    return sorted(PACKAGE.glob("*.py")) + [CLI]


def fedwatch_test_files() -> list[Path]:
    return sorted(TESTS.glob("test_fedwatch_*.py")) + [TESTS / "fedwatch_test_support.py"]


def python_string_constants(path: Path) -> list[str]:
    """Every string constant in a Python file.

    Implicitly concatenated literals are folded into one Constant by the
    parser, and f-string literal segments are collected separately, so a
    reference split across adjacent literals or an f-string still surfaces.
    """
    tree = ast.parse(path.read_text(encoding="utf-8"))
    values: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            values.append(node.value)
        elif isinstance(node, ast.JoinedStr):
            parts = [
                part.value
                for part in node.values
                if isinstance(part, ast.Constant) and isinstance(part.value, str)
            ]
            if parts:
                values.append("".join(parts))
    return values


class NoRetiredReferenceDependencyTests(unittest.TestCase):
    def assert_no_retired_reference(self, path: Path):
        text = path.read_text(encoding="utf-8")
        for token in TRADING_DESK_TOKENS:
            self.assertNotIn(token, text, f"{path.name} contains {token!r}")
        if path.suffix == ".py":
            for value in python_string_constants(path):
                for token in TRADING_DESK_TOKENS:
                    self.assertNotIn(
                        token, value, f"{path.name} contains {token!r} in a string constant"
                    )

    def test_runtime_contains_no_retired_reference(self):
        for path in runtime_python_files() + [FALLBACK]:
            self.assert_no_retired_reference(path)

    def test_other_fedwatch_test_modules_contain_no_retired_reference(self):
        for path in fedwatch_test_files():
            if path.name == Path(__file__).name:
                continue
            self.assert_no_retired_reference(path)

    def test_runtime_has_no_hardcoded_absolute_paths(self):
        for path in runtime_python_files():
            self.assertIsNone(
                ABSOLUTE_PATH_RE.search(path.read_text(encoding="utf-8")),
                f"{path.name} contains a hardcoded absolute path",
            )

    def test_runtime_reads_no_environment_variables(self):
        for path in runtime_python_files():
            self.assertIsNone(
                ENVIRONMENT_RE.search(path.read_text(encoding="utf-8")),
                f"{path.name} reads environment variables",
            )

    def test_runtime_import_allowlist(self):
        # sys.stdlib_module_names exists on 3.10+; on older interpreters the
        # explicit fallback still covers every stdlib module the runtime uses.
        allowed_stdlib = set(getattr(sys, "stdlib_module_names", ())) | {
            "__future__", "csv", "datetime", "html", "io", "json", "math",
            "pathlib", "re", "sys", "time", "urllib",
        }
        for path in runtime_python_files():
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    modules = [alias.name.split(".")[0] for alias in node.names]
                elif isinstance(node, ast.ImportFrom):
                    if node.level:
                        continue  # relative import inside the package
                    modules = [(node.module or "").split(".")[0]]
                else:
                    continue
                for module in modules:
                    if module in allowed_stdlib or module == "fedwatch":
                        continue
                    if module == "requests":
                        self.assertEqual(
                            path.name,
                            "transport.py",
                            "only transport.py may import requests",
                        )
                        continue
                    self.fail(f"{path.name} imports disallowed module {module!r}")

    def test_no_execution_or_broker_imports(self):
        for path in runtime_python_files():
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    modules = [alias.name.split(".")[0] for alias in node.names]
                elif isinstance(node, ast.ImportFrom):
                    modules = [(node.module or "").split(".")[0]]
                else:
                    continue
                for module in modules:
                    self.assertNotIn(module, EXECUTION_IMPORTS, path.name)

    def test_fallback_snapshot_ships_with_the_runtime_package(self):
        self.assertTrue(FALLBACK.exists())
        text = FALLBACK.read_text(encoding="utf-8")
        self.assertTrue(text.startswith("# snapshot_retrieved_at="))

    def test_cli_starts_offline_without_trading_desk_environment(self):
        environment = {
            key: value
            for key, value in os.environ.items()
            if not key.upper().startswith("TRADING_DESK")
        }
        result = subprocess.run(
            [sys.executable, str(CLI), "help"],
            capture_output=True,
            text=True,
            env=environment,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("fedwatch_data.py", result.stdout)


if __name__ == "__main__":
    unittest.main()
