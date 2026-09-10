"""CLI contract tests for scripts/akshare_data.py (Phase 4 corrective pass).

The script's no-argument path is its usage/catalog contract: print the endpoint
catalog and exit non-zero, exactly like the sibling akshare_index.py. The
endpoint-map dispatch must still answer the `get_all_endpoints` catalog query
with success. Both paths are exercised as real subprocesses with no network and
no data stack required (the script guards pandas/akshare imports so the
usage/catalog path works before they are installed).

Run:
    python -m unittest discover -s marketlab/tests -p "test_*.py" -v
Also registered through the same CTest discovery as the CFTC fixtures.
"""

import json
import os
import subprocess
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPTS = os.path.abspath(os.path.join(_HERE, "..", "..", "scripts"))
_SCRIPT = os.path.join(_SCRIPTS, "akshare_data.py")


class AkshareCliTest(unittest.TestCase):
    def _run(self, *args):
        return subprocess.run(
            [sys.executable, _SCRIPT, *args],
            capture_output=True,
            text=True,
            cwd=_SCRIPTS,
            timeout=120,
        )

    def test_no_argument_prints_usage_catalog_and_exits_nonzero(self):
        proc = self._run()
        self.assertEqual(proc.returncode, 1, proc.stderr)
        payload = json.loads(proc.stdout)
        self.assertIn("error", payload)
        self.assertIn("available_endpoints", payload)
        self.assertIn("stock_zh_spot", payload["available_endpoints"])

    def test_catalog_endpoint_returns_success(self):
        proc = self._run("get_all_endpoints")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        payload = json.loads(proc.stdout)
        self.assertTrue(payload.get("success"), payload)
        endpoints = payload["data"]["available_endpoints"]
        self.assertIn("stock_zh_spot", endpoints)
        self.assertGreaterEqual(len(endpoints), 10)


if __name__ == "__main__":
    unittest.main(verbosity=2)
