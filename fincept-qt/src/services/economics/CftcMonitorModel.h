// src/services/economics/CftcMonitorModel.h
// Cross-market COT monitor model and descriptive alert projection.
//
// This layer is a pure consumer of the finalized COT analytical engine:
//   canonical rows -> cftc_parse_history -> cftc_interpret -> projection.
// It never defines a threshold, never reclassifies a state and never emits a
// directional recommendation. Alerts are a deterministic re-projection of the
// Batch 4A states that already exist (`cftc_interpret`), grouped into
// descriptive attention classes, plus explicit data-quality records for
// unavailable/stale/insufficient reports. The rule set identity stays
// cftc-descriptive-interpretation-v2.
//
// The alertable state list is closed: every alert carries the triggering
// `state_id`, participant, horizon and the emitted state metrics, so a
// surfaced market can always be traced back to the engine rule that caused it.
#pragma once

#include "services/economics/CftcInterpretationModel.h"
#include "services/economics/CftcMarketCatalog.h"
#include "services/economics/CftcMetricModel.h"

#include <QCoreApplication>
#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <algorithm>

namespace fincept::services {

// ── Descriptive attention classes ───────────────────────────────────────────

/// The auditable grouping used by the markets-requiring-attention view. A class
/// is a fixed descriptive category, not a score: no numeric severity, weight or
/// probability is derived anywhere. `DataQuality` is deliberately first in
/// display order because a market whose data is unavailable or out of date must
/// be visible before its (possibly absent) positioning reading is trusted.
enum class CftcAttentionClass {
    DataQuality,
    Extreme,
    ExtremeTransition,
    Repositioning,
    OpenInterest,
    Concentration,
};

inline QString cftc_attention_class_code(CftcAttentionClass classification) {
    switch (classification) {
        case CftcAttentionClass::DataQuality:
            return QStringLiteral("data_quality");
        case CftcAttentionClass::Extreme:
            return QStringLiteral("extreme");
        case CftcAttentionClass::ExtremeTransition:
            return QStringLiteral("extreme_transition");
        case CftcAttentionClass::Repositioning:
            return QStringLiteral("repositioning");
        case CftcAttentionClass::OpenInterest:
            return QStringLiteral("open_interest");
        case CftcAttentionClass::Concentration:
            return QStringLiteral("concentration");
    }
    return QStringLiteral("data_quality");
}

inline QString cftc_attention_class_label(CftcAttentionClass classification) {
    switch (classification) {
        case CftcAttentionClass::DataQuality:
            return QCoreApplication::translate("CftcMonitorModel", "Data quality");
        case CftcAttentionClass::Extreme:
            return QCoreApplication::translate("CftcMonitorModel", "Historical extreme");
        case CftcAttentionClass::ExtremeTransition:
            return QCoreApplication::translate("CftcMonitorModel", "Extreme transition");
        case CftcAttentionClass::Repositioning:
            return QCoreApplication::translate("CftcMonitorModel", "Repositioning");
        case CftcAttentionClass::OpenInterest:
            return QCoreApplication::translate("CftcMonitorModel", "Open Interest");
        case CftcAttentionClass::Concentration:
            return QCoreApplication::translate("CftcMonitorModel", "Concentration");
    }
    return {};
}

/// Lower value = shown earlier in the attention view. The order is fixed and
/// documented; it is never derived from an alert count or a magnitude.
inline int cftc_attention_class_priority(CftcAttentionClass classification) {
    switch (classification) {
        case CftcAttentionClass::DataQuality:
            return 0;
        case CftcAttentionClass::Extreme:
            return 1;
        case CftcAttentionClass::ExtremeTransition:
            return 2;
        case CftcAttentionClass::Repositioning:
            return 3;
        case CftcAttentionClass::OpenInterest:
            return 4;
        case CftcAttentionClass::Concentration:
            return 5;
    }
    return 6;
}

// ── Alertable states ────────────────────────────────────────────────────────

struct CftcAlertRule {
    const char* state_id;
    CftcAttentionClass attention_class;
};

/// The closed set of engine states the monitor surfaces as alerts. Direction
/// levels (NET_LONG/NET_SHORT/NET_FLAT) and price relationship states are not
/// alerts: the former is not a development, and the latter is evaluated only in
/// the detailed workspace where a roll-safe price series is supplied.
inline QVector<CftcAlertRule> cftc_alert_rules() {
    return {
        {"HISTORICALLY_HIGH_NET", CftcAttentionClass::Extreme},
        {"HISTORICALLY_LOW_NET", CftcAttentionClass::Extreme},
        {"CROWDED_LONG", CftcAttentionClass::Extreme},
        {"CROWDED_SHORT", CftcAttentionClass::Extreme},
        {"SEVERE_LONG_EXTREME", CftcAttentionClass::Extreme},
        {"SEVERE_SHORT_EXTREME", CftcAttentionClass::Extreme},
        {"PERSISTENT_HIGH_EXTREME", CftcAttentionClass::Extreme},
        {"PERSISTENT_LOW_EXTREME", CftcAttentionClass::Extreme},
        {"EXITED_HIGH_EXTREME", CftcAttentionClass::ExtremeTransition},
        {"EXITED_LOW_EXTREME", CftcAttentionClass::ExtremeTransition},
        {"UNWINDING_HIGH_EXTREME", CftcAttentionClass::ExtremeTransition},
        {"UNWINDING_LOW_EXTREME", CftcAttentionClass::ExtremeTransition},
        {"LONG_ACCUMULATION", CftcAttentionClass::Repositioning},
        {"LONG_LIQUIDATION", CftcAttentionClass::Repositioning},
        {"SHORT_BUILDING", CftcAttentionClass::Repositioning},
        {"SHORT_COVERING", CftcAttentionClass::Repositioning},
        {"NET_LONGWARD_SHIFT", CftcAttentionClass::Repositioning},
        {"NET_SHORTWARD_SHIFT", CftcAttentionClass::Repositioning},
        {"NET_SHARE_RAW_DISAGREEMENT", CftcAttentionClass::Repositioning},
        {"SUSTAINED_LONGWARD_REPOSITIONING_4R", CftcAttentionClass::Repositioning},
        {"SUSTAINED_SHORTWARD_REPOSITIONING_4R", CftcAttentionClass::Repositioning},
        {"SUSTAINED_LONGWARD_REPOSITIONING_13R", CftcAttentionClass::Repositioning},
        {"SUSTAINED_SHORTWARD_REPOSITIONING_13R", CftcAttentionClass::Repositioning},
        {"OI_EXPANSION", CftcAttentionClass::OpenInterest},
        {"OI_CONTRACTION", CftcAttentionClass::OpenInterest},
        {"HIGH_MARKET_CONCENTRATION", CftcAttentionClass::Concentration},
        {"CONCENTRATION_RISING", CftcAttentionClass::Concentration},
    };
}

inline bool cftc_state_is_alertable(const QString& state_id) {
    for (const CftcAlertRule& rule : cftc_alert_rules()) {
        if (state_id == QLatin1String(rule.state_id))
            return true;
    }
    return false;
}

/// The attention class of an alertable state. The state must be alertable;
/// unknown ids are reported under DataQuality so an unmapped id can never be
/// silently dropped from an audit.
inline CftcAttentionClass cftc_state_attention_class(const QString& state_id) {
    for (const CftcAlertRule& rule : cftc_alert_rules()) {
        if (state_id == QLatin1String(rule.state_id))
            return rule.attention_class;
    }
    return CftcAttentionClass::DataQuality;
}

// ── Monitor statuses ────────────────────────────────────────────────────────

/// The provider-reported freshness/availability state for one market. `Ok`
/// means canonical rows exist and the current API refresh succeeded; the
/// distinction between valid, stored-only and unavailable data is preserved
/// instead of being flattened into "has rows".
enum class CftcMonitorStatus {
    Ok,
    ArchiveOnly,
    NoData,
    NoLocalHistory,
    Unavailable,
    UnknownMarket,
};

inline QString cftc_monitor_status_code(CftcMonitorStatus status) {
    switch (status) {
        case CftcMonitorStatus::Ok:
            return QStringLiteral("ok");
        case CftcMonitorStatus::ArchiveOnly:
            return QStringLiteral("archive_only");
        case CftcMonitorStatus::NoData:
            return QStringLiteral("no_data");
        case CftcMonitorStatus::NoLocalHistory:
            return QStringLiteral("no_local_history");
        case CftcMonitorStatus::Unavailable:
            return QStringLiteral("unavailable");
        case CftcMonitorStatus::UnknownMarket:
            return QStringLiteral("unknown_market");
    }
    return QStringLiteral("unavailable");
}

/// Fail-closed parse: an unrecognized provider status is `Unavailable`, never
/// silently treated as current.
inline CftcMonitorStatus cftc_monitor_status_from_code(const QString& code) {
    if (code == QLatin1String("updated") || code == QLatin1String("current") || code == QLatin1String("ok"))
        return CftcMonitorStatus::Ok;
    if (code == QLatin1String("archive_only"))
        return CftcMonitorStatus::ArchiveOnly;
    if (code == QLatin1String("no_data"))
        return CftcMonitorStatus::NoData;
    if (code == QLatin1String("no_local_history"))
        return CftcMonitorStatus::NoLocalHistory;
    if (code == QLatin1String("unknown_market"))
        return CftcMonitorStatus::UnknownMarket;
    return CftcMonitorStatus::Unavailable;
}

// ── Alerts ──────────────────────────────────────────────────────────────────

/// One descriptive alert. `state_id` is the exact Batch 4A state that fired;
/// `metrics`, `percentile` and `move_rank` are copied from that emitted state,
/// so the UI can render the cause without recalculating anything. Data-quality
/// alerts carry no state; they name the concrete condition in `detail_reason`
/// and, where the engine produced one, the engine reason code.
struct CftcAlert {
    QString market_key;
    QString participant_key; // empty for report-wide records
    bool has_horizon = false;
    int horizon_reports = 0;
    QString state_id; // empty for data-quality records
    CftcAttentionClass attention_class = CftcAttentionClass::DataQuality;
    QString detail_reason;
    CftcUnavailableReason reason = CftcUnavailableReason::None;
    bool has_percentile = false;
    double percentile = 0.0;
    int percentile_reference_count = 0;
    bool has_move_rank = false;
    double move_rank = 0.0;
    QVector<CftcStateMetric> metrics;
};

// ── Monitor entry ───────────────────────────────────────────────────────────

struct CftcMonitorEntry {
    QString market_key;
    QString label;
    QString asset_class;
    bool known_market = false;
    CftcFamily family = CftcFamily::Legacy;
    bool futures_only = false;
    QString provider_status;
    CftcMonitorStatus status = CftcMonitorStatus::Unavailable;
    QString status_detail;
    int archive_rows = 0;

    bool interpreted = false;
    bool report_unavailable = false;
    CftcUnavailableReason report_unavailable_reason = CftcUnavailableReason::None;
    QString contract_code;

    QDate report_date;
    bool report_date_available = false;
    bool report_outdated = false;
    bool report_age_available = false;
    int report_age_days = 0;

    bool has_open_interest = false;
    double open_interest = 0.0;
    QVector<CftcOpenInterestReading> open_interest_readings;

    QString principal_participant_key;
    QString principal_label;
    bool principal_available = false;
    bool has_net_position = false;
    double net_position = 0.0;
    bool has_net_pct_oi = false;
    double net_pct_oi = 0.0;
    bool has_percentile = false;
    double percentile = 0.0;
    int percentile_reference_count = 0;
    QVector<CftcHorizonFlowReading> flow_readings;
    bool principal_historical_unavailable = false;
    CftcUnavailableReason principal_historical_unavailable_reason = CftcUnavailableReason::None;

    QVector<CftcInterpretationState> principal_states;
    QVector<CftcInterpretationState> market_states;
    QVector<CftcAlert> alerts;
    bool requires_attention = false;
};

struct CftcMonitorModel {
    QVector<CftcMonitorEntry> entries; // provider order preserved
    QString error;                     // non-empty when the payload is unusable
};

// ── Alert projection ────────────────────────────────────────────────────────

inline CftcAlert cftc_state_alert(const CftcInterpretationState& state) {
    CftcAlert alert;
    alert.participant_key = state.participant_key;
    alert.has_horizon = state.has_horizon;
    alert.horizon_reports = state.horizon_reports;
    alert.state_id = state.state_id;
    alert.attention_class = cftc_state_attention_class(state.state_id);
    alert.has_percentile = state.has_percentile;
    alert.percentile = state.percentile;
    alert.percentile_reference_count = state.percentile_reference_count;
    alert.has_move_rank = state.has_move_rank;
    alert.move_rank = state.move_rank;
    alert.metrics = state.metrics;
    return alert;
}

inline CftcAlert cftc_data_quality_alert(const QString& detail,
                                         CftcUnavailableReason reason = CftcUnavailableReason::None) {
    CftcAlert alert;
    alert.attention_class = CftcAttentionClass::DataQuality;
    alert.detail_reason = detail;
    alert.reason = reason;
    return alert;
}

/// Deterministic alert list for one monitor entry: the alertable states already
/// emitted by the engine, plus explicit data-quality records. Availability
/// reasons come from the engine's own unavailable records, never from invented
/// thresholds. Stable order: attention class, then engine emission order.
inline QVector<CftcAlert> cftc_build_alerts(const CftcMonitorEntry& entry) {
    QVector<CftcAlert> alerts;
    if (entry.status == CftcMonitorStatus::UnknownMarket) {
        alerts.append(cftc_data_quality_alert(
            entry.status_detail.isEmpty()
                ? QCoreApplication::translate("CftcMonitorModel",
                                              "the market is not part of the MarketLab CFTC universe")
                : entry.status_detail));
    } else if (entry.status == CftcMonitorStatus::NoData) {
        alerts.append(cftc_data_quality_alert(
            QCoreApplication::translate("CftcMonitorModel", "no official CFTC observations for this report family")));
    } else if (entry.status == CftcMonitorStatus::NoLocalHistory) {
        alerts.append(cftc_data_quality_alert(
            QCoreApplication::translate("CftcMonitorModel", "no stored history; the current refresh was not run")));
    } else if (entry.status == CftcMonitorStatus::Unavailable) {
        alerts.append(cftc_data_quality_alert(
            entry.status_detail.isEmpty() ? QCoreApplication::translate("CftcMonitorModel", "the data is unavailable")
                                          : entry.status_detail));
    } else if (entry.status == CftcMonitorStatus::ArchiveOnly) {
        // A stored-history fallback is a data-availability condition: the
        // provider refresh failed (or was not run), so the position shown is
        // whatever the durable archive last carried. It stays visible instead
        // of being indistinguishable from a successful current refresh.
        alerts.append(cftc_data_quality_alert(
            entry.status_detail.isEmpty()
                ? QCoreApplication::translate("CftcMonitorModel",
                                              "the provider refresh failed or was not run; stored history is shown")
                : entry.status_detail));
    }
    if (entry.report_unavailable) {
        alerts.append(cftc_data_quality_alert(
            QCoreApplication::translate("CftcMonitorModel", "the report could not be interpreted"),
            entry.report_unavailable_reason));
    } else {
        if (entry.report_outdated) {
            alerts.append(cftc_data_quality_alert(
                QCoreApplication::translate("CftcMonitorModel", "the latest report is %1 days old")
                    .arg(entry.report_age_days)));
        }
        if (entry.principal_historical_unavailable) {
            alerts.append(cftc_data_quality_alert(
                QCoreApplication::translate("CftcMonitorModel", "the historical reference is unavailable"),
                entry.principal_historical_unavailable_reason));
        }
    }
    for (const CftcInterpretationState& state : entry.principal_states) {
        if (state.available && !state.state_id.isEmpty() && cftc_state_is_alertable(state.state_id))
            alerts.append(cftc_state_alert(state));
    }
    for (const CftcInterpretationState& state : entry.market_states) {
        if (state.available && !state.state_id.isEmpty() && cftc_state_is_alertable(state.state_id))
            alerts.append(cftc_state_alert(state));
    }
    std::stable_sort(alerts.begin(), alerts.end(), [](const CftcAlert& a, const CftcAlert& b) {
        return cftc_attention_class_priority(a.attention_class) < cftc_attention_class_priority(b.attention_class);
    });
    return alerts;
}

/// The entry's attention priority: its highest-priority alert class. Used only
/// for the documented grouping order, never as a magnitude.
inline int cftc_entry_attention_priority(const CftcMonitorEntry& entry) {
    int priority = cftc_attention_class_priority(CftcAttentionClass::Concentration) + 1;
    for (const CftcAlert& alert : entry.alerts)
        priority = std::min(priority, cftc_attention_class_priority(alert.attention_class));
    return priority;
}

/// Deterministic markets-requiring-attention order: attention class first, then
/// the market's display label (case-insensitive), then the market key. No
/// composite score, alert count or magnitude ordering is involved.
inline QVector<int> cftc_attention_order(const QVector<CftcMonitorEntry>& entries) {
    QVector<int> order;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i].requires_attention)
            order.append(i);
    }
    std::stable_sort(order.begin(), order.end(), [&entries](int a, int b) {
        const int priority_a = cftc_entry_attention_priority(entries[a]);
        const int priority_b = cftc_entry_attention_priority(entries[b]);
        if (priority_a != priority_b)
            return priority_a < priority_b;
        const int label = QString::compare(entries[a].label, entries[b].label, Qt::CaseInsensitive);
        if (label != 0)
            return label < 0;
        return entries[a].market_key < entries[b].market_key;
    });
    return order;
}

// ── Payload parsing ─────────────────────────────────────────────────────────

/// The canonical observation vector the monitor interprets: the parsed recent
/// rows plus, when the provider supplied it, the older primary-concentration
/// series reconstructed as concentration-carrying observations.
struct CftcMonitorObservations {
    QVector<CftcObservation> observations;
    QString error;
};

/// Merge the provider's compact older primary-concentration series into the
/// parsed recent rows.
///
/// The engine has exactly one unbounded trailing reference: the primary
/// concentration field's full trailing percentile. Transporting the complete
/// row for every historical report would produce a payload far beyond the
/// normal provider contract, so the provider sends a suffix of full rows plus
/// the older primary-field values; this function rebuilds one canonical
/// observation vector where the older rows carry only that value. The
/// provider computes the suffix length from the engine's own reference rules
/// (`_monitor_required_start` in scripts/cftc_data.py): it covers the last 157
/// valid points and computed moves for every participant, Open Interest and
/// the primary concentration field, so the merged vector reproduces every
/// trailing reference exactly. A test pins this equivalence against the
/// full-history interpretation.
inline CftcMonitorObservations cftc_monitor_observations(const CftcHistory& parsed_rows,
                                                         const QJsonObject& concentration_history) {
    CftcMonitorObservations out;
    out.observations = parsed_rows.observations;
    if (concentration_history.isEmpty())
        return out;
    const CftcInterpretationConfig config = cftc_default_interpretation_config();
    const QString field = concentration_history.value(QStringLiteral("field")).toString();
    if (field != config.primary_concentration_field) {
        out.error = QCoreApplication::translate(
                        "CftcMonitorModel",
                        "The monitor supplied the %1 concentration series, but the engine's primary field is %2.")
                        .arg(field.isEmpty() ? QStringLiteral("(unnamed)") : field, config.primary_concentration_field);
        out.observations.clear();
        return out;
    }
    const QJsonArray dates = concentration_history.value(QStringLiteral("dates")).toArray();
    const QJsonArray values = concentration_history.value(QStringLiteral("values")).toArray();
    if (dates.size() != values.size()) {
        out.error = QCoreApplication::translate("CftcMonitorModel",
                                                "The concentration series date and value counts do not match.");
        out.observations.clear();
        return out;
    }
    if (dates.isEmpty())
        return out;
    if (parsed_rows.observations.isEmpty()) {
        out.error = QCoreApplication::translate(
            "CftcMonitorModel", "A concentration series was supplied without any recent observations to anchor it.");
        out.observations.clear();
        return out;
    }
    const CftcObservation& anchor = parsed_rows.observations.first();
    QVector<CftcObservation> older;
    older.reserve(dates.size());
    QDate previous;
    for (int i = 0; i < dates.size(); ++i) {
        CftcObservation observation;
        observation.date_label = dates[i].toString().trimmed();
        observation.date = cftc_parse_report_date(observation.date_label);
        if (!observation.date.isValid()) {
            out.error = QCoreApplication::translate("CftcMonitorModel",
                                                    "The concentration series carries an unusable report date.");
            out.observations.clear();
            return out;
        }
        if (previous.isValid() && observation.date <= previous) {
            out.error = QCoreApplication::translate(
                "CftcMonitorModel", "The concentration series is not strictly ascending by report date.");
            out.observations.clear();
            return out;
        }
        if (observation.date >= anchor.date) {
            out.error = QCoreApplication::translate(
                "CftcMonitorModel", "The concentration series overlaps the transported recent observations.");
            out.observations.clear();
            return out;
        }
        QJsonObject cell;
        cell.insert(QStringLiteral("value"), values[i]);
        observation.concentration_gross_4_long = cftc_number(cell, QStringLiteral("value"));
        if (!observation.concentration_gross_4_long) {
            out.error = QCoreApplication::translate("CftcMonitorModel",
                                                    "The concentration series carries a non-numeric value.");
            out.observations.clear();
            return out;
        }
        observation.contract_code = anchor.contract_code;
        observation.report_basis = anchor.report_basis;
        observation.longs = QVector<std::optional<double>>(anchor.longs.size());
        observation.shorts = QVector<std::optional<double>>(anchor.shorts.size());
        older.append(observation);
        previous = observation.date;
    }
    older += parsed_rows.observations;
    out.observations = older;
    return out;
}

/// Parse one `cot_monitor` payload into monitor entries. `family`,
/// `futures_only` and `principal_participant_key` are the caller's request
/// identity; a payload that declares a different family/basis is refused, and a
/// market whose rows fail parsing/provenance/basis checks becomes an explicit
/// Unavailable entry rather than an empty success.
inline CftcMonitorModel cftc_parse_monitor_payload(const QJsonObject& data, CftcFamily family, bool futures_only,
                                                   const QDate& evaluation_date,
                                                   const QString& principal_participant_key) {
    CftcMonitorModel model;
    const QString declared_family = data.value(QStringLiteral("report_family")).toString();
    if (declared_family.isEmpty()) {
        model.error =
            QCoreApplication::translate("CftcMonitorModel", "The monitor payload does not state its report family.");
        return model;
    }
    if (declared_family != cftc_family_code(family)) {
        model.error = QCoreApplication::translate("CftcMonitorModel",
                                                  "The monitor returned the %1 report family, not the requested %2.")
                          .arg(declared_family, cftc_family_code(family));
        return model;
    }
    const QString basis_code = data.value(QStringLiteral("report_basis")).toString();
    const QString expected_basis =
        futures_only ? QStringLiteral("futures_only") : QStringLiteral("futures_and_options_combined");
    if (basis_code.isEmpty()) {
        model.error =
            QCoreApplication::translate("CftcMonitorModel", "The monitor payload does not state its report basis.");
        return model;
    }
    if (basis_code != expected_basis) {
        model.error = QCoreApplication::translate("CftcMonitorModel",
                                                  "The monitor returned the %1 report basis, not the requested %2.")
                          .arg(basis_code, expected_basis);
        return model;
    }
    const QJsonArray markets = data.value(QStringLiteral("markets")).toArray();
    if (markets.isEmpty()) {
        model.error = QCoreApplication::translate("CftcMonitorModel", "The monitor returned no market entries.");
        return model;
    }

    for (const QJsonValue& value : markets) {
        const QJsonObject object = value.toObject();
        CftcMonitorEntry entry;
        entry.market_key = object.value(QStringLiteral("market_key")).toString();
        const CftcMarketDefinition definition = cftc_market_definition(entry.market_key);
        entry.label = definition.label;
        entry.asset_class = definition.asset_class;
        entry.known_market = cftc_market_is_known(entry.market_key);
        entry.family = family;
        entry.futures_only = futures_only;
        entry.contract_code = object.value(QStringLiteral("contract_code")).toString();
        entry.provider_status = object.value(QStringLiteral("status")).toString();
        entry.status = cftc_monitor_status_from_code(entry.provider_status);
        entry.status_detail = object.value(QStringLiteral("refresh_error")).toString().trimmed();
        entry.archive_rows = object.value(QStringLiteral("archive_rows")).toInt();

        // A known market must declare its code; the payload is never allowed to
        // leave the identity to be synthesized from the observation rows.
        // Unknown-market entries intentionally carry no code and keep their
        // explicit `Unknown market` presentation.
        if (entry.known_market && entry.contract_code.isEmpty()) {
            entry.status = CftcMonitorStatus::Unavailable;
            entry.status_detail = QCoreApplication::translate(
                "CftcMonitorModel", "The monitor payload does not declare the market's CFTC contract-market code.");
            entry.alerts = cftc_build_alerts(entry);
            entry.requires_attention = !entry.alerts.isEmpty();
            model.entries.append(entry);
            continue;
        }

        const QJsonArray rows = object.value(QStringLiteral("rows")).toArray();
        if (rows.isEmpty()) {
            entry.alerts = cftc_build_alerts(entry);
            entry.requires_attention = !entry.alerts.isEmpty();
            model.entries.append(entry);
            continue;
        }

        const CftcHistory history = cftc_parse_history(rows, family);
        if (!history.error.isEmpty()) {
            entry.status = CftcMonitorStatus::Unavailable;
            entry.status_detail = history.error;
            entry.alerts = cftc_build_alerts(entry);
            entry.requires_attention = !entry.alerts.isEmpty();
            model.entries.append(entry);
            continue;
        }
        const QString basis_error = cftc_history_basis_error(history.observations, futures_only);
        if (!basis_error.isEmpty()) {
            entry.status = CftcMonitorStatus::Unavailable;
            entry.status_detail = basis_error;
            entry.alerts = cftc_build_alerts(entry);
            entry.requires_attention = !entry.alerts.isEmpty();
            model.entries.append(entry);
            continue;
        }
        const CftcMonitorObservations merged =
            cftc_monitor_observations(history, object.value(QStringLiteral("concentration_history")).toObject());
        if (!merged.error.isEmpty()) {
            entry.status = CftcMonitorStatus::Unavailable;
            entry.status_detail = merged.error;
            entry.alerts = cftc_build_alerts(entry);
            entry.requires_attention = !entry.alerts.isEmpty();
            model.entries.append(entry);
            continue;
        }
        // The market entry and every observation must agree on one exact CFTC
        // contract-market code. The frozen engine only verifies that a history
        // is internally consistent, so a payload that consistently labels the
        // wrong contract would otherwise be interpreted as the requested
        // market.
        QString history_code = entry.contract_code;
        bool code_conflict = false;
        for (const CftcObservation& observation : merged.observations) {
            if (observation.contract_code.isEmpty()) {
                code_conflict = true;
                break;
            }
            if (history_code.isEmpty())
                history_code = observation.contract_code;
            else if (observation.contract_code != history_code) {
                code_conflict = true;
                break;
            }
        }
        if (code_conflict) {
            entry.status = CftcMonitorStatus::Unavailable;
            entry.status_detail = QCoreApplication::translate(
                "CftcMonitorModel",
                "The market entry and its observations do not carry one CFTC contract-market code.");
            entry.alerts = cftc_build_alerts(entry);
            entry.requires_attention = !entry.alerts.isEmpty();
            model.entries.append(entry);
            continue;
        }

        CftcInterpretationInput input;
        input.family = family;
        input.observations_family_code = cftc_family_code(family);
        input.observations = merged.observations;
        input.report_basis_code = basis_code.isEmpty() ? expected_basis : basis_code;
        input.evaluation_date = evaluation_date;
        input.config = cftc_default_interpretation_config();
        const CftcInterpretationResult result = cftc_interpret(input);

        entry.interpreted = true;
        entry.report_date = result.report_date;
        entry.report_date_available = result.report_date_available;
        entry.report_outdated = result.report_outdated;
        entry.report_age_available = result.report_age_available;
        entry.report_age_days = result.report_age_days;
        entry.has_open_interest = result.open_interest_available;
        entry.open_interest = result.open_interest;
        entry.open_interest_readings = result.open_interest_readings;
        entry.market_states = result.market_context;
        for (const CftcUnavailableRecord& record : result.unavailable) {
            if (record.participant_key.isEmpty() && record.state_family == QLatin1String("NET_EXPOSURE")) {
                entry.report_unavailable = true;
                entry.report_unavailable_reason = record.reason;
                entry.interpreted = false;
                break;
            }
        }
        if (!principal_participant_key.isEmpty()) {
            for (const CftcParticipantInterpretation& participant : result.participants) {
                if (participant.participant_key != principal_participant_key)
                    continue;
                entry.principal_participant_key = participant.participant_key;
                entry.principal_label = participant.label;
                entry.principal_available = true;
                entry.has_net_position = participant.net_available;
                entry.net_position = participant.net_position;
                entry.has_net_pct_oi = participant.has_net_pct_oi;
                entry.net_pct_oi = participant.net_pct_oi;
                entry.has_percentile = participant.historical_percentile_available;
                entry.percentile = participant.percentile;
                entry.percentile_reference_count = participant.percentile_reference_count;
                entry.flow_readings = participant.flow_readings;
                for (const CftcInterpretationState& state : participant.states) {
                    if (state.available)
                        entry.principal_states.append(state);
                }
                break;
            }
            if (!entry.report_unavailable) {
                for (const CftcUnavailableRecord& record : result.unavailable) {
                    if (record.participant_key == principal_participant_key &&
                        record.state_family == QLatin1String("HISTORICAL_RELATIVE_STATE")) {
                        entry.principal_historical_unavailable = true;
                        entry.principal_historical_unavailable_reason = record.reason;
                        break;
                    }
                }
            }
        }
        entry.alerts = cftc_build_alerts(entry);
        entry.requires_attention = !entry.alerts.isEmpty();
        model.entries.append(entry);
    }
    return model;
}

} // namespace fincept::services
