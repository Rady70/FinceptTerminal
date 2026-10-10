// src/screens/economics/panels/CftcWorkspaceContract.h
//
// Small pure contracts shared by the CFTC presentation layers: the 1W | 4W |
// 13W interpretation-horizon vocabulary and the label of the interpretation's
// strict 156-prior-report percentile.
//
// The research-workspace redesign (2026-10) replaced the Analysis page whose
// section order this header used to pin, and its raw-net-contract window
// percentile: the workspace now uses the Net %OI percentile against the
// previous 156 reports as its single historical measure (governing plan:
// percentile, COT Index and z-score are not independent confirmations).
//
// Header-only over Qt Core; it contains no analytical logic.
#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>

namespace fincept::screens {

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

} // namespace fincept::screens
