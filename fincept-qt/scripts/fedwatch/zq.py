"""Optional historical ZQ (Fed Funds futures) reconstruction.

Batch B preserves the qualified ``fedwatch-vs-polymarket`` Fed Funds
deconvolution capability (upstream revision
04b15bcd1e7d9f911630d5f3afcbb0a6945d4115) as an *optional* MarketLab path: it
runs only when the user supplies ZQ contract CSVs, and the live archive and
Polymarket history never depend on it. The reconstructed observations are
stored with the distinct method label ``HISTORICAL_ZQ_RECONSTRUCTED`` and the
user's raw dataset is read in place, never copied into MarketLab history.

The algorithm is the unchanged qualified one:

* per calendar month average price from the ZQ contract closes (expired
  contracts use the month-end cutoff);
* FOMC-month classification from the full meeting list, then CME's price
  propagation rule (one step forward, backward chains solved with the
  day-weighted split where the meeting day itself belongs to the pre-meeting
  period, exactly CME's published September 2022 worked example);
* multi-meeting-month internal rates are an explicit approximation flagged on
  the rows;
* each meeting's local distribution is the unchanged CME integer+mantissa
  binary split, and the cumulative distribution is the convolution chain from
  the watch date.

The port is stdlib-only (the retired implementation used pandas; the
MarketLab app-managed environment carries pandas, but the FedWatch fixture
suite must run on a plain interpreter).
"""

from __future__ import annotations

import calendar
import math
import re
from dataclasses import dataclass, field
from datetime import date, datetime
from pathlib import Path

from fedwatch.errors import ZqDataError
from fedwatch.investing import BP_STEP, local_step_distribution

MONTH_CODES = {
    "F": 1, "G": 2, "H": 3, "J": 4, "K": 5, "M": 6,
    "N": 7, "Q": 8, "U": 9, "V": 10, "X": 11, "Z": 12,
}
FILENAME_RE = re.compile(r"^ZQ([FGHJKMNQUVXZ])(\d{2})\.csv$", re.IGNORECASE)
EXPECTED_COLUMNS = [
    "Date Time", "Open", "High", "Low", "Close", "Change", "Volume", "Open Interest",
]
DEFAULT_OI_LOW_CONFIDENCE_THRESHOLD = 1000
SIGNIFICANT_PROB_THRESHOLD = 1e-4


def _to_float(value: str) -> float | None:
    text = (value or "").strip().replace(",", "")
    if not text:
        return None
    try:
        parsed = float(text)
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) else None


def parse_contract_filename(path: Path) -> tuple[str, int, int]:
    """Parse ``ZQ<month code><yy>.csv`` into ``(symbol, month, year)``."""
    match = FILENAME_RE.match(path.name)
    if not match:
        raise ZqDataError(
            "FEDWATCH_ZQ_DATA_INVALID",
            f"file name does not match the expected ZQ<month code><yy>.csv pattern: {path.name}",
        )
    month_code, year_suffix = match.group(1).upper(), match.group(2)
    return f"ZQ{month_code}{year_suffix}", MONTH_CODES[month_code], 2000 + int(year_suffix)


def load_contract_file(
    path: Path,
    oi_low_confidence_threshold: int = DEFAULT_OI_LOW_CONFIDENCE_THRESHOLD,
) -> tuple[list[dict], dict]:
    """Read one ZQ contract CSV into standardized rows.

    The export carries a ``Symbol: ...`` metadata line, a header line, data
    rows and sometimes a footnote (or the literal ``No data to export``);
    unreadable rows are skipped and counted, never repaired. A wrong column
    structure is an explicit invalid-data failure.
    """
    symbol, month, year = parse_contract_filename(path)
    try:
        text = path.read_text(encoding="utf-8-sig", errors="replace")
    except OSError as exc:
        raise ZqDataError(
            "FEDWATCH_ZQ_DATA_UNAVAILABLE",
            f"ZQ contract file cannot be read: {path.name}: {exc}",
            detail={"file": path.name},
        ) from exc

    lines = text.splitlines()
    report = {
        "file": path.name,
        "rows": 0,
        "skipped_rows": 0,
        "close_missing_rows": 0,
        "low_confidence_rows": 0,
    }
    header_index = 1
    while header_index < len(lines) and not lines[header_index].strip():
        header_index += 1
    if header_index >= len(lines):
        report["empty"] = True
        return [], report

    header = [column.strip() for column in lines[header_index].split(",")]
    if header[: len(EXPECTED_COLUMNS)] != EXPECTED_COLUMNS:
        raise ZqDataError(
            "FEDWATCH_ZQ_DATA_INVALID",
            f"unexpected column structure in {path.name}: {header!r}",
            detail={"file": path.name, "header": header},
        )

    rows: list[dict] = []
    for line in lines[header_index + 1:]:
        if not line.strip():
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < len(EXPECTED_COLUMNS):
            report["skipped_rows"] += 1
            continue
        try:
            day = datetime.strptime(fields[0], "%Y-%m-%d").date()
        except ValueError:
            report["skipped_rows"] += 1
            continue
        close_price = _to_float(fields[4])
        if close_price is None:
            report["close_missing_rows"] += 1
        volume = _to_float(fields[6]) or 0.0
        open_interest = _to_float(fields[7]) or 0.0
        low_confidence = volume == 0 and open_interest < oi_low_confidence_threshold
        if low_confidence:
            report["low_confidence_rows"] += 1
        rows.append(
            {
                "contract_symbol": symbol,
                "contract_month": month,
                "contract_year": year,
                "date": day,
                "close_price": close_price,
                "volume": volume,
                "open_interest": open_interest,
                "low_confidence": low_confidence,
            }
        )
    report["rows"] = len(rows)
    return rows, report


def load_contracts(
    data_dir: str | Path,
    oi_low_confidence_threshold: int = DEFAULT_OI_LOW_CONFIDENCE_THRESHOLD,
) -> tuple[list[dict], dict]:
    """Read every ZQ contract file in ``data_dir`` into one sorted row list."""
    directory = Path(data_dir)
    if not directory.is_dir():
        raise ZqDataError(
            "FEDWATCH_ZQ_DATA_UNAVAILABLE",
            f"ZQ data directory does not exist: {directory}",
            detail={"data_dir": str(directory)},
        )
    files = sorted(
        path for path in directory.iterdir()
        if path.is_file() and FILENAME_RE.match(path.name)
    )
    if not files:
        raise ZqDataError(
            "FEDWATCH_ZQ_DATA_UNAVAILABLE",
            f"no ZQ<month code><yy>.csv files were found in {directory}",
            detail={"data_dir": str(directory)},
        )

    contracts: list[dict] = []
    file_reports: list[dict] = []
    skipped_files: list[dict] = []
    for path in files:
        try:
            rows, report = load_contract_file(path, oi_low_confidence_threshold)
        except ZqDataError as exc:
            skipped_files.append({"file": path.name, "code": exc.code, "detail": exc.message})
            continue
        contracts.extend(rows)
        file_reports.append(report)
    if not contracts:
        raise ZqDataError(
            "FEDWATCH_ZQ_DATA_INVALID",
            "no usable ZQ contract rows were loaded",
            detail={"data_dir": str(directory), "skipped_files": skipped_files},
        )
    contracts.sort(key=lambda row: (row["contract_year"], row["contract_month"], row["date"]))
    report = {
        "data_dir": str(directory),
        "files_read": len(file_reports),
        "skipped_files": skipped_files,
        "rows": len(contracts),
        "low_confidence_rows": sum(item["low_confidence_rows"] for item in file_reports),
        "close_missing_rows": sum(item["close_missing_rows"] for item in file_reports),
        "files": file_reports,
    }
    return contracts, report


# ── price construction and propagation (unchanged qualified algebra) ───────


def _add_months(year: int, month: int, delta: int) -> tuple[int, int]:
    index = (year * 12 + (month - 1)) + delta
    return index // 12, index % 12 + 1


def close_date_uncertain(subset: list[dict], watch_date: date) -> bool:
    """Use the same latest-close window for lookup and deconvolution guards."""
    if not subset or not any(row.get("undated_rejected_row_count", 0) for row in subset):
        return False
    year, month = subset[0]["contract_year"], subset[0]["contract_month"]
    cutoff = watch_date if (year, month) >= (watch_date.year, watch_date.month) else date(
        year, month, calendar.monthrange(year, month)[1])
    eligible = [row for row in subset if row["date"] <= cutoff]
    if not eligible:
        return False
    latest = max(row["date"] for row in eligible).isoformat()
    for row in subset:
        if not row.get("undated_rejected_row_count", 0):
            continue
        if not row.get("date_ordered", False) or "date_uncertainty_windows" not in row:
            return True
        for window in row["date_uncertainty_windows"]:
            if ((window["older_date"] is None or window["older_date"] <= latest) and
                    (window["newer_date"] is None or latest <= window["newer_date"])):
                return True
    return False


def month_avg_price(contracts: list[dict], year: int, month: int, watch_date: date) -> float:
    """Average-priced close for one contract month as of ``watch_date``.

    An active month uses the last close on/before the watch date; an already
    expired month uses the last close on/before the month's final day.
    """
    subset = [
        row for row in contracts
        if row["contract_year"] == year and row["contract_month"] == month
    ]
    if not subset:
        raise ValueError(f"no contract data for {year}-{month:02d}")
    if close_date_uncertain(subset, watch_date):
        raise ValueError(f"contract {year}-{month:02d} has an undated rejected observation; close timing is uncertain")

    watch_period_start = date(watch_date.year, watch_date.month, 1)
    contract_period_start = date(year, month, 1)
    if contract_period_start >= watch_period_start:
        cutoff = watch_date
    else:
        cutoff = date(year, month, calendar.monthrange(year, month)[1])

    eligible = [row for row in subset if row["date"] <= cutoff]
    if not eligible:
        raise ValueError(f"no contract data for {year}-{month:02d} on or before {cutoff}")
    eligible.sort(key=lambda row: row["date"])
    close_price = eligible[-1].get("close_price")
    if eligible[-1].get("close_status") == "REJECTED":
        raise ValueError(f"latest close for {year}-{month:02d} on or before {cutoff} was rejected")
    if close_price is None or not math.isfinite(close_price):
        raise ValueError(
            f"latest close for {year}-{month:02d} on or before {cutoff} is missing"
        )
    return float(close_price)


@dataclass
class MonthRecord:
    year: int
    month: int
    meeting_end_dates: list = field(default_factory=list)
    p_avg: float | None = None
    p_start: float | None = None
    p_end: float | None = None
    resolved_via_approximation: bool = False
    segment_rates: list = field(default_factory=list)

    @property
    def is_fomc_month(self) -> bool:
        return len(self.meeting_end_dates) > 0

    @property
    def multi_meeting_month(self) -> bool:
        return len(self.meeting_end_dates) > 1


def build_month_frame(
    watch_date: date,
    meeting_end_dates: list[date],
    contracts: list[dict],
    final_month: tuple[int, int] | None = None,
) -> tuple[list[MonthRecord], list[str]]:
    """One month record per calendar month from the watch month to the last meeting.

    The full meeting list must be supplied, not a watch-date-filtered one: a
    meeting earlier in the watch month still marks that month as an FOMC month.
    Months without contract data keep ``p_avg=None`` and are reported.
    """
    horizon = sorted(day for day in meeting_end_dates if day >= watch_date)
    if not horizon:
        raise ValueError("no upcoming FOMC meetings on or after the watch date")
    last_meeting_date = horizon[-1]
    all_dates = sorted(meeting_end_dates)

    months: list[MonthRecord] = []
    warnings: list[str] = []
    year, month = watch_date.year, watch_date.month
    while (year, month) <= (final_month or (last_meeting_date.year, last_meeting_date.month)):
        record = MonthRecord(year=year, month=month)
        record.meeting_end_dates = [
            day for day in all_dates if day.year == year and day.month == month
        ]
        try:
            record.p_avg = month_avg_price(contracts, year, month, watch_date)
        except ValueError as exc:
            warnings.append(f"missing ZQ contract data for {year}-{month:02d}: {exc}")
        months.append(record)
        year, month = _add_months(year, month, 1)
    return months, warnings


def _days_in_month(record: MonthRecord) -> int:
    return calendar.monthrange(record.year, record.month)[1]


def _solve_month(record: MonthRecord) -> None:
    days_no = _days_in_month(record)
    meeting_day = record.meeting_end_dates[0].day if record.meeting_end_dates else days_no
    days_after = days_no - meeting_day
    days_before = days_no - days_after
    if record.p_avg is None or record.p_end is None or days_before <= 0:
        return
    record.p_start = (record.p_avg * days_no - days_after * record.p_end) / days_before


def _solve_multi_meeting_month(record: MonthRecord) -> None:
    if record.p_start is None and record.p_end is None:
        return
    if record.p_avg is None:
        return
    days_no = _days_in_month(record)
    days = [day.day for day in record.meeting_end_dates]
    segment_days = [days[0]] + [days[index + 1] - days[index] for index in range(len(days) - 1)] + [
        days_no - days[-1]
    ]

    if record.p_start is None and record.p_end is not None:
        record.p_start = record.p_avg + (record.p_avg - record.p_end) * (
            segment_days[-1] / max(sum(segment_days[:-1]), 1)
        )
    if record.p_end is None and record.p_start is not None:
        record.p_end = record.p_avg + (record.p_avg - record.p_start) * (
            segment_days[0] / max(sum(segment_days[1:]), 1)
        )

    n_meetings = len(record.meeting_end_dates)
    if record.p_start is None or record.p_end is None:
        return
    if n_meetings == 1:
        record.segment_rates = [record.p_end]
    else:
        interior_days = sum(segment_days[1:-1])
        if interior_days > 0:
            interior_rate = (
                record.p_avg * days_no
                - segment_days[0] * record.p_start
                - segment_days[-1] * record.p_end
            ) / interior_days
        else:
            interior_rate = (record.p_start + record.p_end) / 2
        record.segment_rates = [interior_rate] * (n_meetings - 1) + [record.p_end]
    record.resolved_via_approximation = True


def propagate_prices(months: list[MonthRecord]) -> tuple[list[MonthRecord], list[str]]:
    """Fill each month's start/end price with the unchanged CME rule."""
    warnings: list[str] = []
    count = len(months)
    for record in months:
        if not record.is_fomc_month:
            record.p_start = record.p_avg
            record.p_end = record.p_avg

    for index in range(1, count - 1):
        record = months[index]
        if not record.is_fomc_month:
            continue
        previous_end = months[index - 1].p_end
        if record.p_start is None and previous_end is not None:
            record.p_start = previous_end

    for index in range(count - 2, -1, -1):
        record = months[index]
        if not record.is_fomc_month:
            continue
        next_start = months[index + 1].p_start
        if record.p_end is None and next_start is not None:
            record.p_end = next_start
        if record.p_start is None and not record.multi_meeting_month:
            _solve_month(record)

    for record in months:
        if record.multi_meeting_month and not record.segment_rates:
            _solve_multi_meeting_month(record)
            if record.resolved_via_approximation:
                warnings.append(
                    f"{record.year}-{record.month:02d}: multi-meeting month internal rates "
                    f"approximated (no exact solution from the month average alone)"
                )

    unresolved = [
        f"{record.year}-{record.month:02d}"
        for record in months
        if record.p_start is None or record.p_end is None
    ]
    if unresolved:
        warnings.append("price propagation incomplete for month(s): " + ", ".join(unresolved))
    return months, warnings


def _convolve(dist_a: dict, dist_b: dict) -> dict:
    out: dict = {}
    for bp_a, prob_a in dist_a.items():
        for bp_b, prob_b in dist_b.items():
            bp = bp_a + bp_b
            out[bp] = out.get(bp, 0.0) + prob_a * prob_b
    return out


def run_deconvolution(
    watch_date: date,
    meeting_end_dates: list[date],
    contracts: list[dict],
    current_rate_upper: float,
    current_rate_lower: float,
    selected_meeting: date | None = None,
) -> dict:
    """Run the full qualified deconvolution for one watch date.

    Returns ``{"rows": [...], "report": {...}}`` with the qualified row shape
    (``row_type`` ``local``/``cumulative``, ``local_bp_change``,
    ``probability_pct``, ``rate_low``/``rate_high`` for cumulative rows and the
    approximation flags). Meetings whose month lacks the one-contract-month
    backward buffer are excluded rather than silently emitted as missing.
    """
    horizon = sorted(day for day in meeting_end_dates if day >= watch_date)
    if selected_meeting is not None:
        horizon = [day for day in horizon if day <= selected_meeting]
    if not horizon:
        raise ZqDataError(
            "FEDWATCH_ZQ_RECONSTRUCTION_INCOMPLETE",
            "no upcoming FOMC meetings on or after the watch date",
            detail={"watch_date": watch_date.isoformat()},
        )
    if not contracts:
        raise ZqDataError(
            "FEDWATCH_ZQ_RECONSTRUCTION_INCOMPLETE",
            "no ZQ contract rows were supplied",
            detail={"watch_date": watch_date.isoformat()},
        )

    max_contract_period = max(
        (row["contract_year"], row["contract_month"]) for row in contracts
    )
    buffer_skipped = [
        day.isoformat()
        for day in horizon
        if (day.year, day.month) >= max_contract_period
    ]
    trimmed = [
        day for day in horizon if (day.year, day.month) < max_contract_period
    ]
    if not trimmed:
        raise ZqDataError(
            "FEDWATCH_ZQ_RECONSTRUCTION_INCOMPLETE",
            "no meeting retains a contract month after it to solve the backward "
            "propagation; the supplied ZQ data is insufficient",
            detail={
                "watch_date": watch_date.isoformat(),
                "max_contract_period": list(max_contract_period),
            },
        )

    # The bounded selected-meeting path includes its following contract month
    # as a propagation anchor while retaining the full calendar classification.
    # The original all-meeting path and the propagation formula are unchanged.
    final_month = _add_months(selected_meeting.year, selected_meeting.month, 1) if selected_meeting else None
    required_end = final_month or (horizon[-1].year, horizon[-1].month)
    required_symbols = {row["contract_symbol"] for row in contracts
        if (watch_date.year, watch_date.month) <= (row["contract_year"], row["contract_month"]) <= required_end}
    uncertain_contracts = sorted(symbol for symbol in required_symbols if close_date_uncertain(
        [row for row in contracts if row["contract_symbol"] == symbol], watch_date))
    if uncertain_contracts:
        raise ZqDataError("FEDWATCH_ZQ_DATE_UNCERTAIN",
                          "required monthly contract has undated rejected observations; reconstruction timing is uncertain",
                          detail={"watch_date": watch_date.isoformat(), "contracts": uncertain_contracts})
    months, build_warnings = build_month_frame(watch_date, meeting_end_dates, contracts, final_month=final_month)
    months, propagate_warnings = propagate_prices(months)
    month_lookup = {(record.year, record.month): record for record in months}

    rows: list[dict] = []
    skipped_meetings: list[dict] = []
    cumulative: dict = {0: 1.0}
    for ordinal, meeting_date in enumerate(trimmed, start=1):
        record = month_lookup[(meeting_date.year, meeting_date.month)]
        if record.multi_meeting_month and record.segment_rates:
            index_in_month = record.meeting_end_dates.index(meeting_date)
            segment_start = (
                record.p_start if index_in_month == 0
                else record.segment_rates[index_in_month - 1]
            )
            segment_end = record.segment_rates[index_in_month]
        else:
            segment_start, segment_end = record.p_start, record.p_end
        if segment_start is None or segment_end is None:
            skipped_meetings.append(
                {
                    "meeting_date": meeting_date.isoformat(),
                    "reason": "PRICE_PROPAGATION_INCOMPLETE",
                }
            )
            continue

        change = (segment_start - segment_end) / BP_STEP * 100.0
        local_dist = local_step_distribution(change)
        cumulative = _convolve(cumulative, local_dist)
        multi_outcome = (
            sum(1 for probability in cumulative.values() if probability > SIGNIFICANT_PROB_THRESHOLD) > 2
        )

        for bp, probability in sorted(cumulative.items()):
            rows.append(
                {
                    "watch_date": watch_date.isoformat(),
                    "meeting_date": meeting_date.isoformat(),
                    "meeting_ordinal": ordinal,
                    "row_type": "cumulative",
                    "cumulative_bp_change": bp,
                    "local_bp_change": None,
                    "rate_low": round(current_rate_lower + bp / 100.0, 4),
                    "rate_high": round(current_rate_upper + bp / 100.0, 4),
                    "probability_pct": round(probability * 100.0, 6),
                    "multi_outcome_flag": multi_outcome,
                    "multi_meeting_month": record.multi_meeting_month,
                    "approximated_month_split": record.resolved_via_approximation,
                }
            )
        for bp, probability in sorted(local_dist.items()):
            rows.append(
                {
                    "watch_date": watch_date.isoformat(),
                    "meeting_date": meeting_date.isoformat(),
                    "meeting_ordinal": ordinal,
                    "row_type": "local",
                    "cumulative_bp_change": None,
                    "local_bp_change": bp,
                    "rate_low": None,
                    "rate_high": None,
                    "probability_pct": round(probability * 100.0, 6),
                    "multi_outcome_flag": False,
                    "multi_meeting_month": record.multi_meeting_month,
                    "approximated_month_split": record.resolved_via_approximation,
                }
            )

    rows = [row for row in rows if row["probability_pct"] > SIGNIFICANT_PROB_THRESHOLD * 100.0]
    report = {
        "watch_date": watch_date.isoformat(),
        "meeting_count": len(trimmed),
        "row_count": len(rows),
        "skipped_meetings": skipped_meetings,
        "buffer_skipped_meetings": buffer_skipped,
        "multi_meeting_months": sorted(
            f"{record.year}-{record.month:02d}"
            for record in months if record.multi_meeting_month
        ),
        "approximated_months": sorted(
            f"{record.year}-{record.month:02d}"
            for record in months if record.resolved_via_approximation
        ),
        "warnings": build_warnings + propagate_warnings,
    }
    return {"rows": rows, "report": report}
