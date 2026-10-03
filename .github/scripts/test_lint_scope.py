"""Real Git graph regressions for the manual/push lint boundary."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import resolve_lint_scope as scope


class LintScopeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.previous = Path.cwd()
        os.chdir(self.directory.name)
        self.addCleanup(os.chdir, self.previous)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "user.name", "Lint scope fixture")
        self.base = self.commit("base.cpp", "base\n")
        self.git("checkout", "-q", "-b", "feature")
        self.head = self.commit("feature.cpp", "PR code\n")
        self.git("checkout", "-q", "main")
        self.advanced_main = self.commit("unrelated.cpp", "unrelated main work\n")

    def git(self, *args):
        return subprocess.check_output(["git", *args], text=True).strip()

    def commit(self, name, text):
        Path(name).write_text(text, encoding="utf-8")
        self.git("add", name)
        self.git("commit", "-q", "-m", name)
        return self.git("rev-parse", "HEAD")

    def pull(self, base=None, head=None):
        return {"base": {"repo": {"full_name": "owner/repo"}, "sha": base or self.advanced_main},
                "head": {"sha": head or self.head}}

    def manual(self, number="39", metadata=None):
        return scope.resolve("workflow_dispatch", {"inputs": {"pull_request": number}},
                             "owner/repo", self.advanced_main,
                             load_pr=lambda repository, pr: metadata or self.pull())

    def test_pr_uses_merge_base_and_excludes_unrelated_main_changes(self):
        result = self.manual()
        self.assertEqual(result, {"base_sha": self.base, "head_sha": self.head, "scope": "PR #39"})
        self.assertEqual(self.git("diff", "--name-only", result["base_sha"], result["head_sha"]), "feature.cpp")

    def test_merged_pr_preserves_exact_reviewed_source(self):
        self.git("merge", "--no-ff", "-m", "merged PR", "feature")
        merged = self.git("rev-parse", "HEAD")
        result = self.manual(metadata=self.pull(base=self.base))
        self.assertNotEqual(result["head_sha"], merged)
        self.assertEqual(result["head_sha"], self.head)
        self.assertEqual(result["base_sha"], self.base)

    def test_push_preserves_exact_before_after_range(self):
        result = scope.resolve("push", {"before": self.base}, "owner/repo", self.advanced_main)
        self.assertEqual(result["base_sha"], self.base)
        self.assertEqual(result["head_sha"], self.advanced_main)

    def test_first_push_fallback_uses_head_parent(self):
        result = scope.resolve("push", {"before": "0" * 40}, "owner/repo", self.head)
        self.assertEqual(result["base_sha"], self.base)

    def test_manual_requires_explicit_valid_pr_and_never_falls_back(self):
        for number in ("", "0", "-1", "39; echo injected", "$(whoami)", "39\n", 39):
            with self.subTest(number=number), self.assertRaises(ValueError):
                self.manual(number)

    def test_other_repository_is_rejected(self):
        metadata = self.pull()
        metadata["base"]["repo"]["full_name"] = "other/repo"
        with self.assertRaises(ValueError):
            self.manual(metadata=metadata)

    def test_invalid_api_sha_is_rejected(self):
        for value in ("main", "0" * 40, "a" * 40 + "\n", "$(whoami)"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.manual(metadata=self.pull(head=value))

    def test_api_failure_is_not_replaced_by_full_tree(self):
        with self.assertRaises(ValueError):
            scope.resolve("workflow_dispatch", {"inputs": {"pull_request": "39"}},
                          "owner/repo", self.head,
                          load_pr=lambda *args: (_ for _ in ()).throw(ValueError("HTTP 404")))

    def test_unsupported_events_are_rejected(self):
        with self.assertRaises(ValueError):
            scope.resolve("pull_request_target", {}, "owner/repo", self.head)

    def test_head_moving_during_fetch_is_rejected(self):
        with patch.object(scope, "git", side_effect=[subprocess.CalledProcessError(1, "git"), "", self.advanced_main]):
            with self.assertRaisesRegex(ValueError, "PR head changed"):
                scope.ensure_commit(self.head, "39")

    def test_cli_outputs_exact_scope_and_summary(self):
        event = Path("event.json")
        event.write_text(json.dumps({"before": self.base}), encoding="utf-8")
        output, summary = Path("output.txt"), Path("summary.md")
        with patch.dict(os.environ, {"GITHUB_EVENT_NAME": "push", "GITHUB_EVENT_PATH": str(event),
                                   "GITHUB_REPOSITORY": "owner/repo", "GITHUB_SHA": self.advanced_main,
                                   "GITHUB_OUTPUT": str(output), "GITHUB_STEP_SUMMARY": str(summary)}):
            scope.main()
        self.assertIn("base_sha=" + self.base, output.read_text(encoding="utf-8"))
        self.assertIn("head_sha=" + self.advanced_main, output.read_text(encoding="utf-8"))
        self.assertIn(self.base, summary.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
