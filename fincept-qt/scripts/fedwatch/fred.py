"""FRED target-range context (DFEDTARU / DFEDTARL).

The Federal Funds target range is retrieved from FRED's public CSV endpoint
(``fredgraph.csv``) — no API key, no credentials. This is the same source and
the same parsing behavior the qualified component used, and it supplies the
current target-range midpoint the Fed-side local-step conversion is seeded
with.

FRED's bot protection tarpits browser-shaped requests from non-browser TLS
fingerprints, so the default ``requests`` User-Agent is used deliberately; do
not add a spoofed browser UA here.
"""

from __future__ import annotations

import math

from fedwatch import timeutil
from fedwatch.errors import PROVIDER_FRED, FedwatchError
from fedwatch.transport import Transport, TransportError

FRED_CSV_URL = "https://fred.stlouisfed.org/graph/fredgraph.csv?id={series_id}"
SOURCE_LABEL = "fred.stlouisfed.org DFEDTARU/DFEDTARL"
REQUEST_TIMEOUT = 15


def parse_fred_csv(text: str) -> list[dict]:
    """Parse a FRED ``fredgraph.csv`` response into ``[{date, value}, ...]``.

    FRED writes ``.`` for missing observations (weekends and holidays in these
    daily series); those rows are skipped. A response with no usable rows, a
    missing header, or unparseable dates raises ``ValueError`` — the caller
    maps it to an explicit provider failure.
    """
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        raise ValueError("FRED response is empty")

    header = [column.strip() for column in lines[0].split(",")]
    if len(header) < 2:
        raise ValueError(f"FRED response has no value column: {lines[0]!r}")

    observations: list[dict] = []
    for line in lines[1:]:
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 2:
            continue
        raw_date, raw_value = fields[0], fields[1]
        if raw_value in ("", "."):
            continue
        observations.append({"date": timeutil.parse_date(raw_date), "value": float(raw_value)})

    if not observations:
        raise ValueError("FRED response contains no numeric observations")
    return observations


def fetch_series(transport: Transport, series_id: str) -> tuple[list[dict], str]:
    url = FRED_CSV_URL.format(series_id=series_id)
    try:
        text = transport.get_text(url, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_SOURCE_UNAVAILABLE",
            f"FRED {series_id} request failed: {exc}",
            detail=exc.detail(),
        ) from exc
    try:
        return parse_fred_csv(text), url
    except ValueError as exc:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            f"FRED {series_id} response could not be parsed: {exc}",
            detail={"series_id": series_id, "url": url},
        ) from exc


def valid_target_range(upper, lower) -> bool:
    """The qualified target-range invariant ``upper > lower >= 0``, finite.

    Batch A's current-range read enforces this; the historical resolution and
    the ZQ import use the same check so a malformed provider pair can never
    become a durable decision or reconstruction input.
    """
    try:
        upper_value = float(upper)
        lower_value = float(lower)
    except (TypeError, ValueError):
        return False
    if not math.isfinite(upper_value) or not math.isfinite(lower_value):
        return False
    return upper_value > lower_value >= 0


def fetch_target_history(transport: Transport, clock=timeutil.utc_now) -> dict:
    """Fetch the full DFEDTARU/DFEDTARL daily series for historical decisions.

    Batch B uses this to establish resolved FOMC meetings' actual target-range
    change (the qualified FedWatch decision convention) without guessing from
    the meeting date. The series are returned chronologically with ISO dates
    and validated finite values; a malformed series is an explicit provider
    failure, never a silently empty history.
    """
    upper_rows, upper_url = fetch_series(transport, "DFEDTARU")
    lower_rows, _lower_url = fetch_series(transport, "DFEDTARL")
    combined = upper_rows + lower_rows
    if not combined:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            "FRED target-range history is empty",
            detail={"url": upper_url},
        )
    invalid = [
        row for row in combined
        if not math.isfinite(row["value"])
    ]
    if invalid:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            "FRED target-range history contains a non-finite value",
            detail={"invalid_row_count": len(invalid)},
        )
    upper_rows.sort(key=lambda row: row["date"])
    lower_rows.sort(key=lambda row: row["date"])
    return {
        "retrieved_at": timeutil.iso_z(clock()),
        "source": SOURCE_LABEL,
        "method": "FRED target-range series (DFEDTARU/DFEDTARL)",
        "upper": [
            {"date": row["date"].isoformat(), "value": row["value"]} for row in upper_rows
        ],
        "lower": [
            {"date": row["date"].isoformat(), "value": row["value"]} for row in lower_rows
        ],
    }


def fetch_target_range(transport: Transport, clock=timeutil.utc_now) -> dict:
    """Fetch the current target range from DFEDTARU/DFEDTARL.

    The current target range must be a single point-in-time pair, so the two
    series' latest observations must fall on the same date; a lagging series
    would otherwise let MarketLab synthesize a range that never existed. The
    range itself must satisfy the previously qualified validity requirement
    ``upper > lower >= 0`` with finite bounds. Any violation raises
    :class:`FedwatchError` (provider ``fred``); no range is synthesized.
    """
    upper_rows, upper_url = fetch_series(transport, "DFEDTARU")
    lower_rows, _lower_url = fetch_series(transport, "DFEDTARL")
    retrieved_at = clock()

    latest_upper = upper_rows[-1]
    latest_lower = lower_rows[-1]
    upper_value = latest_upper["value"]
    lower_value = latest_lower["value"]

    if latest_upper["date"] != latest_lower["date"]:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            "FRED upper and lower series have different latest observation dates; "
            "a point-in-time target range cannot be constructed",
            detail={
                "upper_observation_date": latest_upper["date"].isoformat(),
                "lower_observation_date": latest_lower["date"].isoformat(),
                "same_latest_date": False,
            },
        )

    if (
        not math.isfinite(upper_value)
        or not math.isfinite(lower_value)
        or not (upper_value > lower_value >= 0)
    ):
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            "FRED target range is not a valid upper > lower >= 0 pair",
            detail={
                "upper": upper_value,
                "lower": lower_value,
                "latest_observation_date": latest_upper["date"].isoformat(),
            },
        )

    today = retrieved_at.date()
    if latest_upper["date"] > today:
        raise FedwatchError(
            PROVIDER_FRED,
            "FRED_TARGET_RANGE_INVALID",
            "FRED target range latest observation is dated in the future",
            detail={
                "upper_observation_date": latest_upper["date"].isoformat(),
                "lower_observation_date": latest_lower["date"].isoformat(),
                "retrieved_at": timeutil.iso_z(retrieved_at),
            },
        )

    lower_by_date = {row["date"]: row["value"] for row in lower_rows}
    recent_observations = [
        {
            "date": upper_row["date"].isoformat(),
            "upper": upper_row["value"],
            "lower": lower_by_date[upper_row["date"]],
        }
        for upper_row in upper_rows[-5:]
        if upper_row["date"] in lower_by_date
    ]

    return {
        "retrieved_at": timeutil.iso_z(retrieved_at),
        "source": SOURCE_LABEL,
        "method": "FRED target-range bounds",
        "target_range": {
            "lower": lower_value,
            "upper": upper_value,
        },
        "latest_observation_date": latest_upper["date"].isoformat(),
        "same_latest_date": True,
        "series": {
            "upper": {
                "series_id": "DFEDTARU",
                "observation_date": latest_upper["date"].isoformat(),
                "value": upper_value,
                "url": upper_url,
            },
            "lower": {
                "series_id": "DFEDTARL",
                "observation_date": latest_lower["date"].isoformat(),
                "value": lower_value,
                "url": FRED_CSV_URL.format(series_id="DFEDTARL"),
            },
        },
        "recent_observations": recent_observations,
    }
