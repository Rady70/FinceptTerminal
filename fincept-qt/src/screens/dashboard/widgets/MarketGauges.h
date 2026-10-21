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
#include <QDateTime>
#include <QString>
#include <QTimeZone>

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

/// The VIX gauge scale: 0..kVixGaugeMax, with the regime boundaries above
/// (15, 20, 30, 40) as its colour bands, so the marker lands in the band whose
/// name the widget shows.
inline constexpr double kVixGaugeMax = 50.0;
inline constexpr double kVixBounds[] = {15.0, 20.0, 30.0, 40.0};

/// Marker position on the gauge, 0..1 (readings above the scale sit at 1).
inline double vix_gauge_fraction(double vix) {
    const double f = vix / kVixGaugeMax;
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

/// Exchange session state from the exchange's own clock (DST-correct through
/// QTimeZone). Regular weekday hours only: exchange holidays are not known.
enum class SessionState { Open, Pre, Break, After, Closed, Unknown };

struct ExchangeSession {
    const char* tz;
    int pre_from; ///< minutes after local midnight, -1 = none
    int open;
    int break_from; ///< -1 = no midday break
    int break_to;
    int close;
    int after_to; ///< -1 = no after-hours session shown
};

/// US: NYSE/Nasdaq 09:30-16:00 ET (pre 04:00, after to 20:00); UK: LSE
/// 08:00-16:30; JP: TSE 09:00-11:30 / 12:30-15:30; CN: SSE 09:30-11:30 /
/// 13:00-15:00 (call auction from 09:15); IN: NSE 09:15-15:30 (pre-open 09:00).
inline std::optional<ExchangeSession> exchange_session(const QString& region) {
    if (region == QLatin1String("US"))
        return ExchangeSession{"America/New_York", 4 * 60, 9 * 60 + 30, -1, -1, 16 * 60, 20 * 60};
    if (region == QLatin1String("UK"))
        return ExchangeSession{"Europe/London", -1, 8 * 60, -1, -1, 16 * 60 + 30, -1};
    if (region == QLatin1String("JP"))
        return ExchangeSession{"Asia/Tokyo", -1, 9 * 60, 11 * 60 + 30, 12 * 60 + 30, 15 * 60 + 30, -1};
    if (region == QLatin1String("CN"))
        return ExchangeSession{"Asia/Shanghai", 9 * 60 + 15, 9 * 60 + 30, 11 * 60 + 30, 13 * 60, 15 * 60, -1};
    if (region == QLatin1String("IN"))
        return ExchangeSession{"Asia/Kolkata", 9 * 60, 9 * 60 + 15, -1, -1, 15 * 60 + 30, -1};
    return std::nullopt;
}

inline SessionState exchange_status(const QString& region, const QDateTime& utc) {
    const auto s = exchange_session(region);
    if (!s)
        return SessionState::Unknown;
    const QTimeZone tz{QByteArray(s->tz)};
    if (!tz.isValid())
        return SessionState::Unknown;
    const QDateTime local = utc.toTimeZone(tz);
    if (local.date().dayOfWeek() >= 6)
        return SessionState::Closed;
    const int m = local.time().hour() * 60 + local.time().minute();
    if (m >= s->open && m < s->close) {
        if (s->break_from >= 0 && m >= s->break_from && m < s->break_to)
            return SessionState::Break;
        return SessionState::Open;
    }
    if (s->pre_from >= 0 && m >= s->pre_from && m < s->open)
        return SessionState::Pre;
    if (s->after_to >= 0 && m >= s->close && m < s->after_to)
        return SessionState::After;
    return SessionState::Closed;
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
