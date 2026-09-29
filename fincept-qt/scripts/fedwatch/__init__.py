"""MarketLab FedWatch backend (Batch A current data; Batch B durable history).

Owned by MarketLab Terminal. Provides Fed policy-expectations data as a
self-contained capability with no dependency on the retired reference project:

* Fed-side probabilities: Investing.com Fed Rate Monitor retrieval, rounded
  display probability validation/normalization, and the unchanged CME
  local-step conversion.
* FRED DFEDTARU/DFEDTARL current target-range context and the full daily
  target-range history used to establish FOMC decisions.
* Federal Reserve FOMC meeting calendar (scrape plus tracked fallback snapshot).
* Polymarket FOMC event discovery, conservative automatic mapping validation,
  current outcome probabilities and historical CLOB price backfill.
* Fed-side vs Polymarket comparison with ``probability_diff_pp = Polymarket -
  Fed-side`` semantics.
* Durable MarketLab SQLite observation history, duplicate-free accepted
  observations, the FOMC meeting lifecycle, the approved historical
  calculations and the optional historical ZQ reconstruction path.

The capability is read-only research. It never places, cancels or modifies
orders, never touches a wallet or broker, and adds no execution authority.

Batch A exposes a JSON document contract through ``fedwatch_data.py``; Batch B
adds durable history, backfill and analytics commands on the same contract and
the same application-owned data path. The dedicated Qt workspace is Batch C.
"""

__all__ = [
    "analytics",
    "comparison",
    "errors",
    "fomc",
    "fred",
    "history",
    "investing",
    "polymarket",
    "snapshot",
    "store",
    "timeutil",
    "transport",
    "zq",
]
