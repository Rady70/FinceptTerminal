#!/usr/bin/env python3
"""Batch 3 CFTC validation roll-up (deterministic evidence assembly).

This script turns the validation harness outputs into the compact retained
evidence set committed under the control repository. It performs no research
work: every metric is counted or aggregated from the harness CSV outputs, and
the only computation independent of the harness is the qualifying
`:created_at` row-creation count, which mirrors the replay rule.

It writes, into --dest:

  * the harness outputs for each phase (summary_market/aggregate/confidence,
    state_quality, context_cases, family_conflicts, exclusions);
  * acquisition_coverage.csv / price_coverage.csv;
  * derived_confidence_<phase>.csv, derived_confidence_structure_<phase>.csv;
  * derived_state_quality_<phase>.csv (all scope) and
    derived_state_quality_primary_<phase>.csv (primary scope);
  * derived_spot_index_outcomes_<phase>.csv;
  * derived_market_spread_signs_<phase>.csv (defined-spread denominators);
  * derived_context_cases_<phase>.csv (the predeclared context cases);
  * derived_participant_diagnostic_development.csv + summary.

Usage (app-managed Python with pandas, e.g. venv-numpy2):

  python marketlab/cftc_validation_rollup.py \
      --data build/win-dev/cftc-validation/data \
      --out-root build/win-dev/cftc-validation/out \
      --dest <control-repo>/reports/cftc-batch3
"""

from __future__ import annotations

import argparse
import json
import shutil
from datetime import date, datetime
from pathlib import Path

import pandas as pd

PRIMARY = "primary_continuous"
BASE_FILES = (
    "summary_market",
    "summary_aggregate",
    "summary_confidence",
    "context_cases",
    "family_conflicts",
    "state_quality",
    "exclusions",
)
FAMILIES = ("legacy", "disaggregated", "tff")
COVERAGE_FAMILIES = (
    "tactical_4w",
    "swing_13w",
    "price_cot",
    "open_interest",
    "participant",
    "historical_context",
)
CONFIDENCE_LEVELS = ("High", "Medium", "Low")


def publication_metadata_qualifies(report_date: str, created_at: str) -> bool:
    """Mirror the replay rule: lag 3..120 days and created on/after 2022-09-14."""
    try:
        report = datetime.strptime(report_date[:10], "%Y-%m-%d").date()
        created = datetime.strptime(created_at[:10], "%Y-%m-%d").date()
    except (TypeError, ValueError):
        return False
    return 3 <= (created - report).days <= 120 and created >= date(2022, 9, 14)


def load_states(out_root: Path, run: str) -> pd.DataFrame:
    frame = pd.read_csv(out_root / run / "states.csv", low_memory=False)
    for horizon in ("4w", "13w"):
        frame[f"r{horizon}"] = pd.to_numeric(frame[f"return_{horizon}_pct"], errors="coerce")
    frame["report_date"] = pd.to_datetime(frame["report_date"])
    return frame


def acquisition_evidence(data: Path) -> tuple[pd.DataFrame, pd.DataFrame]:
    manifest = json.loads((data / "acquisition_manifest.json").read_text(encoding="utf-8"))
    qualifying: dict[tuple[str, str], int] = {}
    for path in sorted((data / "cftc").glob("*.json")):
        payload = json.loads(path.read_text(encoding="utf-8"))
        if payload.get("success") is not True:
            continue
        created = payload.get("publication", {}).get("created_at_by_report_date", {})
        key = (payload["validation"]["market_key"], payload["validation"]["report_family"])
        qualifying[key] = sum(
            1 for report, stamp in created.items() if isinstance(stamp, str) and publication_metadata_qualifies(report, stamp)
        )
    coverage_rows = []
    for pair in manifest["pairs"]:
        coverage_rows.append(
            {
                "market_key": pair.get("market_key"),
                "report_family": pair.get("report_family"),
                "status": pair["status"],
                "rows": pair.get("rows", ""),
                "first_report_date": pair.get("first", ""),
                "last_report_date": pair.get("last", ""),
                "qualifying_created_at_records": qualifying.get(
                    (pair.get("market_key"), pair.get("report_family")), ""
                ),
                "reason": pair.get("reason", ""),
            }
        )
    coverage = pd.DataFrame(coverage_rows).sort_values(["report_family", "market_key"])
    price_rows = []
    for price in manifest["prices"]:
        price_rows.append(
            {
                "market_key": price.get("market_key"),
                "symbol": price.get("symbol"),
                "status": price["status"],
                "points": price.get("points", ""),
                "first_date": price.get("first", ""),
                "last_date": price.get("last", ""),
            }
        )
    prices = pd.DataFrame(price_rows).sort_values("market_key")
    return coverage, prices


def confidence_evidence(states: pd.DataFrame, quality: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
    primary = states[states["proxy_scope"] == PRIMARY]
    derived_rows = []
    for level in CONFIDENCE_LEVELS:
        all_scope = states[states["confidence"] == level]
        primary_level = primary[primary["confidence"] == level]
        derived_rows.append(
            {
                "confidence": level,
                "all_scope_count": len(all_scope),
                "all_scope_share": len(all_scope) / len(states),
                "primary_scope_count": len(primary_level),
                "primary_mean_4w": primary_level["r4w"].dropna().mean(),
                "primary_mean_13w": primary_level["r13w"].dropna().mean(),
            }
        )
    structure_rows = []
    for level in CONFIDENCE_LEVELS:
        frame = primary[primary["confidence"] == level]
        if frame.empty:
            continue
        available_counts = frame["available_confidence_families"].fillna("").apply(
            lambda value: len([token for token in str(value).split("+") if token])
        )
        supporting_counts = frame["supporting_families"].fillna("").apply(
            lambda value: len([token for token in str(value).split("+") if token])
        )
        coherent = (
            (frame["tactical_4w"] == frame["swing_13w"])
            & frame["tactical_4w"].isin(["Bullish", "Bearish"])
        )
        structure_rows.append(
            {
                "confidence": level,
                "n_primary": len(frame),
                "mean_available_families": available_counts.mean(),
                "share_full_coverage": float((available_counts == len(COVERAGE_FAMILIES)).mean()),
                "share_price_available": float(
                    frame["available_confidence_families"].str.contains("price_cot").mean()
                ),
                "share_core_coherent": float(coherent.mean()),
                "share_core_opposed": float(frame["core_opposed"].eq("yes").mean()),
                "mean_supporting_families": supporting_counts.mean(),
                "share_price_conflicted": float(frame["price_conflicted"].eq("yes").mean()),
                "share_oi_conflicted": float(frame["oi_conflicted"].eq("yes").mean()),
                "share_participant_conflicted": float(frame["participant_conflicted"].eq("yes").mean()),
                "share_historical_conflicted": float(frame["historical_conflicted"].eq("yes").mean()),
                "n_markets": int(frame["market_key"].nunique()),
                "top_market_share": float(frame["market_key"].value_counts(normalize=True).iloc[0]),
                "share_legacy": float((frame["family"] == "legacy").mean()),
                "share_disaggregated": float((frame["family"] == "disaggregated").mean()),
                "share_tff": float((frame["family"] == "tff").mean()),
            }
        )
    # keep `quality` parameter for signature symmetry with the caller
    _ = quality
    return pd.DataFrame(derived_rows), pd.DataFrame(structure_rows)


def state_quality_evidence(states: pd.DataFrame, quality: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
    all_rows = []
    primary_rows = []
    for family in FAMILIES:
        q = quality[quality["family"] == family]
        eligible_all = int(q["eligible"].sum())
        candidate = int(q["candidate_observations"].sum())
        frame = states[states["family"] == family]
        primary = frame[frame["proxy_scope"] == PRIMARY]
        base = {
            "family": family,
            "eligible": len(frame),
            "buy_count": int(frame["state"].eq("BUY").sum()),
            "hold_count": int(frame["state"].eq("HOLD").sum()),
            "sell_count": int(frame["state"].eq("SELL").sum()),
            "confidence_high": int(frame["confidence"].eq("High").sum()),
            "confidence_medium": int(frame["confidence"].eq("Medium").sum()),
            "confidence_low": int(frame["confidence"].eq("Low").sum()),
        }
        for family_code in COVERAGE_FAMILIES:
            base[f"coverage_{family_code}"] = float(
                frame["available_confidence_families"].str.contains(family_code).mean()
            )
        base.update(
            {
                "candidate_observations": candidate,
                "coverage_rate": eligible_all / candidate if candidate else 0.0,
                "state_transitions": int(q["state_transitions"].sum()),
                "churn_rate": (
                    float(q["state_transitions"].sum() / (eligible_all - len(q))) if eligible_all > len(q) else 0.0
                ),
            }
        )
        all_rows.append(base)
        primary_row = {
            "family": family,
            "eligible": len(primary),
            "buy_count": int(primary["state"].eq("BUY").sum()),
            "hold_count": int(primary["state"].eq("HOLD").sum()),
            "sell_count": int(primary["state"].eq("SELL").sum()),
        }
        for family_code in COVERAGE_FAMILIES:
            primary_row[f"coverage_{family_code}"] = float(
                primary["available_confidence_families"].str.contains(family_code).mean()
            )
        primary_rows.append(primary_row)
    return pd.DataFrame(all_rows), pd.DataFrame(primary_rows)


def spot_index_outcomes(states: pd.DataFrame) -> pd.DataFrame:
    spot = states[states["proxy_scope"] != PRIMARY]
    rows = []
    for (market, family), frame in spot.groupby(["market_key", "family"]):
        for state in ("BUY", "HOLD", "SELL"):
            sub = frame[frame["state"] == state]
            for horizon in ("4w", "13w"):
                values = sub[f"r{horizon}"].dropna()
                rows.append(
                    {
                        "market_key": market,
                        "family": family,
                        "state": state,
                        "horizon": horizon,
                        "n": len(values),
                        "mean": values.mean() if len(values) else None,
                        "median": values.median() if len(values) else None,
                    }
                )
    return pd.DataFrame(rows).sort_values(["market_key", "family", "state", "horizon"])


def market_spread_signs(market: pd.DataFrame) -> pd.DataFrame:
    """Defined-spread denominators from the harness market summary.

    The harness leaves `mean_buy_minus_sell` empty when a market/family has no
    valid BUY or no valid SELL outcome, so missing sides are excluded from the
    denominator instead of being counted as zero spreads.
    """
    primary = market[market["proxy_scope"] == PRIMARY]
    rows = []
    for horizon in ("4W", "13W"):
        frame = primary[primary["horizon"] == horizon].copy()
        spread = pd.to_numeric(frame["mean_buy_minus_sell"], errors="coerce")
        defined = spread.dropna()
        rows.append(
            {
                "horizon": horizon.lower(),
                "series_total": len(frame),
                "series_defined": len(defined),
                "series_undefined": int(spread.isna().sum()),
                "series_positive": int((defined > 0).sum()),
                "series_negative": int((defined < 0).sum()),
                "series_zero": int((defined == 0).sum()),
                "mean_spread": float(defined.mean()) if len(defined) else None,
                "median_spread": float(defined.median()) if len(defined) else None,
            }
        )
    return pd.DataFrame(rows)


def context_cases(context: pd.DataFrame) -> pd.DataFrame:
    def label(row: pd.Series) -> str:
        context_word = row["historical_context"]
        tactical = row["tactical_4w"]
        swing = row["swing_13w"]
        if context_word == "CrowdedLong" and tactical == "Bullish":
            return "crowded_long_strengthening"
        if context_word == "CrowdedLong" and tactical == "Bearish":
            return "crowded_long_reversal"
        if context_word == "CrowdedShort" and tactical == "Bearish":
            return "crowded_short_weakening"
        if context_word == "CrowdedShort" and tactical == "Bullish":
            return "crowded_short_reversal"
        if context_word == "Neutral" and tactical == "Bullish" and swing == "Bullish":
            return "neutral_coherent_bullish"
        if context_word == "Neutral" and tactical == "Bearish" and swing == "Bearish":
            return "neutral_coherent_bearish"
        return "other_or_mixed"

    frame = context.copy()
    frame["case"] = frame.apply(label, axis=1)
    tested = frame[frame["case"] != "other_or_mixed"]
    rows = []
    for (case, family, horizon), group in tested.groupby(["case", "family", "horizon"]):
        total = group["n"].sum()
        if total == 0:
            continue
        rows.append(
            {
                "case": case,
                "family": family,
                "horizon": horizon,
                "n": int(total),
                "weighted_mean": float((group["n"] * group["mean"]).sum() / total),
                "weighted_positive_rate": float((group["n"] * group["positive_rate"]).sum() / total),
                "combinations": len(group),
            }
        )
    return pd.DataFrame(rows).sort_values(["case", "family", "horizon"])


def participant_diagnostic(v0: pd.DataFrame, core: pd.DataFrame) -> tuple[pd.DataFrame, str]:
    rows = []
    for family in FAMILIES:
        for horizon, label in (("r4w", "4w"), ("r13w", "13w")):
            row = {"scope": "development", "family": family, "horizon": label}
            for name, frame in (("v0", v0), ("core_only", core)):
                sub = frame[(frame["family"] == family) & (frame["proxy_scope"] == PRIMARY)]
                buy = sub[sub["state"] == "BUY"][horizon].dropna()
                sell = sub[sub["state"] == "SELL"][horizon].dropna()
                row[f"{name}_buy_n"] = len(buy)
                row[f"{name}_buy_mean"] = buy.mean()
                row[f"{name}_sell_n"] = len(sell)
                row[f"{name}_sell_mean"] = sell.mean()
                row[f"{name}_spread"] = buy.mean() - sell.mean() if len(buy) and len(sell) else None
            rows.append(row)
    common = v0.merge(core, on=["market_key", "family", "report_date"], suffixes=("_v0", "_core"))
    state_changes = int((common["state_v0"] != common["state_core"]).sum())
    confidence_changes = int((common["confidence_v0"] != common["confidence_core"]).sum())
    summary = (
        "Development diagnostic: full v0 vs participant-unavailable (non-speculative legs stripped).\n"
        f"common states: {len(common)}\n"
        f"state changes: {state_changes} ({state_changes / len(common):.4f})\n"
        f"confidence changes: {confidence_changes} ({confidence_changes / len(common):.4f})\n"
    )
    return pd.DataFrame(rows), summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Batch 3 CFTC validation evidence roll-up")
    parser.add_argument("--data", default="build/win-dev/cftc-validation/data")
    parser.add_argument("--out-root", default="build/win-dev/cftc-validation/out")
    parser.add_argument("--dest", required=True)
    parser.add_argument(
        "--runs",
        default="development=dev-v0,holdout=holdout-v0,all=all-v0",
        help="phase=run-directory pairs",
    )
    parser.add_argument("--core-only-run", default="dev-coreonly")
    args = parser.parse_args()

    data = Path(args.data).resolve()
    out_root = Path(args.out_root).resolve()
    dest = Path(args.dest).resolve()
    dest.mkdir(parents=True, exist_ok=True)
    runs = dict(pair.split("=", 1) for pair in args.runs.split(","))

    coverage, prices = acquisition_evidence(data)
    coverage.to_csv(dest / "acquisition_coverage.csv", index=False)
    prices.to_csv(dest / "price_coverage.csv", index=False)

    for phase, run in runs.items():
        for name in BASE_FILES:
            shutil.copyfile(out_root / run / f"{name}.csv", dest / f"{name}_{phase}.csv")
        states = load_states(out_root, run)
        quality = pd.read_csv(out_root / run / "state_quality.csv")
        confidence, structure = confidence_evidence(states, quality)
        confidence.to_csv(dest / f"derived_confidence_{phase}.csv", index=False)
        structure.to_csv(dest / f"derived_confidence_structure_{phase}.csv", index=False)
        all_quality, primary_quality = state_quality_evidence(states, quality)
        all_quality.to_csv(dest / f"derived_state_quality_{phase}.csv", index=False)
        primary_quality.to_csv(dest / f"derived_state_quality_primary_{phase}.csv", index=False)
        spot_index_outcomes(states).to_csv(dest / f"derived_spot_index_outcomes_{phase}.csv", index=False)
        market = pd.read_csv(out_root / run / "summary_market.csv")
        market_spread_signs(market).to_csv(dest / f"derived_market_spread_signs_{phase}.csv", index=False)
        context = pd.read_csv(out_root / run / "context_cases.csv")
        context_cases(context).to_csv(dest / f"derived_context_cases_{phase}.csv", index=False)

    v0 = load_states(out_root, "dev-v0")
    core = load_states(out_root, args.core_only_run)
    diagnostic, summary = participant_diagnostic(v0, core)
    diagnostic.to_csv(dest / "derived_participant_diagnostic_development.csv", index=False)
    (dest / "derived_participant_diagnostic_summary.txt").write_text(summary, encoding="utf-8")

    print(f"Batch 3 evidence rolled up into {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
