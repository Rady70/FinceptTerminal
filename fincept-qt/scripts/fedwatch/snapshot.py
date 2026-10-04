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

import re
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
    "NOT_FOUND or AMBIGUOUS and no comparison; a validated mapping whose data "
    "is not CURRENT likewise yields no current comparison; nothing is guessed.",
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


def local_sections(distributions, target, calendar, today, meeting_dates=None):
    """Only the first local step uses FRED; later adjacent differences do not."""
    first_error, unverified = None, None
    if target and target.get("status", "CURRENT") == "STALE":
        pair_day = timeutil.parse_date(target["latest_observation_date"])
        decisions = [fomc.serialize_meeting(row) for row in (calendar or {}).get("meetings", [])
                     if pair_day <= row["end_date"] < today]
        if decisions:
            first_error = FedwatchError(
                PROVIDER_FRED, "FIRST_MEETING_AFTER_DECISION",
                "first local meeting withheld: a readable calendar row shows a decision "
                "since the latest FRED pair; the pre-decision range cannot seed it",
                detail={"pair_date": pair_day.isoformat(), "decisions": decisions}).to_dict()
        else:
            unverified = pair_day.isoformat()
    bounds = target["target_range"] if target else {}
    sections = investing.with_local_probabilities(distributions, bounds.get("upper"), bounds.get("lower"),
        meeting_dates=meeting_dates, first_error=first_error, unverified_pair_date=unverified)
    for section in sections:
        if section.get("local_error"):
            section["local_error"] = dict(section["local_error"], detail={
                **section["local_error"].get("detail", {}), "meeting_date": section["meeting_date"]})
    return sections


def build_snapshot(
    transport: Transport | None = None,
    clock=timeutil.utc_now,
    fallback_path=None,
    sleep=time.sleep,
    selected_meeting_dates: list[date] | None = None,
    profile_calendar_path=None,
) -> dict:
    """Build the full current snapshot envelope (``data`` + partial markers)."""
    transport = transport or HttpTransport()
    snapshot_retrieved_at = clock()
    errors: list[dict] = []
    warnings: list[str] = []
    sources: list[dict] = []
    selected_days = {day.isoformat() for day in selected_meeting_dates} if selected_meeting_dates is not None else None

    def selected_error(error):
        day = (error.get("detail") or {}).get("meeting_date")
        return selected_days is None or day is None or day in selected_days

    fomc_result = None
    try:
        fomc_result = fomc.fetch_calendar(transport, fallback_path=fallback_path, clock=clock,
                                         profile_path=profile_calendar_path)
        if fomc_result["source_status"] == "SCRAPED":
            status = "OK"
        else:
            status = fomc_result["source_status"]
        sources.append(
            _source_entry(
                PROVIDER_FOMC_CALENDAR,
                fomc_result["source"],
                status,
                fomc_result["retrieved_at"],
                fallback_snapshot_retrieved_at=fomc_result["fallback_snapshot_retrieved_at"],
                fallback_used=fomc_result.get("fallback_used", False),
                coverage_complete=fomc_result.get("coverage_complete", True),
                merge_conflicts=fomc_result.get("merge_conflicts", []),
                parse_report=fomc_result.get("parse_report"),
            )
        )
        warnings.extend(fomc_result["warnings"])
        if not fomc_result.get("coverage_complete", True) and not fomc_result.get("fallback_stale"):
            errors.append(FedwatchError(PROVIDER_FOMC_CALENDAR, "FOMC_CALENDAR_PARSE_PARTIAL",
                                       "valid live calendar rows retained; coverage incomplete",
                                       detail={"parse_report": fomc_result["parse_report"]}).to_dict())
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

    fred_target = None
    try:
        fred_target = fred.fetch_target_range(transport, clock=clock, calendar=fomc_result)
        sources.append(_source_entry(PROVIDER_FRED, fred.SOURCE_LABEL,
            "OK" if fred_target["status"] == "CURRENT" and not fred_target.get("errors") else "PARTIAL",
            fred_target["retrieved_at"], latest_observation_date=fred_target["latest_observation_date"],
            carried_forward=fred_target["carried_forward"], status_reason=fred_target["status_reason"]))
        errors.extend(fred_target.get("errors", []))
    except FedwatchError as exc:
        errors.append(_error_entry(exc))
        sources.append(_source_entry(PROVIDER_FRED, fred.SOURCE_LABEL, "ERROR",
                                     timeutil.iso_z(clock()), detail=exc.message))

    distributions = None
    try:
        distributions = investing.fetch_distributions(transport, clock=clock)
        warnings.extend(distributions["warnings"])
        errors.extend(error for error in distributions.get("errors", []) if selected_error(error))
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
        sections = local_sections(distributions, fred_target, fomc_result, _as_of_date(clock),
                meeting_dates=sorted({row["end_date"].isoformat() for row in
                    fomc.upcoming_meetings(fomc_result["meetings"], _as_of_date(clock))})
                    if fomc_result and fomc_result.get("coverage_complete", True) and not fomc_result.get("fallback_stale") else None,
            )
        fed_sections = {section["meeting_date"]: section for section in sections}
        errors.extend(section["local_error"] for section in sections
                      if section.get("local_error") and selected_error(section["local_error"])
                      and section["local_error"]["code"] != "INVESTING_MEETING_DATE_MISMATCH")

    upcoming = fomc.authoritative_upcoming(fomc_result, _as_of_date(clock)) if fomc_result else []

    # The official FOMC calendar is authoritative for meeting identity. An
    # Investing date absent from the official upcoming calendar is an
    # Investing/Fed-side quality problem: it is excluded from the composite and
    # from Polymarket validation, and it can never produce a comparison. (This
    # restores the qualified invariant that current Investing meeting dates
    # used by the composite are valid official FOMC meeting dates.)
    #
    # A stale fallback snapshot does not establish which dates are officially
    # scheduled, so when the live calendar is unusable and the fallback is too
    # old the composite must not assign an Investing error: it records the
    # calendar uncertainty instead and treats the calendar like unavailable for
    # alignment purposes.
    calendar_uncertain = bool(fomc_result is not None and fomc_result.get("fallback_stale"))
    calendar_coverage_complete = bool(fomc_result and fomc_result.get("coverage_complete", True))
    if calendar_uncertain:
        errors.append(
            FedwatchError(
                PROVIDER_FOMC_CALENDAR,
                "FOMC_CALENDAR_FALLBACK_STALE",
                "tracked fallback snapshot is stale/unknown-age; its rows cannot "
                "establish calendar authority, while confirmed live identities remain usable",
                detail={
                    "fallback_snapshot_retrieved_at": fomc_result.get(
                        "fallback_snapshot_retrieved_at"
                    ),
                    "fallback_age_days": fomc_result.get("fallback_age_days"),
                    "max_age_days": fomc.FALLBACK_MAX_AGE_DAYS,
                },
            ).to_dict()
        )

    official_upcoming_dates = {row["end_date"] for row in upcoming}
    investing_dates = {
        timeutil.parse_date(section["meeting_date"]) for section in fed_sections.values()
    }
    if selected_meeting_dates is not None:
        investing_dates &= set(selected_meeting_dates)
    investing_only_dates = (
        sorted(investing_dates - official_upcoming_dates)
        if fomc_result is not None and not calendar_uncertain and calendar_coverage_complete
        else []
    )

    selected_missing_dates = sorted(
        (set(selected_meeting_dates or []) & official_upcoming_dates) -
        {timeutil.parse_date(day) for day in fed_sections}
    ) if distributions is not None and fred_target is not None else []
    if distributions is not None:
        for day in selected_missing_dates:
            errors.append(FedwatchError(
                PROVIDER_INVESTING, "INVESTING_SELECTED_MEETING_UNAVAILABLE",
                f"Investing.com has no usable Fed-side distribution for selected official meeting {day.isoformat()}",
                detail={"meeting_date": day.isoformat(), "reported_meeting_dates": sorted(fed_sections)},
            ).to_dict())
        if investing_only_dates:
            errors.append(
                FedwatchError(
                    PROVIDER_INVESTING,
                    "INVESTING_MEETING_DATE_MISMATCH",
                    "Investing.com reported meeting date(s) that are not official "
                    "upcoming FOMC meetings: "
                    + ", ".join(value.isoformat() for value in investing_only_dates),
                    detail={
                        "meeting_dates": [value.isoformat() for value in investing_only_dates],
                        "official_upcoming_dates": sorted(
                            value.isoformat() for value in official_upcoming_dates
                        ),
                    },
                ).to_dict()
            )
            warnings.append(
                "Investing.com meeting date(s) outside the official FOMC calendar were "
                "excluded from the composite: "
                + ", ".join(value.isoformat() for value in investing_only_dates)
            )
        sources.append(
            _source_entry(
                PROVIDER_INVESTING,
                distributions["source"],
                "PARTIAL" if investing_only_dates or selected_missing_dates or
                any(e["provider"] == PROVIDER_INVESTING for e in errors) else "OK",
                distributions["retrieved_at"],
                method=distributions["method"],
                detail=(
                    "reported meeting date(s) absent from the official FOMC calendar"
                    if investing_only_dates
                    else None
                ),
            )
        )

    polymarket_section = None
    if fomc_result is not None and (upcoming or not calendar_uncertain):
        # Official upcoming dates only: an Investing-only date must never be a
        # candidate for Polymarket mapping validation.
        meeting_dates = set(official_upcoming_dates)
        if selected_meeting_dates is not None:
            meeting_dates &= set(selected_meeting_dates)
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
    elif calendar_uncertain:
        # The stale fallback was explicitly judged unable to establish the
        # official schedule, so it must not drive automatic mapping validation
        # either.
        sources.append(
            _source_entry(
                PROVIDER_POLYMARKET,
                polymarket.SOURCE_LABEL,
                "SKIPPED",
                timeutil.iso_z(clock()),
                detail="FOMC calendar is stale/uncertain; mappings cannot be verified",
            )
        )
        warnings.append(
            "Polymarket mapping validation was skipped because the FOMC calendar "
            "authority is stale/uncertain."
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
    # resolved-meeting history and lifecycle belong to a later batch. A stale
    # fallback is not serialized as a meeting's calendar authority.
    fomc_by_end = {row["end_date"]: row for row in upcoming}
    polymarket_by_date = {}
    if polymarket_section is not None:
        polymarket_by_date = {
            timeutil.parse_date(entry["meeting_date"]): entry
            for entry in polymarket_section["meetings"]
        }

    meeting_dates_all = set(fomc_by_end)
    if fomc_result is None or calendar_uncertain or not calendar_coverage_complete:
        # With no authoritative calendar the composite can only follow
        # Investing, and the FOMC provider error already marks the snapshot
        # partial. Fed-only dates stay visible with no calendar row and no
        # comparison.
        meeting_dates_all |= {timeutil.parse_date(value) for value in fed_sections}
    meeting_dates_all |= set(polymarket_by_date)

    meetings = []
    for meeting_date in sorted(meeting_dates_all):
        if selected_meeting_dates is not None and meeting_date not in selected_meeting_dates:
            continue
        calendar_row = fomc_by_end.get(meeting_date)
        fed_section = fed_sections.get(meeting_date.isoformat())
        polymarket_entry = polymarket_by_date.get(meeting_date)

        comparison_rows: list[dict] = []
        if (
            calendar_row is not None
            and fed_section is not None
            and fed_section.get("local_probabilities")
            and polymarket_entry is not None
            and polymarket_entry.get("mapping_status") == "VALIDATED"
            # Batch A compares current expectations only: a stale, partial or
            # unavailable Polymarket mapping must not produce a "current"
            # comparison against the fresh Fed-side observation.
            and polymarket_entry.get("data_status") == "CURRENT"
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
            "status": fred_target.get("status", "CURRENT"),
            "status_reason": fred_target.get("status_reason"),
            "carried_forward": fred_target.get("carried_forward", False),
        }

    # Aggregate diagnostics remain in the full response. Retained meetings
    # receive their own warnings and the shared methodology/provider context.
    warning_dates = {}
    for warning in warnings:
        match = re.search(r"\bmeeting (\d{4}-\d{2}-\d{2})", warning)
        if match:
            warning_dates[warning] = {match.group(1)}
    if investing_only_dates:
        for warning in warnings:
            if warning.startswith("Investing.com meeting date(s) outside"):
                warning_dates[warning] = {d.isoformat() for d in investing_only_dates}
    if polymarket_section is not None:
        for entry in polymarket_section["meetings"]:
            for warning in entry.get("warnings", []):
                warning_dates.setdefault(warning, set()).add(entry["meeting_date"])
    meeting_metadata = {
        meeting["meeting_date"]: {
            "sources": sources,
            "warnings": [w for w in warnings if w not in warning_dates or meeting["meeting_date"] in warning_dates[w]],
            "method_notes": list(METHOD_NOTES),  # Shared methodology, no meeting-specific claims.
        } for meeting in meetings
    }
    failed_components = sorted({entry["provider"] for entry in errors})
    data = {
        "retrieved_at": timeutil.iso_z(snapshot_retrieved_at),
        "calendar_snapshot": {**{key: fomc_result.get(key) for key in
            ("retrieved_at", "source_status", "coverage_complete", "parse_report")},
            "meetings": [fomc.serialize_meeting(row) for row in fomc_result["meetings"]]}
            if fomc_result else None,
        "current_target_range": current_target_range,
        "meetings": meetings,
        "sources": sources,
        "errors": errors,
        "warnings": warnings,
        "method_notes": list(METHOD_NOTES),
        "meeting_metadata": meeting_metadata,
    }
    return {
        "success": True,
        "data": data,
        "partial": bool(errors),
        "failed_components": failed_components,
    }


def build_fed_side_command(transport: Transport | None = None, clock=timeutil.utc_now) -> dict:
    """Standalone Fed-side payload: Investing distributions plus local steps.

    Uses the same calendar-aware FRED carry-forward as the snapshot and retains
    the standalone Investing sequence. An unavailable calendar leaves the age rule in
    place and reports its diagnostics without discarding usable provider data.
    """
    transport = transport or HttpTransport()
    calendar, calendar_errors = None, []
    try:
        calendar = fomc.fetch_calendar(transport, clock=clock)
    except FedwatchError as exc:
        calendar_errors.append(exc.to_dict())
    fred_target = None
    try:
        fred_target = fred.fetch_target_range(transport, clock=clock, calendar=calendar)
    except FedwatchError as exc:
        calendar_errors.append(exc.to_dict())
    distributions = investing.fetch_distributions(transport, clock=clock)
    sections = local_sections(distributions, fred_target, calendar, _as_of_date(clock))
    return {
        "retrieved_at": distributions["retrieved_at"],
        "source": distributions["source"],
        "method": "LIVE_INVESTING_DERIVED",
        "current_target_range": {
            "lower": fred_target["target_range"]["lower"],
            "upper": fred_target["target_range"]["upper"],
            "latest_observation_date": fred_target["latest_observation_date"],
            "source": fred_target["source"],
            "status": fred_target.get("status", "CURRENT"),
            "carried_forward": fred_target.get("carried_forward", False),
            "status_reason": fred_target.get("status_reason"),
        } if fred_target else None,
        "meetings": sections,
        "quality": distributions["quality"],
        "errors": calendar_errors + distributions.get("errors", []) + (fred_target or {}).get("errors", []) +
                  [section["local_error"] for section in sections if section.get("local_error")],
        "warnings": list(distributions["warnings"]) + (calendar["warnings"] if calendar else []),
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
        "fallback_age_days": result.get("fallback_age_days"),
        "fallback_stale": result.get("fallback_stale"),
        "parse_report": result.get("parse_report"),
        "coverage_complete": result.get("coverage_complete"),
        "fallback_used": result.get("fallback_used", False),
        "merge_conflicts": result.get("merge_conflicts", []),
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

    The FOMC calendar is the mapping reference; if it is unavailable or is a
    stale fallback that cannot establish the official schedule, the command
    fails with provider ``fomc_calendar`` rather than validating dates against
    a schedule that is not authoritative.
    """
    transport = transport or HttpTransport()
    fomc_result = fomc.fetch_calendar(transport, fallback_path=fallback_path, clock=clock)
    upcoming = fomc.authoritative_upcoming(fomc_result, _as_of_date(clock))
    if fomc_result.get("fallback_stale") and not upcoming:
        raise FedwatchError(
            PROVIDER_FOMC_CALENDAR,
            "FOMC_CALENDAR_FALLBACK_STALE",
            "live FOMC calendar unavailable and the tracked fallback snapshot is too "
            "old to establish the schedule; Polymarket mappings cannot be verified",
            detail={
                "fallback_snapshot_retrieved_at": fomc_result.get(
                    "fallback_snapshot_retrieved_at"
                ),
                "fallback_age_days": fomc_result.get("fallback_age_days"),
                "max_age_days": fomc.FALLBACK_MAX_AGE_DAYS,
            },
        )
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
