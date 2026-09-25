# CFTC (Commodity Futures Trading Commission) Data Wrapper
# Modular, fault-tolerant design - each endpoint works independently
# Focus on Commitment of Traders (COT) reports for market sentiment analysis

import sys
import json
import math
import re
import csv
import io
import os
import sqlite3
import zipfile
import threading
import requests
import pandas as pd
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Dict, Any, Iterable, List, Optional, Tuple, Union
from datetime import datetime, timedelta, timezone
import traceback


class CFTCError:
    """Custom error class for CFTC API errors"""
    def __init__(self, endpoint: str, error: str, status_code: Optional[int] = None):
        self.endpoint = endpoint
        self.error = error
        self.status_code = status_code
        self.timestamp = int(datetime.now().timestamp())

    def to_dict(self) -> Dict[str, Any]:
        return {
            "endpoint": self.endpoint,
            "error": self.error,
            "status_code": self.status_code,
            "timestamp": self.timestamp,
            "type": "CFTCError"
        }


class CFTCAnnualNotPublished(Exception):
    """The official annual file for this year does not exist (HTTP 404)."""


class CftcCotArchive:
    """Durable local canonical CFTC observation store (SQLite).

    The archive is the convergence point between the historical annual ZIP
    ingestion path and the current Socrata retrieval path: both write the exact
    canonical row shape the C++ COT analytical layer already consumes, keyed by
    (report_date, contract_code, report_family, report_basis). A repeated or
    overlapping ingestion is therefore idempotent, the first writer's
    provenance stays recoverable, and a later official revision of an
    already-stored report is adopted instead of silently ignored.
    """

    _SCHEMA = (
        """
        CREATE TABLE IF NOT EXISTS cot_observations (
            report_date TEXT NOT NULL,
            contract_code TEXT NOT NULL,
            report_family TEXT NOT NULL,
            report_basis TEXT NOT NULL,
            market_and_exchange_names TEXT,
            contract_units TEXT,
            futonly_or_combined TEXT NOT NULL,
            row_json TEXT NOT NULL,
            first_source TEXT NOT NULL,
            first_retrieval_time TEXT NOT NULL,
            last_source TEXT NOT NULL,
            last_retrieval_time TEXT NOT NULL,
            PRIMARY KEY (report_date, contract_code, report_family, report_basis)
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS cot_ingest_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            started_at TEXT NOT NULL,
            finished_at TEXT,
            command TEXT NOT NULL,
            source_detail TEXT,
            report_family TEXT,
            report_basis TEXT,
            years TEXT,
            status TEXT NOT NULL,
            rows_inserted INTEGER NOT NULL DEFAULT 0,
            rows_updated INTEGER NOT NULL DEFAULT 0,
            rows_rejected INTEGER NOT NULL DEFAULT 0,
            detail_json TEXT
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_cot_obs_lookup "
        "ON cot_observations (report_family, report_basis, contract_code, report_date)",
        """
        CREATE TABLE IF NOT EXISTS cot_contract_state (
            report_family TEXT NOT NULL,
            report_basis TEXT NOT NULL,
            contract_code TEXT NOT NULL,
            full_history_fetched_at TEXT NOT NULL,
            PRIMARY KEY (report_family, report_basis, contract_code)
        )
        """,
    )

    def __init__(self, path: Union[str, Path]):
        self.path = Path(path)

    # Display-name metadata the annual files do not publish. They stay in the
    # stored canonical row but never decide whether a row changed.
    _COMPARISON_EXCLUDED_KEYS = frozenset({"commodity", "contract_market_name"})

    def _connect(self) -> sqlite3.Connection:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        conn = sqlite3.connect(str(self.path), timeout=30)
        conn.row_factory = sqlite3.Row
        for statement in self._SCHEMA:
            conn.execute(statement)
        conn.commit()
        return conn

    def ensure_schema(self) -> None:
        self._connect().close()

    def upsert_observations(self, rows: List[Dict[str, Any]], report_family: str, report_basis: str,
                            source: str, retrieval_time: str) -> Tuple[int, int, int]:
        """Insert new canonical rows and update changed ones.

        Returns (inserted, updated, unchanged). Repeating an identical
        ingestion is a no-op that touches nothing; a changed analytical value
        for an already-stored report is adopted and counted as updated.

        Change detection ignores the two display-name metadata columns
        (`commodity`, `contract_market_name`): the annual files do not publish
        them, so comparing them would make the two ingestion paths rewrite each
        other's rows forever without any analytical change. The stored row and
        the first/last writer provenance are kept as written; a later writer
        whose analytical fields are identical does not churn them.
        """
        inserted = updated = unchanged = 0
        if not rows:
            return inserted, updated, unchanged
        # Collapse duplicate primary keys inside one batch. Identical duplicates
        # are harmless; two different observations for the same report would
        # otherwise abort the whole transaction with a UNIQUE violation.
        deduped: Dict[Tuple[str, str], Dict[str, Any]] = {}
        for row in rows:
            key = (row.get("report_date_as_yyyy_mm_dd"),
                   str(row.get("cftc_contract_market_code") or ""))
            previous = deduped.get(key)
            if previous is not None and self._comparison_key(previous) != self._comparison_key(row):
                raise ValueError(
                    f"two different observations for report {key[0]} contract {key[1]}")
            deduped[key] = row
        rows = list(deduped.values())
        conn = self._connect()
        try:
            conn.execute("BEGIN")
            codes = sorted({str(row.get("cftc_contract_market_code") or "") for row in rows})
            dates = [row.get("report_date_as_yyyy_mm_dd") for row in rows
                     if row.get("report_date_as_yyyy_mm_dd")]
            existing: Dict[Tuple[str, str], str] = {}
            if codes and dates:
                placeholders = ",".join("?" for _ in codes)
                for record in conn.execute(
                        f"SELECT report_date, contract_code, row_json FROM cot_observations "
                        f"WHERE report_family = ? AND report_basis = ? AND report_date BETWEEN ? AND ? "
                        f"AND contract_code IN ({placeholders})",
                        [report_family, report_basis, min(dates), max(dates), *codes]):
                    existing[(record["report_date"], record["contract_code"])] = self._comparison_key(
                        json.loads(record["row_json"]))
            for row in rows:
                payload = json.dumps(row, sort_keys=True, separators=(",", ":"))
                comparison = self._comparison_key(row)
                date = row.get("report_date_as_yyyy_mm_dd")
                code = str(row.get("cftc_contract_market_code") or "")
                name = row.get("market_and_exchange_names")
                units = row.get("contract_units")
                basis_text = row.get("futonly_or_combined") or ""
                if (date, code) in existing:
                    if existing[(date, code)] == comparison:
                        unchanged += 1
                        continue
                    conn.execute(
                        """
                        UPDATE cot_observations
                           SET row_json = ?, last_source = ?, last_retrieval_time = ?
                         WHERE report_date = ? AND contract_code = ? AND report_family = ? AND report_basis = ?
                        """,
                        (payload, source, retrieval_time, date, code, report_family, report_basis),
                    )
                    updated += 1
                    continue
                conn.execute(
                    """
                    INSERT INTO cot_observations
                        (report_date, contract_code, report_family, report_basis,
                         market_and_exchange_names, contract_units, futonly_or_combined,
                         row_json, first_source, first_retrieval_time,
                         last_source, last_retrieval_time)
                    VALUES (?,?,?,?,?,?,?,?,?,?,?,?)
                    """,
                    (date, code, report_family, report_basis, name, units, basis_text, payload,
                     source, retrieval_time, source, retrieval_time),
                )
                inserted += 1
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()
        return inserted, updated, unchanged

    @staticmethod
    def _comparison_key(row: Dict[str, Any]) -> str:
        """The analytical row identity used for revision detection.

        The display-only Socrata metadata columns are excluded here, not from
        storage: they stay preserved verbatim in `row_json`.
        """
        return json.dumps(
            {key: value for key, value in row.items()
             if key not in CftcCotArchive._COMPARISON_EXCLUDED_KEYS},
            sort_keys=True, separators=(",", ":"))

    def observations(self, report_family: str, report_basis: str, contract_code: str,
                     limit: Optional[int] = None) -> List[Dict[str, Any]]:
        """The newest `limit` canonical rows for one contract, ascending."""
        conn = self._connect()
        try:
            sql = ("SELECT row_json FROM cot_observations "
                   "WHERE report_family = ? AND report_basis = ? AND contract_code = ? "
                   "ORDER BY report_date DESC")
            params: List[Any] = [report_family, report_basis, contract_code]
            if limit:
                sql += " LIMIT ?"
                params.append(int(limit))
            rows = [json.loads(record["row_json"]) for record in conn.execute(sql, params)]
        finally:
            conn.close()
        rows.reverse()
        return rows

    def latest_report_date(self, report_family: str, report_basis: str, contract_code: str) -> Optional[str]:
        conn = self._connect()
        try:
            row = conn.execute(
                "SELECT MAX(report_date) AS latest FROM cot_observations "
                "WHERE report_family = ? AND report_basis = ? AND contract_code = ?",
                (report_family, report_basis, contract_code),
            ).fetchone()
            return row["latest"] if row and row["latest"] else None
        finally:
            conn.close()

    def count_observations(self, report_family: str, report_basis: str, contract_code: str) -> int:
        conn = self._connect()
        try:
            row = conn.execute(
                "SELECT COUNT(*) AS rows FROM cot_observations "
                "WHERE report_family = ? AND report_basis = ? AND contract_code = ?",
                (report_family, report_basis, contract_code),
            ).fetchone()
            return int(row["rows"]) if row else 0
        finally:
            conn.close()

    def full_history_at(self, report_family: str, report_basis: str, contract_code: str) -> Optional[str]:
        """When the complete provider history for this contract was last read.

        A bounded backfill does not claim completeness, so the monitor performs
        one full-history fetch per contract and only then treats later scans as
        increments. This keeps the monitor's trailing references identical to
        the detailed workspace without re-downloading every contract each scan.
        """
        conn = self._connect()
        try:
            row = conn.execute(
                "SELECT full_history_fetched_at FROM cot_contract_state "
                "WHERE report_family = ? AND report_basis = ? AND contract_code = ?",
                (report_family, report_basis, contract_code),
            ).fetchone()
            return row["full_history_fetched_at"] if row else None
        finally:
            conn.close()

    def mark_full_history(self, report_family: str, report_basis: str, contract_code: str) -> None:
        conn = self._connect()
        try:
            conn.execute(
                "INSERT INTO cot_contract_state "
                "(report_family, report_basis, contract_code, full_history_fetched_at) "
                "VALUES (?,?,?,?) "
                "ON CONFLICT(report_family, report_basis, contract_code) "
                "DO UPDATE SET full_history_fetched_at = excluded.full_history_fetched_at",
                (report_family, report_basis, contract_code,
                 datetime.now(timezone.utc).isoformat(timespec="seconds")),
            )
            conn.commit()
        finally:
            conn.close()

    def coverage(self) -> Dict[str, Any]:
        """Per (family, basis) row counts and date ranges, plus the total."""
        conn = self._connect()
        try:
            groups = []
            for row in conn.execute(
                "SELECT report_family, report_basis, COUNT(*) AS rows, "
                "MIN(report_date) AS first_report_date, MAX(report_date) AS last_report_date "
                "FROM cot_observations GROUP BY report_family, report_basis "
                "ORDER BY report_family, report_basis"
            ):
                groups.append({
                    "report_family": row["report_family"],
                    "report_basis": row["report_basis"],
                    "rows": row["rows"],
                    "first_report_date": row["first_report_date"],
                    "last_report_date": row["last_report_date"],
                })
            total = conn.execute("SELECT COUNT(*) AS rows FROM cot_observations").fetchone()["rows"]
            years = []
            for row in conn.execute(
                "SELECT years, status, rows_inserted, rows_updated, rows_rejected, finished_at, "
                "source_detail, detail_json FROM cot_ingest_runs WHERE command = 'cot_backfill' "
                "ORDER BY id DESC LIMIT 400"
            ):
                entry = dict(row)
                try:
                    entry["detail"] = json.loads(entry.pop("detail_json") or "{}")
                except (TypeError, json.JSONDecodeError):
                    entry["detail"] = {}
                years.append(entry)
        finally:
            conn.close()
        return {
            "path": str(self.path),
            "exists": self.path.exists(),
            "total_observations": total,
            "groups": groups,
            "recent_backfill_runs": years,
        }

    def record_run(self, command: str, source_detail: str, report_family: Optional[str], report_basis: Optional[str],
                   years: Optional[str], status: str, rows_inserted: int = 0, rows_updated: int = 0,
                   rows_rejected: int = 0, detail: Optional[Dict[str, Any]] = None,
                   started_at: Optional[str] = None) -> None:
        now = datetime.now(timezone.utc).isoformat(timespec="seconds")
        conn = self._connect()
        try:
            conn.execute(
                "INSERT INTO cot_ingest_runs "
                "(started_at, finished_at, command, source_detail, report_family, report_basis, years, status, "
                " rows_inserted, rows_updated, rows_rejected, detail_json) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (started_at or now, now, command, source_detail, report_family, report_basis, years, status,
                 int(rows_inserted), int(rows_updated), int(rows_rejected),
                 json.dumps(detail or {}, sort_keys=True)),
            )
            conn.commit()
        finally:
            conn.close()


class CFTCDataWrapper:
    """Modular CFTC data wrapper with fault-tolerant endpoints"""

    def __init__(self, app_token: Optional[str] = None, archive_dir: Optional[str] = None):
        self.base_url = "https://publicreporting.cftc.gov"
        self.app_token = app_token or os.environ.get('CFTC_APP_TOKEN', '')
        self.archive_dir_override = archive_dir
        self.session = requests.Session()
        self.session.headers.update({
            'User-Agent': 'MarketLab Terminal - local research build (no contact email)',
            'Accept': 'application/json'
        })
        # The monitor fetches one page per market; a session shared across the
        # worker threads is not safe, so each thread gets its own.
        self._thread_local = threading.local()

        # Report type mappings
        self.reports_dict = {
            "legacy_futures_only": "6dca-aqww",
            "legacy_combined": "jun7-fc8e",
            "disaggregated_futures_only": "72hh-3qpy",
            "disaggregated_combined": "kh3c-gbw2",
            "tff_futures_only": "gpe5-46if",
            "tff_combined": "yw9f-hn96",
            "supplemental": "4zgm-a668",
        }

        # Report type descriptions
        self.report_descriptions = {
            "legacy": "Legacy reports with commercial, non-commercial and non-reportable classifications",
            "disaggregated": "Disaggregated reports with Producer/Merchant, Swap Dealers, Managed Money classifications",
            "financial": "Traders in Financial Futures (TFF) reports for financial contracts",
            "supplemental": "Supplemental reports for additional market information"
        }

        # Sample COT contract codes (curated from the full list)
        self.cot_codes = {
            # Agricultural
            "corn": "002602",
            "wheat": "001602",
            "soybeans": "005602",
            "cotton": "033661",
            "cocoa": "073732",
            "coffee": "083731",
            "sugar": "080732",
            "live_cattle": "057642",
            "lean_hogs": "054642",

            # Energy
            "crude_oil": "067651",
            # 02365B now belongs to a defunct Permian basis swap; the NYMEX
            # Henry Hub natural gas contract reports under 023651.
            "natural_gas": "023651",
            "gasoline": "111659",
            "heating_oil": "022651",

            # Metals
            "gold": "088691",
            "silver": "084691",
            "copper": "085692",
            "platinum": "076651",
            "palladium": "075651",

            # Financial
            "euro": "099741",
            "jpy": "097741",
            "british_pound": "096742",
            "swiss_franc": "092741",
            "canadian_dollar": "090741",
            "australian_dollar": "232741",

            # Index Futures
            "s&p_500": "13874A",
            "nasdaq_100": "209742",
            "dow_jones": "124603",
            # 240741 (USD-denominated Nikkei) stopped reporting on 2026-03-03.
            # 240743 is the yen-denominated CME Nikkei 225 contract, which still
            # reports and is the contract the NIY=F price proxy trades.
            "nikkei": "240743",
            "vix": "1170E1",

            # Interest Rates
            "treasury_bonds": "020601",
            "treasury_notes_2y": "042601",
            "treasury_notes_5y": "044601",
            "treasury_notes_10y": "043602",
            "fed_funds": "045601",

            # Crypto
            "bitcoin": "133741",
            "ether": "146021",

            # Other
            "us_dollar_index": "098662"
        }

        # Common market codes and names for search
        self.market_mappings = {
            "gold": ["GOLD", "088691"],
            "silver": ["SILVER", "084691"],
            "crude": ["CRUDE OIL", "067651"],
            "wti": ["WTI-PHYSICAL", "067651"],
            "brent": ["BRENT LAST DAY", "06765T"],
            "natural_gas": ["NAT GAS NYME", "023651"],
            "corn": ["CORN", "002602"],
            "wheat": ["WHEAT-SRW", "001602"],
            "s&p": ["E-MINI S&P 500", "13874A"],
            "sp500": ["E-MINI S&P 500", "13874A"],
            "nasdaq": ["NASDAQ MINI", "209742"],
            "bitcoin": ["BITCOIN", "133741"],
            "btc": ["BITCOIN", "133741"],
            "ether": ["ETHER CASH SETTLED", "146021"],
            "eth": ["ETHER CASH SETTLED", "146021"],
            "euro": ["EURO FX", "099741"],
            "yen": ["JAPANESE YEN", "097741"],
            "pound": ["BRITISH POUND", "096742"],
            "vix": ["VIX FUTURES", "1170E1"],
            "treasury": ["UST BOND", "020601"],
            "dollar": ["USD INDEX", "098662"]
        }

    def _make_request(self, url: str) -> Dict[str, Any]:
        """Make HTTP request with proper error handling"""
        try:
            # Add app token if available
            if self.app_token:
                separator = "&" if "?" in url else "?"
                url = f"{url}{separator}$$app_token={self.app_token}"

            response = self.session.get(url, timeout=30)
            response.raise_for_status()

            data = response.json()

            # Check for API errors
            if isinstance(data, dict) and "error" in data:
                raise Exception(f"CFTC API error: {data['error']}")

            return data

        except requests.exceptions.RequestException as e:
            raise Exception(f"HTTP request failed: {str(e)}")
        except json.JSONDecodeError as e:
            raise Exception(f"JSON decode error: {str(e)}")

    def _format_date(self, date_str: str) -> str:
        """Format date string to CFTC format (YYYY-MM-DD)"""
        try:
            if not date_str:
                return ""

            # Handle different date formats
            if '-' in date_str:
                # Already in YYYY-MM-DD format
                return date_str
            elif len(date_str) == 8 and date_str.isdigit():
                # Convert YYYYMMDD to YYYY-MM-DD
                return f"{date_str[:4]}-{date_str[4:6]}-{date_str[6:8]}"
            else:
                # Try to parse and format
                date_obj = pd.to_datetime(date_str)
                return date_obj.strftime('%Y-%m-%d')

        except:
            return date_str

    @staticmethod
    def _coerce_number(value):
        """Socrata returns some numeric columns as strings (sometimes
        comma-grouped). Convert a parseable string to int/float and leave
        anything else — including None — exactly as it arrived, so a missing
        cell can never be mistaken for a zero."""
        if not isinstance(value, str):
            return value
        text = value.strip().replace(',', '')
        if not text:
            return value
        try:
            return int(text)
        except ValueError:
            pass
        try:
            return float(text)
        except ValueError:
            return value

    @staticmethod
    def _to_int(value) -> Optional[int]:
        """Numeric cell -> int, or None when the cell is absent/non-numeric.
        Used by the derived views, which must distinguish a genuine zero from a
        field the report did not carry."""
        if value is None or isinstance(value, bool):
            return None
        if isinstance(value, (int, float)):
            return int(value)
        if isinstance(value, str):
            text = value.strip().replace(',', '')
            if not text:
                return None
            try:
                return int(float(text))
            except ValueError:
                return None
        return None

    @staticmethod
    def _pick(record: Dict[str, Any], names: tuple) -> Optional[int]:
        """First present, non-null alias in `names` -> int, else None."""
        for name in names:
            if name in record and record[name] is not None:
                parsed = CFTCDataWrapper._to_int(record[name])
                if parsed is not None:
                    return parsed
        return None

    @staticmethod
    def _pick_number(record: Dict[str, Any], names: tuple) -> Optional[float]:
        """First present, numeric alias in `names` -> float, else None.

        Concentration cells are decimals ("12.5"), so the integer coercion
        used for position counts would truncate them. A non-numeric or absent
        cell stays None; it never becomes 0."""
        for name in names:
            if name not in record or record[name] is None:
                continue
            value = record[name]
            if isinstance(value, bool):
                continue
            if isinstance(value, (int, float)):
                parsed = float(value)
                if math.isfinite(parsed):
                    return parsed
                continue
            if isinstance(value, str):
                text = value.strip().replace(',', '')
                if not text:
                    continue
                try:
                    parsed = float(text)
                except ValueError:
                    continue
                if not math.isfinite(parsed):
                    continue
                return parsed
        return None

    @staticmethod
    def _report_family(report_type: Optional[str]) -> str:
        """Normalise the CLI/report label to the CFTC report family name."""
        key = (report_type or "legacy").lower()
        if key.startswith("financial"):
            return "tff"
        if key.startswith("tff"):
            return "tff"
        if key.startswith("disagg"):
            return "disaggregated"
        if key.startswith("legacy"):
            return "legacy"
        return key

    # Identifier/metadata fields keep their exact provider representation (a
    # contract-market code such as "088691" or "13874A" is an identifier, not
    # a number); only measured quantities are normalised from string form.
    _STRING_METADATA_KEYS = frozenset({
        "id",
        "report_date_as_yyyy_mm_dd",
        "market_and_exchange_names",
        "commodity",
        "commodity_name",
        "commodity_group_name",
        "commodity_subgroup_name",
        "contract_units",
        "futonly_or_combined",
        "cftc_contract_market_code",
        "cftc_market_code",
        "cftc_commodity_code",
        "cftc_region_code",
    })

    # The participant fields the derived views read, per report family.
    #
    # The commercial / non-commercial split is the CFTC's own Legacy
    # classification. The sentiment and position-summary views (dormant CLI
    # commands; the R3 workspace uses cot_history only) therefore serve the
    # Legacy family only: relabelling Disaggregated Producer/Merchant or Managed
    # Money as "commercial"/"non-commercial", or reconstructing TFF into that
    # split, would mislabel CFTC categories. The trend view keeps each family's
    # own vocabulary (Legacy commercial / non-commercial, Disaggregated
    # producer-merchant / managed-money) and still refuses TFF.
    _POSITION_FIELDS = {
        "legacy": {
            "commercial_long": ("comm_positions_long_all", "comm_long_all"),
            "commercial_short": ("comm_positions_short_all", "comm_short_all"),
            "non_commercial_long": ("noncomm_positions_long_all", "noncomm_long_all"),
            "non_commercial_short": ("noncomm_positions_short_all", "noncomm_short_all"),
            "non_reportable_long": ("nonrept_positions_long_all",),
            "non_reportable_short": ("nonrept_positions_short_all",),
        },
        "disaggregated": {
            "commercial_long": ("prod_merc_positions_long", "prod_merc_positions_long_all"),
            "commercial_short": ("prod_merc_positions_short", "prod_merc_positions_short_all"),
            "non_commercial_long": ("m_money_positions_long_all", "m_money_positions_long"),
            "non_commercial_short": ("m_money_positions_short_all", "m_money_positions_short"),
            "non_reportable_long": ("nonrept_positions_long_all",),
            "non_reportable_short": ("nonrept_positions_short_all",),
        },
    }

    # Full participant map per report family, used by the R3 workspace history
    # command. Each entry is (key, long aliases, short aliases); the keys are
    # stable analytical identifiers, NOT the provider's field names, and the
    # C++ services/economics/CftcMetricModel.h mapping uses the same keys.
    # Names come from the real Socrata resources (verified 2026-09): the
    # disaggregated swap-dealer SHORT field really is
    # "swap__positions_short_all" (double underscore), while the long side is
    # "swap_positions_long_all".
    _PARTICIPANT_FIELDS = {
        "legacy": (
            ("commercial", ("comm_positions_long_all",), ("comm_positions_short_all",)),
            ("non_commercial", ("noncomm_positions_long_all",), ("noncomm_positions_short_all",)),
            ("non_reportable", ("nonrept_positions_long_all",), ("nonrept_positions_short_all",)),
        ),
        "disaggregated": (
            ("producer_merchant",
             ("prod_merc_positions_long", "prod_merc_positions_long_all"),
             ("prod_merc_positions_short", "prod_merc_positions_short_all")),
            ("swap_dealer",
             ("swap_positions_long_all",),
             ("swap__positions_short_all", "swap_positions_short_all")),
            ("managed_money", ("m_money_positions_long_all",), ("m_money_positions_short_all",)),
            ("other_reportable", ("other_rept_positions_long",), ("other_rept_positions_short",)),
            ("non_reportable", ("nonrept_positions_long_all",), ("nonrept_positions_short_all",)),
        ),
        "tff": (
            ("dealer", ("dealer_positions_long_all",), ("dealer_positions_short_all",)),
            ("asset_manager", ("asset_mgr_positions_long",), ("asset_mgr_positions_short",)),
            ("leveraged_funds", ("lev_money_positions_long",), ("lev_money_positions_short",)),
            ("other_reportable", ("other_rept_positions_long",), ("other_rept_positions_short",)),
            ("non_reportable", ("nonrept_positions_long_all",), ("nonrept_positions_short_all",)),
        ),
    }

    # Identifier/metadata columns the workspace history keeps verbatim.
    _HISTORY_METADATA_KEYS = (
        "market_and_exchange_names",
        "contract_market_name",
        "cftc_contract_market_code",
        "commodity",
        "contract_units",
        "futonly_or_combined",
    )

    # Provider trader-count fields the workspace history retains. All three
    # authoritative report families (legacy, disaggregated and financial/TFF)
    # publish these exact columns (verified against the live Socrata resources
    # 2026-09). They are counts of reportable traders, not derived values.
    _TRADER_CONTEXT_FIELDS = (
        ("traders_total", ("traders_tot_all",)),
        ("traders_reportable_long", ("traders_tot_rept_long_all",)),
        ("traders_reportable_short", ("traders_tot_rept_short_all",)),
    )

    # Provider 4-/8-trader concentration percentages (gross and net, per
    # side), published by all three families under the same column names.
    # These are decimals, so they use _pick_number rather than _pick.
    _CONCENTRATION_FIELDS = (
        ("concentration_gross_4_long", ("conc_gross_le_4_tdr_long",)),
        ("concentration_gross_4_short", ("conc_gross_le_4_tdr_short",)),
        ("concentration_gross_8_long", ("conc_gross_le_8_tdr_long",)),
        ("concentration_gross_8_short", ("conc_gross_le_8_tdr_short",)),
        ("concentration_net_4_long", ("conc_net_le_4_tdr_long_all",)),
        ("concentration_net_4_short", ("conc_net_le_4_tdr_short_all",)),
        ("concentration_net_8_long", ("conc_net_le_8_tdr_long_all",)),
        ("concentration_net_8_short", ("conc_net_le_8_tdr_short_all",)),
    )

    # Participant spreading positions actually published by the current
    # Socrata resources, per family. Legacy publishes non-commercial spreading
    # only; Producer/Merchant and Non-Reportable never carry a spread column.
    # A participant without a published column stores None, never 0.
    _SPREAD_FIELDS = {
        "legacy": {
            "non_commercial": ("noncomm_postions_spread_all", "noncomm_positions_spread"),
        },
        "disaggregated": {
            "swap_dealer": ("swap__positions_spread_all",),
            "managed_money": ("m_money_positions_spread", "m_money_positions_spread_1"),
            "other_reportable": ("other_rept_positions_spread", "other_rept_positions_spread_1"),
        },
        "tff": {
            "dealer": ("dealer_positions_spread_all",),
            "asset_manager": ("asset_mgr_positions_spread",),
            "leveraged_funds": ("lev_money_positions_spread",),
            "other_reportable": ("other_rept_positions_spread",),
        },
    }

    # Per-participant reportable-trader counts actually published, per family:
    # (long aliases, short aliases, spread aliases or None). Field names were
    # verified against all six resources (2026-09); e.g. the disaggregated
    # Other Reportable short count really is `traders_other_rept_short` (no
    # `_all` suffix) while Producer/Merchant is `traders_prod_merc_long_all`.
    _PARTICIPANT_TRADER_FIELDS = {
        "legacy": {
            "commercial": (("traders_comm_long_all",), ("traders_comm_short_all",), None),
            "non_commercial": (("traders_noncomm_long_all",), ("traders_noncomm_short_all",),
                               ("traders_noncomm_spread_all",)),
        },
        "disaggregated": {
            "producer_merchant": (("traders_prod_merc_long_all",), ("traders_prod_merc_short_all",), None),
            "swap_dealer": (("traders_swap_long_all",), ("traders_swap_short_all",),
                            ("traders_swap_spread_all",)),
            "managed_money": (("traders_m_money_long_all",), ("traders_m_money_short_all",),
                              ("traders_m_money_spread_all",)),
            "other_reportable": (("traders_other_rept_long_all",), ("traders_other_rept_short",),
                                 ("traders_other_rept_spread",)),
        },
        "tff": {
            "dealer": (("traders_dealer_long_all",), ("traders_dealer_short_all",),
                       ("traders_dealer_spread_all",)),
            "asset_manager": (("traders_asset_mgr_long_all",), ("traders_asset_mgr_short_all",),
                              ("traders_asset_mgr_spread",)),
            "leveraged_funds": (("traders_lev_money_long_all",), ("traders_lev_money_short_all",),
                                ("traders_lev_money_spread",)),
            "other_reportable": (("traders_other_rept_long_all",), ("traders_other_rept_short",),
                                 ("traders_other_rept_spread",)),
        },
    }

    # ── Official CFTC annual historical files ───────────────────────────────
    #
    # The annual files are the historical bootstrap path. Each (family, basis)
    # pair has one stable URL pattern and one text entry inside the ZIP; the
    # entry is chosen by exact name where the archive documents one, and by the
    # single .txt entry otherwise. These are the same official files the CFTC
    # publishes on its Historical Compressed page; the live Socrata path is not
    # replaced by this one.
    ANNUAL_HISTORY_BASE = "https://www.cftc.gov/files/dea/history"
    ANNUAL_SOURCES = {
        ("legacy", True): ("deacot{year}.zip", ("annual.txt",)),
        ("legacy", False): ("deahistfo{year}.zip", ("annualof.txt",)),
        ("disaggregated", True): ("fut_disagg_txt_{year}.zip", ("f_year.txt",)),
        ("disaggregated", False): ("com_disagg_txt_{year}.zip", ("c_year.txt",)),
        ("tff", True): ("fut_fin_txt_{year}.zip", ("FinFutYY.txt",)),
        ("tff", False): ("com_fin_txt_{year}.zip", ("FinComYY.txt",)),
    }
    # The Legacy Combined per-year file was published as `deahistfo_YYYY.zip`
    # from 1995 through 2003 and as `deahistfoYYYY.zip` from 2004 on. The
    # alternate is attempted only when the primary is not published, and the
    # URL actually used (or last attempted) is what the run records.
    ANNUAL_ALTERNATE_SOURCES = {
        ("legacy", False): ("deahistfo_{year}.zip",),
    }
    # First year the official history exists for each (family, basis). Legacy
    # Futures Only starts in 1986 and Legacy Combined in 1995 (the combined
    # series begins with the 1995-03-21 report); the disaggregated and TFF
    # per-year files begin in 2010 -- their 2006-2016 combined archives are
    # deliberately not used (the 156-report reference window does not require
    # them, and mixing archives would ingest the same reports twice). A year
    # with no published file is recorded as `not_published`, never skipped.
    ANNUAL_FIRST_YEAR = {
        ("legacy", True): 1986,
        ("legacy", False): 1995,
        ("disaggregated", True): 2010,
        ("disaggregated", False): 2010,
        ("tff", True): 2010,
        ("tff", False): 2010,
    }

    # The monitor transports at most this many full canonical observations per
    # market; the engine's longest participant/OI reference is 156 prior
    # reports. The primary concentration field's unbounded trailing reference is
    # transported separately (see _monitor_transport_rows). The field name must
    # match the default primary_concentration_field in
    # services/economics/CftcInterpretationModel.h; the C++ monitor model
    # refuses a payload whose declared field differs.
    MONITOR_FULL_WINDOW = 200
    MONITOR_CONCENTRATION_FIELD = "concentration_gross_4_long"
    MONITOR_SUFFICIENT_POINTS = 157

    # Annual-file header aliases, normalised (whitespace collapsed, spaces
    # around "=" removed) before an exact match. Every alias maps to one of the
    # canonical keys services/economics/CftcMetricModel.h already consumes, so
    # the historical and current ingestion paths converge on the same rows.
    # Only the "(All)" legacy columns are accepted; "(Old)" and "(Other)" are
    # different published aggregates and must never be selected here.
    _ANNUAL_FIELD_ALIASES = {
        "report_date_as_yyyy_mm_dd": (
            "Report_Date_as_YYYY-MM-DD",
            "Report_Date_as_MM_DD_YYYY",
            "As_of_Date_In_Form_YYMMDD",
            "As of Date in Form YYYY-MM-DD",
            "As of Date in Form MM_DD_YYYY",
            "As of Date in Form YYMMDD",
        ),
        "cftc_contract_market_code": ("CFTC_Contract_Market_Code", "CFTC Contract Market Code"),
        "market_and_exchange_names": ("Market_and_Exchange_Names", "Market and Exchange Names"),
        "contract_units": ("Contract_Units", "Contract Units"),
        "open_interest_all": ("Open_Interest_All", "Open Interest (All)"),
        "traders_total": ("Traders_Tot_All", "Traders-Total (All)"),
        "traders_reportable_long": ("Traders_Tot_Rept_Long_All", "Traders-Total Reportable-Long (All)"),
        "traders_reportable_short": ("Traders_Tot_Rept_Short_All", "Traders-Total Reportable-Short (All)"),
        "concentration_gross_4_long": ("Conc_Gross_LE_4_TDR_Long_All", "Concentration-Gross LT=4 TDR-Long (All)"),
        "concentration_gross_4_short": ("Conc_Gross_LE_4_TDR_Short_All", "Concentration-Gross LT=4 TDR-Short (All)"),
        "concentration_gross_8_long": ("Conc_Gross_LE_8_TDR_Long_All", "Concentration-Gross LT=8 TDR-Long (All)"),
        "concentration_gross_8_short": ("Conc_Gross_LE_8_TDR_Short_All", "Concentration-Gross LT=8 TDR-Short (All)"),
        "concentration_net_4_long": ("Conc_Net_LE_4_TDR_Long_All", "Concentration-Net LT=4 TDR-Long (All)"),
        "concentration_net_4_short": ("Conc_Net_LE_4_TDR_Short_All", "Concentration-Net LT=4 TDR-Short (All)"),
        "concentration_net_8_long": ("Conc_Net_LE_8_TDR_Long_All", "Concentration-Net LT=8 TDR-Long (All)"),
        "concentration_net_8_short": ("Conc_Net_LE_8_TDR_Short_All", "Concentration-Net LT=8 TDR-Short (All)"),
    }

    # Annual participant-leg aliases per report family. The canonical keys are
    # the same analytical identifiers _PARTICIPANT_FIELDS serves from Socrata.
    _ANNUAL_LEG_ALIASES = {
        "legacy": {
            "commercial": (("Commercial_Positions_Long_All", "Commercial Positions-Long (All)",
                            "Comm_Positions_Long_All"),
                           ("Commercial_Positions_Short_All", "Commercial Positions-Short (All)",
                            "Comm_Positions_Short_All")),
            "non_commercial": (("Noncommercial_Positions_Long_All", "Noncommercial Positions-Long (All)",
                                "NonComm_Positions_Long_All"),
                               ("Noncommercial_Positions_Short_All", "Noncommercial Positions-Short (All)",
                                "NonComm_Positions_Short_All")),
            "non_reportable": (("Nonreportable_Positions_Long_All", "Nonreportable Positions-Long (All)",
                                "NonRept_Positions_Long_All"),
                               ("Nonreportable_Positions_Short_All", "Nonreportable Positions-Short (All)",
                                "NonRept_Positions_Short_All")),
        },
        "disaggregated": {
            "producer_merchant": (("Prod_Merc_Positions_Long_All",), ("Prod_Merc_Positions_Short_All",)),
            "swap_dealer": (("Swap_Positions_Long_All",),
                            ("Swap__Positions_Short_All", "Swap_Positions_Short_All")),
            "managed_money": (("M_Money_Positions_Long_All",), ("M_Money_Positions_Short_All",)),
            "other_reportable": (("Other_Rept_Positions_Long_All",), ("Other_Rept_Positions_Short_All",)),
            "non_reportable": (("NonRept_Positions_Long_All",), ("NonRept_Positions_Short_All",)),
        },
        "tff": {
            "dealer": (("Dealer_Positions_Long_All",), ("Dealer_Positions_Short_All",)),
            "asset_manager": (("Asset_Mgr_Positions_Long_All",), ("Asset_Mgr_Positions_Short_All",)),
            "leveraged_funds": (("Lev_Money_Positions_Long_All",), ("Lev_Money_Positions_Short_All",)),
            "other_reportable": (("Other_Rept_Positions_Long_All",), ("Other_Rept_Positions_Short_All",)),
            "non_reportable": (("NonRept_Positions_Long_All",), ("NonRept_Positions_Short_All",)),
        },
    }

    # Annual-file spread and per-participant trader-count aliases, keyed by
    # family and participant. Only columns present in the real official files
    # are listed (verified 2026-09 against the per-year ZIPs); a participant
    # without a published column stores None in both paths.
    _ANNUAL_SPREAD_ALIASES = {
        "legacy": {
            "non_commercial": ("Noncommercial Positions-Spreading (All)",),
        },
        "disaggregated": {
            "swap_dealer": ("Swap__Positions_Spread_All", "Swap_Positions_Spread_All"),
            "managed_money": ("M_Money_Positions_Spread_All",),
            "other_reportable": ("Other_Rept_Positions_Spread_All",),
        },
        "tff": {
            "dealer": ("Dealer_Positions_Spread_All",),
            "asset_manager": ("Asset_Mgr_Positions_Spread_All",),
            "leveraged_funds": ("Lev_Money_Positions_Spread_All",),
            "other_reportable": ("Other_Rept_Positions_Spread_All",),
        },
    }
    _ANNUAL_TRADER_ALIASES = {
        "legacy": {
            "commercial": (("Traders-Commercial-Long (All)",),
                           ("Traders-Commercial-Short (All)",), None),
            "non_commercial": (("Traders-Noncommercial-Long (All)",),
                               ("Traders-Noncommercial-Short (All)",),
                               ("Traders-Noncommercial-Spreading (All)",)),
        },
        "disaggregated": {
            "producer_merchant": (("Traders_Prod_Merc_Long_All",),
                                  ("Traders_Prod_Merc_Short_All",), None),
            "swap_dealer": (("Traders_Swap_Long_All",), ("Traders_Swap_Short_All",),
                            ("Traders_Swap_Spread_All",)),
            "managed_money": (("Traders_M_Money_Long_All",), ("Traders_M_Money_Short_All",),
                              ("Traders_M_Money_Spread_All",)),
            "other_reportable": (("Traders_Other_Rept_Long_All",),
                                 ("Traders_Other_Rept_Short_All",),
                                 ("Traders_Other_Rept_Spread_All",)),
        },
        "tff": {
            "dealer": (("Traders_Dealer_Long_All",), ("Traders_Dealer_Short_All",),
                       ("Traders_Dealer_Spread_All",)),
            "asset_manager": (("Traders_Asset_Mgr_Long_All",), ("Traders_Asset_Mgr_Short_All",),
                              ("Traders_Asset_Mgr_Spread_All",)),
            "leveraged_funds": (("Traders_Lev_Money_Long_All",), ("Traders_Lev_Money_Short_All",),
                                ("Traders_Lev_Money_Spread_All",)),
            "other_reportable": (("Traders_Other_Rept_Long_All",),
                                 ("Traders_Other_Rept_Short_All",),
                                 ("Traders_Other_Rept_Spread_All",)),
        },
    }

    @staticmethod
    def _normalize_annual_header(name: str) -> str:
        """Collapse whitespace and spaces around '=' so the small spelling
        differences between CFTC annual-file releases do not change a match."""
        text = re.sub(r"\s+", " ", str(name).strip())
        return re.sub(r"\s*=\s*", "=", text)

    def _build_search_query(self, identifier: str) -> str:
        """Build the `$where` fragment that selects one contract/market.

        A mapped identifier uses the CFTC contract-market code, which names one
        contract exactly; the previous free-text name match selected several
        contracts of the same commodity (GOLD, MICRO GOLD, ...) and let their
        rows be mixed into a single dated trend. Unmapped free text falls back
        to a wildcard search, and "all" selects everything."""
        if not identifier or identifier.lower() == "all":
            return ""

        raw = identifier.strip()
        key = raw.lower()

        # The panel's market keys all live in cot_codes. Resolving them there
        # first matters: a free-text fallback would treat "_" as a LIKE
        # wildcard ("crude_oil" matched every crude contract) and the aliases
        # in market_mappings cover only half the panel's keys.
        code = self.cot_codes.get(key)
        if code:
            return f"cftc_contract_market_code = '{code}'"

        mapped = self.market_mappings.get(key)
        if mapped:
            name, mapped_code = mapped
            if mapped_code:
                return f"cftc_contract_market_code = '{mapped_code}'"
            safe_name = name.replace("'", "")
            return f"UPPER(contract_market_name) like UPPER('%{safe_name}%')"

        if raw.isalnum() and len(raw) == 6:
            return f"cftc_contract_market_code = '{raw}'"
        if raw.isdigit():
            return f"cftc_contract_market_code = '{raw}'"

        safe = raw.replace("'", "").replace("_", " ")
        return (
            f"(UPPER(contract_market_name) like UPPER('%{safe}%') OR "
            f"UPPER(commodity) like UPPER('%{safe}%') OR "
            f"UPPER(cftc_contract_market_code) like UPPER('%{safe}%') OR "
            f"UPPER(commodity_group_name) like UPPER('%{safe}%') OR "
            f"UPPER(commodity_subgroup_name) like UPPER('%{safe}%'))"
        )

    # COMMITMENT OF TRADERS (COT) ENDPOINTS

    def get_cot_data(self, identifier: str = "all", report_type: str = "legacy",
                     futures_only: bool = False, start_date: Optional[str] = None,
                     end_date: Optional[str] = None, limit: Optional[int] = 1000) -> Dict[str, Any]:
        """Get Commitment of Traders (COT) data"""
        try:
            # Default dates. A weekly report is up to seven days old, so a
            # seven-day lookback can miss the most recent release entirely —
            # which is exactly what happened for every named market in the C++
            # panel. Any specific market/code therefore defaults to a year;
            # only the "all" sweep keeps the short window (it would otherwise
            # overflow the row limit).
            if not start_date:
                if identifier and identifier.lower() != "all":
                    start_date = (datetime.now() - timedelta(days=365)).strftime("%Y-%m-%d")
                else:
                    start_date = (datetime.now() - timedelta(days=7)).strftime("%Y-%m-%d")

            if not end_date:
                end_date = datetime.now().strftime("%Y-%m-%d")

            # Format dates
            start_formatted = self._format_date(start_date)
            end_formatted = self._format_date(end_date)

            # Build report type
            report_type_key = report_type.lower()
            if report_type_key == "financial":
                report_type_key = "tff"

            # Add futures_only/combined suffix
            if report_type_key != "supplemental":
                if futures_only:
                    report_type_key += "_futures_only"
                else:
                    report_type_key += "_combined"

            # Validate report type
            if report_type_key not in self.reports_dict:
                return {"error": CFTCError("cot_data", f"Invalid report type: {report_type}").to_dict()}

            # Build base URL
            resource_id = self.reports_dict[report_type_key]

            # Build the where clause
            where_clause = f"Report_Date_as_YYYY_MM_DD between '{start_formatted}' AND '{end_formatted}'"

            # Add search filter if identifier is provided
            search_query = self._build_search_query(identifier)
            if search_query:
                where_clause += f" AND ({search_query})"

            # URL encode the where clause
            import urllib.parse
            encoded_where = urllib.parse.quote(where_clause)

            base_url = f"{self.base_url}/resource/{resource_id}.json?$limit={limit}&$where={encoded_where}&$order=Report_Date_as_YYYY_MM_DD%20ASC"

            # Make request
            data = self._make_request(base_url)

            if not data:
                return {"error": CFTCError("cot_data", f"No COT data found for identifier: {identifier}").to_dict()}

            # Process and clean data
            processed_data = []
            for record in data:
                # Clean up record
                clean_record = {}
                for key, value in record.items():
                    # Convert keys to lowercase
                    clean_key = key.lower().replace("__", "_")

                    # Handle different data types
                    if isinstance(value, str):
                        if clean_key == "report_date_as_yyyy_mm_dd":
                            # Clean up date
                            clean_record[clean_key] = value.split("T")[0] if "T" in value else value
                        elif clean_key.startswith("pct_"):
                            # Convert percentage strings to decimal
                            try:
                                clean_record[clean_key] = float(value) / 100
                            except ValueError:
                                clean_record[clean_key] = value
                        else:
                            if clean_key in self._STRING_METADATA_KEYS:
                                # Preserve identifiers/metadata byte-for-byte.
                                clean_record[clean_key] = value
                            else:
                                # Socrata emits numeric columns as strings for
                                # some rows; normalise those so the derived
                                # arithmetic and the panel's statistics see
                                # numbers, not strings.
                                clean_record[clean_key] = self._coerce_number(value)
                    elif value is not None:
                        clean_record[clean_key] = value

                processed_data.append(clean_record)

            return {
                "success": True,
                "data": processed_data,
                "parameters": {
                    "identifier": identifier,
                    "report_type": report_type,
                    "report_family": self._report_family(report_type),
                    "futures_only": futures_only,
                    "start_date": start_formatted,
                    "end_date": end_formatted,
                    "limit": limit,
                    "source": self.base_url,
                    "dataset": resource_id,
                    "retrieved_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                }
            }

        except Exception as e:
            return {"error": CFTCError("cot_data", str(e)).to_dict()}

    def get_cot_history(self, identifier: str, report_type: str = "legacy",
                        futures_only: bool = False, max_rows: int = 20000,
                        page_size: int = 5000) -> Dict[str, Any]:
        """Full official weekly history for one contract and report family.

        This is the R3 workspace acquisition path. It is deliberately separate
        from get_cot_data (which keeps its bounded recent window for the older
        table/sentiment commands):

        * the whole authoritative Socrata resource for the requested family and
          futures-only/combined variant is read with offset pagination ordered
          by report date ascending (and the unique row id, so paging cannot
          skip or repeat a row); the returned rows are ascending;
        * only the provider fields the workspace needs are returned - open
          interest, the family's participant long/short fields, the provider's
          reportable-trader counts and 4-/8-trader concentration percentages,
          and the contract metadata - so a 40-year history stays a few hundred
          KB instead of a multi-MB raw dump;
        * participant keys are the analytical identifiers used by
          services/economics/CftcMetricModel.h; a missing cell stays None
          (JSON null), never 0;
        * hitting the acquisition cap is an error, not a silently truncated
          "success" - the caller must not plot a partial history as complete.
        """
        try:
            family = self._report_family(report_type)
            participants = self._PARTICIPANT_FIELDS.get(family)
            if participants is None:
                return {"error": CFTCError(
                    "cot_history",
                    f"Full history is not defined for the '{report_type}' report family. "
                    "Supported families are legacy, disaggregated and financial (TFF)."
                ).to_dict()}

            report_type_key = (report_type or "legacy").lower()
            if report_type_key == "financial":
                report_type_key = "tff"
            if report_type_key == "supplemental":
                return {"error": CFTCError(
                    "cot_history", "The supplemental report is not part of the R3 workspace.").to_dict()}
            suffix = "_futures_only" if futures_only else "_combined"
            resource_id = self.reports_dict.get(report_type_key + suffix)
            if not resource_id:
                return {"error": CFTCError(
                    "cot_history", f"Invalid report type: {report_type} (futures_only={futures_only})").to_dict()}

            search_query = self._build_search_query(identifier)
            if not search_query:
                return {"error": CFTCError(
                    "cot_history",
                    "A single contract is required; a sweep over every market is not a workspace history."
                ).to_dict()}

            if page_size < 1 or max_rows < 1:
                return {"error": CFTCError("cot_history", "max_rows and page_size must be positive").to_dict()}

            import urllib.parse

            records: List[Dict[str, Any]] = []
            offset = 0
            cap_hit = False
            where_clause = urllib.parse.quote(search_query)
            while len(records) < max_rows:
                limit = min(page_size, max_rows - len(records))
                page_url = (
                    f"{self.base_url}/resource/{resource_id}.json?"
                    f"$limit={limit}&$offset={offset}&$where={where_clause}"
                    f"&$order=report_date_as_yyyy_mm_dd,id%20ASC"
                )
                page = self._make_request(page_url)
                if page is None:
                    # A 200 whose body is not a JSON array is a provider
                    # failure; it must not terminate the walk as if the
                    # resource had simply ended.
                    return {"error": CFTCError(
                        "cot_history",
                        "The provider returned an unreadable page while reading the history."
                    ).to_dict()}
                if not page:
                    break
                records.extend(page)
                if len(page) < limit:
                    break
                offset += len(page)
                if offset >= max_rows:
                    cap_hit = True
                    break
            if cap_hit or len(records) >= max_rows:
                return {"error": CFTCError(
                    "cot_history",
                    f"History for this contract reached the acquisition cap of {max_rows} rows; "
                    "refusing to return a possibly truncated history."
                ).to_dict()}
            if not records:
                return {"error": CFTCError(
                    "cot_history", f"No COT history found for identifier: {identifier}").to_dict()}

            history: List[Dict[str, Any]] = [self._canonical_history_row(record, family) for record in records]

            history.sort(key=lambda item: item.get("report_date_as_yyyy_mm_dd") or "")

            return {
                "success": True,
                "data": history,
                "parameters": {
                    "identifier": identifier,
                    "report_type": report_type,
                    "report_family": family,
                    "futures_only": futures_only,
                    "source": self.base_url,
                    "dataset": resource_id,
                    "count": len(history),
                    "first_report_date": history[0].get("report_date_as_yyyy_mm_dd"),
                    "last_report_date": history[-1].get("report_date_as_yyyy_mm_dd"),
                    "retrieved_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                }
            }

        except Exception as e:
            return {"error": CFTCError("cot_history", str(e)).to_dict()}

    # ── Historical annual-file backfill and durable archive ─────────────────

    def _archive_path(self) -> Path:
        """Where the durable canonical archive lives.

        The host passes `FINCEPT_DATA_DIR` (the application profile root) to
        every provider script; the archive lives under `<root>/cftc`. Standalone
        runs (and tests) override it with `CFTC_COT_ARCHIVE_DIR`.
        """
        override = self.archive_dir_override or os.environ.get("CFTC_COT_ARCHIVE_DIR")
        if override:
            return Path(override) / "cot_archive.db"
        data_dir = os.environ.get("FINCEPT_DATA_DIR")
        if not data_dir:
            local = os.environ.get("LOCALAPPDATA")
            if os.name == "nt" and local:
                data_dir = str(Path(local) / "com.marketlab.terminal")
            else:
                data_dir = str(Path.home() / ".marketlab")
        return Path(data_dir) / "cftc" / "cot_archive.db"

    def _open_archive(self) -> CftcCotArchive:
        archive = CftcCotArchive(self._archive_path())
        archive.ensure_schema()
        return archive

    def _cot_code_values(self) -> frozenset:
        cache = getattr(self, "_cot_code_value_cache", None)
        if cache is None:
            cache = frozenset(self.cot_codes.values())
            self._cot_code_value_cache = cache
        return cache

    def _canonical_history_row(self, record: Dict[str, Any], family: str) -> Dict[str, Any]:
        """One Socrata record projected to the canonical workspace row shape.

        Shared by the current `cot_history` path and the cross-market monitor.
        The row carries the frozen engine's fields plus the participant
        spreading positions and per-participant trader counts the provider
        publishes; the engine ignores the extra keys, but the durable archive
        retains them as published. A cell the report does not carry stays None,
        never zero.
        """
        participants = self._PARTICIPANT_FIELDS[family]
        row: Dict[str, Any] = {}
        date_value = record.get("report_date_as_yyyy_mm_dd")
        if isinstance(date_value, str) and "T" in date_value:
            date_value = date_value.split("T")[0]
        row["report_date_as_yyyy_mm_dd"] = date_value
        for key in self._HISTORY_METADATA_KEYS:
            # Both ingestion paths store the same canonical identity text; see
            # _canonical_text for the whitespace/quote normalization.
            row[key] = self._canonical_text(record.get(key))
        row["open_interest_all"] = self._pick(record, ("open_interest_all",))
        for key, long_aliases, short_aliases in participants:
            row[f"{key}_long"] = self._pick(record, long_aliases)
            row[f"{key}_short"] = self._pick(record, short_aliases)
        for key, aliases in self._SPREAD_FIELDS.get(family, {}).items():
            row[f"{key}_spread"] = self._pick(record, aliases)
        for key, (long_aliases, short_aliases, spread_aliases) in self._PARTICIPANT_TRADER_FIELDS.get(
                family, {}).items():
            row[f"{key}_traders_long"] = self._pick(record, long_aliases)
            row[f"{key}_traders_short"] = self._pick(record, short_aliases)
            row[f"{key}_traders_spread"] = (
                self._pick(record, spread_aliases) if spread_aliases else None)
        for key, aliases in self._TRADER_CONTEXT_FIELDS:
            row[key] = self._pick(record, aliases)
        for key, aliases in self._CONCENTRATION_FIELDS:
            row[key] = self._pick_number(record, aliases)
        return row

    @staticmethod
    def _to_float(value) -> Optional[float]:
        if value is None or isinstance(value, bool):
            return None
        if isinstance(value, (int, float)):
            parsed = float(value)
            return parsed if math.isfinite(parsed) else None
        if isinstance(value, str):
            text = value.strip().replace(",", "")
            if not text:
                return None
            try:
                parsed = float(text)
            except ValueError:
                return None
            return parsed if math.isfinite(parsed) else None
        return None

    @staticmethod
    def _annual_first(record: Dict[str, Any], aliases: Iterable[str]):
        for alias in aliases:
            name = CFTCDataWrapper._normalize_annual_header(alias)
            if name in record and record[name] not in (None, ""):
                return record[name]
        return None

    @staticmethod
    def _normalize_contract_code(value) -> Optional[str]:
        if value is None:
            return None
        text = str(value).strip()
        if not text:
            return None
        # Codes are fixed 6-character identifiers; some older extracts dropped
        # the leading zeros. Zero-padding restores the published identifier.
        if text.isdigit() and len(text) < 6:
            return text.zfill(6)
        return text

    @staticmethod
    def _canonical_text(value) -> Optional[str]:
        """Canonical published display text for the identity/metadata columns.

        Removes surrounding whitespace and, when both ends carry the same
        literal single or double quote, that quote pair. Some older Socrata
        resources (the pre-2011 TFF data) wrap Contract Units in literal
        quotes while the annual files and the newer resources do not; the
        value is the same published text, so both ingestion paths store it
        identically and cannot rewrite each other's rows forever.
        """
        if value is None:
            return None
        text = str(value).strip()
        if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
            text = text[1:-1].strip()
        return text or None

    @staticmethod
    def _annual_text(value) -> Optional[str]:
        """Exact provider text with surrounding whitespace removed.

        The older annual files pad text columns with trailing spaces while the
        Socrata resources do not; the value is the same identity, so the
        canonical row strips it rather than storing a byte-level difference.
        """
        return CFTCDataWrapper._canonical_text(value)

    @staticmethod
    def _annual_report_date(value) -> Optional[str]:
        text = "" if value is None else str(value).strip()
        if not text:
            return None
        if "T" in text:
            text = text.split("T")[0]
        if re.fullmatch(r"\d{4}-\d{2}-\d{2}", text):
            return text
        friendly = text.split(" ")[0]
        match = re.fullmatch(r"(\d{1,2})[/-](\d{1,2})[/-](\d{4})", friendly)
        if match:
            month, day, year = match.groups()
            return f"{int(year):04d}-{int(month):02d}-{int(day):02d}"
        if re.fullmatch(r"\d{8}", text):
            return f"{text[:4]}-{text[4:6]}-{text[6:8]}"
        if re.fullmatch(r"\d{6}", text):
            return f"20{text[:2]}-{text[2:4]}-{text[4:6]}"
        return None

    def _annual_header_error(self, header: List[str], family: str) -> Optional[str]:
        """Refuse a year whose file does not carry the family's required
        columns; a partial header would otherwise be recorded as a valid report
        with every leg missing."""
        present = {self._normalize_annual_header(name) for name in header}
        checks: Dict[str, Iterable[str]] = {
            key: self._ANNUAL_FIELD_ALIASES[key]
            for key in ("report_date_as_yyyy_mm_dd", "cftc_contract_market_code",
                        "market_and_exchange_names", "open_interest_all")
        }
        for key, (long_aliases, short_aliases) in self._ANNUAL_LEG_ALIASES[family].items():
            checks[f"{key}_long"] = long_aliases
            checks[f"{key}_short"] = short_aliases
        missing = sorted(
            concept for concept, aliases in checks.items()
            if not any(self._normalize_annual_header(alias) in present for alias in aliases)
        )
        if missing:
            return "the annual file is missing required columns: " + ", ".join(missing)
        return None

    def _canonical_row_from_annual(self, record: Dict[str, Any], family: str, futures_only: bool):
        """One annual-file CSV record -> canonical row, or (None, reason).

        A missing cell is preserved as None, never zero; a record whose date or
        code cannot be read is rejected and counted rather than guessed.
        """
        normalized = {self._normalize_annual_header(key): value for key, value in record.items() if key}
        code = self._normalize_contract_code(
            self._annual_first(normalized, self._ANNUAL_FIELD_ALIASES["cftc_contract_market_code"]))
        if code is None:
            return None, "missing CFTC contract-market code"
        if code not in self._cot_code_values():
            # Other CFTC contracts are a deliberate filter, not a defect; the
            # caller counts them as filtered rather than rejected.
            return None, "outside_universe"
        report_date = self._annual_report_date(
            self._annual_first(normalized, self._ANNUAL_FIELD_ALIASES["report_date_as_yyyy_mm_dd"]))
        if report_date is None:
            return None, "unparseable report date"
        row: Dict[str, Any] = {
            "report_date_as_yyyy_mm_dd": report_date,
            "cftc_contract_market_code": code,
            "market_and_exchange_names": self._annual_text(
                self._annual_first(normalized, self._ANNUAL_FIELD_ALIASES["market_and_exchange_names"])),
            # The annual files publish the commodity/contract code, not the
            # Socrata display-name metadata columns; the keys stay present for
            # canonical shape parity and are honestly null.
            "contract_market_name": None,
            "commodity": None,
            "contract_units": self._annual_text(
                self._annual_first(normalized, self._ANNUAL_FIELD_ALIASES["contract_units"])),
            "futonly_or_combined": "FutOnly" if futures_only else "Combined",
            "open_interest_all": self._to_int(
                self._annual_first(normalized, self._ANNUAL_FIELD_ALIASES["open_interest_all"])),
        }
        for key, (long_aliases, short_aliases) in self._ANNUAL_LEG_ALIASES[family].items():
            row[f"{key}_long"] = self._to_int(self._annual_first(normalized, long_aliases))
            row[f"{key}_short"] = self._to_int(self._annual_first(normalized, short_aliases))
        for key, aliases in self._ANNUAL_SPREAD_ALIASES.get(family, {}).items():
            row[f"{key}_spread"] = self._to_int(self._annual_first(normalized, aliases))
        for key, (long_aliases, short_aliases, spread_aliases) in self._ANNUAL_TRADER_ALIASES.get(
                family, {}).items():
            row[f"{key}_traders_long"] = self._to_int(self._annual_first(normalized, long_aliases))
            row[f"{key}_traders_short"] = self._to_int(self._annual_first(normalized, short_aliases))
            row[f"{key}_traders_spread"] = (
                self._to_int(self._annual_first(normalized, spread_aliases)) if spread_aliases else None)
        for key in ("traders_total", "traders_reportable_long", "traders_reportable_short"):
            row[key] = self._to_int(self._annual_first(normalized, self._ANNUAL_FIELD_ALIASES[key]))
        for key in ("concentration_gross_4_long", "concentration_gross_4_short",
                    "concentration_gross_8_long", "concentration_gross_8_short",
                    "concentration_net_4_long", "concentration_net_4_short",
                    "concentration_net_8_long", "concentration_net_8_short"):
            row[key] = self._to_float(self._annual_first(normalized, self._ANNUAL_FIELD_ALIASES[key]))
        if row["open_interest_all"] is not None and row["open_interest_all"] < 0:
            return None, "negative Open Interest"
        for key, (long_aliases, short_aliases) in self._ANNUAL_LEG_ALIASES[family].items():
            for leg in (row[f"{key}_long"], row[f"{key}_short"]):
                if leg is not None and leg < 0:
                    return None, f"negative {key} position"
        for key in self._ANNUAL_SPREAD_ALIASES.get(family, {}):
            spread = row[f"{key}_spread"]
            if spread is not None and spread < 0:
                return None, f"negative {key} spreading position"
        for key, (_long, _short, spread_aliases) in self._ANNUAL_TRADER_ALIASES.get(family, {}).items():
            counts = [row[f"{key}_traders_long"], row[f"{key}_traders_short"]]
            if spread_aliases:
                counts.append(row[f"{key}_traders_spread"])
            for count in counts:
                if count is not None and count < 0:
                    return None, f"negative {key} trader count"
        return row, None

    def _download_annual(self, url: str, timeout: int = 180) -> bytes:
        response = self.session.get(
            url, timeout=timeout,
            headers={"Accept": "application/zip, application/octet-stream"})
        if response.status_code == 404:
            raise CFTCAnnualNotPublished(url)
        response.raise_for_status()
        content = response.content
        if not content:
            raise Exception("the annual file download was empty")
        return content

    def _backfill_one_year(self, archive: CftcCotArchive, family: str, futures_only: bool, year: int,
                           wanted_codes: frozenset) -> Dict[str, Any]:
        pattern, preferred = self.ANNUAL_SOURCES[(family, futures_only)]
        patterns = [pattern]
        for alternate in self.ANNUAL_ALTERNATE_SOURCES.get((family, futures_only), ()):
            if alternate not in patterns:
                patterns.append(alternate)
        basis_text = "futures_only" if futures_only else "futures_and_options_combined"
        started_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
        outcome: Dict[str, Any] = {
            "year": year, "url": None, "entry": None,
            "rows_inserted": 0, "rows_updated": 0, "rows_rejected": 0,
            "rows_filtered": 0, "rows_unchanged": 0, "rows_collapsed": 0,
            "status": "failed", "error": None,
        }

        def finish(status: str, error: Optional[str] = None) -> Dict[str, Any]:
            """Record every terminal per-year result durably.

            `not_published`, download `failed` and `malformed` exits are
            ingestion attempts an operator needs to see, so they are written to
            `cot_ingest_runs` exactly like `ok` and `no_market_data`.
            """
            outcome["status"] = status
            outcome["error"] = error
            archive.record_run(
                "cot_backfill", outcome["url"] or "", family, basis_text, str(year), status,
                rows_inserted=outcome["rows_inserted"], rows_updated=outcome["rows_updated"],
                rows_rejected=outcome["rows_rejected"],
                detail={"entry": outcome["entry"], "rows_filtered": outcome["rows_filtered"],
                        "rows_collapsed": outcome["rows_collapsed"], "error": error},
                started_at=started_at)
            return outcome

        content = None
        for candidate in patterns:
            candidate_url = f"{self.ANNUAL_HISTORY_BASE}/{candidate.format(year=year)}"
            outcome["url"] = candidate_url
            try:
                content = self._download_annual(candidate_url)
                break
            except CFTCAnnualNotPublished:
                continue
            except Exception as exc:
                # A real download failure is not retried against another
                # official filename; it is recorded as failed for this year.
                return finish("failed", str(exc))
        if content is None:
            return finish("not_published", "no official annual file exists for this year")

        if not content.startswith(b"PK\x03\x04"):
            return finish("malformed", "the downloaded annual file is not a ZIP archive")

        try:
            with zipfile.ZipFile(io.BytesIO(content)) as zf:
                entries = zf.namelist()
                entry = next((candidate for candidate in preferred if candidate in entries), None)
                if entry is None:
                    candidates = [name for name in entries if name.lower().endswith(".txt")]
                    if len(candidates) == 1:
                        entry = candidates[0]
                if entry is None:
                    return finish("malformed", "the annual ZIP carries no expected text entry")
                outcome["entry"] = entry
                with zf.open(entry) as raw:
                    text = io.TextIOWrapper(raw, encoding="utf-8", errors="replace", newline="")
                    reader = csv.reader(text)
                    try:
                        header = next(reader)
                    except StopIteration:
                        return finish("malformed", "the annual file is empty")
                    header_error = self._annual_header_error(header, family)
                    if header_error:
                        return finish("malformed", header_error)
                    rows: Dict[Tuple[str, str], Dict[str, Any]] = {}
                    for fields in reader:
                        if not fields or all(not str(field).strip() for field in fields):
                            continue
                        record = dict(zip(header, fields))
                        # Decide market membership from the exact contract code
                        # before validating the rest of the record: an
                        # unrequested supported market's malformed cells must
                        # not pollute the requested market's rejected count or
                        # turn a genuinely absent year into `malformed`.
                        normalized = {self._normalize_annual_header(key): value
                                      for key, value in record.items() if key}
                        code = self._normalize_contract_code(self._annual_first(
                            normalized, self._ANNUAL_FIELD_ALIASES["cftc_contract_market_code"]))
                        if code is None:
                            outcome["rows_rejected"] += 1
                            continue
                        if code not in self._cot_code_values() or code not in wanted_codes:
                            outcome["rows_filtered"] += 1
                            continue
                        row, reason = self._canonical_row_from_annual(record, family, futures_only)
                        if row is None:
                            # `code` already proved the market is requested and
                            # in universe; any remaining reason is invalid data.
                            outcome["rows_rejected"] += 1
                            continue
                        key = (row["report_date_as_yyyy_mm_dd"], row["cftc_contract_market_code"])
                        previous = rows.get(key)
                        if previous is not None:
                            if CftcCotArchive._comparison_key(previous) != CftcCotArchive._comparison_key(row):
                                # Two contradictory official observations for
                                # one report: fail closed for the year instead
                                # of storing whichever row happened to appear
                                # first while reporting success. The counts so
                                # far already live in `outcome`, so the durable
                                # run keeps them.
                                return finish(
                                    "malformed",
                                    "conflicting duplicate rows for report {0} contract {1}".format(*key))
                            outcome["rows_collapsed"] += 1
                            continue
                        rows[key] = row
        except zipfile.BadZipFile:
            return finish("malformed", "the annual ZIP could not be read")
        except Exception as exc:
            return finish("malformed", f"the annual file could not be parsed: {exc}")

        if not rows:
            if outcome["rows_rejected"]:
                # Records existed but none could be safely ingested; that is
                # unusable data, not a market genuinely absent from the year.
                return finish("malformed",
                              f"{outcome['rows_rejected']} record(s) for the selected markets "
                              "could not be ingested")
            return finish("no_market_data", None)

        retrieval_time = datetime.now(timezone.utc).isoformat(timespec="seconds")
        inserted, updated, unchanged = archive.upsert_observations(
            list(rows.values()), family, basis_text,
            source=f"cftc-annual:{outcome['url']}", retrieval_time=retrieval_time)
        outcome.update({
            "rows_inserted": inserted,
            "rows_updated": updated,
            "rows_unchanged": unchanged,
        })
        return finish("ok", None)

    def _resolve_markets(self, markets: Union[str, List[str], None]) -> Tuple[List[Tuple[str, str]], List[str]]:
        """Resolve market tokens to (key, code); return (resolved, unknown).

        A token may be a MarketLab market key or an exact CFTC contract-market
        code. Anything else is reported as unknown instead of being silently
        skipped, so a typo cannot look like "this market has no data"."""
        if markets is None or (isinstance(markets, str) and markets.strip().lower() in ("", "all")):
            return list(self.cot_codes.items()), []
        if isinstance(markets, str):
            tokens = [token.strip() for token in markets.split(",") if token.strip()]
        else:
            tokens = [str(token).strip() for token in markets if str(token).strip()]
        resolved: List[Tuple[str, str]] = []
        unknown: List[str] = []
        seen = set()
        code_to_key = {code: key for key, code in self.cot_codes.items()}
        for token in tokens:
            key = token.lower()
            code = self.cot_codes.get(key)
            if code is None and token in code_to_key:
                key, code = code_to_key[token], token
            if code is None:
                unknown.append(token)
                continue
            if key in seen:
                continue
            seen.add(key)
            resolved.append((key, code))
        return resolved, unknown

    def cot_backfill(self, report_type: str = "legacy", futures_only: bool = False,
                     start_year: Optional[int] = None, end_year: Optional[int] = None,
                     markets: Union[str, List[str], None] = "all") -> Dict[str, Any]:
        """Download and store the official CFTC annual historical files.

        The per-year files are the historical bootstrap path for the durable
        canonical archive; the live Socrata path stays the current/incremental
        source. A repeated run is idempotent: an unchanged report is not
        rewritten, while a changed value for a stored report is adopted.
        """
        try:
            family = self._report_family(report_type)
            if (family, bool(futures_only)) not in self.ANNUAL_SOURCES:
                return {"error": CFTCError(
                    "cot_backfill",
                    f"Annual backfill is not defined for the '{report_type}' report family. "
                    "Supported families are legacy, disaggregated and financial (TFF)."
                ).to_dict()}
            current_year = datetime.now(timezone.utc).year
            first_year = self.ANNUAL_FIRST_YEAR[(family, bool(futures_only))]
            resolved, unknown = self._resolve_markets(markets)
            if unknown:
                return {"error": CFTCError(
                    "cot_backfill", f"Unknown market selections: {', '.join(unknown)}").to_dict()}
            if not resolved:
                return {"error": CFTCError("cot_backfill", "No markets were selected.").to_dict()}
            resolved_start = max(first_year, current_year - 9) if start_year in (None, "", 0) else int(start_year)
            resolved_end = current_year if end_year in (None, "", 0) else int(end_year)
            if resolved_start < first_year:
                return {"error": CFTCError(
                    "cot_backfill",
                    f"The {family} annual files begin in {first_year}; {resolved_start} is not available."
                ).to_dict()}
            if resolved_end < resolved_start:
                return {"error": CFTCError(
                    "cot_backfill", f"Invalid year range {resolved_start}-{resolved_end}.").to_dict()}
            wanted_codes = frozenset(code for _key, code in resolved)
            archive = self._open_archive()
            years = []
            for year in range(resolved_start, resolved_end + 1):
                years.append(self._backfill_one_year(archive, family, bool(futures_only), year, wanted_codes))
            inserted = sum(item["rows_inserted"] for item in years)
            updated = sum(item["rows_updated"] for item in years)
            rejected = sum(item["rows_rejected"] for item in years)
            ok_years = [item["year"] for item in years if item["status"] == "ok"]
            failed_years = [item["year"] for item in years if item["status"] in ("failed", "malformed")]
            skipped_years = [item["year"] for item in years if item["status"] == "not_published"]
            no_data_years = [item["year"] for item in years if item["status"] == "no_market_data"]
            return {
                "success": True,
                "data": {
                    "years": years,
                    "rows_inserted": inserted,
                    "rows_updated": updated,
                    "rows_rejected": rejected,
                    "markets": len(resolved),
                    "ok_years": ok_years,
                    "failed_years": failed_years,
                    "skipped_years": skipped_years,
                    "no_data_years": no_data_years,
                    "archive": archive.coverage(),
                },
                "parameters": {
                    "report_type": report_type,
                    "report_family": family,
                    "futures_only": bool(futures_only),
                    "start_year": resolved_start,
                    "end_year": resolved_end,
                    "requested_start_year": start_year,
                    "first_available_year": first_year,
                    "markets": [key for key, _code in resolved],
                    "source": self.ANNUAL_HISTORY_BASE,
                    "retrieved_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                }
            }
        except Exception as e:
            return {"error": CFTCError("cot_backfill", str(e)).to_dict()}

    def cot_archive_status(self) -> Dict[str, Any]:
        """The durable archive's coverage and recent ingestion provenance."""
        try:
            archive = self._open_archive()
            return {
                "success": True,
                "data": archive.coverage(),
                "parameters": {
                    "archive_path": str(archive.path),
                    "retrieved_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                }
            }
        except Exception as e:
            return {"error": CFTCError("cot_archive_status", str(e)).to_dict()}

    # ── Cross-market monitor ────────────────────────────────────────────────

    def _thread_session(self) -> requests.Session:
        session = getattr(self._thread_local, "session", None)
        if session is None:
            session = requests.Session()
            session.headers.update(self.session.headers)
            self._thread_local.session = session
        return session

    def _monitor_get(self, url: str) -> Any:
        """One Socrata page for the monitor (per-thread session)."""
        if self.app_token:
            separator = "&" if "?" in url else "?"
            url = f"{url}{separator}$$app_token={self.app_token}"
        response = self._thread_session().get(url, timeout=60)
        response.raise_for_status()
        return response.json()

    def _monitor_fetch_rows(self, code: str, family: str, resource_id: str, max_rows: int,
                            page_size: int = 5000,
                            since_date: Optional[str] = None) -> List[Dict[str, Any]]:
        """Canonical rows for one contract from the current API path.

        Without `since_date` this is the contract's complete history, paginated
        to the end and failing closed at the cap. With a `since_date` (the
        archive's latest stored report date, included so a revision of that
        report is adopted) it is a single bounded increment. The monitor
        performs the full read once per contract and then increments; a bounded
        backfill therefore still triggers one full read before incremental
        scans begin.
        """
        import urllib.parse

        where = f"cftc_contract_market_code = '{code}'"
        records: List[Dict[str, Any]] = []
        if since_date:
            where += f" AND report_date_as_yyyy_mm_dd >= '{since_date}'"
            url = (f"{self.base_url}/resource/{resource_id}.json?$limit={int(max_rows)}"
                   f"&$where={urllib.parse.quote(where)}"
                   f"&$order=report_date_as_yyyy_mm_dd%20ASC")
            page = self._monitor_get(url)
            if not isinstance(page, list):
                raise Exception("the provider returned an unreadable page while reading the history")
            if len(page) >= max_rows:
                raise Exception(f"the contract increment reached the {max_rows}-row monitor cap")
            records = page
        else:
            offset = 0
            while len(records) < max_rows:
                limit = min(page_size, max_rows - len(records))
                url = (f"{self.base_url}/resource/{resource_id}.json?"
                       f"$limit={limit}&$offset={offset}&$where={urllib.parse.quote(where)}"
                       f"&$order=report_date_as_yyyy_mm_dd,id%20ASC")
                page = self._monitor_get(url)
                if not isinstance(page, list):
                    raise Exception("the provider returned an unreadable page while reading the history")
                if not page:
                    break
                records.extend(page)
                if len(page) < limit:
                    break
                offset += len(page)
            if len(records) >= max_rows:
                raise Exception(f"the contract history reached the {max_rows}-row monitor cap")
        rows = [self._canonical_history_row(record, family) for record in records]
        for row in rows:
            if not row.get("report_date_as_yyyy_mm_dd"):
                raise Exception("a provider row is missing its report date")
            identity = row.get("cftc_contract_market_code")
            if not identity:
                raise Exception("a provider row is missing its contract market code")
            if str(identity) != code:
                # The exact-code filter is part of the request. A row carrying
                # another contract must never reach the durable archive, where
                # the frozen engine would only check that the history is
                # internally consistent.
                raise Exception(
                    "the provider returned a row for contract {0} while {1} was requested".format(
                        identity, code))
        rows.sort(key=lambda item: item["report_date_as_yyyy_mm_dd"])
        for index in range(1, len(rows)):
            if rows[index]["report_date_as_yyyy_mm_dd"] == rows[index - 1]["report_date_as_yyyy_mm_dd"]:
                raise Exception("the provider returned two rows for the same report date")
        return rows

    @staticmethod
    def _monitor_weekly_step(previous: Optional[str], current: Optional[str]) -> bool:
        if not previous or not current:
            return False
        try:
            before = datetime.strptime(str(previous)[:10], "%Y-%m-%d")
            after = datetime.strptime(str(current)[:10], "%Y-%m-%d")
        except ValueError:
            return False
        gap = (after - before).days
        return 5 <= gap <= 10

    def _monitor_required_start(self, rows: List[Dict[str, Any]], family: str) -> int:
        """The earliest row index the engine's trailing references need.

        Mirrors the engine's reference construction exactly. For every
        participant, every 1/4/13-report flow metric, Open Interest and the
        primary concentration field it finds the 157th-from-last valid point or
        computed move and returns the index of the first row that move needs
        (its anchor). A suffix from that index reproduces every trailing
        reference the engine uses. When the full history itself does not carry
        157 valid entries for any series, the full history is required (0), so
        the monitor and the detailed workspace are exactly as unavailable as
        each other.

        The scan is bounded: each series is walked from the newest report
        backwards only until its 157th valid entry is found, and continuity is
        answered in O(1) from a broken-step prefix count.
        """
        count = len(rows)
        if count == 0:
            return 0
        broken = [0] * count
        for index in range(1, count):
            step = self._monitor_weekly_step(rows[index - 1].get("report_date_as_yyyy_mm_dd"),
                                             rows[index].get("report_date_as_yyyy_mm_dd"))
            broken[index] = broken[index - 1] + (0 if step else 1)

        def continuous(start: int, end: int) -> bool:
            return broken[end] - broken[start] == 0

        def oi_positive(row: Dict[str, Any]) -> bool:
            value = row.get("open_interest_all")
            return value is not None and value > 0

        def leg_pair(row: Dict[str, Any], key: str) -> bool:
            return row.get(f"{key}_long") is not None and row.get(f"{key}_short") is not None

        required = self.MONITOR_SUFFICIENT_POINTS

        def points_start(predicate) -> Optional[int]:
            found = 0
            for index in range(count - 1, -1, -1):
                if predicate(rows[index]):
                    found += 1
                    if found >= required:
                        return index
            return None

        def moves_start(horizon: int, predicate) -> Optional[int]:
            found = 0
            for index in range(count - 1, horizon - 1, -1):
                start = index - horizon
                if not predicate(rows[start], rows[index]):
                    continue
                if not continuous(start, index):
                    continue
                found += 1
                if found >= required:
                    return start
            return None

        starts: List[int] = []
        for key, _long_aliases, _short_aliases in self._PARTICIPANT_FIELDS[family]:
            start = points_start(lambda row, k=key: oi_positive(row) and leg_pair(row, k))
            if start is None:
                return 0
            starts.append(start)
            for horizon in (1, 4, 13):
                start = moves_start(horizon, lambda a, b, k=key: (
                    oi_positive(a) and leg_pair(a, k) and leg_pair(b, k)))
                if start is None:
                    return 0
                starts.append(start)
        start = points_start(oi_positive)
        if start is None:
            return 0
        starts.append(start)
        for horizon in (1, 4, 13):
            start = moves_start(horizon, lambda a, b: oi_positive(a) and oi_positive(b))
            if start is None:
                return 0
            starts.append(start)
        start = moves_start(1, lambda a, b: (
            a.get(self.MONITOR_CONCENTRATION_FIELD) is not None
            and b.get(self.MONITOR_CONCENTRATION_FIELD) is not None))
        if start is None:
            return 0
        starts.append(start)
        return max(0, min(starts))

    def _monitor_transport_rows(self, full_rows: List[Dict[str, Any]], family: str):
        """Bound the monitor payload without weakening the engine.

        The engine's participant/OI references are trailing windows of at most
        156 reports and the primary concentration field's percentile reference
        is unbounded. The transport therefore carries the last
        `MONITOR_FULL_WINDOW` full canonical rows (or whatever longer suffix
        `_monitor_required_start` proves is exactly sufficient) plus the older
        primary-concentration values as a compact series. The C++ monitor model
        merges the two back into one canonical observation vector before
        interpreting, so the classification equals the detailed workspace's
        full-history result by construction.
        """
        required_start = self._monitor_required_start(full_rows, family)
        window_size = min(len(full_rows),
                          max(self.MONITOR_FULL_WINDOW, len(full_rows) - required_start))
        recent = full_rows[-window_size:]
        older = full_rows[:-window_size] if window_size < len(full_rows) else []
        dates: List[str] = []
        values: List[float] = []
        for row in older:
            value = row.get(self.MONITOR_CONCENTRATION_FIELD)
            if value is None:
                continue
            dates.append(row.get("report_date_as_yyyy_mm_dd"))
            values.append(value)
        history = None
        if dates:
            history = {"field": self.MONITOR_CONCENTRATION_FIELD, "dates": dates, "values": values}
        return recent, history

    def _monitor_market(self, key: str, code: Optional[str], family: str, resource_id: str,
                        max_rows: int, refresh: bool, full_required: bool,
                        since_date: Optional[str]) -> Dict[str, Any]:
        if code is None or not refresh:
            return {"market_key": key, "contract_code": code, "fetched": None,
                    "fetch_ok": False, "fetch_error": None}
        try:
            fetched = self._monitor_fetch_rows(
                code, family, resource_id, max_rows,
                since_date=None if full_required else since_date)
            return {"market_key": key, "contract_code": code, "fetched": fetched,
                    "fetch_ok": True, "fetch_error": None, "full_history": full_required}
        except Exception as exc:
            return {"market_key": key, "contract_code": code, "fetched": None,
                    "fetch_ok": False, "fetch_error": str(exc), "full_history": full_required}

    def get_cot_monitor(self, report_type: str = "legacy", futures_only: bool = False,
                        markets: Union[str, List[str], None] = "all", max_rows: int = 25000,
                        refresh: bool = True) -> Dict[str, Any]:
        """Current canonical COT state across the supported market universe.

        Acquisition only: the current Socrata path is queried per market (full
        contract history on the first fetch, merged with the durable archive
        afterwards), and the merged canonical rows are returned. No analytical
        state is computed here; the deterministic interpretation stays in the
        C++ analytical layer so the monitor cannot become a second rules engine.
        """
        try:
            family = self._report_family(report_type)
            if family not in self._PARTICIPANT_FIELDS:
                return {"error": CFTCError(
                    "cot_monitor",
                    f"The '{report_type}' report family is not part of the monitor. "
                    "Supported families are legacy, disaggregated and financial (TFF)."
                ).to_dict()}
            if int(max_rows) < 20:
                return {"error": CFTCError("cot_monitor", "max_rows must be at least 20").to_dict()}
            suffix = "_futures_only" if futures_only else "_combined"
            resource_id = self.reports_dict.get(family + suffix)
            if not resource_id:
                return {"error": CFTCError(
                    "cot_monitor", f"Invalid report type: {report_type} (futures_only={futures_only})").to_dict()}
            resolved, unknown = self._resolve_markets(markets)
            if not resolved and not unknown:
                return {"error": CFTCError("cot_monitor", "No markets were selected.").to_dict()}
            basis_code = "futures_only" if futures_only else "futures_and_options_combined"
            archive = self._open_archive()

            fetched_by_key: Dict[str, Dict[str, Any]] = {}
            if resolved:
                # One full provider read per contract, then increments. A
                # bounded backfill never claims completeness, so it triggers
                # that full read on the next scan.
                plan = {
                    key: (archive.latest_report_date(family, basis_code, code),
                          archive.full_history_at(family, basis_code, code) is None)
                    for key, code in resolved
                }
                workers = min(8, max(1, len(resolved)))
                with ThreadPoolExecutor(max_workers=workers) as pool:
                    submissions = [
                        (pool.submit(self._monitor_market, key, code, family, resource_id,
                                     int(max_rows), bool(refresh), plan[key][1], plan[key][0]), key)
                        for key, code in resolved
                    ]
                    for future, key in submissions:
                        try:
                            fetched_by_key[key] = future.result()
                        except Exception as exc:
                            fetched_by_key[key] = {
                                "market_key": key, "contract_code": None,
                                "fetched": None, "fetch_error": str(exc)}

            retrieved_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
            markets_payload: List[Dict[str, Any]] = []
            for key, code in resolved:
                result = fetched_by_key.get(key, {})
                fetched = result.get("fetched")
                fetch_ok = bool(result.get("fetch_ok"))
                fetch_error = result.get("fetch_error")
                refresh_note = fetch_error or ""
                inserted = updated = 0
                if fetched:
                    inserted, updated, _unchanged = archive.upsert_observations(
                        fetched, family, basis_code, source=f"socrata:{resource_id}",
                        retrieval_time=retrieved_at)
                    if fetch_ok and result.get("full_history"):
                        archive.mark_full_history(family, basis_code, code)
                rows = archive.observations(family, basis_code, code)
                archive_rows = len(rows)
                transport_rows, concentration_history = self._monitor_transport_rows(rows, family)
                if fetch_error:
                    status = "archive_only" if rows else "unavailable"
                elif not refresh:
                    status = "archive_only" if rows else "no_local_history"
                elif fetch_ok and (inserted or updated):
                    status = "updated"
                elif fetch_ok and rows:
                    # A successful increment with nothing new (or a full read
                    # whose rows were all already stored): the stored history
                    # is current. A contract that stopped reporting is caught
                    # by the engine's report-age freshness flag.
                    status = "current"
                else:
                    status = "no_data"
                markets_payload.append({
                    "market_key": key,
                    "contract_code": code,
                    "status": status,
                    "refresh_error": refresh_note,
                    "inserted": inserted,
                    "updated": updated,
                    "archive_rows": archive_rows,
                    "rows": transport_rows,
                    "concentration_history": concentration_history,
                })
            for token in unknown:
                markets_payload.append({
                    "market_key": token.lower(),
                    "contract_code": None,
                    "status": "unknown_market",
                    "refresh_error": "the market is not part of the MarketLab CFTC universe",
                    "inserted": 0,
                    "updated": 0,
                    "archive_rows": 0,
                    "rows": [],
                })

            return {
                "success": True,
                "data": {
                    "report_family": family,
                    "report_basis": basis_code,
                    "futures_only": bool(futures_only),
                    "refresh": bool(refresh),
                    "markets": markets_payload,
                    "archive": archive.coverage(),
                },
                "parameters": {
                    "report_type": report_type,
                    "report_family": family,
                    "futures_only": bool(futures_only),
                    "markets_requested": len(resolved) + len(unknown),
                    "markets_returned": len(markets_payload),
                    "max_rows": int(max_rows),
                    "source": self.base_url,
                    "dataset": resource_id,
                    "archive_path": str(archive.path),
                    "retrieved_at": retrieved_at,
                }
            }
        except Exception as e:
            return {"error": CFTCError("cot_monitor", str(e)).to_dict()}

    def search_cot_markets(self, query: str) -> Dict[str, Any]:
        """Search for available COT markets"""
        try:
            if not query:
                return {"error": CFTCError("cot_search", "Search query is required").to_dict()}

            query_lower = query.lower()
            results = []

            # Search through our market mappings
            for key, (name, code) in self.market_mappings.items():
                if (query_lower in key.lower() or
                    query_lower in name.lower() or
                    query_lower == code.lower() or
                    name.lower().startswith(query_lower) or
                    key.lower().startswith(query_lower)):

                    # Try to categorize the market
                    category = "other"
                    if code.startswith("0"):
                        category = "commodities"
                    elif code.startswith("1"):
                        category = "financial"
                    elif code.startswith("2"):
                        category = "energy"
                    elif code.startswith("3"):
                        category = "metals"

                    results.append({
                        "search_key": key,
                        "cftc_code": code,
                        "market_name": name,
                        "category": category,
                        "description": f"{name} ({code})"
                    })

            if not results:
                return {"error": CFTCError("cot_search", f"No markets found for query: {query}").to_dict()}

            return {
                "success": True,
                "data": results,
                "parameters": {"query": query}
            }

        except Exception as e:
            return {"error": CFTCError("cot_search", str(e)).to_dict()}

    def get_available_report_types(self) -> Dict[str, Any]:
        """Get information about available COT report types"""
        try:
            report_info = {}

            for report_type, description in self.report_descriptions.items():
                report_info[report_type] = {
                    "description": description,
                    "has_futures_only_option": report_type != "supplemental"
                }

            return {
                "success": True,
                "data": report_info,
                "parameters": {}
            }

        except Exception as e:
            return {"error": CFTCError("available_report_types", str(e)).to_dict()}

    # MARKET SENTIMENT ANALYSIS ENDPOINTS

    def analyze_market_sentiment(self, identifier: str, report_type: str = "legacy") -> Dict[str, Any]:
        """Describe the latest Legacy commercial / non-commercial positioning.

        Only the Legacy family carries the CFTC's commercial/non-commercial
        split; Disaggregated and TFF return a typed error instead of relabelling
        their own categories. A row missing a required field returns a typed
        error instead of zero-filled positions. The direction fields describe
        the sign of the reported net position only (net_long / net_short /
        flat); they are not a sentiment, a forecast or a trading signal."""
        try:
            family = self._report_family(report_type)
            field_map = self._POSITION_FIELDS.get(family) if family == "legacy" else None
            if field_map is None:
                return {"error": CFTCError(
                    "market_sentiment",
                    f"The commercial/non-commercial view is not defined for the '{report_type}' report family. "
                    "Only the Legacy report classifies traders as commercial and non-commercial; the "
                    "Disaggregated and financial (TFF) reports use their own categories, which this view "
                    "would mislabel. Use the cot_history command."
                ).to_dict()}

            # Get recent COT data
            cot_result = self.get_cot_data(
                identifier=identifier,
                report_type=report_type,
                start_date=(datetime.now() - timedelta(days=90)).strftime("%Y-%m-%d"),
                limit=20
            )

            if not cot_result.get("success"):
                return cot_result

            cot_data = cot_result["data"]
            if not cot_data:
                return {"error": CFTCError("market_sentiment", f"No COT data available for sentiment analysis: {identifier}").to_dict()}

            # Sort by date (most recent first)
            cot_data.sort(key=lambda x: x.get("report_date_as_yyyy_mm_dd", ""), reverse=True)

            # Get most recent data
            latest_data = cot_data[0]
            previous_data = cot_data[1] if len(cot_data) > 1 else None

            open_interest = self._to_int(latest_data.get("open_interest_all"))
            latest = {key: self._pick(latest_data, names) for key, names in field_map.items()}
            missing = [key for key, value in latest.items() if value is None]
            if open_interest is None:
                missing.append("open_interest_all")
            if missing:
                return {"error": CFTCError(
                    "market_sentiment",
                    "The latest {0} row ({1}) is missing: {2}. Sentiment was not derived — a "
                    "missing position is not zero.".format(
                        family, latest_data.get("report_date_as_yyyy_mm_dd", "unknown date"), ", ".join(missing))
                ).to_dict()}

            # Calculate sentiment metrics
            sentiment_analysis = {
                "latest_report": latest_data.get("report_date_as_yyyy_mm_dd"),
                "market_name": latest_data.get("market_and_exchange_names", ""),
                "open_interest": open_interest,
                "commercial_positions": {
                    "long": latest["commercial_long"],
                    "short": latest["commercial_short"],
                    "net": latest["commercial_long"] - latest["commercial_short"]
                },
                "non_commercial_positions": {
                    "long": latest["non_commercial_long"],
                    "short": latest["non_commercial_short"],
                    "net": latest["non_commercial_long"] - latest["non_commercial_short"]
                },
                "non_reportable_positions": {
                    "long": latest["non_reportable_long"],
                    "short": latest["non_reportable_short"]
                }
            }

            # Week-over-week comparisons exist only when the previous week
            # carries the same measured field. A comparison that cannot be made
            # stays absent, and the OI trend / activity that depend on it stay
            # "unavailable" — never a fabricated 0 / "decreasing" / "low".
            previous_oi = self._to_int(previous_data.get("open_interest_all")) if previous_data else None
            oi_change = None
            if previous_oi is not None:
                oi_change = open_interest - previous_oi
                sentiment_analysis["change_in_oi"] = oi_change
            if previous_data:
                previous = {key: self._pick(previous_data, names) for key, names in field_map.items()}
                if all(value is not None for value in previous.values()):
                    sentiment_analysis["commercial_positions"]["long_change"] = latest["commercial_long"] - previous["commercial_long"]
                    sentiment_analysis["commercial_positions"]["short_change"] = latest["commercial_short"] - previous["commercial_short"]
                    sentiment_analysis["non_commercial_positions"]["long_change"] = latest["non_commercial_long"] - previous["non_commercial_long"]
                    sentiment_analysis["non_commercial_positions"]["short_change"] = latest["non_commercial_short"] - previous["non_commercial_short"]

            # Calculate sentiment scores
            total_oi = sentiment_analysis["open_interest"]
            if total_oi > 0:
                sentiment_analysis["commercial_long_pct"] = (sentiment_analysis["commercial_positions"]["long"] / total_oi) * 100
                sentiment_analysis["non_commercial_long_pct"] = (sentiment_analysis["non_commercial_positions"]["long"] / total_oi) * 100
                sentiment_analysis["non_reportable_long_pct"] = (sentiment_analysis["non_reportable_positions"]["long"] / total_oi) * 100

            # The reported net direction only. An exact-zero net is "flat"; an
            # unavailable comparison is "unavailable", not "decreasing", and
            # activity is only derived when the change percent it needs actually
            # exists. No bullish/bearish vocabulary: a net position is an
            # accounting fact, not a statement of motive or a forecast.
            def _bias(net: int) -> str:
                if net > 0:
                    return "net_long"
                if net < 0:
                    return "net_short"
                return "flat"

            net_commercial = sentiment_analysis["commercial_positions"]["net"]
            net_non_commercial = sentiment_analysis["non_commercial_positions"]["net"]
            if oi_change is None:
                oi_trend = "unavailable"
                activity_level = "unavailable"
            else:
                if oi_change > 0:
                    oi_trend = "increasing"
                elif oi_change < 0:
                    oi_trend = "decreasing"
                else:
                    oi_trend = "unchanged"
                oi_change_pct = (oi_change / total_oi * 100) if total_oi > 0 else None
                if oi_change_pct is None:
                    activity_level = "unavailable"
                else:
                    sentiment_analysis["oi_change_pct"] = oi_change_pct
                    activity_level = ("high" if abs(oi_change_pct) > 5
                                      else "moderate" if abs(oi_change_pct) > 1
                                      else "low")

            sentiment_analysis["overall_sentiment"] = {
                "commercial_bias": _bias(net_commercial),
                "non_commercial_bias": _bias(net_non_commercial),
                "oi_trend": oi_trend,
                "activity_level": activity_level
            }

            return {
                "success": True,
                "data": sentiment_analysis,
                "parameters": {
                    "identifier": identifier,
                    "report_type": report_type,
                    "report_family": family,
                    "source": self.base_url,
                    "retrieved_at": datetime.now(timezone.utc).isoformat(timespec="seconds")
                }
            }

        except Exception as e:
            return {"error": CFTCError("market_sentiment", str(e)).to_dict()}

    def get_position_summary(self, identifier: str, report_type: str = "legacy") -> Dict[str, Any]:
        """Get summary of current Legacy positions for a market.

        Same rules as the sentiment view: Legacy only (the commercial /
        non-commercial classes are the Legacy report's own), numeric coercion,
        and a typed error when a required field is missing."""
        try:
            family = self._report_family(report_type)
            field_map = self._POSITION_FIELDS.get(family) if family == "legacy" else None
            if field_map is None:
                return {"error": CFTCError(
                    "position_summary",
                    f"Commercial/non-commercial position classes are not defined for the '{report_type}' "
                    "report family. Only the Legacy report uses them; the Disaggregated and financial (TFF) "
                    "reports use their own categories. Use the cot_history command."
                ).to_dict()}

            # Get latest COT data
            cot_result = self.get_cot_data(
                identifier=identifier,
                report_type=report_type,
                start_date=(datetime.now() - timedelta(days=30)).strftime("%Y-%m-%d"),
                limit=5
            )

            if not cot_result.get("success"):
                return cot_result

            cot_data = cot_result["data"]
            if not cot_data:
                return {"error": CFTCError("position_summary", f"No position data available: {identifier}").to_dict()}

            # Get most recent data. get_cot_data orders ASC, so sort before
            # picking — otherwise this returns the OLDEST row of the window and
            # reports it as "current positions".
            cot_data.sort(key=lambda x: x.get("report_date_as_yyyy_mm_dd", ""), reverse=True)
            latest = cot_data[0]

            open_interest = self._to_int(latest.get("open_interest_all"))
            values = {key: self._pick(latest, names) for key, names in field_map.items()}
            missing = [key for key, value in values.items() if value is None]
            if open_interest is None:
                missing.append("open_interest_all")
            if missing:
                return {"error": CFTCError(
                    "position_summary",
                    "The latest {0} row is missing: {1}. No summary was derived — a missing "
                    "position is not zero.".format(family, ", ".join(missing))
                ).to_dict()}

            def pct(value: int) -> Optional[float]:
                return (value / open_interest) * 100 if open_interest > 0 else None

            # Build position summary
            summary = {
                "report_date": latest.get("report_date_as_yyyy_mm_dd"),
                "market_name": latest.get("market_and_exchange_names", ""),
                "open_interest": open_interest,
                "positions": {
                    "commercial": {
                        "long": values["commercial_long"],
                        "short": values["commercial_short"],
                        "net": values["commercial_long"] - values["commercial_short"],
                        "pct_of_oi": {
                            "long": pct(values["commercial_long"]),
                            "short": pct(values["commercial_short"]),
                        }
                    },
                    "non_commercial": {
                        "long": values["non_commercial_long"],
                        "short": values["non_commercial_short"],
                        "net": values["non_commercial_long"] - values["non_commercial_short"],
                        "pct_of_oi": {
                            "long": pct(values["non_commercial_long"]),
                            "short": pct(values["non_commercial_short"]),
                        }
                    },
                    "non_reportable": {
                        "long": values["non_reportable_long"],
                        "short": values["non_reportable_short"],
                        "pct_of_oi": {
                            "long": pct(values["non_reportable_long"]),
                            "short": pct(values["non_reportable_short"]),
                        }
                    }
                }
            }

            return {
                "success": True,
                "data": summary,
                "parameters": {
                    "identifier": identifier,
                    "report_type": report_type,
                    "report_family": family,
                    "source": self.base_url,
                    "retrieved_at": datetime.now(timezone.utc).isoformat(timespec="seconds")
                }
            }

        except Exception as e:
            return {"error": CFTCError("position_summary", str(e)).to_dict()}

    # COMPREHENSIVE DATA ENDPOINTS

    def get_comprehensive_cot_overview(self, identifiers: List[str] = None, report_type: str = "legacy") -> Dict[str, Any]:
        """Get comprehensive COT overview for multiple markets"""
        try:
            if not identifiers:
                # Default to major markets
                identifiers = ["gold", "crude_oil", "euro", "s&p_500", "bitcoin"]

            results = {}

            for identifier in identifiers:
                # Get position summary
                summary_result = self.get_position_summary(identifier, report_type)
                results[identifier] = summary_result

                # Get sentiment analysis
                sentiment_result = self.analyze_market_sentiment(identifier, report_type)
                results[f"{identifier}_sentiment"] = sentiment_result

            # Check if we have any successful data
            has_data = any(
                result.get("success") and result.get("data")
                for result in results.values()
            )

            if not has_data:
                return {"error": CFTCError("comprehensive_cot_overview", "No COT data found for any markets").to_dict()}

            return {
                "success": True,
                "data": results,
                "parameters": {
                    "identifiers": identifiers,
                    "report_type": report_type
                }
            }

        except Exception as e:
            return {"error": CFTCError("comprehensive_cot_overview", str(e)).to_dict()}

    def get_cot_historical_trend(self, identifier: str, report_type: str = "disaggregated",
                                period: int = 52) -> Dict[str, Any]:
        """Get historical COT trend data for analysis.

        Each point carries the family's actual fields under that family's own
        participant vocabulary: legacy emits commercial / non-commercial,
        disaggregated emits producer-merchant / managed-money (the faithful
        equivalents), so a trend row is never labelled with another report
        family's class names. A field the report did not carry stays null in
        the point (and the derived net stays null when either leg is missing)
        instead of being zero-filled, so a gap is distinguishable from a
        genuine zero position. The TFF family has no commercial/non-commercial
        split and returns a typed error."""
        try:
            family = self._report_family(report_type)
            field_map = self._POSITION_FIELDS.get(family)
            if field_map is None:
                return {"error": CFTCError(
                    "cot_historical_trend",
                    f"Historical trend is not defined for the '{report_type}' report family. "
                    "The CFTC financial futures (TFF) report uses Dealer/Intermediary, Asset "
                    "Manager/Institutional and Leveraged Funds classes. Use the COT Table view."
                ).to_dict()}

            # Get historical data
            cot_result = self.get_cot_data(
                identifier=identifier,
                report_type=report_type,
                start_date=(datetime.now() - timedelta(weeks=period)).strftime("%Y-%m-%d"),
                limit=period * 3
            )

            if not cot_result.get("success"):
                return cot_result

            cot_data = cot_result["data"]
            if not cot_data:
                return {"error": CFTCError("cot_historical_trend", f"No historical COT data: {identifier}").to_dict()}

            # Process trend data. The emitted participant keys follow the
            # family's own vocabulary; the internal field names do not change.
            group_a, group_b = ("commercial", "non_commercial") if family == "legacy" else ("prod_merc", "m_money")
            trend_data = []
            for record in cot_data:
                values = {key: self._pick(record, names) for key, names in field_map.items()}
                a_long = values["commercial_long"]
                a_short = values["commercial_short"]
                b_long = values["non_commercial_long"]
                b_short = values["non_commercial_short"]
                trend_point = {
                    "date": record.get("report_date_as_yyyy_mm_dd"),
                    "open_interest": self._to_int(record.get("open_interest_all")),
                    f"{group_a}_long": a_long,
                    f"{group_a}_short": a_short,
                    f"{group_a}_net": (a_long - a_short) if a_long is not None and a_short is not None else None,
                    f"{group_b}_long": b_long,
                    f"{group_b}_short": b_short,
                    f"{group_b}_net": (b_long - b_short) if b_long is not None and b_short is not None else None,
                }
                trend_data.append(trend_point)

            # Sort by date
            trend_data.sort(key=lambda x: x["date"] or "")

            return {
                "success": True,
                "data": trend_data,
                "parameters": {
                    "identifier": identifier,
                    "report_type": report_type,
                    "report_family": family,
                    "period": period,
                    "source": self.base_url,
                    "retrieved_at": datetime.now(timezone.utc).isoformat(timespec="seconds")
                }
            }

        except Exception as e:
            return {"error": CFTCError("cot_historical_trend", str(e)).to_dict()}


def main(args=None):
    
    if args is None:
        args = sys.argv[1:]
    """Main function for CLI interface"""
    if len(args) + 1 < 2:
        print(json.dumps({
            "error": "Usage: python cftc_data.py <command> [args...]",
            "commands": [
                "cot_data [identifier] [report_type] [futures_only] [start_date] [end_date] [limit]",
                "cot_history [identifier] [report_type] [futures_only] [max_rows]",
                "cot_monitor [report_type] [futures_only] [markets|all] [max_rows]",
                "cot_backfill [report_type] [futures_only] [start_year] [end_year] [markets|all]",
                "cot_archive_status",
                "search_cot_markets [query]",
                "available_report_types",
                "market_sentiment [identifier] [report_type]",
                "position_summary [identifier] [report_type]",
                "comprehensive_cot_overview [identifiers] [report_type]",
                "cot_historical_trend [identifier] [report_type] [period]"
            ],
            "note": "CFTC app token optional but recommended for rate limiting. Set CFTC_APP_TOKEN environment variable.",
            "identifier_examples": ["all", "gold", "crude_oil", "euro", "bitcoin", "002602", "088691"]
        }))
        sys.exit(1)

    command = args[0]
    wrapper = CFTCDataWrapper()

    try:
        if command == "cot_data":
            identifier = args[1] if len(args) + 1 > 2 else "all"
            report_type = args[2] if len(args) + 1 > 3 else "legacy"
            futures_only = args[3].lower() == "true" if len(args) + 1 > 4 else False
            start_date = args[4] if len(args) + 1 > 5 else None
            end_date = sys.argv[6] if len(args) + 1 > 6 else None
            limit = int(sys.argv[7]) if len(args) + 1 > 7 else 1000
            result = wrapper.get_cot_data(identifier, report_type, futures_only, start_date, end_date, limit)

        elif command == "cot_history":
            identifier = args[1] if len(args) + 1 > 2 else None
            report_type = args[2] if len(args) + 1 > 3 else "legacy"
            futures_only = args[3].lower() == "true" if len(args) + 1 > 4 else False
            max_rows = int(args[4]) if len(args) + 1 > 5 else 20000
            result = wrapper.get_cot_history(identifier, report_type, futures_only, max_rows)

        elif command == "cot_monitor":
            report_type = args[1] if len(args) + 1 > 2 else "legacy"
            futures_only = args[2].lower() == "true" if len(args) + 1 > 3 else False
            markets = args[3] if len(args) + 1 > 4 else "all"
            max_rows = int(args[4]) if len(args) + 1 > 5 else 25000
            result = wrapper.get_cot_monitor(report_type, futures_only, markets, max_rows)

        elif command == "cot_backfill":
            report_type = args[1] if len(args) + 1 > 2 else "legacy"
            futures_only = args[2].lower() == "true" if len(args) + 1 > 3 else False
            start_year = int(args[3]) if len(args) + 1 > 4 and args[3] else None
            end_year = int(args[4]) if len(args) + 1 > 5 and args[4] else None
            markets = args[5] if len(args) + 1 > 6 else "all"
            result = wrapper.cot_backfill(report_type, futures_only, start_year, end_year, markets)

        elif command == "cot_archive_status":
            result = wrapper.cot_archive_status()

        elif command == "search_cot_markets":
            query = args[1] if len(args) + 1 > 2 else None
            result = wrapper.search_cot_markets(query)

        elif command == "available_report_types":
            result = wrapper.get_available_report_types()

        elif command == "market_sentiment":
            identifier = args[1] if len(args) + 1 > 2 else None
            report_type = args[2] if len(args) + 1 > 3 else "legacy"
            result = wrapper.analyze_market_sentiment(identifier, report_type)

        elif command == "position_summary":
            identifier = args[1] if len(args) + 1 > 2 else None
            report_type = args[2] if len(args) + 1 > 3 else "legacy"
            result = wrapper.get_position_summary(identifier, report_type)

        elif command == "comprehensive_cot_overview":
            identifiers_str = args[1] if len(args) + 1 > 2 else None
            identifiers = identifiers_str.split(',') if identifiers_str else None
            report_type = args[2] if len(args) + 1 > 3 else "legacy"
            result = wrapper.get_comprehensive_cot_overview(identifiers, report_type)

        elif command == "cot_historical_trend":
            identifier = args[1] if len(args) + 1 > 2 else None
            report_type = args[2] if len(args) + 1 > 3 else "disaggregated"
            period = int(args[3]) if len(args) + 1 > 4 else 52
            result = wrapper.get_cot_historical_trend(identifier, report_type, period)

        else:
            result = {"error": CFTCError(command, f"Unknown command: {command}").to_dict()}

        # The monitor carries every market's canonical history; compact JSON
        # keeps that provider response practical without changing the contract
        # (it is still the same object, parsed identically by the host).
        if command == "cot_monitor":
            print(json.dumps(result, separators=(",", ":")))
        else:
            print(json.dumps(result, indent=2))

    except Exception as e:
        print(json.dumps({"error": CFTCError(command, str(e)).to_dict()}, indent=2))


if __name__ == "__main__":
    main()