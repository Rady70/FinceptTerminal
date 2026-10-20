#pragma once
// MarketGauges — the one set of rules the dashboard uses for the VIX regime
// and for the basket sentiment score.
//
// The Risk Metrics, Market Sentiment and Performance widgets and the Market
// Pulse panel each carried their own copy: Risk Metrics called VIX 18.76
// "NORMAL" while the other two coloured it as a warning above 18, and the
// Sentiment widget scored −100..+100 on ±0.5% moves while Market Pulse scored
// 0..100 on ±0.3% moves — two different "sentiment" numbers on one screen.
// Header-only, no Qt GUI, so the rules are unit-testable.
#include <QString>

#include <cmath>
#include <optional>

namespace fincept::screens::gauges {

/// A daily move counts as up/down only beyond this many percent.
inline constexpr double kMoveThresholdPct = 0.3;

/// +1 up, −1 down, 0 flat (within ±kMoveThresholdPct).
inline int classify_move(double change_pct) {
    if (change_pct > kMoveThresholdPct)
        return 1;
    if (change_pct < -kMoveThresholdPct)
        return -1;
    return 0;
}

enum class Tone { Positive, Neutral, Warning, Negative };

enum class VixRegime { Low, Normal, Elevated, High, Extreme };

inline VixRegime vix_regime(double vix) {
    if (vix < 15)
        return VixRegime::Low;
    if (vix < 20)
        return VixRegime::Normal;
    if (vix < 30)
        return VixRegime::Elevated;
    if (vix < 40)
        return VixRegime::High;
    return VixRegime::Extreme;
}

/// Untranslated key; widgets translate it in their own context.
inline const char* vix_regime_key(VixRegime r) {
    switch (r) {
        case VixRegime::Low:
            return "LOW VOLATILITY";
        case VixRegime::Normal:
            return "NORMAL";
        case VixRegime::Elevated:
            return "ELEVATED";
        case VixRegime::High:
            return "HIGH STRESS";
        case VixRegime::Extreme:
            return "EXTREME FEAR";
    }
    return "NORMAL";
}

inline Tone vix_tone(VixRegime r) {
    switch (r) {
        case VixRegime::Low:
            return Tone::Positive;
        case VixRegime::Normal:
            return Tone::Neutral;
        case VixRegime::Elevated:
            return Tone::Warning;
        case VixRegime::High:
        case VixRegime::Extreme:
            return Tone::Negative;
    }
    return Tone::Neutral;
}

/// Basket sentiment: 50 + (up − down) / observed × 50, then −20 when VIX > 30,
/// −10 when VIX > 25, +10 when VIX < 15, bounded to 0..100. A proxy over a
/// fixed basket, not an external fear & greed index.
struct BasketScore {
    int score = 50;
    const char* key = "NEUTRAL"; ///< EXTREME FEAR | FEAR | NEUTRAL | GREED | EXTREME GREED
    Tone tone = Tone::Warning;
};

inline std::optional<BasketScore> basket_score(int up, int down, int flat, std::optional<double> vix) {
    const int total = up + down + flat;
    if (total <= 0)
        return std::nullopt;
    int score = 50 + static_cast<int>((static_cast<double>(up - down) / total) * 50);
    if (vix && *vix > 0) {
        if (*vix > 30)
            score -= 20;
        else if (*vix > 25)
            score -= 10;
        else if (*vix < 15)
            score += 10;
    }
    score = score < 0 ? 0 : (score > 100 ? 100 : score);
    BasketScore out;
    out.score = score;
    if (score <= 20) {
        out.key = "EXTREME FEAR";
        out.tone = Tone::Negative;
    } else if (score <= 40) {
        out.key = "FEAR";
        out.tone = Tone::Warning;
    } else if (score <= 60) {
        out.key = "NEUTRAL";
        out.tone = Tone::Warning;
    } else if (score <= 80) {
        out.key = "GREED";
        out.tone = Tone::Positive;
    } else {
        out.key = "EXTREME GREED";
        out.tone = Tone::Positive;
    }
    return out;
}

} // namespace fincept::screens::gauges
