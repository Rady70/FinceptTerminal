"""Current FedWatch snapshot orchestration.

Assembles the current-data contract from the four providers. Each provider
failure is caught, attributed, and preserved as its own explicit error entry;
a failure of one provider never changes the reported identity of another, and
no stale value is ever relabeled as current.

The top-level envelope uses the established economics response convention:
``success`` is true when the snapshot was assembled, and when any provider
failed the document additionally carries ``partial: true`` and the
``failed_components`` names, so the Qt layer's envelope classifier treats a
partially failed snapshot as partial rather than a complete success while the
per-provider data remains available for truthful display.
"""

from __future__ import annotations

import time
from datetime import date

from fedwatch import fomc, fred, investing, polymarket, timeutil
from fedwatch.comparison import compare
from fedwatch.errors import (
    PROVIDER_FOMC_CALENDAR,
    PROVIDER_FRED,
    PROVIDER_INVESTING,
    PROVIDER_POLYMARKET,
    FedwatchError,
)
from fedwatch.transport import HttpTransport, Transport

METHOD_NOTES = [
    "Fed-side probabilities are derived from Investing.com's rounded cumulative "
    "display distribution through MarketLab's validated normalization and the "
    "unchanged CME local-step conversion. They are not raw CME/ZQ observations "
    "and not official CME FedWatch output.",
    "The Fed-side local distribution is structurally binary (at most two adjacent "
    "25 bp outcomes) while Polymarket may price broader tails such as -50, -25, 0, "
    "+25 and +50 bp. A non-zero probability difference can therefore reflect both "
    "genuine market disagreement and this model-shape difference; it is not "
    "automatically a mispricing or a trading signal.",
    "probability_diff_pp = Polymarket - Fed-side.",
    "FRED DFEDTARU/DFEDTARL provides the current target-range context; the FRED "
    "latest observation date is reported because the target range only changes on "
    "meeting days.",
    "Polymarket mappings are validated automatically per retrieval: exact U.S. "
    "Eastern event end date, strict Fed-Decision title month/year, and rate "
    "submarket structure. An unverified meeting reports mapping_status "
    "NOT_FOUND or AMBIGUOUS and no comparison; nothing is guessed.",
    "Batch A is a read-only current-observation capability. No broker, wallet, "
    "order, execution or trading path exists here.",
]


def _error_entry(exc: FedwatchError) -> dict:
    return exc.to_dict()


def _source_entry(provider: str, source: str, status: str, retrieved_at: str | None, **extra) -> dict:
    entry = {
        "provider": provider,
        "source": source,
        "status": status,
        "retrieved_at": retrieved_at,
    }
    entry.update(extra)
    return entry


def _as_of_date(clock) -> date:
    return clock().date()


def build_snapshot(
    transport: Transport | None = None,
    clock=timeutil.utc_now,
    fallback_path=None,
    sleep=time.sleep,
) -> dict:
    """Build the full current snapshot envelope (``data`` + partial markers)."""
    transport = transport or HttpTransport()
    snapshot_retrieved_at = clock()
    errors: list[dict] = []
    warnings: list[str] = []
    sources: list[dict] = []

    fred_target = None
    try:
        fred_target = fred.fetch_target_range(transport, clock=clock)
        sources.append(
            _source_entry(
                PROVIDER_FRED,
                fred.SOURCE_LABEL,
                "OK",
                fred_target["retrieved_at"],
                latest_observation_date=fred_target["latest_observation_date"],
            )
        )
    except FedwatchError as exc:
        errors.append(_error_entry(exc))
        sources.append(
            _source_entry(
                PROVIDER_FRED,
                fred.SOURCE_LABEL,
                "ERROR",
                timeutil.iso_z(clock()),
                detail=exc.message,
            )
        )

    fomc_result = None
    try:
        fomc_result = fomc.fetch_calendar(transport, fallback_path=fallback_path, clock=clock)
        status = "OK" if fomc_result["source_status"] == "SCRAPED" else "FALLBACK_SNAPSHOT"
        sources.append(
            _source_entry(
                PROVIDER_FOMC_CALENDAR,
                fomc_result["source"],
                status,
                fomc_result["retrieved_at"],
                fallback_snapshot_retrieved_at=fomc_result["fallback_snapshot_retrieved_at"],
            )
        )
        warnings.extend(fomc_result["warnings"])
    except FedwatchError as exc:
        errors.append(_error_entry(exc))
        sources.append(
            _source_entry(
                PROVIDER_FOMC_CALENDAR,
                fomc.SOURCE_LABEL_SCRAPE,
                "ERROR",
                timeutil.iso_z(clock()),
                detail=exc.message,
            )
        )

    distributions = None
    try:
        distributions = investing.fetch_distributions(transport, clock=clock)
        sources.append(
            _source_entry(
                PROVIDER_INVESTING,
                distributions["source"],
                "OK",
                distributions["retrieved_at"],
                method=distributions["method"],
            )
        )
        warnings.extend(distributions["warnings"])
    except FedwatchError as exc:
        errors.append(_error_entry(exc))
        sources.append(
            _source_entry(
                PROVIDER_INVESTING,
                investing.FED_RATE_MONITOR_URL,
                "ERROR",
                timeutil.iso_z(clock()),
                detail=exc.message,
            )
        )

    fed_sections: dict[str, dict] = {}
    if distributions is not None:
        if fred_target is not None:
            sections = investing.with_local_probabilities(
                distributions,
                upper=fred_target["target_range"]["upper"],
                lower=fred_target["target_range"]["lower"],
            )
        else:
            sections = investing.without_local_probabilities(distributions)
            warnings.append(
                "FRED target range unavailable: Fed-side raw and normalized "
                "distributions are present but the meeting-level local conversion "
                "could not be produced."
            )
        fed_sections = {section["meeting_date"]: section for section in sections}

    fomc_meetings = fomc_result["meetings"] if fomc_result is not None else []
    upcoming = fomc.upcoming_meetings(fomc_meetings, as_of=_as_of_date(clock)) if fomc_result else []

    polymarket_section = None
    if fomc_result is not None:
        meeting_dates = {row["end_date"] for row in upcoming}
        meeting_dates |= {
            timeutil.parse_date(section["meeting_date"]) for section in fed_sections.values()
        }
        try:
            polymarket_section = polymarket.build_section(
                transport, sorted(meeting_dates), clock=clock, sleep=sleep
            )
            status = "OK" if not polymarket_section["errors"] else "PARTIAL"
            sources.append(
                _source_entry(
                    PROVIDER_POLYMARKET,
                    polymarket_section["source"],
                    status,
                    polymarket_section["retrieved_at"],
                    method=polymarket_section["method"],
                )
            )
            warnings.extend(polymarket_section["warnings"])
            errors.extend(_error_entry(exc) for exc in polymarket_section["errors"])
        except FedwatchError as exc:
            errors.append(_error_entry(exc))
            sources.append(
                _source_entry(
                    PROVIDER_POLYMARKET,
                    polymarket.SOURCE_LABEL,
                    "ERROR",
                    timeutil.iso_z(clock()),
                    detail=exc.message,
                )
            )
    else:
        sources.append(
            _source_entry(
                PROVIDER_POLYMARKET,
                polymarket.SOURCE_LABEL,
                "SKIPPED",
                timeutil.iso_z(clock()),
                detail="FOMC calendar reference unavailable; mappings cannot be verified",
            )
        )
        warnings.append(
            "Polymarket mapping validation was skipped because the FOMC calendar "
            "reference is unavailable."
        )

    # Only meetings that are still upcoming appear in the current snapshot;
    # resolved-meeting history and lifecycle belong to a later batch.
    fomc_by_end = {row["end_date"]: row for row in upcoming}
    polymarket_by_date = {}
    if polymarket_section is not None:
        polymarket_by_date = {
            timeutil.parse_date(entry["meeting_date"]): entry
            for entry in polymarket_section["meetings"]
        }

    meeting_dates_all = set(fomc_by_end)
    meeting_dates_all |= {timeutil.parse_date(value) for value in fed_sections}
    meeting_dates_all |= set(polymarket_by_date)

    meetings = []
    for meeting_date in sorted(meeting_dates_all):
        calendar_row = fomc_by_end.get(meeting_date)
        fed_section = fed_sections.get(meeting_date.isoformat())
        polymarket_entry = polymarket_by_date.get(meeting_date)

        if fed_section is not None and calendar_row is None:
            warnings.append(
                f"meeting {meeting_date.isoformat()} was reported by Investing.com but "
                f"not found in the FOMC calendar"
            )

        comparison_rows: list[dict] = []
        if (
            fed_section is not None
            and fed_section.get("local_probabilities")
            and polymarket_entry is not None
            and polymarket_entry.get("mapping_status") == "VALIDATED"
            and polymarket_entry.get("outcomes")
        ):
            comparison_rows = compare(fed_section["local_probabilities"], polymarket_entry["outcomes"])

        meetings.append(
            {
                "meeting_date": meeting_date.isoformat(),
                "status": "UPCOMING",
                "fomc_calendar": fomc.serialize_meeting(calendar_row) if calendar_row else None,
                "fed_side": fed_section,
                "polymarket": polymarket_entry,
                "comparison": comparison_rows,
            }
        )

    current_target_range = None
    if fred_target is not None:
        current_target_range = {
            "lower": fred_target["target_range"]["lower"],
            "upper": fred_target["target_range"]["upper"],
            "latest_observation_date": fred_target["latest_observation_date"],
            "source": fred_target["source"],
        }

    failed_components = sorted({entry["provider"] for entry in errors})
    data = {
        "retrieved_at": timeutil.iso_z(snapshot_retrieved_at),
        "current_target_range": current_target_range,
        "meetings": meetings,
        "sources": sources,
        "errors": errors,
        "warnings": warnings,
        "method_notes": list(METHOD_NOTES),
    }
    return {
        "success": True,
        "data": data,
        "partial": bool(errors),
        "failed_components": failed_components,
    }


def build_fed_side_command(transport: Transport | None = None, clock=timeutil.utc_now) -> dict:
    """Standalone Fed-side payload: Investing distributions plus local steps.

    Requires the FRED current target range; a FRED failure is reported with
    provider ``fred`` and an Investing failure with provider ``investing``.
    """
    transport = transport or HttpTransport()
    fred_target = fred.fetch_target_range(transport, clock=clock)
    distributions = investing.fetch_distributions(transport, clock=clock)
    sections = investing.with_local_probabilities(
        distributions,
        upper=fred_target["target_range"]["upper"],
        lower=fred_target["target_range"]["lower"],
    )
    return {
        "retrieved_at": distributions["retrieved_at"],
        "source": distributions["source"],
        "method": "LIVE_INVESTING_DERIVED",
        "current_target_range": {
            "lower": fred_target["target_range"]["lower"],
            "upper": fred_target["target_range"]["upper"],
            "latest_observation_date": fred_target["latest_observation_date"],
            "source": fred_target["source"],
        },
        "meetings": sections,
        "quality": distributions["quality"],
        "warnings": list(distributions["warnings"]),
    }


def build_fred_target_command(transport: Transport | None = None, clock=timeutil.utc_now) -> dict:
    transport = transport or HttpTransport()
    return fred.fetch_target_range(transport, clock=clock)


def build_fomc_meetings_command(
    transport: Transport | None = None, clock=timeutil.utc_now, fallback_path=None
) -> dict:
    transport = transport or HttpTransport()
    result = fomc.fetch_calendar(transport, fallback_path=fallback_path, clock=clock)
    return {
        "retrieved_at": result["retrieved_at"],
        "source": result["source"],
        "source_status": result["source_status"],
        "fallback_snapshot_retrieved_at": result["fallback_snapshot_retrieved_at"],
        "meetings": [fomc.serialize_meeting(row) for row in result["meetings"]],
        "warnings": list(result["warnings"]),
    }


def build_polymarket_command(
    transport: Transport | None = None,
    clock=timeutil.utc_now,
    fallback_path=None,
    sleep=time.sleep,
) -> dict:
    """Standalone Polymarket payload: discovery, mappings, current prices.

    The FOMC calendar is the mapping reference; if it is unavailable the
    command fails with provider ``fomc_calendar`` rather than guessing dates.
    """
    transport = transport or HttpTransport()
    fomc_result = fomc.fetch_calendar(transport, fallback_path=fallback_path, clock=clock)
    upcoming = fomc.upcoming_meetings(fomc_result["meetings"], as_of=_as_of_date(clock))
    section = polymarket.build_section(
        transport, [row["end_date"] for row in upcoming], clock=clock, sleep=sleep
    )
    return {
        "retrieved_at": section["retrieved_at"],
        "source": section["source"],
        "method": section["method"],
        "fomc_calendar_source": fomc_result["source"],
        "fomc_calendar_source_status": fomc_result["source_status"],
        "discovery": section["discovery"],
        "meetings": section["meetings"],
        "warnings": list(section["warnings"]),
        "errors": [_error_entry(exc) for exc in section["errors"]],
    }
