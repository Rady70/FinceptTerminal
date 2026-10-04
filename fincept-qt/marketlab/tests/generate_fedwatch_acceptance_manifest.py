"""Generate the baseline acceptance manifest using an unchanged git archive.

Run from fincept-qt: python marketlab/tests/generate_fedwatch_acceptance_manifest.py
Only this explicit generation step needs git. The permanent unit test does not.
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

from fedwatch_acceptance_cases import FIXTURES, cases, fixture_digest

BASE = "65cc9b356"
HERE = Path(__file__).resolve().parent


def main():
    root = HERE.parents[2]
    revision = subprocess.check_output(["git", "rev-parse", BASE], cwd=root, text=True).strip()
    archive = subprocess.check_output(["git", "archive", BASE, "fincept-qt/scripts/fedwatch"], cwd=root)
    with tempfile.TemporaryDirectory(prefix="fedwatch-original-") as tmp:
        with tarfile.open(fileobj=io.BytesIO(archive)) as package:
            # Archive comes solely from the selected local Git tree. Reject
            # symlinks/path traversal before extracting its unchanged files.
            for member in package.getmembers():
                if member.issym() or member.islnk() or ".." in Path(member.name).parts or Path(member.name).is_absolute():
                    raise ValueError("unexpected archive member")
            package.extractall(tmp, filter="data")
        env = dict(os.environ, PYTHONPATH=str(Path(tmp) / "fincept-qt" / "scripts"), PYTHONDONTWRITEBYTECODE="1")
        completed = subprocess.run([sys.executable, str(HERE / "fedwatch_acceptance_runner.py")],
                                   cwd=tmp, env=env, text=True, capture_output=True, timeout=120)
        if completed.returncode:
            raise RuntimeError(completed.stderr)
        observed = json.loads(completed.stdout)
        expected = Path(tmp) / "fincept-qt" / "scripts" / "fedwatch" / "__init__.py"
        if Path(observed["package_path"]) != expected:
            raise AssertionError("baseline runner did not import the archived package")
    inputs = cases()
    manifest = {"schema_version": 2, "fixture_hash_convention": "UTF8_BYTES_CRLF_AND_CR_TO_LF", "original_commit": revision,
        "archive_sha256": hashlib.sha256(archive).hexdigest(),
        "fixture_sha256": {path.name: fixture_digest(path.read_bytes())
                           for path in sorted(FIXTURES.iterdir()) if path.suffix in (".html", ".csv", ".json")
                           and not path.name.startswith("acceptance_manifest_")},
        "cases": {name: {"input_sha256": hashlib.sha256(json.dumps(inputs[name], sort_keys=True).encode()).hexdigest(),
                         **result} for name, result in observed["results"].items()}}
    output = FIXTURES / "acceptance_manifest_65cc9b356.json"
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote {output}: {len(inputs)} cases; {sum(len(c['units']) for c in manifest['cases'].values())} accepted units")


if __name__ == "__main__":
    main()
