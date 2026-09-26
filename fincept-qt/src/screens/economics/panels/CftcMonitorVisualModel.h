// src/screens/economics/panels/CftcMonitorVisualModel.h
//
// MarketLab CFTC UI/UX batch: the presentation-only projection of the
// qualified cross-market monitor model (services/economics/CftcMonitorModel.h)
// into grouped sections and one primary visual encoding per market.
//
// This header owns no analysis. Every value it exposes is read from the
// already-computed CftcMonitorEntry fields (the frozen Batch 4A engine output
// projected by the monitor). It never recalculates a percentile, a flow or a
// state, and it never substitutes a plotting value for an unavailable measure:
// a metric that the engine did not form stays `has_value == false`, so the
// widget layer can render it as unavailable rather than as zero.
//
// Grouping uses the qualified CftcMarketCatalog asset classes. Group and row
// order is the catalog's established display order; an entry whose asset class
// is not in the catalog is never dropped — it is carried in an explicit
// trailing group keyed by its own asset-class code.
#pragma once

#include "screens/economics/panels/CftcNetFormat.h"
#include "services/economics/CftcInterpretationModel.h"
#include "services/economics/CftcMarketCatalog.h"
#include "services/economics/CftcMonitorModel.h"

#include <QCoreApplication>
#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <utility>

namespace fincept::screens {

// ── Primary visual metric ───────────────────────────────────────────────────

/// The metrics the grouped cross-market bars may encode. Net %OI is the
/// primary, cross-market comparable measure (normalized by Open Interest);
/// the percentile is the qualified 156-prior-report historical reference; the
/// 1R/4R/13R readings are the principal participant's net flows as a share of
/// prior Open Interest. Raw-net ranking is deliberately not offered.
enum class CftcMonitorMetric {
    NetPctOi,
    Percentile,
    Net1R,
    Net4R,
    Net13R,
};

inline QString cftc_monitor_metric_label(CftcMonitorMetric metric) {
    switch (metric) {
        case CftcMonitorMetric::NetPctOi:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Net %OI");
        case CftcMonitorMetric::Percentile:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Percentile (156R)");
        case CftcMonitorMetric::Net1R:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Net flow 1R");
        case CftcMonitorMetric::Net4R:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Net flow 4R");
        case CftcMonitorMetric::Net13R:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Net flow 13R");
    }
    return {};
}

inline QString cftc_monitor_metric_caption(CftcMonitorMetric metric) {
    switch (metric) {
        case CftcMonitorMetric::NetPctOi:
            return QCoreApplication::translate("CftcMonitorVisualModel",
                                               "Principal participant net position as % of current Open Interest. "
                                               "Comparable across markets of different contract size because it is "
                                               "normalized by each market's own Open Interest.");
        case CftcMonitorMetric::Percentile:
            return QCoreApplication::translate("CftcMonitorVisualModel",
                                               "Net %OI against the previous 156 valid reports, midpoint-tie "
                                               "percentile; the current report is excluded from its own reference.");
        case CftcMonitorMetric::Net1R:
        case CftcMonitorMetric::Net4R:
        case CftcMonitorMetric::Net13R:
            return QCoreApplication::translate("CftcMonitorVisualModel",
                                               "Principal participant net flow over the horizon as % of prior Open "
                                               "Interest.");
    }
    return {};
}

inline bool cftc_monitor_metric_is_percentile(CftcMonitorMetric metric) {
    return metric == CftcMonitorMetric::Percentile;
}

inline int cftc_monitor_metric_decimals(CftcMonitorMetric metric) {
    return cftc_monitor_metric_is_percentile(metric) ? 1 : 2;
}

inline int cftc_monitor_metric_horizon(CftcMonitorMetric metric) {
    switch (metric) {
        case CftcMonitorMetric::Net1R:
            return 1;
        case CftcMonitorMetric::Net4R:
            return 4;
        case CftcMonitorMetric::Net13R:
            return 13;
        case CftcMonitorMetric::NetPctOi:
        case CftcMonitorMetric::Percentile:
            break;
    }
    return 0;
}

// ── Metric reading ──────────────────────────────────────────────────────────

struct CftcMonitorMetricReading {
    bool has_value = false;
    double value = 0.0; // display units: percentile is 0..100, flows are %
};

inline const services::CftcHorizonFlowReading* cftc_monitor_flow_reading(const services::CftcMonitorEntry& entry,
                                                                         int horizon) {
    for (const services::CftcHorizonFlowReading& reading : entry.flow_readings) {
        if (reading.horizon_reports == horizon)
            return &reading;
    }
    return nullptr;
}

/// Read the selected metric from the entry's already-computed fields. A missing
/// measure returns `has_value == false` with the value left at zero; the caller
/// must render that as unavailable, never as a zero bar.
inline CftcMonitorMetricReading cftc_monitor_metric_reading(const services::CftcMonitorEntry& entry,
                                                            CftcMonitorMetric metric) {
    CftcMonitorMetricReading out;
    if (metric == CftcMonitorMetric::NetPctOi) {
        out.has_value = entry.has_net_pct_oi;
        out.value = entry.net_pct_oi;
        return out;
    }
    if (metric == CftcMonitorMetric::Percentile) {
        out.has_value = entry.has_percentile;
        out.value = entry.percentile * 100.0;
        return out;
    }
    const int horizon = cftc_monitor_metric_horizon(metric);
    const services::CftcHorizonFlowReading* reading = cftc_monitor_flow_reading(entry, horizon);
    if (reading && reading->evaluated && reading->has_net_flow) {
        out.has_value = true;
        out.value = reading->net_flow;
    }
    return out;
}

inline QString cftc_monitor_signed_percent(double value, int decimals) {
    const QString digits = QString::number(value, 'f', decimals);
    if (value > 0.0)
        return QStringLiteral("+") + digits;
    return digits;
}

/// Exact metric text: "—" when the engine did not form the measure, "0.00%"
/// (no sign) for an exact zero, "+x.xx%"/"-x.xx%" otherwise.
inline QString cftc_monitor_metric_text(const CftcMonitorMetricReading& reading, CftcMonitorMetric metric) {
    if (!reading.has_value)
        return QStringLiteral("—");
    if (cftc_monitor_metric_is_percentile(metric))
        return QString::number(reading.value, 'f', cftc_monitor_metric_decimals(metric)) + QLatin1Char('%');
    return cftc_monitor_signed_percent(reading.value, cftc_monitor_metric_decimals(metric)) + QLatin1Char('%');
}

/// The shared bar scale for the whole scanned universe. Signed metrics use the
/// largest absolute reading, with a small non-zero floor so an all-zero
/// universe still renders a baseline instead of dividing by zero. The
/// percentile is a bounded rank, so its shared axis is the full 0..100 range
/// regardless of the largest value observed in a given scan: a bar can never
/// reach the labelled end of the axis without the reading to match it. The same
/// extent is passed to every group chart so bars are comparable across groups
/// even while the group filter shows a subset.
inline double cftc_monitor_metric_scale_extent(const QVector<services::CftcMonitorEntry>& entries,
                                               CftcMonitorMetric metric, double minimum = 1.0) {
    if (cftc_monitor_metric_is_percentile(metric))
        return 100.0;
    double extent = 0.0;
    for (const services::CftcMonitorEntry& entry : entries) {
        const CftcMonitorMetricReading reading = cftc_monitor_metric_reading(entry, metric);
        if (reading.has_value)
            extent = std::max(extent, std::abs(reading.value));
    }
    return std::max(extent, minimum);
}

// ── Grouping and filters ────────────────────────────────────────────────────

struct CftcMonitorGroup {
    QString asset_class;
    QString label;
    QVector<int> entry_indexes; // indexes into the monitor entry vector
};

inline QString cftc_monitor_group_label(const QString& asset_class) {
    if (asset_class.isEmpty())
        return QCoreApplication::translate("CftcMonitorVisualModel", "Unknown market");
    return services::cftc_asset_class_label(asset_class);
}

/// The provider/freshness status label. Presentation only; the fail-closed
/// report-level condition is applied by `cftc_monitor_status_text`.
inline QString cftc_monitor_status_label(services::CftcMonitorStatus status) {
    switch (status) {
        case services::CftcMonitorStatus::Ok:
            return QCoreApplication::translate("CftcMonitorVisualModel", "OK");
        case services::CftcMonitorStatus::ArchiveOnly:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Stored history");
        case services::CftcMonitorStatus::NoData:
            return QCoreApplication::translate("CftcMonitorVisualModel", "No data");
        case services::CftcMonitorStatus::NoLocalHistory:
            return QCoreApplication::translate("CftcMonitorVisualModel", "No stored history");
        case services::CftcMonitorStatus::Unavailable:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Unavailable");
        case services::CftcMonitorStatus::UnknownMarket:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Unknown market");
    }
    return QCoreApplication::translate("CftcMonitorVisualModel", "Unavailable");
}

/// Whether the entry is ordinary validated current data: the provider refresh
/// succeeded AND the engine actually interpreted the report. A fresh provider
/// status does not authorize "current" when the report itself could not be
/// interpreted (`report_unavailable`).
inline bool cftc_monitor_report_is_current(const services::CftcMonitorEntry& entry) {
    return entry.status == services::CftcMonitorStatus::Ok && !entry.report_unavailable;
}

/// The effective displayed status: a report the engine could not interpret is
/// never shown as OK, whatever the provider refresh status says.
inline QString cftc_monitor_status_text(const services::CftcMonitorEntry& entry) {
    if (entry.report_unavailable)
        return QCoreApplication::translate("CftcMonitorVisualModel", "Report unavailable");
    return cftc_monitor_status_label(entry.status);
}

/// Whether an entry passes the presentation filters. `asset_class_filter` empty
/// means all groups; `notable_only` keeps only markets requiring attention
/// (data-quality conditions are themselves notable, so they are never hidden).
inline bool cftc_monitor_filter_matches(const services::CftcMonitorEntry& entry, const QString& asset_class_filter,
                                        bool notable_only) {
    if (notable_only && !entry.requires_attention)
        return false;
    if (!asset_class_filter.isEmpty() && entry.asset_class != asset_class_filter)
        return false;
    return true;
}

/// Ordered group sections, catalog order first. Entries whose asset class is
/// not part of the catalog (including an unknown market with no class) are
/// appended in encounter order so a scan can never silently drop a returned
/// market.
inline QVector<CftcMonitorGroup> cftc_monitor_groups(const QVector<services::CftcMonitorEntry>& entries,
                                                     const QString& asset_class_filter = QString(),
                                                     bool notable_only = false) {
    QVector<CftcMonitorGroup> groups;
    QVector<bool> placed(entries.size(), false);

    for (const QString& asset_class : services::cftc_asset_class_order()) {
        if (!asset_class_filter.isEmpty() && asset_class != asset_class_filter)
            continue;
        CftcMonitorGroup group;
        group.asset_class = asset_class;
        group.label = cftc_monitor_group_label(asset_class);
        for (int i = 0; i < entries.size(); ++i) {
            if (placed[i] || entries[i].asset_class != asset_class)
                continue;
            if (!cftc_monitor_filter_matches(entries[i], asset_class_filter, notable_only))
                continue;
            placed[i] = true;
            group.entry_indexes.append(i);
        }
        if (!group.entry_indexes.isEmpty())
            groups.append(group);
    }

    // Any entry not covered by the catalog order is carried explicitly, keyed
    // by its own asset-class code, in encounter order.
    QVector<QString> extra_classes;
    for (int i = 0; i < entries.size(); ++i) {
        if (placed[i] || entries[i].asset_class.isEmpty())
            continue;
        if (!cftc_monitor_filter_matches(entries[i], asset_class_filter, notable_only))
            continue;
        if (!extra_classes.contains(entries[i].asset_class))
            extra_classes.append(entries[i].asset_class);
    }
    if (asset_class_filter.isEmpty()) {
        for (const QString& asset_class : std::as_const(extra_classes)) {
            CftcMonitorGroup group;
            group.asset_class = asset_class;
            group.label = cftc_monitor_group_label(asset_class);
            for (int i = 0; i < entries.size(); ++i) {
                if (placed[i] || entries[i].asset_class != asset_class)
                    continue;
                if (!cftc_monitor_filter_matches(entries[i], asset_class_filter, notable_only))
                    continue;
                placed[i] = true;
                group.entry_indexes.append(i);
            }
            if (!group.entry_indexes.isEmpty())
                groups.append(group);
        }
        // Unknown-class entries (no asset class at all) are still shown.
        CftcMonitorGroup unknown;
        unknown.asset_class = QString();
        unknown.label = cftc_monitor_group_label(QString());
        for (int i = 0; i < entries.size(); ++i) {
            if (placed[i] || !entries[i].asset_class.isEmpty())
                continue;
            if (!cftc_monitor_filter_matches(entries[i], asset_class_filter, notable_only))
                continue;
            placed[i] = true;
            unknown.entry_indexes.append(i);
        }
        if (!unknown.entry_indexes.isEmpty())
            groups.append(unknown);
    }
    return groups;
}

// ── Summary and visual tone ─────────────────────────────────────────────────

struct CftcMonitorClassCount {
    services::CftcAttentionClass classification = services::CftcAttentionClass::DataQuality;
    int markets = 0;
};

struct CftcMonitorSummary {
    int total = 0;
    int current = 0;                             // provider status Ok and report not outdated
    int stored = 0;                              // stored-history fallback (ArchiveOnly)
    int problem = 0;                             // NoData / NoLocalHistory / Unavailable / UnknownMarket
    int outdated = 0;                            // interpreted report older than the engine freshness limit
    int attention = 0;                           // requires_attention
    int metric_available = 0;                    // entries where the selected metric was formed
    QVector<CftcMonitorClassCount> class_counts; // catalog attention-class order
};

/// Presentation summary over the scanned entries. Every count is derived from
/// the entry's own fields; no state is inferred from a visual category.
inline CftcMonitorSummary cftc_monitor_summary(const QVector<services::CftcMonitorEntry>& entries,
                                               CftcMonitorMetric metric) {
    CftcMonitorSummary summary;
    summary.total = entries.size();
    QVector<int> class_markets;
    for (const services::CftcAttentionClass classification :
         {services::CftcAttentionClass::DataQuality, services::CftcAttentionClass::Extreme,
          services::CftcAttentionClass::ExtremeTransition, services::CftcAttentionClass::Repositioning,
          services::CftcAttentionClass::OpenInterest, services::CftcAttentionClass::Concentration}) {
        CftcMonitorClassCount count;
        count.classification = classification;
        summary.class_counts.append(count);
    }
    for (const services::CftcMonitorEntry& entry : entries) {
        switch (entry.status) {
            case services::CftcMonitorStatus::Ok:
                if (entry.report_unavailable) {
                    // A fresh provider read does not make an uninterpretable
                    // report current: it is an availability problem.
                    ++summary.problem;
                } else if (entry.report_outdated) {
                    ++summary.outdated;
                } else {
                    ++summary.current;
                }
                break;
            case services::CftcMonitorStatus::ArchiveOnly:
                ++summary.stored;
                break;
            case services::CftcMonitorStatus::NoData:
            case services::CftcMonitorStatus::NoLocalHistory:
            case services::CftcMonitorStatus::Unavailable:
            case services::CftcMonitorStatus::UnknownMarket:
                ++summary.problem;
                break;
        }
        if (entry.requires_attention)
            ++summary.attention;
        if (cftc_monitor_metric_reading(entry, metric).has_value)
            ++summary.metric_available;
        QSet<int> seen_classes;
        for (const services::CftcAlert& alert : entry.alerts) {
            const int priority = services::cftc_attention_class_priority(alert.attention_class);
            if (seen_classes.contains(priority))
                continue;
            seen_classes.insert(priority);
            for (CftcMonitorClassCount& count : summary.class_counts) {
                if (count.classification == alert.attention_class)
                    ++count.markets;
            }
        }
    }
    return summary;
}

/// The entry's highest-priority alert class (the class shown by the visual
/// accent). `Ordinary` when the entry requires no attention.
enum class CftcMonitorTone {
    Ordinary,
    DataQuality,
    Extreme,
    ExtremeTransition,
    Repositioning,
    OpenInterest,
    Concentration,
};

inline services::CftcAttentionClass cftc_monitor_top_attention_class(const services::CftcMonitorEntry& entry) {
    services::CftcAttentionClass classification = services::CftcAttentionClass::Concentration;
    bool found = false;
    for (const services::CftcAlert& alert : entry.alerts) {
        if (!found || services::cftc_attention_class_priority(alert.attention_class) <
                          services::cftc_attention_class_priority(classification)) {
            classification = alert.attention_class;
            found = true;
        }
    }
    return classification;
}

inline CftcMonitorTone cftc_monitor_class_tone(services::CftcAttentionClass classification) {
    switch (classification) {
        case services::CftcAttentionClass::DataQuality:
            return CftcMonitorTone::DataQuality;
        case services::CftcAttentionClass::Extreme:
            return CftcMonitorTone::Extreme;
        case services::CftcAttentionClass::ExtremeTransition:
            return CftcMonitorTone::ExtremeTransition;
        case services::CftcAttentionClass::Repositioning:
            return CftcMonitorTone::Repositioning;
        case services::CftcAttentionClass::OpenInterest:
            return CftcMonitorTone::OpenInterest;
        case services::CftcAttentionClass::Concentration:
            return CftcMonitorTone::Concentration;
    }
    return CftcMonitorTone::DataQuality;
}

inline CftcMonitorTone cftc_monitor_entry_tone(const services::CftcMonitorEntry& entry) {
    if (!entry.requires_attention)
        return CftcMonitorTone::Ordinary;
    return cftc_monitor_class_tone(cftc_monitor_top_attention_class(entry));
}

inline QString cftc_monitor_tone_label(CftcMonitorTone tone) {
    switch (tone) {
        case CftcMonitorTone::Ordinary:
            return QCoreApplication::translate("CftcMonitorVisualModel", "Ordinary");
        case CftcMonitorTone::DataQuality:
            return services::cftc_attention_class_label(services::CftcAttentionClass::DataQuality);
        case CftcMonitorTone::Extreme:
            return services::cftc_attention_class_label(services::CftcAttentionClass::Extreme);
        case CftcMonitorTone::ExtremeTransition:
            return services::cftc_attention_class_label(services::CftcAttentionClass::ExtremeTransition);
        case CftcMonitorTone::Repositioning:
            return services::cftc_attention_class_label(services::CftcAttentionClass::Repositioning);
        case CftcMonitorTone::OpenInterest:
            return services::cftc_attention_class_label(services::CftcAttentionClass::OpenInterest);
        case CftcMonitorTone::Concentration:
            return services::cftc_attention_class_label(services::CftcAttentionClass::Concentration);
    }
    return {};
}

/// The status presentation tone: ordinary (current canonical data), warning
/// (stored-history fallback or an outdated report) or problem (no usable data).
enum class CftcMonitorStatusTone { Ordinary, Warning, Problem };

inline CftcMonitorStatusTone cftc_monitor_status_tone(const services::CftcMonitorEntry& entry) {
    switch (entry.status) {
        case services::CftcMonitorStatus::Ok:
            if (entry.report_unavailable)
                return CftcMonitorStatusTone::Problem;
            return entry.report_outdated ? CftcMonitorStatusTone::Warning : CftcMonitorStatusTone::Ordinary;
        case services::CftcMonitorStatus::ArchiveOnly:
            return CftcMonitorStatusTone::Warning;
        case services::CftcMonitorStatus::NoData:
        case services::CftcMonitorStatus::NoLocalHistory:
        case services::CftcMonitorStatus::Unavailable:
        case services::CftcMonitorStatus::UnknownMarket:
            return CftcMonitorStatusTone::Problem;
    }
    return CftcMonitorStatusTone::Problem;
}

} // namespace fincept::screens
