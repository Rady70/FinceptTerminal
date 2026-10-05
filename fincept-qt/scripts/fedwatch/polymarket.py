"""Polymarket FOMC discovery, conservative mapping validation, and prices.

The qualified component discovered candidates from Polymarket's Gamma API and
required a human to confirm each mapping in a tracked review CSV. The
integration plan (section 10) explicitly forbids keeping historical manual
review as the permanent mechanism, so this module replaces it with a
conservative *automatic* validation and a truthful unverified state when no
single candidate qualifies:

* discovery is dynamic (``fed-rates`` tag union keyword search, deduplicated);
* a candidate must be an active, open event whose U.S. Eastern end date is
  exactly the FOMC meeting's end date;
* the event title must be exactly a "Fed Decision in <Month>?" form whose
  month (and year, when present) matches the meeting;
* at least three rate submarkets must parse, including a no-change outcome,
  with unique outcomes, valid Yes prices and token ids, and every rate question
  must reference exactly that meeting's month/year;
* the Yes-price sum must be within a wide sanity band;
* if more than one candidate passes, the mapping is AMBIGUOUS and the meeting
  reports Polymarket comparison as unavailable rather than guessing.

Only public read-only Gamma/CLOB endpoints are used. There is no
authentication, no wallet, and no order/execution path anywhere in this file.
"""

from __future__ import annotations

import json
import math
import re
import time
from datetime import date, datetime, timedelta, timezone

from fedwatch import timeutil
from fedwatch.errors import PROVIDER_POLYMARKET, FedwatchError
from fedwatch.transport import Transport, TransportError

GAMMA_BASE_URL = "https://gamma-api.polymarket.com"
CLOB_BASE_URL = "https://clob.polymarket.com"
SOURCE_LABEL = "polymarket gamma-api + clob public read-only"

SEARCH_KEYWORDS = ["Fed", "FOMC", "interest rate"]
FED_RATES_TAG = "fed-rates"

REQUEST_TIMEOUT = 20
RATE_LIMIT_SLEEP_SECONDS = 0.2

# Discovery pagination: the tag listing is the precise candidate set and must
# be exhausted; the keyword searches are paginated to a bounded cap. Coverage
# completeness is recorded and an incomplete discovery marks the provider
# partial instead of allowing a definitive NOT_FOUND.
TAG_PAGE_SIZE = 100
SEARCH_PAGE_SIZE = 50
MAX_DISCOVERY_PAGES = 50

FRESHNESS_MAX_AGE_DAYS = 3
FUTURE_TOLERANCE_SECONDS = 300

MIN_PARSED_SUBMARKETS = 3
OUTCOME_SUM_MIN = 0.85
OUTCOME_SUM_MAX = 1.15

_UP_WORDS = r"increase|increases|raise|raises|hike|hikes"
_DOWN_WORDS = r"decrease|decreases|cut|cuts|lower|lowers"
_UP_WORDS_SET = {word.lower() for word in _UP_WORDS.split("|")}
# "by" and the unit spelling vary between listings ("by 25 bps", "25bp",
# "25 basis points"); the number must still be followed by a bp unit, so a
# year or a date can never be read as a rate change.
_BP_PATTERN = re.compile(
    rf"\b(?P<direction>{_UP_WORDS}|{_DOWN_WORDS})\b.*?(?:\bby\s+)?(?P<bp>\d+)(?P<plus>\+)?\s*"
    r"(?:bps?|basis[\s-]points?)\b",
    re.IGNORECASE,
)
_NO_CHANGE_PATTERN = re.compile(r"no change|\bunchanged\b|\bhold(?:s)?\s+(?:interest\s+)?rates?\b", re.IGNORECASE)

_TITLE_PATTERN = re.compile(r"^fed decision in ([a-z]+)(?:\s+(\d{4}))?\??$", re.IGNORECASE)
# One Fed decision, not a multi-meeting combination, dissent count or other
# side market. Wording around it may change ("October Fed decision?",
# "Fed rate decision in October 2026?").
_TITLE_FED_RE = re.compile(r"\b(?:fed|fomc|federal reserve)\b", re.IGNORECASE)
_TITLE_DECISION_RE = re.compile(r"\bdecision\b", re.IGNORECASE)
_TITLE_EXCLUDED_RE = re.compile(r"\b(?:decisions|dissents?|combos?|how many|chair|vote|votes)\b|&", re.IGNORECASE)
_MONTH_NAME_TO_NUM = {
    "january": 1, "february": 2, "march": 3, "april": 4, "may": 5, "june": 6,
    "july": 7, "august": 8, "september": 9, "october": 10, "november": 11,
    "december": 12,
}
_YEAR_PATTERN = re.compile(r"\b(19\d{2}|20\d{2})\b")


def parse_bp_outcome(question: str) -> dict | None:
    """Parse one submarket question into ``{bp_delta, open_ended}`` or ``None``.

    Unchanged from the qualified implementation: "no change" wins first;
    otherwise a direction word followed by "by N bps" (an optional ``+`` makes
    the outcome open-ended, e.g. "50+ bps"). Non-rate questions such as
    "Powell fired?" return ``None``.
    """
    if _NO_CHANGE_PATTERN.search(question):
        return {"bp_delta": 0, "open_ended": False}

    match = _BP_PATTERN.search(question)
    if not match:
        return None

    bp = int(match.group("bp"))
    sign = 1 if match.group("direction").lower() in _UP_WORDS_SET else -1
    return {"bp_delta": sign * bp, "open_ended": match.group("plus") is not None}


def _load_json_list(value) -> list:
    if isinstance(value, list):
        return value
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
        except (json.JSONDecodeError, ValueError):
            return []
        return parsed if isinstance(parsed, list) else []
    return []


def _usable_event_id(value) -> str | None:
    """Normalize a Gamma event id, or return None when it is malformed.

    Event ids are non-empty strings or positive integers; anything else
    (``None``, empty or whitespace strings, booleans, floats, lists, dicts)
    cannot serve as a deduplication key and counts as discovery loss rather
    than being coerced into a colliding key.
    """
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, int):
        return str(value) if value > 0 else None
    if isinstance(value, str):
        cleaned = value.strip()
        return cleaned or None
    return None


def extract_markets(event: dict) -> list[dict]:
    """Flatten one Gamma event into one row per submarket.

    Gamma encodes ``outcomes``, ``outcomePrices`` and ``clobTokenIds`` as JSON
    strings; the "Yes" outcome index supplies both the yes price and the CLOB
    token id. Non-rate questions stay in the result with
    ``bp_delta=None``/``parse_failed=True`` so callers can see them, but they
    never count as rate outcomes.

    Malformed event structure is tolerated at this boundary: a non-list
    ``markets`` field yields no rows, and non-dict market entries are skipped,
    so schema drift rejects a candidate instead of escaping as an internal
    error. Candidates without usable structure fail validation explicitly.
    """
    markets = event.get("markets", [])
    if not isinstance(markets, list):
        return []

    rows: list[dict] = []
    for market in markets:
        if not isinstance(market, dict):
            continue
        question = market.get("question", "")
        if not isinstance(question, str):
            question = ""
        parsed = parse_bp_outcome(question)
        outcomes = _load_json_list(market.get("outcomes"))
        prices = _load_json_list(market.get("outcomePrices"))
        clob_token_ids = _load_json_list(market.get("clobTokenIds"))

        yes_price = None
        yes_token_id = None
        labels = [outcome.strip().casefold() for outcome in outcomes if isinstance(outcome, str)]
        binary_valid = len(outcomes) == 2 and len(labels) == 2 and set(labels) == {"yes", "no"}
        if binary_valid:
            index = labels.index("yes")
            if index < len(prices):
                try:
                    yes_price = float(prices[index])
                except (TypeError, ValueError, OverflowError):
                    yes_price = None
            if index < len(clob_token_ids):
                token = clob_token_ids[index]
                yes_token_id = _usable_event_id(token)

        rows.append(
            {
                "market_id": str(market.get("id")) if market.get("id") is not None else None,
                "question": question,
                "bp_delta": parsed["bp_delta"] if parsed else None,
                "open_ended": parsed["open_ended"] if parsed else None,
                "parse_failed": parsed is None,
                "yes_probability": yes_price,
                "yes_clob_token_id": yes_token_id,
                "binary_outcomes_valid": binary_valid,
                "market_end_date": market.get("endDate"),
            }
        )
    return rows


def _get_json(transport: Transport, url: str, params: dict, context: str):
    try:
        return transport.get_json(url, params=params, timeout=REQUEST_TIMEOUT)
    except TransportError as exc:
        raise FedwatchError(
            PROVIDER_POLYMARKET,
            "POLYMARKET_DISCOVERY_UNAVAILABLE",
            f"Polymarket {context} request failed: {exc}",
            detail=exc.detail(),
        ) from exc


def search_events(
    transport: Transport,
    query: str,
    limit_per_type: int = SEARCH_PAGE_SIZE,
    max_pages: int = MAX_DISCOVERY_PAGES,
) -> tuple[list, dict]:
    """Paginated gamma public-search (1-based ``page`` until ``hasMore`` is false).

    Returns ``(events, info)``. ``info.complete`` is true only when the
    provider metadata agrees that every reported result has actually been
    received. Completion is judged against the cumulative number of raw events
    received, never against page capacity:

    * ``hasMore: false`` completes only when no trustworthy total is reported,
      or the received count exactly equals the reported total;
    * ``hasMore: true`` never completes on a total alone (a trusted total at or
      below the received count while ``hasMore`` still says otherwise is
      contradictory and fails closed);
    * a missing or wrong-typed ``hasMore`` completes only when the received
      count exactly equals a trustworthy total;
    * a trusted ``totalResults`` that changes between pages is inconsistent and
      fails closed, as do dropped non-dict items and untrusted (boolean or
      negative) totals.

    An unexpected top-level response shape is a source failure, never an empty
    success.
    """
    events: list = []
    page = 1
    pages = 0
    complete = False
    total_results = None
    malformed_item_count = 0
    received_count = 0
    metadata_inconsistent = False
    page_error = None
    while pages < max_pages:
        try:
            data = _get_json(
                transport,
                f"{GAMMA_BASE_URL}/public-search",
                {"q": query, "limit_per_type": limit_per_type, "page": page},
                context=f"search ({query!r}) page {page}",
            )
        except FedwatchError as exc:
            if not pages:
                raise
            page_error = exc.to_dict()
            complete = False
            break
        if not isinstance(data, dict) or not isinstance(data.get("events"), list):
            if pages:
                page_error = {"error": "search response did not contain an events list", "page": page}
                break
            raise FedwatchError(
                PROVIDER_POLYMARKET,
                "POLYMARKET_DISCOVERY_UNAVAILABLE",
                f"Polymarket search ({query!r}) response did not contain an events list",
                detail={"phase": "search", "query": query, "page": page},
            )
        pages += 1
        raw_page = data["events"]
        received_count += len(raw_page)
        page_events = [event for event in raw_page if isinstance(event, dict)]
        malformed_item_count += len(raw_page) - len(page_events)
        events.extend(page_events)

        pagination = data.get("pagination")
        if not isinstance(pagination, dict):
            # No pagination metadata: a short page means exhausted, a full page
            # is unknowable and must be treated as incomplete coverage.
            complete = len(raw_page) < limit_per_type
            break

        raw_total = pagination.get("totalResults")
        trusted_total = None
        if isinstance(raw_total, int) and not isinstance(raw_total, bool) and raw_total >= 0:
            trusted_total = raw_total
        if trusted_total is not None:
            if total_results is None:
                total_results = trusted_total
            elif trusted_total != total_results:
                metadata_inconsistent = True
                complete = False
                break

        has_more = pagination.get("hasMore")
        if has_more is False:
            complete = total_results is None or received_count == total_results
            break
        if has_more is True:
            if total_results is not None and received_count >= total_results:
                # The provider says another page exists while its own total says
                # everything has been received: contradictory, fail closed.
                complete = False
                break
            page += 1
            continue
        # Missing or wrong-typed hasMore cannot establish completion on its own;
        # only a trustworthy total exactly received can.
        complete = total_results is not None and received_count == total_results
        break
    if malformed_item_count or metadata_inconsistent:
        # Dropped items or changing metadata mean coverage cannot be trusted even
        # though pagination finished.
        complete = False
    return events, {
        "pages": pages,
        "event_count": len(events),
        "received_count": received_count,
        "total_results": total_results,
        "malformed_item_count": malformed_item_count,
        "metadata_inconsistent": metadata_inconsistent,
        "complete": complete,
        "page_error": page_error,
    }


def events_by_tag(
    transport: Transport,
    tag_slug: str,
    page_size: int = TAG_PAGE_SIZE,
    max_pages: int = MAX_DISCOVERY_PAGES,
    closed: bool | None = False,
) -> tuple[list, dict]:
    """Paginated gamma tag listing (offset pagination until a short page).

    The default ``closed=false`` filter narrows the listing to the events that
    can matter for current mappings; the loop then follows ``offset`` until the
    provider returns a short page, so coverage is exhausted rather than capped
    at one page. An unexpected top-level response shape is a source failure.
    """
    events: list = []
    offset = 0
    pages = 0
    complete = False
    malformed_item_count = 0
    page_error = None
    while pages < max_pages:
        params = {"tag_slug": tag_slug, "limit": page_size, "offset": offset}
        if closed is not None:
            params["closed"] = str(closed).lower()
        try:
            data = _get_json(
                transport,
                f"{GAMMA_BASE_URL}/events",
                params,
                context=f"tag listing ({tag_slug!r}) page {pages + 1}",
            )
        except FedwatchError as exc:
            if not pages:
                raise
            page_error = exc.to_dict()
            break
        if not isinstance(data, list):
            if pages:
                page_error = {"error": "tag response was not a list", "page": pages + 1}
                break
            raise FedwatchError(
                PROVIDER_POLYMARKET,
                "POLYMARKET_DISCOVERY_UNAVAILABLE",
                f"Polymarket tag listing ({tag_slug!r}) response was not a list",
                detail={"phase": "tag", "tag_slug": tag_slug, "page": pages + 1},
            )
        pages += 1
        page_events = [event for event in data if isinstance(event, dict)]
        malformed_item_count += len(data) - len(page_events)
        events.extend(page_events)
        if len(data) < page_size:
            complete = True
            break
        offset += page_size
    if malformed_item_count:
        # Non-dict response items were dropped; coverage cannot be trusted even
        # though pagination finished.
        complete = False
    return events, {
        "pages": pages,
        "event_count": len(events),
        "malformed_item_count": malformed_item_count,
        "complete": complete,
        "closed_filter": closed,
        "page_error": page_error,
    }


def fetch_price_history(
    transport: Transport,
    clob_token_id: str,
    interval: str = "1d",
    fidelity: int = 1440,
    sleep=time.sleep,
) -> list:
    try:
        data = transport.get_json(
            f"{CLOB_BASE_URL}/prices-history",
            params={"market": clob_token_id, "interval": interval, "fidelity": fidelity},
            timeout=REQUEST_TIMEOUT,
        )
    except TransportError as exc:
        raise FedwatchError(
            PROVIDER_POLYMARKET,
            "POLYMARKET_MARKET_DATA_UNAVAILABLE",
            f"Polymarket CLOB price history request failed: {exc}",
            detail=exc.detail(),
        ) from exc
    finally:
        sleep(RATE_LIMIT_SLEEP_SECONDS)
    if not isinstance(data, dict) or not isinstance(data.get("history"), list):
        raise FedwatchError(PROVIDER_POLYMARKET, "POLYMARKET_MARKET_DATA_INVALID",
                            "CLOB response did not contain a history list", detail={"token_id": clob_token_id})
    return data["history"]


def discover_candidate_events(
    transport: Transport,
    tag_page_size: int = TAG_PAGE_SIZE,
    search_page_size: int = SEARCH_PAGE_SIZE,
    max_pages: int = MAX_DISCOVERY_PAGES,
) -> tuple[list[dict], dict, list[str]]:
    """Union of the paginated ``fed-rates`` tag listing and keyword searches.

    Returns ``(events, discovery_stats, warnings)``. Every source records its
    page and result counts and whether its coverage was exhausted.
    ``coverage_complete`` is true only when every source succeeded and finished
    paginating; partial source failures survive as warnings, and when every
    source fails or the union is empty the provider raises an explicit failure
    instead of returning nothing.
    """
    events_by_id: dict[str, dict] = {}
    warnings: list[str] = []
    events_without_id = 0
    stats = {
        "tag": {"status": "OK"},
        "searches": {keyword: {"status": "OK"} for keyword in SEARCH_KEYWORDS},
    }

    try:
        tag_events, tag_info = events_by_tag(
            transport, FED_RATES_TAG, page_size=tag_page_size, max_pages=max_pages
        )
        stats["tag"].update(tag_info)
        tag_seen: set[str] = set()
        tag_duplicates = 0
        for event in tag_events:
            event_id = _usable_event_id(event.get("id"))
            if event_id is None:
                events_without_id += 1
                continue
            if event_id in tag_seen:
                # The same event consumed two result slots within one paginated
                # source: another result may have been skipped, so the source
                # cannot claim complete coverage.
                tag_duplicates += 1
                continue
            tag_seen.add(event_id)
            events_by_id[event_id] = event
        if tag_duplicates:
            stats["tag"]["duplicate_id_count"] = tag_duplicates
            stats["tag"]["complete"] = False
            warnings.append(
                f"tag listing repeated {tag_duplicates} event id(s) within its own "
                f"pages; coverage may be incomplete"
            )
    except FedwatchError as exc:
        stats["tag"]["status"] = "ERROR"
        stats["tag"]["code"] = exc.code
        warnings.append(f"tag listing failed: {exc.message}")

    for keyword in SEARCH_KEYWORDS:
        try:
            found, info = search_events(
                transport, keyword, limit_per_type=search_page_size, max_pages=max_pages
            )
            stats["searches"][keyword].update(info)
            source_seen: set[str] = set()
            source_duplicates = 0
            for event in found:
                event_id = _usable_event_id(event.get("id"))
                if event_id is None:
                    events_without_id += 1
                    continue
                if event_id in source_seen:
                    source_duplicates += 1
                    continue
                source_seen.add(event_id)
                events_by_id[event_id] = event
            if source_duplicates:
                stats["searches"][keyword]["duplicate_id_count"] = source_duplicates
                stats["searches"][keyword]["complete"] = False
                warnings.append(
                    f"search ({keyword!r}) repeated {source_duplicates} event id(s) "
                    f"within its own pages; coverage may be incomplete"
                )
        except FedwatchError as exc:
            stats["searches"][keyword]["status"] = "ERROR"
            stats["searches"][keyword]["code"] = exc.code
            warnings.append(f"search ({keyword!r}) failed: {exc.message}")

    failed_sources = []
    if stats["tag"]["status"] == "ERROR":
        failed_sources.append(f"tag:{FED_RATES_TAG}")
    failed_sources.extend(
        f"search:{keyword}"
        for keyword, info in stats["searches"].items()
        if info["status"] == "ERROR"
    )
    if not events_by_id:
        if failed_sources:
            raise FedwatchError(
                PROVIDER_POLYMARKET,
                "POLYMARKET_DISCOVERY_UNAVAILABLE",
                "Polymarket discovery returned no events and at least one source failed",
                detail={"discovery": stats},
            )
        raise FedwatchError(
            PROVIDER_POLYMARKET,
            "POLYMARKET_DISCOVERY_EMPTY",
            "Polymarket discovery returned no candidate events",
            detail={"discovery": stats},
        )

    events = list(events_by_id.values())
    stats["unique_event_count"] = len(events)
    market_rows = 0
    events_without_market_list = 0
    for event in events:
        markets = event.get("markets")
        if isinstance(markets, list):
            market_rows += len(markets)
        else:
            events_without_market_list += 1
    stats["candidate_market_rows"] = market_rows
    if events_without_market_list:
        stats["events_without_market_list"] = events_without_market_list
        warnings.append(
            f"{events_without_market_list} candidate event(s) had a non-list markets "
            f"field; they can only be rejected, not validated"
        )
    if events_without_id:
        stats["events_without_id"] = events_without_id
        warnings.append(
            f"{events_without_id} candidate event(s) had no usable id and were dropped; "
            f"discovery coverage cannot be complete"
        )
    stats["coverage_complete"] = bool(
        stats["tag"]["status"] == "OK"
        and stats["tag"].get("complete")
        and all(
            info["status"] == "OK" and info.get("complete")
            for info in stats["searches"].values()
        )
        and events_without_id == 0
    )
    return events, stats, warnings


def _title_meeting(title: str) -> tuple[int, int | None] | None:
    """Month (and year, when present) of a single Fed-decision event title.

    The established "Fed Decision in <Month>[ <Year>]?" form is read first.
    Other wordings qualify when they name the Fed and one decision, mention
    exactly one month, and are not a combination, dissent or side market.
    """
    cleaned = re.sub(r"\s+", " ", (title or "").strip())
    match = _TITLE_PATTERN.match(cleaned)
    if match:
        month = _MONTH_NAME_TO_NUM.get(match.group(1).lower())
        if month is None:
            return None
        return month, int(match.group(2)) if match.group(2) else None
    if not (_TITLE_FED_RE.search(cleaned) and _TITLE_DECISION_RE.search(cleaned)) or _TITLE_EXCLUDED_RE.search(cleaned):
        return None
    months = _months_mentioned(cleaned)
    years = _years_mentioned(cleaned)
    if len(months) != 1 or len(years) > 1:
        return None
    return next(iter(months)), next(iter(years)) if years else None


def _months_mentioned(text: str) -> set[int]:
    return {
        number
        for name, number in _MONTH_NAME_TO_NUM.items()
        if re.search(rf"\b{name}\b", text, re.IGNORECASE)
    }


def _years_mentioned(text: str) -> set[int]:
    return {int(value) for value in _YEAR_PATTERN.findall(text)}


def validate_candidate_event(event: dict, meeting_date: date) -> tuple[dict | None, str | None]:
    """Validate one candidate event against a meeting end date.

    Returns ``(evidence, None)`` when the candidate passes every check, or
    ``(None, reason_code)`` when it does not. The checks are deliberately
    conservative: no approximate title or adjacent date. An invalid binary
    submarket is quarantined independently; it cannot invalidate otherwise
    exact meeting identities and readable validated neighbouring markets.
    """
    # Only an explicit inactive or closed flag disqualifies; a listing that
    # omits either field is judged by its date and market structure.
    if event.get("active") is False or event.get("closed") is True:
        return None, "EVENT_NOT_ACTIVE_AND_OPEN"

    raw_title = event.get("title")
    if not isinstance(raw_title, str):
        return None, "EVENT_TITLE_NOT_STRING"

    raw_end_date = event.get("endDate")
    if not raw_end_date:
        return None, "END_DATE_MISSING"
    try:
        event_end_utc = timeutil.parse_iso_z(str(raw_end_date))
    except ValueError:
        return None, "END_DATE_UNPARSEABLE"
    event_end_eastern = timeutil.eastern_date_from_utc(event_end_utc)
    if event_end_eastern != meeting_date:
        return None, "END_DATE_MISMATCH"

    raw_markets = event.get("markets")
    if isinstance(raw_markets, list):
        for market in raw_markets:
            if isinstance(market, dict):
                question = market.get("question", "")
                if not isinstance(question, str):
                    return None, "MARKET_QUESTION_NOT_STRING"

    title_meeting = _title_meeting(raw_title)
    if title_meeting is None:
        return None, "TITLE_NOT_FED_DECISION"
    title_month, title_year = title_meeting
    if title_month != meeting_date.month:
        return None, "TITLE_MONTH_MISMATCH"
    if title_year is not None and title_year != meeting_date.year:
        return None, "TITLE_YEAR_MISMATCH"

    markets = extract_markets(event)
    rate_markets = [market for market in markets if market["bp_delta"] is not None]
    if len(rate_markets) < MIN_PARSED_SUBMARKETS:
        return None, "INSUFFICIENT_RATE_SUBMARKETS"
    if not any(market["bp_delta"] == 0 for market in rate_markets):
        return None, "NO_NO_CHANGE_OUTCOME"

    seen_outcomes: set[tuple] = set()
    seen_tokens: set[str] = set()
    rejected_binary_markets = []
    for market in rate_markets:
        key = (market["bp_delta"], market["open_ended"])
        if key in seen_outcomes:
            return None, "DUPLICATE_OUTCOMES"
        seen_outcomes.add(key)
        if market["binary_outcomes_valid"]:
            if not market["yes_clob_token_id"]:
                return None, "MISSING_TOKEN_ID"
            if market["yes_clob_token_id"] in seen_tokens:
                return None, "DUPLICATE_TOKEN_IDS"
            seen_tokens.add(market["yes_clob_token_id"])
        else:
            rejected_binary_markets.append({"market_id": market["market_id"],
                "outcome_bp": market["bp_delta"], "open_ended": market["open_ended"],
                "reason": "INVALID_BINARY_OUTCOMES"})
        # A question may omit the month; it must never name another one.
        if _months_mentioned(market["question"]) - {meeting_date.month}:
            return None, "QUESTION_MEETING_MISMATCH"
        mentioned_years = _years_mentioned(market["question"])
        if mentioned_years and mentioned_years != {meeting_date.year}:
            return None, "QUESTION_MEETING_MISMATCH"

    if len(rejected_binary_markets) == len(rate_markets):
        return None, "INVALID_BINARY_OUTCOMES"

    # Gamma's embedded prices are advisory snapshots, not meeting identity.
    # The authoritative CLOB observations are validated after token mapping.
    valid_prices = all(m["yes_probability"] is not None and math.isfinite(m["yes_probability"])
                       and 0 <= m["yes_probability"] <= 1 for m in rate_markets)
    outcome_sum = sum(m["yes_probability"] for m in rate_markets) if valid_prices else None

    evidence = {
        "validation_method": (
            "exact_us_eastern_end_date + fed_decision_title_month + submarket_structure"
        ),
        "event_id": str(event.get("id")),
        "event_title": event.get("title"),
        "event_end_date_utc": timeutil.iso_z(event_end_utc),
        "event_end_date_us_eastern": event_end_eastern.isoformat(),
        "fomc_meeting_end_date": meeting_date.isoformat(),
        "title_month": meeting_date.strftime("%B"),
        "title_year": title_year,
        "rate_submarket_count": len(rate_markets),
        "outcome_bp_values": sorted(market["bp_delta"] for market in rate_markets),
        "outcome_probability_sum": round(outcome_sum, 6) if outcome_sum is not None else None,
        "gamma_price_status": "OK" if outcome_sum is not None and OUTCOME_SUM_MIN <= outcome_sum <= OUTCOME_SUM_MAX else "INVALID_OR_INCOMPLETE",
        "yes_token_ids_present": not rejected_binary_markets,
        "rejected_binary_markets": rejected_binary_markets,
        "validated_binary_market_count": len(rate_markets) - len(rejected_binary_markets),
    }
    return evidence, None


def resolve_mapping(events: list[dict], meeting_date: date) -> dict:
    """Resolve one meeting date to at most one validated candidate event."""
    validated: list[tuple[dict, dict]] = []
    same_date_candidates: list[dict] = []

    for event in events:
        evidence, reason = validate_candidate_event(event, meeting_date)
        if evidence is not None:
            validated.append((event, evidence))
            continue
        try:
            event_end = timeutil.eastern_date_from_utc(
                timeutil.parse_iso_z(str(event.get("endDate", "")))
            )
        except ValueError:
            event_end = None
        if event_end == meeting_date:
            same_date_candidates.append(
                {
                    "event_id": str(event.get("id")),
                    "event_title": event.get("title"),
                    "rejected_reason": reason,
                }
            )

    result = {
        "meeting_date": meeting_date.isoformat(),
        "candidate_event_count": len(events),
        "same_date_candidates": same_date_candidates,
    }
    if len(validated) == 1:
        event, evidence = validated[0]
        result.update(
            {
                "mapping_status": "VALIDATED",
                "event": event,
                "mapping_evidence": evidence,
                "ambiguous_event_ids": [],
            }
        )
        return result
    if len(validated) > 1:
        result.update(
            {
                "mapping_status": "AMBIGUOUS",
                "event": None,
                "mapping_evidence": None,
                "ambiguous_event_ids": [str(event.get("id")) for event, _ in validated],
            }
        )
        return result
    result.update(
        {
            "mapping_status": "NOT_FOUND",
            "event": None,
            "mapping_evidence": None,
            "ambiguous_event_ids": [],
        }
    )
    return result


def normalize_price_points(points: list, now: datetime) -> tuple[list[dict], dict]:
    """Validate points independently; conflicting same-instant values are unknown."""
    accepted, conflicts = {}, set()
    counts = {"malformed": 0, "future": 0, "out_of_range": 0, "conflicting": 0}
    for point in points:
        try:
            if not isinstance(point, dict) or isinstance(point.get("t"), bool) or isinstance(point.get("p"), bool):
                raise ValueError("invalid point")
            timestamp, probability = float(point["t"]), float(point["p"])
            if not math.isfinite(timestamp) or not math.isfinite(probability) or timestamp < 0:
                raise ValueError("invalid number")
            instant = datetime.fromtimestamp(timestamp, tz=timezone.utc)
        except (KeyError, TypeError, ValueError, OverflowError, OSError):
            counts["malformed"] += 1
            continue
        if instant > now + timedelta(seconds=FUTURE_TOLERANCE_SECONDS):
            counts["future"] += 1
            continue
        if not 0 <= probability <= 1:
            counts["out_of_range"] += 1
            continue
        if instant in accepted and accepted[instant] != probability:
            conflicts.add(instant)
            counts["conflicting"] += 1
        accepted[instant] = probability
    return [{"instant": t, "probability": p} for t, p in sorted(accepted.items()) if t not in conflicts], counts


def fetch_current_outcomes(
    transport: Transport,
    mapping: dict,
    clock=timeutil.utc_now,
    sleep=time.sleep,
) -> tuple[list[dict], list[str], list[FedwatchError]]:
    """Fetch the current CLOB probability for every rate outcome of a mapping.

    One row per rate submarket using its latest individually valid point. Bad,
    future or conflicting points are counted and rejected; only an outcome
    with no valid points keeps ``probability_pct=None``. Nothing is fabricated.
    Stale points (older than the freshness window) are returned but the meeting
    is reported STALE, never current.
    """
    event = mapping["event"]
    rate_markets = [m for m in extract_markets(event) if m["bp_delta"] is not None]
    outcomes: list[dict] = []
    warnings: list[str] = []
    errors: list[FedwatchError] = []
    latest_instants: list[datetime] = []
    now = clock()

    for market in rate_markets:
        base = {
            "outcome_bp": market["bp_delta"],
            "open_ended": market["open_ended"],
            "market_id": market["market_id"],
            "token_id": market["yes_clob_token_id"],
            "question": market["question"],
        }
        if not market["binary_outcomes_valid"]:
            errors.append(FedwatchError(PROVIDER_POLYMARKET, "POLYMARKET_INVALID_BINARY_OUTCOMES",
                "rate market does not contain exactly one Yes and one No; neighbouring mappings retained",
                detail={"meeting_date": mapping["meeting_date"], "market_id": market["market_id"],
                        "outcome_bp": market["bp_delta"]}))
            outcomes.append({**base, "binary_outcomes_valid": False,
                             "probability_pct": None, "source_timestamp": None})
            continue
        try:
            history = fetch_price_history(
                transport, market["yes_clob_token_id"], interval="1d", fidelity=1440, sleep=sleep
            )
        except FedwatchError as exc:
            warnings.append(
                f"event {mapping['mapping_evidence']['event_id']} outcome "
                f"{market['bp_delta']}bp: {exc.message}"
            )
            errors.append(exc)
            outcomes.append({**base, "probability_pct": None, "source_timestamp": None})
            continue

        if not history:
            warnings.append(
                f"event {mapping['mapping_evidence']['event_id']} outcome "
                f"{market['bp_delta']}bp: empty CLOB price history"
            )
            outcomes.append({**base, "probability_pct": None, "source_timestamp": None})
            continue

        valid, rejected = normalize_price_points(history, now)
        base["point_quality"] = {"accepted": len(valid), "rejected": rejected}
        if any(rejected.values()):
            warnings.append(f"event {mapping['mapping_evidence']['event_id']} outcome {market['bp_delta']}bp: "
                            f"rejected CLOB points {rejected}; latest valid point retained if available")
        if not valid:
            outcomes.append({**base, "probability_pct": None, "source_timestamp": None})
            continue
        instant, probability = valid[-1]["instant"], valid[-1]["probability"]

        outcomes.append(
            {
                **base,
                "probability_pct": round(probability * 100.0, 4),
                "source_timestamp": timeutil.iso_z(instant),
            }
        )
        latest_instants.append(instant)

    return outcomes, warnings, errors


def _data_status(outcomes: list[dict], latest_instants: list[datetime], now: datetime) -> str:
    """Meeting-level data status from the whole outcome set.

    The oldest accepted outcome decides freshness: a meeting with one fresh and
    four three-week-old quotes is STALE, never CURRENT. A timestamp within the
    future-skew tolerance counts as current (clock skew, not staleness).
    """
    if not any(outcome["probability_pct"] is not None for outcome in outcomes):
        return "UNAVAILABLE"
    if any(outcome["probability_pct"] is None for outcome in outcomes):
        return "PARTIAL"
    total = sum(outcome["probability_pct"] for outcome in outcomes) / 100
    if not OUTCOME_SUM_MIN <= total <= OUTCOME_SUM_MAX:
        return "PARTIAL"
    oldest = min(latest_instants)
    age_days = (now - oldest).total_seconds() / 86400.0
    if age_days <= FRESHNESS_MAX_AGE_DAYS:
        return "CURRENT"
    return "STALE"


def build_section(
    transport: Transport,
    meeting_dates: list[date],
    clock=timeutil.utc_now,
    sleep=time.sleep,
    tag_page_size: int = TAG_PAGE_SIZE,
    search_page_size: int = SEARCH_PAGE_SIZE,
    max_pages: int = MAX_DISCOVERY_PAGES,
) -> dict:
    """Discovery + mapping validation + current prices for the given meetings.

    Raises :class:`FedwatchError` (provider ``polymarket``) only for
    discovery-level failures; everything else is a per-meeting state
    (``mapping_status``/``data_status``) plus warnings. A validated mapping
    whose data quality is not ``CURRENT`` additionally records a
    provider-attributed error (``POLYMARKET_MARKET_DATA_PARTIAL`` /
    ``_STALE`` / ``_UNAVAILABLE``), so the composite snapshot can never report
    a partially covered, stale or unavailable mapping as an ordinary
    successful provider retrieval. ``NOT_FOUND`` and ``AMBIGUOUS`` are
    mapping-availability states, not provider outages, and stay error-free.

    Incomplete discovery coverage (a failed source or a source that did not
    finish paginating) records ``POLYMARKET_DISCOVERY_PARTIAL`` so an
    apparently definitive ``NOT_FOUND`` is never presented as complete
    coverage.
    """
    events, discovery_stats, warnings = discover_candidate_events(
        transport,
        tag_page_size=tag_page_size,
        search_page_size=search_page_size,
        max_pages=max_pages,
    )
    retrieved_at = clock()

    meetings = []
    provider_errors: list[FedwatchError] = []

    failed_discovery_sources = []
    if discovery_stats["tag"]["status"] == "ERROR":
        failed_discovery_sources.append(f"tag:{FED_RATES_TAG}")
    failed_discovery_sources.extend(
        f"search:{keyword}"
        for keyword, info in discovery_stats["searches"].items()
        if info["status"] == "ERROR"
    )
    coverage_complete = bool(discovery_stats.get("coverage_complete"))
    if failed_discovery_sources or not coverage_complete:
        # A failed or capped discovery source is a partial provider failure,
        # recorded explicitly so incomplete candidate coverage cannot be
        # presented as a fully successful provider retrieval.
        provider_errors.append(
            FedwatchError(
                PROVIDER_POLYMARKET,
                "POLYMARKET_DISCOVERY_PARTIAL",
                "Polymarket discovery did not complete candidate coverage; "
                "a NOT_FOUND mapping state is not exhaustive",
                detail={
                    "failed_sources": failed_discovery_sources,
                    "coverage_complete": coverage_complete,
                },
            )
        )

    # Latest instants are tracked per meeting for the freshness computation.
    for meeting_date in sorted(meeting_dates):
        mapping = resolve_mapping(events, meeting_date)
        entry = {
            "meeting_date": meeting_date.isoformat(),
            "mapping_status": mapping["mapping_status"],
            "event_id": None,
            "event_title": None,
            "event_end_date": None,
            "mapping_evidence": mapping["mapping_evidence"],
            "same_date_candidates": mapping["same_date_candidates"],
            "ambiguous_event_ids": mapping["ambiguous_event_ids"],
            "outcomes": [],
            "data_status": None,
            "source_timestamp": None,
            "latest_observation_date": None,
            "freshness_days": None,
            "freshness": {"status": "NOT_APPLICABLE", "age_days": None, "basis": None},
        }

        if mapping["mapping_status"] != "VALIDATED":
            meetings.append(entry)
            continue

        event = mapping["event"]
        entry.update(
            {
                "event_id": str(event.get("id")),
                "event_title": event.get("title"),
                "event_end_date": event.get("endDate"),
            }
        )
        outcomes, outcome_warnings, outcome_errors = fetch_current_outcomes(
            transport, mapping, clock=clock, sleep=sleep
        )
        entry["warnings"] = list(outcome_warnings)
        warnings.extend(outcome_warnings)
        # These failures came from this validated meeting's token requests.
        # Discovery failures above remain global; a failed December token must
        # not invalidate an otherwise complete October retained acquisition.
        provider_errors.extend(FedwatchError(exc.provider, exc.code, exc.message,
                                             detail={**(exc.detail or {}), "meeting_date": meeting_date.isoformat()})
                               for exc in outcome_errors)
        entry["outcomes"] = outcomes

        latest_instants = []
        for outcome in outcomes:
            if outcome["source_timestamp"]:
                latest_instants.append(timeutil.parse_iso_z(outcome["source_timestamp"]))
        now = clock()
        entry["data_status"] = _data_status(outcomes, latest_instants, now)
        if latest_instants:
            newest = max(latest_instants)
            oldest = min(latest_instants)
            entry["source_timestamp"] = timeutil.iso_z(newest)
            entry["latest_observation_date"] = newest.date().isoformat()
            entry["freshness_days"] = round((now - oldest).total_seconds() / 86400.0, 6)
            entry["freshness"] = {
                "status": entry["data_status"],
                "age_days": entry["freshness_days"],
                "basis": "oldest accepted outcome timestamp",
                "oldest_outcome_timestamp": timeutil.iso_z(oldest),
            }
            if entry["data_status"] == "STALE":
                warnings.append(
                    f"meeting {meeting_date.isoformat()}: Polymarket outcomes are older "
                    f"than the {FRESHNESS_MAX_AGE_DAYS}-day freshness window "
                    f"(oldest {entry['freshness_days']:.2f} days)"
                )
        if entry["data_status"] == "STALE":
            entry["warnings"].append(warnings[-1])
        # A validated mapping whose data quality is not CURRENT is a provider
        # quality failure for a current-observation snapshot: it must not leave
        # the provider reported as an ordinary OK with no partial marker.
        if entry["data_status"] in ("PARTIAL", "STALE", "UNAVAILABLE"):
            quality_code = {
                "PARTIAL": "POLYMARKET_MARKET_DATA_PARTIAL",
                "STALE": "POLYMARKET_MARKET_DATA_STALE",
                "UNAVAILABLE": "POLYMARKET_MARKET_DATA_UNAVAILABLE",
            }[entry["data_status"]]
            quality_message = {
                "PARTIAL": "Polymarket outcome coverage is incomplete for meeting "
                f"{meeting_date.isoformat()}",
                "STALE": "Polymarket outcomes are older than the "
                f"{FRESHNESS_MAX_AGE_DAYS}-day freshness window for meeting "
                f"{meeting_date.isoformat()}",
                "UNAVAILABLE": "no current Polymarket outcome probabilities for meeting "
                f"{meeting_date.isoformat()}",
            }[entry["data_status"]]
            provider_errors.append(
                FedwatchError(
                    PROVIDER_POLYMARKET,
                    quality_code,
                    quality_message,
                    detail={
                        "meeting_date": meeting_date.isoformat(),
                        "data_status": entry["data_status"],
                    },
                )
            )
        meetings.append(entry)

    return {
        "retrieved_at": timeutil.iso_z(retrieved_at),
        "source": SOURCE_LABEL,
        "method": "POLYMARKET_CLOB",
        "discovery": discovery_stats,
        "meetings": meetings,
        "warnings": warnings,
        "errors": provider_errors,
    }
