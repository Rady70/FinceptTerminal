// src/screens/economics/panels/CftcWorkspaceData.h
//
// CFTC-specific data model and analytics for the R3 specialist workspace.
// Header-only over Qt Core so every participant mapping, window statistic and
// availability rule is unit-testable without the widget tree (see the HARD
// RULE at the top of tests/CMakeLists.txt).
//
// Truthfulness rules encoded here:
//   * a participant leg the provider did not return stays absent; a net with a
//     missing leg is unavailable, never a fabricated difference;
//   * a report family's participant classes come from that family's own CFTC
//     resource — legacy, disaggregated and TFF never borrow each other's names;
//   * statistics use explicitly defined windows and formulas, and a degenerate
//     window (no variance, too few observations) yields "unavailable" instead
//     of a division by zero or a misleading number;
//   * a weekly change is only weekly when the previous report is an actual
//     weekly neighbour; a longer gap stays unavailable;
//   * range windows are anchored at the latest report actually returned and
//     never synthesise observations the source did not send.
#pragma once

#include "screens/economics/panels/CftcNetFormat.h"
#include "ui/charts/TimeSeriesData.h"

#include <QCoreApplication>
#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>

namespace fincept::screens {

// ── Report families and participant classes ─────────────────────────────────

enum class CftcFamily { Legacy, Disaggregated, Tff };

inline QString cftc_family_code(CftcFamily family) {
    switch (family) {
        case CftcFamily::Disaggregated:
            return QStringLiteral("disaggregated");
        case CftcFamily::Tff:
            return QStringLiteral("tff");
        case CftcFamily::Legacy:
            break;
    }
    return QStringLiteral("legacy");
}

inline CftcFamily cftc_family_from_code(const QString& code) {
    const QString lower = code.trimmed().toLower();
    if (lower == QLatin1String("disaggregated") || lower == QLatin1String("disagg"))
        return CftcFamily::Disaggregated;
    if (lower == QLatin1String("tff") || lower == QLatin1String("financial"))
        return CftcFamily::Tff;
    return CftcFamily::Legacy;
}

struct CftcParticipant {
    QString key;              // stable analytical id; matches scripts/cftc_data.py
    QString label;            // translated display label
    bool speculative = false; // the family's primary speculative class
};

/// The participant classes the CFTC actually reports for a family. Legacy has
/// commercial / non-commercial / non-reportable; disaggregated has
/// producer-merchant, swap dealers, managed money, other reportables and
/// non-reportable; TFF has dealer/intermediary, asset manager/institutional,
/// leveraged funds, other reportables and non-reportable.
inline QVector<CftcParticipant> cftc_family_participants(CftcFamily family) {
    auto label = [](const char* text) { return QCoreApplication::translate("CftcPanel", text); };
    switch (family) {
        case CftcFamily::Disaggregated:
            return {
                {QStringLiteral("producer_merchant"), label("Producer/Merchant/Processor/User"), false},
                {QStringLiteral("swap_dealer"), label("Swap Dealers"), false},
                {QStringLiteral("managed_money"), label("Managed Money"), true},
                {QStringLiteral("other_reportable"), label("Other Reportables"), false},
                {QStringLiteral("non_reportable"), label("Non-Reportable"), false},
            };
        case CftcFamily::Tff:
            return {
                {QStringLiteral("dealer"), label("Dealer/Intermediary"), false},
                {QStringLiteral("asset_manager"), label("Asset Manager/Institutional"), false},
                {QStringLiteral("leveraged_funds"), label("Leveraged Funds"), true},
                {QStringLiteral("other_reportable"), label("Other Reportables"), false},
                {QStringLiteral("non_reportable"), label("Non-Reportable"), false},
            };
        case CftcFamily::Legacy:
            break;
    }
    return {
        {QStringLiteral("commercial"), label("Commercial"), false},
        {QStringLiteral("non_commercial"), label("Non-Commercial (Speculators)"), true},
        {QStringLiteral("non_reportable"), label("Non-Reportable"), false},
    };
}

/// Index of the family's primary speculative class in the participant vector,
/// used as the default headline participant (legacy: non-commercial;
/// disaggregated: managed money; TFF: leveraged funds). -1 when absent.
inline int cftc_speculative_index(const QVector<CftcParticipant>& participants) {
    for (int i = 0; i < participants.size(); ++i) {
        if (participants[i].speculative)
            return i;
    }
    return -1;
}

// ── Observations ────────────────────────────────────────────────────────────

struct CftcObservation {
    QDate date;         // parsed report date; invalid when the row is unusable
    QString date_label; // provider's exact report-date text
    QString market;     // market_and_exchange_names
    QString contract_code;
    QString units; // contract_units
    std::optional<double> open_interest;
    QVector<std::optional<double>> longs;  // one per family participant
    QVector<std::optional<double>> shorts; // one per family participant
};

/// Parse a JSON numeric cell, or a numeric string (Socrata returns some
/// position columns as strings). Anything else — null, absent, junk text — is
/// absent, never zero.
inline std::optional<double> cftc_number(const QJsonObject& row, const QString& key) {
    const QJsonValue value = row.value(key);
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().trimmed().toDouble(&ok);
        if (ok)
            return parsed;
    }
    return std::nullopt;
}

struct CftcHistory {
    QVector<CftcObservation> observations; // ascending by report date
    QString error;                         // non-empty when unusable
};

/// Build the typed history from the provider's rows. Fails closed: a row with
/// an unparseable date or a repeated report date makes the whole payload
/// unusable rather than being silently dropped (which would hide history).
inline CftcHistory cftc_parse_history(const QJsonArray& rows, CftcFamily family) {
    CftcHistory out;
    const auto participants = cftc_family_participants(family);
    if (rows.isEmpty()) {
        out.error = QCoreApplication::translate("CftcPanel", "The provider returned no observations.");
        return out;
    }
    out.observations.reserve(rows.size());
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        CftcObservation obs;
        obs.date_label = row.value(QStringLiteral("report_date_as_yyyy_mm_dd")).toString().trimmed();
        obs.date = ui::parse_time_series_date(obs.date_label);
        if (!obs.date.isValid()) {
            out.error = QCoreApplication::translate("CftcPanel", "A report row has no usable report date.");
            out.observations.clear();
            return out;
        }
        obs.market = row.value(QStringLiteral("market_and_exchange_names")).toString();
        obs.contract_code = row.value(QStringLiteral("cftc_contract_market_code")).toString();
        obs.units = row.value(QStringLiteral("contract_units")).toString();
        obs.open_interest = cftc_number(row, QStringLiteral("open_interest_all"));
        obs.longs.reserve(participants.size());
        obs.shorts.reserve(participants.size());
        for (const auto& participant : participants) {
            obs.longs.append(cftc_number(row, participant.key + QStringLiteral("_long")));
            obs.shorts.append(cftc_number(row, participant.key + QStringLiteral("_short")));
        }
        out.observations.append(obs);
    }
    std::stable_sort(out.observations.begin(), out.observations.end(),
                     [](const CftcObservation& a, const CftcObservation& b) { return a.date < b.date; });
    for (int i = 1; i < out.observations.size(); ++i) {
        if (out.observations[i].date == out.observations[i - 1].date) {
            out.error =
                QCoreApplication::translate("CftcPanel", "The provider returned two rows for the same report date.");
            out.observations.clear();
            return out;
        }
    }
    return out;
}

/// long − short for one participant, or nothing when either leg is absent.
inline std::optional<double> cftc_participant_net(const CftcObservation& obs, int participant_index) {
    if (participant_index < 0 || participant_index >= obs.longs.size() || participant_index >= obs.shorts.size())
        return std::nullopt;
    if (!obs.longs[participant_index].has_value() || !obs.shorts[participant_index].has_value())
        return std::nullopt;
    return *obs.longs[participant_index] - *obs.shorts[participant_index];
}

// ── Analytical history range ────────────────────────────────────────────────

enum class CftcRange { OneYear, TwoYears, FiveYears, TenYears, TwentyYears, Max };

inline int cftc_range_years(CftcRange range) {
    switch (range) {
        case CftcRange::OneYear:
            return 1;
        case CftcRange::TwoYears:
            return 2;
        case CftcRange::FiveYears:
            return 5;
        case CftcRange::TenYears:
            return 10;
        case CftcRange::TwentyYears:
            return 20;
        case CftcRange::Max:
            break;
    }
    return 0;
}

inline QString cftc_range_label(CftcRange range) {
    if (range == CftcRange::Max)
        return QStringLiteral("MAX");
    return QStringLiteral("%1Y").arg(cftc_range_years(range));
}

/// Start of the window, anchored at the latest report actually returned.
inline QDate cftc_range_start(const QVector<CftcObservation>& observations, CftcRange range) {
    if (observations.isEmpty() || range == CftcRange::Max)
        return {};
    return observations.last().date.addYears(-cftc_range_years(range));
}

/// A window is offered only when the returned history actually spans it.
inline bool cftc_range_available(const QVector<CftcObservation>& observations, CftcRange range) {
    if (range == CftcRange::Max)
        return !observations.isEmpty();
    if (observations.isEmpty())
        return false;
    return observations.first().date <= cftc_range_start(observations, range);
}

inline QVector<CftcObservation> cftc_filter_range(const QVector<CftcObservation>& observations, CftcRange range) {
    if (range == CftcRange::Max || observations.isEmpty())
        return observations;
    const QDate start = cftc_range_start(observations, range);
    QVector<CftcObservation> out;
    out.reserve(observations.size());
    for (const auto& obs : observations) {
        if (obs.date >= start)
            out.append(obs);
    }
    return out;
}

// ── Dated series ────────────────────────────────────────────────────────────

struct CftcDatedValue {
    QDate date;
    QString date_label;
    double value = 0.0;
};

inline QVector<CftcDatedValue> cftc_open_interest_series(const QVector<CftcObservation>& observations) {
    QVector<CftcDatedValue> out;
    out.reserve(observations.size());
    for (const auto& obs : observations) {
        if (obs.open_interest)
            out.append({obs.date, obs.date_label, *obs.open_interest});
    }
    return out;
}

/// Convert a dated series to the shared chart point type so the specialist
/// chart can reuse the tested gap-splitting/format rules.
inline QVector<ui::TimeSeriesPoint> cftc_to_time_series(const QVector<CftcDatedValue>& series) {
    QVector<ui::TimeSeriesPoint> out;
    out.reserve(series.size());
    for (const auto& point : series)
        out.append({point.date, point.date_label, point.value});
    return out;
}

// ── Availability-aware change comparisons ───────────────────────────────────

/// One normal weekly CFTC interval, plus slack for a holiday-shifted release.
/// A longer gap is not a weekly change and must stay unavailable.
inline constexpr int kCftcWeeklyGapDays = 10;

struct CftcChange {
    bool has_pair = false;  // at least two observations exist
    bool has_value = false; // the pair is an actual weekly neighbour
    bool stale = false;     // the series ends before the requested as-of report
    int gap_days = 0;       // calendar days between the two observations
    double value = 0.0;     // latest − previous
    QDate previous_date;
};

/// Change between the latest observation and the immediately preceding one,
/// anchored at `as_of` (the actual latest official report date) when given.
/// A series that ends before `as_of` is stale for a current comparison and
/// reports `stale` instead of presenting an older pair as this week's change.
inline CftcChange cftc_weekly_change(const QVector<CftcDatedValue>& series, const QDate& as_of = {}) {
    CftcChange out;
    if (series.size() < 2)
        return out;
    const CftcDatedValue& latest = series.last();
    if (as_of.isValid() && latest.date != as_of) {
        out.stale = true;
        return out;
    }
    out.has_pair = true;
    const CftcDatedValue& previous = series.at(series.size() - 2);
    out.gap_days = static_cast<int>(previous.date.daysTo(latest.date));
    out.previous_date = previous.date;
    if (out.gap_days <= kCftcWeeklyGapDays) {
        out.has_value = true;
        out.value = latest.value - previous.value;
    }
    return out;
}

/// Change against the most recent observation that is at least `days` old
/// (e.g. 28 days for a 4-week change). Absent when the series does not reach
/// back that far, when the anchor is so much older than the target that the
/// span would no longer truthfully be a 4-week/13-week comparison (the anchor
/// must fall within `[latest - (days + kCftcWeeklyGapDays), latest - days]`),
/// or when `as_of` is given and the series does not reach that report.
inline std::optional<double> cftc_change_since_days(const QVector<CftcDatedValue>& series, int days,
                                                    const QDate& as_of = {}) {
    if (series.size() < 2)
        return std::nullopt;
    const QDate latest = series.last().date;
    if (as_of.isValid() && latest != as_of)
        return std::nullopt;
    const QDate target = latest.addDays(-days);
    const QDate floor = latest.addDays(-(days + kCftcWeeklyGapDays));
    const CftcDatedValue* anchor = nullptr;
    for (const auto& point : series) {
        if (point.date <= target)
            anchor = &point;
        else
            break;
    }
    if (!anchor || anchor->date < floor || anchor->date == latest)
        return std::nullopt;
    return series.last().value - anchor->value;
}

// ── Window statistics ───────────────────────────────────────────────────────

/// Statistics over one explicitly defined window. Every measure carries its own
/// presence flag; a formula that would divide by zero (or by a window with no
/// variance) is unavailable, not a number.
///
/// Formulas (for a window of n observations, latest = the value at the actual
/// latest official report when `as_of` names it):
///   LATEST        = value at the latest official report          [series reaches as_of]
///   MIN / MAX/AVG = extremes/mean of every valid window value    [historical, always kept]
///   COT INDEX     = 100 × (latest − MIN) / (MAX − MIN)   [n ≥ 2, MAX > MIN, latest present]
///   PERCENTILE    = 100 × count(value ≤ latest) / n      [n ≥ 2, MAX > MIN, latest present]
///   Z-SCORE       = (latest − AVG) / SD, SD = sample standard deviation (n−1)
///                                                        [n ≥ 2, SD > 0, latest present]
///   FROM HIGH/LOW = latest − MAX / latest − MIN          [latest present]
///   4W / 13W      = latest − value at least 28 / 91 days older [latest present]
///
/// When `as_of` is valid and the series ends before it — e.g. the latest CFTC
/// report did not carry this participant's legs — the current-dependent
/// measures are unavailable rather than silently showing an older reading as
/// current. MIN/MAX/AVG keep describing the historical window.
struct CftcWindowStats {
    int count = 0;                 // observations with a real value in the window
    bool at_latest_report = false; // the series reaches the requested as-of report
    bool has_latest = false;
    double latest = 0.0;
    bool has_min = false;
    double min_value = 0.0;
    bool has_max = false;
    double max_value = 0.0;
    bool has_avg = false;
    double avg = 0.0;
    bool has_cot_index = false;
    double cot_index = 0.0;
    bool has_percentile = false;
    double percentile = 0.0;
    bool has_zscore = false;
    double zscore = 0.0;
    bool has_distance_high = false;
    double distance_high = 0.0;
    bool has_distance_low = false;
    double distance_low = 0.0;
    bool has_change_4w = false;
    double change_4w = 0.0;
    bool has_change_13w = false;
    double change_13w = 0.0;
    bool zero_variance = false; // n ≥ 2 and every value is identical
};

inline CftcWindowStats cftc_window_stats(const QVector<CftcDatedValue>& window, const QDate& as_of = {}) {
    CftcWindowStats stats;
    stats.count = window.size();
    if (window.isEmpty())
        return stats;

    stats.at_latest_report = !as_of.isValid() || window.last().date == as_of;
    if (stats.at_latest_report) {
        stats.has_latest = true;
        stats.latest = window.last().value;
    }

    double min_value = window.first().value;
    double max_value = window.first().value;
    double sum = 0.0;
    for (const auto& point : window) {
        min_value = std::min(min_value, point.value);
        max_value = std::max(max_value, point.value);
        sum += point.value;
    }
    stats.has_min = stats.has_max = true;
    stats.min_value = min_value;
    stats.max_value = max_value;
    stats.has_avg = true;
    stats.avg = sum / static_cast<double>(window.size());

    const double range = max_value - min_value;
    stats.zero_variance = window.size() >= 2 && range == 0.0;
    if (stats.has_latest && window.size() >= 2 && range > 0.0) {
        stats.has_cot_index = true;
        stats.cot_index = (stats.latest - min_value) / range * 100.0;
        int at_or_below = 0;
        for (const auto& point : window) {
            if (point.value <= stats.latest)
                ++at_or_below;
        }
        stats.has_percentile = true;
        stats.percentile = 100.0 * static_cast<double>(at_or_below) / static_cast<double>(window.size());
    }
    if (stats.has_latest && window.size() >= 2) {
        double sum_sq = 0.0;
        for (const auto& point : window) {
            const double deviation = point.value - stats.avg;
            sum_sq += deviation * deviation;
        }
        const double sd = std::sqrt(sum_sq / static_cast<double>(window.size() - 1));
        if (sd > 0.0) {
            stats.has_zscore = true;
            stats.zscore = (stats.latest - stats.avg) / sd;
        }
    }

    if (stats.has_latest) {
        stats.has_distance_high = true;
        stats.distance_high = stats.latest - max_value;
        stats.has_distance_low = true;
        stats.distance_low = stats.latest - min_value;

        const auto change_4w = cftc_change_since_days(window, 28, as_of);
        if (change_4w) {
            stats.has_change_4w = true;
            stats.change_4w = *change_4w;
        }
        const auto change_13w = cftc_change_since_days(window, 91, as_of);
        if (change_13w) {
            stats.has_change_13w = true;
            stats.change_13w = *change_13w;
        }
    }
    return stats;
}

// ── Heatmap series ──────────────────────────────────────────────────────────

/// Minimum observations before a trailing statistic is shown on the heatmap.
/// With fewer than this, a COT index / percentile on 2–7 points is noise.
inline constexpr int kCftcHeatmapMinObservations = 8;

struct CftcHeatmapPoint {
    QDate date;
    QString date_label;
    double net = 0.0;
    bool has_cot_index = false;
    double cot_index = 0.0;
    bool has_percentile = false;
    double percentile = 0.0;
    bool has_zscore = false;
    double zscore = 0.0;
    bool has_change = false;
    double change = 0.0; // week-over-week change, weekly-neighbour rule applied
};

/// The last `columns` official CFTC report dates of a window, independent of
/// any participant's missing observations. The heatmap's horizontal axis is
/// built from these, so a report remains a column even when one participant
/// class has no value on it (that row's cell stays blank).
inline QVector<QDate> cftc_report_dates(const QVector<CftcObservation>& window, int columns) {
    QVector<QDate> out;
    if (window.isEmpty() || columns < 1)
        return out;
    const int first = std::max(0, static_cast<int>(window.size()) - columns);
    out.reserve(window.size() - first);
    for (int i = first; i < window.size(); ++i)
        out.append(window[i].date);
    return out;
}

/// The last `columns` observations of a window, each with trailing statistics
/// computed from the window observations up to and including that report (no
/// lookahead; the earliest columns simply have less history).
inline QVector<CftcHeatmapPoint> cftc_heatmap_series(const QVector<CftcDatedValue>& window, int columns) {
    QVector<CftcHeatmapPoint> out;
    if (window.isEmpty() || columns < 1)
        return out;
    const int first = std::max(0, static_cast<int>(window.size()) - columns);
    for (int i = first; i < window.size(); ++i) {
        CftcHeatmapPoint point;
        point.date = window[i].date;
        point.date_label = window[i].date_label;
        point.net = window[i].value;

        if (i > 0) {
            const int gap = static_cast<int>(window[i - 1].date.daysTo(window[i].date));
            if (gap <= kCftcWeeklyGapDays) {
                point.has_change = true;
                point.change = window[i].value - window[i - 1].value;
            }
        }
        const QVector<CftcDatedValue> prefix = window.mid(0, i + 1);
        if (prefix.size() >= kCftcHeatmapMinObservations) {
            const CftcWindowStats stats = cftc_window_stats(prefix);
            point.has_cot_index = stats.has_cot_index;
            point.cot_index = stats.cot_index;
            point.has_percentile = stats.has_percentile;
            point.percentile = stats.percentile;
            point.has_zscore = stats.has_zscore;
            point.zscore = stats.zscore;
        }
        out.append(point);
    }
    return out;
}

// ── Price alignment ─────────────────────────────────────────────────────────

struct CftcPricePoint {
    QDate date;
    double close = 0.0;
};

/// Close of the most recent session on or before `date` (input ascending).
inline std::optional<double> cftc_price_on_or_before(const QVector<CftcPricePoint>& prices, const QDate& date) {
    const CftcPricePoint* found = nullptr;
    for (const auto& point : prices) {
        if (point.date <= date)
            found = &point;
        else
            break;
    }
    if (!found)
        return std::nullopt;
    return found->close;
}

/// Price change between the latest close and the close on or before
/// (latest date − days). Absent when the history does not reach back that far
/// or when the anchor is far enough before the target that the span no longer
/// matches the comparison it is paired with (same bound as the positioning
/// change).
inline std::optional<double> cftc_price_change_since_days(const QVector<CftcPricePoint>& prices, int days) {
    if (prices.size() < 2)
        return std::nullopt;
    const QDate latest = prices.last().date;
    const QDate floor = latest.addDays(-(days + kCftcWeeklyGapDays));
    const CftcPricePoint* anchor = nullptr;
    for (const auto& point : prices) {
        if (point.date <= latest.addDays(-days))
            anchor = &point;
        else
            break;
    }
    if (!anchor || anchor->date < floor)
        return std::nullopt;
    return prices.last().close - anchor->close;
}

enum class CftcDirection { Unavailable, Down, Flat, Up };

inline CftcDirection cftc_direction(std::optional<double> change) {
    if (!change)
        return CftcDirection::Unavailable;
    if (*change > 0.0)
        return CftcDirection::Up;
    if (*change < 0.0)
        return CftcDirection::Down;
    return CftcDirection::Flat;
}

/// Whether two directional measurements agree. Flat agrees only with flat, so
/// a flat price against a rising position is a divergence, not an alignment.
inline bool cftc_directions_aligned(CftcDirection a, CftcDirection b) {
    return a != CftcDirection::Unavailable && a == b;
}

/// Extreme reads of a positioning window keyed by the report dates that
/// produced them, so price can be inspected at the actual extreme report dates.
inline bool cftc_net_extreme_dates(const QVector<CftcDatedValue>& window, QDate& high_date, QDate& low_date) {
    if (window.isEmpty())
        return false;
    const CftcDatedValue* high = &window.first();
    const CftcDatedValue* low = &window.first();
    for (const auto& point : window) {
        if (point.value > high->value)
            high = &point;
        if (point.value < low->value)
            low = &point;
    }
    high_date = high->date;
    low_date = low->date;
    return true;
}

} // namespace fincept::screens
