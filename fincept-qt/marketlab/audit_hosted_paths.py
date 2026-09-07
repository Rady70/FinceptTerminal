#!/usr/bin/env python3
"""MarketLab Terminal — hosted-path source audit (FINCEPT_FORK_PLAN.md §5.3).

Static audit control: every Fincept-owned host reference in the source tree
must have an explicit disposition recorded in hosted_path_inventory.json
(removed | guarded | unreachable | not_built | rejected). A new reference
that appears without a disposition fails the audit.

This is an audit control, not runtime verification — runtime network
observation is a separate required evidence type.

Usage:
    python audit_hosted_paths.py [--manifest PATH] [--root PATH] [--fail-on-new-only]
Exit code 0 = every match has an explicit disposition.
Exit code 1 = new/undispositioned matches (or malformed manifest).
"""

import argparse
import json
import os
import re
import sys

# Host patterns that identify a Fincept-owned destination. Kept deliberately
# host-shaped (not "fincept" alone) so internal identifiers, namespaces, and
# class names do not trip the audit.
HOST_PATTERNS = [
    re.compile(r"fincept\.in\b", re.IGNORECASE),
    re.compile(r"fincept\.com\b", re.IGNORECASE),
    re.compile(r"fincept\.app\b", re.IGNORECASE),
    re.compile(r"fincept\.ai\b", re.IGNORECASE),
    re.compile(r"markets\.fincept", re.IGNORECASE),
    re.compile(r"Fincept-Corporation", re.IGNORECASE),
]

SCAN_EXTS = {
    ".cpp", ".h", ".hpp", ".cc", ".cxx", ".qml", ".js", ".html", ".htm",
    ".py", ".json", ".ini", ".txt", ".cmake", ".rc", ".ts",
    ".yml", ".yaml", ".md", ".in", ".ps1", ".bat", ".cmd", ".mjs", ".css",
    ".xml", ".desktop", ".sh",
}

SKIP_DIRS = {
    "build", ".git", "third_party", "venv", "__pycache__", ".venv", "node_modules",
}

DISPOSITIONS = {"removed", "guarded", "unreachable", "not_built", "rejected", "disabled"}


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if data.get("schema") != 1:
        raise SystemExit(f"manifest {path}: unsupported schema {data.get('schema')}")
    rules = {}
    for rule in data.get("rules", []):
        pattern = rule.get("pattern", "")
        disposition = rule.get("disposition", "")
        note = rule.get("note", "")
        if not pattern or disposition not in DISPOSITIONS:
            raise SystemExit(f"manifest {path}: bad rule {rule!r}")
        if pattern not in rules:
            rules[pattern] = []
        # Each entry is (file_selector, disposition, note). Selectors are an
        # exact relative path, a "dir/**" subtree, or "*" (any file).
        for file_sel in rule.get("files", []):
            rules[pattern].append((file_sel, disposition, note))
        if rule.get("any") is not None:
            rules[pattern].append(("*", rule["any"], note))
    return rules


def selector_matches(selector, rel):
    if selector == "*":
        return True
    if selector.endswith("/**"):
        prefix = selector[:-3]
        return rel == prefix or rel.startswith(prefix + "/")
    return rel == selector


def match_patterns(text):
    hits = []
    for pat in HOST_PATTERNS:
        for m in pat.finditer(text):
            hits.append((m.group(0), m.start()))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--root", default=None)
    args = ap.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = args.root or os.path.dirname(script_dir)  # fincept-qt/
    manifest_path = args.manifest or os.path.join(script_dir, "hosted_path_inventory.json")
    if not os.path.exists(manifest_path):
        print(f"FATAL: manifest not found: {manifest_path}")
        return 1

    rules = load_manifest(manifest_path)

    problems = []
    scanned = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fname in filenames:
            ext = os.path.splitext(fname)[1].lower()
            if ext not in SCAN_EXTS:
                continue
            full = os.path.join(dirpath, fname)
            rel = os.path.relpath(full, root).replace("\\", "/")
            if rel.startswith("marketlab/"):
                continue  # the audit's own files (manifest documents itself)
            try:
                with open(full, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            scanned += 1
            hits = match_patterns(text)
            for token, _pos in hits:
                disposed = False
                for pattern, entries in rules.items():
                    if re.search(re.escape(pattern), token, re.IGNORECASE):
                        for file_sel, disposition, note in entries:
                            if selector_matches(file_sel, rel):
                                disposed = True
                                break
                    if disposed:
                        break
                if not disposed:
                    problems.append((rel, token))

    if problems:
        print("AUDIT FAILED — Fincept-owned host references without an explicit disposition:")
        for rel, token in sorted(set(problems)):
            print(f"  {rel}: {token}")
        print(f"\n{len(set(problems))} undispositioned reference(s). "
              f"Add each (file, pattern) to marketlab/hosted_path_inventory.json with a "
              f"disposition, or remove the reference.")
        return 1

    print(f"AUDIT OK — {scanned} files scanned; every Fincept-owned host reference "
          f"has an explicit disposition in hosted_path_inventory.json.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
