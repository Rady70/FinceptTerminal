"""Resolve the exact source/base for push or explicitly selected PR lint."""

import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path


def commit_sha(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("Expected an exact 40-character commit SHA")
    if value == "0" * 40:
        raise ValueError("An all-zero SHA is not a lint base")
    return value


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def ensure_commit(sha, pull_request=None):
    try:
        git("cat-file", "-e", sha + "^{commit}")
    except subprocess.CalledProcessError:
        ref = f"refs/pull/{pull_request}/head" if pull_request else sha
        git("fetch", "--no-tags", "origin", ref)
        if pull_request and git("rev-parse", "FETCH_HEAD") != sha:
            raise ValueError("PR head changed during resolution; retry explicitly")
        git("cat-file", "-e", sha + "^{commit}")


def fetch_pull_request(repository, number):
    api = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "MarketLab-lint"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(
        f"{api}/repos/{repository}/pulls/{number}", headers=headers
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        raise ValueError(f"Cannot resolve PR #{number}: HTTP {error.code}") from error


def resolve(event_name, event, repository, workflow_sha, load_pr=fetch_pull_request):
    if event_name == "push":
        head = commit_sha(workflow_sha)
        before = event.get("before")
        base = commit_sha(before) if before and before != "0" * 40 else commit_sha(git("rev-parse", head + "^"))
        ensure_commit(base)
        ensure_commit(head)
        return {"base_sha": base, "head_sha": head, "scope": "main push"}
    if event_name != "workflow_dispatch":
        raise ValueError("Lint scope supports only main push or explicit manual PR selection")
    number = event.get("inputs", {}).get("pull_request", "")
    if not isinstance(number, str) or not re.fullmatch(r"[1-9][0-9]*", number):
        raise ValueError("Manual Lint requires a positive pull_request number; no full-tree fallback")
    pull = load_pr(repository, number)
    if pull["base"]["repo"]["full_name"].casefold() != repository.casefold():
        raise ValueError("PR does not target this repository")
    base = commit_sha(pull["base"]["sha"])
    head = commit_sha(pull["head"]["sha"])
    ensure_commit(base)
    ensure_commit(head, pull_request=number)
    # GitHub PRs use a three-dot diff. If main advanced, exclude unrelated
    # main commits by comparing the PR head against its merge base.
    base = commit_sha(git("merge-base", base, head))
    return {"base_sha": base, "head_sha": head, "scope": f"PR #{number}"}


def main():
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    scope = resolve(
        os.environ["GITHUB_EVENT_NAME"], event,
        os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_SHA"],
    )
    with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
        for key, value in scope.items():
            output.write(f"{key}={value}\n")
    print(f"Lint source: {scope['scope']}; base={scope['base_sha']}; head={scope['head_sha']}")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with Path(summary).open("a", encoding="utf-8") as output:
            output.write(f"### Lint source: {scope['scope']}\n\nBase: `{scope['base_sha']}`\n\nHead: `{scope['head_sha']}`\n")


if __name__ == "__main__":
    try:
        main()
    except (KeyError, TypeError, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"::error::{error}", file=sys.stderr)
        sys.exit(1)
