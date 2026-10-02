"""Bounded local Investing monthly-contract export adapter; no web transport.

Provisional qualification: synthetic fixtures only; real provider export
compatibility has not been established."""
from __future__ import annotations

import csv
import hashlib
import io
import math
from datetime import datetime
from pathlib import Path
from urllib.parse import urlparse, parse_qs

from fedwatch import zq
from fedwatch.errors import ZqDataError


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
    contracts, reports = [], []
    for path in files:
        symbol, month, year = zq.parse_contract_filename(path)
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
            reader = csv.DictReader(io.StringIO("\n".join(lines[2:])))
            if reader.fieldnames != ["Date", "Price", "Open", "High", "Low", "Vol.", "Change %"]:
                raise ValueError("unexpected Investing CSV header")
            by_day = {}
            for index, row in enumerate(reader):
                if index >= 20_000:
                    raise ValueError("too many daily rows")
                parsed_date = None
                for fmt in ("%b %d, %Y", "%m/%d/%Y", "%Y-%m-%d"):
                    try:
                        parsed_date = datetime.strptime(row["Date"], fmt).date()
                        break
                    except ValueError:
                        pass
                if parsed_date is None:
                    raise ValueError("invalid observation date")
                close_text = row["Price"].strip().replace(",", "")
                close = None if close_text in ("", "-", "N/A") else float(close_text)
                if close is not None and (not math.isfinite(close) or close <= 0):
                    raise ValueError("invalid Fed Funds indicative close")
                if parsed_date in by_day and by_day[parsed_date] != close:
                    raise ValueError("conflicting duplicate daily close")
                by_day[parsed_date] = close
            if not by_day:
                raise ValueError("empty provider history")
        except (OSError, UnicodeError, ValueError, IndexError, KeyError, TypeError) as exc:
            raise ZqDataError("FEDWATCH_ZQ_DATA_INVALID", f"{path.name}: {exc}") from exc
        for day, close in sorted(by_day.items()):
            contracts.append({"contract_symbol": symbol, "contract_month": month, "contract_year": year,
                              "date": day, "close_price": close, "volume": None, "open_interest": None,
                              "low_confidence": True})
        reports.append({"file": path.name, "symbol": symbol, "source_url": source,
                        "input_sha256": hashlib.sha256(raw).hexdigest(), "rows": len(by_day),
                        "first_date": min(by_day).isoformat(), "last_date": max(by_day).isoformat(),
                        "missing_close_rows": sum(p is None for p in by_day.values()),
                        "identity_status": "USER_DECLARED_MONTHLY_CONTRACT",
                        "price_type": "INVESTING_INDICATIVE_DAILY_CLOSE", "open_interest": "NOT_PROVIDED"})
    return contracts, {"files": reports, "rows": len(contracts), "format": "investing",
                       "warnings": ["User must verify each cid's actual month; cid/FFcN may roll. Indicative close is not CME settlement."]}
