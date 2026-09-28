"""MarketLab FedWatch backend (Batch A).

Owned by MarketLab Terminal. Provides current Fed policy-expectations data as
a self-contained capability with no dependency on the retired reference
project:

* Fed-side probabilities: Investing.com Fed Rate Monitor retrieval, rounded
  display probability validation/normalization, and the unchanged CME
  local-step conversion.
* FRED DFEDTARU/DFEDTARL current target-range context.
* Federal Reserve FOMC meeting calendar (scrape plus tracked fallback snapshot).
* Polymarket FOMC event discovery, conservative automatic mapping validation,
  and current outcome probabilities.
* Fed-side vs Polymarket comparison with ``probability_diff_pp = Polymarket -
  Fed-side`` semantics.

The capability is read-only research. It never places, cancels or modifies
orders, never touches a wallet or broker, and adds no execution authority.

Batch A exposes a JSON document contract through ``fedwatch_data.py``; the
eventual Qt workspace consumes that contract through MarketLab's existing
generic economics provider path. Historical persistence and historical
analytics are deliberately later batches.
"""

__all__ = [
    "comparison",
    "errors",
    "fomc",
    "fred",
    "investing",
    "polymarket",
    "snapshot",
    "timeutil",
    "transport",
]
