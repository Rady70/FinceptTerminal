// tests/tst_cftc_monitor.cpp
//
// MarketLab CFTC Batch 5: the cross-market monitor contract
// (services/economics/CftcMarketCatalog.h, services/economics/CftcMonitorModel.h)
// and its descriptive alert projection. It pins:
//   * the exact supported-market catalog identity and grouping;
//   * the closed alertable-state list and its attention classes;
//   * the projection of a monotone synthetic history into monitor entries that
//     equal the direct engine result (principal percentile, Net %OI, flows,
//     Open Interest readings and emitted states);
//   * the payload budget rule: the last 200 full observations plus the older
//     primary-concentration series must classify exactly like the full history;
//   * explicit data-quality records for unavailable/unknown/no-history/outdated
//     reports, and fail-closed payload family/basis/row validation;
//   * the deterministic attention ordering (documented class order, then label);
//   * descriptive, traceable wording with no BUY/HOLD/SELL, bullish/bearish or
//     predictive vocabulary.
// Header-only over Qt Core; no app sources (tests/ HARD RULE).
#include "services/economics/CftcMarketCatalog.h"
#include "services/economics/CftcMonitorModel.h"
#include "screens/economics/panels/CftcInterpretationPresentation.h"

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <optional>

using namespace fincept::services;
using namespace fincept::screens;

namespace {

const QString kBasis = QStringLiteral("futures_only");

QJsonObject make_legacy_row(const QDate& date, double open_interest, double net, double concentration) {
    QJsonObject row;
    row[QStringLiteral("report_date_as_yyyy_mm_dd")] = date.toString(Qt::ISODate);
    row[QStringLiteral("cftc_contract_market_code")] = QStringLiteral("088691");
    row[QStringLiteral("market_and_exchange_names")] = QStringLiteral("GOLD - COMMODITY EXCHANGE INC.");
    row[QStringLiteral("contract_units")] = QStringLiteral("(CONTRACTS OF 100 TROY OUNCES)");
    row[QStringLiteral("futonly_or_combined")] = QStringLiteral("FutOnly");
    row[QStringLiteral("open_interest_all")] = open_interest;
    row[QStringLiteral("commercial_long")] = 20000.0;
    row[QStringLiteral("commercial_short")] = 20000.0;
    row[QStringLiteral("non_commercial_long")] = (open_interest + net) / 2.0;
    row[QStringLiteral("non_commercial_short")] = (open_interest - net) / 2.0;
    row[QStringLiteral("non_reportable_long")] = 1000.0;
    row[QStringLiteral("non_reportable_short")] = 1000.0;
    row[QStringLiteral("traders_total")] = 300.0;
    row[QStringLiteral("traders_reportable_long")] = 240.0;
    row[QStringLiteral("traders_reportable_short")] = 160.0;
    row[QStringLiteral("concentration_gross_4_long")] = concentration;
    row[QStringLiteral("concentration_gross_4_short")] = 30.0;
    return row;
}

/// A weekly legacy history whose net position rises steadily and then jumps on
/// the final week: the last report is the historical maximum (extreme) and the
/// final net change is the largest in the reference (material flow).
QVector<QJsonObject> make_monotone_series(int weeks, const QDate& first) {
    QVector<QJsonObject> rows;
    rows.reserve(weeks);
    QDate date = first;
    double net = -10000.0;
    for (int i = 0; i < weeks; ++i) {
        if (i == weeks - 1)
            net += 5000.0;
        else if (i > 0)
            net += 200.0;
        const double concentration = 10.0 + 0.05 * i;
        rows.append(make_legacy_row(date, 100000.0, net, concentration));
        date = date.addDays(7);
    }
    return rows;
}

QJsonArray to_array(const QVector<QJsonObject>& rows, int from = 0, int count = -1) {
    QJsonArray array;
    const int end = count < 0 ? rows.size() : qMin(rows.size(), from + count);
    for (int i = from; i < end; ++i)
        array.append(rows[i]);
    return array;
}

QJsonObject concentration_history(const QVector<QJsonObject>& rows, int end_exclusive) {
    QJsonObject history;
    history[QStringLiteral("field")] = QStringLiteral("concentration_gross_4_long");
    QJsonArray dates;
    QJsonArray values;
    for (int i = 0; i < end_exclusive && i < rows.size(); ++i) {
        dates.append(rows[i].value(QStringLiteral("report_date_as_yyyy_mm_dd")));
        values.append(rows[i].value(QStringLiteral("concentration_gross_4_long")));
    }
    history[QStringLiteral("dates")] = dates;
    history[QStringLiteral("values")] = values;
    return history;
}

QJsonObject make_market(const QString& key, const QString& status, const QJsonArray& rows,
                        const QJsonObject& history = {}) {
    QJsonObject market;
    market[QStringLiteral("market_key")] = key;
    market[QStringLiteral("contract_code")] = QStringLiteral("088691");
    market[QStringLiteral("status")] = status;
    market[QStringLiteral("refresh_error")] = QString();
    market[QStringLiteral("archive_rows")] = rows.size();
    market[QStringLiteral("rows")] = rows;
    if (!history.isEmpty())
        market[QStringLiteral("concentration_history")] = history;
    return market;
}

QJsonObject make_payload(const QJsonArray& markets, const QString& family = QStringLiteral("legacy"),
                         const QString& basis = kBasis) {
    QJsonObject data;
    data[QStringLiteral("report_family")] = family;
    data[QStringLiteral("report_basis")] = basis;
    data[QStringLiteral("markets")] = markets;
    return data;
}

CftcMonitorEntry parse_single_market(const QJsonObject& market, const QDate& evaluation_date) {
    const QJsonArray markets{market};
    const CftcMonitorModel model = cftc_parse_monitor_payload(
        make_payload(markets), CftcFamily::Legacy, true, evaluation_date,
        cftc_principal_participant_key(CftcFamily::Legacy));
    if (model.entries.isEmpty())
        return {};
    return model.entries.first();
}

CftcInterpretationResult interpret_rows(const QJsonArray& rows, const QDate& evaluation_date) {
    CftcInterpretationInput input;
    input.family = CftcFamily::Legacy;
    input.observations_family_code = cftc_family_code(CftcFamily::Legacy);
    input.observations = cftc_parse_history(rows, CftcFamily::Legacy).observations;
    input.report_basis_code = kBasis;
    input.evaluation_date = evaluation_date;
    input.config = cftc_default_interpretation_config();
    return cftc_interpret(input);
}

QStringList active_state_ids(const QVector<CftcInterpretationState>& states) {
    QStringList ids;
    for (const CftcInterpretationState& state : states) {
        if (state.available)
            ids << state.state_id;
    }
    return ids;
}

bool contains_forbidden(const QString& text) {
    const QString lower = text.toLower();
    for (const QString& word :
         {QStringLiteral("buy"), QStringLiteral("sell"), QStringLiteral("hold"), QStringLiteral("bullish"),
          QStringLiteral("bearish"), QStringLiteral("forecast"), QStringLiteral("expected return"),
          QStringLiteral("confidence"), QStringLiteral("recommend")}) {
        if (lower.contains(word))
            return true;
    }
    return false;
}

} // namespace

class TstCftcMonitor : public QObject {
    Q_OBJECT
  private slots:
    void catalog_is_the_supported_universe();
    void asset_classes_are_complete_and_ordered();
    void alertable_states_match_the_engine_taxonomy();
    void monitor_entry_matches_the_direct_engine_result();
    void monitor_older_concentration_series_matches_full_history();
    void data_quality_states_are_explicit();
    void outdated_report_is_a_data_quality_condition();
    void payload_identity_fails_closed();
    void attention_order_is_class_then_label();
    void alerts_are_descriptive_and_traceable();
};

void TstCftcMonitor::catalog_is_the_supported_universe() {
    const QVector<CftcMarketDefinition> catalog = cftc_market_catalog();
    QCOMPARE(catalog.size(), 37);
    QSet<QString> keys;
    for (const CftcMarketDefinition& market : catalog) {
        QVERIFY(!market.key.isEmpty());
        QVERIFY(!market.label.isEmpty());
        QVERIFY(!market.asset_class.isEmpty());
        QVERIFY(!keys.contains(market.key));
        keys.insert(market.key);
    }
    QVERIFY(keys.contains(QStringLiteral("gold")));
    QVERIFY(keys.contains(QStringLiteral("s&p_500")));
    QVERIFY(keys.contains(QStringLiteral("us_dollar_index")));
    QCOMPARE(cftc_market_definition(QStringLiteral("gold")).label, QStringLiteral("Gold"));
    QVERIFY(cftc_market_is_known(QStringLiteral("treasury_bonds")));
    QVERIFY(!cftc_market_is_known(QStringLiteral("micro_gold")));
    const CftcMarketDefinition unknown = cftc_market_definition(QStringLiteral("unknown_key"));
    QCOMPARE(unknown.label, QStringLiteral("unknown_key"));
    QVERIFY(unknown.asset_class.isEmpty());
}

void TstCftcMonitor::asset_classes_are_complete_and_ordered() {
    const QStringList order = cftc_asset_class_order();
    QCOMPARE(order.size(), 8);
    for (const CftcMarketDefinition& market : cftc_market_catalog())
        QVERIFY(order.contains(market.asset_class));
    QCOMPARE(cftc_asset_class_label(QStringLiteral("metals")), QStringLiteral("Metals"));
    QCOMPARE(cftc_asset_class_label(QStringLiteral("equity_indices")), QStringLiteral("Equity Indices"));
    QCOMPARE(cftc_asset_class_label(QStringLiteral("interest_rates")), QStringLiteral("Interest Rates"));
    // Catalog order preserves the established panel grouping.
    QCOMPARE(cftc_market_catalog().first().key, QStringLiteral("gold"));
    QCOMPARE(cftc_market_catalog().last().key, QStringLiteral("us_dollar_index"));
}

void TstCftcMonitor::alertable_states_match_the_engine_taxonomy() {
    const QVector<CftcAlertRule> rules = cftc_alert_rules();
    QVERIFY(!rules.isEmpty());
    const QStringList taxonomy = cftc_interpretation_state_ids();
    QSet<QString> seen;
    for (const CftcAlertRule& rule : rules) {
        const QString id = QString::fromLatin1(rule.state_id);
        QVERIFY2(taxonomy.contains(id), qPrintable(QStringLiteral("unknown state id: ") + id));
        QVERIFY(!seen.contains(id));
        seen.insert(id);
        QCOMPARE(cftc_state_is_alertable(id), true);
        QCOMPARE(cftc_state_attention_class(id), rule.attention_class);
    }
    QCOMPARE(cftc_state_attention_class(QStringLiteral("HISTORICALLY_HIGH_NET")), CftcAttentionClass::Extreme);
    QCOMPARE(cftc_state_attention_class(QStringLiteral("EXITED_LOW_EXTREME")),
             CftcAttentionClass::ExtremeTransition);
    QCOMPARE(cftc_state_attention_class(QStringLiteral("SHORT_COVERING")), CftcAttentionClass::Repositioning);
    QCOMPARE(cftc_state_attention_class(QStringLiteral("OI_EXPANSION")), CftcAttentionClass::OpenInterest);
    QCOMPARE(cftc_state_attention_class(QStringLiteral("CONCENTRATION_RISING")),
             CftcAttentionClass::Concentration);
    // Direction levels and price relationships are deliberately not alerts.
    QVERIFY(!cftc_state_is_alertable(QStringLiteral("NET_LONG")));
    QVERIFY(!cftc_state_is_alertable(QStringLiteral("NET_SHORT")));
    QVERIFY(!cftc_state_is_alertable(QStringLiteral("NET_FLAT")));
    QVERIFY(!cftc_state_is_alertable(QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE")));
    QVERIFY(!cftc_state_is_alertable(QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP")));
    // No directional wording vocabulary is owned by this layer.
    for (const CftcAlertRule& rule : rules)
        QVERIFY(!contains_forbidden(QString::fromLatin1(rule.state_id)));
}

void TstCftcMonitor::monitor_entry_matches_the_direct_engine_result() {
    const QVector<QJsonObject> rows = make_monotone_series(200, QDate(2022, 11, 1));
    const QDate evaluation_date = QDate(2026, 9, 25);
    const QJsonObject market = make_market(QStringLiteral("gold"), QStringLiteral("updated"), to_array(rows));
    const CftcMonitorEntry entry = parse_single_market(market, evaluation_date);

    QVERIFY(entry.status_detail.isEmpty());
    QCOMPARE(entry.status, CftcMonitorStatus::Ok);
    QVERIFY(entry.known_market);
    QCOMPARE(entry.label, QStringLiteral("Gold"));
    QCOMPARE(entry.asset_class, QStringLiteral("metals"));
    QCOMPARE(entry.principal_participant_key, QStringLiteral("non_commercial"));
    QVERIFY(entry.interpreted);
    QVERIFY(!entry.report_unavailable);

    const CftcInterpretationResult direct = interpret_rows(to_array(rows), evaluation_date);
    QVERIFY(!direct.participants.isEmpty());
    const CftcParticipantInterpretation* principal = nullptr;
    for (const CftcParticipantInterpretation& participant : direct.participants) {
        if (participant.participant_key == entry.principal_participant_key)
            principal = &participant;
    }
    QVERIFY(principal != nullptr);
    QCOMPARE(principal->participant_key, entry.principal_participant_key);
    QCOMPARE(entry.has_net_pct_oi, principal->has_net_pct_oi);
    QCOMPARE(entry.net_pct_oi, principal->net_pct_oi);
    QCOMPARE(entry.has_percentile, principal->historical_percentile_available);
    QCOMPARE(entry.percentile, principal->percentile);
    QCOMPARE(entry.percentile_reference_count, principal->percentile_reference_count);
    QCOMPARE(entry.flow_readings.size(), principal->flow_readings.size());
    QCOMPARE(entry.has_open_interest, direct.open_interest_available);
    QCOMPARE(entry.open_interest_readings.size(), direct.open_interest_readings.size());
    QCOMPARE(active_state_ids(entry.principal_states), active_state_ids(principal->states));
    QCOMPARE(active_state_ids(entry.market_states), active_state_ids(direct.market_context));

    // The monotone series must produce a historical extreme, persistence and a
    // material 1-report flow, all as descriptive alerts.
    const QStringList states = active_state_ids(entry.principal_states);
    QVERIFY(states.contains(QStringLiteral("SEVERE_LONG_EXTREME")));
    QVERIFY(states.contains(QStringLiteral("HISTORICALLY_HIGH_NET")));
    QVERIFY(states.contains(QStringLiteral("PERSISTENT_HIGH_EXTREME")));
    QVERIFY(entry.requires_attention);
    QVERIFY(!entry.alerts.isEmpty());
    QSet<QString> classes;
    for (const CftcAlert& alert : entry.alerts)
        classes.insert(cftc_attention_class_code(alert.attention_class));
    QVERIFY(classes.contains(QStringLiteral("extreme")));
    QVERIFY(classes.contains(QStringLiteral("repositioning")));
}

void TstCftcMonitor::monitor_older_concentration_series_matches_full_history() {
    const int weeks = 320;
    const QVector<QJsonObject> rows = make_monotone_series(weeks, QDate(2020, 1, 7));
    const QDate evaluation_date = QDate(2026, 9, 25);
    const QJsonArray full_rows = to_array(rows);
    const CftcInterpretationResult full = interpret_rows(full_rows, evaluation_date);

    // The monitor transport: last 200 rows + the older primary-concentration
    // values.
    const int window = 200;
    const QJsonArray recent = to_array(rows, weeks - window, window);
    const QJsonObject market = make_market(QStringLiteral("gold"), QStringLiteral("updated"), recent,
                                           concentration_history(rows, weeks - window));
    const CftcMonitorEntry entry = parse_single_market(market, evaluation_date);
    QVERIFY(entry.status_detail.isEmpty());
    QVERIFY(entry.interpreted);

    // The bounded window alone would lose the older concentration reference.
    const CftcConcentrationAssessment* full_primary = nullptr;
    for (const CftcConcentrationAssessment& assessment : full.concentration) {
        if (assessment.field_key == QStringLiteral("concentration_gross_4_long"))
            full_primary = &assessment;
    }
    QVERIFY(full_primary != nullptr);
    QVERIFY(full_primary->has_percentile);
    bool full_has_concentration_state = false;
    for (const CftcInterpretationState& state : full.market_context) {
        if (state.state_id == QStringLiteral("HIGH_MARKET_CONCENTRATION"))
            full_has_concentration_state = true;
    }
    QVERIFY(full_has_concentration_state);

    bool transport_has_concentration_state = false;
    for (const CftcInterpretationState& state : entry.market_states) {
        if (state.state_id == QStringLiteral("HIGH_MARKET_CONCENTRATION"))
            transport_has_concentration_state = true;
    }
    QVERIFY(transport_has_concentration_state);

    // Participant results are identical between the full history and the
    // transported history, because every participant reference is windowed at
    // 156 reports.
    const CftcParticipantInterpretation* full_principal = nullptr;
    for (const CftcParticipantInterpretation& participant : full.participants) {
        if (participant.participant_key == entry.principal_participant_key)
            full_principal = &participant;
    }
    QVERIFY(full_principal != nullptr);
    QCOMPARE(entry.has_percentile, full_principal->historical_percentile_available);
    QCOMPARE(entry.percentile, full_principal->percentile);
    QCOMPARE(entry.percentile_reference_count, full_principal->percentile_reference_count);
    QCOMPARE(entry.has_net_pct_oi, full_principal->has_net_pct_oi);
    QCOMPARE(entry.net_pct_oi, full_principal->net_pct_oi);
    QCOMPARE(active_state_ids(entry.principal_states), active_state_ids(full_principal->states));

    // The direct merge helper must produce the same concentration percentile as
    // the full history.
    const CftcHistory parsed = cftc_parse_history(recent, CftcFamily::Legacy);
    const CftcMonitorObservations merged =
        cftc_monitor_observations(parsed, concentration_history(rows, weeks - window));
    QVERIFY(merged.error.isEmpty());
    QCOMPARE(merged.observations.size(), weeks);
    CftcInterpretationInput input;
    input.family = CftcFamily::Legacy;
    input.observations_family_code = cftc_family_code(CftcFamily::Legacy);
    input.observations = merged.observations;
    input.report_basis_code = kBasis;
    input.evaluation_date = evaluation_date;
    input.config = cftc_default_interpretation_config();
    const CftcInterpretationResult merged_result = cftc_interpret(input);
    const CftcConcentrationAssessment* merged_primary = nullptr;
    for (const CftcConcentrationAssessment& assessment : merged_result.concentration) {
        if (assessment.field_key == QStringLiteral("concentration_gross_4_long"))
            merged_primary = &assessment;
    }
    QVERIFY(merged_primary != nullptr);
    QVERIFY(merged_primary->has_percentile);
    QCOMPARE(merged_primary->percentile, full_primary->percentile);
    QCOMPARE(merged_primary->percentile_reference_count, full_primary->percentile_reference_count);
    QCOMPARE(active_state_ids(merged_result.market_context), active_state_ids(full.market_context));
}

void TstCftcMonitor::data_quality_states_are_explicit() {
    const QDate evaluation_date = QDate(2026, 9, 25);
    struct Case {
        QString status;
        CftcMonitorStatus expected;
    };
    const QVector<Case> cases = {
        {QStringLiteral("no_data"), CftcMonitorStatus::NoData},
        {QStringLiteral("no_local_history"), CftcMonitorStatus::NoLocalHistory},
        {QStringLiteral("unavailable"), CftcMonitorStatus::Unavailable},
        {QStringLiteral("unknown_market"), CftcMonitorStatus::UnknownMarket},
        {QStringLiteral("something_new"), CftcMonitorStatus::Unavailable},
    };
    for (const Case& item : cases) {
        const QJsonObject market = make_market(QStringLiteral("gold"), item.status, QJsonArray());
        const QJsonArray markets{market};
        const CftcMonitorModel model = cftc_parse_monitor_payload(
            make_payload(markets), CftcFamily::Legacy, true, evaluation_date,
            cftc_principal_participant_key(CftcFamily::Legacy));
        QCOMPARE(model.entries.size(), 1);
        const CftcMonitorEntry& entry = model.entries.first();
        QCOMPARE(entry.status, item.expected);
        QVERIFY(entry.alerts.size() >= 1);
        QCOMPARE(entry.alerts.first().attention_class, CftcAttentionClass::DataQuality);
        QVERIFY(entry.requires_attention);
        // No fabricated positioning: no Net %OI and no percentile.
        QVERIFY(!entry.has_net_pct_oi);
        QVERIFY(!entry.has_percentile);
    }

    // A stored-history fallback is itself a data-quality condition: it must
    // surface in the attention view with the provider's failure detail.
    const QVector<QJsonObject> rows = make_monotone_series(180, QDate(2022, 5, 3));
    QJsonObject archived = make_market(QStringLiteral("gold"), QStringLiteral("archive_only"), to_array(rows));
    archived[QStringLiteral("refresh_error")] = QStringLiteral("provider offline");
    const CftcMonitorModel archived_model = cftc_parse_monitor_payload(
        make_payload(QJsonArray{archived}), CftcFamily::Legacy, true, evaluation_date,
        cftc_principal_participant_key(CftcFamily::Legacy));
    QCOMPARE(archived_model.entries.size(), 1);
    const CftcMonitorEntry& archived_entry = archived_model.entries.first();
    QCOMPARE(archived_entry.status, CftcMonitorStatus::ArchiveOnly);
    QVERIFY(archived_entry.interpreted);
    QVERIFY(archived_entry.requires_attention);
    bool has_stored_alert = false;
    for (const CftcAlert& alert : archived_entry.alerts) {
        if (alert.attention_class == CftcAttentionClass::DataQuality &&
            alert.detail_reason.contains(QStringLiteral("provider offline")))
            has_stored_alert = true;
    }
    QVERIFY(has_stored_alert);
}

void TstCftcMonitor::outdated_report_is_a_data_quality_condition() {
    const QVector<QJsonObject> rows = make_monotone_series(180, QDate(2022, 5, 3));
    const QDate last_report = QDate::fromString(
        rows.last().value(QStringLiteral("report_date_as_yyyy_mm_dd")).toString(), Qt::ISODate);
    const QDate evaluation_date = last_report.addDays(30);
    const QJsonObject market = make_market(QStringLiteral("gold"), QStringLiteral("updated"), to_array(rows));
    const CftcMonitorEntry entry = parse_single_market(market, evaluation_date);
    QVERIFY(entry.interpreted);
    QVERIFY(entry.report_outdated);
    QVERIFY(entry.report_age_available);
    QCOMPARE(entry.report_age_days, 30);
    bool has_outdated_alert = false;
    for (const CftcAlert& alert : entry.alerts) {
        if (alert.attention_class == CftcAttentionClass::DataQuality && alert.detail_reason.contains(QStringLiteral("30")))
            has_outdated_alert = true;
    }
    QVERIFY(has_outdated_alert);
}

void TstCftcMonitor::payload_identity_fails_closed() {
    const QVector<QJsonObject> rows = make_monotone_series(180, QDate(2022, 5, 3));
    const QDate evaluation_date = QDate(2026, 9, 25);
    const QJsonArray markets{make_market(QStringLiteral("gold"), QStringLiteral("updated"), to_array(rows))};

    // Wrong declared family or basis at the payload level.
    const CftcMonitorModel wrong_family = cftc_parse_monitor_payload(
        make_payload(markets, QStringLiteral("tff")), CftcFamily::Legacy, true, evaluation_date,
        cftc_principal_participant_key(CftcFamily::Legacy));
    QVERIFY(!wrong_family.error.isEmpty());
    const CftcMonitorModel wrong_basis = cftc_parse_monitor_payload(
        make_payload(markets, QStringLiteral("legacy"), QStringLiteral("futures_and_options_combined")),
        CftcFamily::Legacy, true, evaluation_date, cftc_principal_participant_key(CftcFamily::Legacy));
    QVERIFY(!wrong_basis.error.isEmpty());

    // An omitted family or basis is not "unspecified": the payload must state
    // the identity it was requested for, or it is refused.
    QJsonObject without_family = make_payload(markets);
    without_family.remove(QStringLiteral("report_family"));
    QVERIFY(!cftc_parse_monitor_payload(without_family, CftcFamily::Legacy, true, evaluation_date,
                                        cftc_principal_participant_key(CftcFamily::Legacy))
                 .error.isEmpty());
    QJsonObject without_basis = make_payload(markets);
    without_basis.remove(QStringLiteral("report_basis"));
    QVERIFY(!cftc_parse_monitor_payload(without_basis, CftcFamily::Legacy, true, evaluation_date,
                                        cftc_principal_participant_key(CftcFamily::Legacy))
                 .error.isEmpty());

    // Empty market list is an error, never an empty success.
    QVERIFY(!cftc_parse_monitor_payload(make_payload(QJsonArray()), CftcFamily::Legacy, true, evaluation_date,
                                        cftc_principal_participant_key(CftcFamily::Legacy))
                 .error.isEmpty());

    // A duplicate report date in the rows makes the market unavailable.
    QJsonArray duplicated = to_array(rows, 0, 2);
    duplicated.append(rows.first());
    const CftcMonitorModel duplicate_model = cftc_parse_monitor_payload(
        make_payload(QJsonArray{make_market(QStringLiteral("gold"), QStringLiteral("updated"), duplicated)}),
        CftcFamily::Legacy, true, evaluation_date, cftc_principal_participant_key(CftcFamily::Legacy));
    QCOMPARE(duplicate_model.entries.size(), 1);
    QCOMPARE(duplicate_model.entries.first().status, CftcMonitorStatus::Unavailable);
    QVERIFY(!duplicate_model.entries.first().status_detail.isEmpty());

    // Rows without the published basis metadata are refused exactly as the
    // detailed workspace refuses them.
    QJsonArray missing_basis = to_array(rows, 0, 10);
    for (int i = 0; i < missing_basis.size(); ++i) {
        QJsonObject row = missing_basis.at(i).toObject();
        row.remove(QStringLiteral("futonly_or_combined"));
        missing_basis.replace(i, row);
    }
    const CftcMonitorModel basis_model = cftc_parse_monitor_payload(
        make_payload(QJsonArray{make_market(QStringLiteral("gold"), QStringLiteral("updated"), missing_basis)}),
        CftcFamily::Legacy, true, evaluation_date, cftc_principal_participant_key(CftcFamily::Legacy));
    QCOMPARE(basis_model.entries.size(), 1);
    QCOMPARE(basis_model.entries.first().status, CftcMonitorStatus::Unavailable);

    // The market entry's declared contract code must match every observation.
    QJsonArray wrong_history = to_array(rows, 0, 10);
    for (int i = 0; i < wrong_history.size(); ++i) {
        QJsonObject row = wrong_history.at(i).toObject();
        row[QStringLiteral("cftc_contract_market_code")] = QStringLiteral("999999");
        wrong_history.replace(i, row);
    }
    const CftcMonitorModel code_model = cftc_parse_monitor_payload(
        make_payload(QJsonArray{make_market(QStringLiteral("gold"), QStringLiteral("updated"), wrong_history)}),
        CftcFamily::Legacy, true, evaluation_date, cftc_principal_participant_key(CftcFamily::Legacy));
    QCOMPARE(code_model.entries.size(), 1);
    QCOMPARE(code_model.entries.first().status, CftcMonitorStatus::Unavailable);
    QVERIFY(!code_model.entries.first().status_detail.isEmpty());

    // A known market must declare its code; consistent rows do not authorize
    // the payload to synthesize the declaration.
    QJsonObject undeclared =
        make_market(QStringLiteral("gold"), QStringLiteral("updated"), to_array(rows, 0, 10));
    undeclared.remove(QStringLiteral("contract_code"));
    const CftcMonitorModel undeclared_model = cftc_parse_monitor_payload(
        make_payload(QJsonArray{undeclared}), CftcFamily::Legacy, true, evaluation_date,
        cftc_principal_participant_key(CftcFamily::Legacy));
    QCOMPARE(undeclared_model.entries.size(), 1);
    QCOMPARE(undeclared_model.entries.first().status, CftcMonitorStatus::Unavailable);
    QVERIFY(!undeclared_model.entries.first().status_detail.isEmpty());

    // A concentration series that names a different field than the engine's
    // primary is refused rather than silently accepted.
    QJsonObject mismatched_history = concentration_history(rows, 10);
    mismatched_history[QStringLiteral("field")] = QStringLiteral("concentration_gross_8_short");
    const CftcMonitorModel concentration_model = cftc_parse_monitor_payload(
        make_payload(QJsonArray{make_market(QStringLiteral("gold"), QStringLiteral("updated"), to_array(rows, 10),
                                             mismatched_history)}),
        CftcFamily::Legacy, true, evaluation_date, cftc_principal_participant_key(CftcFamily::Legacy));
    QCOMPARE(concentration_model.entries.size(), 1);
    QCOMPARE(concentration_model.entries.first().status, CftcMonitorStatus::Unavailable);
}

void TstCftcMonitor::attention_order_is_class_then_label() {
    auto entry_with = [](const QString& key, const QString& label, const QString& state_id,
                         CftcAttentionClass classification) {
        CftcMonitorEntry entry;
        entry.market_key = key;
        entry.label = label;
        entry.requires_attention = true;
        CftcAlert alert;
        alert.state_id = state_id;
        alert.attention_class = classification;
        entry.alerts.append(alert);
        return entry;
    };
    QVector<CftcMonitorEntry> entries;
    entries.append(entry_with(QStringLiteral("gold"), QStringLiteral("Gold"), QStringLiteral("NET_LONGWARD_SHIFT"),
                              CftcAttentionClass::Repositioning));
    entries.append(entry_with(QStringLiteral("silver"), QStringLiteral("Silver"),
                              QStringLiteral("HISTORICALLY_HIGH_NET"), CftcAttentionClass::Extreme));
    entries.append(entry_with(QStringLiteral("copper"), QStringLiteral("Copper"), QString(),
                              CftcAttentionClass::DataQuality));
    entries.append(entry_with(QStringLiteral("corn"), QStringLiteral("Corn"), QStringLiteral("OI_EXPANSION"),
                              CftcAttentionClass::OpenInterest));
    entries.append(entry_with(QStringLiteral("wheat"), QStringLiteral("Wheat"),
                              QStringLiteral("HISTORICALLY_LOW_NET"), CftcAttentionClass::Extreme));
    CftcMonitorEntry quiet;
    quiet.market_key = QStringLiteral("vix");
    quiet.label = QStringLiteral("VIX");
    entries.append(quiet);

    const QVector<int> order = cftc_attention_order(entries);
    QStringList labels;
    for (int index : order)
        labels << entries[index].label;
    QCOMPARE(labels, QStringList({QStringLiteral("Copper"), QStringLiteral("Silver"), QStringLiteral("Wheat"),
                                  QStringLiteral("Gold"), QStringLiteral("Corn")}));
    QVERIFY(!labels.contains(QStringLiteral("VIX")));

    // Reversing the input must not change the order.
    QVector<CftcMonitorEntry> reversed = entries;
    std::reverse(reversed.begin(), reversed.end());
    QStringList reversed_labels;
    for (int index : cftc_attention_order(reversed))
        reversed_labels << reversed[index].label;
    QCOMPARE(reversed_labels, labels);
}

void TstCftcMonitor::alerts_are_descriptive_and_traceable() {
    const QVector<QJsonObject> rows = make_monotone_series(200, QDate(2022, 11, 1));
    const QDate evaluation_date = QDate(2026, 9, 25);
    const CftcMonitorEntry entry =
        parse_single_market(make_market(QStringLiteral("gold"), QStringLiteral("updated"), to_array(rows)),
                            evaluation_date);
    QVERIFY(!entry.alerts.isEmpty());
    for (const CftcAlert& alert : entry.alerts) {
        QVERIFY(!alert.detail_reason.isEmpty() || !alert.state_id.isEmpty());
        // Every state alert is traceable to the exact emitted engine state.
        if (!alert.state_id.isEmpty()) {
            QVERIFY(entry.principal_states.size() + entry.market_states.size() > 0);
            bool found = false;
            for (const CftcInterpretationState& state : entry.principal_states)
                found = found || state.state_id == alert.state_id;
            for (const CftcInterpretationState& state : entry.market_states)
                found = found || state.state_id == alert.state_id;
            QVERIFY2(found, qPrintable(QStringLiteral("untraceable alert: ") + alert.state_id));
            QVERIFY(cftc_state_is_alertable(alert.state_id));
        } else {
            QVERIFY(alert.attention_class == CftcAttentionClass::DataQuality);
        }
        const QString summary = cftc_alert_summary(alert, entry.principal_label);
        QVERIFY(!summary.isEmpty());
        QVERIFY(!contains_forbidden(summary));
    }
    // The monitor's development wording must use the finalized terminology
    // mapping: Legacy BroadNonCommercial is "Non-Commercial", never
    // "Non-Commercial (Speculators)".
    const QString neutral =
        cftc_metric_participant_display_name(CftcFamily::Legacy, QStringLiteral("non_commercial"),
                                             QStringLiteral("Non-Commercial (Speculators)"));
    QCOMPARE(neutral, QStringLiteral("Non-Commercial"));
    CftcAlert participant_alert;
    participant_alert.state_id = QStringLiteral("HISTORICALLY_HIGH_NET");
    participant_alert.participant_key = QStringLiteral("non_commercial");
    participant_alert.attention_class = CftcAttentionClass::Extreme;
    participant_alert.has_percentile = true;
    participant_alert.percentile = 0.95;
    participant_alert.percentile_reference_count = 156;
    const QString state_summary = cftc_alert_summary(participant_alert, neutral);
    QVERIFY(state_summary.contains(neutral));
    QVERIFY(!state_summary.contains(QStringLiteral("Speculators")));
    // Market-level alerts carry no participant label.
    CftcAlert market_level;
    market_level.state_id = QStringLiteral("OI_EXPANSION");
    market_level.attention_class = CftcAttentionClass::OpenInterest;
    QCOMPARE(market_level.participant_key, QString());
    QVERIFY(!cftc_alert_summary(market_level, neutral).contains(neutral));
    // The projection is deterministic.
    const CftcMonitorEntry again =
        parse_single_market(make_market(QStringLiteral("gold"), QStringLiteral("updated"), to_array(rows)),
                            evaluation_date);
    QStringList first;
    QStringList second;
    for (const CftcAlert& alert : entry.alerts)
        first << cftc_alert_summary(alert, entry.principal_label);
    for (const CftcAlert& alert : again.alerts)
        second << cftc_alert_summary(alert, again.principal_label);
    QCOMPARE(first, second);
}

QTEST_GUILESS_MAIN(TstCftcMonitor)
#include "tst_cftc_monitor.moc"
