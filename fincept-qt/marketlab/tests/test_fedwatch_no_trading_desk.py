"""Guard tests: no retired reference runtime dependency and no execution surface.

Batch A must be self-contained inside MarketLab Terminal. These tests enforce
the boundary statically over the runtime files that ship with the application:

* no retired-project name, path or environment reference anywhere in the runtime
  package, the CLI, its runtime resource (the tracked FOMC fallback snapshot),
  or the other FedWatch test modules. Python sources are scanned by AST, so
  implicit literal concatenation and f-strings cannot hide a token;
* no hardcoded absolute Windows paths in the runtime;
* no environment-variable reads except the single Batch B exception: the
  durable history store resolves the host-provided application profile root
  (``FINCEPT_DATA_DIR``) in ``fedwatch/store.py`` and nowhere else, and no
  other variable may ever be read;
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
import tempfile
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
ENV_LITERAL_RE = re.compile(r"""os\.(?:environ\.get|getenv)\(\s*["']([^"']+)["']""")
# Batch B: the durable history store resolves its default location from the
# single host-provided application profile root. No other environment variable
# may be read anywhere in the runtime, and the access must be the literal
# ``os.environ.get("FINCEPT_DATA_DIR")`` form (no dynamic lookup).
ENV_READ_ALLOWLIST = {"store.py": {"FINCEPT_DATA_DIR"}}
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


def python_environment_accesses(path: Path) -> tuple[set[str], bool]:
    """Environment variables read by a Python file, plus a dynamic-use flag.

    Covers ``os.environ.get("X")``, ``os.getenv("X")``, ``os.environ["X"]``,
    ``import os as alias`` aliasing, and ``from os import environ/getenv``
    forms. ``dynamic`` is true when any environment access cannot be resolved
    to a literal variable name, so no allowlist can hide behind indirection.
    """
    tree = ast.parse(path.read_text(encoding="utf-8"))
    os_aliases = {"os"}
    from_aliases: set[str] = set()
    dynamic = False
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name == "os":
                    os_aliases.add(alias.asname or "os")
                if alias.name == "importlib":
                    dynamic = True
        elif isinstance(node, ast.ImportFrom):
            if node.module == "os":
                for alias in node.names:
                    if alias.name in ("environ", "getenv"):
                        from_aliases.add(alias.asname or alias.name)
            if node.module == "importlib":
                dynamic = True

    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            func = node.func
            if isinstance(func, ast.Name) and func.id in ("__import__", "vars"):
                # Indirection could reach os.environ/os.getenv without a
                # literal environment access the scanner can see.
                dynamic = True
            if isinstance(func, ast.Name) and func.id == "getattr":
                target = node.args[0] if node.args else None
                attribute = node.args[1] if len(node.args) > 1 else None
                if isinstance(target, ast.Name) and target.id in os_aliases:
                    dynamic = True
                elif not (
                    isinstance(attribute, ast.Constant) and isinstance(attribute.value, str)
                ):
                    dynamic = True
                elif attribute.value in ("environ", "getenv"):
                    dynamic = True
            if isinstance(func, ast.Attribute) and func.attr == "import_module":
                dynamic = True
            is_environ_get = (
                isinstance(func, ast.Attribute)
                and func.attr == "get"
                and (
                    (
                        isinstance(func.value, ast.Attribute)
                        and func.value.attr == "environ"
                        and isinstance(func.value.value, ast.Name)
                        and func.value.value.id in os_aliases
                    )
                    or (
                        isinstance(func.value, ast.Name)
                        and func.value.id in from_aliases
                    )
                )
            )
            is_getenv = (
                isinstance(func, ast.Attribute)
                and func.attr == "getenv"
                and isinstance(func.value, ast.Name)
                and func.value.id in os_aliases
            ) or (
                isinstance(func, ast.Name)
                and func.id == "getenv"
                and "getenv" in from_aliases
            )
            if is_environ_get or is_getenv:
                argument = node.args[0] if node.args else None
                if isinstance(argument, ast.Constant) and isinstance(argument.value, str):
                    names.add(argument.value)
                else:
                    dynamic = True
        elif isinstance(node, ast.Subscript):
            owner = node.value
            if (
                isinstance(owner, ast.Attribute)
                and owner.attr == "environ"
                and isinstance(owner.value, ast.Name)
                and owner.value.id in os_aliases
            ):
                if isinstance(node.slice, ast.Constant) and isinstance(node.slice.value, str):
                    names.add(node.slice.value)
                else:
                    dynamic = True
            elif isinstance(owner, ast.Name) and owner.id in from_aliases:
                dynamic = True
        elif isinstance(node, ast.Attribute):
            if isinstance(node.value, ast.Name) and node.value.id in from_aliases:
                dynamic = True
        elif isinstance(node, ast.Name):
            if node.id in from_aliases and isinstance(node.ctx, ast.Load):
                dynamic = True
    return names, dynamic


class EnvironmentScannerTests(unittest.TestCase):
    """The env scanner must catch aliased and from-import access forms."""

    def scan(self, source: str) -> tuple[set[str], bool]:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "probe.py"
        path.write_text(source, encoding="utf-8")
        return python_environment_accesses(path)

    def test_scanner_resolves_aliased_and_literal_accesses(self):
        names, dynamic = self.scan("import os as _o\nx = _o.environ.get('SECRET')\n")
        self.assertEqual(names, {"SECRET"})
        self.assertFalse(dynamic)
        names, dynamic = self.scan("import os\nx = os.environ['SECRET']\n")
        self.assertEqual(names, {"SECRET"})
        self.assertFalse(dynamic)

    def test_scanner_flags_from_import_and_dynamic_accesses(self):
        names, _dynamic = self.scan("from os import environ\nx = environ.get('SECRET')\n")
        self.assertEqual(names, {"SECRET"})
        names, _dynamic = self.scan("from os import getenv\nx = getenv('SECRET')\n")
        self.assertEqual(names, {"SECRET"})
        names, dynamic = self.scan("import os\nkey = 'SECRET'\nx = os.environ.get(key)\n")
        self.assertEqual(names, set())
        self.assertTrue(dynamic)

    def test_scanner_flags_indirection_forms(self):
        for source in (
            "import os\nx = getattr(os, 'environ').get('SECRET')\n",
            "x = __import__('os').environ.get('SECRET')\n",
            "import importlib\nx = importlib.import_module('os').environ.get('SECRET')\n",
            "import os\nx = vars(os)['environ'].get('SECRET')\n",
        ):
            names, dynamic = self.scan(source)
            self.assertTrue(
                dynamic or names,
                f"the scanner missed an indirection form: {source!r}",
            )

    def test_store_is_the_only_allowlisted_runtime_access(self):
        names, dynamic = python_environment_accesses(PACKAGE / "store.py")
        self.assertEqual(names, {"FINCEPT_DATA_DIR"})
        self.assertFalse(dynamic)


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

    def test_runtime_environment_variable_allowlist(self):
        for path in runtime_python_files():
            names, dynamic = python_environment_accesses(path)
            allowed = ENV_READ_ALLOWLIST.get(path.name)
            if allowed is None:
                self.assertEqual(
                    names, set(), f"{path.name} reads environment variables {sorted(names)}"
                )
                self.assertFalse(dynamic, f"{path.name} has a dynamic environment access")
                self.assertIsNone(
                    ENVIRONMENT_RE.search(path.read_text(encoding="utf-8")),
                    f"{path.name} uses an environment access form outside the AST scanner",
                )
                continue
            self.assertTrue(names, f"{path.name} is allowlisted but reads no variable")
            self.assertLessEqual(
                names,
                allowed,
                f"{path.name} reads an environment variable outside the allowlist: {names - allowed}",
            )
            self.assertFalse(dynamic, f"{path.name} has a dynamic environment access")
            remainder = ENV_LITERAL_RE.sub("", path.read_text(encoding="utf-8"))
            self.assertIsNone(
                ENVIRONMENT_RE.search(remainder),
                f"{path.name} uses an environment access form outside the literal "
                "os.environ.get(\"FINCEPT_DATA_DIR\") pattern",
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
