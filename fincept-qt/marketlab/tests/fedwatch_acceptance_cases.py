"""Inputs for the original-version manifest and git-free acceptance tests.

Captured public fixtures remain unmodified. Local CSVs and adversarial inputs
are explicitly synthetic; they establish software behaviour, not data rights
or native provider-export qualification.
"""
import copy
import csv
import io
import hashlib
import json
import re
from pathlib import Path

FIXTURES = Path(__file__).parent / "fixtures" / "fedwatch"
NOW = "2026-10-04T12:00:00+00:00"


def fixture_digest(raw):
    """Content identity independent of Git's platform checkout line endings."""
    return hashlib.sha256(raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n")).hexdigest()


def investing_html(meetings):
    return "".join('<div class="infoFed"><span>Meeting Time:</span><i>' + day +
        '</i><span>Future Price:</span><i>96.0</i></div><div class="percfedRateWrap">' +
        "".join(f'<div class="percfedRateItem"><span>{low} - {high}</span><i></i>'
                f'<div style="width: 10.0%"></div><span>{pct}%</span></div>'
                for low, high, pct in buckets) + '</div>' for day, buckets in meetings)


def calendar_html(rows):
    return '<html><div class="panel"><div class="panel-heading"><h4><a>2026 FOMC Meetings</a></h4></div>' + "".join(
        '<div class="fomc-meeting"><div class="fomc-meeting__month"><strong>' + month +
        '</strong></div><div class="fomc-meeting__date">' + day + '</div></div>'
        for month, day in rows) + '</div></html>'


def csv_text(rows):
    stream = io.StringIO()
    csv.writer(stream, lineterminator="\n").writerows(rows)
    return stream.getvalue()


def csv_variations(text, header_lines):
    lines = text.splitlines()
    spaced = lines[:header_lines]
    for line in lines[header_lines:]:
        spaced.append(csv_text([[f" {field} " for field in next(csv.reader([line]))]]).rstrip("\n"))
    return {"bom": "\ufeff" + text, "crlf": text.replace("\n", "\r\n"),
            "blank": text + "\n \n", "empty": text + ",,,,\n",
            "spaces": "\n".join(spaced) + "\n"}


def html_variations(text, investing=False):
    variants = {
        "attributes": re.sub(r'<([a-zA-Z][\w:-]*)(\s|>)', r'<\1 data-review="harmless"\2', text),
        "class_tokens": re.sub(r'class="([^"]+)"', r'class="extra \1 other"', text),
        "single_quotes": re.sub(r'([\w-]+)="([^"]*)"', r"\1='\2'", text),
        "whitespace": re.sub(r'>\s*<', '>\n \t<', text),
    }
    if investing:
        variants["after_percentage"] = text.replace('%</span>', '%</span><i class="trend-icon"></i>')
        variants["before_label"] = text.replace('<div class="percfedRateItem">',
            '<div class="percfedRateItem"><span class="sr-only">Target</span>')
    return variants


def json_variations(event):
    variants = {}
    for variant in ("case", "spaces", "order", "unknown"):
        changed = copy.deepcopy(event)
        if variant == "unknown":
            changed["unknown_review_field"] = {"harmless": True}
        for market in changed.get("markets", []):
            outcomes = json.loads(market["outcomes"])
            if variant in ("case", "spaces"):
                market["outcomes"] = json.dumps([label.upper() if variant == "case" else f" {label} " for label in outcomes])
            elif variant == "order":
                for field in ("outcomes", "outcomePrices", "clobTokenIds"):
                    market[field] = json.dumps(list(reversed(json.loads(market[field]))))
            else:
                market["unknown_review_field"] = "harmless"
        variants[variant] = changed
    return variants


def cases():
    result = {}

    def add(name, kind, payload, base=None, exception=None, **options):
        result[name] = dict(kind=kind, payload=payload, base=base, exception=exception, **options)

    for path in sorted(FIXTURES.iterdir()):
        if path.name.startswith("acceptance_manifest_"):
            continue
        if path.suffix not in (".html", ".csv", ".json"):
            continue
        name = "fixture/" + path.name
        text = path.read_text(encoding="utf-8")
        if path.name.startswith("investing_"):
            add(name, "investing", text)
            for variation, value in html_variations(text, investing=True).items():
                add(name + "/" + variation, "investing", value, base=name)
        elif path.name.startswith("fomc_"):
            add(name, "calendar", text)
            for variation, value in html_variations(text).items():
                add(name + "/" + variation, "calendar", value, base=name)
        elif path.name.startswith("fred_"):
            add(name, "fred", text)
            for variation, value in csv_variations(text, 1).items():
                if variation != "empty":
                    add(name + "/" + variation, "fred", value, base=name)
            add(name + "/leading_blank", "fred", "\n \n" + text, base=name)
        elif path.name.startswith("polymarket_"):
            event = json.loads(text)
            add(name, "polymarket", event)
            for variation, value in json_variations(event).items():
                add(name + "/" + variation, "polymarket", value, base=name)
        else:
            raise AssertionError("Uncovered fixture: " + path.name)

    monthly = "Symbol: ZQV26\nSource: https://www.investing.com/history?cid=1\n" + csv_text([
        ["Date", "Price", "Open", "High", "Low", "Vol.", "Change %"],
        ["10/01/2026", 96.1, 96, 96, 96, 1, "0%"], ["10/04/2026", 96.2, 96, 96, 96, 1, "0%"]])
    barchart = "Symbol: ZQV26\nDate Time,Open,High,Low,Close,Change,Volume,Open Interest\n" + \
        "2026-10-01,96,96,96,96.1,0,1,1000\n2026-10-04,96,96,96,96.2,0,1,1000\n"
    published = csv_text([["meeting_date", "observation_date", "rate_low_bp", "rate_high_bp", "probability_pct"],
        ["2026-10-28", "2026-10-02", 375, 400, 100]])
    for name, kind, text, headers in (("monthly", "monthly", monthly, 3),
                                     ("barchart", "barchart", barchart, 2), ("published", "published", published, 1)):
        key = "synthetic/" + name
        add(key, kind, text)
        for variation, value in csv_variations(text, headers).items():
            if kind == "monthly" and variation == "empty":
                value = text + ",,,,,,\n"
            add(key + "/" + variation, kind, value, base=key)

    # Snapshot acceptance includes locals and comparisons, not just raw tables.
    add("scenario/target_range", "target", None)
    add("scenario/snapshot_six_day_pair", "snapshot", None)
    add("scenario/fed_side_six_day_pair", "fed_side", None)
    live = (FIXTURES / "investing_fed_rate_monitor_2026-09-28.html").read_text(encoding="utf-8")
    for variation, value in html_variations(live, investing=True).items():
        add("scenario/snapshot/" + variation, "snapshot", value, base="scenario/snapshot_six_day_pair")
    official = (FIXTURES / "fomc_calendar_2026-09-28.html").read_text(encoding="utf-8")
    partial = official[:official.index("2027 FOMC Meetings")]
    add("scenario/partial_calendar_fresh", "calendar_fetch", partial, capture="2026-09-28T00:00:00Z")
    add("scenario/partial_calendar_stale", "calendar_fetch", partial, capture="2026-01-01T00:00:00Z")
    add("scenario/partial_calendar_unknown", "calendar_fetch", partial, capture=None)
    for age, capture in (("fresh", "2026-09-28T00:00:00Z"), ("stale", "2026-01-01T00:00:00Z"), ("unknown", None)):
        add("scenario/snapshot_partial_calendar_" + age, "snapshot_calendar", partial, capture=capture)
    for mode in ("snapshot", "fed_side"):
        add("round5/" + mode + "/stale_partial", "round5_local", partial, mode=mode)
        add("round5/" + mode + "/fred_failure", "round5_local", partial, mode=mode, fred_failure=True)
        decision = partial.replace('<div class="fomc-meeting',
            '<div class="fomc-meeting"><div class="fomc-meeting__month"><strong>September</strong></div>'
            '<div class="fomc-meeting__date">30</div></div><div class="fomc-meeting', 1)
        add("round5/" + mode + "/after_decision", "round5_local", decision, mode=mode,
            exception="FIRST_MEETING_AFTER_DECISION")
    broken = investing_html([("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100)]),
                             ("Dec 09, 2026 02:00PM ET", [(3.5, 3.75, "bad"), (3.75, 4, 70)])])
    add("scenario/hidden_bucket", "investing", broken + '<aside><span>4 - 4.25</span><div></div><span>30%</span></aside>')
    add("scenario/copy_conflict", "investing", investing_html([
        ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 97)]),
        ("Oct 28, 2026 02:00PM ET", [(3.5, 3.75, 60), (3.75, 4, 40)])]))

    # The ONLY allowed reductions. Each scenario has an independently stated
    # original error, checked by test_fedwatch_acceptance (never a blanket waiver).
    boundary = investing_html([("Oct 28, 2026 02:00PM ET", []),
                               ("Dec 09, 2026 02:00PM ET", [(3.75, 4, 100)])])
    boundary = boundary.replace('<span>Future Price:</span><i>96.0</i>', '', 1)
    add("exception/meeting_boundary", "investing_parse", boundary, exception="MEETING_BOUNDARY")
    add("exception/duplicate_bucket", "investing", investing_html([
        ("Oct 28, 2026 02:00PM ET", [(3.75, 4, 100), (3.75, 4, 90)])]), exception="DUPLICATE_BUCKET")
    add("exception/overlap", "investing", investing_html([
        ("Oct 28, 2026 02:00PM ET", [(3.5, 4, 50), (3.75, 4.25, 50)])]), exception="OVERLAP")
    add("exception/predecessor", "local_gap", None, exception="PREDECESSOR")
    add("exception/calendar_reverse", "calendar", calendar_html([("October", "29-28")]), exception="CALENDAR_REVERSE")
    add("exception/calendar_conflict", "calendar", calendar_html([("October", "27-28"), ("October", "26-28")]),
        exception="CALENDAR_CONFLICT")
    add("exception/fred_conflict", "fred", "DATE,DFEDTARU\n2026-09-28,4\n2026-09-28,5\n2026-09-29,4\n",
        exception="FRED_CONFLICT")
    add("exception/fred_future", "fred_history", None, exception="FRED_FUTURE")
    october = json.loads((FIXTURES / "polymarket_event_606422_october.json").read_text())
    nonbinary = copy.deepcopy(october)
    nonbinary["markets"][0]["outcomes"] = json.dumps(["Yes", "Yes"])
    add("exception/nonbinary", "polymarket", nonbinary, exception="NONBINARY")
    add("exception/clob_conflict", "polymarket", october, conflict_points=True, exception="CLOB_CONFLICT")
    return result
