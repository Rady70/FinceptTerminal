"""Time helpers for the MarketLab FedWatch backend.

Pure-stdlib on purpose: the deterministic fixture tests run on a plain Python
installation with no third-party packages, and no ``zoneinfo``/``tzdata``
dependency is introduced for the one timezone decision this batch needs
(interpreting Polymarket event end instants as a U.S. Eastern calendar date).

The U.S. Eastern offset is computed from the post-2007 DST rule (second Sunday
in March through the first Sunday in November). That is stable for every year
this capability is currently used with, and it is deterministic without
relying on the host's timezone database.
"""

from __future__ import annotations

from datetime import date, datetime, timedelta, timezone

ISO_Z_FORMAT = "%Y-%m-%dT%H:%M:%SZ"
DATE_FORMAT = "%Y-%m-%d"


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def iso_z(value: datetime) -> str:
    """Render an instant as UTC ISO 8601 seconds precision with a Z suffix."""
    return value.astimezone(timezone.utc).strftime(ISO_Z_FORMAT)


def parse_iso_z(text: str) -> datetime:
    """Parse an ISO 8601 instant (``Z`` suffix or explicit offset) to UTC.

    Accepts the seconds-precision form this package emits plus the fractional
    seconds form Polymarket returns.
    """
    cleaned = text.strip()
    if cleaned.endswith("Z") or cleaned.endswith("z"):
        cleaned = cleaned[:-1] + "+00:00"
    parsed = datetime.fromisoformat(cleaned)
    if parsed.tzinfo is None:
        raise ValueError(f"timestamp has no timezone: {text!r}")
    return parsed.astimezone(timezone.utc)


def parse_date(text: str) -> date:
    return date.fromisoformat(text.strip())


def is_us_dst_us_date(value: date) -> bool:
    """True inside the post-2007 U.S. DST window for that calendar date."""
    march = date(value.year, 3, 1)
    first_sunday_march = march + timedelta(days=(6 - march.weekday()) % 7)
    second_sunday_march = first_sunday_march + timedelta(days=7)
    november = date(value.year, 11, 1)
    first_sunday_november = november + timedelta(days=(6 - november.weekday()) % 7)
    return second_sunday_march <= value < first_sunday_november


def eastern_from_utc(value: datetime) -> datetime:
    """Convert a UTC instant to the U.S. Eastern wall-clock instant."""
    if value.tzinfo is None:
        raise ValueError("eastern_from_utc requires a timezone-aware instant")
    utc = value.astimezone(timezone.utc)
    # The offset is chosen from the local date; the one-hour transition gap is
    # far from the evening deadlines this mapping compares, so the two-pass
    # approach is exact for every date this capability acts on.
    approximate_date = (utc - timedelta(hours=5)).date()
    offset_hours = -4 if is_us_dst_us_date(approximate_date) else -5
    return utc + timedelta(hours=offset_hours)


def eastern_date_from_utc(value: datetime) -> date:
    return eastern_from_utc(value).date()
