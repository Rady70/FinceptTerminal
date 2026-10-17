// src/screens/economics/panels/CftcResearchText.h
//
// Labels and plain-language statements for the CFTC research workspace.
//
// This layer turns the interpretation engine's output (CftcInterpretationResult,
// rule set cftc-descriptive-interpretation-v3: the previous 52 weekly reports
// are the basis) into
//   * a small fixed vocabulary of labels the workspace shows as tags, so
//     markets and groups can be compared at a glance (level band, flow type,
//     change size, the speculators' lean, ONE-SIDED, CROWDED, side flips);
//   * short statements a reader can check, shown as the labels' tooltips:
//     every statement carries the reading it rests on and the comparison
//     behind it.
// It classifies nothing beyond those definitions: levels, tails and change
// sizes are the engine's own percentiles and size ranks with the engine's
// thresholds (10% / 90% tails, 75% notable and 90% large changes).
//
// The speculators' lean (LEANING LONG / LEANING SHORT / NEUTRAL) describes how
// the speculative group is positioned and moving over the last year. It uses
// no price words: tested against later prices it showed no predictive value,
// and ONE-SIDED marks a market whose speculators have held the same side for
// three years, where the side alone says little. CROWDED LONG / SHORT is the
// engine's CROWDED_LONG / CROWDED_SHORT state (speculative group net long in
// the top 10%, or net short in the bottom 10%, of the previous 52 reports).
//
// Rules the statements follow:
//   * numbers first: "net long 38.2% of open interest, higher than 96% of the
//     previous 52 weekly reports";
//   * every participant group whose positioning is at a historical tail is
//     described, not only the headline group, because the categories of one
//     report are often the more informative side of a market;
//   * direction words follow the actual sign: a net-short group whose net
//     position rises is "net short shrank", never "bought";
//   * a limitation is stated once, briefly, in `not_assessed`;
//   * no forecast, recommendation or trading vocabulary.
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

/// "higher than 96% of the previous 52 weekly reports". The percentile is
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

/// Size of a change from the engine's size rank (large >= 0.90, notable >= 0.75,
/// ordinary >= 0.5, small). Rule set v3 (WeeklyScaled) adds its multiple of the
/// typical change of that length: the root mean square of the previous
/// `history_window` one-week changes x sqrt(weeks). A v3 reading without a
/// scale has a reference without any change. The v2 preset (TrailingRank)
/// compares with the previous same-length changes.
inline QString cftc_move_size_text(double rank, int horizon, bool has_scale, double scale,
                                   const services::CftcInterpretationConfig& config) {
    const QString word = rank >= 0.90   ? cftc_text_tr("a large change")
                         : rank >= 0.75 ? cftc_text_tr("a notable change")
                         : rank >= 0.5  ? cftc_text_tr("an ordinary change")
                                        : cftc_text_tr("a small change");
    const int weeks = config.history_window;
    if (config.move_size_method == services::CftcMoveSizeMethod::TrailingRank) {
        const QString reference = horizon == 1 ? cftc_text_tr("comparable one-week changes")
                                               : cftc_text_tr("comparable %1-week changes").arg(horizon);
        return word + QStringLiteral(", ") +
               (rank >= 0.5 ? cftc_rank_comparison(cftc_text_tr("bigger"), rank, reference)
                            : cftc_rank_comparison(cftc_text_tr("smaller"), 1.0 - rank, reference));
    }
    if (!has_scale) {
        return rank > 0.0 ? cftc_text_tr("a large change: there was no change in the previous %1 weeks").arg(weeks)
                          : cftc_text_tr("no change, as in the previous %1 weeks").arg(weeks);
    }
    const QString typical =
        horizon == 1 ? cftc_text_tr("the typical one-week change of the last %1 weeks").arg(weeks)
                     : cftc_text_tr("the typical %1-week change of the last %2 weeks").arg(horizon).arg(weeks);
    return cftc_text_tr("%1, %2 times %3").arg(word, QString::number(scale, 'f', 1), typical);
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
/// interest — unusually small: lower than 90% of the previous 52
/// weekly reports. In the bottom 10% for 7 consecutive weeks."
/// Empty when the latest report carries no Net %OI for the group.
inline QString cftc_level_statement(services::CftcFamily family, const services::CftcParticipantInterpretation& p,
                                    const services::CftcParticipantTrail* trail,
                                    const services::CftcInterpretationConfig& config, int horizon,
                                    bool include_usual_range) {
    if (!p.has_net_pct_oi)
        return {};
    const QString name = cftc_group_name(family, p.participant_key);
    if (!p.historical_percentile_available) {
        return cftc_text_tr("%1: %2 (no %3-report history to compare with).")
            .arg(name, cftc_net_position_text(p.net_pct_oi))
            .arg(config.history_window);
    }
    const CftcLevelTail tail = cftc_level_tail(p.percentile, config);
    if (tail == CftcLevelTail::None && !include_usual_range)
        return {};
    QString text = cftc_text_tr("%1: %2 — %3: %4.")
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
                                     const services::CftcParticipantTrail* trail, int horizon,
                                     const services::CftcInterpretationConfig& config) {
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
        text += QStringLiteral(" — ") +
                cftc_move_size_text(reading->net_rank, horizon, reading->has_net_scale, reading->net_scale, config);
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
            text += QStringLiteral(" — ") +
                    cftc_move_size_text(reading.rank, horizon, reading.has_scale, reading.scale, result.config);
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
            text += QStringLiteral(" — ") +
                    cftc_rank_comparison(
                        cftc_text_tr("higher"), assessment.percentile,
                        result.config.concentration_window > 0
                            ? cftc_text_tr("the previous %1 reports").arg(result.config.concentration_window)
                            : cftc_text_tr("earlier reports"));
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
            headline_parts << cftc_text_tr("%1: %2 (%3 over %4 weeks)")
                                  .arg(cftc_group_name(input.family, principal_key),
                                       tail == CftcLevelTail::None ? cftc_net_position_text(principal->net_pct_oi)
                                                                   : cftc_tail_phrase(tail, principal->net_pct_oi),
                                       cftc_percentile_ordinal(principal->percentile))
                                  .arg(principal->percentile_reference_count);
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
        const QString change = cftc_change_statement(
            input.family, *principal, detail::trail_for(input.trails, principal_key), input.horizon, config);
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
        const QString change = cftc_change_statement(
            input.family, p, detail::trail_for(input.trails, p.participant_key), input.horizon, config);
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

/// The method note shown once, at the foot of the workspace: one line per
/// label family.
inline QString cftc_method_note() {
    return cftc_text_tr(
        "Basis: the previous 52 weekly reports. Net position = long minus short, as a share of open interest.\n"
        "LEVEL: VERY LOW = bottom 10% of the previous 52 reports, LOW = up to 30%, NORMAL, HIGH = from 70%, VERY "
        "HIGH = top 10%. For a net-short group a high level is a smaller net short (VERY HIGH = least short).\n"
        "NOTABLE / LARGE: a change of about 1.15 / 1.65 times the typical change of that length (the typical "
        "size of the previous 52 weekly changes x the square root of the weeks).\n"
        "FLOW: the side that moved notably: LONG BUILD, LONG LIQUIDATION, SHORT BUILD, SHORT COVERING, BOTH "
        "ADDING, BOTH CUTTING; when only the net change is notable, the side that moved it most; otherwise "
        "QUIET.\n"
        "LEANING LONG / LEANING SHORT / NEUTRAL (speculators: Non-Commercial, Managed Money, Leveraged Funds): "
        "LEANING LONG = net long, at or above the middle of their year and not cutting sharply over 4 weeks; "
        "LEANING SHORT = the mirror image; otherwise NEUTRAL. ONE-SIDED: on the same side in each of the previous "
        "156 reports (about 3 years), so the side alone says little.\n"
        "CROWDED LONG / SHORT: speculators net long in the top 10% / net short in the bottom 10% of the year. In "
        "the official reports since 1988 such episodes lasted a median of 2 weeks, 38% lasted 4 weeks or more, "
        "and 44% were crowded on the same side 4 weeks after they began.\n"
        "FLIPPED LONG / SHORT: the net position changed side since the previous weekly report. TURNING: moved "
        "out of the 10% tail this week, or fell back after at least 3 weeks in it within the last 13 (overview: "
        "the speculators). LARGE MOVE: the speculators' one-week change is LARGE.\n"
        "These are MarketLab conventions that describe reported positions, not prices, and not CFTC definitions. "
        "Tested against later prices (29 markets, futures-only reports, spot and ETF prices, 2001-2026), the lean "
        "and CROWDED showed no predictive value.");
}

// ── Standard labels ─────────────────────────────────────────────────────────

/// Where a group's net position sits among the previous 52 reports.
enum class CftcLevelBand { VeryLow, Low, Normal, High, VeryHigh };

inline CftcLevelBand cftc_level_band(double percentile, const services::CftcInterpretationConfig& config) {
    if (percentile >= config.extreme_percentile)
        return CftcLevelBand::VeryHigh;
    if (percentile >= 0.70)
        return CftcLevelBand::High;
    if (percentile <= config.low_extreme_percentile)
        return CftcLevelBand::VeryLow;
    if (percentile <= 0.30)
        return CftcLevelBand::Low;
    return CftcLevelBand::Normal;
}

inline QString cftc_level_band_label(CftcLevelBand band) {
    switch (band) {
        case CftcLevelBand::VeryLow:
            return cftc_text_tr("VERY LOW");
        case CftcLevelBand::Low:
            return cftc_text_tr("LOW");
        case CftcLevelBand::Normal:
            return cftc_text_tr("NORMAL");
        case CftcLevelBand::High:
            return cftc_text_tr("HIGH");
        case CftcLevelBand::VeryHigh:
            return cftc_text_tr("VERY HIGH");
    }
    return {};
}

/// The level in words of the group's side, because a high level of a net-short
/// group is a smaller net short: "least short" (VERY HIGH), "less short"
/// (HIGH), "more short" (LOW), "most short" (VERY LOW), and the long-side
/// mirror "most long", "more long", "less long", "least long". Empty for
/// NORMAL and for a flat position.
inline QString cftc_level_side_text(CftcLevelBand band, double net_pct_oi) {
    if (net_pct_oi == 0.0)
        return {};
    const bool long_side = net_pct_oi > 0.0;
    switch (band) {
        case CftcLevelBand::VeryHigh:
            return long_side ? cftc_text_tr("most long") : cftc_text_tr("least short");
        case CftcLevelBand::High:
            return long_side ? cftc_text_tr("more long") : cftc_text_tr("less short");
        case CftcLevelBand::Low:
            return long_side ? cftc_text_tr("less long") : cftc_text_tr("more short");
        case CftcLevelBand::VeryLow:
            return long_side ? cftc_text_tr("least long") : cftc_text_tr("most short");
        case CftcLevelBand::Normal:
            break;
    }
    return {};
}

/// The level band as a tag that also names the side: "VERY HIGH · LEAST SHORT".
inline QString cftc_level_band_side_label(CftcLevelBand band, double net_pct_oi) {
    const QString side = cftc_level_side_text(band, net_pct_oi);
    return side.isEmpty() ? cftc_level_band_label(band)
                          : cftc_level_band_label(band) + QStringLiteral(" · ") + side.toUpper();
}

/// Which side of a group moved notably over a horizon. A leg moved notably when
/// its size rank is at least the engine's materiality cut-off (0.75). When
/// both legs moved notably in opposite directions, the larger one names it.
/// When neither leg alone moved notably but the net change did (a moderate
/// rise of the longs together with a moderate fall of the shorts), the side
/// that moved the net position most names it; QUIET is left for a net change
/// below the cut-off as well.
enum class CftcFlowType {
    Unavailable,
    Quiet,
    LongBuild,
    LongLiquidation,
    ShortBuild,
    ShortCovering,
    BothAdding,
    BothCutting
};

inline CftcFlowType cftc_flow_type(const services::CftcHorizonFlowReading& reading,
                                   const services::CftcInterpretationConfig& config) {
    if (!reading.evaluated || !reading.has_long_flow || !reading.has_short_flow || !reading.has_long_rank ||
        !reading.has_short_rank)
        return CftcFlowType::Unavailable;
    const bool long_moved = reading.long_flow != 0.0 && reading.long_rank >= config.material_move_percentile;
    const bool short_moved = reading.short_flow != 0.0 && reading.short_rank >= config.material_move_percentile;
    auto long_type = [&] { return reading.long_flow > 0.0 ? CftcFlowType::LongBuild : CftcFlowType::LongLiquidation; };
    auto short_type = [&] { return reading.short_flow > 0.0 ? CftcFlowType::ShortBuild : CftcFlowType::ShortCovering; };
    if (!long_moved && !short_moved) {
        if (!reading.has_net_flow || !reading.has_net_rank || reading.net_flow == 0.0 ||
            reading.net_rank < config.material_move_percentile)
            return CftcFlowType::Quiet;
        // What each leg added to the net change: longs up and shorts down
        // both raise it. At least one of them points the net change's way.
        const double from_long = reading.long_flow;
        const double from_short = -reading.short_flow;
        const bool rising = reading.net_flow > 0.0;
        auto pushes = [rising](double part) { return rising ? part > 0.0 : part < 0.0; };
        const bool long_drives =
            pushes(from_long) && (!pushes(from_short) || std::fabs(from_long) >= std::fabs(from_short));
        return long_drives ? long_type() : short_type();
    }
    if (long_moved && !short_moved)
        return long_type();
    if (short_moved && !long_moved)
        return short_type();
    if (reading.long_flow > 0.0 && reading.short_flow > 0.0)
        return CftcFlowType::BothAdding;
    if (reading.long_flow < 0.0 && reading.short_flow < 0.0)
        return CftcFlowType::BothCutting;
    if (reading.long_rank != reading.short_rank)
        return reading.long_rank > reading.short_rank ? long_type() : short_type();
    return std::fabs(reading.long_flow) >= std::fabs(reading.short_flow) ? long_type() : short_type();
}

inline QString cftc_flow_label(CftcFlowType type) {
    switch (type) {
        case CftcFlowType::Unavailable:
            return {};
        case CftcFlowType::Quiet:
            return cftc_text_tr("QUIET");
        case CftcFlowType::LongBuild:
            return cftc_text_tr("LONG BUILD");
        case CftcFlowType::LongLiquidation:
            return cftc_text_tr("LONG LIQUIDATION");
        case CftcFlowType::ShortBuild:
            return cftc_text_tr("SHORT BUILD");
        case CftcFlowType::ShortCovering:
            return cftc_text_tr("SHORT COVERING");
        case CftcFlowType::BothAdding:
            return cftc_text_tr("BOTH ADDING");
        case CftcFlowType::BothCutting:
            return cftc_text_tr("BOTH CUTTING");
    }
    return {};
}

/// Whether a flow type moves the net position up (+1), down (-1) or not (0).
inline int cftc_flow_direction(CftcFlowType type) {
    switch (type) {
        case CftcFlowType::LongBuild:
        case CftcFlowType::ShortCovering:
            return 1;
        case CftcFlowType::LongLiquidation:
        case CftcFlowType::ShortBuild:
            return -1;
        default:
            return 0;
    }
}

/// Size of a group's net change over a horizon (or of an Open Interest change).
enum class CftcChangeSize { Unavailable, Ordinary, Notable, Large };

inline CftcChangeSize cftc_change_size(bool has_rank, double rank, const services::CftcInterpretationConfig& config) {
    if (!has_rank)
        return CftcChangeSize::Unavailable;
    if (rank >= config.large_move_percentile)
        return CftcChangeSize::Large;
    if (rank >= config.material_move_percentile)
        return CftcChangeSize::Notable;
    return CftcChangeSize::Ordinary;
}

inline QString cftc_change_size_label(CftcChangeSize size) {
    switch (size) {
        case CftcChangeSize::Large:
            return cftc_text_tr("LARGE");
        case CftcChangeSize::Notable:
            return cftc_text_tr("NOTABLE");
        default:
            return {};
    }
}

/// The speculative group's lean over the last year. LEANING LONG: net long and
/// at or above the middle of the previous 52 reports, unless its net position
/// fell by a LARGE amount over 4 weeks; LEANING SHORT: net short and at or
/// below the middle, unless it rose by a LARGE amount over 4 weeks; NEUTRAL
/// otherwise. It describes positions, not prices: tested against later prices
/// (2001-2026) it showed no predictive value, so it carries no price words.
enum class CftcBias { Unavailable, LeaningLong, LeaningShort, Neutral };

inline CftcBias cftc_positioning_bias(const services::CftcParticipantInterpretation& p,
                                      const services::CftcInterpretationConfig& config) {
    if (!p.has_net_pct_oi || !p.historical_percentile_available)
        return CftcBias::Unavailable;
    const services::CftcHorizonFlowReading* four = nullptr;
    for (const auto& reading : p.flow_readings)
        if (reading.horizon_reports == 4)
            four = &reading;
    const bool sharp_fall = four && four->evaluated && four->has_net_flow && four->has_net_rank &&
                            four->net_flow < 0.0 && four->net_rank >= config.large_move_percentile;
    const bool sharp_rise = four && four->evaluated && four->has_net_flow && four->has_net_rank &&
                            four->net_flow > 0.0 && four->net_rank >= config.large_move_percentile;
    if (p.net_pct_oi > 0.0 && p.percentile >= 0.5)
        return sharp_fall ? CftcBias::Neutral : CftcBias::LeaningLong;
    if (p.net_pct_oi < 0.0 && p.percentile <= 0.5)
        return sharp_rise ? CftcBias::Neutral : CftcBias::LeaningShort;
    return CftcBias::Neutral;
}

inline QString cftc_bias_label(CftcBias bias) {
    switch (bias) {
        case CftcBias::LeaningLong:
            return cftc_text_tr("LEANING LONG");
        case CftcBias::LeaningShort:
            return cftc_text_tr("LEANING SHORT");
        case CftcBias::Neutral:
            return cftc_text_tr("NEUTRAL");
        case CftcBias::Unavailable:
            break;
    }
    return {};
}

/// The bias as a sentence, for its tooltip. The branches follow the rule in
/// cftc_positioning_bias.
inline QString cftc_bias_reason(CftcBias bias, const services::CftcParticipantInterpretation& p,
                                const services::CftcInterpretationConfig& config) {
    if (bias == CftcBias::Unavailable)
        return {};
    const QString side = cftc_net_position_text(p.net_pct_oi);
    const QString level = cftc_level_rank_text(p.percentile, config.history_window);
    if (bias == CftcBias::LeaningLong)
        return cftc_text_tr("LEANING LONG: %1, %2, and the net long has not shrunk by a large amount over 4 weeks. "
                            "It describes positions, not prices.")
            .arg(side, level);
    if (bias == CftcBias::LeaningShort)
        return cftc_text_tr("LEANING SHORT: %1, %2, and the net short has not shrunk by a large amount over 4 weeks. "
                            "It describes positions, not prices.")
            .arg(side, level);
    if (p.net_pct_oi > 0.0 && p.percentile >= 0.5)
        return cftc_text_tr("NEUTRAL: %1, %2, but the net long shrank by a large amount over 4 weeks.")
            .arg(side, level);
    if (p.net_pct_oi < 0.0 && p.percentile <= 0.5)
        return cftc_text_tr("NEUTRAL: %1, %2, but the net short shrank by a large amount over 4 weeks.")
            .arg(side, level);
    if (p.net_pct_oi == 0.0)
        return cftc_text_tr("NEUTRAL: %1.").arg(side);
    return cftc_text_tr("NEUTRAL: %1, but %2 — the side and the level of the year point different ways.")
        .arg(side, level);
}

/// A speculative group whose net position was on its current side in each of
/// the previous `reports` reports (156, about 3 years): in such a market the
/// side is the usual state, so LEANING LONG / SHORT says little on its own and
/// the level carries the information. A report without a net position, a flat
/// one or a shorter history never qualifies.
struct CftcOneSided {
    int direction = 0; ///< +1 always net long, -1 always net short, 0 not one-sided
    int reports = 0;
};

inline constexpr int kCftcOneSidedReports = 156;

inline CftcOneSided cftc_one_sided(const QVector<services::CftcMonitorHistoryPoint>& path,
                                   int reports = kCftcOneSidedReports) {
    CftcOneSided out;
    const int n = static_cast<int>(path.size());
    if (reports < 1 || n < reports + 1)
        return out;
    const auto& now = path[n - 1];
    if (!now.has_net_pct_oi || now.net_pct_oi == 0.0)
        return out;
    const bool long_side = now.net_pct_oi > 0.0;
    for (int i = n - 1 - reports; i < n - 1; ++i) {
        const auto& point = path[i];
        if (!point.has_net_pct_oi || point.net_pct_oi == 0.0 || (point.net_pct_oi > 0.0) != long_side)
            return out;
    }
    out.direction = long_side ? 1 : -1;
    out.reports = reports;
    return out;
}

inline QString cftc_one_sided_label(const CftcOneSided& one_sided) {
    return one_sided.direction != 0 ? cftc_text_tr("ONE-SIDED") : QString();
}

inline QString cftc_one_sided_reason(const CftcOneSided& one_sided) {
    if (one_sided.direction == 0)
        return {};
    return cftc_text_tr("ONE-SIDED: net %1 in each of the previous %2 reports (about 3 years). In this market "
                        "the side is the usual state, so the side alone says little; the level shows whether the "
                        "position is larger or smaller than usual.")
        .arg(one_sided.direction > 0 ? cftc_text_tr("long") : cftc_text_tr("short"))
        .arg(one_sided.reports);
}

/// The engine's CROWDED_LONG / CROWDED_SHORT state of a speculative group.
enum class CftcCrowding { None, Long, Short };

inline CftcCrowding cftc_crowding(const services::CftcParticipantInterpretation& p) {
    for (const auto& state : p.states) {
        if (state.state_id == QLatin1String("CROWDED_LONG"))
            return CftcCrowding::Long;
        if (state.state_id == QLatin1String("CROWDED_SHORT"))
            return CftcCrowding::Short;
    }
    return CftcCrowding::None;
}

inline QString cftc_crowding_label(CftcCrowding crowding) {
    if (crowding == CftcCrowding::Long)
        return cftc_text_tr("CROWDED LONG");
    if (crowding == CftcCrowding::Short)
        return cftc_text_tr("CROWDED SHORT");
    return {};
}

/// The crowding label as a sentence, with the category caveat the engine's
/// terminology contract requires wherever crowding wording is used.
inline QString cftc_crowding_reason(CftcCrowding crowding, const services::CftcParticipantInterpretation& p,
                                    const services::CftcInterpretationConfig& config) {
    if (crowding == CftcCrowding::None)
        return {};
    QString text =
        (crowding == CftcCrowding::Long
             ? cftc_text_tr("CROWDED LONG: net long and in the top 10% of the previous %1 weekly reports.")
             : cftc_text_tr("CROWDED SHORT: net short and in the bottom 10% of the previous %1 weekly reports."))
            .arg(config.history_window);
    const QString caveat = cftc_terminology_caveat_sentence(p);
    if (!caveat.isEmpty())
        text += QLatin1Char(' ') + caveat;
    return text;
}

/// A group whose net position changed side in the latest report: the new side
/// (+1 long, -1 short, 0 none) and how many consecutive weekly reports it spent
/// on the other side before (capped when the run reaches the start of the
/// history or a gap).
struct CftcSideFlip {
    int direction = 0;
    int prior_weeks = 0;
    bool prior_capped = false;
};

inline CftcSideFlip cftc_side_flip(const QVector<services::CftcMonitorHistoryPoint>& path) {
    CftcSideFlip out;
    const int n = static_cast<int>(path.size());
    if (n < 2)
        return out;
    const auto& now = path[n - 1];
    const auto& before = path[n - 2];
    if (!now.has_net_pct_oi || !before.has_net_pct_oi || now.net_pct_oi == 0.0 || before.net_pct_oi == 0.0 ||
        (now.net_pct_oi > 0.0) == (before.net_pct_oi > 0.0) || !services::cftc_weekly_neighbour(before.date, now.date))
        return out;
    out.direction = now.net_pct_oi > 0.0 ? 1 : -1;
    const bool before_long = before.net_pct_oi > 0.0;
    int index = n - 2;
    for (; index >= 0; --index) {
        const auto& point = path[index];
        if (!point.has_net_pct_oi || point.net_pct_oi == 0.0 || (point.net_pct_oi > 0.0) != before_long)
            break;
        ++out.prior_weeks;
        if (index > 0 && !services::cftc_weekly_neighbour(path[index - 1].date, point.date)) {
            out.prior_capped = true;
            break;
        }
    }
    if (index < 0)
        out.prior_capped = true;
    return out;
}

inline QString cftc_side_flip_label(const CftcSideFlip& flip) {
    if (flip.direction > 0)
        return cftc_text_tr("FLIPPED LONG");
    if (flip.direction < 0)
        return cftc_text_tr("FLIPPED SHORT");
    return {};
}

// ── Concentration ───────────────────────────────────────────────────────────

/// "4 largest · long · gross": one of the CFTC's eight concentration figures.
inline QString cftc_concentration_name(int traders, bool long_side, bool net) {
    return cftc_text_tr("%1 largest · %2 · %3")
        .arg(traders)
        .arg(long_side ? cftc_text_tr("long") : cftc_text_tr("short"),
             net ? cftc_text_tr("net") : cftc_text_tr("gross"));
}

/// One concentration figure as a sentence, for its tooltip: the share the
/// CFTC publishes, its level among the previous reports, its one-week change
/// with its size, and what gross or net means.
inline QString cftc_concentration_statement(int traders, bool long_side, bool net,
                                            const services::CftcConcentrationAssessment& a,
                                            const services::CftcInterpretationConfig& config) {
    if (!a.has_current)
        return {};
    const QString side = long_side ? cftc_text_tr("long") : cftc_text_tr("short");
    QString text = cftc_text_tr("The %1 largest traders account for %2 of open interest on the %3 side (%4)")
                       .arg(traders)
                       .arg(cftc_abs_pct(a.current), side, net ? cftc_text_tr("net") : cftc_text_tr("gross"));
    if (a.has_percentile)
        text += QStringLiteral(" — ") + cftc_level_rank_text(a.percentile, a.percentile_reference_count);
    text += QLatin1Char('.');
    if (a.has_change) {
        const QString amount = std::fabs(a.change) < 0.05
                                   ? cftc_text_tr("about unchanged")
                                   : (a.change > 0.0 ? QStringLiteral("+") : QString()) +
                                         QString::number(a.change, 'f', 1) + cftc_text_tr(" percentage points");
        text += QLatin1Char(' ') + cftc_text_tr("This week: %1").arg(amount);
        if (a.has_change_rank)
            text += QStringLiteral(" — ") +
                    cftc_move_size_text(a.change_rank, 1, a.has_change_scale, a.change_scale, config);
        text += QLatin1Char('.');
    } else if (a.change_unavailable_reason != services::CftcUnavailableReason::None) {
        text += QLatin1Char(' ') +
                cftc_text_tr("One-week change: %1.").arg(cftc_unavailable_reason_wording(a.change_unavailable_reason));
    }
    text += QLatin1Char(' ') +
            (net ? cftc_text_tr("Net: after offsetting each trader's own long and short positions against each other.")
                 : cftc_text_tr("Gross: each trader's whole %1 position.").arg(side));
    return text;
}

inline QString cftc_side_flip_reason(const CftcSideFlip& flip) {
    if (flip.direction == 0)
        return {};
    const QString side = flip.direction > 0 ? cftc_text_tr("net long") : cftc_text_tr("net short");
    const QString other = flip.direction > 0 ? cftc_text_tr("net short") : cftc_text_tr("net long");
    return (flip.prior_capped ? cftc_text_tr("Turned %1 this week after at least %2 consecutive weeks %3.")
                              : cftc_text_tr("Turned %1 this week after %2 consecutive weeks %3."))
        .arg(side)
        .arg(flip.prior_weeks)
        .arg(other);
}

} // namespace fincept::screens
