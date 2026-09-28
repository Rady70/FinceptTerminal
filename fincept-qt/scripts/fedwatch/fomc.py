"""Federal Reserve FOMC meeting calendar.

Fetches the official calendar page and parses the current and future meeting
schedule. The parsing rules are the ones the qualified component used, ported
to the standard library's :mod:`html.parser` so this package (and its
deterministic fixture suite) has no third-party HTML dependency:

* a year heading ("2026 FOMC Meetings") starts a panel;
* each ``fomc-meeting`` row carries a month and a day range;
* a trailing``*`` on the day range marks projection materials;
* ``(notation vote)`` rows are notation votes on the same day;
* the statement link's ``monetary<YYYYMMDD>`` filename is the exact decision
  date and overrides the text-parsed end date when they differ.

If the scrape fails for any reason (network, 403, HTML change), a tracked
fallback snapshot is loaded and explicitly reported as a fallback, never passed
off as a live scrape. If the fallback is also unusable the provider fails with
``FOMC_CALENDAR_UNAVAILABLE``.
"""

from __future__ import annotations

import csv
import io
import re
from datetime import date, datetime
from html.parser import HTMLParser
from pathlib import Path

from fedwatch import timeutil
from fedwatch.errors import PROVIDER_FOMC_CALENDAR, FedwatchError
from fedwatch.transport import Transport, TransportError

CALENDAR_URL = "https://www.federalreserve.gov/monetarypolicy/fomccalendars.htm"
SOURCE_LABEL_SCRAPE = CALENDAR_URL
SOURCE_LABEL_FALLBACK = "federalreserve.gov FOMC calendar tracked fallback snapshot"

FALLBACK_PATH = Path(__file__).resolve().parent / "fomc_dates_fallback.csv"
FALLBACK_METADATA_PREFIX = "#"

# federalreserve.gov answers 403 to empty/bot-like User-Agents; the qualified
# implementation used a browser UA for this host only.
REQUEST_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0 Safari/537.36"
    )
}
REQUEST_TIMEOUT = 15

STATEMENT_LINK_RE = re.compile(r"monetary(\d{8})[a-z]?\d*\.(?:htm|pdf)$", re.IGNORECASE)
YEAR_HEADING_RE = re.compile(r"(\d{4})\s+FOMC Meetings", re.IGNORECASE)
NOTATION_VOTE_RE = re.compile(r"^(\d+)\s*\(notation vote\)$", re.IGNORECASE)
DAY_RANGE_RE = re.compile(r"^(\d+)(?:-(\d+))?$")

VOID_ELEMENTS = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "source", "track", "wbr",
}

MEETING_COLUMNS = [
    "start_date", "end_date", "meeting_type", "has_projection_materials", "source",
]


def _month_name_to_num(name: str) -> int:
    cleaned = name.strip()
    for fmt in ("%B", "%b"):
        try:
            return datetime.strptime(cleaned, fmt).month
        except ValueError:
            continue
    raise ValueError(f"unknown month name in the FOMC calendar: {name!r}")


def parse_meeting_row(month_text: str, date_text: str, year: int) -> dict:
    has_projection_materials = "*" in date_text
    date_clean = date_text.replace("*", "").strip()

    notation_match = NOTATION_VOTE_RE.match(date_clean)
    if notation_match:
        meeting_type = "notation_vote"
        day1 = day2 = int(notation_match.group(1))
    else:
        meeting_type = "regular"
        day_match = DAY_RANGE_RE.match(date_clean)
        if not day_match:
            raise ValueError(f"unparseable date field: {date_text!r}")
        day1 = int(day_match.group(1))
        day2 = int(day_match.group(2)) if day_match.group(2) else day1

    months = [month.strip() for month in month_text.split("/")]
    start_month = _month_name_to_num(months[0])
    end_month = _month_name_to_num(months[-1])

    start_year = year
    end_year = year
    if end_month < start_month:
        # A meeting spanning December -> January belongs to the next year.
        end_year = year + 1

    return {
        "start_date": date(start_year, start_month, day1),
        "end_date": date(end_year, end_month, day2),
        "meeting_type": meeting_type,
        "has_projection_materials": has_projection_materials,
    }


class _FomcCalendarParser(HTMLParser):
    """Sequential extractor for the FOMC calendar's meeting rows.

    The page nests each meeting in a ``div.fomc-meeting`` whose month/date
    cells use dedicated classes and whose statement links carry the exact
    decision date. The parser tracks div depth so it can close each meeting row
    without building a full document tree. A truncated document simply loses
    its incomplete final row; nothing is fabricated.
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack: list[tuple[str, set]] = []
        self.current_year: int | None = None
        self.meeting: dict | None = None
        self.meeting_index: int | None = None
        self.capture_stack: list[tuple[str, int]] = []
        self.year_capture_depth: int | None = None
        self.year_text: list[str] = []
        self.rows: list[dict] = []
        self.warnings: list[str] = []

    @staticmethod
    def _classes(attrs) -> set:
        for key, value in attrs:
            if key == "class" and value:
                return set(value.split())
        return set()

    def handle_starttag(self, tag, attrs):
        classes = self._classes(attrs)
        if tag == "a":
            href = dict(attrs).get("href", "")
            if self.meeting is not None and href:
                self.meeting["hrefs"].append(href)
            self.year_capture_depth = len(self.stack)
            self.year_text = []
        if tag not in VOID_ELEMENTS:
            self.stack.append((tag, classes))

        if tag != "div":
            return

        if (
            "fomc-meeting" in classes
            and "fomc-meeting__month" not in classes
            and "fomc-meeting__date" not in classes
        ):
            if self.meeting is None:
                self.meeting = {"month": "", "date": "", "hrefs": [], "year": self.current_year}
                self.meeting_index = len(self.stack) - 1
            return

        if self.meeting is not None:
            if "fomc-meeting__month" in classes:
                self.capture_stack.append(("month", len(self.stack) - 1))
            elif "fomc-meeting__date" in classes:
                self.capture_stack.append(("date", len(self.stack) - 1))

    def handle_endtag(self, tag):
        if tag == "a" and self.year_capture_depth is not None:
            text = " ".join(part.strip() for part in self.year_text).strip()
            year_match = YEAR_HEADING_RE.search(text)
            if year_match:
                self.current_year = int(year_match.group(1))
            self.year_capture_depth = None

        if self.capture_stack and len(self.stack) - 1 <= self.capture_stack[-1][1]:
            self.capture_stack.pop()

        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index][0] == tag:
                del self.stack[index:]
                break

        if (
            self.meeting is not None
            and self.meeting_index is not None
            and len(self.stack) <= self.meeting_index
        ):
            self._finish_meeting()

    def handle_data(self, data):
        text = data.strip()
        if not text:
            return
        if self.year_capture_depth is not None:
            self.year_text.append(data)
        if self.capture_stack and self.meeting is not None:
            kind = self.capture_stack[-1][0]
            self.meeting[kind] = (self.meeting[kind] + " " + text).strip()

    def _finish_meeting(self):
        meeting = self.meeting
        self.meeting = None
        self.meeting_index = None
        self.capture_stack = []
        self.year_text = []
        self.year_capture_depth = None

        if meeting["year"] is None:
            self.warnings.append(
                f"FOMC meeting row without a year heading skipped ({meeting['month']!r})"
            )
            return
        try:
            parsed = parse_meeting_row(meeting["month"], meeting["date"], meeting["year"])
        except ValueError as exc:
            self.warnings.append(
                f"FOMC meeting row skipped ({meeting['month']!r} {meeting['date']!r}): {exc}"
            )
            return

        for href in meeting["hrefs"]:
            link_match = STATEMENT_LINK_RE.search(href)
            if link_match:
                link_date = datetime.strptime(link_match.group(1), "%Y%m%d").date()
                parsed["end_date"] = link_date
                break

        parsed["source"] = "scrape"
        self.rows.append(parsed)


def parse_fomc_calendar(html: str) -> tuple[list[dict], list[str]]:
    """Parse the FOMC calendar HTML into meeting rows.

    Returns ``(rows, warnings)``. Rows are sorted by ``(start_date, end_date)``
    and de-duplicated. An HTML body with no meeting rows raises ``ValueError``;
    the provider layer maps that to an explicit failure unless the tracked
    fallback snapshot is available.
    """
    parser = _FomcCalendarParser()
    parser.feed(html)
    parser.close()

    if not parser.rows:
        raise ValueError("no FOMC meeting data could be extracted from the calendar HTML")

    deduplicated: list[dict] = []
    seen: set[tuple] = set()
    for row in sorted(parser.rows, key=lambda item: (item["start_date"], item["end_date"])):
        key = (row["start_date"], row["end_date"])
        if key in seen:
            continue
        seen.add(key)
        deduplicated.append(row)
    return deduplicated, parser.warnings


def load_fallback_snapshot(path: Path = FALLBACK_PATH) -> tuple[list[dict], str | None]:
    """Load the tracked fallback snapshot.

    The file may carry a leading ``# snapshot_retrieved_at=...`` metadata line
    recording when the snapshot was captured; it is returned so callers can
    report the fallback's age truthfully.
    """
    if not path.exists():
        raise FedwatchError(
            PROVIDER_FOMC_CALENDAR,
            "FOMC_CALENDAR_UNAVAILABLE",
            f"FOMC calendar scrape failed and no fallback snapshot exists at {path}",
        )

    snapshot_retrieved_at: str | None = None
    data_lines: list[str] = []
    try:
        fallback_text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise FedwatchError(
            PROVIDER_FOMC_CALENDAR,
            "FOMC_CALENDAR_UNAVAILABLE",
            f"FOMC fallback snapshot at {path} could not be read: {exc}",
        ) from exc
    for line in fallback_text.splitlines():
        if line.startswith(FALLBACK_METADATA_PREFIX):
            payload = line[len(FALLBACK_METADATA_PREFIX):].strip()
            if payload.startswith("snapshot_retrieved_at="):
                candidate = payload.split("=", 1)[1].strip()
                try:
                    timeutil.parse_iso_z(candidate)
                    snapshot_retrieved_at = candidate
                except ValueError:
                    # Unreadable metadata is reported as absent, not reused.
                    snapshot_retrieved_at = None
            continue
        if line.strip():
            data_lines.append(line)

    if not data_lines:
        raise FedwatchError(
            PROVIDER_FOMC_CALENDAR,
            "FOMC_CALENDAR_UNAVAILABLE",
            f"FOMC fallback snapshot at {path} contains no meeting rows",
        )

    rows: list[dict] = []
    reader = csv.DictReader(io.StringIO("\n".join(data_lines)))
    for raw in reader:
        try:
            rows.append(
                {
                    "start_date": timeutil.parse_date(raw["start_date"]),
                    "end_date": timeutil.parse_date(raw["end_date"]),
                    "meeting_type": raw["meeting_type"],
                    "has_projection_materials": raw["has_projection_materials"].strip().lower()
                    in ("1", "true"),
                    "source": "fallback_snapshot",
                }
            )
        except (KeyError, ValueError) as exc:
            raise FedwatchError(
                PROVIDER_FOMC_CALENDAR,
                "FOMC_CALENDAR_UNAVAILABLE",
                f"FOMC fallback snapshot at {path} is malformed: {exc}",
            ) from exc

    if not rows:
        raise FedwatchError(
            PROVIDER_FOMC_CALENDAR,
            "FOMC_CALENDAR_UNAVAILABLE",
            f"FOMC fallback snapshot at {path} contains no meeting rows",
        )
    rows.sort(key=lambda item: (item["start_date"], item["end_date"]))
    return rows, snapshot_retrieved_at


def fetch_calendar(transport: Transport, fallback_path: Path | None = None, clock=timeutil.utc_now) -> dict:
    """Retrieve the FOMC calendar: scrape first, tracked fallback snapshot second.

    The returned payload states which path produced the data
    (``source_status`` = ``SCRAPED`` or ``FALLBACK_SNAPSHOT``) and includes the
    fallback snapshot's capture time when it was used. A fallback is never
    reported as a live scrape.
    """
    path = fallback_path or FALLBACK_PATH
    warnings: list[str] = []
    try:
        html = transport.get_text(CALENDAR_URL, headers=REQUEST_HEADERS, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        warnings.append(f"FOMC calendar request failed: {exc}")
        html = None

    if html is not None:
        try:
            rows, parse_warnings = parse_fomc_calendar(html)
            warnings.extend(parse_warnings)
            retrieved_at = clock()
            return {
                "retrieved_at": timeutil.iso_z(retrieved_at),
                "source": SOURCE_LABEL_SCRAPE,
                "source_status": "SCRAPED",
                "fallback_snapshot_retrieved_at": None,
                "meetings": rows,
                "warnings": warnings,
            }
        except ValueError as exc:
            warnings.append(f"FOMC calendar parse failed: {exc}")

    rows, snapshot_retrieved_at = load_fallback_snapshot(path)
    retrieved_at = clock()
    return {
        "retrieved_at": timeutil.iso_z(retrieved_at),
        "source": SOURCE_LABEL_FALLBACK,
        "source_status": "FALLBACK_SNAPSHOT",
        "fallback_snapshot_retrieved_at": snapshot_retrieved_at,
        "meetings": rows,
        "warnings": warnings,
    }


def serialize_meeting(row: dict) -> dict:
    if isinstance(row["start_date"], date):
        return {
            "start_date": row["start_date"].isoformat(),
            "end_date": row["end_date"].isoformat(),
            "meeting_type": row["meeting_type"],
            "has_projection_materials": row["has_projection_materials"],
            "source": row["source"],
        }
    return dict(row)


def upcoming_meetings(rows: list[dict], as_of: date) -> list[dict]:
    """Meetings whose end date is on or after ``as_of``, chronologically."""
    return sorted(
        (row for row in rows if row["end_date"] >= as_of),
        key=lambda row: (row["start_date"], row["end_date"]),
    )
