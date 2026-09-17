"""
Our World in Data Fetcher
CO2, energy, health, poverty, education, democracy data for all countries.
Uses OWID API and GitHub CSV data. No API key required.
"""
import sys
import json
import csv
import io
import math
import os
import requests
from typing import Dict, Any, Optional, List

BASE_URL = "https://api.ourworldindata.org/v1"
GITHUB_RAW = "https://raw.githubusercontent.com/owid/owid-datasets/master/datasets"
CATALOG_URL = "https://catalog.ourworldindata.org"

session = requests.Session()
adapter = requests.adapters.HTTPAdapter(pool_connections=10, pool_maxsize=10, max_retries=3)
session.mount('https://', adapter)
session.mount('http://', adapter)
session.headers.update({"Accept": "application/json"})


def _make_request(endpoint: str, params: Dict = None) -> Any:
    """Make HTTP request with error handling."""
    url = f"{BASE_URL}/{endpoint}" if not endpoint.startswith('http') else endpoint
    try:
        response = session.get(url, params=params, timeout=30)
        response.raise_for_status()
        return response.json()
    except requests.exceptions.HTTPError as e:
        return {"error": f"HTTP {e.response.status_code}: {str(e)}"}
    except requests.exceptions.RequestException as e:
        return {"error": f"Request failed: {str(e)}"}
    except (json.JSONDecodeError, ValueError) as e:
        return {"error": f"JSON decode error: {str(e)}"}


def _fetch_owid_csv_api(dataset: str, country: str = None, columns: List[str] = None) -> Any:
    """Fetch OWID data via the indicators API."""
    params = {"datasetCode": dataset}
    if country:
        params["entityName"] = country
    data = _make_request("indicators", params=params)
    return data


def get_indicator(indicator_id: int) -> Any:
    """Get data for a specific OWID indicator by numeric ID."""
    data = _make_request(f"indicator/{indicator_id}.json")
    if isinstance(data, dict) and "error" not in data:
        return data
    return data


def search_indicators(query: str) -> Any:
    """Search OWID indicators by keyword."""
    params = {"query": query, "limit": 30}
    data = _make_request("search", params=params)
    if isinstance(data, dict) and "results" in data:
        return {"query": query, "results": data["results"][:30], "count": len(data["results"])}
    return data


def _coerce_owid_value(value):
    if value is None or value == "":
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return value
    if not math.isfinite(number):
        return None
    return int(number) if number.is_integer() else number


def _fetch_owid_csv(url: str, country: str, limit: int) -> Any:
    """OWID retired the JSON dumps for these datasets; the same series ship as
    CSV. Fetch the CSV, select the country block and return the last records."""
    try:
        response = session.get(url, timeout=60)
        response.raise_for_status()
        rows = list(csv.DictReader(io.StringIO(response.text)))
        wanted = country.lower()
        matches = [r for r in rows if (r.get("country") or "").lower() == wanted]
        matched_country = country
        available = []
        if not matches:
            available = sorted({(r.get("country") or "") for r in rows if r.get("country")})
            fuzzy = [name for name in available if wanted in name.lower()]
            if fuzzy:
                matched_country = fuzzy[0]
                matches = [r for r in rows if (r.get("country") or "") == matched_country]
        if not matches:
            return {"error": f"Country '{country}' not found", "available_sample": available[:20]}
        matches.sort(key=lambda r: r.get("year") or "")
        recent = matches[-limit:]
        records = [{k: _coerce_owid_value(v) for k, v in r.items()} for r in recent]
        return {
            "country": matched_country,
            "data": records,
            "count": len(records),
            "fields": list(records[0].keys()) if records else [],
            "source": "Our World in Data (CSV distribution)",
        }
    except Exception as e:
        return {"error": f"Failed to fetch OWID data: {str(e)}"}


def get_co2_data(country: str = "World") -> Any:
    """Get CO2 and greenhouse gas emissions data for a country.
    Uses Our World in Data CO2 dataset via GitHub.
    country: Country name as in OWID (e.g. 'United States', 'Germany', 'China', 'World').
    """
    url = "https://raw.githubusercontent.com/owid/co2-data/master/owid-co2-data.csv"
    return _fetch_owid_csv(url, country, 50)


def get_energy_data(country: str = "World") -> Any:
    """Get energy consumption and production data for a country.
    country: Country name as in OWID.
    """
    url = "https://raw.githubusercontent.com/owid/energy-data/master/owid-energy-data.csv"
    return _fetch_owid_csv(url, country, 30)


def get_health_data(country: str = None) -> Any:
    """Get health indicators (life expectancy, child mortality, etc.) via OWID API."""
    # Life expectancy indicator
    params = {"entityName": country} if country else {}
    data = _make_request("indicators/life-expectancy", params=params)
    if isinstance(data, dict) and "error" not in data:
        return {"category": "health", "country": country, "indicator": "life_expectancy", "data": data}
    # Fallback: return indicator IDs for health metrics
    return {
        "category": "health",
        "note": "Use indicator command with numeric IDs for specific metrics",
        "common_health_indicators": {
            "life_expectancy": "Search 'life expectancy' via search command",
            "child_mortality": "Search 'child mortality' via search command",
            "maternal_mortality": "Search 'maternal mortality rate' via search command",
        }
    }


def get_poverty_data(country: str = None) -> Any:
    """Get poverty and inequality data via OWID API."""
    params = {}
    if country:
        params["entityName"] = country
    data = _make_request("indicators/share-of-population-in-extreme-poverty", params=params)
    if isinstance(data, dict) and "error" not in data:
        return {"category": "poverty", "country": country, "indicator": "extreme_poverty", "data": data}
    return {
        "category": "poverty",
        "note": "Use search command to find specific poverty indicators",
        "common_poverty_indicators": {
            "extreme_poverty": "Search 'extreme poverty' via search command",
            "gini_coefficient": "Search 'gini coefficient' via search command",
            "income_share": "Search 'income share' via search command",
        }
    }


def get_democracy_data(country: str = None) -> Any:
    """Get democracy and governance indices (V-Dem, Freedom House, Polity)."""
    params = {}
    if country:
        params["entityName"] = country
    data = _make_request("indicators/electoral-democracy", params=params)
    if isinstance(data, dict) and "error" not in data:
        return {"category": "democracy", "country": country, "data": data}
    return {
        "category": "democracy",
        "note": "Use search command to find democracy/governance indicators",
        "suggested_searches": ["v-dem", "democracy index", "freedom house", "polity"],
    }


def main(args=None):
    if args is None:
        args = sys.argv[1:]
    if not args:
        print(json.dumps({"error": "No command provided. Available: co2, energy, health, poverty, democracy, indicator, search"}))
        return

    command = args[0]

    if command == "co2":
        country = args[1] if len(args) > 1 else "World"
        result = get_co2_data(country)
    elif command == "energy":
        country = args[1] if len(args) > 1 else "World"
        result = get_energy_data(country)
    elif command == "health":
        country = args[1] if len(args) > 1 else None
        result = get_health_data(country)
    elif command == "poverty":
        country = args[1] if len(args) > 1 else None
        result = get_poverty_data(country)
    elif command == "democracy":
        country = args[1] if len(args) > 1 else None
        result = get_democracy_data(country)
    elif command == "indicator":
        if len(args) < 2:
            result = {"error": "Usage: indicator <indicator_id>"}
        else:
            try:
                result = get_indicator(int(args[1]))
            except ValueError:
                result = {"error": f"Indicator ID must be numeric, got: {args[1]}"}
    elif command == "search":
        if len(args) < 2:
            result = {"error": "Usage: search <query>"}
        else:
            result = search_indicators(args[1])
    else:
        result = {"error": f"Unknown command: {command}. Available: co2, energy, health, poverty, democracy, indicator, search"}

    print(json.dumps(result))


if __name__ == "__main__":
    main()
