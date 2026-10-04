"""Completed-session rule for daily bars.

A daily bar is stored only when its session had closed when the response was
received. A bar dated today in the exchange's own time zone, retrieved before
that exchange's regular close plus a settling buffer, is an in-progress bar:
its close and volume will still change, so it is dropped and counted rather
than stored and later revised.

The close times are the regular sessions of the exchanges the research
universe reaches. An unknown suffix uses the most conservative rule: any bar
dated on or after the local calendar date of the retrieval is in progress.
"""

from __future__ import annotations

import datetime as _dt

try:  # Python 3.9+: zoneinfo needs the tzdata package on Windows.
    from zoneinfo import ZoneInfo
except ImportError:  # pragma: no cover - interpreter without zoneinfo
    ZoneInfo = None  # type: ignore[assignment]

SETTLE_MINUTES = 30

# suffix -> (IANA zone, regular close hour, minute)
_EXCHANGES = {
    "": ("America/New_York", 16, 0),
    ".BK": ("Asia/Bangkok", 16, 30),
    ".TW": ("Asia/Taipei", 13, 30),
    ".KS": ("Asia/Seoul", 15, 30),
    ".KQ": ("Asia/Seoul", 15, 30),
    ".HK": ("Asia/Hong_Kong", 16, 10),
    ".L": ("Europe/London", 16, 35),
    ".DE": ("Europe/Berlin", 17, 35),
    ".PA": ("Europe/Paris", 17, 35),
    ".AS": ("Europe/Amsterdam", 17, 35),
    ".MC": ("Europe/Madrid", 17, 35),
    ".SW": ("Europe/Zurich", 17, 30),
    ".SS": ("Asia/Shanghai", 15, 0),
    ".SZ": ("Asia/Shanghai", 15, 0),
    ".T": ("Asia/Tokyo", 15, 30),
    ".NS": ("Asia/Kolkata", 15, 30),
    ".TO": ("America/Toronto", 16, 0),
    ".AX": ("Australia/Sydney", 16, 10),
    ".SA": ("America/Sao_Paulo", 17, 0),
}


def exchange_rule(symbol: str):
    """(zone, close hour, close minute, known) for a Yahoo symbol."""
    sym = symbol.upper()
    if sym.startswith("^"):
        # Indexes carry their market's suffix (^SET.BK); a bare ^ is U.S.
        sym = sym[1:]
    dot = sym.rfind(".")
    suffix = sym[dot:] if dot > 0 else ""
    if suffix in _EXCHANGES:
        zone, hh, mm = _EXCHANGES[suffix]
        return zone, hh, mm, True
    return "UTC", 0, 0, False


def is_completed_session(symbol: str, session_date: _dt.date, retrieved_at_utc: _dt.datetime) -> bool:
    """True when the session dated ``session_date`` had closed (plus buffer) at retrieval."""
    zone, hh, mm, known = exchange_rule(symbol)
    if ZoneInfo is None:
        # Without time-zone data be conservative: nothing on or after the UTC
        # retrieval date counts as complete.
        return session_date < retrieved_at_utc.date()
    tz = ZoneInfo(zone)
    local_now = retrieved_at_utc.astimezone(tz)
    if not known:
        return session_date < local_now.date()
    close_local = _dt.datetime(session_date.year, session_date.month, session_date.day, hh, mm, tzinfo=tz)
    return local_now >= close_local + _dt.timedelta(minutes=SETTLE_MINUTES)
