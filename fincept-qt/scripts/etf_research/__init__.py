"""MarketLab ETF Flow & Sector Rotation research acquisition (manual refresh only).

Runtime owner: MarketLab Terminal. The package is reached only through
``etf_research_data.py``, which the application starts when the user presses
**Refresh ETF Research Data** (or runs the headless ``--etf-research refresh``
command). Nothing here schedules, polls or runs on import.

Sources, in the order the owner's free-data policy prefers them:

* FRED public ``fredgraph.csv`` through the existing FedWatch transport and
  parser (MarketLab's qualified FRED path; no key, no spoofed User-Agent);
* World Bank public indicator API (country macro inputs MarketLab has no other
  source for);
* Yahoo through the ``yfinance`` package MarketLab already ships (ETF AUM/NAV
  snapshots, fund metadata, holdings, sector weights and daily history for the
  research universe, which the read-only IBKR route does not cover).

Every value leaves this package as a source observation with its own status;
nothing is filled, averaged or converted to a signal here. The application
persists the observations and computes every derived value itself.
"""
