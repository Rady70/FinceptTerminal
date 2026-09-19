// src/services/economics/CftcMetricModel.h
//
// Pure, deterministic COT (Commitments of Traders) analytical metric model.
//
// Extracted from the R3 CFTC workspace (previously
// screens/economics/panels/CftcWorkspaceData.h) so the analytical layer is
// reusable outside the widget tree: it is header-only over Qt Core, makes no
// network call, holds no broker/execution authority and can replay historical
// observations. The R3 workspace and the future research-state engine are its
// consumers.
//
// Statistics come in two explicitly separate flavours:
//
//   * Descriptive window statistics (CftcWindowStats) summarise a selected
//     window for the workspace cards and include the latest observation in the
//     distribution. This preserves the existing R3 UI semantics: COT Index and
//     percentile stay bounded 0–100 over that window.
//
//   * Signal-reference trailing statistics (CftcTrailingStats) are for
//     research-engine calculations that judge whether the current observation
//     is historically unusual. Their reference distribution is strictly
//     trailing: the current observation is NOT part of the window used to
//     judge it, so a 2Y COT Index compares report t against the eligible
//     history ending before t. A current value outside the prior range
//     therefore yields a COT Index outside 0–100 by design.
//
// Truthfulness rules encoded here:
//   * a participant leg the provider did not return stays absent; a net with a
//     missing leg is unavailable, never a fabricated difference;
//   * a report family's participant classes come from that family's own CFTC
//     resource — legacy, disaggregated and TFF never borrow each other's names;
//   * normalized position metrics (Long/Short/Net % of open interest) are
//     unavailable when open interest is missing or not positive, never zero;
//   * statistics use explicitly defined windows and formulas, and a degenerate
//     window (no variance, too few observations) yields "unavailable" instead
//     of a division by zero or a misleading number;
//   * a weekly change is only weekly when the previous report is an actual
//     weekly neighbour; a longer gap stays unavailable;
//   * 4W/13W/26W anchors are selected by actual report dates under an explicit
//     tolerance, so a gapped or stale anchor is unavailable, never relabelled;
//   * range windows are anchored at the latest report actually returned and
//     never synthesise observations the source did not send;
//   * every current-reading primitive takes the actual latest official report
//     as `as_of`; when it is omitted the series' newest observation is treated
//     as current, so a caller holding an official report date should pass it;
//   * price observations are a separate source, are never fetched here, and
//     remain distinct from CFTC observations.
#pragma once

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

namespace fincept::services {

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

    // Provider trader-count / concentration fields retained by cot_history
    // where the authoritative CFTC resource carries them. These are provider
    // observations, not derived values; a cell the report did not carry stays
    // absent. Counts are whole traders; concentration values are percentages
    // of open interest exactly as published by CFTC.
    std::optional<double> traders_total;
    std::optional<double> traders_reportable_long;
    std::optional<double> traders_reportable_short;
    std::optional<double> concentration_gross_4_long;
    std::optional<double> concentration_gross_4_short;
    std::optional<double> concentration_gross_8_long;
    std::optional<double> concentration_gross_8_short;
    std::optional<double> concentration_net_4_long;
    std::optional<double> concentration_net_4_short;
    std::optional<double> concentration_net_8_long;
    std::optional<double> concentration_net_8_short;
};

/// Parse the CFTC report-date text into a QDate. The provider sends
/// `YYYY-MM-DD` and Socrata sometimes appends `T00:00:00.000`; a trailing time
/// part is ignored. `YYYY-MM` and `YYYY` are accepted the same way the shared
/// chart date contract accepts them, so the core stays dependency-free of the
/// UI layer without changing the accepted input. Anything else is invalid.
inline QDate cftc_parse_report_date(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    int cut = trimmed.indexOf(QLatin1Char('T'));
    const int space = trimmed.indexOf(QLatin1Char(' '));
    if (space >= 0 && (cut < 0 || space < cut))
        cut = space;
    const QString date_part = cut > 0 ? trimmed.left(cut) : trimmed;
    const QStringList parts = date_part.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (parts.size() == 3)
        return QDate(parts[0].toInt(), parts[1].toInt(), parts[2].toInt());
    if (parts.size() == 2)
        return QDate(parts[0].toInt(), parts[1].toInt(), 1);
    if (parts.size() == 1) {
        bool ok = false;
        const int year = parts[0].toInt(&ok);
        if (ok && year > 0)
            return QDate(year, 1, 1);
    }
    return {};
}

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
        obs.date = cftc_parse_report_date(obs.date_label);
        if (!obs.date.isValid()) {
            out.error = QCoreApplication::translate("CftcPanel", "A report row has no usable report date.");
            out.observations.clear();
            return out;
        }
        obs.market = row.value(QStringLiteral("market_and_exchange_names")).toString();
        obs.contract_code = row.value(QStringLiteral("cftc_contract_market_code")).toString();
        obs.units = row.value(QStringLiteral("contract_units")).toString();
        obs.open_interest = cftc_number(row, QStringLiteral("open_interest_all"));
        obs.traders_total = cftc_number(row, QStringLiteral("traders_total"));
        obs.traders_reportable_long = cftc_number(row, QStringLiteral("traders_reportable_long"));
        obs.traders_reportable_short = cftc_number(row, QStringLiteral("traders_reportable_short"));
        obs.concentration_gross_4_long = cftc_number(row, QStringLiteral("concentration_gross_4_long"));
        obs.concentration_gross_4_short = cftc_number(row, QStringLiteral("concentration_gross_4_short"));
        obs.concentration_gross_8_long = cftc_number(row, QStringLiteral("concentration_gross_8_long"));
        obs.concentration_gross_8_short = cftc_number(row, QStringLiteral("concentration_gross_8_short"));
        obs.concentration_net_4_long = cftc_number(row, QStringLiteral("concentration_net_4_long"));
        obs.concentration_net_4_short = cftc_number(row, QStringLiteral("concentration_net_4_short"));
        obs.concentration_net_8_long = cftc_number(row, QStringLiteral("concentration_net_8_long"));
        obs.concentration_net_8_short = cftc_number(row, QStringLiteral("concentration_net_8_short"));
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

// ── Position metrics ────────────────────────────────────────────────────────

/// Position legs plus open-interest normalization for one participant at one
/// report. `% of open interest` values are percentages (× 100), matching the
/// plan's `Long % OI` naming. Normalization requires a present, positive open
/// interest: a missing or zero open interest leaves the percentages absent
/// rather than fabricating a value, while raw legs and net stay available.
struct CftcPositionMetrics {
    bool has_long = false;
    double long_leg = 0.0;
    bool has_short = false;
    double short_leg = 0.0;
    bool has_net = false;
    double net_position = 0.0;
    bool has_open_interest = false;
    double open_interest = 0.0;
    bool normalizable = false; // open interest present and > 0
    bool has_long_pct_oi = false;
    double long_pct_oi = 0.0;
    bool has_short_pct_oi = false;
    double short_pct_oi = 0.0;
    bool has_net_pct_oi = false;
    double net_pct_oi = 0.0;
};

inline CftcPositionMetrics cftc_position_metrics(const CftcObservation& obs, int participant_index) {
    CftcPositionMetrics out;
    if (participant_index < 0 || participant_index >= obs.longs.size() || participant_index >= obs.shorts.size())
        return out;
    out.has_long = obs.longs[participant_index].has_value();
    if (out.has_long)
        out.long_leg = *obs.longs[participant_index];
    out.has_short = obs.shorts[participant_index].has_value();
    if (out.has_short)
        out.short_leg = *obs.shorts[participant_index];
    const std::optional<double> net = cftc_participant_net(obs, participant_index);
    out.has_net = net.has_value();
    if (net)
        out.net_position = *net;
    out.has_open_interest = obs.open_interest.has_value();
    if (obs.open_interest)
        out.open_interest = *obs.open_interest;
    out.normalizable = out.has_open_interest && out.open_interest > 0.0;
    if (!out.normalizable)
        return out;
    if (out.has_long) {
        out.has_long_pct_oi = true;
        out.long_pct_oi = out.long_leg / out.open_interest * 100.0;
    }
    if (out.has_short) {
        out.has_short_pct_oi = true;
        out.short_pct_oi = out.short_leg / out.open_interest * 100.0;
    }
    if (out.has_net) {
        out.has_net_pct_oi = true;
        out.net_pct_oi = out.net_position / out.open_interest * 100.0;
    }
    return out;
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

// ── Metric series ───────────────────────────────────────────────────────────

/// The participant metric kinds the analytical core can materialise as dated
/// series. Open interest itself is materialised by cftc_open_interest_series.
enum class CftcMetricKind { Long, Short, Net, LongPctOi, ShortPctOi, NetPctOi };

inline std::optional<double> cftc_metric_value(const CftcObservation& obs, int participant_index, CftcMetricKind kind) {
    if (kind == CftcMetricKind::Long) {
        if (participant_index < 0 || participant_index >= obs.longs.size())
            return std::nullopt;
        return obs.longs[participant_index];
    }
    if (kind == CftcMetricKind::Short) {
        if (participant_index < 0 || participant_index >= obs.shorts.size())
            return std::nullopt;
        return obs.shorts[participant_index];
    }
    const CftcPositionMetrics metrics = cftc_position_metrics(obs, participant_index);
    switch (kind) {
        case CftcMetricKind::Net:
            return metrics.has_net ? std::optional<double>(metrics.net_position) : std::nullopt;
        case CftcMetricKind::LongPctOi:
            return metrics.has_long_pct_oi ? std::optional<double>(metrics.long_pct_oi) : std::nullopt;
        case CftcMetricKind::ShortPctOi:
            return metrics.has_short_pct_oi ? std::optional<double>(metrics.short_pct_oi) : std::nullopt;
        case CftcMetricKind::NetPctOi:
            return metrics.has_net_pct_oi ? std::optional<double>(metrics.net_pct_oi) : std::nullopt;
        case CftcMetricKind::Long:
        case CftcMetricKind::Short:
            break;
    }
    return std::nullopt;
}

/// One dated point per report that actually carries the requested metric; a
/// report missing the metric contributes no point (never a zero).
inline QVector<CftcDatedValue> cftc_metric_series(const QVector<CftcObservation>& observations, int participant_index,
                                                  CftcMetricKind kind) {
    QVector<CftcDatedValue> out;
    out.reserve(observations.size());
    for (const auto& obs : observations) {
        const std::optional<double> value = cftc_metric_value(obs, participant_index, kind);
        if (value)
            out.append({obs.date, obs.date_label, *value});
    }
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

/// A dated comparison against a target that is `days` before the latest
/// observation. The anchor is the most recent observation at or before the
/// target; it must also fall within `[latest - (days + kCftcWeeklyGapDays),
/// latest - days]`, so a gapped series can never present a much longer span as
/// a 4-week/13-week/26-week comparison. `has_anchor` records that an anchor
/// observation exists at all; `has_value` records that it passed the tolerance.
struct CftcHorizonChange {
    bool has_anchor = false;
    bool has_value = false;
    bool stale = false;    // the series ends before the requested as-of report
    bool adjacent = false; // the anchor is the immediately preceding report
    int days = 0;          // nominal span requested
    int gap_days = 0;      // actual span between anchor and latest report
    QDate anchor_date;
    double value = 0.0; // latest − anchor
};

inline CftcHorizonChange cftc_change_at_days(const QVector<CftcDatedValue>& series, int days, const QDate& as_of = {}) {
    CftcHorizonChange out;
    out.days = days;
    if (series.size() < 2)
        return out;
    const QDate latest = series.last().date;
    if (as_of.isValid() && latest != as_of) {
        out.stale = true;
        return out;
    }
    const QDate target = latest.addDays(-days);
    const QDate floor = latest.addDays(-(days + kCftcWeeklyGapDays));
    int anchor_index = -1;
    for (int i = 0; i < series.size(); ++i) {
        if (series[i].date <= target)
            anchor_index = i;
        else
            break;
    }
    if (anchor_index < 0)
        return out;
    out.has_anchor = true;
    out.anchor_date = series[anchor_index].date;
    out.adjacent = anchor_index == series.size() - 2;
    out.gap_days = static_cast<int>(series[anchor_index].date.daysTo(latest));
    if (series[anchor_index].date < floor || series[anchor_index].date == latest)
        return out;
    out.has_value = true;
    out.value = series.last().value - series[anchor_index].value;
    return out;
}

/// Change against the most recent observation that is at least `days` old.
/// Absent when the series does not reach back that far, when the anchor is so
/// much older than the target that the span would no longer truthfully be the
/// requested comparison, or when `as_of` is given and the series does not
/// reach that report.
inline std::optional<double> cftc_change_since_days(const QVector<CftcDatedValue>& series, int days,
                                                    const QDate& as_of = {}) {
    const CftcHorizonChange change = cftc_change_at_days(series, days, as_of);
    if (!change.has_value)
        return std::nullopt;
    return change.value;
}

/// The practical positioning horizons. OneReport is the adjacent-report change
/// (the existing weekly rule); the others are date-anchored comparisons.
enum class CftcHorizon { OneReport, FourWeeks, ThirteenWeeks, TwentySixWeeks };

inline int cftc_horizon_days(CftcHorizon horizon) {
    switch (horizon) {
        case CftcHorizon::OneReport:
            return 7;
        case CftcHorizon::FourWeeks:
            return 28;
        case CftcHorizon::ThirteenWeeks:
            return 91;
        case CftcHorizon::TwentySixWeeks:
            return 182;
    }
    return 0;
}

inline QString cftc_horizon_code(CftcHorizon horizon) {
    switch (horizon) {
        case CftcHorizon::OneReport:
            return QStringLiteral("1W");
        case CftcHorizon::FourWeeks:
            return QStringLiteral("4W");
        case CftcHorizon::ThirteenWeeks:
            return QStringLiteral("13W");
        case CftcHorizon::TwentySixWeeks:
            return QStringLiteral("26W");
    }
    return {};
}

inline CftcHorizonChange cftc_horizon_change(const QVector<CftcDatedValue>& series, CftcHorizon horizon,
                                             const QDate& as_of = {}) {
    if (horizon == CftcHorizon::OneReport) {
        const CftcChange weekly = cftc_weekly_change(series, as_of);
        CftcHorizonChange out;
        out.days = cftc_horizon_days(horizon);
        out.has_anchor = weekly.has_pair;
        out.has_value = weekly.has_value;
        out.stale = weekly.stale;
        out.adjacent = weekly.has_pair;
        out.gap_days = weekly.gap_days;
        out.anchor_date = weekly.previous_date;
        out.value = weekly.value;
        return out;
    }
    return cftc_change_at_days(series, cftc_horizon_days(horizon), as_of);
}

/// The official report-date axis of a history, as a value-free dated series.
/// Used only to select one shared anchor pair from the actual CFTC report
/// dates, independently of whether any participant carried a value.
inline QVector<CftcDatedValue> cftc_report_axis(const QVector<CftcObservation>& observations) {
    QVector<CftcDatedValue> out;
    out.reserve(observations.size());
    for (const auto& obs : observations)
        out.append({obs.date, obs.date_label, 0.0});
    return out;
}

/// The as-of report of an observation history: the explicit `as_of` when the
/// caller names one, otherwise the newest official report the history actually
/// carried. Wrappers that receive the full observations use this so a metric
/// series that ends before the latest official report is detected as stale
/// instead of silently treating an older report as current.
inline QDate cftc_official_as_of(const QVector<CftcObservation>& observations, const QDate& as_of) {
    if (as_of.isValid())
        return as_of;
    return observations.isEmpty() ? QDate() : observations.last().date;
}

/// One report-anchored horizon pair selected from the official report dates.
/// `latest_date` is the report the change ends on; `anchor_date` the report it
/// starts from. All metrics of a change set are evaluated against these same
/// two reports.
struct CftcHorizonAnchor {
    bool has_anchor = false;
    bool has_value = false;
    bool stale = false;
    bool adjacent = false;
    int days = 0;
    int gap_days = 0;
    QDate latest_date;
    QDate anchor_date;
};

inline CftcHorizonAnchor cftc_horizon_anchor(const QVector<CftcObservation>& observations, CftcHorizon horizon,
                                             const QDate& as_of = {}) {
    CftcHorizonAnchor out;
    out.days = cftc_horizon_days(horizon);
    if (observations.isEmpty())
        return out;
    out.latest_date = observations.last().date;
    const QDate effective = cftc_official_as_of(observations, as_of);
    const CftcHorizonChange change = cftc_horizon_change(cftc_report_axis(observations), horizon, effective);
    out.has_anchor = change.has_anchor;
    out.has_value = change.has_value;
    out.stale = change.stale;
    out.adjacent = change.adjacent;
    out.gap_days = change.gap_days;
    out.anchor_date = change.anchor_date;
    return out;
}

/// Change of a metric series between the shared anchor report and the current
/// official report. The pair comes from the official report axis, so every
/// metric in a change set spans the same dates; a metric missing at either
/// endpoint is unavailable instead of silently re-anchoring to another report.
inline CftcHorizonChange cftc_anchored_change(const QVector<CftcDatedValue>& series, const CftcHorizonAnchor& anchor) {
    CftcHorizonChange out;
    out.days = anchor.days;
    out.adjacent = anchor.adjacent;
    out.stale = anchor.stale;
    out.has_anchor = anchor.has_anchor;
    out.gap_days = anchor.gap_days;
    out.anchor_date = anchor.anchor_date;
    if (!anchor.has_value)
        return out;
    const CftcDatedValue* latest = nullptr;
    const CftcDatedValue* start = nullptr;
    for (const auto& point : series) {
        if (point.date == anchor.latest_date)
            latest = &point;
        if (point.date == anchor.anchor_date)
            start = &point;
    }
    if (!latest) {
        // The metric does not reach the current official report.
        out.stale = true;
        return out;
    }
    if (!start)
        return out; // the metric is absent at the shared anchor report
    out.has_value = true;
    out.value = latest->value - start->value;
    return out;
}

/// A participant's change set at one horizon. The gross legs stay separate so
/// the caller can distinguish new longs from short covering (and new shorts
/// from long liquidation) using only what the report establishes; this layer
/// attaches no aggregate interpretation. Every metric is measured between the
/// same two official reports, so an available ΔLong, ΔShort and ΔNet are
/// internally consistent (ΔLong − ΔShort == ΔNet).
struct CftcPositionChanges {
    CftcHorizon horizon = CftcHorizon::OneReport;
    CftcHorizonChange long_leg;
    CftcHorizonChange short_leg;
    CftcHorizonChange net;
    CftcHorizonChange net_pct_oi;
};

inline CftcPositionChanges cftc_position_changes(const QVector<CftcObservation>& observations, int participant_index,
                                                 CftcHorizon horizon, const QDate& as_of = {}) {
    CftcPositionChanges out;
    out.horizon = horizon;
    const CftcHorizonAnchor anchor = cftc_horizon_anchor(observations, horizon, as_of);
    out.long_leg =
        cftc_anchored_change(cftc_metric_series(observations, participant_index, CftcMetricKind::Long), anchor);
    out.short_leg =
        cftc_anchored_change(cftc_metric_series(observations, participant_index, CftcMetricKind::Short), anchor);
    out.net = cftc_anchored_change(cftc_metric_series(observations, participant_index, CftcMetricKind::Net), anchor);
    out.net_pct_oi =
        cftc_anchored_change(cftc_metric_series(observations, participant_index, CftcMetricKind::NetPctOi), anchor);
    return out;
}

inline CftcHorizonChange cftc_open_interest_change(const QVector<CftcObservation>& observations, CftcHorizon horizon,
                                                   const QDate& as_of = {}) {
    const CftcHorizonAnchor anchor = cftc_horizon_anchor(observations, horizon, as_of);
    return cftc_anchored_change(cftc_open_interest_series(observations), anchor);
}

// ── Directions ──────────────────────────────────────────────────────────────

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

/// Opposite directions are an explicit Up/Down pair; flat and unavailable are
/// neither aligned nor opposed.
inline bool cftc_directions_opposed(CftcDirection a, CftcDirection b) {
    return (a == CftcDirection::Up && b == CftcDirection::Down) || (a == CftcDirection::Down && b == CftcDirection::Up);
}

inline bool cftc_changes_same_direction(std::optional<double> a, std::optional<double> b) {
    if (!a || !b)
        return false;
    return cftc_directions_aligned(cftc_direction(a), cftc_direction(b));
}

inline bool cftc_changes_opposed(std::optional<double> a, std::optional<double> b) {
    if (!a || !b)
        return false;
    return cftc_directions_opposed(cftc_direction(a), cftc_direction(b));
}

// ── Positioning momentum ────────────────────────────────────────────────────

/// A causal trailing mean of Net over the last `observations` reports (the
/// latest report included; nothing later is ever used). `count` reports the
/// observations actually in the window and must equal the request for the
/// average to be available, so a short history is unavailable rather than an
/// average of fewer points presented as a 4W/13W mean. `gapped` marks an
/// internal report gap so callers can see the window spans more than the
/// nominal number of weeks.
struct CftcMovingAverage {
    int requested = 0;
    bool at_latest_report = false;
    int count = 0;
    bool gapped = false;
    QDate first_date;
    QDate last_date;
    bool has_value = false;
    double value = 0.0;
};

inline CftcMovingAverage cftc_moving_average(const QVector<CftcDatedValue>& series, int observations,
                                             const QDate& as_of = {}) {
    CftcMovingAverage out;
    out.requested = observations;
    if (series.isEmpty() || observations < 1)
        return out;
    out.at_latest_report = !as_of.isValid() || series.last().date == as_of;
    if (!out.at_latest_report)
        return out;
    const int first = std::max(0, static_cast<int>(series.size()) - observations);
    out.count = series.size() - first;
    out.first_date = series[first].date;
    out.last_date = series.last().date;
    double sum = 0.0;
    for (int i = first; i < series.size(); ++i) {
        sum += series[i].value;
        if (i > first && static_cast<int>(series[i - 1].date.daysTo(series[i].date)) > kCftcWeeklyGapDays)
            out.gapped = true;
    }
    if (out.count < observations)
        return out;
    out.has_value = true;
    out.value = sum / static_cast<double>(out.count);
    return out;
}

/// One scalar reading tied to the report date it describes, carrying the
/// underlying window's gap state so a derived momentum value can never look
/// like an ordinary scalar when its moving-average window spans a missing
/// report.
struct CftcMetricReading {
    bool has_value = false;
    bool gapped = false;
    double value = 0.0;
    QDate reference_date;
};

/// Latest Net minus its causal trailing moving average. Unavailable when the
/// average is unavailable; `gapped` mirrors the underlying window.
inline CftcMetricReading cftc_net_minus_moving_average(const QVector<CftcDatedValue>& series, int observations,
                                                       const QDate& as_of = {}) {
    CftcMetricReading out;
    const CftcMovingAverage average = cftc_moving_average(series, observations, as_of);
    if (!average.has_value)
        return out;
    out.value = series.last().value - average.value;
    out.reference_date = series.last().date;
    out.gapped = average.gapped;
    out.has_value = true;
    return out;
}

/// Moving-average slope: the current trailing mean minus the trailing mean of
/// the same length one report earlier (the last report is dropped from the
/// previous mean). Only causal data is used; fewer than `observations` + 1
/// reports makes the reading unavailable. `gapped` is set when either of the
/// two windows spans a missing report, so the slope is never mistaken for an
/// ordinary measurement.
inline CftcMetricReading cftc_moving_average_slope(const QVector<CftcDatedValue>& series, int observations,
                                                   const QDate& as_of = {}) {
    CftcMetricReading out;
    if (observations < 1 || series.size() < static_cast<int>(observations) + 1)
        return out;
    const CftcMovingAverage current = cftc_moving_average(series, observations, as_of);
    if (!current.has_value)
        return out;
    const CftcMovingAverage previous = cftc_moving_average(series.mid(0, series.size() - 1), observations);
    if (!previous.has_value)
        return out;
    out.value = current.value - previous.value;
    out.reference_date = current.last_date;
    out.gapped = current.gapped || previous.gapped;
    out.has_value = true;
    return out;
}

/// Direction and persistence of the most recent positioning steps. A run is
/// broken by a direction change or by a report gap beyond the weekly
/// tolerance; the latest pair must itself be a weekly neighbour or the whole
/// reading is unavailable. `changes` counts consecutive same-direction weekly
/// changes; `since_date` is the report date the run starts from.
struct CftcPositioningPersistence {
    bool has_value = false;
    CftcDirection direction = CftcDirection::Unavailable;
    int changes = 0;
    QDate since_date;
};

inline CftcPositioningPersistence cftc_positioning_persistence(const QVector<CftcDatedValue>& series,
                                                               const QDate& as_of = {}) {
    CftcPositioningPersistence out;
    if (series.size() < 2)
        return out;
    if (as_of.isValid() && series.last().date != as_of)
        return out;
    const int last = series.size() - 1;
    if (static_cast<int>(series[last - 1].date.daysTo(series[last].date)) > kCftcWeeklyGapDays)
        return out;
    const auto step_direction = [](double step) {
        if (step > 0.0)
            return CftcDirection::Up;
        if (step < 0.0)
            return CftcDirection::Down;
        return CftcDirection::Flat;
    };
    out.direction = step_direction(series[last].value - series[last - 1].value);
    out.has_value = true;
    out.changes = 1;
    out.since_date = series[last - 1].date;
    for (int i = last - 1; i > 0; --i) {
        if (static_cast<int>(series[i - 1].date.daysTo(series[i].date)) > kCftcWeeklyGapDays)
            break;
        if (step_direction(series[i].value - series[i - 1].value) != out.direction)
            break;
        ++out.changes;
        out.since_date = series[i - 1].date;
    }
    return out;
}

// ── Signal-reference trailing normalization ─────────────────────────────────

/// Explicit trailing windows for judging whether the current reading is
/// historically unusual. Weeks are exact 7-day spans (26W = 182 days,
/// 52W = 364 days); years are calendar years.
enum class CftcTrailingWindow { Weeks26, Weeks52, Years2, Years5 };

inline QString cftc_trailing_window_code(CftcTrailingWindow window) {
    switch (window) {
        case CftcTrailingWindow::Weeks26:
            return QStringLiteral("26W");
        case CftcTrailingWindow::Weeks52:
            return QStringLiteral("52W");
        case CftcTrailingWindow::Years2:
            return QStringLiteral("2Y");
        case CftcTrailingWindow::Years5:
            return QStringLiteral("5Y");
    }
    return {};
}

inline QDate cftc_trailing_window_start(const QDate& latest_report, CftcTrailingWindow window) {
    if (!latest_report.isValid())
        return {};
    switch (window) {
        case CftcTrailingWindow::Weeks26:
            return latest_report.addDays(-182);
        case CftcTrailingWindow::Weeks52:
            return latest_report.addDays(-364);
        case CftcTrailingWindow::Years2:
            return latest_report.addYears(-2);
        case CftcTrailingWindow::Years5:
            return latest_report.addYears(-5);
    }
    return {};
}

/// Minimum trailing observations before a signal-reference percentile / COT
/// Index / z-score is treated as evidence. A window that spans the requested
/// period but holds only a handful of reports cannot describe a distribution;
/// callers may lower it explicitly when they accept a sparser reference.
inline constexpr int kCftcTrailingMinObservations = 8;

/// Strictly trailing reference statistics for one window.
///
/// The reference distribution is every valid observation at or after the
/// window start and strictly before the current report, so the current
/// observation never contributes to the min/max/average/percentile/z-score
/// that judge it. Because the reference excludes the current value, COT Index
/// and percentile are not clamped: a new historical high yields a COT Index
/// above 100 and a percentile of 100, and a new low below 0 / 0%.
///
/// Availability follows the truthfulness rules: the series must reach the
/// requested as-of report, the returned history must span the whole window
/// (`reference_covered`), and the reference must hold at least
/// `minimum_reference` observations (default kCftcTrailingMinObservations,
/// below which a percentile is noise rather than evidence). Otherwise every
/// reference measure stays unavailable. `zero_variance` records a flat
/// reference window where COT Index and z-score are undefined; percentile
/// follows its own tie rule (count of references at or below the current
/// value) and min/max/average still describe the real observations.
struct CftcTrailingStats {
    CftcTrailingWindow window = CftcTrailingWindow::Weeks26;
    QDate latest_report;
    bool at_latest_report = false;
    bool current_present = false;
    double current = 0.0;
    bool reference_covered = false;
    int reference_count = 0;
    QDate reference_first_date;
    QDate reference_last_date;
    bool has_min = false;
    double min_value = 0.0;
    bool has_max = false;
    double max_value = 0.0;
    bool has_avg = false;
    double avg = 0.0;
    bool zero_variance = false;
    bool has_percentile = false;
    double percentile = 0.0;
    bool has_cot_index = false;
    double cot_index = 0.0;
    bool has_zscore = false;
    double zscore = 0.0;
    bool has_distance_high = false;
    double distance_high = 0.0;
    bool has_distance_low = false;
    double distance_low = 0.0;
};

inline CftcTrailingStats cftc_trailing_stats(const QVector<CftcDatedValue>& series, CftcTrailingWindow window,
                                             const QDate& as_of = {},
                                             int minimum_reference = kCftcTrailingMinObservations) {
    CftcTrailingStats out;
    out.window = window;
    if (series.isEmpty())
        return out;
    out.latest_report = series.last().date;
    out.at_latest_report = !as_of.isValid() || out.latest_report == as_of;
    if (!out.at_latest_report)
        return out;
    out.current_present = true;
    out.current = series.last().value;

    const QDate start = cftc_trailing_window_start(out.latest_report, window);
    out.reference_covered = series.first().date <= start;
    if (!out.reference_covered)
        return out;

    QVector<CftcDatedValue> reference;
    reference.reserve(series.size());
    for (const auto& point : series) {
        if (point.date >= start && point.date < out.latest_report)
            reference.append(point);
    }
    out.reference_count = reference.size();
    if (out.reference_count < std::max(2, minimum_reference))
        return out;
    out.reference_first_date = reference.first().date;
    out.reference_last_date = reference.last().date;

    double min_value = reference.first().value;
    double max_value = reference.first().value;
    double sum = 0.0;
    for (const auto& point : reference) {
        min_value = std::min(min_value, point.value);
        max_value = std::max(max_value, point.value);
        sum += point.value;
    }
    out.has_min = out.has_max = out.has_avg = true;
    out.min_value = min_value;
    out.max_value = max_value;
    out.avg = sum / static_cast<double>(out.reference_count);

    const double range = max_value - min_value;
    out.zero_variance = range == 0.0;
    if (range > 0.0) {
        out.has_cot_index = true;
        out.cot_index = (out.current - min_value) / range * 100.0;
    }
    // Percentile = share of the reference at or below the current value. It
    // divides by the reference count, not by the range, so it stays defined
    // for a flat reference window (all references tie with a current value at
    // or above the flat level; a current value below it reads 0%).
    int at_or_below = 0;
    for (const auto& point : reference) {
        if (point.value <= out.current)
            ++at_or_below;
    }
    out.has_percentile = true;
    out.percentile = 100.0 * static_cast<double>(at_or_below) / static_cast<double>(out.reference_count);
    double sum_sq = 0.0;
    for (const auto& point : reference) {
        const double deviation = point.value - out.avg;
        sum_sq += deviation * deviation;
    }
    const double sd = std::sqrt(sum_sq / static_cast<double>(out.reference_count - 1));
    if (sd > 0.0) {
        out.has_zscore = true;
        out.zscore = (out.current - out.avg) / sd;
    }
    out.has_distance_high = true;
    out.distance_high = out.current - max_value;
    out.has_distance_low = true;
    out.distance_low = out.current - min_value;
    return out;
}

// ── Extreme state / return-from-extreme primitives ──────────────────────────

/// Measurement-only extreme state under explicitly supplied thresholds. This
/// layer never labels an exit from an extreme: it reports whether the current
/// report sits at/above the upper or at/below the lower threshold, how many
/// consecutive weekly reports stayed there, whether the previous report just
/// left the band, and the most recent prior extreme inside the supplied
/// history together with the distance from it. A gap beyond the weekly
/// tolerance breaks a streak because the unobserved reports are unknown.
struct CftcExtremeState {
    bool at_latest_report = false;
    bool has_value = false;
    double current = 0.0;
    bool has_previous_adjacent = false;
    bool at_upper = false;
    bool at_lower = false;
    int weeks_at_upper = 0;
    int weeks_at_lower = 0;
    bool remained_at_upper = false;
    bool remained_at_lower = false;
    bool left_upper = false;
    bool left_lower = false;
    bool has_prior_upper = false;
    QDate prior_upper_date;
    double prior_upper_value = 0.0;
    bool has_prior_lower = false;
    QDate prior_lower_date;
    double prior_lower_value = 0.0;
    bool has_distance_from_upper = false;
    double distance_from_upper = 0.0;
    bool has_distance_from_lower = false;
    double distance_from_lower = 0.0;
    bool away_from_upper = false;
    bool away_from_lower = false;
};

inline CftcExtremeState cftc_extreme_state(const QVector<CftcDatedValue>& series, std::optional<double> upper_threshold,
                                           std::optional<double> lower_threshold, const QDate& as_of = {}) {
    CftcExtremeState out;
    if (series.isEmpty())
        return out;
    out.at_latest_report = !as_of.isValid() || series.last().date == as_of;
    if (!out.at_latest_report)
        return out;
    out.has_value = true;
    out.current = series.last().value;
    out.at_upper = upper_threshold.has_value() && out.current >= *upper_threshold;
    out.at_lower = lower_threshold.has_value() && out.current <= *lower_threshold;

    const auto streak = [&series](std::optional<double> threshold, bool upper) {
        if (!threshold)
            return 0;
        int weeks = 0;
        for (int i = series.size() - 1; i >= 0; --i) {
            const bool satisfies = upper ? series[i].value >= *threshold : series[i].value <= *threshold;
            if (!satisfies)
                break;
            if (weeks > 0 && static_cast<int>(series[i].date.daysTo(series[i + 1].date)) > kCftcWeeklyGapDays)
                break;
            ++weeks;
        }
        return weeks;
    };
    out.weeks_at_upper = streak(upper_threshold, true);
    out.weeks_at_lower = streak(lower_threshold, false);
    out.remained_at_upper = out.at_upper && out.weeks_at_upper >= 2;
    out.remained_at_lower = out.at_lower && out.weeks_at_lower >= 2;

    if (series.size() >= 2) {
        out.has_previous_adjacent =
            static_cast<int>(series[series.size() - 2].date.daysTo(series.last().date)) <= kCftcWeeklyGapDays;
        if (out.has_previous_adjacent) {
            const double previous = series[series.size() - 2].value;
            out.left_upper = upper_threshold.has_value() && previous >= *upper_threshold && !out.at_upper;
            out.left_lower = lower_threshold.has_value() && previous <= *lower_threshold && !out.at_lower;
        }
    }

    for (int i = series.size() - 2; i >= 0; --i) {
        if (!out.has_prior_upper && upper_threshold.has_value() && series[i].value >= *upper_threshold) {
            out.has_prior_upper = true;
            out.prior_upper_date = series[i].date;
            out.prior_upper_value = series[i].value;
        }
        if (!out.has_prior_lower && lower_threshold.has_value() && series[i].value <= *lower_threshold) {
            out.has_prior_lower = true;
            out.prior_lower_date = series[i].date;
            out.prior_lower_value = series[i].value;
        }
        const bool upper_done = !upper_threshold.has_value() || out.has_prior_upper;
        const bool lower_done = !lower_threshold.has_value() || out.has_prior_lower;
        if (upper_done && lower_done)
            break;
    }
    if (out.has_prior_upper) {
        out.has_distance_from_upper = true;
        out.distance_from_upper = out.current - out.prior_upper_value;
        out.away_from_upper = !out.at_upper && out.current < out.prior_upper_value;
    }
    if (out.has_prior_lower) {
        out.has_distance_from_lower = true;
        out.distance_from_lower = out.current - out.prior_lower_value;
        out.away_from_lower = !out.at_lower && out.current > out.prior_lower_value;
    }
    return out;
}

// ── Descriptive window statistics ───────────────────────────────────────────

/// Statistics over one explicitly defined window. Every measure carries its own
/// presence flag; a formula that would divide by zero (or by a window with no
/// variance) is unavailable, not a number.
///
/// Unlike CftcTrailingStats, this describes a window and includes the latest
/// observation in the distribution (the R3 workspace's existing semantics).
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
/// (latest date − days). When `as_of` is a valid report date, the "latest"
/// close is the most recent session on or before that date, so a full price
/// history extending past the report cannot leak future closes into a
/// historical calculation. Absent when no close exists at or before `as_of`,
/// when the history does not reach back that far, or when the anchor is far
/// enough before the target that the span no longer matches the comparison it
/// is paired with (same bound as the positioning change).
inline std::optional<double> cftc_price_change_since_days(const QVector<CftcPricePoint>& prices, int days,
                                                          const QDate& as_of = {}) {
    if (prices.size() < 2)
        return std::nullopt;
    int last_index = prices.size() - 1;
    if (as_of.isValid()) {
        last_index = -1;
        for (int i = 0; i < prices.size(); ++i) {
            if (prices[i].date <= as_of)
                last_index = i;
            else
                break;
        }
        if (last_index < 1)
            return std::nullopt;
    }
    const QDate latest = prices[last_index].date;
    const QDate floor = latest.addDays(-(days + kCftcWeeklyGapDays));
    const CftcPricePoint* anchor = nullptr;
    for (int i = 0; i <= last_index; ++i) {
        if (prices[i].date <= latest.addDays(-days))
            anchor = &prices[i];
        else
            break;
    }
    if (!anchor || anchor->date < floor)
        return std::nullopt;
    return prices[last_index].close - anchor->close;
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

/// Directional pair for a price change and an open-interest change at the same
/// horizon. Either side may be unavailable; the combination never substitutes
/// zero for a missing change and never labels a regime here.
struct CftcPriceOiDirections {
    bool price_available = false;
    bool open_interest_available = false;
    CftcDirection price = CftcDirection::Unavailable;
    CftcDirection open_interest = CftcDirection::Unavailable;
};

inline CftcPriceOiDirections cftc_price_oi_directions(std::optional<double> price_change,
                                                      std::optional<double> open_interest_change) {
    CftcPriceOiDirections out;
    out.price_available = price_change.has_value();
    out.open_interest_available = open_interest_change.has_value();
    out.price = cftc_direction(price_change);
    out.open_interest = cftc_direction(open_interest_change);
    return out;
}

} // namespace fincept::services
