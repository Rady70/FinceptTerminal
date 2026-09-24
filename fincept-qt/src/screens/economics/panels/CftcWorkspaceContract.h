// src/screens/economics/panels/CftcWorkspaceContract.h
//
// Batch 4B correction pass: the CFTC Analysis page hierarchy, the
// interpretation-horizon controls, the selected-window percentile labelling and
// the neutral participant terminology as small pure contracts.
//
// This header exists so the page structure the user sees is testable without
// linking the widget tree (tests/ HARD RULE). CftcPanel consumes these helpers
// directly: the section order guard runs during page construction, the visible-
// range predicate decides which sections a range change refreshes, the horizon
// helpers drive the 1W | 4W | 13W buttons, and the percentile labels keep the
// interpretation's strict 156-prior-report metric visually distinct from the
// selected-window raw-net statistic.
//
// Header-only over Qt Core. It contains no analytical logic: Batch 4A decides
// every state and every metric, and this contract only names the presentation
// structure.
#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>

namespace fincept::screens {

/// One top-level section of the CFTC Analysis page. The order returned by
/// cftc_workspace_section_order() is the authoritative information hierarchy:
/// the report header, the latest snapshot, the Batch 4A interpretation and the
/// participant/statistics detail tables, then the analytical charts and the
/// heatmap.
enum class CftcWorkspaceSection {
    Header,                   ///< market/report identity and the history-display controls
    CurrentSnapshot,          ///< the latest official report at a glance
    Interpretation,           ///< the Batch 4A conclusion for the selected horizon
    Positioning,              ///< per-participant latest legs and shares
    WeeklyChanges,            ///< the two newest reports, all participants
    Statistics,               ///< selected-window statistics and extremes
    PricePositioningEvidence, ///< per-horizon price/positioning evidence (raw)
    SyncChart,                ///< the synchronized price + positioning chart
    HistoricalPositioning,    ///< the principal multi-series historical chart
    Heatmap,                  ///< the trailing positioning heatmap
};

inline QVector<CftcWorkspaceSection> cftc_workspace_section_order() {
    return {CftcWorkspaceSection::Header,
            CftcWorkspaceSection::CurrentSnapshot,
            CftcWorkspaceSection::Interpretation,
            CftcWorkspaceSection::Positioning,
            CftcWorkspaceSection::WeeklyChanges,
            CftcWorkspaceSection::Statistics,
            CftcWorkspaceSection::PricePositioningEvidence,
            CftcWorkspaceSection::SyncChart,
            CftcWorkspaceSection::HistoricalPositioning,
            CftcWorkspaceSection::Heatmap};
}

/// Whether the history-display range (1Y … MAX) changes this section's
/// presentation. The Interpretation is deliberately excluded: it always
/// evaluates the full validated official history through the Batch 4A engine,
/// and the strict 156-prior-report reference makes any visible-range scoping an
/// error. WeeklyChanges only reads the two newest reports, so it is independent
/// of the display range as well.
inline bool cftc_section_refreshed_by_visible_range(CftcWorkspaceSection section) {
    switch (section) {
        case CftcWorkspaceSection::Interpretation:
            return false;
        case CftcWorkspaceSection::WeeklyChanges:
            return false;
        case CftcWorkspaceSection::Positioning:
            // The positioning table is now latest-report legs and shares only;
            // it no longer carries selected-window statistics.
            return false;
        case CftcWorkspaceSection::Header:
        case CftcWorkspaceSection::CurrentSnapshot:
        case CftcWorkspaceSection::SyncChart:
        case CftcWorkspaceSection::HistoricalPositioning:
        case CftcWorkspaceSection::Statistics:
        case CftcWorkspaceSection::PricePositioningEvidence:
        case CftcWorkspaceSection::Heatmap:
            return true;
    }
    return false;
}

// ── Interpretation horizon (1W | 4W | 13W) ───────────────────────────────────

/// The supported interpretation horizons, in display order. These are CFTC
/// weekly report counts: a "1W" reading is the latest weekly report, "4W" the
/// four-report window and "13W" the thirteen-report window. They are the
/// Batch 4A evaluation horizons, not calendar-day anchors.
inline QVector<int> cftc_interpretation_horizons() {
    return {1, 4, 13};
}

inline bool cftc_is_interpretation_horizon(int horizon_reports) {
    return horizon_reports == 1 || horizon_reports == 4 || horizon_reports == 13;
}

/// The initially selected horizon. Four reports is the middle supported horizon
/// and matches the Batch 4A sustained-repositioning primary window.
constexpr int cftc_default_interpretation_horizon() {
    return 4;
}

/// The 1W | 4W | 13W button label for a supported horizon; empty otherwise.
inline QString cftc_horizon_button_label(int horizon_reports) {
    if (horizon_reports == 1)
        return QCoreApplication::translate("CftcWorkspace", "1W");
    if (horizon_reports == 4)
        return QCoreApplication::translate("CftcWorkspace", "4W");
    if (horizon_reports == 13)
        return QCoreApplication::translate("CftcWorkspace", "13W");
    return {};
}

/// The evidence-matrix column header for one selected horizon.
inline QString cftc_horizon_evidence_header(int horizon_reports) {
    if (horizon_reports == 1)
        return QCoreApplication::translate("CftcWorkspace", "1 REPORT");
    if (horizon_reports == 4)
        return QCoreApplication::translate("CftcWorkspace", "4 REPORTS");
    if (horizon_reports == 13)
        return QCoreApplication::translate("CftcWorkspace", "13 REPORTS");
    return {};
}

// ── Percentile labelling ─────────────────────────────────────────────────────

/// The Batch 4A interpretation percentile: the strict trailing Net %OI
/// percentile against the 156 prior reports. The label states both the metric
/// and the reference count so it can never be confused with the selected-window
/// statistic.
inline QString cftc_interpretation_percentile_label() {
    return QCoreApplication::translate("CftcPresentation", "Historical percentile (Net %OI vs 156 prior reports)");
}

/// The older workspace percentile: a descriptive percentile of the raw net
/// position inside the selected history display range, with the latest report
/// included in its own distribution. It is a different metric on a different
/// window and must never be presented as the interpretation measurement: the
/// card subtitle and this tooltip name the metric, and no categorical range
/// verdict is attached.
inline QString cftc_window_percentile_label(const QString& range_label) {
    return QCoreApplication::translate("CftcWorkspace", "WINDOW PERCENTILE (%1)").arg(range_label);
}

inline QString cftc_window_percentile_tooltip() {
    return QCoreApplication::translate(
        "CftcWorkspace",
        "Descriptive percentile of the raw net position (contracts) within the selected history display range "
        "(1Y–MAX), with the latest report included in the distribution. This is a different measure from the COT "
        "interpretation's strict trailing Net %OI percentile over the 156 prior reports: raw contracts rise with "
        "market size, so the two can differ widely when Open Interest has grown.");
}

/// The information-hierarchy guard the panel runs while building the Analysis
/// page: sections must be created in the contract order above.
inline bool cftc_section_order_stays_monotonic(int previous_index, int next_index) {
    return next_index > previous_index;
}

} // namespace fincept::screens
