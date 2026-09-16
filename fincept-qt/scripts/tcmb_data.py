"""
Central Bank of the Republic of Turkey (TCMB / CBRT) Data Wrapper
Fetches exchange rate data from the TCMB public XML bulletin service.

API Reference:
  Base URL:  https://www.tcmb.gov.tr/kurlar
  Format:    XML (UTF-8)
  Auth:      None required — fully public
  Docs:      https://www.tcmb.gov.tr/wps/wcm/connect/EN/TCMB+EN/Main+Menu/Statistics/Exchange+Rates/

URL patterns:
  Today:     GET /kurlar/today.xml
  By date:   GET /kurlar/{YYYYMM}/{DDMMYYYY}.xml   (business days only)

Exchange rates published on Turkish business days (Mon–Fri, excluding holidays).
Rates are TRY per foreign currency unit.
Each currency entry includes:
  ForexBuying   — wholesale foreign exchange buying
  ForexSelling  — wholesale foreign exchange selling
  BanknoteBuying  — banknote buying
  BanknoteSelling — banknote selling
  CrossRateUSD  — cross rate in USD (if applicable)

Currencies available (22):
  USD, AUD, DKK, EUR, GBP, CHF, SEK, CAD, KWD, NOK, SAR,
  JPY (per 100), RON, RUB, CNY, PKR, QAR, KRW, AZN, AED, KZT, XDR

Returns JSON output for C++ integration.
"""

import sys
import json
import requests
import traceback
import xml.etree.ElementTree as ET
from typing import Dict, Any, List, Optional
from datetime import datetime, date, timedelta, timezone


BASE_URL        = "https://www.tcmb.gov.tr/kurlar"
DEFAULT_TIMEOUT = 30

MAJOR_CURRENCIES = ["USD", "EUR", "GBP", "JPY", "CHF", "CAD", "AUD",
                    "SEK", "NOK", "DKK", "CNY", "SAR", "AED"]


# ---------------------------------------------------------------------------
# Error container
# ---------------------------------------------------------------------------

class TCMBError:
    def __init__(self, endpoint: str, error: str, status_code: Optional[int] = None):
        self.endpoint    = endpoint
        self.error       = error
        self.status_code = status_code
        self.timestamp   = int(datetime.now(timezone.utc).timestamp())

    def to_dict(self) -> Dict[str, Any]:
        return {
            "success":     False,
            "endpoint":    self.endpoint,
            "error":       self.error,
            "status_code": self.status_code,
            "timestamp":   self.timestamp,
            "type":        "TCMBError",
        }


# ---------------------------------------------------------------------------
# Main wrapper
# ---------------------------------------------------------------------------

class TCMBWrapper:
    """
    Wrapper for the TCMB (Central Bank of Turkey) XML exchange rate bulletin.

    Rates are TRY (Turkish Lira) per 1 unit of foreign currency.
    JPY is quoted per 100 JPY; the wrapper normalises to per 1 JPY.
    Data is published on Turkish business days only.
    """

    def __init__(self):
        self.session = requests.Session()
        self.session.headers.update({
            "User-Agent": "MarketLabTerminal/0.1.0",
            "Accept":     "application/xml, text/xml, */*",
        })

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _fetch(self, url: str) -> bytes:
        resp = self.session.get(url, timeout=DEFAULT_TIMEOUT)
        resp.raise_for_status()
        return resp.content

    def _parse_bulletin(self, content: bytes) -> Dict[str, Any]:
        """
        Parse a single TCMB XML bulletin into a dict of currency data.
        Returns {date, USD: {buy, sell, banknote_buy, banknote_sell}, ...}
        JPY values are normalised (÷100).
        """
        root       = ET.fromstring(content)
        date_str   = root.get("Date", "")          # MM/DD/YYYY
        bulten_no  = root.get("Bulten_No", "")

        # Normalise date to ISO format
        try:
            parsed_date = datetime.strptime(date_str, "%m/%d/%Y").date().isoformat()
        except ValueError:
            parsed_date = date_str

        rates: Dict[str, Any] = {}
        for cur in root.findall("Currency"):
            code = cur.get("CurrencyCode", "")
            unit = int(cur.findtext("Unit", "1") or "1")

            def _val(tag: str) -> Optional[float]:
                txt = cur.findtext(tag, "")
                if not txt or txt.strip() == "":
                    return None
                try:
                    return round(float(txt.replace(",", ".")), 6) / unit
                except ValueError:
                    return None

            rates[code] = {
                "forex_buying":      _val("ForexBuying"),
                "forex_selling":     _val("ForexSelling"),
                "banknote_buying":   _val("BanknoteBuying"),
                "banknote_selling":  _val("BanknoteSelling"),
                "unit":              unit,
            }
            cross = cur.findtext("CrossRateUSD", "").strip()
            if cross:
                try:
                    rates[code]["cross_rate_usd"] = float(cross.replace(",", "."))
                except ValueError:
                    pass

        return {
            "date":      parsed_date,
            "bulten_no": bulten_no,
            "rates":     rates,
        }

    def _date_url(self, d: date) -> str:
        return f"{BASE_URL}/{d.strftime('%Y%m')}/{d.strftime('%d%m%Y')}.xml"

    def _find_latest_business_day(self, start: Optional[date] = None,
                                  max_lookback: int = 10) -> Optional[date]:
        """Walk back from `start` (default today) to the most recent
        published bulletin. Turkish bank holidays have no bulletin, so the
        probe needs a few more days than a weekend."""
        anchor = start or date.today()
        for i in range(max_lookback):
            d = anchor - timedelta(days=i)
            try:
                content = self._fetch(self._date_url(d))
                ET.fromstring(content)   # verify it's valid XML
                return d
            except Exception:
                continue
        return None

    def _bulletin_rows(self, bulletin: Dict[str, Any]) -> List[Dict[str, Any]]:
        """Flatten one bulletin to per-currency rows the panel can render.

        The panel only keeps rows with a direct numeric value, so an object
        keyed by currency (each value an object) must never be returned raw.
        """
        rows: List[Dict[str, Any]] = []
        for code, data in sorted(bulletin["rates"].items()):
            row: Dict[str, Any] = {"date": bulletin["date"], "currency": code}
            for f in ("forex_buying", "forex_selling", "banknote_buying",
                      "banknote_selling", "cross_rate_usd", "unit"):
                val = data.get(f)
                if val is not None:
                    row[f] = val
            if any(k not in ("date", "currency")
                   for k in row):
                rows.append(row)
        return rows

    def _flatten_rates(self, bulletin: Dict[str, Any],
                       fields: Optional[List[str]] = None) -> Dict[str, Any]:
        """Flatten bulletin to {date, USD_buy, USD_sell, EUR_buy, ...} row."""
        if fields is None:
            fields = ["forex_buying", "forex_selling"]
        row: Dict[str, Any] = {"date": bulletin["date"]}
        for code, data in bulletin["rates"].items():
            for f in fields:
                val = data.get(f)
                if val is not None:
                    key = f"{code}_{f}" if len(fields) > 1 else code
                    row[key] = val
        return row

    # ------------------------------------------------------------------
    # Public methods
    # ------------------------------------------------------------------

    def get_today(self) -> Dict[str, Any]:
        """Latest exchange rate bulletin (most recent business day)."""
        try:
            content  = self._fetch(f"{BASE_URL}/today.xml")
            bulletin = self._parse_bulletin(content)
            return {
                "success":    True,
                "date":       bulletin["date"],
                "bulten_no":  bulletin["bulten_no"],
                "data":       self._bulletin_rows(bulletin),
                "count":      len(bulletin["rates"]),
                "note":       "TRY per 1 unit of foreign currency (JPY normalised from per-100)",
                "source":     "Central Bank of the Republic of Turkey",
                "url":        f"{BASE_URL}/today.xml",
                "timestamp":  int(datetime.now(timezone.utc).timestamp()),
            }
        except requests.exceptions.HTTPError as e:
            sc = e.response.status_code if e.response is not None else None
            return TCMBError("today", str(e), sc).to_dict()
        except Exception as e:
            return TCMBError("today", str(e)).to_dict()

    @staticmethod
    def _missing_bulletin(exc: Exception) -> bool:
        """True only for the provider condition that means 'no bulletin for
        this date'. TCMB answers 404 for weekends/bank holidays; any other
        status or a network/parse error is a genuine failure and must stay
        typed, never be converted into a 'no bulletin' walk-back."""
        if isinstance(exc, requests.exceptions.HTTPError) and exc.response is not None:
            return exc.response.status_code == 404
        return False

    def get_date(self, rate_date: str) -> Dict[str, Any]:
        """Exchange rates for a specific date (YYYY-MM-DD).

        TCMB publishes bulletins on Turkish business days only; a requested
        weekend or bank-holiday date has no bulletin. Only that provider
        condition triggers the bounded walk-back to the latest published
        bulletin; every other failure stays a typed error, because the
        requested bulletin may exist and merely have failed to download or
        parse.
        """
        try:
            d        = datetime.strptime(rate_date, "%Y-%m-%d").date()
            url      = self._date_url(d)
            try:
                content  = self._fetch(url)
                bulletin = self._parse_bulletin(content)
                served   = d
            except Exception as first_exc:
                if not self._missing_bulletin(first_exc):
                    raise
                served = self._find_latest_business_day(start=d - timedelta(days=1),
                                                        max_lookback=10)
                if served is None:
                    raise first_exc
                url      = self._date_url(served)
                content  = self._fetch(url)
                bulletin = self._parse_bulletin(content)
            return {
                "success":        True,
                "date":           bulletin["date"],
                "requested_date": rate_date,
                "bulten_no":      bulletin["bulten_no"],
                "data":           self._bulletin_rows(bulletin),
                "count":          len(bulletin["rates"]),
                "note":           ("TRY per 1 unit of foreign currency"
                                   + ("" if served == d else
                                      f"; no bulletin for {rate_date}, showing the latest published day")),
                "source":         "Central Bank of the Republic of Turkey",
                "url":            url,
                "timestamp":      int(datetime.now(timezone.utc).timestamp()),
            }
        except requests.exceptions.HTTPError as e:
            sc = e.response.status_code if e.response is not None else None
            return TCMBError(f"date/{rate_date}", str(e), sc).to_dict()
        except Exception as e:
            return TCMBError(f"date/{rate_date}", str(e)).to_dict()

    def _collect_range(self, start: date, end: date,
                       currencies: Optional[List[str]] = None,
                       fields: Optional[List[str]] = None) -> Dict[str, Any]:
        """Fetch every weekday bulletin in [start, end] once.

        Returns (rows, missing_dates, failed_dates):
          * missing_dates — provider returned 'no bulletin' (expected holidays),
          * failed_dates  — genuine fetch/parse failure for a day that produced
            no row; these must be reported, never swallowed.
        """
        rows: List[Dict[str, Any]] = []
        missing: List[str] = []
        failed: List[str] = []
        d = start
        while d <= end:
            if d.weekday() < 5:   # Mon–Fri only
                try:
                    content  = self._fetch(self._date_url(d))
                    bulletin = self._parse_bulletin(content)
                    if fields:
                        row: Dict[str, Any] = {"date": bulletin["date"]}
                        for code, data in bulletin["rates"].items():
                            if currencies is None or code in currencies:
                                for f in fields:
                                    val = data.get(f)
                                    if val is not None:
                                        row[f if len(currencies or []) == 1 else f"{code}_{f}"] = val
                    else:
                        row = {"date": bulletin["date"]}
                        for code, data in bulletin["rates"].items():
                            if currencies is None or code in currencies:
                                buy = data.get("forex_buying")
                                if buy is not None:
                                    row[code] = buy
                    if any(k != "date" for k in row):
                        rows.append(row)
                    else:
                        failed.append(f"{d.isoformat()}: bulletin carried no matching rates")
                except Exception as exc:
                    if self._missing_bulletin(exc):
                        missing.append(d.isoformat())
                    else:
                        failed.append(f"{d.isoformat()}: {exc}")
            d += timedelta(days=1)
        return {"rows": rows, "missing_dates": missing, "failed_dates": failed}

    def _range_result(self, start: date, end: date, label: str,
                      collected: Dict[str, Any]) -> Dict[str, Any]:
        rows         = collected["rows"]
        missing      = collected["missing_dates"]
        failed       = collected["failed_dates"]
        if not rows and failed:
            return TCMBError(
                label,
                f"no bulletin fetched for {start.isoformat()}..{end.isoformat()} "
                f"({len(failed)} failed days; first: {failed[0]})").to_dict()
        result: Dict[str, Any] = {
            "success":      True,
            "start_date":   start.isoformat(),
            "end_date":     end.isoformat(),
            "data":         rows,
            "count":        len(rows),
            "missing_dates": missing,
            "note":         "TRY per 1 unit (forex buying rate)",
            "source":       "Central Bank of the Republic of Turkey",
            "timestamp":    int(datetime.now(timezone.utc).timestamp()),
        }
        if failed:
            result["partial"]      = True
            result["failed_dates"] = failed
        return result

    def get_range(self, start_date: str, end_date: Optional[str] = None,
                  currencies: Optional[List[str]] = None) -> Dict[str, Any]:
        """
        Exchange rates for a date range — fetches each business day individually.
        Returns wide-format rows with forex_buying rates. Days with no bulletin
        (weekends, Turkish bank holidays) are expected and reported under
        `missing_dates`; genuine per-day failures make the result explicit
        partial or, when nothing could be fetched, a typed failure.
        """
        try:
            start = datetime.strptime(start_date, "%Y-%m-%d").date()
            end   = datetime.strptime(end_date, "%Y-%m-%d").date() if end_date else date.today()

            collected = self._collect_range(start, end, currencies)
            result    = self._range_result(start, end, "range", collected)
            if result.get("success"):
                result["currencies"] = currencies or "all"
            return result
        except Exception as e:
            return TCMBError("range", str(e)).to_dict()

    def get_currency(self, currency: str,
                     start_date: Optional[str] = None,
                     end_date: Optional[str] = None) -> Dict[str, Any]:
        """
        Buying and selling rates for a single currency over a date range.
        Defaults to the last 30 calendar days. Fetches each date exactly once;
        expected missing bulletins are reported under `missing_dates`, genuine
        per-day failures under `failed_dates` (explicit partial), and a range
        where nothing could be fetched is a typed failure.
        """
        currency = currency.upper()
        if start_date is None:
            start_date = (date.today() - timedelta(days=30)).isoformat()
        try:
            start = datetime.strptime(start_date, "%Y-%m-%d").date()
            end_d = datetime.strptime(end_date, "%Y-%m-%d").date() if end_date else date.today()

            collected = self._collect_range(
                start, end_d, [currency],
                fields=["forex_buying", "forex_selling", "banknote_buying", "banknote_selling"])
            result = self._range_result(start, end_d, f"currency/{currency}", collected)
            if result.get("success"):
                result["currency"] = currency
                result["note"]     = f"TRY per 1 {currency}"
            return result
        except Exception as e:
            return TCMBError(f"currency/{currency}", str(e)).to_dict()

    def get_major_currencies(self, start_date: Optional[str] = None) -> Dict[str, Any]:
        """Major currencies vs TRY — last 30 days if no start given."""
        if start_date is None:
            start_date = (date.today() - timedelta(days=30)).isoformat()
        return self.get_range(start_date, currencies=MAJOR_CURRENCIES)

    def get_overview(self) -> Dict[str, Any]:
        """Snapshot: today's rates for major currencies (buying & selling)."""
        today = self.get_today()
        if not today.get("success"):
            return today
        rows: List[Dict[str, Any]] = []
        for r in today.get("data", []):
            c = r.get("currency")
            if c not in MAJOR_CURRENCIES:
                continue
            row: Dict[str, Any] = {"date": today.get("date"), "currency": c}
            if r.get("forex_buying") is not None:
                row["buy"] = r["forex_buying"]
            if r.get("forex_selling") is not None:
                row["sell"] = r["forex_selling"]
            if len(row) > 2:
                rows.append(row)
        if not rows:
            return TCMBError("overview", "today's bulletin carries no major-currency rates").to_dict()
        return {
            "success":    True,
            "date":       today.get("date"),
            "bulten_no":  today.get("bulten_no"),
            "data":       rows,
            "count":      len(rows),
            "note":       "TRY per 1 unit of foreign currency",
            "source":     "Central Bank of the Republic of Turkey",
            "timestamp":  int(datetime.now(timezone.utc).timestamp()),
        }

    def available_currencies(self) -> Dict[str, Any]:
        """List of currencies published in TCMB bulletins."""
        return {
            "success": True,
            "major_currencies": MAJOR_CURRENCIES,
            "all_currencies": [
                "USD", "AUD", "DKK", "EUR", "GBP", "CHF", "SEK", "CAD",
                "KWD", "NOK", "SAR", "JPY", "RON", "RUB", "CNY", "PKR",
                "QAR", "KRW", "AZN", "AED", "KZT", "XDR",
            ],
            "note":   "TRY per 1 unit; JPY is per 100 in raw XML but normalised here",
            "source": "Central Bank of the Republic of Turkey",
            "timestamp": int(datetime.now(timezone.utc).timestamp()),
        }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

COMMANDS = {
    "today":    "                              — Latest bulletin (most recent business day)",
    "date":     "<YYYY-MM-DD>                 — Bulletin for a specific date",
    "range":    "<start> [end] [currencies]   — Date range, wide format (forex buying)",
    "currency": "<CCY> [start] [end]           — Single currency buy/sell history",
    "major":    "[start]                       — Major currencies last 30 days",
    "overview": "                              — Today's major currency snapshot",
    "available":"                              — List available currencies",
}


def _a(n: int, d: Any = None) -> Any:
    return sys.argv[n] if len(sys.argv) > n and sys.argv[n] else d


def main() -> None:
    if len(sys.argv) < 2:
        print(json.dumps({
            "error":    "No command provided.",
            "usage":    "python tcmb_data.py <command> [args...]",
            "commands": COMMANDS,
        }, indent=2))
        sys.exit(1)

    cmd     = sys.argv[1].lower()
    wrapper = TCMBWrapper()

    try:
        if cmd == "today":
            result = wrapper.get_today()
        elif cmd == "date":
            if len(sys.argv) < 3:
                result = {"error": "date requires <YYYY-MM-DD>"}
            else:
                result = wrapper.get_date(sys.argv[2])
        elif cmd == "range":
            if len(sys.argv) < 3:
                result = {"error": "range requires <start_date>"}
            else:
                ccys = [c.strip().upper() for c in _a(4, "").split(",")] if _a(4) else None
                result = wrapper.get_range(sys.argv[2], _a(3), ccys)
        elif cmd == "currency":
            if len(sys.argv) < 3:
                result = {"error": "currency requires <CCY>"}
            else:
                result = wrapper.get_currency(sys.argv[2], _a(3), _a(4))
        elif cmd == "major":
            result = wrapper.get_major_currencies(_a(2))
        elif cmd == "overview":
            result = wrapper.get_overview()
        elif cmd in ("available", "currencies"):
            result = wrapper.available_currencies()
        else:
            result = {"error": f"Unknown command: {cmd}", "commands": COMMANDS}

        print(json.dumps(result, indent=2, ensure_ascii=True))

    except Exception as exc:
        print(json.dumps({
            "success":   False,
            "error":     str(exc),
            "traceback": traceback.format_exc(),
        }, indent=2))
        sys.exit(1)


if __name__ == "__main__":
    main()
