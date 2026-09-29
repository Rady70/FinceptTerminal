"""Fed-side versus Polymarket comparison.

``probability_diff_pp = polymarket_probability_pct - fed_probability_pct`` —
the sign is preserved exactly from the qualified component, and a probability
gap is never interpreted as mispricing, a signal, or a trade instruction.

The Fed-side meeting-level distribution is structurally binary (at most two
adjacent 25 bp outcomes) while Polymarket may price broader tails (-50, -25, 0,
+25, +50 bp). A non-zero difference can therefore reflect both genuine market
disagreement and the model-shape difference; the payload states this limitation
explicitly rather than hiding it.
"""

from __future__ import annotations


def fed_probability_for(
    local_rows: list[dict] | None, bp_delta: int, open_ended: bool
) -> float | None:
    """Look up the Fed-side probability for one Polymarket outcome.

    ``None`` means there is no Fed-side distribution for the meeting at all —
    the difference is then unavailable (never zero). When a distribution does
    exist, an outcome the binary Fed-side distribution does not carry is 0.0,
    and an open-ended Polymarket tail is compared against the Fed-side tail
    sum on the same side, exactly as qualified.
    """
    if local_rows is None:
        return None
    if not open_ended:
        for row in local_rows:
            if row["outcome_bp"] == bp_delta:
                return float(row["probability_pct"])
        return 0.0
    if bp_delta >= 0:
        return float(
            sum(row["probability_pct"] for row in local_rows if row["outcome_bp"] >= bp_delta)
        )
    return float(
        sum(row["probability_pct"] for row in local_rows if row["outcome_bp"] <= bp_delta)
    )


def compare(fed_local: list[dict] | None, polymarket_outcomes: list[dict]) -> list[dict]:
    """One comparison row per Polymarket outcome.

    Every row keeps both source values distinct. ``probability_diff_pp`` is
    ``None`` unless both sides are available; a missing side is never rendered
    as zero.
    """
    rows: list[dict] = []
    for outcome in polymarket_outcomes:
        fed_probability = fed_probability_for(
            fed_local, int(outcome["outcome_bp"]), bool(outcome["open_ended"])
        )
        polymarket_probability = outcome["probability_pct"]
        difference = None
        if fed_probability is not None and polymarket_probability is not None:
            difference = round(float(polymarket_probability) - fed_probability, 6)
        rows.append(
            {
                "outcome_bp": outcome["outcome_bp"],
                "open_ended": outcome["open_ended"],
                "fed_probability_pct": fed_probability,
                "polymarket_probability_pct": polymarket_probability,
                "probability_diff_pp": difference,
            }
        )
    return rows
