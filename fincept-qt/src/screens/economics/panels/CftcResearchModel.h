// src/screens/economics/panels/CftcResearchModel.h
//
// Presentation projections for the CFTC research workspace: the cross-market
// overview rows, its key figures and notable developments, and the series a
// single-market page draws. Every number is read from the interpretation
// engine's result or computed with the engine's own helpers
// (cftc_trailing_percentile_at, cftc_report_flow, cftc_position_metrics); no
// threshold or state is defined here.
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

    QVector<CftcMatrixCell> cells; ///< every participant, family order

    bool has_open_interest = false;
    double open_interest = 0.0;
    std::optional<double> oi_change_4r;
    std::optional<double> oi_rank_4r;
    std::optional<double> concentration_long; ///< top-4 gross long, % of OI
    std::optional<double> concentration_long_percentile;

    QStringList tags; ///< compact development labels ("EXTREME", "TURNING", "LARGE MOVE")
};

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
        if (const services::CftcParticipantTrail* trail = detail::entry_trail(entry, p.participant_key)) {
            row.percentile_1r = trail->percentile_1r;
            row.percentile_4r = trail->percentile_4r;
            row.percentile_13r = trail->percentile_13r;
            row.tail_run = trail->tail_run_reports;
        }
    }
    row.has_open_interest = result.open_interest_available;
    row.open_interest = result.open_interest;
    for (const auto& reading : result.open_interest_readings) {
        if (reading.horizon_reports != 4 || !reading.evaluated)
            continue;
        if (reading.has_oi_change)
            row.oi_change_4r = reading.oi_change;
        if (reading.has_rank)
            row.oi_rank_4r = reading.rank;
    }
    for (const auto& assessment : result.concentration) {
        if (assessment.field_key != QLatin1String("concentration_gross_4_long"))
            continue;
        if (assessment.has_current)
            row.concentration_long = assessment.current;
        if (assessment.has_percentile)
            row.concentration_long_percentile = assessment.percentile;
    }
    // Compact labels for the map/matrix rows (the developments list carries
    // the full wording).
    bool any_tail = false;
    bool turning = false;
    bool large = false;
    for (const auto& p : result.participants) {
        if (p.historical_percentile_available && cftc_level_tail(p.percentile, config) != CftcLevelTail::None)
            any_tail = true;
        if (detail::has_state(p, "EXITED_HIGH_EXTREME") || detail::has_state(p, "EXITED_LOW_EXTREME") ||
            detail::has_state(p, "UNWINDING_HIGH_EXTREME") || detail::has_state(p, "UNWINDING_LOW_EXTREME"))
            turning = true;
        for (const auto& reading : p.flow_readings)
            if (reading.horizon_reports == 1 && reading.evaluated && reading.has_net_rank &&
                reading.net_rank >= config.large_move_percentile && p.participant_key == row.principal_key)
                large = true;
    }
    if (row.tail != CftcLevelTail::None)
        row.tags << cftc_text_tr("EXTREME");
    else if (any_tail)
        row.tags << cftc_text_tr("GROUP EXTREME");
    if (turning)
        row.tags << cftc_text_tr("TURNING");
    if (large)
        row.tags << cftc_text_tr("LARGE MOVE");
    return row;
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
    return rows;
}

// ── Key figures ─────────────────────────────────────────────────────────────

struct CftcOverviewFigures {
    QDate latest_report;     ///< the most common latest report date among current markets
    int markets_listed = 0;  ///< markets the selected report covers
    int markets_current = 0; ///< with the latest report
    int markets_stored = 0;  ///< stored data only (refresh failed / not run)
    int markets_old = 0;     ///< latest report older than the freshness limit
    int markets_unavailable = 0;
    int markets_not_covered = 0;
    QStringList extreme_markets;    ///< principal group in a 10% tail
    QStringList turning_markets;    ///< any group moved out of / back from a tail
    QStringList large_move_markets; ///< principal one-week change >= 90% of comparable changes
    int oi_up_notable = 0;          ///< 4-week Open Interest change >= 75% of comparable changes, rising
    int oi_down_notable = 0;
};

inline CftcOverviewFigures cftc_overview_figures(const QVector<CftcMarketRow>& rows) {
    CftcOverviewFigures figures;
    QHash<QDate, int> dates;
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
        if (row.report_date.isValid() && row.availability != CftcRowAvailability::Old)
            dates[row.report_date] += 1;
        if (row.tail != CftcLevelTail::None)
            figures.extreme_markets << row.label;
        if (row.tags.contains(cftc_text_tr("TURNING")))
            figures.turning_markets << row.label;
        if (row.tags.contains(cftc_text_tr("LARGE MOVE")))
            figures.large_move_markets << row.label;
        if (row.oi_change_4r && row.oi_rank_4r && *row.oi_rank_4r >= 0.75)
            (*row.oi_change_4r >= 0.0 ? figures.oi_up_notable : figures.oi_down_notable) += 1;
    }
    int best = 0;
    for (auto it = dates.constBegin(); it != dates.constEnd(); ++it) {
        if (it.value() > best || (it.value() == best && it.key() > figures.latest_report)) {
            best = it.value();
            figures.latest_report = it.key();
        }
    }
    return figures;
}

// ── Notable developments ────────────────────────────────────────────────────

enum class CftcDevelopmentKind { Extreme, Turning, LargeMove, MarketStructure };

struct CftcDevelopment {
    CftcDevelopmentKind kind = CftcDevelopmentKind::Extreme;
    int entry_index = -1;
    QString market_key;
    QString market_label;
    QString participant_key; ///< empty for market-level items
    QString text;            ///< plain statement without the market name
    // Extreme items also carry their figures, for the overview's table.
    double net_pct_oi = 0.0;
    double percentile = 0.0;
    CftcLevelTail tail = CftcLevelTail::None;
    int tail_run = 0; ///< consecutive weekly reports in the tail (0 = not known)
    bool tail_run_capped = false;
};

/// The developments that deserve attention across the scanned markets, from
/// the engine's own states and ranks only:
///   * Extreme — any participant group in its top or bottom 10% (one line per
///     group, with the percentile and how long it has been there);
///   * Turning — a group moved out of a tail, or back from a recent extreme;
///   * LargeMove — a one-week net change, or failing that a four-week one,
///     bigger than 90% of comparable changes (the engine's "large" rank);
///   * MarketStructure — a large four-week Open Interest change, or a market
///     concentration in the top 10% of its history.
/// Ordinary 75th-percentile moves are not listed here: across three
/// horizons and several groups almost every market has one, so they stay in
/// the market's own page.
inline QVector<CftcDevelopment> cftc_overview_developments(const QVector<services::CftcMonitorEntry>& entries,
                                                           const QVector<CftcMarketRow>& rows) {
    QVector<CftcDevelopment> out;
    for (const CftcMarketRow& row : rows) {
        if (row.availability == CftcRowAvailability::NotCovered || row.availability == CftcRowAvailability::Unavailable)
            continue;
        const services::CftcMonitorEntry& entry = entries.at(row.entry_index);
        const services::CftcInterpretationResult& result = entry.result;
        const services::CftcInterpretationConfig& config = result.config;
        auto add = [&](CftcDevelopmentKind kind, const QString& participant, const QString& text) {
            if (text.isEmpty())
                return;
            out.append({kind, row.entry_index, row.market_key, row.label, participant, text});
        };
        // Principal first, then the other groups in family order.
        QVector<const services::CftcParticipantInterpretation*> ordered;
        for (const auto& p : result.participants)
            if (p.participant_key == row.principal_key)
                ordered.prepend(&p);
            else
                ordered.append(&p);
        for (const auto* p : ordered) {
            if (!p->has_net_pct_oi || !p->historical_percentile_available)
                continue;
            const CftcLevelTail tail = cftc_level_tail(p->percentile, config);
            if (tail == CftcLevelTail::None)
                continue;
            const services::CftcParticipantTrail* trail = detail::entry_trail(entry, p->participant_key);
            QString text =
                cftc_text_tr("%1: %2 (%3 of open interest), %4 over 3 years")
                    .arg(cftc_group_name(entry.family, p->participant_key), cftc_tail_phrase(tail, p->net_pct_oi),
                         cftc_abs_pct(p->net_pct_oi), cftc_percentile_ordinal(p->percentile));
            if (trail && trail->tail_run_reports >= 2)
                text += cftc_text_tr(", %1%2 weeks running")
                            .arg(trail->tail_run_capped ? cftc_text_tr("at least ") : QString())
                            .arg(trail->tail_run_reports);
            else if (trail && trail->tail_run_reports == 1)
                text += cftc_text_tr(", new this week");
            add(CftcDevelopmentKind::Extreme, p->participant_key, text);
            CftcDevelopment& item = out.last();
            item.net_pct_oi = p->net_pct_oi;
            item.percentile = p->percentile;
            item.tail = tail;
            item.tail_run = trail ? trail->tail_run_reports : 0;
            item.tail_run_capped = trail && trail->tail_run_capped;
        }
        for (const auto* p : ordered) {
            const QString transition =
                cftc_transition_statement(entry.family, *p, detail::entry_trail(entry, p->participant_key));
            add(CftcDevelopmentKind::Turning, p->participant_key, transition);
        }
        for (const auto* p : ordered) {
            for (int horizon : {1, 4}) {
                const services::CftcHorizonFlowReading* reading = nullptr;
                for (const auto& r : p->flow_readings)
                    if (r.horizon_reports == horizon)
                        reading = &r;
                if (!reading || !reading->evaluated || !reading->has_net_rank ||
                    reading->net_rank < config.large_move_percentile)
                    continue;
                const services::CftcParticipantTrail* trail = detail::entry_trail(entry, p->participant_key);
                const std::optional<double> before = detail::net_pct_ago(trail, horizon);
                add(CftcDevelopmentKind::LargeMove, p->participant_key,
                    cftc_text_tr("%1: %2 %3 (%4)")
                        .arg(cftc_group_name(entry.family, p->participant_key),
                             cftc_net_change_phrase(reading->net_flow, before, p->net_pct_oi), cftc_weeks_text(horizon),
                             cftc_rank_comparison(cftc_text_tr("bigger"), reading->net_rank,
                                                  cftc_text_tr("comparable changes"))));
                break; // one horizon per group: the shorter large move
            }
        }
        for (const auto& reading : result.open_interest_readings) {
            if (reading.horizon_reports != 4 || !reading.evaluated || !reading.has_rank || !reading.has_oi_change ||
                reading.rank < config.large_move_percentile)
                continue;
            add(CftcDevelopmentKind::MarketStructure, QString(),
                cftc_text_tr("Open interest %1 %2 %3 (%4)")
                    .arg(reading.oi_change >= 0.0 ? cftc_text_tr("up") : cftc_text_tr("down"),
                         cftc_abs_pct(reading.oi_change), cftc_weeks_text(4),
                         cftc_rank_comparison(cftc_text_tr("bigger"), reading.rank,
                                              cftc_text_tr("comparable changes"))));
        }
        for (const auto& state : result.market_context) {
            if (state.state_id != QLatin1String("HIGH_MARKET_CONCENTRATION"))
                continue;
            if (row.concentration_long && row.concentration_long_percentile)
                add(CftcDevelopmentKind::MarketStructure, QString(),
                    cftc_text_tr("Largest 4 long traders: %1 of open interest, %2")
                        .arg(cftc_abs_pct(*row.concentration_long),
                             cftc_rank_comparison(cftc_text_tr("higher"), *row.concentration_long_percentile,
                                                  cftc_text_tr("earlier reports for this market"))));
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

/// The engine's strictly trailing 156-report percentile of one participant's
/// Net %OI at every report (absent until 156 earlier valid readings exist).
inline QVector<CftcSeriesPoint> cftc_percentile_series(const QVector<services::CftcObservation>& observations,
                                                       int participant_index, int window = 156) {
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
/// 156 reports: the range of Net %OI with dates, and how far back one has to
/// go to find a reading beyond today's (for "largest since").
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
    /// today's direction (above today's when today is above the 156-report
    /// median, below otherwise); invalid when none exists in the history.
    QDate beyond_since;
    bool beyond_found = false;
};

inline CftcHistoricalContext cftc_historical_context(const QVector<services::CftcObservation>& observations,
                                                     int participant_index, int window = 156) {
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

} // namespace fincept::screens
