"""Deterministic tests for the durable FedWatch history store.

The store is the Batch B persistence boundary (FEDWATCH_INTEGRATION_PLAN.md
sections 7-8): value episodes that deduplicate identical refreshes, revisions
that adopt provider restatements, explicit provenance per source/method, and
a single compact SQLite file under the application profile root. No test uses
the network.
"""

from __future__ import annotations

import json
import os
import sqlite3
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from fedwatch_test_support import utc

from fedwatch.errors import HistoryStoreError
from fedwatch import store as fedwatch_store
from fedwatch.store import (
    HISTORY_SCHEMA_VERSION,
    FedwatchHistoryStore,
    content_digest,
)


def digest_for(value, tag="v1") -> str:
    return content_digest({"tag": tag, "value": value})


class StoreSchemaTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.db = Path(self._tmp.name) / "fedwatch" / "fedwatch_history.db"

    def test_schema_version_and_tables(self):
        store = FedwatchHistoryStore(self.db)
        store.ensure_schema()
        connection = sqlite3.connect(str(self.db))
        try:
            version = connection.execute("PRAGMA user_version").fetchone()[0]
            tables = {
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_master WHERE type = 'table'"
                )
            }
        finally:
            connection.close()
        self.assertEqual(version, HISTORY_SCHEMA_VERSION)
        self.assertEqual(
            tables,
            {
                "fedwatch_meetings",
                "fedwatch_source_mappings",
                "fedwatch_probability_observations",
                "fedwatch_backfills",
                "sqlite_sequence",
            },
        )

    def test_newer_schema_is_refused(self):
        self.db.parent.mkdir(parents=True)
        connection = sqlite3.connect(str(self.db))
        connection.execute(f"PRAGMA user_version = {HISTORY_SCHEMA_VERSION + 1}")
        connection.commit()
        connection.close()
        with self.assertRaises(HistoryStoreError) as caught:
            FedwatchHistoryStore(self.db).ensure_schema()
        self.assertEqual(caught.exception.code, "FEDWATCH_HISTORY_SCHEMA_NEWER")

    def test_default_path_fails_closed_without_environment(self):
        original = os.environ.pop("FINCEPT_DATA_DIR", None)
        try:
            with self.assertRaises(HistoryStoreError) as caught:
                fedwatch_store.default_db_path()
        finally:
            if original is not None:
                os.environ["FINCEPT_DATA_DIR"] = original
        self.assertEqual(caught.exception.code, "FEDWATCH_HISTORY_LOCATION_UNAVAILABLE")

    def test_default_path_uses_the_application_profile_root(self):
        original = os.environ.get("FINCEPT_DATA_DIR")
        os.environ["FINCEPT_DATA_DIR"] = self._tmp.name
        try:
            resolved = fedwatch_store.default_db_path()
        finally:
            if original is None:
                os.environ.pop("FINCEPT_DATA_DIR", None)
            else:
                os.environ["FINCEPT_DATA_DIR"] = original
        self.assertEqual(resolved, Path(self._tmp.name) / "fedwatch" / "fedwatch_history.db")


class ObservationEpisodeTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.store = FedwatchHistoryStore(Path(self._tmp.name) / "history.db")
        self.store.upsert_meeting("2026-10-28", default_status="UPCOMING")

    def record(self, value, observed_at, digest=None, **overrides):
        payload = {
            "meeting_date": "2026-10-28",
            "source": "investing",
            "method": "LIVE_INVESTING_DERIVED",
            "outcome_bp": 25,
            "open_ended": False,
            "probability_pct": value,
            "raw_probability_pct": None,
            "normalized_probability_pct": None,
            "observed_at": observed_at,
            "source_observed_at": None,
            "retrieved_at": observed_at,
            "quality_status": "OK",
            "freshness_status": "SOURCE_TIMESTAMP_UNAVAILABLE",
            "digest": digest if digest is not None else digest_for(value),
            "detail": {"origin": "test"},
        }
        payload.update(overrides)
        return self.store.record_observation(**payload)

    def test_identical_refresh_extends_the_episode(self):
        self.assertEqual(self.record(70.0, "2026-09-28T12:00:00Z"), "inserted")
        self.assertEqual(self.record(70.0, "2026-09-28T12:10:00Z"), "extended")
        self.assertEqual(self.record(70.0, "2026-09-28T12:20:00Z"), "extended")
        rows = self.store.observations(meeting_date="2026-10-28")
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row["observed_at"], "2026-09-28T12:00:00Z")
        self.assertEqual(row["last_observed_at"], "2026-09-28T12:20:00Z")
        self.assertEqual(row["last_retrieved_at"], "2026-09-28T12:20:00Z")
        self.assertEqual(row["retrieved_at"], "2026-09-28T12:00:00Z")
        self.assertEqual(row["observation_count"], 3)

    def test_same_instant_identical_is_a_duplicate(self):
        self.record(70.0, "2026-09-28T12:00:00Z")
        self.assertEqual(self.record(70.0, "2026-09-28T12:00:00Z"), "duplicate")
        self.assertEqual(len(self.store.observations(meeting_date="2026-10-28")), 1)

    def test_same_instant_restatement_revises_the_row(self):
        self.record(70.0, "2026-09-28T12:00:00Z")
        self.assertEqual(
            self.record(71.0, "2026-09-28T12:00:00Z", digest=digest_for(71.0)), "revised"
        )
        rows = self.store.observations(meeting_date="2026-10-28")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["probability_pct"], 71.0)

    def test_changed_observation_creates_a_distinct_episode(self):
        self.record(70.0, "2026-09-28T12:00:00Z")
        self.record(70.0, "2026-09-28T12:10:00Z")
        self.assertEqual(
            self.record(72.5, "2026-09-28T12:20:00Z", digest=digest_for(72.5)), "inserted"
        )
        rows = self.store.observations(meeting_date="2026-10-28")
        self.assertEqual([row["probability_pct"] for row in rows], [70.0, 72.5])
        self.assertEqual(rows[0]["last_observed_at"], "2026-09-28T12:10:00Z")
        self.assertEqual(rows[1]["observed_at"], "2026-09-28T12:20:00Z")

    def test_out_of_order_backfill_is_idempotent_and_revisable(self):
        self.record(70.0, "2026-09-28T12:00:00Z")
        self.assertEqual(
            self.record(60.0, "2026-09-01T00:00:00Z", digest=digest_for(60.0)), "inserted"
        )
        self.assertEqual(
            self.record(60.0, "2026-09-01T00:00:00Z", digest=digest_for(60.0)), "duplicate"
        )
        self.assertEqual(
            self.record(61.0, "2026-09-01T00:00:00Z", digest=digest_for(61.0)), "revised"
        )
        rows = self.store.observations(meeting_date="2026-10-28")
        self.assertEqual([row["observed_at"] for row in rows],
                         ["2026-09-01T00:00:00Z", "2026-09-28T12:00:00Z"])

    def test_conflicting_value_inside_an_episode_splits_it_without_overlap_beyond_one_instant(self):
        self.record(80.0, "2026-07-14T00:00:00Z")
        self.assertEqual(self.record(80.0, "2026-07-15T00:00:00Z"), "extended")
        self.assertEqual(
            self.record(20.0, "2026-07-15T00:00:00Z", digest=digest_for(20.0)), "inserted"
        )
        rows = self.store.observations(meeting_date="2026-10-28")
        self.assertEqual([row["probability_pct"] for row in rows], [80.0, 20.0])
        self.assertEqual(rows[0]["observed_at"], "2026-07-14T00:00:00Z")
        self.assertEqual(rows[0]["last_observed_at"], "2026-07-15T00:00:00Z")
        self.assertEqual(rows[1]["observed_at"], "2026-07-15T00:00:00Z")

    def test_same_value_beyond_the_continuity_window_starts_a_new_episode(self):
        self.record(70.0, "2026-09-20T00:00:00Z")
        self.assertEqual(self.record(70.0, "2026-09-25T00:00:00Z"), "inserted")
        rows = self.store.observations(meeting_date="2026-10-28")
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["last_observed_at"], "2026-09-20T00:00:00Z")
        self.assertEqual(rows[1]["observed_at"], "2026-09-25T00:00:00Z")

    def test_older_out_of_order_observation_never_truncates_the_latest_episode(self):
        self.record(70.0, "2026-09-28T12:00:00Z")
        self.assertEqual(
            self.record(60.0, "2026-09-01T00:00:00Z", digest=digest_for(60.0)), "inserted"
        )
        rows = self.store.observations(meeting_date="2026-10-28")
        self.assertEqual(
            [(row["observed_at"], row["last_observed_at"], row["probability_pct"]) for row in rows],
            [
                ("2026-09-01T00:00:00Z", "2026-09-01T00:00:00Z", 60.0),
                ("2026-09-28T12:00:00Z", "2026-09-28T12:00:00Z", 70.0),
            ],
        )
        for row in rows:
            self.assertGreaterEqual(row["last_observed_at"], row["observed_at"])

    def test_replaying_inside_coverage_does_not_regress_the_coverage_end(self):
        self.record(70.0, "2026-09-20T00:00:00Z")
        self.assertEqual(self.record(70.0, "2026-09-25T00:00:00Z"), "inserted")
        self.record(70.0, "2026-09-20T00:00:00Z")
        rows = self.store.observations(meeting_date="2026-10-28")
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["last_observed_at"], "2026-09-20T00:00:00Z")
        self.assertEqual(rows[1]["last_observed_at"], "2026-09-25T00:00:00Z")

    def test_series_are_isolated_by_meeting_method_and_outcome(self):
        self.record(70.0, "2026-09-28T12:00:00Z")
        self.record(30.0, "2026-09-28T12:00:00Z", outcome_bp=0, digest=digest_for(30.0))
        self.record(55.0, "2026-09-28T12:00:00Z", method="HISTORICAL_ZQ_RECONSTRUCTED",
                    source="zq", digest=digest_for(55.0))
        self.store.upsert_meeting("2026-12-09")
        self.record(
            10.0, "2026-09-28T12:00:00Z", meeting_date="2026-12-09", digest=digest_for(10.0)
        )
        self.assertEqual(len(self.store.observations(meeting_date="2026-10-28")), 3)
        self.assertEqual(
            len(self.store.observations(
                meeting_date="2026-10-28", method="LIVE_INVESTING_DERIVED", outcome_bp=25)),
            1,
        )
        self.assertEqual(len(self.store.observations(meeting_date="2026-12-09")), 1)

    def test_persistence_across_reopen(self):
        self.record(70.0, "2026-09-28T12:00:00Z")
        reopened = FedwatchHistoryStore(self.store.path)
        rows = reopened.observations(meeting_date="2026-10-28")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["probability_pct"], 70.0)
        self.assertEqual(reopened.get_meeting("2026-10-28")["status"], "UPCOMING")


class MappingAndBackfillStateTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.store = FedwatchHistoryStore(Path(self._tmp.name) / "history.db")

    def test_mapping_upsert_and_validated_filter(self):
        self.store.upsert_mapping_outcome(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "606422-3", "606422-tok-25",
            "…by 25 bps…", {"validation_method": "test"},
        )
        self.store.upsert_mapping_outcome(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False, "VALIDATED",
            "606422", "Fed Decision in October?", "606422-3", "606422-tok-25",
            "…by 25 bps…", {"validation_method": "test"},
        )
        rows = self.store.validated_mappings()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["external_token_id"], "606422-tok-25")
        self.assertEqual(rows[0]["mapping_evidence"]["validation_method"], "test")

        self.store.upsert_mapping_outcome(
            "2026-12-09", "polymarket", "POLYMARKET_CLOB", 0, False, "NOT_FOUND",
            None, None, None, None, None, None,
        )
        self.assertEqual(len(self.store.validated_mappings()), 1)
        self.assertEqual(len(self.store.validated_mappings(["2026-12-09"])), 0)
        self.assertEqual(len(self.store.validated_mappings(["2026-10-28"])), 1)

    def test_backfill_state_roundtrip(self):
        self.store.upsert_backfill_state(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False,
            "606422", "606422-tok-25", "606422-3", "question",
            "2026-09-20T00:00:00Z", 5, 4, 1, "OK", detail={"point_counts": {}},
            now=datetime(2026, 9, 28, 12, 0, tzinfo=timezone.utc),
        )
        state = self.store.get_backfill_state(
            "2026-10-28", "polymarket", "POLYMARKET_CLOB", 25, False
        )
        self.assertEqual(state["point_count"], 5)
        self.assertEqual(state["status"], "OK")
        self.assertEqual(state["first_backfill_at"], "2026-09-28T12:00:00Z")
        self.assertEqual(len(self.store.backfill_states()), 1)


class MalformedStoredDataTests(unittest.TestCase):
    def test_malformed_stored_json_is_reported_not_crashing(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        db = Path(tmp.name) / "history.db"
        store = FedwatchHistoryStore(db)
        store.upsert_meeting("2026-10-28")
        original_digest = digest_for(70.0)
        store.record_observation(
            meeting_date="2026-10-28", source="investing", method="LIVE_INVESTING_DERIVED",
            outcome_bp=25, open_ended=False, probability_pct=70.0,
            raw_probability_pct=None, normalized_probability_pct=None,
            observed_at="2026-09-28T12:00:00Z", source_observed_at=None,
            retrieved_at="2026-09-28T12:00:00Z", quality_status="OK",
            freshness_status=None, digest=original_digest, detail={"origin": "test"},
        )
        connection = sqlite3.connect(str(db))
        try:
            connection.execute(
                "UPDATE fedwatch_probability_observations SET detail_json = 'not json'"
            )
            connection.execute(
                "UPDATE fedwatch_meetings SET calendar_json = '{broken'"
            )
            connection.commit()
        finally:
            connection.close()
        row = store.observations(meeting_date="2026-10-28")[0]
        self.assertIsNone(row["detail"])
        self.assertEqual(row["detail_error"], "stored observation detail is not valid JSON")
        meeting = store.get_meeting("2026-10-28")
        self.assertIsNone(meeting["calendar"])
        self.assertEqual(meeting["calendar_error"], "stored calendar is not valid JSON")


class StorageFootprintTests(unittest.TestCase):
    def test_normal_use_creates_only_the_database_file(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        store = FedwatchHistoryStore(root / "fedwatch" / "fedwatch_history.db")
        store.ensure_schema()
        store.upsert_meeting("2026-10-28")
        for index in range(5):
            store.record_observation(
                meeting_date="2026-10-28", source="investing",
                method="LIVE_INVESTING_DERIVED", outcome_bp=25, open_ended=False,
                probability_pct=70.0, raw_probability_pct=None,
                normalized_probability_pct=None,
                observed_at=f"2026-09-28T12:{index:02d}:00Z", source_observed_at=None,
                retrieved_at=f"2026-09-28T12:{index:02d}:00Z", quality_status="OK",
                freshness_status=None, digest=digest_for(70.0), detail={"origin": "test"},
            )
        files = {path.name for path in (root / "fedwatch").iterdir()}
        self.assertLessEqual(
            files,
            {"fedwatch_history.db", "fedwatch_history.db-journal", "fedwatch_history.db-wal",
             "fedwatch_history.db-shm"},
        )
        self.assertIn("fedwatch_history.db", files)
        self.assertEqual(store.count_observations(), 1)
        self.assertEqual(
            [entry for entry in (root / "fedwatch").iterdir() if entry.suffix not in (".db", ".db-journal", ".db-wal", ".db-shm")],
            [],
        )


if __name__ == "__main__":
    unittest.main()
