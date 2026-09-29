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
One row is a *value episode*: a maximal run of accepted observations of one
``(meeting_date, source, method, outcome_bp, open_ended)`` series that share
the same content digest. Repeating an identical refresh extends the episode's
``last_*`` timestamps and increments ``observation_count`` instead of creating
a row; a changed observation closes the episode and inserts a new one. A
provider restatement of an already-stored instant revises that row in place.
The digest is computed by the caller over the exact stored content
(probability plus the raw/normalized distribution context), so no stored
value is silently dropped.

Only accepted observations are stored. Provider failures, missing outcomes and
zero-probability placeholders are never inserted; the recording layer reports
them separately.

The plan's ``raw_probability_pct`` / ``normalized_probability_pct`` columns are
populated where a source's value has that exact per-outcome meaning (the
Polymarket CLOB price is both). The Investing-derived Fed-side rows store the
meeting-level *cumulative rate-band* distributions and their normalization
record in ``detail_json`` instead: those rows are keyed by rate band, not by
outcome bp, and collapsing them into a per-outcome column would invent a
mapping the methodology does not have. The local per-outcome probability is
the row's ``probability_pct``.

Timing semantics (all UTC ISO 8601 seconds with ``Z``):
    observed_at         when this value was first observed by MarketLab (the
                        provider observation time where the source exposes
                        one; otherwise the documented retrieval instant)
    source_observed_at  the provider's own timestamp when it exists
    retrieved_at        when MarketLab retrieved the data
    recorded_at         when MarketLab first wrote the row
    last_*              the same fields for the most recent accepted
                        observation covered by the episode

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

HISTORY_SCHEMA_VERSION = 1

DEFAULT_DB_RELATIVE = Path("fedwatch") / "fedwatch_history.db"

# Same-value observations only extend an episode's coverage while consecutive
# accepted retrievals stay within this gap. A longer gap is a coverage gap and
# opens a new episode even for an identical value, so the store never claims a
# value was continuously observed across a period with no observation.
EPISODE_CONTINUITY_MAX_GAP_SECONDS = 36 * 3600

MEETING_STATUS_UPCOMING = "UPCOMING"
MEETING_STATUS_PENDING = "PENDING"
MEETING_STATUS_RESOLVED = "RESOLVED"
MEETING_STATUSES = (
    MEETING_STATUS_UPCOMING,
    MEETING_STATUS_PENDING,
    MEETING_STATUS_RESOLVED,
)

_SCHEMA_STATEMENTS = (
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
        probability_pct REAL NOT NULL,
        raw_probability_pct REAL,
        normalized_probability_pct REAL,
        observed_at TEXT NOT NULL,
        source_observed_at TEXT,
        retrieved_at TEXT NOT NULL,
        recorded_at TEXT NOT NULL,
        last_observed_at TEXT NOT NULL,
        last_retrieved_at TEXT NOT NULL,
        last_recorded_at TEXT NOT NULL,
        quality_status TEXT NOT NULL,
        freshness_status TEXT,
        content_digest TEXT NOT NULL,
        observation_count INTEGER NOT NULL DEFAULT 1,
        detail_json TEXT,
        UNIQUE (meeting_date, source, method, outcome_bp, open_ended, observed_at)
    )
    """,
    "CREATE INDEX IF NOT EXISTS idx_fw_obs_series "
    "ON fedwatch_probability_observations "
    "(meeting_date, method, outcome_bp, open_ended, observed_at)",
    "CREATE INDEX IF NOT EXISTS idx_fw_obs_source "
    "ON fedwatch_probability_observations (source, method, meeting_date)",
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
    """Canonical digest over the exact content that defines a value episode."""
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
    """Durable episode store for FedWatch probability observations."""

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
            if version < HISTORY_SCHEMA_VERSION:
                for statement in _SCHEMA_STATEMENTS:
                    conn.execute(statement)
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

    # ── observations (value episodes) ─────────────────────────────────────

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
        detail: dict | None = None,
        recorded_at: datetime | None = None,
    ) -> str:
        """Record one accepted observation into its value episode.

        Returns one of ``inserted``, ``extended``, ``duplicate`` or ``revised``.
        """
        observed_at = _require_iso_instant(observed_at, "observed_at")
        retrieved_at = _require_iso_instant(retrieved_at, "retrieved_at")
        if source_observed_at is not None:
            source_observed_at = _require_iso_instant(source_observed_at, "source_observed_at")
        if not isinstance(probability_pct, (int, float)) or isinstance(probability_pct, bool):
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"probability_pct is not numeric for {meeting_date} {method} {outcome_bp}",
            )
        if not math.isfinite(float(probability_pct)):
            raise HistoryStoreError(
                "FEDWATCH_HISTORY_WRITE_FAILED",
                f"probability_pct is not finite for {meeting_date} {method} {outcome_bp}",
            )
        recorded_iso = timeutil.iso_z(recorded_at) if recorded_at is not None else _utc_now_iso()
        detail_json = json.dumps(detail, sort_keys=True) if detail else None
        open_ended_flag = 1 if open_ended else 0
        series = (meeting_date, source, method, int(outcome_bp), open_ended_flag)

        conn = self._connect()
        try:
            self._prepare(conn)
            with conn:
                latest = conn.execute(
                    """
                    SELECT * FROM fedwatch_probability_observations
                     WHERE meeting_date = ? AND source = ? AND method = ?
                       AND outcome_bp = ? AND open_ended = ?
                     ORDER BY observed_at DESC, id DESC LIMIT 1
                    """,
                    series,
                ).fetchone()

                if latest is not None and latest["observed_at"] == observed_at:
                    if latest["content_digest"] == digest:
                        conn.execute(
                            """
                            UPDATE fedwatch_probability_observations
                               SET last_observed_at = MAX(last_observed_at, ?),
                                   last_retrieved_at = MAX(last_retrieved_at, ?),
                                   last_recorded_at = MAX(last_recorded_at, ?),
                                   observation_count = observation_count + 1
                             WHERE id = ?
                            """,
                            (observed_at, retrieved_at, recorded_iso, latest["id"]),
                        )
                        return "duplicate"
                    conn.execute(
                        """
                        UPDATE fedwatch_probability_observations
                           SET probability_pct = ?, raw_probability_pct = ?,
                               normalized_probability_pct = ?, source_observed_at = ?,
                               quality_status = ?, freshness_status = ?,
                               content_digest = ?, detail_json = ?,
                               last_observed_at = ?, last_retrieved_at = ?,
                               last_recorded_at = ?,
                               observation_count = observation_count + 1
                         WHERE id = ?
                        """,
                        (
                            float(probability_pct), raw_probability_pct,
                            normalized_probability_pct, source_observed_at,
                            quality_status, freshness_status,
                            digest, detail_json, observed_at, retrieved_at, recorded_iso,
                            latest["id"],
                        ),
                    )
                    return "revised"

                if (
                    latest is not None
                    and latest["observed_at"] < observed_at <= latest["last_observed_at"]
                ):
                    # The instant falls inside the current episode's coverage
                    # (strictly after its start): the observation is a replay
                    # or a contradicting correction of the covered span.
                    if latest["content_digest"] == digest:
                        conn.execute(
                            """
                            UPDATE fedwatch_probability_observations
                               SET last_retrieved_at = MAX(last_retrieved_at, ?),
                                   last_recorded_at = MAX(last_recorded_at, ?),
                                   observation_count = observation_count + 1
                             WHERE id = ?
                            """,
                            (retrieved_at, recorded_iso, latest["id"]),
                        )
                        return "duplicate"
                    # A contradicting value at an instant the old value still
                    # covered: end the old episode at this instant (never
                    # overlapping beyond it) and open the new one. Analytics
                    # resolves the shared instant toward the later episode.
                    conn.execute(
                        """
                        UPDATE fedwatch_probability_observations
                           SET last_observed_at = ?
                         WHERE id = ?
                        """,
                        (observed_at, latest["id"]),
                    )

                if latest is not None and observed_at > latest["last_observed_at"]:
                    gap_seconds = (
                        timeutil.parse_iso_z(observed_at)
                        - timeutil.parse_iso_z(latest["last_observed_at"])
                    ).total_seconds()
                    if (
                        latest["content_digest"] == digest
                        and gap_seconds <= EPISODE_CONTINUITY_MAX_GAP_SECONDS
                    ):
                        conn.execute(
                            """
                            UPDATE fedwatch_probability_observations
                               SET last_observed_at = ?, last_retrieved_at = ?,
                                   last_recorded_at = ?,
                                   observation_count = observation_count + 1
                             WHERE id = ?
                            """,
                            (observed_at, retrieved_at, recorded_iso, latest["id"]),
                        )
                        return "extended"

                existing = conn.execute(
                    """
                    SELECT * FROM fedwatch_probability_observations
                     WHERE meeting_date = ? AND source = ? AND method = ?
                       AND outcome_bp = ? AND open_ended = ? AND observed_at = ?
                    """,
                    (*series, observed_at),
                ).fetchone()
                if existing is not None:
                    if existing["content_digest"] == digest:
                        conn.execute(
                            """
                            UPDATE fedwatch_probability_observations
                               SET last_observed_at = MAX(last_observed_at, ?),
                                   last_retrieved_at = MAX(last_retrieved_at, ?),
                                   last_recorded_at = MAX(last_recorded_at, ?),
                                   observation_count = observation_count + 1
                             WHERE id = ?
                            """,
                            (observed_at, retrieved_at, recorded_iso, existing["id"]),
                        )
                        return "duplicate"
                    conn.execute(
                        """
                        UPDATE fedwatch_probability_observations
                           SET probability_pct = ?, raw_probability_pct = ?,
                               normalized_probability_pct = ?, source_observed_at = ?,
                               quality_status = ?, freshness_status = ?,
                               content_digest = ?, detail_json = ?,
                               last_observed_at = ?, last_retrieved_at = ?,
                               last_recorded_at = ?,
                               observation_count = observation_count + 1
                         WHERE id = ?
                        """,
                        (
                            float(probability_pct), raw_probability_pct,
                            normalized_probability_pct, source_observed_at,
                            quality_status, freshness_status,
                            digest, detail_json, observed_at, retrieved_at, recorded_iso,
                            existing["id"],
                        ),
                    )
                    return "revised"

                conn.execute(
                    """
                    INSERT INTO fedwatch_probability_observations
                        (meeting_date, source, method, outcome_bp, open_ended,
                         probability_pct, raw_probability_pct, normalized_probability_pct,
                         observed_at, source_observed_at, retrieved_at, recorded_at,
                         last_observed_at, last_retrieved_at, last_recorded_at,
                         quality_status, freshness_status, content_digest,
                         observation_count, detail_json)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?)
                    """,
                    (
                        meeting_date, source, method, int(outcome_bp), open_ended_flag,
                        float(probability_pct), raw_probability_pct, normalized_probability_pct,
                        observed_at, source_observed_at, retrieved_at, recorded_iso,
                        observed_at, retrieved_at, recorded_iso,
                        quality_status, freshness_status, digest, detail_json,
                    ),
                )
                return "inserted"
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
            "probability_pct": row["probability_pct"],
            "raw_probability_pct": row["raw_probability_pct"],
            "normalized_probability_pct": row["normalized_probability_pct"],
            "observed_at": row["observed_at"],
            "source_observed_at": row["source_observed_at"],
            "retrieved_at": row["retrieved_at"],
            "recorded_at": row["recorded_at"],
            "last_observed_at": row["last_observed_at"],
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
            "SELECT meeting_date, source, method, COUNT(*) AS episodes, "
            "SUM(observation_count) AS retrievals, MIN(observed_at) AS first_observed_at, "
            "MAX(last_observed_at) AS last_observed_at "
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
