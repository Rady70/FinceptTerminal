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
//   * the positioning pane is the report-family-appropriate primary
//     participant's reported net position in contracts;
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

#include <QDate>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace fincept::screens {

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
    int positioning_palette_index = 0; // series-palette slot for the primary participant
    QStringList horizon_context;
    QDate report_date;
};

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
/// chart annotation text. States are read verbatim; no state is inferred.
inline QStringList cftc_sync_horizon_context(const services::CftcInterpretationResult& interpretation,
                                             const QString& participant_key) {
    const services::CftcParticipantInterpretation* participant = nullptr;
    for (const auto& candidate : interpretation.participants) {
        if (candidate.participant_key == participant_key) {
            participant = &candidate;
            break;
        }
    }
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

/// Build the complete synchronized chart contract. `requested_market_key` is the
/// market the panel is displaying; `price_market_key` identifies the market the
/// retained price points were fetched for. A mismatch yields an unavailable
/// price pane rather than another market's series.
inline CftcSyncChartData cftc_build_sync_chart_data(const services::CftcInterpretationResult& interpretation,
                                                    const QVector<services::CftcObservation>& full_history,
                                                    const QVector<services::CftcPricePoint>& prices,
                                                    const QString& requested_market_key,
                                                    const QString& price_market_key, services::CftcRange range,
                                                    const QString& price_source, bool price_continuous_proxy,
                                                    bool price_spot_index) {
    CftcSyncChartData data;
    data.price_source = price_source;
    data.price_continuous_proxy = price_continuous_proxy;
    data.price_spot_index = price_spot_index;
    data.price.available = false;
    data.positioning.available = false;

    const QVector<services::CftcObservation> window = services::cftc_filter_range(full_history, range);
    if (!window.isEmpty()) {
        data.report_date = window.last().date;

        int primary_index = -1;
        QString primary_key;
        const QVector<services::CftcParticipant> family_participants =
            services::cftc_family_participants(interpretation.family);
        const int speculative = services::cftc_speculative_index(family_participants);
        if (speculative >= 0) {
            primary_key = family_participants[speculative].key;
            primary_index = speculative;
            data.positioning_palette_index = speculative;
        }
        for (const auto& participant : interpretation.participants) {
            if (participant.participant_key == primary_key) {
                data.positioning_label = cftc_participant_display_name(participant);
                break;
            }
        }
        if (data.positioning_label.isEmpty())
            data.positioning_label = cftc_presentation_tr("Primary participant");

        if (primary_index >= 0) {
            const QVector<services::CftcDatedValue> net_series =
                services::cftc_metric_series(window, primary_index, services::CftcMetricKind::Net);
            data.positioning.points.reserve(net_series.size());
            for (const auto& point : net_series)
                data.positioning.points.append({point.date, point.date_label, point.value});
            data.positioning.available = !data.positioning.points.isEmpty();
            if (!data.positioning.available)
                data.positioning.unavailable_reason =
                    cftc_presentation_tr("the primary participant's net position is not carried in this range");
        } else {
            data.positioning.unavailable_reason =
                cftc_presentation_tr("no report-family principal participant is defined");
        }

        data.horizon_context = cftc_sync_horizon_context(interpretation, primary_key);
    }

    if (price_source.trimmed().isEmpty()) {
        data.price.unavailable_reason = cftc_presentation_tr("the price source is unspecified");
    } else if (requested_market_key.isEmpty()) {
        data.price.unavailable_reason = cftc_presentation_tr("no market is selected");
    } else if (requested_market_key != price_market_key) {
        data.price.unavailable_reason =
            cftc_presentation_tr("the available price context belongs to a different market");
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
