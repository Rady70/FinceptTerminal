#!/usr/bin/env python3
"""MarketLab Terminal — hosted-path source audit (FINCEPT_FORK_PLAN.md §5.3).

Static audit control with two independent checks, both driven by
hosted_path_inventory.json:

1. Host references. Every Fincept-owned host reference in the source tree must
   have an explicit disposition in the manifest's "rules" section
   (removed | guarded | unreachable | not_built | rejected | disabled). A new
   reference that appears without a disposition fails the audit.

2. Network sinks. Every occurrence under src/ of an API that can initiate
   outbound contact must have an explicit disposition in the manifest's "sinks"
   section AND a pinned occurrence count in its "sink_counts" section. This
   check exists because check 1 structurally cannot see the most dangerous
   case: a *configuration-derived* route. A connector whose host arrives from
   stored configuration never appears as a literal string in the source, so no
   literal-host grep can ever catch it — but the sink it reaches the network
   through is always in the source. Inventorying the sinks is what makes a
   newly added unguarded connectToHost(), a new direct QNetworkAccessManager, a
   new QWebSocket or a new external-browser launch a build failure instead of a
   silent regression.

   The disposition alone is per-FILE, so on its own it rubber-stamps every
   future site in a file (or, for a "dir/**" selector, every future file in a
   subtree) that is already listed. That is the hole a careless change — or an
   attacker — walks straight through: add one more connectToHost() to a file
   already dispositioned "guarded" and a file-keyed check sees nothing new.
   "sink_counts" closes it by pinning the exact occurrence count of every
   (file, sink) pair: a new site changes the count and fails, a new file under
   an already-listed subtree is absent from the map and fails, and a site that
   disappears leaves a stale entry that also fails, so the inventory cannot
   quietly drift away from the tree it describes.

   Known limit, stated rather than implied: a count is not a fingerprint. A
   change that adds one sink and removes another of the same kind from the same
   file leaves the count equal and passes. Catching that needs site-level
   pinning, which is a different (and much larger) control.

Both checks are deliberately narrow. This is an audit control for one specific
containment boundary, not a general networking framework (§5.3), so the sink
list is a fixed, short enumeration of outbound-capable APIs and nothing else.

This is an audit control, not runtime verification — runtime network
observation is a separate required evidence type.

Usage:
    python audit_hosted_paths.py [--manifest PATH] [--root PATH]
Exit code 0 = every match has an explicit disposition and every sink site
              matches its pinned count.
Exit code 1 = new/undispositioned matches, a sink count that no longer matches
              the tree (or malformed manifest).
"""

import argparse
import ast
import collections
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

# Outbound-capable sinks. One entry per kind, in the order they are reported.
# Case-sensitive on purpose: these are Qt/Win32 type and function names, and a
# lowercase "qwebsocket" in prose is not a sink.
#
#   tcp_socket             raw socket use — QTcpSocket anywhere, and every
#                          connectToHost() call whatever the socket type
#   network_access_manager any direct QNetworkAccessManager, i.e. every HTTP
#                          path that does NOT go through the shared HttpClient
#                          and therefore cannot be seen by its deny-list
#   websocket              QWebSocket (the ws/wss transport)
#   open_url               handing a URL to the OS: QDesktopServices::openUrl,
#                          Win32 ShellExecute*, and the two argv forms a
#                          QProcess would use to launch the default browser
#                          (xdg-open, rundll32 url.dll)
SINK_PATTERNS = [
    ("tcp_socket", re.compile(r"\bQTcpSocket\b|\bconnectToHost\s*\(")),
    # GuardedNetworkAccessManager is counted as a sink in its own right. It IS a
    # QNetworkAccessManager, and if swapping a raw manager for the guarded one
    # silently dropped the file out of this inventory, the control would lose
    # sight of a file precisely when it began making requests through a wrapper.
    ("network_access_manager", re.compile(r"\bQNetworkAccessManager\b|\bGuardedNetworkAccessManager\b")),
    ("websocket", re.compile(r"\bQWebSocket\b")),
    # ExternalUrlGuard::open_external is the guarded external-browser handoff and
    # is counted for the same reason: a screen that routes through it can still
    # launch a browser, so it must stay inventoried.
    ("open_url", re.compile(r"QDesktopServices::openUrl\s*\("
                            r"|ExternalUrlGuard::open_external\s*\("
                            r"|\bShellExecute(?:A|W|Ex|ExA|ExW)?\s*\("
                            r"|\bxdg-open\b"
                            r"|rundll32[^\n]*url\.dll")),
]
SINK_KINDS = [name for name, _pat in SINK_PATTERNS]
# Extended below with PY_SINK_KINDS once those are defined; the manifest
# validates every disposition and every pinned count against this one list.

SCAN_EXTS = {
    ".cpp", ".h", ".hpp", ".cc", ".cxx", ".qml", ".js", ".html", ".htm",
    ".py", ".json", ".ini", ".txt", ".cmake", ".rc", ".ts",
    ".yml", ".yaml", ".md", ".in", ".ps1", ".bat", ".cmd", ".mjs", ".css",
    ".xml", ".desktop", ".sh",
}

# The C++ sink check scans the application sources only: translation units and
# headers under src/. A sink in tests/ is not in the product.
#
# A previous revision of this comment also excluded scripts/, on the ground that
# it "is not in the product". That was wrong, and it is the reason the inventory
# was blind to half of this application's outbound surface: MarketLab launches
# production Python as child processes. AgentService starts
# scripts/agents/finagent_core/main.py, MarketDataService runs
# scripts/yfinance_data.py, and two hundred more .py paths are named as argv from
# src/. Those children do their own networking, through their own clients, and
# FINCEPT_FORK_PLAN.md §5.3 names Python networking explicitly as one of the
# bypass mechanisms that has to be accounted for. See PY_SINK_* below.
SINK_EXTS = {".cpp", ".h", ".hpp", ".cc", ".cxx"}
SINK_ROOT = "src"

# ── Production-Python sinks ───────────────────────────────────────────────────
#
# Scope is "what the product can actually run", not "every .py in the tree".
# scripts/ carries ~1,300 modules and nothing launches most of them;
# dispositioning all of them would be the general networking framework §5.3 says
# not to build. The reachable set is the transitive local-import closure of the
# entry points named as string literals in src/ — 338 files, 72 with a sink.
#
# Honest limit, stated the way the header/impl pairing limit below is: the
# closure follows static `import` / `from ... import` only. A module reached
# solely through importlib, a plugin registry or an exec() string is not in it.
# The literal-host scan still covers every file in scripts/ regardless, so a
# Fincept host *written into* such a module is still caught; what this would miss
# is a Fincept host reached from one purely through configuration.
PY_SINK_PATTERNS = [
    ("py_http_client", re.compile(
        r"^[ \t]*(?:import|from)[ \t]+(?:httpx|requests|aiohttp)\b", re.MULTILINE)),
    # urllib.request / urlopen only. urllib.parse is string manipulation, not a
    # transport, and counting it would pad the inventory with non-sinks.
    ("py_urllib", re.compile(
        r"^[ \t]*(?:import|from)[ \t]+urllib.request\b|\burlopen\s*\(", re.MULTILINE)),
    ("py_socket", re.compile(
        r"^[ \t]*(?:import|from)[ \t]+socket\b", re.MULTILINE)),
    ("py_websocket", re.compile(
        r"^[ \t]*(?:import|from)[ \t]+websockets?\b", re.MULTILINE)),
]
PY_SINK_KINDS = [name for name, _pat in PY_SINK_PATTERNS]
SINK_KINDS = SINK_KINDS + PY_SINK_KINDS
PY_SINK_ROOT = "scripts"
PY_ENTRY_ROOT = "src"
PY_ENTRY_RE = re.compile(r'"([A-Za-z0-9_/.\-]+\.py)"')

SKIP_DIRS = {
    "build", ".git", "third_party", "venv", "__pycache__", ".venv", "node_modules",
}

# "guarded" is a load-bearing claim: the failure text tells the author "if it is
# guarded, say by what". Using it for a sink that simply has no Fincept-owned
# destination made the word mean two different things, and a reader of the
# evidence could not tell which. "no_hosted_route" carries that second meaning
# explicitly — the path is reachable and unguarded, but its destination is fixed
# by the code (a local file, a pinned third-party API) rather than derived from
# anything a Fincept configuration could steer.
DISPOSITIONS = {"removed", "guarded", "unreachable", "not_built", "rejected", "disabled",
                "no_hosted_route"}

# The manifest and this script are versioned together in one repo, so there is
# no external consumer to keep compatible. Schema 2 added the required "sinks"
# section; schema 3 adds the required "sink_counts" section, without which the
# per-file dispositions would silently accept newly added sites in files that
# are already listed. The "rules" section is parsed exactly as schema 1 parsed
# it.
MANIFEST_SCHEMA = 3


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if data.get("schema") != MANIFEST_SCHEMA:
        raise SystemExit(
            f"manifest {path}: unsupported schema {data.get('schema')} "
            f"(expected {MANIFEST_SCHEMA}). Schema {MANIFEST_SCHEMA} requires the "
            f"\"sinks\" and \"sink_counts\" sections alongside the unchanged \"rules\" "
            f"section; a manifest that predates them cannot express a sink disposition "
            f"or pin an occurrence count, and accepting it would silently disable the "
            f"half of this audit that sees configuration-derived routes.")

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

    sinks = {}
    for rule in data.get("sinks", []):
        sink = rule.get("sink", "")
        disposition = rule.get("disposition", "")
        note = rule.get("note", "")
        if sink not in SINK_KINDS or disposition not in DISPOSITIONS:
            raise SystemExit(f"manifest {path}: bad sink rule {rule!r}")
        if not note.strip():
            raise SystemExit(f"manifest {path}: sink rule for {sink!r} has no note")
        if sink not in sinks:
            sinks[sink] = []
        for file_sel in rule.get("files", []):
            # A catch-all sink disposition would dispose of every present AND
            # future sink site in one line, which is exactly the defect this
            # check exists to remove. Selectors must name a file or a subtree.
            if file_sel == "*":
                raise SystemExit(
                    f"manifest {path}: sink rule for {sink!r} uses the \"*\" catch-all. "
                    f"A catch-all disposes of every future sink site too, which defeats "
                    f"the check — name the files, or a \"dir/**\" subtree whose files "
                    f"genuinely share one disposition.")
            sinks[sink].append((file_sel, disposition, note))

    # "sink_counts": {"<path under src/>": {"<sink kind>": <occurrences>}}.
    # Required and exhaustive — every (file, sink) with at least one occurrence
    # must appear with its exact count. This is what makes the check see a new
    # site in an already-dispositioned file, which the per-file disposition
    # cannot.
    raw_counts = data.get("sink_counts")
    if not isinstance(raw_counts, dict) or not raw_counts:
        raise SystemExit(
            f"manifest {path}: schema {MANIFEST_SCHEMA} requires a non-empty \"sink_counts\" "
            f"object. Without it the sink check is keyed on the file alone, so a new "
            f"connectToHost() in a file that is already dispositioned \"guarded\" — or a new "
            f"file under an already-listed \"dir/**\" subtree — passes unseen.")
    sink_counts = {}
    for rel, per_sink in raw_counts.items():
        if not isinstance(per_sink, dict) or not per_sink:
            raise SystemExit(f"manifest {path}: bad sink_counts entry for {rel!r}")
        for sink, n in per_sink.items():
            if sink not in SINK_KINDS or not isinstance(n, int) or isinstance(n, bool) or n < 1:
                raise SystemExit(
                    f"manifest {path}: bad sink_counts entry {rel!r} -> {sink!r}: {n!r} "
                    f"(sink must be one of {SINK_KINDS}; count must be a positive integer)")
        sink_counts[rel] = dict(per_sink)
    return rules, sinks, sink_counts


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


def python_entry_points(root):
    """The .py paths named as string literals in C++ under src/.

    These are the argv the application hands to its bundled interpreter, so they
    are exactly the Python processes it is able to start.
    """
    entries = set()
    for dirpath, dirnames, filenames in os.walk(os.path.join(root, PY_ENTRY_ROOT)):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fname in filenames:
            if os.path.splitext(fname)[1].lower() not in SINK_EXTS:
                continue
            try:
                with open(os.path.join(dirpath, fname), "r",
                          encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for hit in PY_ENTRY_RE.findall(text):
                cand = hit.lstrip("/")
                if os.path.isfile(os.path.join(root, PY_SINK_ROOT, cand)):
                    entries.add(cand)
    return entries


def python_reachable(root, entries, unparsed):
    """Transitive local-import closure of `entries`, as paths under scripts/."""
    scripts_root = os.path.join(root, PY_SINK_ROOT)
    by_rel = {}
    by_base = {}
    for dirpath, dirnames, filenames in os.walk(scripts_root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fname in filenames:
            if not fname.endswith(".py"):
                continue
            full = os.path.join(dirpath, fname)
            rel = os.path.relpath(full, scripts_root).replace(os.sep, "/")
            by_rel[rel] = full
            by_base.setdefault(os.path.splitext(fname)[0], []).append(rel)

    def resolve(mod, cur_rel):
        out = []
        as_path = mod.replace(".", "/")
        for cand in (as_path + ".py", as_path + "/__init__.py"):
            if cand in by_rel:
                out.append(cand)
        parent = cur_rel.rsplit("/", 1)[0] if "/" in cur_rel else ""
        sibling = (parent + "/" if parent else "") + mod.split(".")[-1] + ".py"
        if sibling in by_rel:
            out.append(sibling)
        if not out:
            # Unambiguous basename only. Two modules sharing a name would make
            # this a guess, and a guess does not belong in an evidence control.
            base = mod.split(".")[-1]
            if len(by_base.get(base, [])) == 1:
                out = list(by_base[base])
        return out

    seen = set()
    queue = collections.deque(sorted(entries))
    while queue:
        rel = queue.popleft()
        if rel in seen:
            continue
        seen.add(rel)
        full = by_rel.get(rel)
        if not full:
            continue
        try:
            with open(full, "r", encoding="utf-8", errors="replace") as f:
                tree = ast.parse(f.read())
        except (OSError, SyntaxError, ValueError):
            # Reported, not skipped silently: a module the audit cannot parse is
            # a module whose imports it cannot follow.
            unparsed.add(rel)
            continue
        mods = []
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                mods.extend(a.name for a in node.names)
            elif isinstance(node, ast.ImportFrom):
                if node.level:
                    base_parts = rel.split("/")[:-node.level]
                    tail = (node.module or "").replace(".", "/")
                    joined = "/".join([p for p in ["/".join(base_parts), tail] if p])
                    if joined:
                        mods.append(joined.replace("/", "."))
                elif node.module:
                    mods.append(node.module)
        for mod in mods:
            for cand in resolve(mod, rel):
                if cand not in seen:
                    queue.append(cand)
    return seen


def match_py_sinks(text):
    """Occurrence count per Python sink kind for one file ({} when none)."""
    counts = {}
    for name, pat in PY_SINK_PATTERNS:
        n = len(pat.findall(text))
        if n:
            counts[name] = n
    return counts


def match_sinks(text):
    """Occurrence count per sink kind for one file ({} when the file has none)."""
    counts = {}
    for name, pat in SINK_PATTERNS:
        n = len(pat.findall(text))
        if n:
            counts[name] = n
    return counts


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

    rules, sinks, sink_counts = load_manifest(manifest_path)

    # The production-Python surface, resolved before the walk so the scan can ask
    # "is this module something the application can actually run?" per file.
    py_unparsed = set()
    py_entries = python_entry_points(root)
    py_reachable = python_reachable(root, py_entries, py_unparsed)

    problems = []
    sink_problems = []
    paired_problems = []
    header_sinks = {}
    impl_files = set()
    count_problems = []
    seen_counts = set()
    sink_totals = {name: 0 for name in SINK_KINDS}
    sink_files = set()
    scanned = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fname in filenames:
            ext = os.path.splitext(fname)[1].lower()
            full = os.path.join(dirpath, fname)
            rel = os.path.relpath(full, root).replace("\\", "/")
            if rel.startswith("marketlab/"):
                continue  # the audit's own files (manifest documents itself)
            in_sink_scope = ext in SINK_EXTS and (rel == SINK_ROOT or rel.startswith(SINK_ROOT + "/"))
            in_py_sink_scope = (
                ext == ".py"
                and rel.startswith(PY_SINK_ROOT + "/")
                and rel[len(PY_SINK_ROOT) + 1:] in py_reachable)
            if ext not in SCAN_EXTS and not in_sink_scope and not in_py_sink_scope:
                continue
            try:
                with open(full, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue

            if ext in SCAN_EXTS:
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

            if in_sink_scope:
                found = match_sinks(text)
                if ext in (".h", ".hpp"):
                    header_sinks[rel] = set(found)
                elif ext in (".cpp", ".cc", ".cxx"):
                    impl_files.add(rel)
                if found:
                    sink_files.add(rel)
                expected_here = sink_counts.get(rel, {})
                for sink, count in found.items():
                    sink_totals[sink] += count
                    disposed = any(selector_matches(file_sel, rel)
                                   for file_sel, _disp, _note in sinks.get(sink, []))
                    if not disposed:
                        sink_problems.append((rel, sink, count))
                    # The count is checked even when the disposition is missing:
                    # the two failures answer different questions ("is this site
                    # judged?" and "is this the same set of sites that was
                    # judged?") and a reader needs both.
                    expected = expected_here.get(sink)
                    seen_counts.add((rel, sink))
                    if expected is None:
                        count_problems.append((rel, sink, count, None))
                    elif expected != count:
                        count_problems.append((rel, sink, count, expected))

            if in_py_sink_scope:
                # Same two questions as the C++ branch, same two failures: is
                # this site judged, and is it still the same set of sites that
                # was judged?
                found = match_py_sinks(text)
                if found:
                    sink_files.add(rel)
                expected_here = sink_counts.get(rel, {})
                for sink, count in found.items():
                    sink_totals[sink] += count
                    disposed = any(selector_matches(file_sel, rel)
                                   for file_sel, _disp, _note in sinks.get(sink, []))
                    if not disposed:
                        sink_problems.append((rel, sink, count))
                    expected = expected_here.get(sink)
                    seen_counts.add((rel, sink))
                    if expected is None:
                        count_problems.append((rel, sink, count, None))
                    elif expected != count:
                        count_problems.append((rel, sink, count, expected))

    # A manager declared as a member in the .h and *used* only in the .cpp leaves
    # no sink token in the .cpp at all: src/services/updater/UpdateService.cpp
    # issues three HTTP GETs through net_.get(req) while the string
    # "QNetworkAccessManager" appears only in UpdateService.h. Scanning for the
    # type name alone therefore never asks that translation unit for a
    # disposition. Require the sibling implementation file to be judged too.
    #
    # Honest limit: this forces the .cpp to be *dispositioned*, it does not pin a
    # count there (the regex count is genuinely zero), so a newly added
    # net_.get() in an already-judged .cpp still changes nothing measurable. What
    # it does buy is that no such file is invisible to the reviewer.
    for hrel, hsinks in header_sinks.items():
        stem = hrel.rsplit(".", 1)[0]
        for cand in (stem + ".cpp", stem + ".cc", stem + ".cxx"):
            if cand not in impl_files:
                continue
            for sink in sorted(hsinks):
                if (cand, sink) in seen_counts:
                    continue  # the .cpp names the type itself; already judged above
                disposed = any(selector_matches(file_sel, cand)
                               for file_sel, _disp, _note in sinks.get(sink, []))
                if not disposed:
                    paired_problems.append((cand, sink, hrel))

    # Entries pinned in the manifest that no longer exist in the tree. A sink
    # that is genuinely gone is good news, but the inventory has to say so:
    # left stale, the map slowly stops describing the tree it is evidence about.
    for rel, per_sink in sink_counts.items():
        for sink, expected in per_sink.items():
            if (rel, sink) not in seen_counts:
                count_problems.append((rel, sink, 0, expected))

    failed = False

    if py_unparsed:
        # Not a failure on its own — an unparsable module is usually a py2 file
        # nothing runs — but it must be visible, because its imports were not
        # followed and anything reachable only through it is outside the closure.
        print(f"NOTE — {len(py_unparsed)} reachable Python module(s) could not be parsed, "
              f"so their imports were not followed:")
        for rel in sorted(py_unparsed):
            print(f"  {PY_SINK_ROOT}/{rel}")
        print("")

    if problems:
        failed = True
        print("AUDIT FAILED — Fincept-owned host references without an explicit disposition:")
        for rel, token in sorted(set(problems)):
            print(f"  {rel}: {token}")
        print(f"\n{len(set(problems))} undispositioned reference(s). "
              f"Add each (file, pattern) to marketlab/hosted_path_inventory.json with a "
              f"disposition, or remove the reference.")

    if sink_problems:
        if failed:
            print("")
        failed = True
        print("AUDIT FAILED — network sinks without an explicit disposition:")
        for rel, sink, count in sorted(set(sink_problems)):
            print(f"  {rel}: {sink} ({count} occurrence(s))")
        print(f"\n{len(set(sink_problems))} undispositioned sink site(s). A sink is a path that "
              f"can initiate outbound contact, so every occurrence needs a truthful disposition — "
              f"add each (file, sink) to the \"sinks\" section of "
              f"marketlab/hosted_path_inventory.json, or remove the sink. If it is guarded, say "
              f"by what; if the surface is not reachable in this fork, say why.")

    if paired_problems:
        if failed:
            print("")
        failed = True
        print("AUDIT FAILED — implementation files whose sink is declared in their header:")
        for rel, sink, hrel in sorted(set(paired_problems)):
            print(f"  {rel}: {sink} (declared in {hrel})")
        print(f"" + chr(10) + f"{len(set(paired_problems))} unjudged implementation file(s). The member is "
              f"declared in the header and used here, so this translation unit can initiate "
              f"outbound contact without ever naming the type — add each (file, sink) to the "
              f"\"sinks\" section of marketlab/hosted_path_inventory.json.")

    if count_problems:
        if failed:
            print("")
        failed = True
        print("AUDIT FAILED — network-sink occurrence counts do not match hosted_path_inventory.json:")
        # Sort on (rel, sink) only: `expected` is None for an unpinned site and an
        # int otherwise, and those two do not compare.
        for rel, sink, found_n, expected in sorted(set(count_problems), key=lambda e: (e[0], e[1])):
            if expected is None:
                print(f"  {rel}: {sink} — {found_n} occurrence(s), not pinned in \"sink_counts\"")
            elif found_n == 0:
                print(f"  {rel}: {sink} — pinned at {expected}, no occurrence found (stale entry)")
            else:
                print(f"  {rel}: {sink} — {found_n} occurrence(s), pinned at {expected}")
        print(f"\n{len(set(count_problems))} sink count mismatch(es). The per-file disposition cannot "
              f"see a NEW site in a file it already covers, so every (file, sink) pair is pinned to an "
              f"exact count. A count that went up is a new outbound-capable site: judge it, say what "
              f"guards it or why it is unreachable, and only then update the number. A count that went "
              f"down (or a file that vanished) means the inventory is describing a tree that no longer "
              f"exists — update it so the evidence stays true.")

    if failed:
        return 1

    cpp_summary = ", ".join(f"{sink_totals[name]} {name}"
                            for name in SINK_KINDS if name not in PY_SINK_KINDS)
    py_summary = ", ".join(f"{sink_totals[name]} {name}" for name in PY_SINK_KINDS)
    cpp_files = len([r for r in sink_files if not r.startswith(PY_SINK_ROOT + "/")])
    py_files = len([r for r in sink_files if r.startswith(PY_SINK_ROOT + "/")])
    print(f"AUDIT OK — {scanned} files scanned; every Fincept-owned host reference "
          f"has an explicit disposition in hosted_path_inventory.json. "
          f"C++ network sinks: {cpp_summary} across {cpp_files} {SINK_ROOT}/ file(s). "
          f"Production-Python network sinks: {py_summary} across {py_files} "
          f"{PY_SINK_ROOT}/ file(s), from a reachable set of {len(py_reachable)} module(s) "
          f"behind {len(py_entries)} entry point(s) named in {PY_ENTRY_ROOT}/ — "
          f"every site has an explicit disposition and matches its pinned occurrence count.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
