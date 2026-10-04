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
import os
import re
import tempfile
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
PROFILE_CALENDAR_FILENAME = "fomc_dates_fallback.csv"
FALLBACK_METADATA_PREFIX = "#"
# The tracked snapshot is a point-in-time capture. Once it is older than this
# many days it no longer establishes which meeting dates are officially
# scheduled (a new meeting can be announced between captures), so the
# composite must treat the calendar as uncertain rather than authoritative.
# A capture timestamp materially in the future is equally untrustworthy.
FALLBACK_MAX_AGE_DAYS = 30
FALLBACK_MAX_FUTURE_DAYS = 1

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
# A meeting row container class token; excludes fomc-meeting__month/date/minutes
# and the fomc-meeting--shaded modifier.
_CLASS_ATTRIBUTE_RE = re.compile(r'\bclass\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|([^\s>]+))', re.I)

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
        self.skipped_rows = 0
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
            if self.meeting is not None:
                self.skipped_rows += 1
                self.warnings.append("FOMC incomplete meeting row skipped at next meeting boundary")
                self.capture_stack = []
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
            self.skipped_rows += 1
            self.warnings.append(
                f"FOMC meeting row without a year heading skipped ({meeting['month']!r})"
            )
            return
        try:
            parsed = parse_meeting_row(meeting["month"], meeting["date"], meeting["year"])
        except ValueError as exc:
            self.skipped_rows += 1
            self.warnings.append(
                f"FOMC meeting row skipped ({meeting['month']!r} {meeting['date']!r}): {exc}"
            )
            return

        for href in meeting["hrefs"]:
            link_match = STATEMENT_LINK_RE.search(href)
            if link_match:
                try:
                    link_date = datetime.strptime(link_match.group(1), "%Y%m%d").date()
                except ValueError:
                    self.skipped_rows += 1
                    self.warnings.append("FOMC meeting row skipped: invalid statement-link date")
                    return
                parsed["end_date"] = link_date
                break

        if parsed["end_date"] < parsed["start_date"]:
            self.skipped_rows += 1
            self.warnings.append("FOMC meeting row skipped: end precedes start")
            return

        parsed["source"] = "scrape"
        self.rows.append(parsed)


def parse_fomc_calendar(html: str) -> tuple[list[dict], list[str], dict]:
    """Parse the FOMC calendar HTML into meeting rows.

    Returns ``(rows, warnings, report)``. Rows are sorted by
    ``(start_date, end_date)`` and de-duplicated. An HTML body with no meeting
    rows raises ``ValueError``.

    The report records whether the parse was structurally complete: the number
    of ``fomc-meeting`` row markers found in the raw HTML must equal the rows
    that were parsed plus the rows that were explicitly skipped, no row may be
    left open at end of document, and at least one row must have parsed. Since
    the calendar is the authoritative meeting identity, incomplete coverage
    cannot disprove absent dates. Successfully parsed rows remain live evidence.
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

    row_marker_count = sum("fomc-meeting" in next((group for group in match.groups() if group is not None), "").split()
                           for match in _CLASS_ATTRIBUTE_RE.finditer(html))
    by_end = {}
    conflicts = set()
    for row in deduplicated:
        prior = by_end.get(row["end_date"])
        if prior is not None and prior != row:
            conflicts.add(row["end_date"])
        by_end[row["end_date"]] = row
    if conflicts:
        parser.warnings.append("FOMC conflicting meeting identities rejected: " + ", ".join(d.isoformat() for d in sorted(conflicts)))
        deduplicated = [row for row in deduplicated if row["end_date"] not in conflicts]
    accounted_rows = len(parser.rows) + parser.skipped_rows
    document_closed = bool(re.search(r"</html\s*>", html, re.IGNORECASE))
    report = {
        "row_marker_count": row_marker_count,
        "parsed_row_count": len(parser.rows),
        "skipped_row_count": parser.skipped_rows,
        "open_meeting_at_eof": parser.meeting is not None or bool(parser.capture_stack),
        "document_closed": document_closed,
        "conflicting_end_dates": sorted(day.isoformat() for day in conflicts),
        "structurally_complete": bool(
            row_marker_count == accounted_rows
            and parser.skipped_rows == 0
            and parser.meeting is None
            and not parser.capture_stack
            and document_closed
            and parser.rows
            and not conflicts
        ),
    }
    return deduplicated, parser.warnings, report


def load_fallback_snapshot(path: Path | None = None) -> tuple[list[dict], str | None]:
    """Load the tracked fallback snapshot.

    The file may carry a leading ``# snapshot_retrieved_at=...`` metadata line
    recording when the snapshot was captured; it is returned so callers can
    report the fallback's age truthfully.
    """
    path = path if path is not None else FALLBACK_PATH
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
    except (OSError, UnicodeError) as exc:
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
        except (KeyError, ValueError, TypeError, AttributeError) as exc:
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
    identities = {}
    for row in rows:
        key = row["end_date"]
        identity = (row["start_date"], row["meeting_type"], row["has_projection_materials"])
        if row["start_date"] > key or (key in identities and identities[key] != identity):
            raise FedwatchError(PROVIDER_FOMC_CALENDAR, "FOMC_CALENDAR_UNAVAILABLE",
                                "fallback snapshot contains reversed or conflicting meeting identities")
        identities[key] = identity
    rows.sort(key=lambda item: (item["start_date"], item["end_date"]))
    return rows, snapshot_retrieved_at


def profile_calendar_path(db_path: Path | None = None) -> Path | None:
    """Resolve beside the history DB without opening it or creating folders."""
    if db_path is None:
        from fedwatch.store import default_db_path, HistoryStoreError
        try:
            db_path = default_db_path()
        except HistoryStoreError:
            return None
    return Path(db_path).parent / PROFILE_CALENDAR_FILENAME


def snapshot_csv(rows: list[dict], captured_at: str) -> str:
    timeutil.parse_iso_z(captured_at)
    stream = io.StringIO()
    stream.write("# snapshot_retrieved_at=" + captured_at + "\n")
    writer = csv.writer(stream, lineterminator="\n")
    writer.writerow(["start_date", "end_date", "meeting_type", "has_projection_materials"])
    for row in rows:
        serialized = serialize_meeting(row)
        writer.writerow([serialized["start_date"], serialized["end_date"], serialized["meeting_type"],
                         str(serialized["has_projection_materials"]).lower()])
    return stream.getvalue()


def save_profile_calendar(calendar: dict | None, path: Path) -> dict:
    """Durable collect only: atomically save a complete live capture, never a merge."""
    if not calendar or calendar.get("source_status") != "SCRAPED" or not calendar.get("coverage_complete") or not (
            calendar.get("parse_report") or {}).get("structurally_complete"):
        return {"status": "NOT_WRITTEN", "reason": "NO_COMPLETE_LIVE_CAPTURE"}
    captured_at = calendar["retrieved_at"]
    capture = timeutil.parse_iso_z(captured_at)
    if path.exists():
        try:
            _, previous = load_fallback_snapshot(path)
            if previous and timeutil.parse_iso_z(previous) >= capture:
                return {"status": "NOT_WRITTEN", "reason": "EXISTING_CAPTURE_AS_NEW_OR_NEWER"}
        except FedwatchError:
            pass
    contents = snapshot_csv(calendar["meetings"], captured_at)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="", dir=path.parent,
                                         prefix="fomc-calendar-", suffix=".tmp", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(contents)
        load_fallback_snapshot(temporary)
        os.replace(temporary, path)
    finally:
        if temporary and temporary.exists():
            temporary.unlink()
    return {"status": "WRITTEN", "captured_at": captured_at, "meeting_rows": len(calendar["meetings"])}


def _newest_fallback(path, profile_path, clock, warnings):
    candidates, failure = [], None
    for candidate, profile in ((path, False), (profile_path, True)):
        if candidate is None or (profile and not candidate.exists()):
            continue
        try:
            rows, captured = load_fallback_snapshot(candidate)
            instant = timeutil.parse_iso_z(captured) if captured else None
            if profile and (instant is None or (instant - clock()).total_seconds() > FALLBACK_MAX_FUTURE_DAYS * 86400):
                raise FedwatchError(PROVIDER_FOMC_CALENDAR, "FOMC_CALENDAR_UNAVAILABLE",
                                    "profile calendar capture time is missing, invalid or future-dated")
            candidates.append((instant or datetime.min.replace(tzinfo=clock().tzinfo), rows, captured, profile))
        except FedwatchError as exc:
            if profile:
                warnings.append("Unreadable profile calendar copy ignored: " + exc.message)
            else:
                failure = exc
    if not candidates:
        raise failure or FedwatchError(PROVIDER_FOMC_CALENDAR, "FOMC_CALENDAR_UNAVAILABLE", "no readable calendar backup")
    _, rows, captured, profile = max(candidates, key=lambda candidate: candidate[0])
    if profile:
        warnings.append("Newest readable fallback is the profile calendar copy")
    return rows, captured, profile


def fetch_calendar(transport: Transport, fallback_path: Path | None = None, clock=timeutil.utc_now,
                   profile_path: Path | None = None) -> dict:
    """Retrieve the FOMC calendar: scrape first, tracked fallback snapshot second.

    The returned payload states which path produced the data
    (``source_status`` = ``SCRAPED`` or ``FALLBACK_SNAPSHOT``), includes the
    fallback snapshot's capture time when it was used, and carries the live
    parse report. A fallback is never reported as a live scrape.

    A partial live parse is supplemented by readable tracked snapshot rows.
    Live identities win; replacements and per-row provenance remain explicit.
    Stale/unknown-age backup rows are labelled and cannot grant complete
    coverage. An unreadable snapshot leaves live rows as SCRAPED_PARTIAL.
    """
    path = fallback_path or FALLBACK_PATH
    profile_path = profile_path if profile_path is not None else profile_calendar_path()
    warnings: list[str] = []
    live_parse_report: dict | None = None
    live_rows = []
    try:
        html = transport.get_text(CALENDAR_URL, headers=REQUEST_HEADERS, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        warnings.append(f"FOMC calendar request failed: {exc}")
        html = None

    if html is not None:
        try:
            live_rows, parse_warnings, live_parse_report = parse_fomc_calendar(html)
            warnings.extend(parse_warnings)
            if live_rows:
                retrieved_at = clock()
                complete = live_parse_report["structurally_complete"]
                if not complete:
                    warnings.append("FOMC calendar live coverage is structurally incomplete; valid live rows retained")
                live_result = {
                    "retrieved_at": timeutil.iso_z(retrieved_at),
                    "source": SOURCE_LABEL_SCRAPE,
                    "source_status": "SCRAPED" if complete else "SCRAPED_PARTIAL",
                    "coverage_complete": complete,
                    "fallback_snapshot_retrieved_at": None,
                    "fallback_age_days": None,
                    "fallback_stale": False,
                    "meetings": live_rows,
                    "parse_report": live_parse_report,
                    "warnings": warnings,
                    "fallback_used": False,
                    "merge_conflicts": [],
                }
                if complete:
                    return live_result
        except ValueError as exc:
            warnings.append(f"FOMC calendar parse failed: {exc}")

    try:
        rows, snapshot_retrieved_at, profile_used = _newest_fallback(path, profile_path, clock, warnings)
    except FedwatchError as exc:
        if not live_rows:
            raise
        warnings.append(f"FOMC partial live coverage could not be supplemented: {exc}")
        return live_result
    retrieved_at = clock()
    fallback_age_days = None
    if snapshot_retrieved_at:
        try:
            snapshot_instant = timeutil.parse_iso_z(snapshot_retrieved_at)
            fallback_age_days = (retrieved_at - snapshot_instant).total_seconds() / 86400.0
        except ValueError:
            fallback_age_days = None
    fallback_stale = (
        fallback_age_days is None
        or fallback_age_days > FALLBACK_MAX_AGE_DAYS
        or fallback_age_days < -FALLBACK_MAX_FUTURE_DAYS
    )
    if fallback_stale:
        warnings.append(
            "FOMC fallback snapshot age is "
            + ("unknown" if fallback_age_days is None else f"{fallback_age_days:.1f} days")
            + f" (accepted window -{FALLBACK_MAX_FUTURE_DAYS} to "
            f"{FALLBACK_MAX_AGE_DAYS} days); the calendar is not authoritative "
            "for meeting-date alignment"
        )
    if live_rows:
        conflicts = []
        ambiguous_dates = set(live_parse_report["conflicting_end_dates"])
        merged = []
        identity_fields = ("start_date", "end_date", "meeting_type", "has_projection_materials")
        for backup in rows:
            if backup["end_date"].isoformat() in ambiguous_dates:
                # A fresh backup does not resolve contradictory live evidence.
                continue
            replacements = [live for live in live_rows if
                            live["end_date"] == backup["end_date"] or
                            (live["start_date"] <= backup["end_date"] and
                             backup["start_date"] <= live["end_date"])]
            if replacements:
                for live in replacements:
                    if any(live[key] != backup[key] for key in identity_fields):
                        conflicts.append({"live": serialize_meeting(live),
                                          "fallback": serialize_meeting(backup),
                                          "resolution": "LIVE_ROW_WINS"})
            else:
                merged.append(dict(backup, stale=fallback_stale))
        merged.extend(live_rows)
        merged.sort(key=lambda row: (row["start_date"], row["end_date"]))
        warnings.append("FOMC partial live calendar supplemented by " +
                         ("stale/unknown-age" if fallback_stale else "fresh") +
                         (" profile calendar copy" if profile_used else " bundled fallback snapshot") +
                         "; per-row sources retained")
        if conflicts:
            warnings.append(f"FOMC live rows superseded {len(conflicts)} conflicting fallback identities")
        live_result.update(source=SOURCE_LABEL_SCRAPE + (" + profile calendar copy" if profile_used else " + tracked fallback snapshot"),
                           source_status="SCRAPED_WITH_STALE_FALLBACK" if fallback_stale else "SCRAPED_WITH_FALLBACK",
                           coverage_complete=not ambiguous_dates and not fallback_stale,
                           fallback_stale=fallback_stale,
                           fallback_snapshot_retrieved_at=snapshot_retrieved_at,
                           fallback_age_days=fallback_age_days, fallback_used=True,
                            meetings=merged, merge_conflicts=conflicts)
        live_result["fallback_origin"] = "PROFILE_COPY" if profile_used else "BUNDLED_FILE"
        return live_result
    return {
        "retrieved_at": timeutil.iso_z(retrieved_at),
        "source": "federalreserve.gov FOMC calendar profile fallback snapshot" if profile_used else SOURCE_LABEL_FALLBACK,
        "fallback_origin": "PROFILE_COPY" if profile_used else "BUNDLED_FILE",
        "source_status": "FALLBACK_STALE" if fallback_stale else "FALLBACK_SNAPSHOT",
        "fallback_snapshot_retrieved_at": snapshot_retrieved_at,
        "fallback_age_days": fallback_age_days,
        "fallback_stale": fallback_stale,
        "coverage_complete": not fallback_stale,
        "meetings": [dict(row, stale=fallback_stale) for row in rows],
        "parse_report": live_parse_report,
        "warnings": warnings,
        "fallback_used": True,
        "merge_conflicts": [],
    }


def serialize_meeting(row: dict) -> dict:
    if isinstance(row["start_date"], date):
        return {
            "start_date": row["start_date"].isoformat(),
            "end_date": row["end_date"].isoformat(),
            "meeting_type": row["meeting_type"],
            "has_projection_materials": row["has_projection_materials"],
            "source": row["source"],
            **({"stale": row["stale"]} if "stale" in row else {}),
        }
    return dict(row)


def authoritative_upcoming(result: dict, as_of: date) -> list[dict]:
    """Positive live identities survive an old supplement; old rows do not
    gain authority from their neighbours or establish complete coverage.
    """
    return [row for row in upcoming_meetings(result["meetings"], as_of)
            if not row.get("stale", result.get("fallback_stale", False) and row.get("source") != "scrape")]


def upcoming_meetings(rows: list[dict], as_of: date) -> list[dict]:
    """Meetings whose end date is on or after ``as_of``, chronologically."""
    return sorted(
        (row for row in rows if row["end_date"] >= as_of),
        key=lambda row: (row["start_date"], row["end_date"]),
    )
