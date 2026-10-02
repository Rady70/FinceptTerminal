# VectorBT Sharpe inference regression checks

The MarketLab fork correction authorized on 2026-10-02 replaces the two
inherited Deflated Sharpe calculations in `vbt_returns.py` and `vbt_metrics.py`.
Both now use `ReturnsAccessor.sharpe_inference` and the probability formula in
[Bailey and Lopez de Prado (2014), equations 1-2](https://www.davidhbailey.com/dhbpapers/deflated-sharpe.pdf).
The numerical checks were also compared with the
[vectorbt reference implementation](https://github.com/polakowo/vectorbt/blob/master/vectorbt/returns/metrics.py).

## Calculation and input contract

- The observed Sharpe uses arithmetic mean excess return divided by sample
  standard deviation, at the return series' observation frequency. It does not
  reuse the annualized, CAGR-based headline Sharpe.
- Sampling uncertainty uses `T - 1`, skewness and **Pearson kurtosis** (normal
  returns have kurtosis 3). The probability is the normal CDF of the benchmark
  difference divided by that uncertainty; multiplying by it is incorrect.
- `params.n_trials` declares the effective number of independent trials. It
  must be a positive integer. Omission means unknown, not one. Correlated
  configurations must not automatically be counted as independent trials.
- For more than one trial, `params.trial_sharpe_variance` is required: the
  cross-trial variance of **per-observation Sharpe estimates** on a consistent
  return frequency and risk-free convention. It is not return variance or the
  selected strategy's sampling variance. If the trial Sharpes were annualized
  by `sqrt(A)`, divide their variance by `A` first. A measured zero variance is
  valid; missing variance is not zero.
- The expected maximum benchmark uses this dispersion and the weighted normal
  quantiles from equation 1. One explicitly declared trial uses benchmark zero.
- `riskFreeRate` is treated as an annual simple rate; inference divides it by
  observations per year. Daily analysis uses the existing 252-day convention.
  Backtests with another `interval` require `periodsPerYear` when the risk-free
  rate is nonzero. With a zero rate, no annualization factor enters inference.

Example parameters for either `analyze_returns` (`analysisType: returns_stats`)
or `run_backtest`:

```json
{
  "params": {
    "n_trials": 100,
    "trial_sharpe_variance": 0.0019880715705765406
  }
}
```

These are illustrative inputs, not an inferred trial history. The native UI
does not collect trial count/variance, so its ordinary request reports DSR as
unavailable until a caller supplies the required inputs. A separate
`Probabilistic Sharpe (no multiple-testing adjustment)` reports confidence
against a zero benchmark. It is not a substitute for the multiple-testing
correction.

## Output and unavailable states

`deflated_sharpe_ratio()` returns a probability in `[0, 1]`, or `None`.
`sharpe_inference()` also returns a readable status, PSR, and the declared
trial metadata. Missing trial inputs, invalid inputs, fewer than ten
observations, non-finite returns, and constant returns are not converted into
zero confidence or a successful number. Original returns are retained for
inference before the accessor's legacy zero fill. The provider preserves JSON
`null`, not a string containing `None`.

Returns Analysis displays the status in its existing metrics table. Trial
count and variance are strings in that display summary to avoid the generic
table's currency formatting and four-decimal rounding. The inference result
and backtest `extended_stats` retain numeric trial metadata. The wire JSON
serializer exposes the latter as `extendedStats` in the backtest Raw JSON tab.

Backtest inference uses successive portfolio equity changes; the first value
has no preceding observation. Returns Analysis uses successive price changes;
when a benchmark is requested, its existing overlap selection still determines
the sample. These are different financial objects and need not yield equal
values. Returns are not fetched by these tests. The approximation assumes a
suitable independent-return sampling model; no serial-correlation correction,
strategy-search ledger, live data qualification, or profitability claim is
added by this fix.

## Run locally

From the application repository root, use its existing NumPy/pandas runtime.
`PythonRunner.cpp` routes VectorBT scripts to `venv-numpy1`:

```powershell
& "$env:LOCALAPPDATA\com.marketlab.terminal\venv-numpy1\Scripts\python.exe" -B -m unittest discover -s fincept-qt/marketlab/vectorbt_tests -p test_sharpe.py -v
```

The suite is kept separate from `marketlab/tests`, whose shared CTest discovery
has a stdlib-only dependency contract. There is no new dependency or hosted CI
configuration. Tests cover reference probabilities, unit consistency, Pearson
kurtosis, multiple trials, missing/error states, real provider dispatch with
injected prices, and JSON serialization. They do not launch the native UI.

Validation on 2026-10-02: all 29 tests passed under the routed Python 3.11.9 /
NumPy 1.26.4 / pandas 2.2.3 runtime and also under Python 3.11.9 / NumPy 2.4.6 /
pandas 2.3.3. Syntax checks passed for the three production files and test file;
the hosted-path source audit passed across 3,204 files. No native UI run,
installed-script deployment, or live-data run was performed for this repair.
