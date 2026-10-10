// src/screens/economics/panels/CftcResearchText.h
//
// Plain-language conclusions for the CFTC research workspace.
//
// This layer turns the frozen interpretation engine's output
// (CftcInterpretationResult, rule set cftc-descriptive-interpretation-v2) into
// short statements a reader can check: every statement carries the reading it
// rests on and the historical comparison behind it. It never classifies
// anything itself: levels, tails and move sizes are read from the engine's
// own percentiles and ranks, and the thresholds are the engine's (10% / 90%
// tails, 75% notable and 90% large moves).
//
// Rules the wording follows:
//   * numbers first: "net long 38.2% of open interest, higher than 96% of the
//     previous 156 weekly reports" instead of a label such as "crowded";
//   * every participant group whose positioning is at a historical tail is
//     described, not only the headline group, because the categories of one
//     report are often the more informative side of a market;
//   * direction words follow the actual sign: a net-short group whose net
//     position rises is "net short shrank", never "bought";
//   * a limitation is stated once, briefly, in `not_assessed`;
//   * no forecast, recommendation, signal or trading vocabulary.
//
// Header-only over Qt Core so the wording contract is unit-testable without
// the widget tree.
#pragma once

#include "screens/economics/panels/CftcInterpretationPresentation.h"
#include "services/economics/CftcInterpretationModel.h"
#include "services/economics/CftcMonitorModel.h"

#include <QCoreApplication>
#include <QDate>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fincept::screens {

inline QString cftc_text_tr(const char* text) {
    return QCoreApplication::translate("CftcResearch", text);
}

// ── Names ───────────────────────────────────────────────────────────────────

/// The CFTC category name for one participant key of one family. Names follow
/// the CFTC report headings; TFF categories are never renamed into the Legacy
/// commercial/non-commercial split.
inline QString cftc_group_name(services::CftcFamily family, const QString& key) {
    using services::CftcFamily;
    if (key == QLatin1String("commercial"))
        return cftc_text_tr("Commercial");
    if (key == QLatin1String("non_commercial"))
        return cftc_text_tr("Non-Commercial");
    if (key == QLatin1String("non_reportable"))
        return cftc_text_tr("Non-Reportable");
    if (key == QLatin1String("producer_merchant"))
        return cftc_text_tr("Producer/Merchant");
    if (key == QLatin1String("swap_dealer"))
        return cftc_text_tr("Swap Dealers");
    if (key == QLatin1String("managed_money"))
        return cftc_text_tr("Managed Money");
    if (key == QLatin1String("other_reportable"))
        return cftc_text_tr("Other Reportables");
    if (key == QLatin1String("dealer"))
        return cftc_text_tr("Dealers");
    if (key == QLatin1String("asset_manager"))
        return cftc_text_tr("Asset Managers");
    if (key == QLatin1String("leveraged_funds"))
        return cftc_text_tr("Leveraged Funds");
    Q_UNUSED(family);
    return key;
}

/// Compact column heading for the participant matrix.
inline QString cftc_group_short_name(const QString& key) {
    if (key == QLatin1String("commercial"))
        return cftc_text_tr("COMMERCIAL");
    if (key == QLatin1String("non_commercial"))
        return cftc_text_tr("NON-COMM.");
    if (key == QLatin1String("non_reportable"))
        return cftc_text_tr("NON-REPT.");
    if (key == QLatin1String("producer_merchant"))
        return cftc_text_tr("PROD/MERCH");
    if (key == QLatin1String("swap_dealer"))
        return cftc_text_tr("SWAP DLR");
    if (key == QLatin1String("managed_money"))
        return cftc_text_tr("MANAGED $");
    if (key == QLatin1String("other_reportable"))
        return cftc_text_tr("OTHER RPT");
    if (key == QLatin1String("dealer"))
        return cftc_text_tr("DEALERS");
    if (key == QLatin1String("asset_manager"))
        return cftc_text_tr("ASSET MGR");
    if (key == QLatin1String("leveraged_funds"))
        return cftc_text_tr("LEV. FUNDS");
    return key.toUpper();
}

inline QString cftc_family_name(services::CftcFamily family) {
    switch (family) {
        case services::CftcFamily::Disaggregated:
            return cftc_text_tr("Disaggregated");
        case services::CftcFamily::Tff:
            return cftc_text_tr("Traders in Financial Futures");
        case services::CftcFamily::Legacy:
            break;
    }
    return cftc_text_tr("Legacy");
}

// ── Numbers ─────────────────────────────────────────────────────────────────

/// "+2.4%" / "-0.5%" / "0.0%".
inline QString cftc_signed_pct(double value, int decimals = 1) {
    const double rounded = std::round(value * std::pow(10.0, decimals)) / std::pow(10.0, decimals);
    const QString text = QString::number(std::fabs(rounded), 'f', decimals) + QLatin1Char('%');
    if (rounded > 0.0)
        return QStringLiteral("+") + text;
    if (rounded < 0.0)
        return QStringLiteral("-") + text;
    return text;
}

inline QString cftc_abs_pct(double value, int decimals = 1) {
    return QString::number(std::fabs(value), 'f', decimals) + QLatin1Char('%');
}

/// Whole contracts with thousands separators ("409,899").
inline QString cftc_contracts(double value) {
    return QLocale(QLocale::English).toString(static_cast<qlonglong>(std::llround(value)));
}

/// A share in percent for a 0-1 rank, never rounded onto the bounds it does
/// not reach ("more than 99%" rather than "100%").
inline QString cftc_share_text(double fraction) {
    const double pct = fraction * 100.0;
    if (pct >= 99.5 && fraction < 1.0)
        return cftc_text_tr("more than 99%");
    if (pct < 0.5 && fraction > 0.0)
        return cftc_text_tr("less than 1%");
    return QString::number(std::lround(pct)) + QLatin1Char('%');
}

/// "bigger than 96% of comparable changes", for ranks of at least one half.
/// A midpoint-tie rank of 1 means every reference value is smaller, which
/// reads "bigger than all comparable changes", never "100%"; a rank that
/// would round to 100% reads "nearly all".
inline QString cftc_rank_comparison(const QString& comparative, double rank, const QString& reference) {
    if (rank >= 1.0)
        return cftc_text_tr("%1 than all %2").arg(comparative, reference);
    if (rank >= 0.995)
        return cftc_text_tr("%1 than nearly all %2").arg(comparative, reference);
    return cftc_text_tr("%1 than %2 of %3").arg(comparative, cftc_share_text(rank), reference);
}

/// "higher than 80% of …" at or above the middle, "lower than 80% of …"
/// below it.
inline QString cftc_rank_position(double rank, const QString& reference) {
    if (rank >= 0.5)
        return cftc_rank_comparison(cftc_text_tr("higher"), rank, reference);
    return cftc_rank_comparison(cftc_text_tr("lower"), 1.0 - rank, reference);
}

/// "higher than 96% of the previous 156 weekly reports". The percentile is
/// the engine's midpoint-tie rank against its strictly trailing reference.
inline QString cftc_level_rank_text(double percentile, int reference_count) {
    const QString reference = cftc_text_tr("the previous %1 weekly reports").arg(reference_count);
    if (percentile >= 1.0)
        return cftc_text_tr("higher than every one of %1").arg(reference);
    if (percentile <= 0.0)
        return cftc_text_tr("lower than every one of %1").arg(reference);
    if (percentile >= 0.995)
        return cftc_text_tr("higher than nearly every one of %1").arg(reference);
    if (percentile <= 0.005)
        return cftc_text_tr("lower than nearly every one of %1").arg(reference);
    if (percentile >= 0.5)
        return cftc_text_tr("higher than %1 of %2").arg(cftc_share_text(percentile), reference);
    return cftc_text_tr("lower than %1 of %2").arg(cftc_share_text(1.0 - percentile), reference);
}

/// Percentile as a short ordinal-free reading for tables and headlines: "96%".
inline QString cftc_percentile_short(double percentile) {
    return cftc_share_text(std::clamp(percentile, 0.0, 1.0));
}

/// "96th percentile", with "the highest"/"the lowest" at the bounds (the
/// reading is above or below every reading it is compared with). Within one
/// point of the 10% and 90% tail limits the value keeps a decimal ("10.3
/// percentile"), so a reading just outside a tail never rounds onto its limit.
inline QString cftc_percentile_ordinal(double percentile) {
    if (percentile >= 1.0)
        return cftc_text_tr("the highest");
    if (percentile <= 0.0)
        return cftc_text_tr("the lowest");
    const double value = percentile * 100.0;
    if ((std::fabs(value - 10.0) < 1.0 || std::fabs(value - 90.0) < 1.0) && std::fabs(value - std::round(value)) > 0.05)
        return cftc_text_tr("%1 percentile").arg(QString::number(value, 'f', 1));
    long n = std::lround(value);
    n = std::clamp<long>(n, 1, 99);
    const long tens = n % 100;
    QString suffix = QStringLiteral("th");
    if (tens < 11 || tens > 13) {
        if (n % 10 == 1)
            suffix = QStringLiteral("st");
        else if (n % 10 == 2)
            suffix = QStringLiteral("nd");
        else if (n % 10 == 3)
            suffix = QStringLiteral("rd");
    }
    return cftc_text_tr("%1 percentile").arg(QString::number(n) + suffix);
}

inline QString cftc_weeks_text(int horizon) {
    if (horizon == 1)
        return cftc_text_tr("this week");
    return cftc_text_tr("over %1 weeks").arg(horizon);
}

/// Size of a move from its engine rank against the previous comparable moves
/// of the same length: large (>= 0.90), notable (>= 0.75), ordinary, small.
inline QString cftc_move_size_text(double rank, int horizon) {
    const QString comparable = horizon == 1 ? cftc_text_tr("comparable one-week changes")
                                            : cftc_text_tr("comparable %1-week changes").arg(horizon);
    const QString bigger = cftc_rank_comparison(cftc_text_tr("bigger"), rank, comparable);
    if (rank >= 0.90)
        return cftc_text_tr("a large change, %1").arg(bigger);
    if (rank >= 0.75)
        return cftc_text_tr("a notable change, %1").arg(bigger);
    if (rank >= 0.5)
        return cftc_text_tr("an ordinary change, %1").arg(bigger);
    return cftc_text_tr("a small change, %1")
        .arg(cftc_rank_comparison(cftc_text_tr("smaller"), 1.0 - rank, comparable));
}

// ── Level ───────────────────────────────────────────────────────────────────

enum class CftcLevelTail { None, High, Low };

inline CftcLevelTail cftc_level_tail(double percentile, const services::CftcInterpretationConfig& config) {
    if (percentile >= config.extreme_percentile)
        return CftcLevelTail::High;
    if (percentile <= config.low_extreme_percentile)
        return CftcLevelTail::Low;
    return CftcLevelTail::None;
}

/// "unusually large" / "unusually small" / "within its usual range" for the
/// net position, following its sign: the top tail of a net-short group's
/// Net %OI is its smallest net short, the bottom tail its largest.
inline QString cftc_tail_adjective(CftcLevelTail tail, double net) {
    const bool long_side = net > 0.0;
    if (tail == CftcLevelTail::High)
        return long_side ? cftc_text_tr("unusually large") : cftc_text_tr("unusually small");
    if (tail == CftcLevelTail::Low)
        return long_side ? cftc_text_tr("unusually small") : cftc_text_tr("unusually large");
    return cftc_text_tr("within its usual range");
}

/// "unusually large net long" etc. (no article), for headlines and lists.
inline QString cftc_tail_phrase(CftcLevelTail tail, double net) {
    if (net == 0.0)
        return cftc_text_tr("flat net position");
    const QString side = net > 0.0 ? cftc_text_tr("net long") : cftc_text_tr("net short");
    if (tail == CftcLevelTail::None)
        return side;
    return cftc_tail_adjective(tail, net) + QLatin1Char(' ') + side;
}

/// "net long 38.2% of open interest".
inline QString cftc_net_position_text(double net_pct_oi) {
    if (net_pct_oi > 0.0)
        return cftc_text_tr("net long %1 of open interest").arg(cftc_abs_pct(net_pct_oi));
    if (net_pct_oi < 0.0)
        return cftc_text_tr("net short %1 of open interest").arg(cftc_abs_pct(net_pct_oi));
    return cftc_text_tr("flat (long equals short)");
}

// ── Composer ────────────────────────────────────────────────────────────────

/// What the conclusion card renders.
struct CftcConclusion {
    QString headline;
    QStringList points;       ///< ordered statements, each with its numbers
    QStringList not_assessed; ///< brief "what could not be assessed and why"
};

struct CftcConclusionInput {
    services::CftcFamily family = services::CftcFamily::Legacy;
    const services::CftcInterpretationResult* result = nullptr;
    /// Per-participant percentile trail and Net %OI path (family order), from
    /// services::cftc_participant_trail over the same history.
    QVector<services::CftcParticipantTrail> trails;
    int horizon = 4; ///< 1, 4 or 13 weekly reports
    CftcPriceContextState price_state = CftcPriceContextState::Unavailable;
    QString price_unavailable_note; ///< the caller's concrete reason when no prices arrived
    QString price_series_label;     ///< e.g. "DX-Y.NYB (spot index)"
};

namespace detail {

inline const services::CftcParticipantTrail* trail_for(const QVector<services::CftcParticipantTrail>& trails,
                                                       const QString& key) {
    for (const auto& trail : trails)
        if (trail.participant_key == key)
            return &trail;
    return nullptr;
}

inline const services::CftcInterpretationState* state_for(const services::CftcParticipantInterpretation& p,
                                                          const QString& id, int horizon = -1) {
    for (const auto& state : p.states) {
        if (state.state_id != id)
            continue;
        if (horizon >= 0 && (!state.has_horizon || state.horizon_reports != horizon))
            continue;
        return &state;
    }
    return nullptr;
}

inline const services::CftcHorizonFlowReading* reading_for(const services::CftcParticipantInterpretation& p,
                                                           int horizon) {
    for (const auto& reading : p.flow_readings)
        if (reading.horizon_reports == horizon)
            return &reading;
    return nullptr;
}

/// Net %OI `reports` back on the participant's path, when the path has it.
inline std::optional<double> net_pct_ago(const services::CftcParticipantTrail* trail, int reports) {
    if (!trail || trail->net_pct_oi.size() <= reports)
        return std::nullopt;
    const auto& point = trail->net_pct_oi.at(trail->net_pct_oi.size() - 1 - reports);
    return point.has_net_pct_oi ? std::optional<double>(point.net_pct_oi) : std::nullopt;
}

inline std::optional<double> percentile_ago(const services::CftcParticipantTrail* trail, int horizon) {
    if (!trail)
        return std::nullopt;
    if (horizon == 1)
        return trail->percentile_1r;
    if (horizon == 4)
        return trail->percentile_4r;
    if (horizon == 13)
        return trail->percentile_13r;
    return std::nullopt;
}

inline QString tail_zone_text(CftcLevelTail tail) {
    return tail == CftcLevelTail::High ? cftc_text_tr("top 10%") : cftc_text_tr("bottom 10%");
}

} // namespace detail

/// One participant's level statement: "Managed Money: net long 11.7% of open
/// interest, an unusually small net long — lower than 90% of the previous 156
/// weekly reports (about 3 years). In the bottom 10% for 7 consecutive weeks."
/// Empty when the latest report carries no Net %OI for the group.
inline QString cftc_level_statement(services::CftcFamily family, const services::CftcParticipantInterpretation& p,
                                    const services::CftcParticipantTrail* trail,
                                    const services::CftcInterpretationConfig& config, int horizon,
                                    bool include_usual_range) {
    if (!p.has_net_pct_oi)
        return {};
    const QString name = cftc_group_name(family, p.participant_key);
    if (!p.historical_percentile_available) {
        return cftc_text_tr("%1: %2 (no 156-report history to compare with).")
            .arg(name, cftc_net_position_text(p.net_pct_oi));
    }
    const CftcLevelTail tail = cftc_level_tail(p.percentile, config);
    if (tail == CftcLevelTail::None && !include_usual_range)
        return {};
    QString text = cftc_text_tr("%1: %2 — %3: %4 (about 3 years).")
                       .arg(name, cftc_net_position_text(p.net_pct_oi), cftc_tail_adjective(tail, p.net_pct_oi),
                            cftc_level_rank_text(p.percentile, p.percentile_reference_count));
    if (tail != CftcLevelTail::None && trail && trail->tail_run_reports >= 2) {
        text +=
            QLatin1Char(' ') + (trail->tail_run_capped ? cftc_text_tr("In the %1 for at least %2 consecutive weeks.")
                                                       : cftc_text_tr("In the %1 for %2 consecutive weeks."))
                                   .arg(detail::tail_zone_text(tail))
                                   .arg(trail->tail_run_reports);
    } else if (tail != CftcLevelTail::None && trail && trail->tail_run_reports == 1) {
        text += QLatin1Char(' ') + cftc_text_tr("Entered the %1 this week.").arg(detail::tail_zone_text(tail));
    } else if (tail == CftcLevelTail::None) {
        const std::optional<double> before = detail::percentile_ago(trail, horizon);
        if (before) {
            text += QLatin1Char(' ') + (horizon == 1
                                            ? cftc_text_tr("A week ago: %1.").arg(cftc_percentile_ordinal(*before))
                                            : cftc_text_tr("%1 weeks ago: %2.")
                                                  .arg(QString::number(horizon), cftc_percentile_ordinal(*before)));
        }
    }
    return text;
}

/// Exit / pull-back from a tail, from the engine's EXITED_* and UNWINDING_*
/// states, with both percentiles.
inline QString cftc_transition_statement(services::CftcFamily family, const services::CftcParticipantInterpretation& p,
                                         const services::CftcParticipantTrail* trail) {
    const QString name = cftc_group_name(family, p.participant_key);
    const QString now = cftc_percentile_ordinal(p.percentile);
    const std::optional<double> previous = trail ? trail->percentile_1r : std::nullopt;
    if (detail::state_for(p, QStringLiteral("EXITED_HIGH_EXTREME")) && previous)
        return cftc_text_tr("%1: moved out of the top 10% this week (%2 → %3).")
            .arg(name, cftc_percentile_ordinal(*previous), now);
    if (detail::state_for(p, QStringLiteral("EXITED_LOW_EXTREME")) && previous)
        return cftc_text_tr("%1: moved out of the bottom 10% this week (%2 → %3).")
            .arg(name, cftc_percentile_ordinal(*previous), now);
    if (detail::state_for(p, QStringLiteral("UNWINDING_HIGH_EXTREME")))
        return cftc_text_tr("%1: fell back to the %2 this week, after at least 3 weeks in a row in the top 10% "
                            "within the last 13 weeks.")
            .arg(name, now);
    if (detail::state_for(p, QStringLiteral("UNWINDING_LOW_EXTREME")))
        return cftc_text_tr("%1: rose back to the %2 this week, after at least 3 weeks in a row in the bottom 10% "
                            "within the last 13 weeks.")
            .arg(name, now);
    return {};
}

/// The sign-aware description of a net change between two Net %OI readings.
inline QString cftc_net_change_phrase(double net_flow, std::optional<double> net_before, double net_now) {
    const QString size = cftc_abs_pct(net_flow);
    if (std::fabs(net_flow) < 0.05)
        return cftc_text_tr("net position about unchanged (%1 of open interest)").arg(cftc_signed_pct(net_flow, 2));
    if (net_before && (*net_before < 0.0) != (net_now < 0.0) && *net_before != 0.0 && net_now != 0.0) {
        return net_now > 0.0 ? cftc_text_tr("switched from net short to net long (net change %1 of open interest)")
                                   .arg(cftc_signed_pct(net_flow))
                             : cftc_text_tr("switched from net long to net short (net change %1 of open interest)")
                                   .arg(cftc_signed_pct(net_flow));
    }
    if (net_now >= 0.0) {
        return net_flow >= 0.0 ? cftc_text_tr("net long grew by %1 of open interest").arg(size)
                               : cftc_text_tr("net long shrank by %1 of open interest").arg(size);
    }
    return net_flow <= 0.0 ? cftc_text_tr("net short grew by %1 of open interest").arg(size)
                           : cftc_text_tr("net short shrank by %1 of open interest").arg(size);
}

/// One participant's change over the horizon: the net change with its legs and
/// its size against comparable moves, plus the engine's sustained count.
/// Empty when the horizon could not be formed.
inline QString cftc_change_statement(services::CftcFamily family, const services::CftcParticipantInterpretation& p,
                                     const services::CftcParticipantTrail* trail, int horizon) {
    const services::CftcHorizonFlowReading* reading = detail::reading_for(p, horizon);
    if (!reading || !reading->evaluated || !reading->has_net_flow || !p.has_net_pct_oi)
        return {};
    const QString name = cftc_group_name(family, p.participant_key);
    QString legs;
    if (reading->has_long_flow && reading->has_short_flow) {
        legs = cftc_text_tr(" (longs %1, shorts %2)")
                   .arg(cftc_signed_pct(reading->long_flow), cftc_signed_pct(reading->short_flow));
    }
    QString text =
        cftc_text_tr("%1: %2 %3%4")
            .arg(name, cftc_net_change_phrase(reading->net_flow, detail::net_pct_ago(trail, horizon), p.net_pct_oi),
                 cftc_weeks_text(horizon), legs);
    if (reading->has_net_rank)
        text += QStringLiteral(" — ") + cftc_move_size_text(reading->net_rank, horizon);
    text += QLatin1Char('.');
    // Both gross sides moved materially in the same direction: the net figure
    // understates the activity.
    const bool longs_up = detail::state_for(p, QStringLiteral("LONG_ACCUMULATION"), horizon) != nullptr;
    const bool shorts_up = detail::state_for(p, QStringLiteral("SHORT_BUILDING"), horizon) != nullptr;
    const bool longs_down = detail::state_for(p, QStringLiteral("LONG_LIQUIDATION"), horizon) != nullptr;
    const bool shorts_down = detail::state_for(p, QStringLiteral("SHORT_COVERING"), horizon) != nullptr;
    if (longs_up && shorts_up)
        text += QLatin1Char(' ') + cftc_text_tr("Both long and short positions grew notably, so the net figure "
                                                "understates the new positions taken.");
    else if (longs_down && shorts_down)
        text += QLatin1Char(' ') + cftc_text_tr("Both long and short positions shrank notably, so the net figure "
                                                "understates the positions closed.");
    for (const QString& id :
         {QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_4R"), QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_4R"),
          QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R"),
          QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_13R")}) {
        const services::CftcInterpretationState* state = detail::state_for(p, id, horizon);
        if (!state)
            continue;
        for (const auto& metric : state->metrics) {
            if (metric.key == QLatin1String("confirming_steps") && metric.has_value) {
                text += QLatin1Char(' ') + cftc_text_tr("The net position moved the same way in %1 of the %2 weeks.")
                                               .arg(static_cast<int>(metric.value))
                                               .arg(horizon);
            }
        }
    }
    return text;
}

inline QString cftc_open_interest_statement(const services::CftcInterpretationResult& result, int horizon) {
    if (!result.open_interest_available)
        return {};
    QString text = cftc_text_tr("Open interest: %1 contracts").arg(cftc_contracts(result.open_interest));
    for (const auto& reading : result.open_interest_readings) {
        if (reading.horizon_reports != horizon || !reading.evaluated || !reading.has_oi_change)
            continue;
        if (std::fabs(reading.oi_change) < 0.05)
            text += QStringLiteral(", ") + cftc_text_tr("about unchanged %1").arg(cftc_weeks_text(horizon));
        else
            text += QStringLiteral(", ") +
                    (reading.oi_change >= 0.0 ? cftc_text_tr("up %1 %2") : cftc_text_tr("down %1 %2"))
                        .arg(cftc_abs_pct(reading.oi_change), cftc_weeks_text(horizon));
        if (reading.has_rank)
            text += QStringLiteral(" — ") + cftc_move_size_text(reading.rank, horizon);
    }
    return text + QLatin1Char('.');
}

/// Market concentration, only when the engine found it high or rising.
inline QString cftc_concentration_statement(const services::CftcInterpretationResult& result) {
    const services::CftcInterpretationState* high = nullptr;
    const services::CftcInterpretationState* rising = nullptr;
    for (const auto& state : result.market_context) {
        if (state.state_id == QLatin1String("HIGH_MARKET_CONCENTRATION"))
            high = &state;
        else if (state.state_id == QLatin1String("CONCENTRATION_RISING"))
            rising = &state;
    }
    if (!high && !rising)
        return {};
    for (const auto& assessment : result.concentration) {
        if (assessment.field_key != result.config.primary_concentration_field || !assessment.has_current)
            continue;
        QString text =
            cftc_text_tr("Concentration: the 4 largest traders on the long side account for %1 of open interest")
                .arg(cftc_abs_pct(assessment.current));
        if (high && assessment.has_percentile)
            text += QStringLiteral(" — ") + cftc_rank_comparison(cftc_text_tr("higher"), assessment.percentile,
                                                                 cftc_text_tr("earlier reports for this market"));
        if (rising && assessment.has_change)
            text += QStringLiteral("; ") +
                    cftc_text_tr("up %1 percentage points this week").arg(QString::number(assessment.change, 'f', 1));
        return text + QLatin1Char('.');
    }
    return {};
}

inline QString cftc_price_statement(services::CftcFamily family, const services::CftcInterpretationResult& result,
                                    const QString& principal_key, int horizon, const QString& series_label) {
    for (const auto& assessment : result.price_context) {
        if (assessment.participant_key != principal_key || assessment.horizon_reports != horizon)
            continue;
        if (!assessment.evaluated || !assessment.has_price_move)
            return {};
        const QString name = cftc_group_name(family, principal_key);
        QString text = cftc_text_tr("Price (%1): %2 %3 (%4 → %5)")
                           .arg(series_label, cftc_signed_pct(assessment.price_move_pct), cftc_weeks_text(horizon),
                                assessment.price_anchor_date.toString(Qt::ISODate),
                                assessment.price_latest_date.toString(Qt::ISODate));
        const QString state = assessment.state_id;
        if (state == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_UP") ||
            state == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_DOWN"))
            text +=
                QStringLiteral(" — ") + cftc_text_tr("%1 positioning moved notably in the same direction").arg(name);
        else if (state == QLatin1String("PRICE_UP_POSITIONING_DOWN_DIVERGENCE") ||
                 state == QLatin1String("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"))
            text += QStringLiteral(" — ") +
                    cftc_text_tr("%1 positioning moved notably in the opposite direction").arg(name);
        else
            text +=
                QStringLiteral(" — ") + cftc_text_tr("price and %1 positioning did not both move notably").arg(name);
        return text + QLatin1Char('.');
    }
    return {};
}

/// The full conclusion for one market and one horizon.
inline CftcConclusion cftc_compose_conclusion(const CftcConclusionInput& input) {
    CftcConclusion out;
    if (!input.result)
        return out;
    const services::CftcInterpretationResult& result = *input.result;
    const services::CftcInterpretationConfig& config = result.config;
    const QString principal_key = cftc_principal_participant_key(input.family);
    const services::CftcParticipantInterpretation* principal = nullptr;
    for (const auto& p : result.participants)
        if (p.participant_key == principal_key)
            principal = &p;

    if (result.report_outdated) {
        out.points << cftc_text_tr("Old report: the latest available report is dated %1, %2 days ago. CFTC "
                                   "publishes weekly, so this contract may no longer be reported.")
                          .arg(result.report_date.toString(Qt::ISODate))
                          .arg(result.report_age_days);
    }

    // 1. Levels: the headline group always, then every other group in a tail.
    QStringList headline_parts;
    if (principal) {
        const QString level =
            cftc_level_statement(input.family, *principal, detail::trail_for(input.trails, principal->participant_key),
                                 config, input.horizon, /*include_usual_range=*/true);
        if (!level.isEmpty())
            out.points << level;
        if (principal->has_net_pct_oi && principal->historical_percentile_available) {
            const CftcLevelTail tail = cftc_level_tail(principal->percentile, config);
            headline_parts << cftc_text_tr("%1: %2 (%3 over 3 years)")
                                  .arg(cftc_group_name(input.family, principal_key),
                                       tail == CftcLevelTail::None ? cftc_net_position_text(principal->net_pct_oi)
                                                                   : cftc_tail_phrase(tail, principal->net_pct_oi),
                                       cftc_percentile_ordinal(principal->percentile));
        } else if (principal->has_net_pct_oi) {
            headline_parts << cftc_text_tr("%1: %2").arg(cftc_group_name(input.family, principal_key),
                                                         cftc_net_position_text(principal->net_pct_oi));
        }
    }
    const services::CftcParticipantInterpretation* most_extreme_other = nullptr;
    double most_extreme_distance = 0.0;
    for (const auto& p : result.participants) {
        if (&p == principal || !p.has_net_pct_oi || !p.historical_percentile_available)
            continue;
        const QString level = cftc_level_statement(input.family, p, detail::trail_for(input.trails, p.participant_key),
                                                   config, input.horizon, /*include_usual_range=*/false);
        if (level.isEmpty())
            continue;
        out.points << level;
        const double distance = std::fabs(p.percentile - 0.5);
        if (distance > most_extreme_distance) {
            most_extreme_distance = distance;
            most_extreme_other = &p;
        }
    }
    if (most_extreme_other) {
        headline_parts << cftc_text_tr("%1: %2 (%3)")
                              .arg(cftc_group_name(input.family, most_extreme_other->participant_key),
                                   cftc_tail_phrase(cftc_level_tail(most_extreme_other->percentile, config),
                                                    most_extreme_other->net_pct_oi),
                                   cftc_percentile_ordinal(most_extreme_other->percentile));
    }
    // 2. Transitions out of a tail, any group.
    for (const auto& p : result.participants) {
        const QString transition =
            cftc_transition_statement(input.family, p, detail::trail_for(input.trails, p.participant_key));
        if (!transition.isEmpty())
            out.points << transition;
    }
    // 3. Changes over the horizon: the headline group, then large moves of others.
    if (principal) {
        const QString change = cftc_change_statement(input.family, *principal,
                                                     detail::trail_for(input.trails, principal_key), input.horizon);
        if (!change.isEmpty()) {
            out.points << change;
        } else {
            const services::CftcHorizonFlowReading* reading = detail::reading_for(*principal, input.horizon);
            if (reading && !reading->evaluated) {
                out.not_assessed << cftc_text_tr("%1 change %2: %3")
                                        .arg(cftc_group_name(input.family, principal_key),
                                             cftc_weeks_text(input.horizon),
                                             cftc_unavailable_reason_wording(reading->reason));
            }
        }
    }
    for (const auto& p : result.participants) {
        if (&p == principal)
            continue;
        const services::CftcHorizonFlowReading* reading = detail::reading_for(p, input.horizon);
        if (!reading || !reading->evaluated || !reading->has_net_rank ||
            reading->net_rank < config.large_move_percentile)
            continue;
        const QString change =
            cftc_change_statement(input.family, p, detail::trail_for(input.trails, p.participant_key), input.horizon);
        if (!change.isEmpty())
            out.points << change;
    }
    // 4. Open interest, 5. concentration, 6. price.
    const QString oi = cftc_open_interest_statement(result, input.horizon);
    if (!oi.isEmpty())
        out.points << oi;
    const QString concentration = cftc_concentration_statement(result);
    if (!concentration.isEmpty())
        out.points << concentration;
    const QString price =
        cftc_price_statement(input.family, result, principal_key, input.horizon, input.price_series_label);
    if (!price.isEmpty()) {
        out.points << price;
    } else if (input.price_state == CftcPriceContextState::Unavailable) {
        QString reason = input.price_unavailable_note;
        for (const auto& assessment : result.price_context) {
            if (assessment.participant_key == principal_key && assessment.horizon_reports == input.horizon &&
                !assessment.evaluated && assessment.reason != services::CftcUnavailableReason::MissingPriceContext) {
                reason = cftc_unavailable_reason_wording(assessment.reason);
            }
        }
        if (!reason.isEmpty())
            out.not_assessed << cftc_text_tr("price relationship: %1").arg(reason);
    } else if (input.price_state == CftcPriceContextState::Ready) {
        for (const auto& assessment : result.price_context) {
            if (assessment.participant_key == principal_key && assessment.horizon_reports == input.horizon &&
                !assessment.evaluated) {
                out.not_assessed
                    << cftc_text_tr("price relationship: %1").arg(cftc_unavailable_reason_wording(assessment.reason));
            }
        }
    }
    if (principal && principal->has_net_pct_oi && !principal->historical_percentile_available) {
        out.not_assessed << cftc_text_tr("historical comparison: fewer than %1 earlier reports exist for this "
                                         "contract")
                                .arg(config.history_window);
    }
    out.headline = headline_parts.join(QStringLiteral("; "));
    return out;
}

/// The one-paragraph method note shown once, at the foot of the workspace.
inline QString cftc_method_note() {
    return cftc_text_tr(
        "Net position = long minus short, shown as a share of total open interest. Percentiles compare the latest "
        "reading with the previous 156 weekly reports (about 3 years); the top and bottom 10% are called unusual. "
        "Changes are compared with the previous 156 changes of the same length: bigger than 75% is notable, bigger "
        "than 90% is large. These cut-offs are MarketLab conventions, not CFTC definitions. The statements describe "
        "reported positions only; they are not forecasts.");
}

} // namespace fincept::screens
