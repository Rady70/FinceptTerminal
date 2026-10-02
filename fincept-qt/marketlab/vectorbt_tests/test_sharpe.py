"""Offline Sharpe regression tests; run with the app's NumPy/pandas interpreter.

This directory is separate from the stdlib-only MarketLab fixture discovery.
No market data, brokerage connection, or installed vectorbt is needed.
"""

import contextlib
import io
import json
import math
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

import numpy as np
import pandas as pd

SCRIPT_DIR = Path(__file__).resolve().parents[2] / 'scripts/Analytics/backtesting/vectorbt'
sys.path.insert(0, str(SCRIPT_DIR))

from vbt_returns import ReturnsAccessor, _sharpe_probability
from vbt_metrics import _extract_extended_stats
from vectorbt_provider import VectorBTProvider
from base.base_provider import json_response


def returns_fixture():
    """504 observations, arithmetic annual SR=1, skew=0, Pearson kurtosis=1."""
    noise = np.tile([-1.0, 1.0], 252)
    return pd.Series(noise / noise.std(ddof=1) * 0.01 + 0.01 / math.sqrt(252))


def prices_fixture(returns=None):
    if returns is None:
        returns = returns_fixture()
    values = np.r_[100.0, 100.0 * np.cumprod(1 + returns.to_numpy())]
    return pd.Series(values, index=pd.bdate_range('2020-01-01', periods=len(values)))


class FixtureProvider(VectorBTProvider):
    """Inject local prices through the provider's existing data-loading seam."""

    def __init__(self, prices=None):
        super().__init__()
        self.prices = prices_fixture() if prices is None else prices

    def _load_market_data(self, *args, **kwargs):
        return self.prices.copy(), True

    def _import_vbt(self):
        return SimpleNamespace(__version__='offline-fixture')


class SharpeMathTests(unittest.TestCase):
    def setUp(self):
        self.acc = ReturnsAccessor(returns_fixture())

    def test_two_year_normal_moments_example(self):
        probability = _sharpe_probability(1 / math.sqrt(252), 504, 0, 3)
        self.assertAlmostEqual(probability, 0.920938175206, places=11)

    def test_published_five_year_example(self):
        # Bailey-Lopez de Prado (2014), pp. 9-10: annual SR=2.5,
        # T=1250, 250/year, skew=-3, Pearson kurtosis=10, N=100,
        # annual cross-trial variance=0.5. Independent SciPy reference:
        # expected maximum per-period SR=0.11317200186513214, DSR~=0.90.
        result = _sharpe_probability(
            2.5 / math.sqrt(250), 1250, -3, 10, 0.11317200186513214,
        )
        self.assertAlmostEqual(result, 0.9003968344493903, places=12)

    def test_raw_returns_use_arithmetic_per_period_sharpe(self):
        self.assertAlmostEqual(self.acc.deflated_sharpe_ratio(1), 0.9211441831976721, places=12)

    def test_annualization_does_not_change_zero_rate_inference(self):
        for periods in (12, 252, 365, 252 * 390):
            with self.subTest(periods=periods):
                acc = ReturnsAccessor(returns_fixture(), year_freq=periods)
                self.assertAlmostEqual(acc.deflated_sharpe_ratio(1), 0.9211441831976721, places=12)

    def test_negative_skew_and_fat_tails_reduce_positive_sharpe_confidence(self):
        baseline = _sharpe_probability(0.1, 504, 0, 3)
        self.assertLess(_sharpe_probability(0.1, 504, -2, 3), baseline)
        self.assertLess(_sharpe_probability(0.1, 504, 0, 10), baseline)

    def test_actual_fat_tailed_returns_reduce_probability(self):
        results = []
        for pattern in ([-1.0, 1.0], [-3.0, 0.0, 0.0, 0.0, 0.0, 3.0]):
            noise = np.tile(pattern, 504 // len(pattern))
            values = noise / noise.std(ddof=1) * 0.01 + 0.01 / math.sqrt(252)
            results.append(ReturnsAccessor(pd.Series(values)).deflated_sharpe_ratio(1))
        self.assertLess(results[1], results[0])

    def test_multiple_trial_reference_values(self):
        # Independently evaluated with scipy.stats.norm for the known moments
        # in returns_fixture; variance is in per-observation Sharpe units.
        expected = {2: 0.8140860147710973, 10: 0.4357362341386038, 100: 0.13182770227896645}
        for trials, value in expected.items():
            with self.subTest(trials=trials):
                self.assertAlmostEqual(self.acc.deflated_sharpe_ratio(trials, 1 / 503), value, places=12)

    def test_more_trials_and_more_trial_variance_lower_dsr(self):
        self.assertLess(self.acc.deflated_sharpe_ratio(100, 1 / 503),
                        self.acc.deflated_sharpe_ratio(10, 1 / 503))
        self.assertLess(self.acc.deflated_sharpe_ratio(10, 2 / 503),
                        self.acc.deflated_sharpe_ratio(10, 1 / 503))

    def test_zero_trial_variance_is_valid_and_not_missing(self):
        self.assertAlmostEqual(self.acc.deflated_sharpe_ratio(100, 0),
                               self.acc.deflated_sharpe_ratio(1), places=12)

    def test_unknown_trials_do_not_default_to_one(self):
        result = self.acc.sharpe_inference()
        self.assertIsNone(result['deflatedSharpe'])
        self.assertIsNone(result['deflatedSharpeTrials'])
        self.assertIn('not supplied', result['deflatedSharpeStatus'])
        self.assertAlmostEqual(result['probabilisticSharpe'], 0.9211441831976721, places=12)

    def test_multiple_trials_require_trial_variance(self):
        result = self.acc.sharpe_inference(10)
        self.assertIsNone(result['deflatedSharpe'])
        self.assertIn('variance not supplied', result['deflatedSharpeStatus'])

    def test_bad_trial_counts_are_unavailable(self):
        for trials in (0, -1, True, 1.5, '10', float('nan'), float('inf')):
            with self.subTest(trials=trials):
                result = self.acc.sharpe_inference(trials, 0.001)
                self.assertIsNone(result['deflatedSharpe'])
                self.assertIn('positive integer', result['deflatedSharpeStatus'])

    def test_bad_trial_variances_are_unavailable(self):
        for variance in (-1, True, '0.01', float('nan'), float('inf')):
            with self.subTest(variance=variance):
                result = self.acc.sharpe_inference(10, variance)
                self.assertIsNone(result['deflatedSharpe'])
                self.assertIn('invalid', result['deflatedSharpeStatus'])

    def test_large_trial_count_stays_finite(self):
        value = self.acc.deflated_sharpe_ratio(10**18, 0.001)
        self.assertTrue(math.isfinite(value))
        self.assertGreaterEqual(value, 0)
        self.assertLessEqual(value, 1)

    def test_missing_and_nonfinite_observations_are_not_filled_or_dropped(self):
        for bad in (np.nan, np.inf, -np.inf):
            with self.subTest(bad=bad):
                values = returns_fixture()
                values.iloc[50] = bad
                result = ReturnsAccessor(values).sharpe_inference(1)
                self.assertIsNone(result['deflatedSharpe'])
                self.assertIsNone(result['probabilisticSharpe'])

    def test_short_and_constant_returns_are_unavailable(self):
        for values in ([], [0.01] * 9, [0.0] * 504, [0.001] * 504, [1.0] * 504):
            with self.subTest(length=len(values), first=values[:1]):
                result = ReturnsAccessor(pd.Series(values, dtype=float)).sharpe_inference(1)
                self.assertIsNone(result['deflatedSharpe'])
                self.assertIsNone(result['probabilisticSharpe'])

    def test_zero_and_negative_mean_return_probabilities(self):
        noise = pd.Series(np.tile([-0.01, 0.01], 252))
        self.assertEqual(ReturnsAccessor(noise).deflated_sharpe_ratio(1), 0.5)
        self.assertLess(ReturnsAccessor(noise - 0.001).deflated_sharpe_ratio(1), 0.5)

    def test_annual_risk_free_conversion(self):
        rate = 0.04
        shifted = ReturnsAccessor(returns_fixture() + rate / 252)
        self.assertAlmostEqual(shifted.deflated_sharpe_ratio(1, risk_free=rate),
                               self.acc.deflated_sharpe_ratio(1), places=12)

    def test_invalid_risk_free_or_frequency_is_unavailable(self):
        for rate, periods in ((float('nan'), 252), (True, 252), (0.04, None), (0.04, 0)):
            with self.subTest(rate=rate, periods=periods):
                result = ReturnsAccessor(returns_fixture(), year_freq=periods).sharpe_inference(1, risk_free=rate)
                self.assertIsNone(result['deflatedSharpe'])

    def test_invalid_sampling_variance_does_not_return_a_sharpe_as_probability(self):
        with self.assertRaisesRegex(ValueError, 'sampling variance'):
            _sharpe_probability(1, 504, 10, 3)

    def test_non_daily_risk_free_requires_explicit_frequency(self):
        unknown = ReturnsAccessor(returns_fixture(), year_freq=None)
        self.assertAlmostEqual(unknown.deflated_sharpe_ratio(1), 0.9211441831976721, places=12)
        self.assertIsNone(unknown.deflated_sharpe_ratio(1, risk_free=0.04))

    def test_extreme_observations_are_unavailable_without_numeric_overflow(self):
        values = pd.Series(np.tile([-1e308, 1e308], 252))
        result = ReturnsAccessor(values).sharpe_inference(1)
        self.assertIsNone(result['deflatedSharpe'])
        self.assertIsNone(result['probabilisticSharpe'])


class SharpeIntegrationTests(unittest.TestCase):
    def test_extended_stats_and_returns_accessor_agree(self):
        prices = prices_fixture()
        portfolio = SimpleNamespace(value=lambda: prices)
        for trials in (None, 1, 10):
            with self.subTest(trials=trials):
                variance = 1 / 503 if trials == 10 else None
                # Deliberately wrong headline Sharpe must not affect inference.
                extended = _extract_extended_stats(
                    portfolio, {'Sharpe Ratio': 999}, 100, prices,
                    n_trials=trials, trial_sharpe_variance=variance,
                )
                expected = ReturnsAccessor(prices.pct_change(fill_method=None).iloc[1:]).sharpe_inference(trials, variance)
                self.assertEqual(extended['deflatedSharpeStatus'], expected['deflatedSharpeStatus'])
                if expected['deflatedSharpe'] is None:
                    self.assertIsNone(extended['deflatedSharpe'])
                else:
                    self.assertAlmostEqual(extended['deflatedSharpe'], expected['deflatedSharpe'], places=12)

    def test_extended_stats_preserve_missing_equity(self):
        prices = prices_fixture()
        prices.iloc[50] = np.nan
        result = _extract_extended_stats(SimpleNamespace(value=lambda: prices), {}, 100, prices, n_trials=1)
        self.assertIsNone(result['deflatedSharpe'])
        self.assertIn('Unavailable', result['deflatedSharpeStatus'])

    def test_returns_provider_preserves_null_and_status_in_wire_json(self):
        result = FixtureProvider().analyze_returns({'analysisType': 'returns_stats', 'symbols': ['FIXTURE']})
        self.assertTrue(result['success'], result)
        wire = json.loads(json_response(result))
        stats = wire['data']['stats']
        self.assertIsNone(stats['Deflated Sharpe'])
        self.assertIn('not supplied', stats['Deflated Sharpe Status'])
        self.assertIsInstance(stats['Probabilistic Sharpe (no multiple-testing adjustment)'], float)

    def test_returns_provider_passes_trial_parameters_and_risk_free_rate(self):
        request = {'analysisType': 'returns_stats', 'symbols': ['FIXTURE'], 'riskFreeRate': 0.04,
                   'params': {'n_trials': 10, 'trial_sharpe_variance': 1 / 503}}
        result = FixtureProvider().analyze_returns(request)
        self.assertTrue(result['success'], result)
        expected = ReturnsAccessor(returns_fixture()).deflated_sharpe_ratio(10, 1 / 503, 0.04)
        self.assertAlmostEqual(result['data']['stats']['Deflated Sharpe'], expected, places=12)
        self.assertEqual(result['data']['stats']['Deflated Sharpe Trials'], '10')

    def test_trial_inputs_avoid_currency_formatting_and_small_value_rounding(self):
        stats = ReturnsAccessor(returns_fixture()).stats(10000, 1e-8)
        self.assertEqual(stats['Deflated Sharpe Trials'], '10000')
        self.assertEqual(float(stats['Deflated Sharpe Trial Variance (per observation)']), 1e-8)
        self.assertIsInstance(stats['Deflated Sharpe Trial Variance (per observation)'], str)

    def test_returns_provider_keeps_missing_price_observations(self):
        prices = prices_fixture()
        prices.iloc[50] = np.nan
        result = FixtureProvider(prices).analyze_returns({
            'analysisType': 'returns_stats', 'symbols': ['FIXTURE'], 'params': {'n_trials': 1},
        })
        self.assertTrue(result['success'], result)
        self.assertIsNone(result['data']['stats']['Deflated Sharpe'])
        self.assertIn('invalid return', result['data']['stats']['Deflated Sharpe Status'])

    def test_backtest_provider_passes_trial_parameters(self):
        request = {'symbols': ['FIXTURE'], 'strategy': {'type': 'buy_and_hold'},
                   'initialCapital': 100000, 'startDate': '2020-01-01', 'endDate': '2022-01-01',
                   'params': {'n_trials': 10, 'trial_sharpe_variance': 1 / 503}}
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            result = FixtureProvider().run_backtest(request)
        self.assertTrue(result['success'], result)
        extended = result['data']['extended_stats']
        self.assertEqual(extended['deflatedSharpeTrials'], 10)
        self.assertEqual(extended['deflatedSharpeTrialVariance'], 1 / 503)
        self.assertIsNotNone(extended['deflatedSharpe'])
        self.assertLess(extended['deflatedSharpe'], extended['probabilisticSharpe'])
        wire = json.loads(json_response(result))
        self.assertIn('deflatedSharpe', wire['data']['extendedStats'])


if __name__ == '__main__':
    unittest.main()
