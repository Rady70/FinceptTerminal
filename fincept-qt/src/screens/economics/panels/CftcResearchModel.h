// src/screens/economics/panels/CftcResearchModel.h
//
// Presentation projections for the CFTC research workspace: the cross-market
// overview rows with their standard labels, the key figures, and the series a
// single-market page draws. Every number is read from the interpretation
// engine's result or computed with the engine's own helpers
// (cftc_trailing_percentile_at, cftc_report_flow, cftc_position_metrics); the
// labels are the definitions in CftcResearchText.h.
//
// Header-only over Qt Core so the projections are unit-testable.
#pragma once

#include "screens/economics/panels/CftcResearchText.h"
#include "services/economics/CftcInterpretationModel.h"
#include "services/economics/CftcMarketCatalog.h"
#include "services/economics/CftcMetricModel.h"
#include "services/economics/CftcMonitorModel.h"

#include <QCoreApplication>
#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fincept::screens {

// ── Overview rows ───────────────────────────────────────────────────────────

/// One participant cell of the overview matrix.
struct CftcMatrixCell {
    QString participant_key;
    bool has_value = false;
    double net_pct_oi = 0.0;
    std::optional<double> percentile;
    CftcLevelTail tail = CftcLevelTail::None;
};

/// Availability of one market in the overview. `NotCovered` is a market the
/// selected report does not publish (e.g. a currency in the Disaggregated
/// report): it is not a data problem and is listed separately, not as an error.
enum class CftcRowAvailability { Current, Stored, Old, NotCovered, Unavailable };

struct CftcMarketRow {
    int entry_index = -1;
    QString market_key;
    QString label;
    QString asset_class;
    CftcRowAvailability availability = CftcRowAvailability::Unavailable;
    QString availability_note; ///< short reason for Stored / Old / Unavailable
    QDate report_date;
    /// The market's latest stored report is older than the newest report
    /// another market of this overview already has (a partial refresh, or a
    /// refresh not yet run): its figures are a week or more behind the rest.
    bool behind = false;
    QString behind_note;

    QString principal_key;
    bool has_reading = false; ///< principal Net %OI present at the latest report
    double net_pct_oi = 0.0;
    std::optional<double> percentile; ///< principal, latest report
    std::optional<double> percentile_1r;
    std::optional<double> percentile_4r;
    std::optional<double> percentile_13r;
    std::optional<double> net_flow_1w; ///< principal one-report net flow, % of the previous report's OI
    CftcLevelTail tail = CftcLevelTail::None;
    int tail_run = 0;

    // Standard labels of the principal (speculative) group.
    std::optional<CftcLevelBand> band;
    CftcBias bias = CftcBias::Unavailable;
    QString bias_reason;
    CftcCrowding crowding = CftcCrowding::None;
    QString crowding_reason; ///< with the CFTC category caveat
    CftcSideFlip flip;
    CftcFlowType flow_1w = CftcFlowType::Unavailable;
    CftcChangeSize size_1w = CftcChangeSize::Unavailable;
    std::optional<double> rank_1w;  ///< size rank of the one-week net change
    std::optional<double> scale_1w; ///< |one-week net change| / typical one-week change
    QString size_text_1w;           ///< the size in words, with its multiple
    bool turning = false;           ///< the speculators moved out of, or fell back from, a 10% tail
    QString turning_reason;
    CftcOneSided one_sided; ///< the speculators held one side in each of the previous 156 reports
    QString one_sided_reason;

    QVector<CftcMatrixCell> cells; ///< every participant, family order

    bool has_open_interest = false;
    double open_interest = 0.0;
    std::optional<double> oi_change_4r;
    std::optional<double> oi_rank_4r;
    QString oi_size_4r;                       ///< size of the 4-week Open Interest change, in words
    std::optional<double> concentration_long; ///< top-4 gross long, % of OI
    std::optional<double> concentration_long_percentile;

    QStringList tags; ///< standard tags in display order: CROWDED, FLIPPED, TURNING, ONE-SIDED, LARGE MOVE
};

/// Whether a row carries this week's figures: a report that is neither old
/// nor behind the overview's newest report.
inline bool cftc_row_at_newest_report(const CftcMarketRow& row) {
    return (row.availability == CftcRowAvailability::Current || row.availability == CftcRowAvailability::Stored) &&
           !row.behind;
}

/// The engine's one-report net flow of a group: the change in net contracts
/// since the previous weekly report, as a share of that report's Open
/// Interest. It is the figure the statements quote, the move ranks rank and
/// the LARGE MOVE tag uses, so every "1W" column shows it (not the difference
/// of two Net %OI levels, which also moves when Open Interest alone changes).
inline std::optional<double> cftc_one_week_net_flow(const services::CftcParticipantInterpretation& p) {
    for (const auto& reading : p.flow_readings)
        if (reading.horizon_reports == 1 && reading.evaluated && reading.has_net_flow)
            return reading.net_flow;
    return std::nullopt;
}

namespace detail {

inline const services::CftcParticipantTrail* entry_trail(const services::CftcMonitorEntry& entry, const QString& key) {
    for (const auto& trail : entry.trails)
        if (trail.participant_key == key)
            return &trail;
    return nullptr;
}

inline bool has_state(const services::CftcParticipantInterpretation& p, const char* id) {
    for (const auto& s : p.states)
        if (s.state_id == QLatin1String(id))
            return true;
    return false;
}

} // namespace detail

inline CftcRowAvailability cftc_row_availability(const services::CftcMonitorEntry& entry, QString& note) {
    using services::CftcMonitorStatus;
    note.clear();
    if (entry.status == CftcMonitorStatus::NoData)
        return CftcRowAvailability::NotCovered;
    if (!entry.interpreted || entry.report_unavailable) {
        note = entry.status_detail.isEmpty()
                   ? (entry.report_unavailable ? cftc_unavailable_reason_wording(entry.report_unavailable_reason)
                                               : cftc_text_tr("no stored or current report"))
                   : entry.status_detail;
        return CftcRowAvailability::Unavailable;
    }
    if (entry.report_outdated) {
        note = cftc_text_tr("latest report %1 days old").arg(entry.report_age_days);
        return CftcRowAvailability::Old;
    }
    // Stored history is flagged only when a refresh was asked for and failed.
    // A read of the stored reports alone (the panel's opening read) is the
    // normal state, and its age is judged by the report-age check above.
    if (entry.status == CftcMonitorStatus::ArchiveOnly && !entry.status_detail.isEmpty()) {
        note = cftc_text_tr("stored data; refresh failed: %1").arg(entry.status_detail);
        return CftcRowAvailability::Stored;
    }
    return CftcRowAvailability::Current;
}

inline CftcMarketRow cftc_market_row(const services::CftcMonitorEntry& entry, int index) {
    CftcMarketRow row;
    row.entry_index = index;
    row.market_key = entry.market_key;
    row.label = entry.label;
    row.asset_class = entry.asset_class;
    row.availability = cftc_row_availability(entry, row.availability_note);
    row.report_date = entry.report_date;
    row.principal_key = cftc_principal_participant_key(entry.family);
    if (row.availability == CftcRowAvailability::NotCovered || row.availability == CftcRowAvailability::Unavailable)
        return row;
    const services::CftcInterpretationResult& result = entry.result;
    const services::CftcInterpretationConfig& config = result.config;
    for (const auto& p : result.participants) {
        CftcMatrixCell cell;
        cell.participant_key = p.participant_key;
        cell.has_value = p.has_net_pct_oi;
        cell.net_pct_oi = p.net_pct_oi;
        if (p.historical_percentile_available) {
            cell.percentile = p.percentile;
            cell.tail = cftc_level_tail(p.percentile, config);
        }
        row.cells.append(cell);
        if (p.participant_key != row.principal_key)
            continue;
        row.has_reading = p.has_net_pct_oi;
        row.net_flow_1w = cftc_one_week_net_flow(p);
        row.net_pct_oi = p.net_pct_oi;
        row.percentile = cell.percentile;
        row.tail = cell.tail;
        if (cell.percentile)
            row.band = cftc_level_band(*cell.percentile, config);
        row.bias = cftc_positioning_bias(p, config);
        row.bias_reason = cftc_bias_reason(row.bias, p, config);
        row.crowding = cftc_crowding(p);
        row.crowding_reason = cftc_crowding_reason(row.crowding, p, config);
        for (const auto& reading : p.flow_readings) {
            if (reading.horizon_reports != 1)
                continue;
            row.flow_1w = cftc_flow_type(reading, config);
            if (reading.evaluated) {
                row.size_1w = cftc_change_size(reading.has_net_rank, reading.net_rank, config);
                if (reading.has_net_rank) {
                    row.rank_1w = reading.net_rank;
                    row.size_text_1w =
                        cftc_move_size_text(reading.net_rank, 1, reading.has_net_scale, reading.net_scale, config);
                }
                if (reading.has_net_scale)
                    row.scale_1w = reading.net_scale;
            }
        }
        if (const services::CftcParticipantTrail* trail = detail::entry_trail(entry, p.participant_key)) {
            row.percentile_1r = trail->percentile_1r;
            row.percentile_4r = trail->percentile_4r;
            row.percentile_13r = trail->percentile_13r;
            row.tail_run = trail->tail_run_reports;
            row.flip = cftc_side_flip(trail->net_pct_oi);
            row.one_sided = cftc_one_sided(trail->net_pct_oi);
            row.one_sided_reason = cftc_one_sided_reason(row.one_sided);
        }
    }
    row.has_open_interest = result.open_interest_available;
    row.open_interest = result.open_interest;
    for (const auto& reading : result.open_interest_readings) {
        if (reading.horizon_reports != 4 || !reading.evaluated)
            continue;
        if (reading.has_oi_change)
            row.oi_change_4r = reading.oi_change;
        if (reading.has_rank) {
            row.oi_rank_4r = reading.rank;
            row.oi_size_4r = cftc_move_size_text(reading.rank, 4, reading.has_scale, reading.scale, config);
        }
    }
    for (const auto& assessment : result.concentration) {
        if (assessment.field_key != QLatin1String("concentration_gross_4_long"))
            continue;
        if (assessment.has_current)
            row.concentration_long = assessment.current;
        if (assessment.has_percentile)
            row.concentration_long_percentile = assessment.percentile;
    }
    // Turning: the speculators moved out of, or fell back from, a 10% tail
    // (every tag of the row describes the speculators; the market page shows
    // each group's own).
    for (const auto& p : result.participants) {
        if (p.participant_key != row.principal_key)
            continue;
        if (detail::has_state(p, "EXITED_HIGH_EXTREME") || detail::has_state(p, "EXITED_LOW_EXTREME") ||
            detail::has_state(p, "UNWINDING_HIGH_EXTREME") || detail::has_state(p, "UNWINDING_LOW_EXTREME"))
            row.turning_reason =
                cftc_transition_statement(entry.family, p, detail::entry_trail(entry, p.participant_key));
    }
    row.turning = !row.turning_reason.isEmpty();
    if (row.crowding != CftcCrowding::None)
        row.tags << cftc_crowding_label(row.crowding);
    if (row.flip.direction != 0)
        row.tags << cftc_side_flip_label(row.flip);
    if (row.turning)
        row.tags << cftc_text_tr("TURNING");
    if (row.one_sided.direction != 0)
        row.tags << cftc_one_sided_label(row.one_sided);
    if (row.size_1w == CftcChangeSize::Large)
        row.tags << cftc_text_tr("LARGE MOVE");
    return row;
}

/// The report date that decides when the overview refreshes itself: the oldest
/// latest report among the markets that have one. The stored copy can hold
/// markets at different reports (a read of only some markets), and each of
/// them is due its next report ten days after its own; the newest date alone
/// would leave the others a week behind until REFRESH. An old report (a
/// contract that may no longer be reported) counts too; the panel's hourly
/// limit keeps that from repeating the read.
inline QDate cftc_overview_oldest_report(const QVector<CftcMarketRow>& rows) {
    QDate oldest;
    for (const auto& row : rows) {
        if (row.availability == CftcRowAvailability::NotCovered || row.availability == CftcRowAvailability::Unavailable)
            continue;
        if (row.report_date.isValid() && (!oldest.isValid() || row.report_date < oldest))
            oldest = row.report_date;
    }
    return oldest;
}

/// Marks the rows whose latest report is older than the newest report among
/// the rows with a current or stored one. The CFTC publishes every market of a
/// report on the same day, so an older date means this copy was not refreshed
/// for that market (for example a read of only some markets); the row is
/// named as behind instead of being mixed silently with this week's figures.
inline void cftc_mark_behind(QVector<CftcMarketRow>& rows) {
    QDate newest;
    for (const auto& row : rows)
        if ((row.availability == CftcRowAvailability::Current || row.availability == CftcRowAvailability::Stored) &&
            row.report_date.isValid() && row.report_date > newest)
            newest = row.report_date;
    for (auto& row : rows) {
        row.behind = false;
        row.behind_note.clear();
        if ((row.availability != CftcRowAvailability::Current && row.availability != CftcRowAvailability::Stored) ||
            !row.report_date.isValid() || !newest.isValid() || row.report_date >= newest)
            continue;
        row.behind = true;
        row.behind_note = cftc_text_tr("report of %1; other markets already have the report of %2")
                              .arg(row.report_date.toString(QStringLiteral("d MMM yyyy")),
                                   newest.toString(QStringLiteral("d MMM yyyy")));
    }
}

/// Overview rows in catalog order (asset class, then catalog position), with
/// unknown markets last.
inline QVector<CftcMarketRow> cftc_market_rows(const QVector<services::CftcMonitorEntry>& entries) {
    QVector<CftcMarketRow> rows;
    const auto catalog = services::cftc_market_catalog();
    auto position = [&catalog](const QString& key) {
        for (int i = 0; i < catalog.size(); ++i)
            if (catalog[i].key == key)
                return i;
        return static_cast<int>(catalog.size());
    };
    for (int i = 0; i < entries.size(); ++i)
        rows.append(cftc_market_row(entries[i], i));
    std::stable_sort(rows.begin(), rows.end(), [&position](const CftcMarketRow& a, const CftcMarketRow& b) {
        return position(a.market_key) < position(b.market_key);
    });
    cftc_mark_behind(rows);
    return rows;
}

// ── Key figures ─────────────────────────────────────────────────────────────

struct CftcOverviewFigures {
    QDate latest_report;       ///< the most common report date among the markets at the newest report
    int markets_listed = 0;    ///< markets the selected report covers
    int markets_current = 0;   ///< with the latest report
    int markets_stored = 0;    ///< stored data only (refresh failed)
    int markets_old = 0;       ///< latest report older than the freshness limit
    int markets_behind = 0;    ///< a report older than other markets' newest (counted in current/stored too)
    int markets_at_newest = 0; ///< the markets this week's figures (FLIPPED, LARGE) are counted over
    /// No market of these rows has the newest report (every one is behind or
    /// old): latest_report is then the newest date they have.
    bool latest_report_behind = false;
    int markets_unavailable = 0;
    int markets_not_covered = 0;
    QStringList leaning_long; ///< speculators' lean
    QStringList leaning_short;
    QStringList neutral;
    QStringList one_sided; ///< speculators on one side in each of the previous 156 reports
    QStringList behind;
    QStringList crowded_long;
    QStringList crowded_short;
    QStringList flipped;            ///< speculators' net position changed side this week (newest report only)
    QStringList turning_markets;    ///< speculators moved out of / fell back from a tail
    QStringList large_move_markets; ///< speculators' one-week net change LARGE (newest report only)
};

inline CftcOverviewFigures cftc_overview_figures(const QVector<CftcMarketRow>& rows) {
    CftcOverviewFigures figures;
    QHash<QDate, int> dates;
    QDate fallback; ///< the newest report among old or behind markets
    for (const auto& row : rows) {
        switch (row.availability) {
            case CftcRowAvailability::NotCovered:
                ++figures.markets_not_covered;
                continue;
            case CftcRowAvailability::Unavailable:
                ++figures.markets_unavailable;
                break;
            case CftcRowAvailability::Old:
                ++figures.markets_old;
                break;
            case CftcRowAvailability::Stored:
                ++figures.markets_stored;
                break;
            case CftcRowAvailability::Current:
                ++figures.markets_current;
                break;
        }
        ++figures.markets_listed;
        const bool this_week = cftc_row_at_newest_report(row);
        if (row.behind) {
            ++figures.markets_behind;
            figures.behind << row.label;
        }
        if (this_week)
            ++figures.markets_at_newest;
        if (row.report_date.isValid() && this_week)
            dates[row.report_date] += 1;
        if (row.report_date.isValid() && (row.availability == CftcRowAvailability::Old || row.behind) &&
            (!fallback.isValid() || row.report_date > fallback))
            fallback = row.report_date;
        if (row.bias == CftcBias::LeaningLong)
            figures.leaning_long << row.label;
        else if (row.bias == CftcBias::LeaningShort)
            figures.leaning_short << row.label;
        else if (row.bias == CftcBias::Neutral)
            figures.neutral << row.label;
        if (row.one_sided.direction != 0)
            figures.one_sided << row.label;
        if (row.crowding == CftcCrowding::Long)
            figures.crowded_long << row.label;
        else if (row.crowding == CftcCrowding::Short)
            figures.crowded_short << row.label;
        // This week's changes come only from markets at the newest report: an
        // old or behind market's latest change is from an earlier week.
        if (row.flip.direction != 0 && this_week)
            figures.flipped << row.label;
        if (row.turning)
            figures.turning_markets << row.label;
        if (row.size_1w == CftcChangeSize::Large && this_week)
            figures.large_move_markets << row.label;
    }
    int best = 0;
    for (auto it = dates.constBegin(); it != dates.constEnd(); ++it) {
        if (it.value() > best || (it.value() == best && it.key() > figures.latest_report)) {
            best = it.value();
            figures.latest_report = it.key();
        }
    }
    // An asset-class tab whose markets are all behind (or old) still names
    // the report it shows, marked as behind.
    if (!figures.latest_report.isValid() && fallback.isValid()) {
        figures.latest_report = fallback;
        figures.latest_report_behind = true;
    }
    return figures;
}

// ── Largest weekly changes ──────────────────────────────────────────────────

/// One market of the weekly-change ranking: the speculators' one-week net
/// change and its size against their typical one-week change.
struct CftcMoveBar {
    QString market_key;
    QString label;
    double net_flow = 0.0; ///< % of the previous report's open interest
    double rank = 0.0;     ///< the engine's size rank
    bool has_scale = false;
    double scale = 0.0; ///< |net_flow| / typical one-week change
    CftcChangeSize size = CftcChangeSize::Unavailable;
    CftcFlowType flow = CftcFlowType::Unavailable;
    QString tooltip;
};

struct CftcMoveRanking {
    QVector<CftcMoveBar> rises; ///< net position rose (longs added or shorts covered), largest first
    QVector<CftcMoveBar> falls; ///< net position fell, largest first
};

/// Ranks the markets' one-week net changes by size: the multiple of the
/// typical one-week change, a change against a reference without any change
/// first, then the larger change as a share of open interest. Markets without
/// the newest report (an old or behind one is not this week's change) or
/// without a sized one-week change are left out, and so is a change of exactly
/// zero.
inline CftcMoveRanking cftc_weekly_move_ranking(const QVector<CftcMarketRow>& rows, int per_side = 8) {
    CftcMoveRanking out;
    for (const auto& row : rows) {
        if (!cftc_row_at_newest_report(row) || !row.net_flow_1w || *row.net_flow_1w == 0.0 || !row.rank_1w)
            continue;
        CftcMoveBar bar;
        bar.market_key = row.market_key;
        bar.label = row.label;
        bar.net_flow = *row.net_flow_1w;
        bar.rank = *row.rank_1w;
        bar.has_scale = row.scale_1w.has_value();
        bar.scale = row.scale_1w.value_or(0.0);
        bar.size = row.size_1w;
        bar.flow = row.flow_1w;
        bar.tooltip = cftc_text_tr("%1: net position %2 of open interest since the previous report%3 — %4.")
                          .arg(row.label, cftc_signed_pct(bar.net_flow),
                               bar.flow == CftcFlowType::Unavailable
                                   ? QString()
                                   : QStringLiteral(" (") + cftc_flow_label(bar.flow) + QLatin1Char(')'),
                               row.size_text_1w);
        (bar.net_flow > 0.0 ? out.rises : out.falls) << bar;
    }
    auto larger = [](const CftcMoveBar& a, const CftcMoveBar& b) {
        if (a.has_scale != b.has_scale)
            return !a.has_scale; // a change after a year without any change
        if (a.has_scale && a.scale != b.scale)
            return a.scale > b.scale;
        if (std::fabs(a.net_flow) != std::fabs(b.net_flow))
            return std::fabs(a.net_flow) > std::fabs(b.net_flow);
        return a.label < b.label;
    };
    for (QVector<CftcMoveBar>* side : {&out.rises, &out.falls}) {
        std::sort(side->begin(), side->end(), larger);
        if (side->size() > per_side)
            side->resize(per_side);
    }
    return out;
}

// ── Concentration ───────────────────────────────────────────────────────────

/// One of the CFTC's eight published concentration figures for the latest
/// report: the share of open interest held by the 4 or 8 largest traders on
/// one side, gross or net of each trader's own opposite positions, with the
/// engine's level among the previous 52 reports and its one-week change.
struct CftcConcentrationCell {
    QString field_key;
    int traders = 4;
    bool long_side = true;
    bool net = false;
    bool has_value = false;
    double value = 0.0; ///< % of open interest
    std::optional<double> percentile;
    std::optional<CftcLevelBand> band;
    bool has_change = false;
    double change = 0.0; ///< percentage points since the previous weekly report
    CftcChangeSize size = CftcChangeSize::Unavailable;
    QString tooltip;
};

/// The eight cells in panel order: the 4 then the 8 largest traders, each
/// long gross, long net, short gross, short net. A figure the report does not
/// carry stays absent.
inline QVector<CftcConcentrationCell> cftc_concentration_cells(const services::CftcInterpretationResult& result) {
    QVector<CftcConcentrationCell> out;
    for (int traders : {4, 8}) {
        for (bool long_side : {true, false}) {
            for (bool net : {false, true}) {
                CftcConcentrationCell cell;
                cell.traders = traders;
                cell.long_side = long_side;
                cell.net = net;
                cell.field_key = QStringLiteral("concentration_%1_%2_%3")
                                     .arg(net ? QStringLiteral("net") : QStringLiteral("gross"))
                                     .arg(traders)
                                     .arg(long_side ? QStringLiteral("long") : QStringLiteral("short"));
                for (const auto& a : result.concentration) {
                    if (a.field_key != cell.field_key)
                        continue;
                    cell.has_value = a.has_current;
                    cell.value = a.current;
                    if (a.has_current && a.has_percentile) {
                        cell.percentile = a.percentile;
                        cell.band = cftc_level_band(a.percentile, result.config);
                    }
                    cell.has_change = a.has_change;
                    cell.change = a.change;
                    if (a.has_change)
                        cell.size = cftc_change_size(a.has_change_rank, a.change_rank, result.config);
                    cell.tooltip = cftc_concentration_statement(traders, long_side, net, a, result.config);
                }
                out << cell;
            }
        }
    }
    return out;
}

// ── Single-market series ────────────────────────────────────────────────────

/// A dated value that may be absent (absent points break a line).
struct CftcSeriesPoint {
    QDate date;
    bool has_value = false;
    double value = 0.0;
};

/// Net %OI of one participant at every report (absent where not formable).
inline QVector<CftcSeriesPoint> cftc_net_pct_series(const QVector<services::CftcObservation>& observations,
                                                    int participant_index) {
    QVector<CftcSeriesPoint> out;
    out.reserve(observations.size());
    for (const auto& obs : observations) {
        const std::optional<double> v =
            services::cftc_metric_value(obs, participant_index, services::CftcMetricKind::NetPctOi);
        out.append({obs.date, v.has_value(), v.value_or(0.0)});
    }
    return out;
}

/// The engine's strictly trailing percentile of one participant's Net %OI at
/// every report against the previous `window` valid readings (rule set v3: 52;
/// absent until that many earlier valid readings exist).
inline QVector<CftcSeriesPoint> cftc_percentile_series(const QVector<services::CftcObservation>& observations,
                                                       int participant_index, int window = 52) {
    QVector<CftcSeriesPoint> out;
    out.reserve(observations.size());
    const QVector<services::CftcDatedValue> series =
        services::cftc_metric_series(observations, participant_index, services::CftcMetricKind::NetPctOi);
    // Incremental form of cftc_trailing_percentile_at: the reference is the
    // previous `window` valid readings, the current one excluded.
    int position = 0; // index in `series` of the next valid reading
    for (const auto& obs : observations) {
        CftcSeriesPoint point{obs.date, false, 0.0};
        while (position < series.size() && series[position].date < obs.date)
            ++position;
        if (position < series.size() && series[position].date == obs.date && position >= window) {
            const double current = series[position].value;
            int below = 0;
            int equal = 0;
            for (int k = position - window; k < position; ++k) {
                if (series[k].value < current)
                    ++below;
                else if (series[k].value == current)
                    ++equal;
            }
            point.has_value = true;
            point.value = (below + 0.5 * equal) / static_cast<double>(window);
        }
        out.append(point);
    }
    return out;
}

inline QVector<CftcSeriesPoint> cftc_open_interest_points(const QVector<services::CftcObservation>& observations) {
    QVector<CftcSeriesPoint> out;
    out.reserve(observations.size());
    for (const auto& obs : observations)
        out.append({obs.date, obs.open_interest.has_value(), obs.open_interest.value_or(0.0)});
    return out;
}

inline QVector<CftcSeriesPoint> cftc_concentration_points(const QVector<services::CftcObservation>& observations,
                                                          bool long_side) {
    QVector<CftcSeriesPoint> out;
    out.reserve(observations.size());
    for (const auto& obs : observations) {
        const std::optional<double>& v = long_side ? obs.concentration_gross_4_long : obs.concentration_gross_4_short;
        out.append({obs.date, v.has_value(), v.value_or(0.0)});
    }
    return out;
}

/// One cell of the weekly flow strip: the one-report net change of a
/// participant as a percentage of the previous report's Open Interest (the
/// engine's flow definition), or the Open Interest change in percent.
struct CftcFlowCell {
    QDate date;
    bool has_value = false;
    double value = 0.0;
};

inline QVector<CftcFlowCell> cftc_weekly_flow_cells(const QVector<services::CftcObservation>& observations,
                                                    int participant_index, int count) {
    QVector<CftcFlowCell> out;
    const int start = std::max(0, static_cast<int>(observations.size()) - count);
    for (int i = start; i < observations.size(); ++i) {
        const services::CftcReportFlow flow = services::cftc_report_flow(observations, participant_index, 1, i);
        CftcFlowCell cell{observations[i].date, false, 0.0};
        if (participant_index >= 0 && flow.has_net_flow) {
            cell.has_value = true;
            cell.value = flow.net_flow;
        } else if (participant_index < 0 && flow.has_oi_change) {
            cell.has_value = true;
            cell.value = flow.oi_change;
        }
        out.append(cell);
    }
    return out;
}

/// Historical context of one participant over the full history and the last
/// 52 reports: the range of Net %OI with dates, and how far back one has to go
/// to find a reading beyond today's (for "largest since").
struct CftcHistoricalContext {
    bool available = false;
    double now = 0.0;
    double window_min = 0.0;
    QDate window_min_date;
    double window_max = 0.0;
    QDate window_max_date;
    int window_reports = 0;
    double record_min = 0.0;
    QDate record_min_date;
    double record_max = 0.0;
    QDate record_max_date;
    QDate history_start;
    /// The most recent earlier report with a reading at least as extreme in
    /// today's direction (above today's when today is above the 52-report
    /// median, below otherwise); invalid when none exists in the history.
    QDate beyond_since;
    bool beyond_found = false;
};

inline CftcHistoricalContext cftc_historical_context(const QVector<services::CftcObservation>& observations,
                                                     int participant_index, int window = 52) {
    CftcHistoricalContext out;
    const QVector<services::CftcDatedValue> series =
        services::cftc_metric_series(observations, participant_index, services::CftcMetricKind::NetPctOi);
    if (series.isEmpty() || observations.isEmpty() || series.last().date != observations.last().date)
        return out;
    out.available = true;
    out.now = series.last().value;
    out.history_start = series.first().date;
    out.record_min = out.record_max = series.first().value;
    out.record_min_date = out.record_max_date = series.first().date;
    for (const auto& point : series) {
        if (point.value < out.record_min) {
            out.record_min = point.value;
            out.record_min_date = point.date;
        }
        if (point.value > out.record_max) {
            out.record_max = point.value;
            out.record_max_date = point.date;
        }
    }
    const int first = std::max(0, static_cast<int>(series.size()) - 1 - window);
    out.window_reports = static_cast<int>(series.size()) - first;
    out.window_min = out.window_max = series[first].value;
    out.window_min_date = out.window_max_date = series[first].date;
    QVector<double> reference;
    for (int i = first; i < series.size(); ++i) {
        const auto& point = series[i];
        if (point.value < out.window_min) {
            out.window_min = point.value;
            out.window_min_date = point.date;
        }
        if (point.value > out.window_max) {
            out.window_max = point.value;
            out.window_max_date = point.date;
        }
        if (i < series.size() - 1)
            reference.append(point.value);
    }
    std::sort(reference.begin(), reference.end());
    const double median = reference.isEmpty() ? out.now : reference[reference.size() / 2];
    const bool upward = out.now >= median;
    for (int i = static_cast<int>(series.size()) - 2; i >= 0; --i) {
        if ((upward && series[i].value >= out.now) || (!upward && series[i].value <= out.now)) {
            out.beyond_since = series[i].date;
            out.beyond_found = true;
            break;
        }
    }
    return out;
}

// ── Raw table ───────────────────────────────────────────────────────────────

/// Column name of a concentration field in the raw table: "top-4 gross long %OI".
inline QString cftc_concentration_column(const QString& field_key) {
    // field keys: concentration_<gross|net>_<4|8>_<long|short>
    const QStringList parts = field_key.split(QLatin1Char('_'));
    if (parts.size() != 4)
        return field_key;
    return cftc_text_tr("top-%1 %2 %3 %OI").arg(parts[2], parts[1], parts[3]);
}

/// The market TABLE columns: date, open interest; for every group its long and
/// short contracts, Net %OI and the 52-week percentile of Net %OI; the CFTC's
/// eight concentration figures; reportable traders.
inline QStringList cftc_market_table_columns(services::CftcFamily family) {
    QStringList columns{QStringLiteral("date"), cftc_text_tr("open interest")};
    for (const auto& participant : services::cftc_family_participants(family)) {
        const QString name = cftc_group_name(family, participant.key);
        columns << name + cftc_text_tr(" long") << name + cftc_text_tr(" short") << name + cftc_text_tr(" net %OI")
                << name + cftc_text_tr(" 52-week percentile");
    }
    for (const QString& field : services::cftc_concentration_field_keys())
        columns << cftc_concentration_column(field);
    columns << cftc_text_tr("traders");
    return columns;
}

/// The market TABLE rows, newest report first. The percentile is the engine's
/// strictly trailing 52-report percentile (cftc_percentile_series), in
/// percent; it is absent until 52 earlier readings exist. Absent values are
/// left out of a row, never written as zero.
inline QJsonArray cftc_market_table_rows(const QVector<services::CftcObservation>& observations,
                                         services::CftcFamily family) {
    QJsonArray out;
    const auto participants = services::cftc_family_participants(family);
    QVector<QVector<CftcSeriesPoint>> percentiles;
    for (int p = 0; p < participants.size(); ++p)
        percentiles << cftc_percentile_series(observations, p);
    const QStringList fields = services::cftc_concentration_field_keys();
    for (int r = static_cast<int>(observations.size()) - 1; r >= 0; --r) {
        const services::CftcObservation& obs = observations[r];
        QJsonObject row;
        row[QStringLiteral("date")] = obs.date.toString(Qt::ISODate);
        if (obs.open_interest)
            row[cftc_text_tr("open interest")] = *obs.open_interest;
        for (int p = 0; p < participants.size(); ++p) {
            const QString name = cftc_group_name(family, participants[p].key);
            const services::CftcPositionMetrics m = services::cftc_position_metrics(obs, p);
            if (m.has_long)
                row[name + cftc_text_tr(" long")] = m.long_leg;
            if (m.has_short)
                row[name + cftc_text_tr(" short")] = m.short_leg;
            if (m.has_net_pct_oi)
                row[name + cftc_text_tr(" net %OI")] = std::round(m.net_pct_oi * 100.0) / 100.0;
            const CftcSeriesPoint& percentile = percentiles[p][r];
            if (percentile.has_value)
                row[name + cftc_text_tr(" 52-week percentile")] = std::round(percentile.value * 1000.0) / 10.0;
        }
        for (const QString& field : fields)
            if (const std::optional<double> value = services::cftc_concentration_value(obs, field))
                row[cftc_concentration_column(field)] = *value;
        if (obs.traders_total)
            row[cftc_text_tr("traders")] = *obs.traders_total;
        out.append(row);
    }
    return out;
}

} // namespace fincept::screens
