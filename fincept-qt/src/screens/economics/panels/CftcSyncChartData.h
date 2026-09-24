// src/screens/economics/panels/CftcSyncChartData.h
//
// Batch 4B: the pure data contract behind the synchronized Price + Positioning
// chart. The batch-4B chart component renders this contract; the panel builds
// it from the full official CFTC history and the retained price path.
//
// Truthfulness rules carried here:
//   * price is aligned to official CFTC report dates using the Batch 4A
//     report-price rule (the close on or before the report date, only when it
//     is contemporaneous) — a later price never represents an earlier report;
//   * a report date without a qualifying price simply has no price point, so a
//     gap stays a gap and is never interpolated;
//   * price context that belongs to another market is rejected outright, so a
//     stale series from a previous fetch cannot populate a new market;
//   * the caller's actual price state (pending / concrete failure / ready) is
//     carried into the pane status so the chart never says "source unspecified"
//     merely because the request is still running or failed;
//   * the positioning pane is the report-family principal participant defined
//     by the finalized Batch 4A terminology contract (Legacy Non-Commercial,
//     Disaggregated Managed Money, TFF Leveraged Funds) and never a generic
//     speculative-flag selection; a missing principal participant leaves the
//     pane truthfully unavailable;
//   * both panes snap the crosshair to one shared set of official report dates
//     (the union of the dates the two series carry), so a report date with
//     positioning but no qualifying price stays reachable from either pane
//     while the price gap itself is preserved and never interpolated;
//   * the visible range filters the drawn points only and never the Batch 4A
//     interpretation, which is computed from the full history above.
//
// Header-only over Qt Core so the alignment contract is unit-testable without
// the widget tree (tests/ HARD RULE).
#pragma once

#include "screens/economics/panels/CftcInterpretationPresentation.h"
#include "services/economics/CftcInterpretationModel.h"
#include "services/economics/CftcMetricModel.h"
#include "ui/charts/TimeSeriesData.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QTimeZone>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace fincept::screens {

/// The time zone the retained price provider stamps daily bars in. Yahoo
/// Finance stamps each daily futures bar at 00:00 exchange time: 00:00
/// America/New_York for every mapped CME/COMEX/NYMEX/CBOT/ICE futures proxy and
/// DX-Y.NYB, and 00:00 America/Chicago for ^VIX (verified 2026-09-24). A
/// New York wall-clock date recovers the session date for all of them (Chicago
/// midnight is 01:00 in New York on the same date).
inline QTimeZone cftc_price_session_time_zone() {
    static const QTimeZone zone(QByteArrayLiteral("America/New_York"));
    return zone;
}

/// The trading-session date of a provider daily bar. Converting the epoch
/// timestamp in the viewer's local zone would move a bar stamped 00:00 New York
/// time back to the previous calendar day on any machine at UTC-5 or further
/// west, so the "close on or before the report date" rule would read the
/// session after the report (lookahead). The conversion therefore uses the
/// exchange zone and never the local zone. If the zone database is unavailable
/// the UTC date is used, which equals the session date for bars stamped at a US
/// midnight (04:00-06:00 UTC).
inline QDate cftc_price_session_date(qint64 timestamp_secs) {
    const QTimeZone zone = cftc_price_session_time_zone();
    if (zone.isValid())
        return QDateTime::fromSecsSinceEpoch(timestamp_secs, zone).date();
    return QDateTime::fromSecsSinceEpoch(timestamp_secs, QTimeZone::utc()).date();
}

struct CftcSyncPane {
    QVector<ui::TimeSeriesPoint> points; // real observations only; ascending by report date
    bool available = false;
    QString unavailable_reason;
};

struct CftcSyncChartData {
    CftcSyncPane price;
    CftcSyncPane positioning;
    QString price_source;
    bool price_continuous_proxy = false;
    bool price_spot_index = false;
    QString positioning_label;
    int positioning_palette_index = 0; // series-palette slot for the principal participant
    QStringList horizon_context;
    QDate report_date;
    int horizon_reports = 0; // 0 = the full 1/4/13 context; else the selected horizon only
};

/// The value-axis label format for a positioning/price pane. Contract counts are
/// integers, so a span that cannot need decimals uses plain digits instead of
/// the default six-significant-digit scientific notation (which the axis strip
/// could otherwise render as "..."). A genuinely fractional span keeps a short
/// general format. A fractional-valued pane whose nice-number span reaches 2
/// loses sub-unit axis labels; its exact values remain on the hover readout.
inline QString cftc_sync_value_axis_format(double min_value, double max_value) {
    const double span = max_value - min_value;
    if (std::isfinite(span) && span >= 2.0)
        return QStringLiteral("%.0f");
    return QStringLiteral("%.6g");
}

/// The exact hover readout for one snapped report date, shared by the widget's
/// crosshair tooltip and its unit tests. A pane without a point at the date
/// reports explicit absence, never zero.
struct CftcSyncHoverValue {
    QDate date;
    bool has_price = false;
    double price = 0.0;
    bool has_positioning = false;
    double positioning = 0.0;
};

inline CftcSyncHoverValue cftc_sync_hover_values(const CftcSyncChartData& data, const QDate& date) {
    CftcSyncHoverValue out;
    out.date = date;
    for (const auto& point : data.price.points) {
        if (point.date == date) {
            out.has_price = true;
            out.price = point.value;
            break;
        }
    }
    for (const auto& point : data.positioning.points) {
        if (point.date == date) {
            out.has_positioning = true;
            out.positioning = point.value;
            break;
        }
    }
    return out;
}

/// Shared crosshair snapping: both panes must resolve a hover position to the
/// same official report date. The snap set is the union of the report dates the
/// two series carry, so a report date with positioning but no qualifying price
/// stays selectable from either pane and its truthful "positioning without
/// price" readout remains reachable. The price gaps themselves are preserved;
/// nothing is interpolated to fill them.
inline QVector<QDate> cftc_sync_snap_dates(const CftcSyncChartData& data) {
    QVector<QDate> dates;
    dates.reserve(data.price.points.size() + data.positioning.points.size());
    for (const auto& point : data.price.points) {
        if (point.date.isValid())
            dates.append(point.date);
    }
    for (const auto& point : data.positioning.points) {
        if (point.date.isValid())
            dates.append(point.date);
    }
    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    return dates;
}

/// The official report date both panes snap to for a millisecond axis position:
/// the nearest snap date inside the shared visible window, or an invalid date
/// when the window contains none. Both canvases call this with the same snap
/// set, target and window, so they can never resolve to different dates. Ties
/// resolve to the earlier date for determinism.
inline QDate cftc_sync_nearest_snap_date(const QVector<QDate>& snap_dates, qint64 target_ms, qint64 window_min_ms,
                                         qint64 window_max_ms) {
    QDate best;
    qint64 best_distance = -1;
    for (const QDate& date : snap_dates) {
        const qint64 x = QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch();
        if (x < window_min_ms || x > window_max_ms)
            continue;
        const qint64 distance = x >= target_ms ? x - target_ms : target_ms - x;
        if (best_distance < 0 || distance < best_distance) {
            best_distance = distance;
            best = date;
        }
    }
    return best;
}

/// Price aligned to the official report dates of `window`: one point per report
/// date that has a contemporaneous close on or before it. Reports without such a
/// close carry no point (a truthful gap).
inline CftcSyncPane cftc_align_price_to_report_dates(const QVector<services::CftcObservation>& window,
                                                     const QVector<services::CftcPricePoint>& prices) {
    CftcSyncPane pane;
    pane.points.reserve(window.size());
    for (const auto& observation : window) {
        const services::CftcReportPrice price = services::cftc_report_price(prices, observation.date);
        if (!price.available)
            continue;
        pane.points.append({observation.date, observation.date_label, price.close});
    }
    pane.available = !pane.points.isEmpty();
    return pane;
}

/// Deterministic priority for the horizon context words. The engine's own state
/// ordering is stable; this only groups the reading so a line starts with the
/// net move, then the sustained statement, then the gross legs.
inline int cftc_sync_state_priority(const QString& state_id) {
    if (state_id == QLatin1String("NET_LONGWARD_SHIFT") || state_id == QLatin1String("NET_SHORTWARD_SHIFT"))
        return 0;
    if (state_id.startsWith(QLatin1String("SUSTAINED_")))
        return 1;
    if (state_id == QLatin1String("LONG_ACCUMULATION") || state_id == QLatin1String("LONG_LIQUIDATION") ||
        state_id == QLatin1String("SHORT_BUILDING") || state_id == QLatin1String("SHORT_COVERING"))
        return 2;
    if (state_id == QLatin1String("NET_SHARE_RAW_DISAGREEMENT"))
        return 3;
    return 4;
}

/// The current 1/4/13-report interpretation context for one participant, as
/// chart annotation text. States are read verbatim; no state is inferred. When
/// the participant is absent from the result the list is empty: the chart
/// already reports the unavailable positioning pane, and "no material state"
/// would be a false claim about a participant that was never resolved.
inline QStringList cftc_sync_horizon_context(const services::CftcInterpretationResult& interpretation,
                                             const QString& participant_key) {
    const services::CftcParticipantInterpretation* participant = nullptr;
    for (const auto& candidate : interpretation.participants) {
        if (candidate.participant_key == participant_key) {
            participant = &candidate;
            break;
        }
    }
    if (!participant)
        return {};
    QStringList lines;
    for (int horizon : {1, 4, 13}) {
        QStringList words;
        if (participant) {
            QVector<const services::CftcInterpretationState*> states;
            for (const auto& state : participant->states) {
                if (state.has_horizon && state.horizon_reports == horizon)
                    states.append(&state);
            }
            std::stable_sort(states.begin(), states.end(), [](const auto* a, const auto* b) {
                const int pa = cftc_sync_state_priority(a->state_id);
                const int pb = cftc_sync_state_priority(b->state_id);
                if (pa != pb)
                    return pa < pb;
                return a->state_id < b->state_id;
            });
            for (const auto* state : states) {
                const QString wording = cftc_state_short_wording(state->state_id);
                if (!wording.isEmpty())
                    words << wording;
            }
        }
        QString line;
        if (!words.isEmpty()) {
            line = cftc_presentation_tr("%1: %2").arg(cftc_horizon_phrase(horizon), words.join(QStringLiteral(", ")));
        } else {
            const services::CftcUnavailableRecord* record = cftc_presentation_unavailable(
                interpretation.unavailable, QStringLiteral("NET_SHIFT"), participant_key, horizon);
            if (record) {
                line = cftc_presentation_tr("%1: unavailable — %2")
                           .arg(cftc_horizon_phrase(horizon), cftc_unavailable_reason_wording(record->reason));
            } else {
                line = cftc_presentation_tr("%1: no material state").arg(cftc_horizon_phrase(horizon));
            }
        }
        lines << line;
    }
    return lines;
}

/// The current interpretation context line for one selected horizon (the same
/// wording the panel's 1W | 4W | 13W selection uses). Returns an empty string
/// for an unsupported horizon or a participant absent from the result.
inline QString cftc_sync_horizon_context_line(const services::CftcInterpretationResult& interpretation,
                                              const QString& participant_key, int horizon_reports) {
    if (!cftc_is_interpretation_horizon(horizon_reports))
        return {};
    const services::CftcParticipantInterpretation* participant = nullptr;
    for (const auto& candidate : interpretation.participants) {
        if (candidate.participant_key == participant_key) {
            participant = &candidate;
            break;
        }
    }
    if (!participant)
        return {};
    QStringList words;
    QVector<const services::CftcInterpretationState*> states;
    for (const auto& state : participant->states) {
        if (state.has_horizon && state.horizon_reports == horizon_reports)
            states.append(&state);
    }
    std::stable_sort(states.begin(), states.end(), [](const auto* a, const auto* b) {
        const int pa = cftc_sync_state_priority(a->state_id);
        const int pb = cftc_sync_state_priority(b->state_id);
        if (pa != pb)
            return pa < pb;
        return a->state_id < b->state_id;
    });
    for (const auto* state : states) {
        const QString wording = cftc_state_short_wording(state->state_id);
        if (!wording.isEmpty())
            words << wording;
    }
    if (!words.isEmpty())
        return cftc_presentation_tr("%1: %2").arg(cftc_horizon_phrase(horizon_reports),
                                                  words.join(QStringLiteral(", ")));
    if (const services::CftcUnavailableRecord* record = cftc_presentation_unavailable(
            interpretation.unavailable, QStringLiteral("NET_SHIFT"), participant_key, horizon_reports)) {
        return cftc_presentation_tr("%1: unavailable — %2")
            .arg(cftc_horizon_phrase(horizon_reports), cftc_unavailable_reason_wording(record->reason));
    }
    return cftc_presentation_tr("%1: no material state").arg(cftc_horizon_phrase(horizon_reports));
}

/// Build the complete synchronized chart contract. `requested_market_key` is the
/// market the panel is displaying; `price_market_key` identifies the market the
/// retained price points were fetched for. A mismatch yields an unavailable
/// price pane rather than another market's series. `price_state` and
/// `price_unavailable_reason` are the caller's actual price situation, so a
/// pending or failed request is never described as an unspecified source.
/// `horizon_reports` narrows the caption to the selected interpretation horizon
/// (0 keeps the combined 1/4/13 lines).
inline CftcSyncChartData cftc_build_sync_chart_data(
    const services::CftcInterpretationResult& interpretation, const QVector<services::CftcObservation>& full_history,
    const QVector<services::CftcPricePoint>& prices, const QString& requested_market_key,
    const QString& price_market_key, services::CftcRange range, CftcPriceContextState price_state,
    const QString& price_unavailable_reason, const QString& price_source, bool price_continuous_proxy,
    bool price_spot_index, int horizon_reports = 0) {
    CftcSyncChartData data;
    data.price_source = price_source;
    data.price_continuous_proxy = price_continuous_proxy;
    data.price_spot_index = price_spot_index;
    data.horizon_reports = cftc_is_interpretation_horizon(horizon_reports) ? horizon_reports : 0;
    data.price.available = false;
    data.positioning.available = false;

    const QVector<services::CftcObservation> window = services::cftc_filter_range(full_history, range);
    if (!window.isEmpty()) {
        data.report_date = window.last().date;

        // The principal participant comes from the finalized Batch 4A
        // terminology contract; the observation slot index is its position in
        // the family participant vector.
        const QString primary_key = cftc_principal_participant_key(interpretation.family);
        int primary_index = -1;
        const QVector<services::CftcParticipant> family_participants =
            services::cftc_family_participants(interpretation.family);
        for (int i = 0; i < family_participants.size(); ++i) {
            if (family_participants[i].key == primary_key) {
                primary_index = i;
                break;
            }
        }
        bool participant_label_resolved = false;
        for (const auto& participant : interpretation.participants) {
            if (!primary_key.isEmpty() && participant.participant_key == primary_key) {
                data.positioning_label = cftc_participant_display_name(participant);
                participant_label_resolved = true;
                break;
            }
        }

        if (primary_key.isEmpty() || primary_index < 0) {
            data.positioning.unavailable_reason =
                cftc_presentation_tr("no report-family principal participant is defined");
        } else if (!participant_label_resolved) {
            data.positioning.unavailable_reason =
                cftc_presentation_tr("the principal participant is not present in the interpretation result");
        } else {
            data.positioning_palette_index = primary_index;
            const QVector<services::CftcDatedValue> net_series =
                services::cftc_metric_series(window, primary_index, services::CftcMetricKind::Net);
            data.positioning.points.reserve(net_series.size());
            for (const auto& point : net_series)
                data.positioning.points.append({point.date, point.date_label, point.value});
            data.positioning.available = !data.positioning.points.isEmpty();
            if (!data.positioning.available)
                data.positioning.unavailable_reason =
                    cftc_presentation_tr("the principal participant's net position is not carried in this range");
        }

        if (data.horizon_reports == 0) {
            data.horizon_context = cftc_sync_horizon_context(interpretation, primary_key);
        } else if (const QString line =
                       cftc_sync_horizon_context_line(interpretation, primary_key, data.horizon_reports);
                   !line.isEmpty()) {
            data.horizon_context = {line};
        }
    }

    if (price_state == CftcPriceContextState::Pending) {
        data.price.unavailable_reason = cftc_presentation_tr("price context is pending");
    } else if (requested_market_key.isEmpty()) {
        data.price.unavailable_reason = cftc_presentation_tr("no market is selected");
    } else if (requested_market_key != price_market_key) {
        data.price.unavailable_reason =
            cftc_presentation_tr("the available price context belongs to a different market");
    } else if (price_state == CftcPriceContextState::Unavailable) {
        data.price.unavailable_reason =
            price_unavailable_reason.trimmed().isEmpty()
                ? cftc_presentation_tr("no retained free price source is mapped for this market")
                : price_unavailable_reason;
    } else if (price_source.trimmed().isEmpty()) {
        data.price.unavailable_reason = cftc_presentation_tr("the price source is unspecified");
    } else if (prices.isEmpty()) {
        data.price.unavailable_reason = cftc_presentation_tr("no price observations were supplied");
    } else if (!window.isEmpty()) {
        data.price = cftc_align_price_to_report_dates(window, prices);
        if (!data.price.available)
            data.price.unavailable_reason =
                cftc_presentation_tr("no price close is available on or before the report dates in this range");
    }
    return data;
}

} // namespace fincept::screens
