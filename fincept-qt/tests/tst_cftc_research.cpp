// tests/tst_cftc_research.cpp
//
// CFTC research workspace (2026-10): the parser's set-aside rules
// (services/economics/CftcMetricModel.h), the participant percentile trail
// (services/economics/CftcMonitorModel.h), the plain-language conclusions
// (screens/economics/panels/CftcResearchText.h) and the overview / market
// projections (screens/economics/panels/CftcResearchModel.h), and the standard
// labels (level band, flow type, change size, the speculators' lean,
// one-sided markets, crowding, side flip) under rule set v3 (52-report basis). Header-only over Qt Core; no app
// sources (tests/ HARD RULE).
#include "screens/economics/panels/CftcResearchModel.h"
#include "screens/economics/panels/CftcResearchText.h"
#include "services/economics/CftcMetricModel.h"
#include "services/economics/CftcMonitorModel.h"

#include <QRegularExpression>
#include <QtTest>

using namespace fincept::services;
using namespace fincept::screens;

namespace {

/// A Legacy canonical row (commercial, non_commercial, non_reportable).
QJsonObject legacy_row(const QDate& date, double oi, double c_long, double c_short, double nc_long, double nc_short,
                       double nr_long, double nr_short, const QString& basis = QStringLiteral("FutOnly")) {
    QJsonObject row;
    row[QStringLiteral("report_date_as_yyyy_mm_dd")] = date.toString(Qt::ISODate);
    row[QStringLiteral("cftc_contract_market_code")] = QStringLiteral("088691");
    row[QStringLiteral("market_and_exchange_names")] = QStringLiteral("GOLD - COMMODITY EXCHANGE INC.");
    if (!basis.isEmpty())
        row[QStringLiteral("futonly_or_combined")] = basis;
    row[QStringLiteral("open_interest_all")] = oi;
    row[QStringLiteral("commercial_long")] = c_long;
    row[QStringLiteral("commercial_short")] = c_short;
    row[QStringLiteral("non_commercial_long")] = nc_long;
    row[QStringLiteral("non_commercial_short")] = nc_short;
    row[QStringLiteral("non_reportable_long")] = nr_long;
    row[QStringLiteral("non_reportable_short")] = nr_short;
    row[QStringLiteral("concentration_gross_4_long")] = 20.0;
    row[QStringLiteral("concentration_gross_4_short")] = 30.0;
    return row;
}

/// 200 weekly reports where the Non-Commercial net position sets a new high
/// every week (so it is the highest of every 52-report reference), the
/// Commercial net position a new low, and Non-Reportable oscillates.
QJsonArray trending_rows(int count = 200) {
    QJsonArray rows;
    const QDate start(2022, 1, 4);
    for (int i = 0; i < count; ++i) {
        rows.append(
            legacy_row(start.addDays(7 * i), 1000.0, 400.0, 500.0 + i, 300.0 + i, 200.0, 100.0, 100.0 + (i % 7)));
    }
    return rows;
}

CftcHistory parse(const QJsonArray& rows) {
    return cftc_parse_history(rows, CftcFamily::Legacy);
}

CftcInterpretationResult interpret(const CftcHistory& history, const QDate& evaluation) {
    return cftc_interpret(cftc_make_interpretation_input(CftcFamily::Legacy, history.observations,
                                                         QStringLiteral("futures_only"), {}, QString(), false, false,
                                                         evaluation));
}

QVector<CftcParticipantTrail> trails_of(const CftcHistory& history, const CftcInterpretationResult& result) {
    QVector<CftcParticipantTrail> out;
    const auto participants = cftc_family_participants(CftcFamily::Legacy);
    for (int i = 0; i < participants.size(); ++i)
        out << cftc_participant_trail(history.observations, i, participants[i].key, result.config);
    return out;
}

/// Trading, forecast and recommendation words: never in any text.
const QRegularExpression& forbidden_vocabulary() {
    static const QRegularExpression pattern(
        QStringLiteral("\\b(buy|sell|hold|signal|forecast|predict|expect|likely|should|squeeze|overbought|oversold|"
                       "reversal|opportunit|conviction|recommend|target|smart money|bullish|bearish)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

/// The standard label words: they belong to the labels, their definitions and
/// their tooltips, never to the numbers-first statements.
const QRegularExpression& label_vocabulary() {
    static const QRegularExpression pattern(QStringLiteral("\\b(leaning|crowded|one-sided)\\b"),
                                            QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

CftcHorizonFlowReading flow_reading(double long_flow, double long_rank, double short_flow, double short_rank) {
    CftcHorizonFlowReading reading;
    reading.horizon_reports = 1;
    reading.evaluated = true;
    reading.has_long_flow = reading.has_short_flow = reading.has_net_flow = true;
    reading.long_flow = long_flow;
    reading.short_flow = short_flow;
    reading.net_flow = long_flow - short_flow;
    reading.has_long_rank = reading.has_short_rank = true;
    reading.long_rank = long_rank;
    reading.short_rank = short_rank;
    return reading;
}

/// A speculative participant with a level and a 4-week net change.
CftcParticipantInterpretation speculator(double net, double percentile, double four_week_net = 0.0,
                                         double four_week_rank = 0.0) {
    CftcParticipantInterpretation p;
    p.participant_key = QStringLiteral("non_commercial");
    p.has_net_pct_oi = true;
    p.net_pct_oi = net;
    p.historical_percentile_available = true;
    p.percentile = percentile;
    p.percentile_reference_count = 52;
    CftcHorizonFlowReading four;
    four.horizon_reports = 4;
    four.evaluated = true;
    four.has_net_flow = true;
    four.net_flow = four_week_net;
    four.has_net_rank = true;
    four.net_rank = four_week_rank;
    p.flow_readings << four;
    return p;
}

QVector<CftcMonitorHistoryPoint> weekly_path(const QVector<double>& values) {
    QVector<CftcMonitorHistoryPoint> path;
    const QDate start(2025, 1, 7);
    for (int i = 0; i < values.size(); ++i)
        path << CftcMonitorHistoryPoint{start.addDays(7 * i), true, values[i]};
    return path;
}

} // namespace

class TstCftcResearch : public QObject {
    Q_OBJECT
  private slots:
    // Parser and basis.
    void impossible_report_is_set_aside_and_the_rest_kept();
    void leg_over_open_interest_respects_the_rounding_tolerance();
    void non_finite_and_undated_reports_are_set_aside();
    void nothing_usable_is_an_error();
    void repeated_rows_collapse_and_contradictions_are_set_aside();
    void family_mismatch_still_refuses_the_payload();
    void partial_dates_are_invalid_and_slash_dates_read();
    void report_basis_is_inherited_or_set_aside();
    // Trail.
    void trail_matches_the_engine_percentile_rule();
    void tail_run_counts_consecutive_weeks_and_breaks_on_gaps();
    void weeks_ago_readings_need_an_unbroken_weekly_run();
    // Text.
    void tail_wording_follows_the_net_sign();
    void percentile_text_never_rounds_onto_a_limit();
    void change_wording_follows_sign_and_size();
    void conclusion_covers_every_group_at_a_tail();
    void old_report_is_stated_first();
    void no_forecast_or_recommendation_vocabulary();
    // Labels.
    void level_band_boundaries();
    void level_side_text_names_the_side();
    void flow_type_names_the_side_that_moved();
    void quiet_legs_with_a_notable_net_change_name_the_driver();
    void change_size_follows_the_engine_cut_offs();
    void positioning_bias_rule_and_reasons();
    void one_sided_needs_three_years_on_one_side();
    void crowding_follows_the_engine_state_and_carries_the_caveat();
    void side_flip_counts_the_weeks_on_the_other_side();
    // Model.
    void rows_keep_availability_states_apart();
    void behind_markets_are_named_and_left_out_of_this_week();
    void percentile_series_equals_the_engine_helper();
    void concentration_cells_follow_the_engine();
    void weekly_move_ranking_orders_by_size();
    void market_table_carries_percentiles_and_concentration();
    void flow_cells_break_on_a_missing_weekly_step();
    void historical_context_finds_records_and_last_beyond();
};

// ── Parser ──────────────────────────────────────────────────────────────────

void TstCftcResearch::impossible_report_is_set_aside_and_the_rest_kept() {
    QJsonArray rows = trending_rows(10);
    // The official 1987 crude oil row: a negative derived Non-Reportable position.
    QJsonObject bad = rows[3].toObject();
    bad[QStringLiteral("non_reportable_long")] = -8554.0;
    rows.replace(3, bad);
    const CftcHistory history = parse(rows);
    QVERIFY2(history.error.isEmpty(), qPrintable(history.error));
    QCOMPARE(history.observations.size(), 9);
    QCOMPARE(history.set_aside.size(), 1);
    QCOMPARE(history.set_aside.first().date_label, bad.value(QStringLiteral("report_date_as_yyyy_mm_dd")).toString());
    QCOMPARE(history.set_aside.first().reason, QStringLiteral("a negative Non-Reportable long position"));
    // The set-aside report leaves a gap: no observation carries its date.
    for (const auto& obs : history.observations)
        QVERIFY(obs.date_label != history.set_aside.first().date_label);
}

void TstCftcResearch::leg_over_open_interest_respects_the_rounding_tolerance() {
    QJsonArray rows = trending_rows(4);
    QJsonObject within = rows[1].toObject();
    within[QStringLiteral("commercial_long")] = 1002.0; // Combined rounding: kept
    rows.replace(1, within);
    QJsonObject over = rows[2].toObject();
    over[QStringLiteral("commercial_long")] = 1003.0;
    rows.replace(2, over);
    const CftcHistory history = parse(rows);
    QCOMPARE(history.observations.size(), 3);
    QCOMPARE(history.set_aside.size(), 1);
    QCOMPARE(history.set_aside.first().reason, QStringLiteral("a Commercial long position larger than Open Interest"));
}

void TstCftcResearch::non_finite_and_undated_reports_are_set_aside() {
    QJsonArray rows = trending_rows(6);
    QJsonObject nan = rows[1].toObject();
    nan[QStringLiteral("concentration_gross_4_long")] = QStringLiteral("nan");
    rows.replace(1, nan);
    QJsonObject undated = rows[2].toObject();
    undated[QStringLiteral("report_date_as_yyyy_mm_dd")] = QStringLiteral("not-a-date");
    rows.replace(2, undated);
    const CftcHistory history = parse(rows);
    QVERIFY(history.error.isEmpty());
    QCOMPARE(history.observations.size(), 4);
    QCOMPARE(history.set_aside.size(), 2);
    QStringList reasons;
    for (const auto& item : history.set_aside)
        reasons << item.reason;
    QVERIFY(reasons.join(QLatin1Char('|')).contains(QStringLiteral("non-finite value in concentration_gross_4_long")));
    QVERIFY(reasons.join(QLatin1Char('|')).contains(QStringLiteral("report date cannot be read")));
}

void TstCftcResearch::nothing_usable_is_an_error() {
    QJsonArray rows = trending_rows(2);
    for (int i = 0; i < rows.size(); ++i) {
        QJsonObject row = rows[i].toObject();
        row[QStringLiteral("open_interest_all")] = -1.0;
        rows.replace(i, row);
    }
    const CftcHistory history = parse(rows);
    QVERIFY(!history.error.isEmpty());
    QVERIFY(history.observations.isEmpty());
    QVERIFY(history.error.contains(QStringLiteral("None of the 2 provider reports")));
    QVERIFY(!parse(QJsonArray()).error.isEmpty());
}

void TstCftcResearch::repeated_rows_collapse_and_contradictions_are_set_aside() {
    QJsonArray rows = trending_rows(5);
    rows.append(rows[1]); // identical repeat
    QJsonObject contradicting = rows[3].toObject();
    contradicting[QStringLiteral("open_interest_all")] = 1500.0;
    rows.append(contradicting);
    const CftcHistory history = parse(rows);
    QVERIFY(history.error.isEmpty());
    QCOMPARE(history.observations.size(), 4); // 5 dates, one contradicted date set aside
    QCOMPARE(history.set_aside.size(), 1);
    QVERIFY(history.set_aside.first().reason.contains(QStringLiteral("2 different rows")));
}

void TstCftcResearch::family_mismatch_still_refuses_the_payload() {
    QJsonObject tff;
    tff[QStringLiteral("report_date_as_yyyy_mm_dd")] = QStringLiteral("2026-09-15");
    tff[QStringLiteral("open_interest_all")] = 5000.0;
    tff[QStringLiteral("leveraged_funds_long")] = 800.0;
    tff[QStringLiteral("leveraged_funds_short")] = 1400.0;
    const CftcHistory history = parse(QJsonArray{tff});
    QVERIFY(!history.error.isEmpty());
    QVERIFY(history.error.contains(QStringLiteral("report family")));
}

void TstCftcResearch::partial_dates_are_invalid_and_slash_dates_read() {
    QVERIFY(!cftc_parse_report_date(QStringLiteral("2026-09")).isValid());
    QVERIFY(!cftc_parse_report_date(QStringLiteral("2026")).isValid());
    QCOMPARE(cftc_parse_report_date(QStringLiteral("2026-09-15T00:00:00.000")), QDate(2026, 9, 15));
    QCOMPARE(cftc_parse_report_date(QStringLiteral("09/15/2026")), QDate(2026, 9, 15));
}

void TstCftcResearch::report_basis_is_inherited_or_set_aside() {
    QJsonArray rows = trending_rows(4);
    QJsonObject unstated = rows[0].toObject();
    unstated.remove(QStringLiteral("futonly_or_combined"));
    rows.replace(0, unstated);
    QJsonObject other = rows[1].toObject();
    other[QStringLiteral("futonly_or_combined")] = QStringLiteral("Combined");
    rows.replace(1, other);
    CftcHistory history = parse(rows);
    QVERIFY(cftc_apply_report_basis(history, /*futures_only=*/true).isEmpty());
    QCOMPARE(history.observations.size(), 3);
    QCOMPARE(history.basis_inherited, 1);
    QCOMPARE(history.observations.first().report_basis, QStringLiteral("FutOnly"));
    QCOMPARE(history.set_aside.size(), 1);
    QVERIFY(history.set_aside.first().reason.contains(QStringLiteral("not the requested Futures Only basis")));
    // The engine accepts the result: every row now states the requested basis.
    const CftcInterpretationResult result = interpret(history, history.observations.last().date);
    QVERIFY(result.report_basis_verified);
    // Nothing of the requested basis: an error.
    CftcHistory combined = parse(trending_rows(3));
    QVERIFY(!cftc_apply_report_basis(combined, /*futures_only=*/false).isEmpty());
}

// ── Trail ───────────────────────────────────────────────────────────────────

void TstCftcResearch::trail_matches_the_engine_percentile_rule() {
    const CftcHistory history = parse(trending_rows());
    const CftcInterpretationResult result = interpret(history, history.observations.last().date.addDays(3));
    const QVector<CftcParticipantTrail> trails = trails_of(history, result);
    for (int p = 0; p < result.participants.size(); ++p) {
        QVERIFY(result.participants[p].historical_percentile_available);
        QVERIFY(trails[p].percentile_now.has_value());
        QCOMPARE(*trails[p].percentile_now, result.participants[p].percentile);
        // 4 reports back equals the helper evaluated on that report.
        const QVector<CftcDatedValue> series = cftc_metric_series(history.observations, p, CftcMetricKind::NetPctOi);
        const QDate four_back = history.observations[history.observations.size() - 5].date;
        const CftcTrailingPercentile expected =
            cftc_trailing_percentile_at(series, four_back, result.config.history_window);
        QVERIFY(trails[p].percentile_4r.has_value());
        QCOMPARE(*trails[p].percentile_4r, expected.percentile);
    }
    QCOMPARE(*trails[1].percentile_now, 1.0); // Non-Commercial: new high every week
    QCOMPARE(*trails[0].percentile_now, 0.0); // Commercial: new low every week
    QCOMPARE(trails[1].net_pct_oi.size(), kCftcMonitorHistoryMaxPoints < 200 ? kCftcMonitorHistoryMaxPoints : 200);
}

void TstCftcResearch::tail_run_counts_consecutive_weeks_and_breaks_on_gaps() {
    const CftcHistory history = parse(trending_rows());
    const CftcInterpretationResult result = interpret(history, history.observations.last().date);
    const QVector<CftcParticipantTrail> trails = trails_of(history, result);
    // Reports 52..199 all have a 52-report reference and are all in the tail;
    // the run reaches the start of the computable window, so it is "at least".
    QCOMPARE(trails[1].tail_run_reports, 148);
    QVERIFY(trails[1].tail_run_capped);
    // A two-week gap three reports back breaks the run there.
    QJsonArray gapped = trending_rows();
    gapped.removeAt(196);
    const CftcHistory gap_history = parse(gapped);
    const CftcInterpretationResult gap_result = interpret(gap_history, gap_history.observations.last().date);
    const QVector<CftcParticipantTrail> gap_trails = trails_of(gap_history, gap_result);
    QCOMPARE(gap_trails[1].tail_run_reports, 3);
    QVERIFY(!gap_trails[1].tail_run_capped);
    // Non-Reportable is not in a tail: no run.
    QVERIFY(trails[2].percentile_now.has_value());
}

void TstCftcResearch::weeks_ago_readings_need_an_unbroken_weekly_run() {
    // "4 weeks ago" counts weeks: across a gap the reading 4 reports back is
    // further back than 4 weeks and is absent, as every 4-week flow is.
    QJsonArray gapped = trending_rows();
    gapped.removeAt(197); // two reports before the latest
    const CftcHistory history = parse(gapped);
    const CftcInterpretationResult result = interpret(history, history.observations.last().date);
    const QVector<CftcParticipantTrail> trails = trails_of(history, result);
    QVERIFY(trails[1].percentile_now.has_value());
    QVERIFY(trails[1].percentile_1r.has_value()); // the last step is weekly
    QVERIFY(!trails[1].percentile_4r.has_value());
    QVERIFY(!trails[1].percentile_13r.has_value());
    // Without a gap all three exist.
    const CftcHistory full = parse(trending_rows());
    const QVector<CftcParticipantTrail> full_trails = trails_of(full, interpret(full, full.observations.last().date));
    QVERIFY(full_trails[1].percentile_4r.has_value());
    QVERIFY(full_trails[1].percentile_13r.has_value());
}

// ── Text ────────────────────────────────────────────────────────────────────

void TstCftcResearch::tail_wording_follows_the_net_sign() {
    QCOMPARE(cftc_tail_adjective(CftcLevelTail::High, 12.0), QStringLiteral("unusually large"));
    QCOMPARE(cftc_tail_adjective(CftcLevelTail::High, -12.0), QStringLiteral("unusually small"));
    QCOMPARE(cftc_tail_adjective(CftcLevelTail::Low, 12.0), QStringLiteral("unusually small"));
    QCOMPARE(cftc_tail_adjective(CftcLevelTail::Low, -12.0), QStringLiteral("unusually large"));
    QCOMPARE(cftc_tail_phrase(CftcLevelTail::Low, -3.0), QStringLiteral("unusually large net short"));
    QCOMPARE(cftc_tail_phrase(CftcLevelTail::None, 3.0), QStringLiteral("net long"));
    QCOMPARE(cftc_net_position_text(38.24), QStringLiteral("net long 38.2% of open interest"));
    QCOMPARE(cftc_net_position_text(-5.08), QStringLiteral("net short 5.1% of open interest"));
}

void TstCftcResearch::percentile_text_never_rounds_onto_a_limit() {
    QCOMPARE(cftc_percentile_ordinal(0.103), QStringLiteral("10.3 percentile"));
    QCOMPARE(cftc_percentile_ordinal(0.897), QStringLiteral("89.7 percentile"));
    QCOMPARE(cftc_percentile_ordinal(0.96), QStringLiteral("96th percentile"));
    QCOMPARE(cftc_percentile_ordinal(0.012), QStringLiteral("1st percentile"));
    QCOMPARE(cftc_percentile_ordinal(0.123), QStringLiteral("12th percentile"));
    QCOMPARE(cftc_percentile_ordinal(1.0), QStringLiteral("the highest"));
    QCOMPARE(cftc_percentile_ordinal(0.0), QStringLiteral("the lowest"));
    QCOMPARE(cftc_share_text(0.997), QStringLiteral("more than 99%"));
    QCOMPARE(cftc_share_text(0.003), QStringLiteral("less than 1%"));
    QCOMPARE(cftc_level_rank_text(1.0, 52), QStringLiteral("higher than every one of the previous 52 weekly reports"));
    QCOMPARE(cftc_level_rank_text(0.04, 52), QStringLiteral("lower than 96% of the previous 52 weekly reports"));
}

void TstCftcResearch::change_wording_follows_sign_and_size() {
    QCOMPARE(cftc_net_change_phrase(2.4, 10.0, 12.4), QStringLiteral("net long grew by 2.4% of open interest"));
    QCOMPARE(cftc_net_change_phrase(-2.4, 14.0, 11.6), QStringLiteral("net long shrank by 2.4% of open interest"));
    QCOMPARE(cftc_net_change_phrase(-1.2, -4.0, -5.2), QStringLiteral("net short grew by 1.2% of open interest"));
    QCOMPARE(cftc_net_change_phrase(1.7, -12.9, -11.2), QStringLiteral("net short shrank by 1.7% of open interest"));
    QVERIFY(cftc_net_change_phrase(3.0, -1.0, 2.0).startsWith(QStringLiteral("switched from net short to net long")));
    // Rule set v3: the multiple of the typical change of that length.
    const CftcInterpretationConfig v3;
    QCOMPARE(cftc_move_size_text(0.954, 4, true, 2.0, v3),
             QStringLiteral("a large change, 2.0 times the typical 4-week change of the last 52 weeks"));
    QVERIFY(cftc_move_size_text(0.80, 4, true, 1.28, v3).startsWith(QStringLiteral("a notable change, 1.3 times")));
    QCOMPARE(cftc_move_size_text(0.60, 1, true, 0.84, v3),
             QStringLiteral("an ordinary change, 0.8 times the typical one-week change of the last 52 weeks"));
    QVERIFY(cftc_move_size_text(0.2, 13, true, 0.26, v3).startsWith(QStringLiteral("a small change, 0.3 times")));
    // A rank of 1 from a very large multiple is still a multiple, not a flat reference.
    QCOMPARE(cftc_move_size_text(1.0, 1, true, 9.4, v3),
             QStringLiteral("a large change, 9.4 times the typical one-week change of the last 52 weeks"));
    // A reference without any change: no multiple exists.
    QCOMPARE(cftc_move_size_text(1.0, 4, false, 0.0, v3),
             QStringLiteral("a large change: there was no change in the previous 52 weeks"));
    QCOMPARE(cftc_move_size_text(0.0, 1, false, 0.0, v3), QStringLiteral("no change, as in the previous 52 weeks"));
    // The v2 preset compares with earlier changes of the same length; a rank of
    // 1 (every comparable change smaller) never reads "100%".
    const CftcInterpretationConfig v2 = cftc_v2_interpretation_config();
    QCOMPARE(cftc_move_size_text(0.25, 1, false, 0.0, v2),
             QStringLiteral("a small change, smaller than 75% of comparable one-week changes"));
    QCOMPARE(cftc_move_size_text(1.0, 4, false, 0.0, v2),
             QStringLiteral("a large change, bigger than all comparable 4-week changes"));
    QCOMPARE(cftc_rank_comparison(QStringLiteral("higher"), 0.997, QStringLiteral("earlier reports")),
             QStringLiteral("higher than nearly all earlier reports"));
    QCOMPARE(cftc_rank_position(0.3, QStringLiteral("earlier reports")),
             QStringLiteral("lower than 70% of earlier reports"));
    QCOMPARE(cftc_level_rank_text(0.997, 52),
             QStringLiteral("higher than nearly every one of the previous 52 weekly reports"));
}

void TstCftcResearch::conclusion_covers_every_group_at_a_tail() {
    const CftcHistory history = parse(trending_rows());
    const CftcInterpretationResult result = interpret(history, history.observations.last().date.addDays(3));
    CftcConclusionInput input;
    input.family = CftcFamily::Legacy;
    input.result = &result;
    input.trails = trails_of(history, result);
    input.horizon = 4;
    const CftcConclusion conclusion = cftc_compose_conclusion(input);
    QVERIFY2(conclusion.headline.startsWith(QStringLiteral("Non-Commercial: unusually large net long (the highest over "
                                                           "52 weeks)")),
             qPrintable(conclusion.headline));
    QVERIFY2(conclusion.headline.contains(QStringLiteral("Commercial: unusually large net short (the lowest)")),
             qPrintable(conclusion.headline));
    const QString all = conclusion.points.join(QLatin1Char('\n'));
    QVERIFY2(all.contains(QStringLiteral("Non-Commercial: net long 29.9% of open interest — unusually large: higher "
                                         "than every one of the previous 52 weekly reports. In the top 10% for at "
                                         "least 148 consecutive weeks.")),
             qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("Commercial: net short 29.9% of open interest — unusually large: lower than "
                                         "every one of the previous 52 weekly reports")),
             qPrintable(all));
    // The 4-week change of the headline group carries its legs and its size: a
    // steady +0.1 a week is 0.4 over 4 weeks, twice the typical 4-week change
    // (root mean square of the weekly changes, 0.1, x sqrt(4)).
    QVERIFY2(all.contains(QStringLiteral("Non-Commercial: net long grew by 0.4% of open interest over 4 weeks (longs "
                                         "+0.4%, shorts 0.0%) — a large change, 2.0 times the typical 4-week change of "
                                         "the last 52 weeks.")),
             qPrintable(all));
    QVERIFY(all.contains(QStringLiteral("Open interest: 1,000 contracts")));
    // No price was supplied: one brief "not assessed" note, no price sentence.
    QVERIFY(!all.contains(QStringLiteral("Price (")));
}

void TstCftcResearch::old_report_is_stated_first() {
    const CftcHistory history = parse(trending_rows());
    const CftcInterpretationResult result = interpret(history, history.observations.last().date.addDays(60));
    QVERIFY(result.report_outdated);
    CftcConclusionInput input;
    input.result = &result;
    input.trails = trails_of(history, result);
    const CftcConclusion conclusion = cftc_compose_conclusion(input);
    QVERIFY(conclusion.points.first().startsWith(QStringLiteral("Old report: the latest available report is dated")));
}

void TstCftcResearch::no_forecast_or_recommendation_vocabulary() {
    const CftcHistory history = parse(trending_rows());
    for (int days : {3, 60}) {
        const CftcInterpretationResult result = interpret(history, history.observations.last().date.addDays(days));
        for (int horizon : {1, 4, 13}) {
            CftcConclusionInput input;
            input.result = &result;
            input.trails = trails_of(history, result);
            input.horizon = horizon;
            input.price_unavailable_note = QStringLiteral("no price");
            const CftcConclusion conclusion = cftc_compose_conclusion(input);
            QStringList texts = conclusion.points;
            texts << conclusion.headline << conclusion.not_assessed;
            for (const QString& text : std::as_const(texts)) {
                QVERIFY2(!forbidden_vocabulary().match(text).hasMatch(), qPrintable(text));
                QVERIFY2(!label_vocabulary().match(text).hasMatch(), qPrintable(text));
            }
        }
    }
    QVERIFY(!forbidden_vocabulary().match(cftc_method_note()).hasMatch());
    // Every label and its tooltip sentence stays free of trading vocabulary.
    const CftcInterpretationConfig config;
    QStringList labels;
    for (const auto& p : {speculator(12.0, 0.8), speculator(12.0, 0.8, -3.0, 0.95), speculator(-8.0, 0.2),
                          speculator(-8.0, 0.6), speculator(0.0, 0.5)}) {
        const CftcBias bias = cftc_positioning_bias(p, config);
        labels << cftc_bias_label(bias) << cftc_bias_reason(bias, p, config);
    }
    for (CftcCrowding crowding : {CftcCrowding::Long, CftcCrowding::Short})
        labels << cftc_crowding_label(crowding) << cftc_crowding_reason(crowding, speculator(1.0, 0.5), config);
    for (int type = 0; type <= static_cast<int>(CftcFlowType::BothCutting); ++type)
        labels << cftc_flow_label(static_cast<CftcFlowType>(type));
    labels << cftc_side_flip_reason({1, 12, false}) << cftc_side_flip_reason({-1, 3, true});
    labels << cftc_one_sided_label({1, 156}) << cftc_one_sided_reason({1, 156}) << cftc_one_sided_reason({-1, 156});
    for (double net : {5.0, -5.0})
        for (CftcLevelBand band : {CftcLevelBand::VeryLow, CftcLevelBand::Low, CftcLevelBand::Normal,
                                   CftcLevelBand::High, CftcLevelBand::VeryHigh})
            labels << cftc_level_band_side_label(band, net);
    for (const QString& text : std::as_const(labels))
        QVERIFY2(!forbidden_vocabulary().match(text).hasMatch(), qPrintable(text));
}

// ── Labels ──────────────────────────────────────────────────────────────────

void TstCftcResearch::level_band_boundaries() {
    const CftcInterpretationConfig config;
    QCOMPARE(cftc_level_band(1.0, config), CftcLevelBand::VeryHigh);
    QCOMPARE(cftc_level_band(0.90, config), CftcLevelBand::VeryHigh);
    QCOMPARE(cftc_level_band(0.899, config), CftcLevelBand::High);
    QCOMPARE(cftc_level_band(0.70, config), CftcLevelBand::High);
    QCOMPARE(cftc_level_band(0.699, config), CftcLevelBand::Normal);
    QCOMPARE(cftc_level_band(0.5, config), CftcLevelBand::Normal);
    QCOMPARE(cftc_level_band(0.301, config), CftcLevelBand::Normal);
    QCOMPARE(cftc_level_band(0.30, config), CftcLevelBand::Low);
    QCOMPARE(cftc_level_band(0.101, config), CftcLevelBand::Low);
    QCOMPARE(cftc_level_band(0.10, config), CftcLevelBand::VeryLow);
    QCOMPARE(cftc_level_band(0.0, config), CftcLevelBand::VeryLow);
    QCOMPARE(cftc_level_band_label(CftcLevelBand::VeryHigh), QStringLiteral("VERY HIGH"));
    QCOMPARE(cftc_level_band_label(CftcLevelBand::Normal), QStringLiteral("NORMAL"));
    QCOMPARE(cftc_level_band_label(CftcLevelBand::VeryLow), QStringLiteral("VERY LOW"));
}

void TstCftcResearch::level_side_text_names_the_side() {
    // A high level of a net-short group is a smaller net short.
    QCOMPARE(cftc_level_side_text(CftcLevelBand::VeryHigh, -12.0), QStringLiteral("least short"));
    QCOMPARE(cftc_level_side_text(CftcLevelBand::High, -12.0), QStringLiteral("less short"));
    QCOMPARE(cftc_level_side_text(CftcLevelBand::Low, -12.0), QStringLiteral("more short"));
    QCOMPARE(cftc_level_side_text(CftcLevelBand::VeryLow, -12.0), QStringLiteral("most short"));
    QCOMPARE(cftc_level_side_text(CftcLevelBand::VeryHigh, 12.0), QStringLiteral("most long"));
    QCOMPARE(cftc_level_side_text(CftcLevelBand::VeryLow, 12.0), QStringLiteral("least long"));
    QVERIFY(cftc_level_side_text(CftcLevelBand::Normal, -12.0).isEmpty());
    QVERIFY(cftc_level_side_text(CftcLevelBand::VeryHigh, 0.0).isEmpty());
    QCOMPARE(cftc_level_band_side_label(CftcLevelBand::VeryHigh, -3.0), QStringLiteral("VERY HIGH · LEAST SHORT"));
    QCOMPARE(cftc_level_band_side_label(CftcLevelBand::Normal, -3.0), QStringLiteral("NORMAL"));
}

void TstCftcResearch::flow_type_names_the_side_that_moved() {
    const CftcInterpretationConfig config;
    QCOMPARE(cftc_flow_type(flow_reading(0.5, 0.74, -0.4, 0.70), config), CftcFlowType::Quiet);
    QCOMPARE(cftc_flow_type(flow_reading(0.5, 0.75, -0.1, 0.30), config), CftcFlowType::LongBuild);
    QCOMPARE(cftc_flow_type(flow_reading(-0.9, 0.95, 0.1, 0.30), config), CftcFlowType::LongLiquidation);
    QCOMPARE(cftc_flow_type(flow_reading(0.1, 0.30, 0.8, 0.80), config), CftcFlowType::ShortBuild);
    QCOMPARE(cftc_flow_type(flow_reading(0.1, 0.30, -0.8, 0.80), config), CftcFlowType::ShortCovering);
    QCOMPARE(cftc_flow_type(flow_reading(0.6, 0.80, 0.7, 0.85), config), CftcFlowType::BothAdding);
    QCOMPARE(cftc_flow_type(flow_reading(-0.6, 0.80, -0.7, 0.85), config), CftcFlowType::BothCutting);
    // Opposite notable legs: the larger size names it, then the larger amount.
    QCOMPARE(cftc_flow_type(flow_reading(0.9, 0.95, -0.5, 0.80), config), CftcFlowType::LongBuild);
    QCOMPARE(cftc_flow_type(flow_reading(-0.5, 0.80, 0.9, 0.95), config), CftcFlowType::ShortBuild);
    QCOMPARE(cftc_flow_type(flow_reading(0.2, 1.0, -0.5, 1.0), config), CftcFlowType::ShortCovering);
    // A leg that did not move never names the flow, whatever its rank.
    QCOMPARE(cftc_flow_type(flow_reading(0.0, 1.0, 0.0, 0.2), config), CftcFlowType::Quiet);
    // Missing pieces are unavailable, never quiet.
    CftcHorizonFlowReading missing = flow_reading(0.5, 0.9, 0.1, 0.1);
    missing.has_short_rank = false;
    QCOMPARE(cftc_flow_type(missing, config), CftcFlowType::Unavailable);
    CftcHorizonFlowReading not_evaluated = flow_reading(0.5, 0.9, 0.1, 0.1);
    not_evaluated.evaluated = false;
    QCOMPARE(cftc_flow_type(not_evaluated, config), CftcFlowType::Unavailable);
    QVERIFY(cftc_flow_label(CftcFlowType::Unavailable).isEmpty());
    QCOMPARE(cftc_flow_label(CftcFlowType::ShortCovering), QStringLiteral("SHORT COVERING"));
    QCOMPARE(cftc_flow_direction(CftcFlowType::LongBuild), 1);
    QCOMPARE(cftc_flow_direction(CftcFlowType::ShortCovering), 1);
    QCOMPARE(cftc_flow_direction(CftcFlowType::LongLiquidation), -1);
    QCOMPARE(cftc_flow_direction(CftcFlowType::ShortBuild), -1);
    QCOMPARE(cftc_flow_direction(CftcFlowType::BothAdding), 0);
    QCOMPARE(cftc_flow_direction(CftcFlowType::Quiet), 0);
}

void TstCftcResearch::quiet_legs_with_a_notable_net_change_name_the_driver() {
    const CftcInterpretationConfig config;
    auto with_net = [](double long_flow, double long_rank, double short_flow, double short_rank, double net_rank) {
        CftcHorizonFlowReading reading = flow_reading(long_flow, long_rank, short_flow, short_rank);
        reading.has_net_rank = true;
        reading.net_rank = net_rank;
        return reading;
    };
    // The official Australian dollar Commercial reading of 6 Oct 2026: longs
    // +9.2% (rank 0.68), shorts -3.3% (rank 0.26), net +12.5% LARGE. Not QUIET:
    // the longs moved the net position most.
    QCOMPARE(cftc_flow_type(with_net(9.2, 0.68, -3.3, 0.26, 0.93), config), CftcFlowType::LongBuild);
    // The shorts drove it.
    QCOMPARE(cftc_flow_type(with_net(0.3, 0.40, -0.9, 0.70, 0.80), config), CftcFlowType::ShortCovering);
    QCOMPARE(cftc_flow_type(with_net(-0.2, 0.30, 0.9, 0.70, 0.80), config), CftcFlowType::ShortBuild);
    QCOMPARE(cftc_flow_type(with_net(-0.9, 0.70, 0.2, 0.30, 0.80), config), CftcFlowType::LongLiquidation);
    // Both legs added; the longs added more, so the net rose through them.
    QCOMPARE(cftc_flow_type(with_net(0.9, 0.70, 0.2, 0.30, 0.80), config), CftcFlowType::LongBuild);
    // A net change below NOTABLE stays QUIET; so does a reading without a net size.
    QCOMPARE(cftc_flow_type(with_net(9.2, 0.68, -3.3, 0.26, 0.74), config), CftcFlowType::Quiet);
    QCOMPARE(cftc_flow_type(flow_reading(9.2, 0.68, -3.3, 0.26), config), CftcFlowType::Quiet);
    // Equal contributions: the longs name it.
    QCOMPARE(cftc_flow_type(with_net(0.5, 0.50, -0.5, 0.50, 0.80), config), CftcFlowType::LongBuild);
}

void TstCftcResearch::change_size_follows_the_engine_cut_offs() {
    const CftcInterpretationConfig config;
    QCOMPARE(cftc_change_size(false, 0.99, config), CftcChangeSize::Unavailable);
    QCOMPARE(cftc_change_size(true, 0.90, config), CftcChangeSize::Large);
    QCOMPARE(cftc_change_size(true, 0.899, config), CftcChangeSize::Notable);
    QCOMPARE(cftc_change_size(true, 0.75, config), CftcChangeSize::Notable);
    QCOMPARE(cftc_change_size(true, 0.749, config), CftcChangeSize::Ordinary);
    QCOMPARE(cftc_change_size_label(CftcChangeSize::Large), QStringLiteral("LARGE"));
    QCOMPARE(cftc_change_size_label(CftcChangeSize::Notable), QStringLiteral("NOTABLE"));
    QVERIFY(cftc_change_size_label(CftcChangeSize::Ordinary).isEmpty());
    // In rule set v3 the cut-offs are about 1.15 and 1.645 typical changes.
    QVERIFY(std::erf(1.16 / std::sqrt(2.0)) >= config.material_move_percentile);
    QVERIFY(std::erf(1.14 / std::sqrt(2.0)) < config.material_move_percentile);
    QVERIFY(std::erf(1.65 / std::sqrt(2.0)) >= config.large_move_percentile);
    QVERIFY(std::erf(1.64 / std::sqrt(2.0)) < config.large_move_percentile);
}

void TstCftcResearch::positioning_bias_rule_and_reasons() {
    const CftcInterpretationConfig config;
    auto bias = [&](const CftcParticipantInterpretation& p) { return cftc_positioning_bias(p, config); };
    // Net long at or above the middle of the year.
    QCOMPARE(bias(speculator(12.0, 0.8)), CftcBias::LeaningLong);
    QCOMPARE(bias(speculator(12.0, 0.5)), CftcBias::LeaningLong);
    QCOMPARE(bias(speculator(12.0, 0.8, 3.0, 0.99)), CftcBias::LeaningLong);  // adding sharply
    QCOMPARE(bias(speculator(12.0, 0.8, -3.0, 0.89)), CftcBias::LeaningLong); // cutting, but not by a LARGE amount
    QCOMPARE(bias(speculator(12.0, 0.8, -3.0, 0.90)), CftcBias::Neutral);     // cutting by a LARGE amount
    QCOMPARE(bias(speculator(12.0, 0.49)), CftcBias::Neutral);                // long, but low for the year
    // The mirror image.
    QCOMPARE(bias(speculator(-8.0, 0.2)), CftcBias::LeaningShort);
    QCOMPARE(bias(speculator(-8.0, 0.5)), CftcBias::LeaningShort);
    QCOMPARE(bias(speculator(-8.0, 0.2, -2.0, 0.99)), CftcBias::LeaningShort);
    QCOMPARE(bias(speculator(-8.0, 0.2, 2.0, 0.95)), CftcBias::Neutral);
    QCOMPARE(bias(speculator(-8.0, 0.5, 2.0, 0.95)), CftcBias::Neutral);
    QCOMPARE(bias(speculator(-8.0, 0.51)), CftcBias::Neutral);
    QCOMPARE(bias(speculator(0.0, 0.5)), CftcBias::Neutral);
    // No 4-week reading: the level alone decides.
    CftcParticipantInterpretation no_four = speculator(12.0, 0.8, -3.0, 0.99);
    no_four.flow_readings[0].evaluated = false;
    QCOMPARE(bias(no_four), CftcBias::LeaningLong);
    // Without a year of history there is no bias.
    CftcParticipantInterpretation young = speculator(12.0, 0.8);
    young.historical_percentile_available = false;
    QCOMPARE(bias(young), CftcBias::Unavailable);
    QVERIFY(cftc_bias_reason(CftcBias::Unavailable, young, config).isEmpty());
    QVERIFY(cftc_bias_label(CftcBias::Unavailable).isEmpty());
    // Each reason restates the rule branch that applied.
    auto reason = [&](const CftcParticipantInterpretation& p) { return cftc_bias_reason(bias(p), p, config); };
    QCOMPARE(reason(speculator(12.0, 0.8)),
             QStringLiteral("LEANING LONG: net long 12.0% of open interest, higher than 80% of the previous 52 "
                            "weekly reports, and the net long has not shrunk by a large amount over 4 weeks. It "
                            "describes positions, not prices."));
    QCOMPARE(reason(speculator(-8.0, 0.2)),
             QStringLiteral("LEANING SHORT: net short 8.0% of open interest, lower than 80% of the previous 52 "
                            "weekly reports, and the net short has not shrunk by a large amount over 4 weeks. It "
                            "describes positions, not prices."));
    QVERIFY(reason(speculator(12.0, 0.8, -3.0, 0.95))
                .endsWith(QStringLiteral("but the net long shrank by a large amount over 4 weeks.")));
    QVERIFY(reason(speculator(-8.0, 0.5, 2.0, 0.95))
                .endsWith(QStringLiteral("but the net short shrank by a large amount over 4 weeks.")));
    QCOMPARE(reason(speculator(12.0, 0.3)),
             QStringLiteral("NEUTRAL: net long 12.0% of open interest, but lower than 70% of the previous 52 weekly "
                            "reports — the side and the level of the year point different ways."));
    QCOMPARE(reason(speculator(0.0, 0.5)), QStringLiteral("NEUTRAL: flat (long equals short)."));
}

void TstCftcResearch::one_sided_needs_three_years_on_one_side() {
    QVector<double> values(157, -4.0);
    CftcOneSided one = cftc_one_sided(weekly_path(values));
    QCOMPARE(one.direction, -1);
    QCOMPARE(one.reports, 156);
    QCOMPARE(cftc_one_sided_label(one), QStringLiteral("ONE-SIDED"));
    QCOMPARE(cftc_one_sided_reason(one),
             QStringLiteral("ONE-SIDED: net short in each of the previous 156 reports (about 3 years). In this "
                            "market the side is the usual state, so the side alone says little; the level shows "
                            "whether the position is larger or smaller than usual."));
    // One report on the other side, a flat or a missing report, or fewer than
    // 157 reports: not one-sided.
    QVector<double> crossed = values;
    crossed[0] = 0.5;
    QCOMPARE(cftc_one_sided(weekly_path(crossed)).direction, 0);
    QVector<double> flat = values;
    flat[80] = 0.0;
    QCOMPARE(cftc_one_sided(weekly_path(flat)).direction, 0);
    QVector<CftcMonitorHistoryPoint> missing = weekly_path(values);
    missing[10].has_net_pct_oi = false;
    QCOMPARE(cftc_one_sided(missing).direction, 0);
    QCOMPARE(cftc_one_sided(weekly_path(QVector<double>(156, -4.0))).direction, 0);
    // A crossing older than the window does not count.
    QVector<double> older(170, 3.0);
    older[5] = -1.0;
    QCOMPARE(cftc_one_sided(weekly_path(older)).direction, 1);
    QVERIFY(cftc_one_sided_label({}).isEmpty());
    QVERIFY(cftc_one_sided_reason({}).isEmpty());
}

void TstCftcResearch::crowding_follows_the_engine_state_and_carries_the_caveat() {
    const CftcInterpretationConfig config;
    CftcParticipantInterpretation p = speculator(20.0, 0.95);
    QCOMPARE(cftc_crowding(p), CftcCrowding::None); // the label only restates the engine state
    CftcInterpretationState state;
    state.state_id = QStringLiteral("CROWDED_LONG");
    p.states << state;
    QCOMPARE(cftc_crowding(p), CftcCrowding::Long);
    QCOMPARE(cftc_crowding_label(CftcCrowding::Long), QStringLiteral("CROWDED LONG"));
    QCOMPARE(cftc_crowding_label(CftcCrowding::Short), QStringLiteral("CROWDED SHORT"));
    QVERIFY(cftc_crowding_label(CftcCrowding::None).isEmpty());
    p.terminology_caveat_code = QStringLiteral("broad_category_caveat");
    const QString reason = cftc_crowding_reason(CftcCrowding::Long, p, config);
    QVERIFY2(reason.startsWith(QStringLiteral("CROWDED LONG: net long and in the top 10% of the previous 52 weekly "
                                              "reports. Non-Commercial is the CFTC's broad non-commercial category")),
             qPrintable(reason));
    p.terminology_caveat_code = QStringLiteral("not_all_outright_speculation_caveat");
    QVERIFY(cftc_crowding_reason(CftcCrowding::Short, p, config)
                .endsWith(QStringLiteral("not all positions in it are outright speculation.")));
    p.terminology_caveat_code.clear();
    QCOMPARE(cftc_crowding_reason(CftcCrowding::Short, p, config),
             QStringLiteral("CROWDED SHORT: net short and in the bottom 10% of the previous 52 weekly reports."));
    QVERIFY(cftc_crowding_reason(CftcCrowding::None, p, config).isEmpty());
    // On the trending fixture the engine sets the state for the speculators only.
    const CftcHistory history = parse(trending_rows());
    const CftcInterpretationResult result = interpret(history, history.observations.last().date.addDays(3));
    QCOMPARE(cftc_crowding(result.participants[1]), CftcCrowding::Long); // Non-Commercial, highest
    QCOMPARE(cftc_crowding(result.participants[0]), CftcCrowding::None); // Commercial, lowest
}

void TstCftcResearch::side_flip_counts_the_weeks_on_the_other_side() {
    // Ten weeks net short from the start of the path, then net long.
    CftcSideFlip flip = cftc_side_flip(weekly_path({-5, -4, -3, -3, -2, -2, -1, -1, -1, -0.5, 0.4}));
    QCOMPARE(flip.direction, 1);
    QCOMPARE(flip.prior_weeks, 10);
    QVERIFY(flip.prior_capped);
    QCOMPARE(cftc_side_flip_label(flip), QStringLiteral("FLIPPED LONG"));
    QCOMPARE(cftc_side_flip_reason(flip),
             QStringLiteral("Turned net long this week after at least 10 consecutive weeks net short."));
    // Three weeks net long after a net-short stretch, then net short.
    flip = cftc_side_flip(weekly_path({-1, -2, 1, 2, 3, -0.2}));
    QCOMPARE(flip.direction, -1);
    QCOMPARE(flip.prior_weeks, 3);
    QVERIFY(!flip.prior_capped);
    QCOMPARE(cftc_side_flip_label(flip), QStringLiteral("FLIPPED SHORT"));
    QCOMPARE(cftc_side_flip_reason(flip),
             QStringLiteral("Turned net short this week after 3 consecutive weeks net long."));
    // No flip: same side, a flat report, a missing report, a gap before the
    // latest report, or a single report.
    QCOMPARE(cftc_side_flip(weekly_path({1, 2, 3})).direction, 0);
    QCOMPARE(cftc_side_flip(weekly_path({1, 0, -1})).direction, 0);
    QVector<CftcMonitorHistoryPoint> missing = weekly_path({1, 2, -1});
    missing[1].has_net_pct_oi = false;
    QCOMPARE(cftc_side_flip(missing).direction, 0);
    QVector<CftcMonitorHistoryPoint> gap = weekly_path({1, 2, -1});
    gap[2].date = gap[2].date.addDays(7);
    QCOMPARE(cftc_side_flip(gap).direction, 0);
    QCOMPARE(cftc_side_flip(weekly_path({-1})).direction, 0);
    // A gap inside the earlier run stops the count there ("at least").
    QVector<CftcMonitorHistoryPoint> inner_gap = weekly_path({1, 1, 1, 1, -1});
    for (int i = 0; i < 2; ++i)
        inner_gap[i].date = inner_gap[i].date.addDays(-7);
    flip = cftc_side_flip(inner_gap);
    QCOMPARE(flip.direction, -1);
    QCOMPARE(flip.prior_weeks, 2);
    QVERIFY(flip.prior_capped);
}

// ── Model ───────────────────────────────────────────────────────────────────

void TstCftcResearch::rows_keep_availability_states_apart() {
    QJsonObject payload;
    payload[QStringLiteral("report_family")] = QStringLiteral("legacy");
    payload[QStringLiteral("report_basis")] = QStringLiteral("futures_only");
    QJsonArray markets;
    auto market = [](const QString& key, const QString& code, const QString& status, const QJsonArray& rows) {
        QJsonObject m;
        m[QStringLiteral("market_key")] = key;
        m[QStringLiteral("contract_code")] = code;
        m[QStringLiteral("status")] = status;
        m[QStringLiteral("rows")] = rows;
        return m;
    };
    QJsonArray gold = trending_rows();
    QJsonArray silver = trending_rows();
    for (int i = 0; i < silver.size(); ++i) {
        QJsonObject row = silver[i].toObject();
        row[QStringLiteral("cftc_contract_market_code")] = QStringLiteral("084691");
        silver.replace(i, row);
    }
    markets.append(market(QStringLiteral("gold"), QStringLiteral("088691"), QStringLiteral("current"), gold));
    QJsonObject failed =
        market(QStringLiteral("silver"), QStringLiteral("084691"), QStringLiteral("archive_only"), silver);
    failed[QStringLiteral("refresh_error")] = QStringLiteral("HTTP 503");
    markets.append(failed);
    markets.append(market(QStringLiteral("euro"), QStringLiteral("099741"), QStringLiteral("no_data"), {}));
    payload[QStringLiteral("markets")] = markets;
    const QDate evaluation = QDate(2022, 1, 4).addDays(7 * 199 + 3);
    const CftcMonitorModel model = cftc_parse_monitor_payload(payload, CftcFamily::Legacy, true, evaluation,
                                                              cftc_principal_participant_key(CftcFamily::Legacy));
    QVERIFY2(model.error.isEmpty(), qPrintable(model.error));
    const QVector<CftcMarketRow> rows = cftc_market_rows(model.entries);
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows[0].market_key, QStringLiteral("gold"));
    QCOMPARE(rows[0].availability, CftcRowAvailability::Current);
    QCOMPARE(rows[1].availability, CftcRowAvailability::Stored);
    QCOMPARE(rows[1].availability_note, QStringLiteral("stored data; refresh failed: HTTP 503"));
    QCOMPARE(rows[2].availability, CftcRowAvailability::NotCovered);
    // A read of the stored reports alone (no refresh asked for) is not flagged.
    {
        QJsonObject stored_only = payload;
        QJsonArray only;
        only.append(market(QStringLiteral("silver"), QStringLiteral("084691"), QStringLiteral("archive_only"), silver));
        stored_only[QStringLiteral("markets")] = only;
        const CftcMonitorModel stored_model = cftc_parse_monitor_payload(
            stored_only, CftcFamily::Legacy, true, evaluation, cftc_principal_participant_key(CftcFamily::Legacy));
        const QVector<CftcMarketRow> stored_rows = cftc_market_rows(stored_model.entries);
        QCOMPARE(stored_rows.size(), 1);
        QCOMPARE(stored_rows[0].availability, CftcRowAvailability::Current);
        QVERIFY(stored_rows[0].availability_note.isEmpty());
    }
    QCOMPARE(rows[0].cells.size(), 3);
    QCOMPARE(rows[0].tail, CftcLevelTail::High);
    // The speculators' labels: highest of the year, net long and adding.
    QCOMPARE(rows[0].band, std::optional<CftcLevelBand>(CftcLevelBand::VeryHigh));
    QCOMPARE(rows[0].bias, CftcBias::LeaningLong);
    QVERIFY(rows[0].bias_reason.startsWith(QStringLiteral("LEANING LONG: net long 29.9% of open interest")));
    // Net long in all 200 reports: ONE-SIDED, with its reason.
    QCOMPARE(rows[0].one_sided.direction, 1);
    QVERIFY(rows[0].one_sided_reason.startsWith(QStringLiteral("ONE-SIDED: net long in each of the previous 156 "
                                                               "reports")));
    QCOMPARE(rows[0].crowding, CftcCrowding::Long);
    QVERIFY(rows[0].crowding_reason.contains(QStringLiteral("broad non-commercial category")));
    QCOMPARE(rows[0].flip.direction, 0);
    QVERIFY(!rows[0].turning);
    // A steady +0.1 a week is one typical weekly change: quiet and ordinary.
    QCOMPARE(rows[0].flow_1w, CftcFlowType::Quiet);
    QCOMPARE(rows[0].size_1w, CftcChangeSize::Ordinary);
    QCOMPARE(rows[0].tags, (QStringList{QStringLiteral("CROWDED LONG"), QStringLiteral("ONE-SIDED")}));
    QVERIFY(!rows[0].behind && !rows[1].behind); // both at the same report
    const CftcOverviewFigures figures = cftc_overview_figures(rows);
    QCOMPARE(figures.markets_listed, 2); // a market the report does not publish is not listed
    QCOMPARE(figures.markets_not_covered, 1);
    QCOMPARE(figures.markets_current, 1);
    QCOMPARE(figures.markets_stored, 1);
    QCOMPARE(figures.leaning_long.size(), 2);
    QVERIFY(figures.leaning_short.isEmpty());
    QCOMPARE(figures.one_sided.size(), 2);
    QCOMPARE(figures.markets_behind, 0);
    QCOMPARE(figures.crowded_long.size(), 2);
    QVERIFY(figures.crowded_short.isEmpty());
    QVERIFY(figures.flipped.isEmpty());
    QVERIFY(figures.large_move_markets.isEmpty());
}

void TstCftcResearch::behind_markets_are_named_and_left_out_of_this_week() {
    // Gold has the report of week 199, Silver only week 198 (a read of some
    // markets only): Silver is behind, named, and left out of this week's
    // figures and ranking; the latest report is Gold's.
    QJsonObject payload;
    payload[QStringLiteral("report_family")] = QStringLiteral("legacy");
    payload[QStringLiteral("report_basis")] = QStringLiteral("futures_only");
    auto market = [](const QString& key, const QString& code, const QJsonArray& rows) {
        QJsonObject m;
        m[QStringLiteral("market_key")] = key;
        m[QStringLiteral("contract_code")] = code;
        m[QStringLiteral("status")] = QStringLiteral("current");
        m[QStringLiteral("rows")] = rows;
        return m;
    };
    QJsonArray silver = trending_rows(199);
    for (int i = 0; i < silver.size(); ++i) {
        QJsonObject row = silver[i].toObject();
        row[QStringLiteral("cftc_contract_market_code")] = QStringLiteral("084691");
        silver.replace(i, row);
    }
    payload[QStringLiteral("markets")] =
        QJsonArray{market(QStringLiteral("gold"), QStringLiteral("088691"), trending_rows()),
                   market(QStringLiteral("silver"), QStringLiteral("084691"), silver)};
    const QDate newest = QDate(2022, 1, 4).addDays(7 * 199);
    const CftcMonitorModel model = cftc_parse_monitor_payload(payload, CftcFamily::Legacy, true, newest.addDays(3),
                                                              cftc_principal_participant_key(CftcFamily::Legacy));
    QVERIFY2(model.error.isEmpty(), qPrintable(model.error));
    const QVector<CftcMarketRow> rows = cftc_market_rows(model.entries);
    QCOMPARE(rows.size(), 2);
    const CftcMarketRow& gold = rows[0].market_key == QLatin1String("gold") ? rows[0] : rows[1];
    const CftcMarketRow& behind = rows[0].market_key == QLatin1String("gold") ? rows[1] : rows[0];
    QCOMPARE(behind.availability, CftcRowAvailability::Current); // ten days old: not an old report
    QVERIFY(behind.behind);
    QVERIFY(!gold.behind);
    QCOMPARE(behind.behind_note, QStringLiteral("report of %1; other markets already have the report of %2")
                                     .arg(newest.addDays(-7).toString(QStringLiteral("d MMM yyyy")),
                                          newest.toString(QStringLiteral("d MMM yyyy"))));
    QVERIFY(!cftc_row_at_newest_report(behind));
    const CftcOverviewFigures figures = cftc_overview_figures(rows);
    QCOMPARE(figures.latest_report, newest);
    QCOMPARE(figures.markets_behind, 1);
    QCOMPARE(figures.behind, QStringList{behind.label});
    QCOMPARE(figures.leaning_long.size(), 2); // positions are counted, with the market named as behind
    QCOMPARE(figures.markets_at_newest, 1);
    QVERIFY(!figures.latest_report_behind);
    // A behind market's flip and LARGE change are from its own, earlier week:
    // this week's figures leave them out.
    QVector<CftcMarketRow> tagged = rows;
    for (auto& row : tagged) {
        row.flip.direction = 1;
        row.size_1w = CftcChangeSize::Large;
    }
    const CftcOverviewFigures tagged_figures = cftc_overview_figures(tagged);
    QCOMPARE(tagged_figures.flipped, QStringList{gold.label});
    QCOMPARE(tagged_figures.large_move_markets, QStringList{gold.label});
    // An asset-class tab holding only behind markets still names their report, as behind.
    const CftcOverviewFigures behind_only = cftc_overview_figures(QVector<CftcMarketRow>{behind});
    QCOMPARE(behind_only.latest_report, newest.addDays(-7));
    QVERIFY(behind_only.latest_report_behind);
    QCOMPARE(behind_only.markets_at_newest, 0);
    const CftcMoveRanking ranking = cftc_weekly_move_ranking(rows);
    QCOMPARE(ranking.rises.size(), 1);
    QCOMPARE(ranking.rises.first().market_key, QStringLiteral("gold"));
    // The automatic refresh is due by the market that is behind, not by Gold.
    QCOMPARE(cftc_overview_oldest_report(rows), newest.addDays(-7));
    QVector<CftcMarketRow> unavailable = rows;
    unavailable[0].availability = CftcRowAvailability::Unavailable;
    unavailable[0].report_date = newest.addDays(-700);
    QCOMPARE(cftc_overview_oldest_report(unavailable), unavailable[1].report_date);
    QVERIFY(!cftc_overview_oldest_report({}).isValid());
    // A market that is old is old, not behind.
    QVector<CftcMarketRow> with_old = rows;
    for (auto& row : with_old)
        if (row.market_key == QLatin1String("silver"))
            row.availability = CftcRowAvailability::Old;
    cftc_mark_behind(with_old);
    for (const auto& row : with_old)
        QVERIFY(!row.behind);
}

void TstCftcResearch::percentile_series_equals_the_engine_helper() {
    QJsonArray rows = trending_rows();
    // A missing leg in the middle: the reading is absent, never zero.
    QJsonObject missing = rows[170].toObject();
    missing.remove(QStringLiteral("non_commercial_short"));
    rows.replace(170, missing);
    const CftcHistory history = parse(rows);
    const QVector<CftcSeriesPoint> series = cftc_percentile_series(history.observations, 1);
    const QVector<CftcDatedValue> values = cftc_metric_series(history.observations, 1, CftcMetricKind::NetPctOi);
    QCOMPARE(series.size(), history.observations.size());
    for (int i = 0; i < series.size(); ++i) {
        const CftcTrailingPercentile expected = cftc_trailing_percentile_at(values, history.observations[i].date,
                                                                            CftcInterpretationConfig{}.history_window);
        QCOMPARE(series[i].has_value, expected.available);
        if (expected.available)
            QCOMPARE(series[i].value, expected.percentile);
    }
    QVERIFY(!series[170].has_value);
}

void TstCftcResearch::flow_cells_break_on_a_missing_weekly_step() {
    QJsonArray rows = trending_rows(20);
    rows.removeAt(10); // a 14-day step
    const CftcHistory history = parse(rows);
    const QVector<CftcFlowCell> cells = cftc_weekly_flow_cells(history.observations, 1, 52);
    QCOMPARE(cells.size(), history.observations.size());
    QVERIFY(!cells.first().has_value);
    QVERIFY(!cells[10].has_value); // the report after the gap
    QVERIFY(cells[11].has_value);
    QVERIFY(qAbs(cells[11].value - 0.1) < 1e-9); // +1 contract on 1,000 OI
    const QVector<CftcFlowCell> oi = cftc_weekly_flow_cells(history.observations, -1, 52);
    QVERIFY(oi[11].has_value);
    QCOMPARE(oi[11].value, 0.0);
}

void TstCftcResearch::historical_context_finds_records_and_last_beyond() {
    QJsonArray rows = trending_rows(60);
    // An early spike above today's level: today is the highest since then.
    QJsonObject spike = rows[5].toObject();
    spike[QStringLiteral("non_commercial_long")] = 900.0;
    rows.replace(5, spike);
    const CftcHistory history = parse(rows);
    const CftcHistoricalContext context = cftc_historical_context(history.observations, 1);
    QVERIFY(context.available);
    QCOMPARE(context.record_max_date, history.observations[5].date);
    QCOMPARE(context.record_min_date, history.observations[0].date);
    QVERIFY(context.beyond_found);
    QCOMPARE(context.beyond_since, history.observations[5].date);
    QCOMPARE(context.history_start, history.observations.first().date);
}

void TstCftcResearch::concentration_cells_follow_the_engine() {
    CftcInterpretationResult result;
    CftcConcentrationAssessment gross_4_long;
    gross_4_long.field_key = QStringLiteral("concentration_gross_4_long");
    gross_4_long.has_current = true;
    gross_4_long.current = 21.4;
    gross_4_long.has_percentile = true;
    gross_4_long.percentile = 0.82;
    gross_4_long.percentile_reference_count = 52;
    gross_4_long.has_change = true;
    gross_4_long.change = 0.8;
    gross_4_long.has_change_rank = true;
    gross_4_long.change_rank = 0.80;
    gross_4_long.has_change_scale = true;
    gross_4_long.change_scale = 1.28;
    CftcConcentrationAssessment net_8_short;
    net_8_short.field_key = QStringLiteral("concentration_net_8_short");
    net_8_short.has_current = true;
    net_8_short.current = 30.0;
    net_8_short.has_percentile = true;
    net_8_short.percentile = 0.05;
    net_8_short.percentile_reference_count = 52;
    net_8_short.change_unavailable_reason = CftcUnavailableReason::BrokenReportSequence;
    result.concentration << net_8_short << gross_4_long; // engine order does not matter
    const QVector<CftcConcentrationCell> cells = cftc_concentration_cells(result);
    QCOMPARE(cells.size(), 8);
    // Panel order: 4 then 8 largest; long gross, long net, short gross, short net.
    const QStringList keys{QStringLiteral("concentration_gross_4_long"),  QStringLiteral("concentration_net_4_long"),
                           QStringLiteral("concentration_gross_4_short"), QStringLiteral("concentration_net_4_short"),
                           QStringLiteral("concentration_gross_8_long"),  QStringLiteral("concentration_net_8_long"),
                           QStringLiteral("concentration_gross_8_short"), QStringLiteral("concentration_net_8_short")};
    for (int i = 0; i < 8; ++i)
        QCOMPARE(cells[i].field_key, keys[i]);
    QVERIFY(cells[0].has_value);
    QCOMPARE(cells[0].band, std::optional<CftcLevelBand>(CftcLevelBand::High));
    QCOMPARE(cells[0].size, CftcChangeSize::Notable);
    QCOMPARE(cells[0].tooltip,
             QStringLiteral(
                 "The 4 largest traders account for 21.4% of open interest on the long side (gross) — higher than "
                 "82% of the previous 52 weekly reports. This week: +0.8 percentage points — a notable "
                 "change, 1.3 times the typical one-week change of the last 52 weeks. Gross: each "
                 "trader's whole long position."));
    QCOMPARE(cells[7].band, std::optional<CftcLevelBand>(CftcLevelBand::VeryLow));
    QVERIFY(!cells[7].has_change);
    QCOMPARE(cells[7].size, CftcChangeSize::Unavailable);
    QVERIFY2(cells[7].tooltip.contains(QStringLiteral("One-week change: ")), qPrintable(cells[7].tooltip));
    QVERIFY(cells[7].tooltip.endsWith(
        QStringLiteral("Net: after offsetting each trader's own long and short positions against each other.")));
    // A figure the report does not carry stays absent, never zero.
    QVERIFY(!cells[2].has_value);
    QVERIFY(!cells[2].band.has_value());
    QVERIFY(cells[2].tooltip.isEmpty());
    // Through the engine: a constant top-4 share is NORMAL and an ordinary
    // (zero) change against a reference without any change.
    const CftcHistory history = parse(trending_rows());
    const CftcInterpretationResult engine = interpret(history, history.observations.last().date.addDays(3));
    const QVector<CftcConcentrationCell> engine_cells = cftc_concentration_cells(engine);
    QVERIFY(engine_cells[0].has_value);
    QCOMPARE(engine_cells[0].value, 20.0);
    QCOMPARE(engine_cells[0].band, std::optional<CftcLevelBand>(CftcLevelBand::Normal));
    QCOMPARE(engine_cells[0].size, CftcChangeSize::Ordinary);
    QVERIFY2(engine_cells[0].tooltip.contains(
                 QStringLiteral("This week: about unchanged — no change, as in the previous 52 weeks.")),
             qPrintable(engine_cells[0].tooltip));
    QVERIFY(!engine_cells[1].has_value); // the fixture carries no net figures
    for (const auto& cell : engine_cells)
        QVERIFY(!forbidden_vocabulary().match(cell.tooltip).hasMatch());
}

void TstCftcResearch::weekly_move_ranking_orders_by_size() {
    auto row = [](const QString& key, double flow, std::optional<double> scale, CftcChangeSize size,
                  CftcRowAvailability availability = CftcRowAvailability::Current) {
        CftcMarketRow r;
        r.market_key = key;
        r.label = key;
        r.availability = availability;
        r.net_flow_1w = flow;
        r.rank_1w = scale ? std::erf(*scale / std::sqrt(2.0)) : 1.0;
        r.scale_1w = scale;
        r.size_1w = size;
        r.flow_1w = CftcFlowType::Quiet;
        r.size_text_1w = QStringLiteral("size");
        return r;
    };
    QVector<CftcMarketRow> rows{
        row(QStringLiteral("a"), 1.0, 2.0, CftcChangeSize::Large),
        row(QStringLiteral("b"), 3.0, 1.2, CftcChangeSize::Notable),
        row(QStringLiteral("c"), 0.5, std::nullopt, CftcChangeSize::Large), // after a year without any change
        row(QStringLiteral("d"), -2.0, 1.7, CftcChangeSize::Large),
        row(QStringLiteral("e"), -0.4, 0.3, CftcChangeSize::Ordinary),
        row(QStringLiteral("j"), -1.0, 0.3, CftcChangeSize::Ordinary), // same multiple as e, larger change
        row(QStringLiteral("old"), 5.0, 3.0, CftcChangeSize::Large, CftcRowAvailability::Old),
        row(QStringLiteral("zero"), 0.0, 0.0, CftcChangeSize::Ordinary),
        row(QStringLiteral("not_covered"), 1.0, 1.0, CftcChangeSize::Ordinary, CftcRowAvailability::NotCovered),
    };
    CftcMarketRow unsized = row(QStringLiteral("unsized"), 2.0, 1.0, CftcChangeSize::Unavailable);
    unsized.rank_1w.reset();
    rows << unsized;
    const CftcMoveRanking ranking = cftc_weekly_move_ranking(rows);
    QStringList rises;
    for (const auto& bar : ranking.rises)
        rises << bar.market_key;
    QStringList falls;
    for (const auto& bar : ranking.falls)
        falls << bar.market_key;
    QCOMPARE(rises, (QStringList{QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b")}));
    QCOMPARE(falls, (QStringList{QStringLiteral("d"), QStringLiteral("j"), QStringLiteral("e")}));
    QCOMPARE(ranking.rises[1].tooltip, QStringLiteral("a: net position +1.0% of open interest since the previous "
                                                      "report (QUIET) — size."));
    const CftcMoveRanking capped = cftc_weekly_move_ranking(rows, 2);
    QCOMPARE(capped.rises.size(), 2);
    QCOMPARE(capped.falls.size(), 2);
    QCOMPARE(capped.rises.first().market_key, QStringLiteral("c"));
    // Through the model: a steady +0.1 a week is one typical weekly change.
    QJsonObject payload;
    payload[QStringLiteral("report_family")] = QStringLiteral("legacy");
    payload[QStringLiteral("report_basis")] = QStringLiteral("futures_only");
    QJsonObject gold;
    gold[QStringLiteral("market_key")] = QStringLiteral("gold");
    gold[QStringLiteral("contract_code")] = QStringLiteral("088691");
    gold[QStringLiteral("status")] = QStringLiteral("current");
    gold[QStringLiteral("rows")] = trending_rows();
    payload[QStringLiteral("markets")] = QJsonArray{gold};
    const CftcMonitorModel model =
        cftc_parse_monitor_payload(payload, CftcFamily::Legacy, true, QDate(2022, 1, 4).addDays(7 * 199 + 3),
                                   cftc_principal_participant_key(CftcFamily::Legacy));
    const CftcMoveRanking engine = cftc_weekly_move_ranking(cftc_market_rows(model.entries));
    QCOMPARE(engine.rises.size(), 1);
    QVERIFY(engine.falls.isEmpty());
    QVERIFY(engine.rises.first().has_scale);
    QVERIFY(qAbs(engine.rises.first().scale - 1.0) < 1e-9);
    QVERIFY(qAbs(engine.rises.first().net_flow - 0.1) < 1e-9);
    QCOMPARE(engine.rises.first().size, CftcChangeSize::Ordinary);
}

void TstCftcResearch::market_table_carries_percentiles_and_concentration() {
    const CftcHistory history = parse(trending_rows(60));
    const QStringList columns = cftc_market_table_columns(CftcFamily::Legacy);
    QCOMPARE(columns.mid(0, 6),
             (QStringList{QStringLiteral("date"), QStringLiteral("open interest"), QStringLiteral("Commercial long"),
                          QStringLiteral("Commercial short"), QStringLiteral("Commercial net %OI"),
                          QStringLiteral("Commercial 52-week percentile")}));
    for (const QString& column :
         {QStringLiteral("top-4 gross long %OI"), QStringLiteral("top-4 net short %OI"),
          QStringLiteral("top-8 gross short %OI"), QStringLiteral("top-8 net long %OI"), QStringLiteral("traders")})
        QVERIFY2(columns.contains(column), qPrintable(column));
    const QJsonArray rows = cftc_market_table_rows(history.observations, CftcFamily::Legacy);
    QCOMPARE(rows.size(), history.observations.size());
    const QJsonObject newest = rows.first().toObject();
    QCOMPARE(newest.value(QStringLiteral("date")).toString(), history.observations.last().date.toString(Qt::ISODate));
    // The 52-week percentile is the engine helper's value, in percent.
    const QVector<CftcSeriesPoint> series = cftc_percentile_series(history.observations, 1);
    QVERIFY(series.last().has_value);
    QCOMPARE(newest.value(QStringLiteral("Non-Commercial 52-week percentile")).toDouble(),
             std::round(series.last().value * 1000.0) / 10.0);
    QCOMPARE(newest.value(QStringLiteral("Non-Commercial 52-week percentile")).toDouble(), 100.0);
    // Before 52 earlier readings exist there is no percentile: the key is absent, not zero.
    const QJsonObject oldest = rows.last().toObject();
    QVERIFY(!oldest.contains(QStringLiteral("Non-Commercial 52-week percentile")));
    QVERIFY(rows.at(rows.size() - 53).toObject().contains(QStringLiteral("Non-Commercial 52-week percentile")));
    QVERIFY(!rows.at(rows.size() - 52).toObject().contains(QStringLiteral("Non-Commercial 52-week percentile")));
    // Concentration: the fixture's gross top-4 figures; net figures it does not carry are absent.
    QCOMPARE(newest.value(QStringLiteral("top-4 gross long %OI")).toDouble(), 20.0);
    QCOMPARE(newest.value(QStringLiteral("top-4 gross short %OI")).toDouble(), 30.0);
    QVERIFY(!newest.contains(QStringLiteral("top-4 net long %OI")));
}

QTEST_GUILESS_MAIN(TstCftcResearch)
#include "tst_cftc_research.moc"
