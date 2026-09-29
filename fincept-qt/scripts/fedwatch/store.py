"""Durable MarketLab FedWatch history store (SQLite).

Batch B of the FedWatch migration (FEDWATCH_INTEGRATION_PLAN.md sections 7-9)
requires compact durable history in MarketLab's own SQLite, keyed so repeated
refreshes cannot create meaningless duplicates while genuine probability
changes remain distinct observations.

Location
--------
The database lives under the host-provided application profile root
(``FINCEPT_DATA_DIR``, set by ``PythonRunner`` for every provider script) at
``<root>/fedwatch/fedwatch_history.db``, mirroring the established CFTC
archive convention (``<root>/cftc/cot_archive.db``). Exactly one environment
variable is read, only here; every function also accepts an explicit path so
tests and standalone qualification never depend on the host environment. When
``FINCEPT_DATA_DIR`` is absent the default path resolves as an explicit error
instead of silently writing somewhere else.

Observation model
-----------------
One row is one *accepted observation instant* of one
``(meeting_date, source, method, outcome_bp, open_ended)`` series, keyed by its
``observed_at`` instant. The row carries the value exactly as accepted at that
instant; the store never compresses consecutive observations into an interval,
so no accepted instant or value can be lost and no coverage is inferred
between observations. Recording the same instant again with the same content
digest is a duplicate (it only refreshes retrieval metadata); a provider
restatement of the same instant revises that row in place; a new instant is a
new observation row. The digest is computed by the caller over the source
observation's identity and value, so the same underlying provider observation
reaches the same row regardless of which ingestion path recorded it.

Only accepted observations are stored. Provider failures, missing outcomes and
zero-probability placeholders are never inserted; the recording layer reports
them separately. Probabilities and supplied raw/normalized values must be
finite and inside ``[0, 100]``; the durable boundary fails closed instead of
trusting every caller.

The plan's ``raw_probability_pct`` / ``normalized_probability_pct`` columns are
populated where a source's value has that exact per-outcome meaning (the
Polymarket CLOB price is its raw probability). The Investing-derived Fed-side
rows store the meeting-level *cumulative rate-band* distributions and their
normalization record in ``detail_json`` instead: those rows are keyed by rate
band, not by outcome bp, and collapsing them into a per-outcome column would
invent a mapping the methodology does not have. The local per-outcome
probability is the row's ``probability_pct``.

Timing semantics (all UTC ISO 8601 seconds with ``Z``):
    observed_at         when the value was observed by the source, or the
                        documented retrieval instant where the source exposes
                        no observation timestamp
    source_observed_at  the provider's own timestamp when it exists
    retrieved_at        when MarketLab first retrieved this observation
    recorded_at         when MarketLab first wrote the row
    last_*              the same fields for the most recent accepted
                        re-observation of this same instant

The durable schema is versioned with ``PRAGMA user_version``; a database
written by a newer schema is refused, never downgraded.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import sqlite3
from datetime import date, datetime, timezone
from pathlib import Path

from fedwatch.errors import HistoryStoreError
from fedwatch import timeutil

HISTORY_SCHEMA_VERSION = 2
# Version 1 was an unreleased pre-review schema (first as a compressed value
# episode, then as an observation-instant schema without instrument identity).
# It is refused explicitly instead of being silently reused.
LEGACY_SCHEMA_VERSION = 1

DEFAULT_DB_RELATIVE = Path("fedwatch") / "fedwatch_history.db"

MEETING_STATUS_UPCOMING = "UPCOMING"
MEETING_STATUS_PENDING = "PENDING"
MEETING_STATUS_RESOLVED = "RESOLVED"
MEETING_STATUSES = (
    MEETING_STATUS_UPCOMING,
    MEETING_STATUS_PENDING,
    MEETING_STATUS_RESOLVED,
)

_SCHEMA_TABLE_STATEMENTS = (
    """
    CREATE TABLE IF NOT EXISTS fedwatch_meetings (
        meeting_date TEXT PRIMARY KEY,
        status TEXT NOT NULL,
        status_reason TEXT,
        calendar_json TEXT,
        first_seen_at TEXT NOT NULL,
        last_updated_at TEXT NOT NULL,
        resolved_at TEXT,
        actual_outcome_bp INTEGER,
        actual_outcome_source TEXT,
        actual_outcome_detail_json TEXT
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS fedwatch_source_mappings (
        meeting_date TEXT NOT NULL,
        source TEXT NOT NULL,
        method TEXT NOT NULL,
        outcome_bp INTEGER NOT NULL,
        open_ended INTEGER NOT NULL,
        mapping_status TEXT NOT NULL,
        external_event_id TEXT,
        external_event_title TEXT,
        external_market_id TEXT,
        external_token_id TEXT,
        question TEXT,
        mapping_evidence_json TEXT,
        last_revalidation_status TEXT,
        last_revalidated_at TEXT,
        first_seen_at TEXT NOT NULL,
        last_seen_at TEXT NOT NULL,
        PRIMARY KEY (meeting_date, source, method, outcome_bp, open_ended)
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS fedwatch_probability_observations (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        meeting_date TEXT NOT NULL,
        source TEXT NOT NULL,
        method TEXT NOT NULL,
        outcome_bp INTEGER NOT NULL,
        open_ended INTEGER NOT NULL,
        instrument_key TEXT NOT NULL DEFAULT '',
        probability_pct REAL NOT NULL,
        raw_probability_pct REAL,
        normalized_probability_pct REAL,
        observed_at TEXT NOT NULL,
        source_observed_at TEXT,
        retrieved_at TEXT NOT NULL,
        recorded_at TEXT NOT NULL,
        last_retrieved_at TEXT NOT NULL,
        last_recorded_at TEXT NOT NULL,
        quality_status TEXT NOT NULL,
        freshness_status TEXT,
        content_digest TEXT NOT NULL,
        observation_count INTEGER NOT NULL DEFAULT 1,
        detail_json TEXT,
        UNIQUE (meeting_date, source, method, outcome_bp, open_ended, instrument_key, observed_at)
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS fedwatch_backfills (
        meeting_date TEXT NOT NULL,
        source TEXT NOT NULL,
        method TEXT NOT NULL,
        outcome_bp INTEGER NOT NULL,
        open_ended INTEGER NOT NULL,
        external_event_id TEXT,
        token_id TEXT,
        market_id TEXT,
        question TEXT,
        first_backfill_at TEXT NOT NULL,
        last_backfill_at TEXT NOT NULL,
        last_point_observed_at TEXT,
        point_count INTEGER NOT NULL DEFAULT 0,
        inserted_count INTEGER NOT NULL DEFAULT 0,
        observed_again_count INTEGER NOT NULL DEFAULT 0,
        status TEXT NOT NULL,
        detail_json TEXT,
        PRIMARY KEY (meeting_date, source, method, outcome_bp, open_ended)
    )
    """,
)

_SCHEMA_INDEX_STATEMENTS = (
    "CREATE INDEX IF NOT EXISTS idx_fw_obs_series "
    "ON fedwatch_probability_observations "
    "(meeting_date, method, outcome_bp, open_ended, observed_at)",
    "CREATE INDEX IF NOT EXISTS idx_fw_obs_source "
    "ON fedwatch_probability_observations (source, method, meeting_date)",
    "CREATE INDEX IF NOT EXISTS idx_fw_obs_instrument "
    "ON fedwatch_probability_observations "
    "(meeting_date, method, outcome_bp, open_ended, instrument_key, observed_at)",
)

_REQUIRED_COLUMNS = {
    "fedwatch_meetings": {
        "meeting_date", "status", "status_reason", "calendar_json",
        "first_seen_at", "last_updated_at", "resolved_at", "actual_outcome_bp",
        "actual_outcome_source", "actual_outcome_detail_json",
    },
    "fedwatch_source_mappings": {
        "meeting_date", "source", "method", "outcome_bp", "open_ended",
        "mapping_status", "external_event_id", "external_event_title",
        "external_market_id", "external_token_id", "question",
        "mapping_evidence_json", "last_revalidation_status", "last_revalidated_at",
        "first_seen_at", "last_seen_at",
    },
    "fedwatch_probability_observations": {
        "meeting_date", "source", "method", "outcome_bp", "open_ended",
        "instrument_key", "probability_pct", "raw_probability_pct",
        "normalized_probability_pct", "observed_at", "source_observed_at",
        "retrieved_at", "recorded_at", "last_retrieved_at", "last_recorded_at",
        "quality_status", "freshness_status", "content_digest",
        "observation_count", "detail_json",
    },
    "fedwatch_backfills": {
        "meeting_date", "source", "method", "outcome_bp", "open_ended",
        "external_event_id", "token_id", "market_id", "question",
        "first_backfill_at", "last_backfill_at", "last_point_observed_at",
        "point_count", "inserted_count", "observed_again_count", "status",
        "detail_json",
    },
}


def default_db_path() -> Path:
    """Resolve the application-owned history database path.

    ``FINCEPT_DATA_DIR`` is the application profile root the host passes to
    every provider script. Without it the function fails closed: a standalone
    caller must pass an explicit path (``--db``) rather than having market
    history written to an unintended location.
    """
    root = os.environ.get("FINCEPT_DATA_DIR")
    if not root:
        raise HistoryStoreError(
            "FEDWATCH_HISTORY_LOCATION_UNAVAILABLE",
            "FINCEPT_DATA_DIR is not set; the FedWatch history database cannot be "
            "located. Pass an explicit path (--db) for standalone runs.",
        )
    return Path(root) / DEFAULT_DB_RELATIVE


def content_digest(payload: dict) -> str:
    """Canonical digest over the content that identifies an observation."""
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"), default=str)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _utc_now_iso() -> str:
    return timeutil.iso_z(datetime.now(timezone.utc))


def _require_iso_instant(value: str, field: str) -> str:
    try:
        return timeutil.iso_z(timeutil.parse_iso_z(value))
    except (AttributeError, ValueError) as exc:
        raise HistoryStoreError(
            "FEDWATCH_HISTORY_WRITE_FAILED",
            f"{field} is not a valid UTC instant: {value!r}",
        ) from exc


class FedwatchHistoryStore:
    """Durable observation store for FedWatch probability history."""

    def __init__(self, path: str | Path):
        self.path = Path(path)

    # ── connection / schema ───────────────────────────────────────────────

    def _connect(self) -> sqlite3.Connection:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            conn = sqlite3.connect(str(self.path), timeout=30)
        except (OSError, sqlite3.Error) as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_UNAVAILABLE",
                f"FedWatch history database cannot be opened at {self.path}: {exc}",
                detail={"path": str(self.path)},
            ) from exc
        conn.row_factory = sqlite3.Row
        return conn

    def _prepare(self, conn: sqlite3.Connection) -> None:
        try:
            version = int(conn.execute("PRAGMA user_version").fetchone()[0])
            if version > HISTORY_SCHEMA_VERSION:
                raise HistoryStoreError(
                    "FEDWATCH_HISTORY_SCHEMA_NEWER",
                    "the FedWatch history database was written by a newer schema; "
                    "this application will not modify it",
                    detail={"database_version": version, "supported_version": HISTORY_SCHEMA_VERSION},
                )
            if version == LEGACY_SCHEMA_VERSION:
                raise HistoryStoreError(
                    "FEDWATCH_HISTORY_SCHEMA_INCOMPATIBLE",
                    "the FedWatch history database was written by an unreleased "
                    "pre-review schema (version 1) with different observation "
                    "semantics; delete the file or start a new history store",
                    detail={"database_version": version, "supported_version": HISTORY_SCHEMA_VERSION},
                )
            for statement in _SCHEMA_TABLE_STATEMENTS:
                conn.execute(statement)
            incompatible = []
            for table, required in _REQUIRED_COLUMNS.items():
                columns = {
                    row["name"] for row in conn.execute(f"PRAGMA table_info({table})")
                }
                if not columns:
                    incompatible.append(f"{table}: missing table")
                    continue
                missing = sorted(required - columns)
                if missing:
                    incompatible.append(f"{table}: missing {missing}")
            if "last_observed_at" in {
                row["name"] for row in conn.execute(
                    "PRAGMA table_info(fedwatch_probability_observations)"
                )
            }:
                incompatible.append("fedwatch_probability_observations: legacy episode column")
            if incompatible:
                raise HistoryStoreError(
                    "FEDWATCH_HISTORY_SCHEMA_INCOMPATIBLE",
                    "the FedWatch history database schema does not match this "
                    "application version; delete the file or start a new history store",
                    detail={
                        "database_version": version,
                        "supported_version": HISTORY_SCHEMA_VERSION,
                        "problems": incompatible,
                    },
                )
            for statement in _SCHEMA_INDEX_STATEMENTS:
                conn.execute(statement)
            if version < HISTORY_SCHEMA_VERSION:
                conn.execute(f"PRAGMA user_version = {HISTORY_SCHEMA_VERSION}")
                conn.commit()
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_UNAVAILABLE",
                f"FedWatch history schema cannot be initialized: {exc}",
                detail={"path": str(self.path)},
            ) from exc

    def ensure_schema(self) -> None:
        conn = self._connect()
        try:
            self._prepare(conn)
        finally:
            conn.close()

    def close(self) -> None:
        """Compatibility no-op; connections are opened per operation."""

    # ── meetings ──────────────────────────────────────────────────────────

    def upsert_meeting(
        self,
        meeting_date: str,
        calendar: dict | None = None,
        default_status: str = MEETING_STATUS_UPCOMING,
        now: datetime | None = None,
    ) -> str:
        """Insert a meeting if unseen; otherwise refresh its calendar snapshot.

        An existing meeting keeps its lifecycle status: only the explicit
        resolution/pending transitions below may change it.
        """
        if default_status not in MEETING_STATUSES:
            raise ValueError(f"unknown meeting status {default_status!r}")
        now_iso = timeutil.iso_z(now) if now is not None else _utc_now_iso()
        calendar_json = json.dumps(calendar, sort_keys=True) if calendar else None
        conn = self._connect()
        try:
            self._prepare(conn)
            with conn:
                row = conn.execute(
                    "SELECT status FROM fedwatch_meetings WHERE meeting_date = ?",
                    (meeting_date,),
                ).fetchone()
                if row is None:
                    conn.execute(
                        """
                        INSERT INTO fedwatch_meetings
                            (meeting_date, status, status_reason, calendar_json,
                             first_seen_at, last_updated_at)
                        VALUES (?, ?, ?, ?, ?, ?)
                        """,
                        (meeting_date, default_status, None, calendar_json, now_iso, now_iso),
                    )
                    return "inserted"
                conn.execute(
                    """
                    UPDATE fedwatch_meetings
                       SET last_updated_at = ?
                     WHERE meeting_date = ?
                    """,
                    (now_iso, meeting_date),
                )
                if calendar_json is not None:
                    conn.execute(
                        """
                        UPDATE fedwatch_meetings
                           SET calendar_json = ?
                         WHERE meeting_date = ?
                        """,
                        (calendar_json, meeting_date),
                    )
                return "existing"
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"FedWatch meeting upsert failed: {exc}",
                detail={"meeting_date": meeting_date},
            ) from exc
        finally:
            conn.close()

    def mark_pending(self, meeting_date: str, reason: str, detail: dict | None = None,
                     now: datetime | None = None) -> None:
        now_iso = timeutil.iso_z(now) if now is not None else _utc_now_iso()
        detail_json = json.dumps(detail, sort_keys=True) if detail else None
        conn = self._connect()
        try:
            self._prepare(conn)
            with conn:
                conn.execute(
                    """
                    UPDATE fedwatch_meetings
                       SET status = ?, status_reason = ?, last_updated_at = ?,
                           actual_outcome_detail_json = COALESCE(?, actual_outcome_detail_json)
                     WHERE meeting_date = ? AND status != ?
                    """,
                    (
                        MEETING_STATUS_PENDING,
                        reason,
                        now_iso,
                        detail_json,
                        meeting_date,
                        MEETING_STATUS_RESOLVED,
                    ),
                )
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"FedWatch meeting pending transition failed: {exc}",
                detail={"meeting_date": meeting_date},
            ) from exc
        finally:
            conn.close()

    def mark_resolved(
        self,
        meeting_date: str,
        actual_outcome_bp: int,
        source: str,
        detail: dict | None = None,
        now: datetime | None = None,
    ) -> None:
        now_iso = timeutil.iso_z(now) if now is not None else _utc_now_iso()
        detail_json = json.dumps(detail, sort_keys=True) if detail else None
        conn = self._connect()
        try:
            self._prepare(conn)
            with conn:
                conn.execute(
                    """
                    UPDATE fedwatch_meetings
                       SET status = ?, status_reason = NULL, resolved_at = ?,
                           actual_outcome_bp = ?, actual_outcome_source = ?,
                           actual_outcome_detail_json = ?, last_updated_at = ?
                     WHERE meeting_date = ?
                    """,
                    (
                        MEETING_STATUS_RESOLVED,
                        now_iso,
                        int(actual_outcome_bp),
                        source,
                        detail_json,
                        now_iso,
                        meeting_date,
                    ),
                )
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"FedWatch meeting resolution failed: {exc}",
                detail={"meeting_date": meeting_date},
            ) from exc
        finally:
            conn.close()

    @staticmethod
    def _meeting_row(row: sqlite3.Row) -> dict:
        out = {
            "meeting_date": row["meeting_date"],
            "status": row["status"],
            "status_reason": row["status_reason"],
            "resolved_at": row["resolved_at"],
            "actual_outcome_bp": row["actual_outcome_bp"],
            "actual_outcome_source": row["actual_outcome_source"],
            "first_seen_at": row["first_seen_at"],
            "last_updated_at": row["last_updated_at"],
        }
        if row["calendar_json"]:
            try:
                out["calendar"] = json.loads(row["calendar_json"])
            except ValueError:
                out["calendar"] = None
                out["calendar_error"] = "stored calendar is not valid JSON"
        else:
            out["calendar"] = None
        if row["actual_outcome_detail_json"]:
            try:
                out["actual_outcome_detail"] = json.loads(row["actual_outcome_detail_json"])
            except ValueError:
                out["actual_outcome_detail"] = None
                out["actual_outcome_detail_error"] = "stored resolution detail is not valid JSON"
        else:
            out["actual_outcome_detail"] = None
        return out

    def get_meeting(self, meeting_date: str) -> dict | None:
        conn = self._connect()
        try:
            self._prepare(conn)
            row = conn.execute(
                "SELECT * FROM fedwatch_meetings WHERE meeting_date = ?", (meeting_date,)
            ).fetchone()
            return self._meeting_row(row) if row else None
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED",
                f"FedWatch meeting read failed: {exc}",
                detail={"meeting_date": meeting_date},
            ) from exc
        finally:
            conn.close()

    def list_meetings(self) -> list[dict]:
        conn = self._connect()
        try:
            self._prepare(conn)
            rows = conn.execute(
                "SELECT * FROM fedwatch_meetings ORDER BY meeting_date"
            ).fetchall()
            return [self._meeting_row(row) for row in rows]
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED", f"FedWatch meeting listing failed: {exc}"
            ) from exc
        finally:
            conn.close()

    def meetings_awaiting_resolution(self, as_of: date) -> list[dict]:
        """Stored meetings whose decision day has passed but are not resolved."""
        conn = self._connect()
        try:
            self._prepare(conn)
            rows = conn.execute(
                """
                SELECT * FROM fedwatch_meetings
                 WHERE status != ? AND meeting_date < ?
                 ORDER BY meeting_date
                """,
                (MEETING_STATUS_RESOLVED, as_of.isoformat()),
            ).fetchall()
            return [self._meeting_row(row) for row in rows]
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED",
                f"FedWatch pending-meeting listing failed: {exc}",
            ) from exc
        finally:
            conn.close()

    # ── Polymarket mapping provenance ─────────────────────────────────────

    def upsert_mapping_outcome(
        self,
        meeting_date: str,
        source: str,
        method: str,
        outcome_bp: int,
        open_ended: bool,
        mapping_status: str,
        external_event_id: str | None,
        external_event_title: str | None,
        external_market_id: str | None,
        external_token_id: str | None,
        question: str | None,
        mapping_evidence: dict | None,
        now: datetime | None = None,
    ) -> str:
        now_iso = timeutil.iso_z(now) if now is not None else _utc_now_iso()
        evidence_json = json.dumps(mapping_evidence, sort_keys=True) if mapping_evidence else None
        key = (meeting_date, source, method, int(outcome_bp), 1 if open_ended else 0)
        conn = self._connect()
        try:
            self._prepare(conn)
            with conn:
                row = conn.execute(
                    """
                    SELECT 1 FROM fedwatch_source_mappings
                     WHERE meeting_date = ? AND source = ? AND method = ?
                       AND outcome_bp = ? AND open_ended = ?
                    """,
                    key,
                ).fetchone()
                if row is None:
                    conn.execute(
                        """
                        INSERT INTO fedwatch_source_mappings
                            (meeting_date, source, method, outcome_bp, open_ended,
                             mapping_status, external_event_id, external_event_title,
                             external_market_id, external_token_id, question,
                             mapping_evidence_json, last_revalidation_status,
                             last_revalidated_at, first_seen_at, last_seen_at)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                        """,
                        (*key, mapping_status, external_event_id, external_event_title,
                         external_market_id, external_token_id, question, evidence_json,
                         mapping_status, now_iso, now_iso, now_iso),
                    )
                    return "inserted"
                conn.execute(
                    """
                    UPDATE fedwatch_source_mappings
                       SET mapping_status = ?, external_event_id = ?,
                           external_event_title = ?, external_market_id = ?,
                           external_token_id = ?, question = ?,
                           mapping_evidence_json = ?, last_revalidation_status = ?,
                           last_revalidated_at = ?, last_seen_at = ?
                     WHERE meeting_date = ? AND source = ? AND method = ?
                       AND outcome_bp = ? AND open_ended = ?
                    """,
                    (mapping_status, external_event_id, external_event_title, external_market_id,
                     external_token_id, question, evidence_json, mapping_status, now_iso,
                     now_iso, *key),
                )
                return "updated"
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"FedWatch mapping upsert failed: {exc}",
                detail={"meeting_date": meeting_date, "outcome_bp": outcome_bp},
            ) from exc
        finally:
            conn.close()

    def mark_mapping_revalidation(
        self,
        meeting_date: str,
        source: str,
        method: str,
        status: str,
        now: datetime | None = None,
    ) -> None:
        """Record the latest automatic re-validation result for a meeting.

        A negative re-validation (``NOT_FOUND``/``AMBIGUOUS``) is stored on the
        previously validated rows so the backfill path can fail closed for a
        meeting that is still current, while a resolved meeting keeps its last
        validated historical mapping.
        """
        now_iso = timeutil.iso_z(now) if now is not None else _utc_now_iso()
        conn = self._connect()
        try:
            self._prepare(conn)
            with conn:
                conn.execute(
                    """
                    UPDATE fedwatch_source_mappings
                       SET last_revalidation_status = ?, last_revalidated_at = ?
                     WHERE meeting_date = ? AND source = ? AND method = ?
                    """,
                    (status, now_iso, meeting_date, source, method),
                )
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"FedWatch mapping revalidation write failed: {exc}",
                detail={"meeting_date": meeting_date},
            ) from exc
        finally:
            conn.close()

    def validated_mappings(self, meeting_dates: list[str] | None = None) -> list[dict]:
        """Stored validated Polymarket outcome mappings, optionally filtered."""
        query = (
            "SELECT * FROM fedwatch_source_mappings WHERE mapping_status = 'VALIDATED'"
        )
        params: list = []
        if meeting_dates:
            placeholders = ",".join("?" for _ in meeting_dates)
            query += f" AND meeting_date IN ({placeholders})"
            params.extend(meeting_dates)
        query += " ORDER BY meeting_date, outcome_bp, open_ended"
        conn = self._connect()
        try:
            self._prepare(conn)
            rows = conn.execute(query, params).fetchall()
            out = []
            for row in rows:
                entry = dict(row)
                for key in ("mapping_evidence_json",):
                    value = entry.pop(key, None)
                    if value:
                        try:
                            entry["mapping_evidence"] = json.loads(value)
                        except ValueError:
                            entry["mapping_evidence"] = None
                            entry["mapping_evidence_error"] = "stored mapping evidence is not valid JSON"
                    else:
                        entry["mapping_evidence"] = None
                entry["open_ended"] = bool(entry["open_ended"])
                out.append(entry)
            return out
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED", f"FedWatch mapping read failed: {exc}"
            ) from exc
        finally:
            conn.close()

    # ── observations (accepted observation instants) ──────────────────────

    @staticmethod
    def _validate_probability(value, field: str, meeting_date: str, method: str, outcome_bp: int):
        if value is None:
            return None
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"{field} is not numeric for {meeting_date} {method} {outcome_bp}",
            )
        parsed = float(value)
        if not math.isfinite(parsed) or not 0.0 <= parsed <= 100.0:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"{field} is not a finite probability in [0, 100] for "
                f"{meeting_date} {method} {outcome_bp}: {value!r}",
            )
        return parsed

    def record_observation(
        self,
        meeting_date: str,
        source: str,
        method: str,
        outcome_bp: int,
        open_ended: bool,
        probability_pct: float,
        raw_probability_pct: float | None,
        normalized_probability_pct: float | None,
        observed_at: str,
        source_observed_at: str | None,
        retrieved_at: str,
        quality_status: str,
        freshness_status: str | None,
        digest: str,
        instrument_key: str = "",
        detail: dict | None = None,
        recorded_at: datetime | None = None,
    ) -> str:
        """Record one accepted observation instant.

        Returns ``inserted``, ``duplicate`` or ``revised``. No compression is
        applied: a different instant is always a new observation row, so the
        stored chronology is exactly the accepted chronology. ``instrument_key``
        is the source instrument identity where the source has one (the
        Polymarket CLOB token id); it is part of the observation key so a
        re-created market's observations never overwrite another token's."""
        observed_at = _require_iso_instant(observed_at, "observed_at")
        retrieved_at = _require_iso_instant(retrieved_at, "retrieved_at")
        if source_observed_at is not None:
            source_observed_at = _require_iso_instant(source_observed_at, "source_observed_at")
        probability = self._validate_probability(
            probability_pct, "probability_pct", meeting_date, method, outcome_bp
        )
        raw_probability = self._validate_probability(
            raw_probability_pct, "raw_probability_pct", meeting_date, method, outcome_bp
        )
        normalized_probability = self._validate_probability(
            normalized_probability_pct, "normalized_probability_pct", meeting_date, method, outcome_bp
        )
        if not isinstance(instrument_key, str):
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"instrument_key is not a string for {meeting_date} {method} {outcome_bp}",
            )
        recorded_iso = timeutil.iso_z(recorded_at) if recorded_at is not None else _utc_now_iso()
        detail_json = json.dumps(detail, sort_keys=True) if detail else None
        open_ended_flag = 1 if open_ended else 0
        series = (meeting_date, source, method, int(outcome_bp), open_ended_flag, instrument_key)

        conn = self._connect()
        try:
            self._prepare(conn)
            with conn:
                existing = conn.execute(
                    """
                    SELECT * FROM fedwatch_probability_observations
                     WHERE meeting_date = ? AND source = ? AND method = ?
                       AND outcome_bp = ? AND open_ended = ? AND instrument_key = ?
                       AND observed_at = ?
                    """,
                    (*series, observed_at),
                ).fetchone()
                if existing is None:
                    conn.execute(
                        """
                        INSERT INTO fedwatch_probability_observations
                            (meeting_date, source, method, outcome_bp, open_ended,
                             instrument_key, probability_pct, raw_probability_pct,
                             normalized_probability_pct,
                             observed_at, source_observed_at, retrieved_at, recorded_at,
                             last_retrieved_at, last_recorded_at,
                             quality_status, freshness_status, content_digest,
                             observation_count, detail_json)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?)
                        """,
                        (
                            meeting_date, source, method, int(outcome_bp), open_ended_flag,
                            instrument_key,
                            probability, raw_probability, normalized_probability,
                            observed_at, source_observed_at, retrieved_at, recorded_iso,
                            retrieved_at, recorded_iso,
                            quality_status, freshness_status, digest, detail_json,
                        ),
                    )
                    return "inserted"
                if existing["content_digest"] == digest:
                    conn.execute(
                        """
                        UPDATE fedwatch_probability_observations
                           SET last_retrieved_at = MAX(last_retrieved_at, ?),
                               last_recorded_at = MAX(last_recorded_at, ?),
                               observation_count = observation_count + 1
                         WHERE id = ?
                        """,
                        (retrieved_at, recorded_iso, existing["id"]),
                    )
                    return "duplicate"
                conn.execute(
                    """
                    UPDATE fedwatch_probability_observations
                       SET probability_pct = ?, raw_probability_pct = ?,
                           normalized_probability_pct = ?, source_observed_at = ?,
                           quality_status = ?, freshness_status = ?,
                           content_digest = ?, detail_json = ?,
                           last_retrieved_at = MAX(last_retrieved_at, ?),
                           last_recorded_at = MAX(last_recorded_at, ?),
                           observation_count = observation_count + 1
                     WHERE id = ?
                    """,
                    (
                        probability, raw_probability, normalized_probability,
                        source_observed_at, quality_status, freshness_status,
                        digest, detail_json, retrieved_at, recorded_iso,
                        existing["id"],
                    ),
                )
                return "revised"
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"FedWatch observation write failed: {exc}",
                detail={"meeting_date": meeting_date, "method": method, "outcome_bp": outcome_bp},
            ) from exc
        finally:
            conn.close()

    @staticmethod
    def _observation_row(row: sqlite3.Row, include_detail: bool = True) -> dict:
        out = {
            "id": row["id"],
            "meeting_date": row["meeting_date"],
            "source": row["source"],
            "method": row["method"],
            "outcome_bp": row["outcome_bp"],
            "open_ended": bool(row["open_ended"]),
            "instrument_key": row["instrument_key"],
            "probability_pct": row["probability_pct"],
            "raw_probability_pct": row["raw_probability_pct"],
            "normalized_probability_pct": row["normalized_probability_pct"],
            "observed_at": row["observed_at"],
            "source_observed_at": row["source_observed_at"],
            "retrieved_at": row["retrieved_at"],
            "recorded_at": row["recorded_at"],
            "last_retrieved_at": row["last_retrieved_at"],
            "last_recorded_at": row["last_recorded_at"],
            "quality_status": row["quality_status"],
            "freshness_status": row["freshness_status"],
            "observation_count": row["observation_count"],
        }
        if include_detail:
            if row["detail_json"]:
                try:
                    out["detail"] = json.loads(row["detail_json"])
                except ValueError:
                    out["detail"] = None
                    out["detail_error"] = "stored observation detail is not valid JSON"
            else:
                out["detail"] = None
        return out

    def observations(
        self,
        meeting_date: str | None = None,
        method: str | None = None,
        outcome_bp: int | None = None,
        open_ended: bool | None = None,
        source: str | None = None,
    ) -> list[dict]:
        query = "SELECT * FROM fedwatch_probability_observations WHERE 1 = 1"
        params: list = []
        if meeting_date is not None:
            query += " AND meeting_date = ?"
            params.append(meeting_date)
        if method is not None:
            query += " AND method = ?"
            params.append(method)
        if outcome_bp is not None:
            query += " AND outcome_bp = ?"
            params.append(int(outcome_bp))
        if open_ended is not None:
            query += " AND open_ended = ?"
            params.append(1 if open_ended else 0)
        if source is not None:
            query += " AND source = ?"
            params.append(source)
        query += " ORDER BY meeting_date, method, outcome_bp, open_ended, observed_at, id"
        conn = self._connect()
        try:
            self._prepare(conn)
            rows = conn.execute(query, params).fetchall()
            return [self._observation_row(row) for row in rows]
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED", f"FedWatch observation read failed: {exc}"
            ) from exc
        finally:
            conn.close()

    def count_observations(self) -> int:
        conn = self._connect()
        try:
            self._prepare(conn)
            return int(conn.execute(
                "SELECT COUNT(*) FROM fedwatch_probability_observations"
            ).fetchone()[0])
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED", f"FedWatch observation count failed: {exc}"
            ) from exc
        finally:
            conn.close()

    def method_summary(self, meeting_date: str | None = None) -> list[dict]:
        query = (
            "SELECT meeting_date, source, method, COUNT(*) AS observations, "
            "SUM(observation_count) AS accepted_count, MIN(observed_at) AS first_observed_at, "
            "MAX(observed_at) AS last_observed_at "
            "FROM fedwatch_probability_observations"
        )
        params: list = []
        if meeting_date is not None:
            query += " WHERE meeting_date = ?"
            params.append(meeting_date)
        query += " GROUP BY meeting_date, source, method ORDER BY meeting_date, method"
        conn = self._connect()
        try:
            self._prepare(conn)
            rows = conn.execute(query, params).fetchall()
            return [dict(row) for row in rows]
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED", f"FedWatch method summary failed: {exc}"
            ) from exc
        finally:
            conn.close()

    # ── backfill state ────────────────────────────────────────────────────

    def get_backfill_state(self, meeting_date: str, source: str, method: str,
                           outcome_bp: int, open_ended: bool) -> dict | None:
        conn = self._connect()
        try:
            self._prepare(conn)
            row = conn.execute(
                """
                SELECT * FROM fedwatch_backfills
                 WHERE meeting_date = ? AND source = ? AND method = ?
                   AND outcome_bp = ? AND open_ended = ?
                """,
                (meeting_date, source, method, int(outcome_bp), 1 if open_ended else 0),
            ).fetchone()
            return dict(row) if row else None
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED", f"FedWatch backfill state read failed: {exc}"
            ) from exc
        finally:
            conn.close()

    def upsert_backfill_state(
        self,
        meeting_date: str,
        source: str,
        method: str,
        outcome_bp: int,
        open_ended: bool,
        external_event_id: str | None,
        token_id: str | None,
        market_id: str | None,
        question: str | None,
        last_point_observed_at: str | None,
        point_count: int,
        inserted_count: int,
        observed_again_count: int,
        status: str,
        detail: dict | None = None,
        now: datetime | None = None,
    ) -> None:
        now_iso = timeutil.iso_z(now) if now is not None else _utc_now_iso()
        detail_json = json.dumps(detail, sort_keys=True) if detail else None
        key = (meeting_date, source, method, int(outcome_bp), 1 if open_ended else 0)
        conn = self._connect()
        try:
            self._prepare(conn)
            with conn:
                existing = conn.execute(
                    """
                    SELECT first_backfill_at FROM fedwatch_backfills
                     WHERE meeting_date = ? AND source = ? AND method = ?
                       AND outcome_bp = ? AND open_ended = ?
                    """,
                    key,
                ).fetchone()
                first_backfill_at = existing["first_backfill_at"] if existing else now_iso
                if existing:
                    conn.execute(
                        """
                        UPDATE fedwatch_backfills
                           SET external_event_id = ?, token_id = ?, market_id = ?,
                               question = ?, last_backfill_at = ?,
                               last_point_observed_at = ?, point_count = ?,
                               inserted_count = ?, observed_again_count = ?,
                               status = ?, detail_json = ?
                         WHERE meeting_date = ? AND source = ? AND method = ?
                           AND outcome_bp = ? AND open_ended = ?
                        """,
                        (
                            external_event_id, token_id, market_id, question, now_iso,
                            last_point_observed_at, int(point_count), int(inserted_count),
                            int(observed_again_count), status, detail_json, *key,
                        ),
                    )
                else:
                    conn.execute(
                        """
                        INSERT INTO fedwatch_backfills
                            (meeting_date, source, method, outcome_bp, open_ended,
                             external_event_id, token_id, market_id, question,
                             first_backfill_at, last_backfill_at,
                             last_point_observed_at, point_count, inserted_count,
                             observed_again_count, status, detail_json)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                        """,
                        (
                            *key, external_event_id, token_id, market_id, question,
                            first_backfill_at, now_iso, last_point_observed_at,
                            int(point_count), int(inserted_count), int(observed_again_count),
                            status, detail_json,
                        ),
                    )
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"FedWatch backfill state write failed: {exc}",
                detail={"meeting_date": meeting_date, "outcome_bp": outcome_bp},
            ) from exc
        finally:
            conn.close()

    def backfill_states(self, meeting_date: str | None = None) -> list[dict]:
        query = "SELECT * FROM fedwatch_backfills"
        params: list = []
        if meeting_date is not None:
            query += " WHERE meeting_date = ?"
            params.append(meeting_date)
        query += " ORDER BY meeting_date, outcome_bp, open_ended"
        conn = self._connect()
        try:
            self._prepare(conn)
            rows = conn.execute(query, params).fetchall()
            out = []
            for row in rows:
                entry = dict(row)
                detail = entry.pop("detail_json", None)
                if detail:
                    try:
                        entry["detail"] = json.loads(detail)
                    except ValueError:
                        entry["detail"] = None
                        entry["detail_error"] = "stored backfill detail is not valid JSON"
                else:
                    entry["detail"] = None
                entry["open_ended"] = bool(entry["open_ended"])
                out.append(entry)
            return out
        except sqlite3.Error as exc:
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_READ_FAILED", f"FedWatch backfill state listing failed: {exc}"
            ) from exc
        finally:
            conn.close()
