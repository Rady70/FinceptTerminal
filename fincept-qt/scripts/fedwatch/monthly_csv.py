"""Bounded local Investing monthly-contract export adapter; no web transport.

Provisional qualification: synthetic fixtures only; real provider export
compatibility has not been established."""
from __future__ import annotations

import csv
import hashlib
import math
import re
from datetime import datetime
from pathlib import Path
from urllib.parse import urlparse, parse_qs

from fedwatch import zq
from fedwatch.errors import ZqDataError
from fedwatch.row_dates import uncertainty_windows


def _observation_date(raw):
    raw = raw.strip() if isinstance(raw, str) else raw
    for fmt in ("%b %d, %Y", "%m/%d/%Y", "%Y-%m-%d"):
        try:
            return datetime.strptime(raw, fmt).date()
        except (ValueError, TypeError):
            pass
    return None


def load_contracts(directory):
    """Each user-supplied export declares Symbol and contract-specific Source.

    Investing's rolling FFc1/FFcN identity is never used as a fixed contract.
    The declared ZQ month must match its filename. Actual export columns are
    Date,Price,Open,High,Low,Vol.,Change %. Price is the indicative daily close,
    not an exchange settlement. Missing open interest is None, never zero.
    """
    directory = Path(directory)
    if not directory.is_dir():
        raise ZqDataError("FEDWATCH_ZQ_DATA_UNAVAILABLE", "monthly-contract directory does not exist")
    files = sorted(p for p in directory.glob("*.csv") if zq.FILENAME_RE.match(p.name))
    if not files or len(files) > 60:
        raise ZqDataError("FEDWATCH_ZQ_DATA_INVALID", "supply 1 to 60 monthly ZQ CSV files")
    contracts, reports, errors = [], [], []
    for path in files:
        symbol, month, year = zq.parse_contract_filename(path)
        raw = None
        try:
            with path.open("rb") as handle:
                raw = handle.read(5_000_001)
            if len(raw) > 5_000_000:
                raise ValueError("file exceeds bounded size")
            lines = raw.decode("utf-8-sig").splitlines()
            if lines[0].strip() != f"Symbol: {symbol}":
                raise ValueError("monthly Symbol declaration must match filename")
            source = lines[1].removeprefix("Source: ").strip()
            url = urlparse(source)
            if (not lines[1].startswith("Source: ") or url.scheme != "https" or
                    url.hostname != "www.investing.com" or not parse_qs(url.query).get("cid")):
                raise ValueError("Source must name the Investing contract history URL with cid")
            columns = ["Date", "Price", "Open", "High", "Low", "Vol.", "Change %"]
            if next(csv.reader(lines[2:3]), []) != columns:
                raise ValueError("unexpected Investing CSV header")
            by_day = {}
            rejected_days = set()
            row_errors = []
            row_dates = []
            for index, line in enumerate(lines[3:]):
                if index >= 20_000:
                    raise ValueError("too many daily rows")
                if not line.strip():
                    continue
                try:
                    fields = next(csv.reader([line], strict=True))
                except csv.Error as exc:
                    prefix = re.match(r'^("(?:[^\"]|\"\")*"|[^,]*),', line)
                    parsed_date = _observation_date(next(csv.reader([prefix.group(1)]))[0]) if prefix else None
                    if parsed_date:
                        rejected_days.add(parsed_date)
                    row_dates.append((index + 4, parsed_date))
                    row_errors.append({"row": index + 4, "date": parsed_date.isoformat() if parsed_date else None,
                                       "reason": str(exc)})
                    continue
                if not any(field.strip() for field in fields):
                    continue
                row = dict(zip(columns, fields))
                parsed_date = _observation_date(row.get("Date", ""))
                row_dates.append((index + 4, parsed_date))
                if parsed_date is None:
                    row_errors.append({"row": index + 4, "date": None, "reason": "invalid observation date",
                                       "source_fields": fields})
                    continue
                try:
                    if len(fields) != len(columns):
                        raise ValueError("unexpected columns")
                    close_text = row["Price"].strip().replace(",", "")
                    close = None if close_text in ("", "-", "N/A") else float(close_text)
                    if close is not None and (not math.isfinite(close) or close <= 0):
                        raise ValueError("invalid Fed Funds indicative close")
                    if parsed_date in by_day and by_day[parsed_date] != close:
                        raise ValueError("conflicting duplicate daily close")
                    by_day[parsed_date] = close
                except (ValueError, TypeError, AttributeError) as exc:
                    rejected_days.add(parsed_date)
                    row_errors.append({"row": index + 4, "date": parsed_date.isoformat(), "reason": str(exc)})
            for day in rejected_days:
                # Retain an explicit unknown on this date, so an earlier price
                # cannot be carried over a rejected latest daily observation.
                by_day[day] = None
            if not by_day:
                raise ValueError("empty provider history")
        except (OSError, UnicodeError, ValueError, IndexError, KeyError, TypeError, csv.Error) as exc:
            errors.append(ZqDataError("FEDWATCH_ZQ_DATA_INVALID", f"{path.name}: {exc}",
                                     detail={"file": path.name}).to_dict())
            reports.append({"file": path.name, "symbol": symbol, "status": "REJECTED",
                            "input_sha256": hashlib.sha256(raw).hexdigest()
                            if raw is not None and len(raw) <= 5_000_000 else None})
            continue
        if row_errors:
            errors.append(ZqDataError("FEDWATCH_ZQ_DATA_PARTIAL", f"{path.name}: daily rows rejected",
                                      detail={"file": path.name, "rejected_rows": row_errors}).to_dict())
        undated_count = sum(error.get("date") is None for error in row_errors)
        date_ordered, windows = uncertainty_windows(row_dates)
        close_quality = []
        for day, close in sorted(by_day.items()):
            close_status = "REJECTED" if day in rejected_days else "SOURCE_MISSING" if close is None else "OBSERVED"
            if close_status != "OBSERVED":
                close_quality.append({"date": day.isoformat(), "status": close_status})
            contracts.append({"contract_symbol": symbol, "contract_month": month, "contract_year": year,
                              "date": day, "close_price": close, "volume": None, "open_interest": None,
                              "low_confidence": True, "close_status": close_status,
                              "undated_rejected_row_count": undated_count,
                              "date_ordered": date_ordered, "date_uncertainty_windows": windows})
        reports.append({"file": path.name, "symbol": symbol, "source_url": source,
                        "status": "PARTIAL" if row_errors else "OK", "rejected_rows": row_errors,
                        "input_sha256": hashlib.sha256(raw).hexdigest(), "rows": len(by_day),
                        "first_date": min(by_day).isoformat(), "last_date": max(by_day).isoformat(),
                        "missing_close_rows": sum(p is None and day not in rejected_days for day, p in by_day.items()),
                        "rejected_close_rows": len(rejected_days), "close_quality": close_quality,
                        "undated_rejected_row_count": undated_count,
                        "date_ordered": date_ordered, "date_uncertainty_windows": windows,
                        "reconstruction_status": "DATE_UNCERTAIN" if undated_count else "DATE_IDENTIFIED",
                        "identity_status": "USER_DECLARED_MONTHLY_CONTRACT",
                        "price_type": "INVESTING_INDICATIVE_DAILY_CLOSE", "open_interest": "NOT_PROVIDED"})
    if not contracts:
        raise ZqDataError("FEDWATCH_ZQ_DATA_INVALID", "no usable monthly-contract files",
                          detail={"errors": errors})
    return contracts, {"files": reports, "rows": len(contracts), "format": "investing", "errors": errors,
                       "warnings": ["User must verify each cid's actual month; cid/FFcN may roll. Indicative close is not CME settlement."]}
