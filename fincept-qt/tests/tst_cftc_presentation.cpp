// tests/tst_cftc_presentation.cpp
//
// Batch 4B of the CFTC work: the deterministic presentation/composition layer
// (screens/economics/panels/CftcInterpretationPresentation.h) governed by
// CFTC_DESCRIPTIVE_INTERPRETATION_PLAN.md. It proves that finalized Batch 4A
// state ids map to the intended predefined wording, that headline precedence
// follows the governing order, that crowded/accumulation and crowded/unwind
// combinations render, that gross-leg mechanisms stay distinguishable, that
// 4-report/13-report disagreement is exposed as mixed, that Legacy,
// Disaggregated and TFF terminology stays correct (with no TFF
// commercial/speculator reconstruction), that both divergence directions are
// explicit and non-predictive, that missing price or insufficient history does
// not suppress available direct states, that the visible chart range cannot
// change the interpretation, and that the composed conclusion contains no
// BUY/HOLD/SELL, bullish/bearish, confidence, expected-return, forecast or AI
// language. The 2026-09-24 audit corrections add sign-aware level wording,
// explicit unavailable level/persistence/sustained statements, contribution-
// share attribution, inline MarketLab thresholds, price series/session
// disclosure, the out-of-date report flag, market concentration and a golden
// end-to-end test on 200 official Gold Legacy reports. Header-only over Qt
// Core; no app sources (tests/ HARD RULE).
#include "screens/economics/panels/CftcInterpretationPresentation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QtTest>

using namespace fincept::screens;
using namespace fincept::services;

namespace {

const QDate kLatest(2026, 9, 15);

CftcInterpretationState make_state(const QString& state_id, const QString& participant_key, int horizon = -1) {
    CftcInterpretationState state;
    state.state_id = state_id;
    state.participant_key = participant_key;
    if (horizon > 0) {
        state.has_horizon = true;
        state.horizon_reports = horizon;
    }
    state.threshold_basis = CftcEvidenceBasis::EngineHeuristic;
    state.scope = CftcInterpretationScope::DescriptiveFlow;
    return state;
}

void add_metric(CftcInterpretationState& state, const QString& key, double value) {
    CftcStateMetric metric;
    metric.key = key;
    metric.has_value = true;
    metric.value = value;
    state.metrics.append(metric);
}

CftcInterpretationResult make_result(CftcFamily family) {
    CftcInterpretationResult result;
    result.rule_set_version = cftc_interpretation_rule_set_version();
    result.config_is_default = true;
    result.family = family;
    result.family_code = cftc_family_code(family);
    result.futures_only = false;
    result.report_basis = QStringLiteral("futures_and_options_combined");
    result.report_date = kLatest;
    result.report_date_available = true;
    result.latest_observation_date = kLatest;
    result.open_interest_available = true;
    result.open_interest = 1000000.0;
    for (const auto& candidate : cftc_family_participants(family)) {
        CftcParticipantInterpretation participant;
        participant.participant_key = candidate.key;
        participant.label = candidate.label;
        participant.terminology = cftc_participant_terminology(family, candidate.key);
        participant.terminology_code = cftc_terminology_code(participant.terminology);
        participant.crowding_terminology_allowed = cftc_crowding_terminology_allowed(family, candidate.key);
        if (participant.terminology == CftcTerminologyClass::BroadNonCommercial)
            participant.terminology_caveat_code = QStringLiteral("broad_category_caveat");
        else if (participant.terminology == CftcTerminologyClass::LeveragedFunds)
            participant.terminology_caveat_code = QStringLiteral("not_all_outright_speculation_caveat");
        participant.net_available = true;
        participant.net_position = 100000.0;
        participant.has_net_pct_oi = true;
        participant.net_pct_oi = 12.5;
        participant.historical_percentile_available = true;
        participant.percentile = 0.95;
        participant.percentile_reference_count = 156;
        result.participants.append(participant);
    }
    return result;
}

/// The expected principal participant per family, hard-coded here rather than
/// derived through the helper under test or through the generic speculative
/// flag. The expected key's Batch 4A terminology class is asserted separately.
QString expected_principal_key(CftcFamily family) {
    switch (family) {
        case CftcFamily::Disaggregated:
            return QStringLiteral("managed_money");
        case CftcFamily::Tff:
            return QStringLiteral("leveraged_funds");
        case CftcFamily::Legacy:
            break;
    }
    return QStringLiteral("non_commercial");
}

CftcTerminologyClass expected_principal_terminology(CftcFamily family) {
    switch (family) {
        case CftcFamily::Disaggregated:
            return CftcTerminologyClass::ManagedMoney;
        case CftcFamily::Tff:
            return CftcTerminologyClass::LeveragedFunds;
        case CftcFamily::Legacy:
            break;
    }
    return CftcTerminologyClass::BroadNonCommercial;
}

CftcParticipantInterpretation* primary_participant(CftcInterpretationResult& result) {
    const QString key = expected_principal_key(result.family);
    for (auto& participant : result.participants) {
        if (participant.participant_key == key)
            return &participant;
    }
    return result.participants.isEmpty() ? nullptr : &result.participants.first();
}

const CftcParticipantInterpretation* find_participant(const CftcInterpretationResult& result, const QString& key) {
    for (const auto& participant : result.participants) {
        if (participant.participant_key == key)
            return &participant;
    }
    return nullptr;
}

void add_price_assessment(CftcInterpretationResult& result, const QString& participant_key, int horizon,
                          const QString& state_id, const QStringList& mechanisms, double price_move,
                          double positioning_move) {
    CftcPricePositionAssessment assessment;
    assessment.participant_key = participant_key;
    assessment.horizon_reports = horizon;
    assessment.evaluated = !state_id.isEmpty();
    assessment.has_price_move = assessment.evaluated;
    assessment.price_move = price_move;
    assessment.price_material = assessment.evaluated;
    assessment.has_positioning_move = assessment.evaluated;
    assessment.positioning_move = positioning_move;
    assessment.positioning_material = assessment.evaluated;
    assessment.has_state = !state_id.isEmpty();
    assessment.state_id = state_id;
    assessment.mechanism_state_ids = mechanisms;
    assessment.has_price_move_rank = assessment.evaluated;
    assessment.price_move_rank = 0.9;
    assessment.has_positioning_move_rank = assessment.evaluated;
    assessment.positioning_move_rank = 0.9;
    result.price_context.append(assessment);
}

void add_missing_price_assessment(CftcInterpretationResult& result, const QString& participant_key, int horizon) {
    CftcPricePositionAssessment assessment;
    assessment.participant_key = participant_key;
    assessment.horizon_reports = horizon;
    assessment.evaluated = false;
    assessment.reason = CftcUnavailableReason::MissingPriceContext;
    result.price_context.append(assessment);
}

void add_unavailable(CftcInterpretationResult& result, const QString& state_family, const QString& participant_key,
                     CftcUnavailableReason reason, int horizon = -1, const QString& state_id = QString()) {
    CftcUnavailableRecord record;
    record.state_family = state_family;
    record.participant_key = participant_key;
    record.reason = reason;
    record.state_id = state_id;
    if (horizon > 0) {
        record.has_horizon = true;
        record.horizon_reports = horizon;
    }
    result.unavailable.append(record);
}

/// An evaluated per-horizon flow reading (percent of prior Open Interest), as
/// the engine emits for every configured horizon.
void add_flow_reading(CftcParticipantInterpretation& participant, int horizon, double long_flow, double short_flow,
                      double rank = 0.9) {
    CftcHorizonFlowReading reading;
    reading.horizon_reports = horizon;
    reading.evaluated = true;
    reading.has_long_flow = true;
    reading.long_flow = long_flow;
    reading.has_short_flow = true;
    reading.short_flow = short_flow;
    reading.has_net_flow = true;
    reading.net_flow = long_flow - short_flow;
    reading.has_long_rank = true;
    reading.long_rank = rank;
    reading.has_short_rank = true;
    reading.short_rank = rank;
    reading.has_net_rank = true;
    reading.net_rank = rank;
    reading.net_rank_reference_count = 156;
    participant.flow_readings.append(reading);
}

QString conclusions_text(const CftcInterpretationView& view) {
    QStringList parts;
    parts << view.headline << view.sentences;
    for (const auto& item : view.evidence)
        parts << item.label << item.value;
    return parts.join(QLatin1Char('\n'));
}

QString join_sentences(const CftcInterpretationView& view) {
    return view.sentences.join(QLatin1Char(' '));
}

CftcObservation legacy_observation(const QDate& date, double open_interest, double long_leg, double short_leg) {
    CftcObservation observation;
    observation.date = date;
    observation.date_label = date.toString(Qt::ISODate);
    observation.market = QStringLiteral("TEST - EXCHANGE");
    observation.contract_code = QStringLiteral("000000");
    observation.units = QStringLiteral("Test units");
    observation.open_interest = open_interest;
    observation.longs = {100.0, long_leg, 100.0};
    observation.shorts = {100.0, short_leg, 100.0};
    return observation;
}

QVector<CftcObservation> make_legacy_series(int count, double long_start, double long_end, double short_start,
                                            double short_end, double open_interest = 1000.0) {
    QVector<CftcObservation> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double t = count > 1 ? static_cast<double>(i) / static_cast<double>(count - 1) : 0.0;
        const double long_leg = long_start + (long_end - long_start) * t;
        const double short_leg = short_start + (short_end - short_start) * t;
        out.append(legacy_observation(kLatest.addDays(-7LL * (count - 1 - i)), open_interest, long_leg, short_leg));
    }
    return out;
}

CftcInterpretationInput legacy_input(const QVector<CftcObservation>& observations) {
    CftcInterpretationInput input;
    input.family = CftcFamily::Legacy;
    input.observations_family_code = cftc_family_code(CftcFamily::Legacy);
    input.observations = observations;
    input.report_basis_code = QStringLiteral("futures_and_options_combined");
    input.config = cftc_default_interpretation_config();
    return input;
}

const QStringList kForbiddenPatterns = {
    QStringLiteral("\\bBUY\\b"), QStringLiteral("\\bSELL\\b"), QStringLiteral("\\bHOLD\\b"),
    QStringLiteral("\\bbullish\\b"), QStringLiteral("\\bbearish\\b"), QStringLiteral("\\boverbought\\b"),
    QStringLiteral("\\boversold\\b"), QStringLiteral("\\bsmart money\\b"), QStringLiteral("\\bdumb money\\b"),
    QStringLiteral("\\bexpected return\\b"), QStringLiteral("\\bconfidence\\b"), QStringLiteral("\\bforecast\\b"),
    QStringLiteral("\\bsignal\\b"), QStringLiteral("\\breversal\\b"), QStringLiteral("\\brecommend"),
    QStringLiteral("\\bAI\\b"), QStringLiteral("\\btarget price\\b"),
    // 2026-09-24 audit: continuation, attribution and sign-blind level wording.
    QStringLiteral("\\bdeveloping\\b"), QStringLiteral("\\bcontinuing\\b"), QStringLiteral("driven primarily"),
    QStringLiteral("historically (high|low) net exposure"), QStringLiteral("exposure is historically (high|low)"),
    QStringLiteral("across the individual weekly reports"), QStringLiteral("\\b(UPPER|MIDDLE|LOWER) RANGE\\b")};

QString forbidden_language(const QString& text) {
    for (const QString& pattern : kForbiddenPatterns) {
        const QRegularExpression expression(pattern, QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = expression.match(text);
        if (match.hasMatch())
            return QStringLiteral("%1 … %2").arg(match.captured(0), text.left(300));
    }
    return {};
}

/// The last 200 official CFTC Legacy Futures Only reports for Gold (contract
/// 088691, Socrata dataset 6dca-aqww, 2022-11-22 .. 2026-09-15), copied verbatim
/// from the public CFTC resource: date, open interest, then long/short for
/// Commercial, Non-Commercial and Non-Reportable. 200 reports are enough for
/// every strictly trailing 156-report reference at the latest report (the
/// 13-report move references reach back 169 reports and the unwind lookback
/// 171), so the latest report's results equal the full-history results.
const char* const kGoldLegacyFuturesOnlyRows = R"csv(2022-11-22,449542,133754,263818,214284,98171,40296,26345
2022-11-29,433661,134184,253417,209161,99158,33946,24716
2022-12-06,422100,117578,247657,211472,96347,37785,22831
2022-12-13,437040,119908,258437,224409,98760,37309,24429
2022-12-20,436653,116465,258979,226136,97288,38696,25030
2022-12-27,440040,116744,270520,229525,92645,39424,22528
2023-01-03,449393,113520,273494,230801,89135,42337,24029
2023-01-10,481519,111718,284196,243566,93031,46373,24430
2023-01-17,491818,122814,298727,246874,93634,48128,25455
2023-01-24,499927,127419,307973,253311,95638,47880,24999
2023-01-31,471642,120147,300667,256417,96136,43984,23745
2023-02-07,439755,118802,271768,223298,94483,48076,23925
2023-02-14,426116,120812,249884,210374,104845,45444,21901
2023-02-21,422648,119040,248395,210185,103084,43956,21702
2023-02-28,426890,123391,250878,208419,99826,41958,23064
2023-03-07,458474,125394,244314,215574,117100,43640,23194
2023-03-14,459064,122457,277897,230823,90492,41795,26686
2023-03-21,469874,121678,305246,237891,79286,47624,22661
2023-03-28,478611,131786,333294,245135,63505,47388,27510
2023-04-04,476592,116869,335119,266164,70948,48740,25706
2023-04-11,476567,119710,336258,260165,67420,49102,25299
2023-04-18,482254,123496,339921,260061,70168,50647,24115
2023-04-25,473209,124518,336446,253186,67922,51048,24384
2023-05-02,493804,127451,345035,262413,66846,47502,25485
2023-05-09,518951,130985,353500,266472,70658,52012,25311
2023-05-16,521832,138322,349879,255250,75436,54852,23109
2023-05-23,479080,142984,330007,236149,75417,49327,23036
2023-05-30,449515,133329,323334,237306,67990,43611,22922
2023-06-06,436301,116363,314550,237467,61826,47595,25049
2023-06-13,432898,120461,306523,227317,67108,48953,23100
2023-06-20,438037,121419,307956,229308,66333,47402,23840
2023-06-27,431861,119054,294389,220881,68971,48042,24617
2023-07-03,448063,114905,299622,235081,71984,45490,23870
2023-07-11,483170,114790,302539,240546,74792,46618,24623
2023-07-18,482104,115498,329213,263740,70392,45784,25417
2023-07-25,476176,125491,323701,248229,74590,47300,22729
2023-08-01,439383,117610,304455,238985,74061,43254,21333
2023-08-08,427759,113890,281444,228846,85861,44996,20427
2023-08-15,433611,116062,257992,233078,111942,44337,23543
2023-08-22,430239,117646,238682,226340,124394,45312,26222
2023-08-29,442806,118132,258726,236054,112782,43767,26445
2023-09-05,438672,111456,269695,235802,97796,45966,25733
2023-09-12,441221,113062,257872,235704,111840,46740,25794
2023-09-19,438078,110014,262946,237716,102553,43921,26152
2023-09-26,435620,110844,245680,235560,119745,45213,26192
2023-10-03,431226,110666,222648,228406,137180,45256,24500
2023-10-10,436499,121341,211769,220332,148899,44470,25475
2023-10-17,439581,110062,238061,231412,118674,40863,25602
2023-10-24,463476,106180,272053,251469,102084,42129,25641
2023-10-31,475808,105971,288678,261053,97628,44792,25510
2023-11-07,484347,110651,296742,261653,95437,45484,25609
2023-11-14,486634,110728,287747,254352,98976,45409,23766
2023-11-21,508429,115937,306172,269701,97996,43959,25429
2023-11-28,505658,115596,334188,289845,89761,45650,27142
2023-12-05,487469,103193,330138,288840,85296,47483,24082
2023-12-12,469939,105710,318518,273536,85303,46457,21882
2023-12-19,481203,105559,332617,280972,79633,48019,22300
2023-12-26,491343,105951,338296,289843,82125,48036,23409
2024-01-02,500364,108804,344282,290706,83057,49200,21371
2024-01-09,489849,106489,324190,271665,83051,50574,21487
2024-01-16,489334,109287,316707,262360,82467,49640,22113
2024-01-23,465872,113094,305187,241100,71626,45644,23025
2024-01-30,430328,117525,288043,219222,71431,45364,22637
2024-02-06,419121,104344,284585,228623,66885,41863,23360
2024-02-13,420812,109529,262509,217505,86337,44228,22416
2024-02-20,407063,106419,265830,211034,70772,41846,22697
2024-02-27,411195,108432,266784,214948,73312,38943,22227
2024-03-05,471616,104023,310815,264148,72855,44782,29283
2024-03-12,516057,129432,347705,283062,81460,47992,31321
2024-03-19,535047,138102,355632,278732,77130,49131,33203
2024-03-26,503642,141774,360114,265647,66353,48004,28958
2024-04-02,500045,127114,352828,281399,74149,48931,30467
2024-04-09,505214,128094,352676,279799,77380,51559,29396
2024-04-16,517193,132684,355442,278777,76854,50245,29410
2024-04-23,516249,129083,355955,279535,76644,48914,24933
2024-04-30,519791,126925,354423,278850,74640,50274,26986
2024-05-07,529835,124899,352165,272144,72577,54701,27002
2024-05-14,522952,133583,365693,277642,73146,52633,25019
2024-05-21,530591,118514,377177,300729,70923,55264,26407
2024-05-28,485430,109181,370131,286737,50152,51528,27163
2024-06-04,447488,83418,349349,284566,47264,49807,21178
2024-06-11,436857,82174,342685,276769,42843,50311,23726
2024-06-18,444345,83440,351170,287147,44063,47659,23013
2024-06-25,452190,86551,358039,284885,38656,48436,23177
2024-07-02,454730,86905,351704,280117,38574,47694,24438
2024-07-09,516324,92907,373536,303043,48268,50623,24769
2024-07-16,579862,110041,419345,349827,64803,53814,29534
2024-07-23,572306,102237,403162,341600,68526,53705,25854
2024-07-30,510914,99015,371872,313364,66763,49931,23675
2024-08-06,480645,88442,357476,298119,59370,51205,20920
2024-08-13,505872,88313,380815,328769,61505,50388,25150
2024-08-20,532867,86232,402738,355551,64298,52918,27665
2024-08-27,521759,85102,407421,343330,48885,54689,26815
2024-09-03,511136,80416,392303,339157,51599,51198,26869
2024-09-10,511501,78232,385131,340006,57505,50937,26539
2024-09-17,537627,74647,409774,369734,59668,52976,27915
2024-09-24,564640,76713,416419,387572,72182,53444,29128
2024-10-01,533107,67099,394623,370051,70120,53191,25598
2024-10-08,520316,68957,372933,348891,70711,51440,25644
2024-10-15,541232,70028,382686,357494,71060,50490,24266
)csv"
                                               R"csv(2024-10-22,573760,66245,390946,371813,75609,54214,25717
2024-10-29,579468,68935,372606,366636,87983,53432,28414
2024-11-05,558034,73287,356346,336816,81487,52637,24907
2024-11-12,535981,75347,340929,316225,79774,56838,27707
2024-11-19,502952,73459,335781,309354,74987,52421,24466
2024-11-26,472660,72377,344897,303401,53063,46574,24392
2024-12-03,462040,63251,349526,307611,47875,48337,21798
2024-12-10,483346,66025,362896,324332,48746,46448,25163
2024-12-17,466571,61012,346245,302978,40937,48388,25196
2024-12-24,454646,58136,331472,289416,41787,47813,22106
2024-12-31,458584,60399,331695,282907,35628,47357,23340
2025-01-07,477043,62674,341320,291721,36810,47574,23839
2025-01-14,526467,92075,394527,312568,33205,48250,25161
2025-01-21,571387,95116,421724,337289,36505,50638,24814
2025-01-28,577505,110034,433756,338371,38962,49224,24911
2025-02-04,542004,74378,402553,356500,53992,49522,23855
2025-02-11,528719,72622,382510,343766,59262,49509,24125
2025-02-18,522330,72393,368464,334043,65369,50882,23485
2025-02-25,512179,71796,359896,316948,55323,48803,22328
2025-03-04,489270,65713,336828,303132,59871,49602,21748
2025-03-11,511276,84545,349733,296172,60072,53747,24659
2025-03-18,533566,91549,376717,321654,63722,52841,25605
2025-03-25,511482,86838,365436,316572,66776,52852,24050
2025-04-01,498746,68875,333936,327936,89502,50688,24061
2025-04-08,445468,69214,301397,269833,69118,50486,19018
2025-04-15,456628,70568,302480,271707,69497,51349,21647
2025-04-22,465351,91128,293396,258896,83518,50904,24014
2025-04-29,451868,92787,283548,240377,77059,50675,23232
2025-05-06,452414,87244,279347,237445,74948,52632,23026
2025-05-13,440842,88412,279619,238191,76982,51570,21572
2025-05-20,448000,90256,287904,238062,74081,54053,20386
2025-05-27,437538,87509,295096,234087,59903,56541,23138
2025-06-03,415941,70905,291731,246982,59077,51996,19075
2025-06-10,417143,68432,291150,245995,58514,54008,18771
2025-06-17,441214,71684,309411,260586,59938,55840,18761
2025-06-24,434958,73323,303883,256077,61073,55009,19453
2025-07-01,437662,68820,304889,258631,56651,53324,19235
2025-07-08,443144,71875,310229,261685,58717,56291,20905
2025-07-15,448531,75989,326677,270227,57112,56976,19403
2025-07-22,489423,76726,359063,311949,58911,54052,24753
2025-07-29,445259,79093,338172,281241,57645,53216,17733
2025-08-05,449647,74075,344221,292194,55144,52597,19501
2025-08-12,446152,63427,326492,288115,58630,53656,20076
2025-08-19,438541,66670,316635,275277,62687,56098,18723
2025-08-26,443760,72908,323873,275767,61456,56712,20058
2025-09-02,492908,73919,347817,315796,66266,56635,32267
2025-09-09,509625,73850,347225,324875,63135,58990,47355
2025-09-16,516221,77867,380238,326778,60368,60872,24911
2025-09-23,528789,82971,381374,332808,66059,57683,26029
2025-09-30,493748,68872,357108,318804,65896,57524,22196
2025-10-07,485559,77599,341371,300798,68842,56574,24758
2025-10-14,485788,92027,328894,278405,74489,59259,26308
2025-10-21,472421,107847,317964,253851,77242,55921,22413
2025-10-28,457122,89653,326680,266308,61644,52742,20379
2025-11-04,450399,88355,324897,256572,54265,53661,19426
2025-11-10,459997,87076,331899,265916,58847,56742,18988
2025-11-18,471953,97001,342586,269556,59217,54239,18993
2025-11-25,432946,82657,324060,253266,48678,53773,16958
2025-12-02,418490,60147,315495,261331,43771,55812,18024
2025-12-09,432569,63707,326286,268485,44599,55485,16792
2025-12-16,471093,74655,349013,280920,46942,58795,18415
2025-12-23,492103,75835,359361,290161,49461,62268,19442
2025-12-30,481866,82254,357500,275592,44419,60217,16144
2026-01-06,488116,80218,352253,274435,46803,60118,15715
2026-01-13,527455,83382,380488,296183,44945,66220,20352
2026-01-20,528004,85869,375558,295772,51002,62136,17217
2026-01-27,488463,96200,344485,252100,46704,62677,19788
2026-02-03,409694,87964,295742,214508,48904,56610,14436
2026-02-10,404391,88738,286476,212808,52796,52916,15190
2026-02-17,407078,88237,285019,213432,53517,51821,14954
2026-02-24,420182,86198,287004,211649,52472,67772,26143
2026-03-03,409789,84834,285417,213752,53607,55126,14688
2026-03-10,413956,85280,288256,215445,52313,53424,13580
2026-03-17,411388,86184,284832,215961,56092,52913,14134
2026-03-24,403925,76997,280825,220861,52534,49273,13772
2026-03-31,361409,58697,260337,207602,44400,50617,12179
2026-04-07,354877,57729,251480,205368,49063,49117,11671
2026-04-14,362274,55757,256839,210009,47483,51568,13012
2026-04-21,365842,56261,259201,212893,48887,52223,13289
2026-04-28,369530,60553,255366,211818,52247,49280,14038
2026-05-05,367932,58230,257165,211814,48511,49252,13620
2026-05-12,376496,57725,267983,219793,48171,49951,11315
2026-05-19,379325,69520,261149,211018,51185,47082,15286
2026-05-26,353489,74641,260407,200704,46444,47149,15643
2026-06-02,326052,53851,260196,206096,30076,43656,13331
2026-06-09,332709,58986,260022,207984,34147,42891,15692
2026-06-16,339330,58220,265783,211127,30907,43966,16623
2026-06-23,352167,64579,269983,217028,35689,39265,15200
2026-06-30,369541,59118,280188,229619,35600,49035,21984
2026-07-07,371776,59564,281846,233713,39467,45636,17600
2026-07-14,383689,79639,294427,227310,40628,44419,16313
2026-07-21,383368,80457,293656,224785,40875,46143,16854
2026-07-28,384603,75460,287769,219622,37552,61384,31145
2026-08-04,371551,71832,298323,227013,29379,44531,15674
2026-08-11,400309,69385,322025,250936,32996,51051,16351
2026-08-18,406260,69050,327468,256902,34713,51347,15118
2026-08-25,427957,62453,342038,277159,33825,54725,18474
2026-09-01,415196,61450,326168,260485,32361,52030,15436
2026-09-08,411227,54403,324677,261007,29047,52554,14240
2026-09-15,409899,56417,318138,258059,27721,47460,16077
)csv";

/// The rows in the provider's cot_history shape, exactly as the panel parses them.
QJsonArray gold_legacy_futures_only_rows() {
    QJsonArray rows;
    const QStringList lines =
        QString::fromLatin1(kGoldLegacyFuturesOnlyRows).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList cells = line.split(QLatin1Char(','));
        QJsonObject row;
        row[QStringLiteral("report_date_as_yyyy_mm_dd")] = cells[0];
        row[QStringLiteral("market_and_exchange_names")] = QStringLiteral("GOLD - COMMODITY EXCHANGE INC.");
        row[QStringLiteral("cftc_contract_market_code")] = QStringLiteral("088691");
        row[QStringLiteral("contract_units")] = QStringLiteral("(CONTRACTS OF 100 TROY OUNCES)");
        row[QStringLiteral("futonly_or_combined")] = QStringLiteral("FutOnly");
        row[QStringLiteral("open_interest_all")] = cells[1].toDouble();
        row[QStringLiteral("commercial_long")] = cells[2].toDouble();
        row[QStringLiteral("commercial_short")] = cells[3].toDouble();
        row[QStringLiteral("non_commercial_long")] = cells[4].toDouble();
        row[QStringLiteral("non_commercial_short")] = cells[5].toDouble();
        row[QStringLiteral("non_reportable_long")] = cells[6].toDouble();
        row[QStringLiteral("non_reportable_short")] = cells[7].toDouble();
        rows.append(row);
    }
    return rows;
}

QSet<QString> state_keys(const CftcParticipantInterpretation& participant) {
    QSet<QString> out;
    for (const auto& state : participant.states)
        out.insert(state.has_horizon ? QStringLiteral("%1@%2").arg(state.state_id).arg(state.horizon_reports)
                                     : state.state_id);
    return out;
}

} // namespace

class TstCftcPresentation : public QObject {
    Q_OBJECT
  private slots:
    void state_ids_map_to_predefined_wording();
    void headline_precedence_places_level_before_repositioning();
    void crowded_long_with_accumulation();
    void crowded_long_with_unwind();
    void long_accumulation_with_short_covering_composition();
    void long_liquidation_with_short_building_composition();
    void four_and_thirteen_report_conflict_is_exposed_as_mixed();
    void four_and_thirteen_relationship_conflict_keeps_divergence_semantics();
    void legacy_terminology_is_neutral_and_crowding_gated();
    void disaggregated_terminology();
    void tff_terminology();
    void tff_is_never_reconstructed_into_commercial_speculator();
    void both_divergence_directions_are_distinct();
    void moving_together_is_not_predictive_confirmation();
    void divergence_wording_contains_no_forecast();
    void missing_price_preserves_cftc_only_conclusions();
    void concrete_price_failure_reason_is_reported();
    void participant_specific_failure_reason_is_reported();
    void report_wide_failure_reason_is_reported();
    void participant_specific_reason_wins_over_report_wide_record();
    void net_share_disagreement_wording_is_exact();
    void principal_mapping_follows_terminology_contract();
    void missing_principal_participant_is_unavailable_not_fallback();
    void evidence_flow_metrics_use_exact_units();
    void evidence_distinguishes_no_material_state_from_unavailable();
    void open_interest_evidence_uses_the_state_horizon();
    void terminology_caveats_render_only_where_required();
    void severe_extreme_is_not_downgraded_to_crowding();
    void insufficient_history_does_not_suppress_direct_states();
    void visible_range_does_not_change_interpretation();
    void deterministic_repeatability();
    void vocabulary_audit();
    void horizon_selection_changes_the_horizon_specific_interpretation();
    void horizon_selection_keeps_the_156_report_state_and_percentile();
    void horizon_view_distinguishes_evaluated_not_material_from_unavailable();
    void horizon_view_carries_concrete_unavailable_reasons();
    void horizon_view_price_pending_versus_failed();
    void horizon_view_open_interest_context();
    void horizon_view_evidence_is_tagged_with_the_selected_horizon();
    void unsupported_horizon_falls_back_to_combined_view();
    void neutral_legacy_metric_label_has_no_speculator_wording();
    void horizon_views_keep_the_forbidden_language_audit();
    void leg_evidence_uses_the_leg_own_record();
    void non_material_open_interest_evidence_is_explicit();
    void price_evidence_keeps_quoted_units_precision();
    void horizon_evidence_uses_emitted_readings();
    void raw_reading_without_materiality_reference_is_not_below_threshold();
    void net_delta_reason_never_comes_from_price();

    // 2026-09-24 audit corrections
    void historically_high_net_short_is_not_high_exposure();
    void unavailable_level_and_persistence_are_stated();
    void remains_requires_established_persistence();
    void divergence_names_the_leg_that_supplied_the_move();
    void disagreement_sentence_states_both_values();
    void extreme_transitions_do_not_claim_continuation();
    void sustained_wording_states_the_actual_count();
    void net_pct_unavailable_reason_names_the_cause();
    void one_report_sentence_matches_percentile_availability();
    void missing_leg_composes_a_partial_view();
    void market_concentration_is_surfaced_market_level();
    void combined_view_below_threshold_legs_are_not_available();
    void heuristic_thresholds_are_labelled_inline();
    void price_conclusions_name_the_series_and_sessions();
    void two_material_legs_without_net_move_headline();
    void outdated_report_is_flagged_in_headline_and_body();
    void golden_real_gold_legacy_report();
};

void TstCftcPresentation::state_ids_map_to_predefined_wording() {
    const QStringList state_ids = cftc_interpretation_state_ids();
    QVERIFY(state_ids.size() >= 30);
    for (const QString& state_id : state_ids) {
        const QString short_wording = cftc_state_short_wording(state_id);
        const QString relationship = cftc_relationship_wording(state_id);
        QVERIFY2(!short_wording.isEmpty() || !relationship.isEmpty(),
                 qPrintable(QStringLiteral("state id without predefined wording: %1").arg(state_id)));
    }
    QVERIFY(cftc_state_short_wording(QStringLiteral("NOT_A_STATE")).isEmpty());
    QVERIFY(cftc_relationship_wording(QStringLiteral("NOT_A_STATE")).isEmpty());
}

void TstCftcPresentation::headline_precedence_places_level_before_repositioning() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const int level = view.headline.indexOf(QStringLiteral("historically crowded long"));
    const int trajectory = view.headline.indexOf(QStringLiteral("long accumulation"));
    QVERIFY(level >= 0);
    QVERIFY(trajectory > level);
    // No persistence state was emitted, so the level is "is", not "remains";
    // and the headline never claims the move is still continuing.
    QVERIFY(view.sentences.first().contains(QStringLiteral("is unusually net long")));
    QVERIFY(!view.headline.contains(QStringLiteral("continuing")));
}

void TstCftcPresentation::crowded_long_with_accumulation() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    add_metric(net_state, QStringLiteral("net_flow"), 4.2);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")};
    primary->states << net_state;
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("SHORT_COVERING"), primary->participant_key, 4);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY2(view.headline.startsWith(QStringLiteral("Managed Money — historically crowded long; long accumulation")),
             qPrintable(view.headline));
    QVERIFY(!view.headline.contains(QStringLiteral("continuing")));
    QVERIFY(join_sentences(view).contains(QStringLiteral("is unusually net long")));
    QVERIFY(
        join_sentences(view).contains(QStringLiteral("Net positioning shifted longward over the last four reports")));
    // The combined view names the horizon the gross mechanism comes from.
    QVERIFY(join_sentences(view).contains(
        QStringLiteral("Over the last four reports, long exposure increased while short positions were reduced, "
                       "producing a material longward shift.")));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));
}

void TstCftcPresentation::crowded_long_with_unwind() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("UNWINDING_HIGH_EXTREME"), primary->participant_key);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_LIQUIDATION")};
    primary->states << net_state;
    primary->states << make_state(QStringLiteral("LONG_LIQUIDATION"), primary->participant_key, 4);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.contains(QStringLiteral("historically crowded long")));
    // A transition, not a claim that the unwind is still "developing".
    QVERIFY(view.headline.contains(QStringLiteral("moved well back from a recent extreme")));
    QVERIFY(!view.headline.contains(QStringLiteral("developing")));
    QVERIFY(join_sentences(view).contains(
        QStringLiteral("fallen back to or below the 75th percentile within 13 reports of a persistent high extreme")));
    QVERIFY(
        join_sentences(view).contains(QStringLiteral("Net positioning shifted shortward over the last four reports")));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));
}

void TstCftcPresentation::long_accumulation_with_short_covering_composition() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")};
    primary->states << net_state;

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(join_sentences(view).contains(
        QStringLiteral("Over the last four reports, long exposure increased while short positions were reduced, "
                       "producing a material longward shift.")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("positioning increased")));
}

void TstCftcPresentation::long_liquidation_with_short_building_composition() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 13);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_LIQUIDATION"), QStringLiteral("SHORT_BUILDING")};
    primary->states << net_state;

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(join_sentences(view).contains(
        QStringLiteral("Over the last thirteen reports, long exposure decreased while short positions increased, "
                       "producing a material shortward shift.")));
}

void TstCftcPresentation::four_and_thirteen_report_conflict_is_exposed_as_mixed() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 13);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(join_sentences(view).contains(QStringLiteral("Positioning is mixed across horizons: longward over four "
                                                         "reports but still shortward over thirteen reports.")));
    QVERIFY(view.headline.contains(QStringLiteral("mixed repositioning across horizons")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("shifted longward over the last four and thirteen")));
}

void TstCftcPresentation::four_and_thirteen_relationship_conflict_keeps_divergence_semantics() {
    // 4R moving together, 13R divergence: the conflict is exposed and the
    // divergent horizon keeps its gross-leg mechanism and its
    // contemporaneous-only statement. Before the fix the conflict early return
    // dropped both.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {},
                         100.0, 2000.0);
    add_price_assessment(result, primary->participant_key, 13, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                         {QStringLiteral("LONG_LIQUIDATION")}, 300.0, -4000.0);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = join_sentences(view);
    QVERIFY(text.contains(QStringLiteral("relationship differs across horizons")));
    QVERIFY(text.contains(QStringLiteral("Price rose materially over thirteen reports")));
    QVERIFY(text.contains(QStringLiteral("Non-Commercial shifted materially shortward")));
    QVERIFY(text.contains(QStringLiteral("The positioning change included long liquidation.")));
    QVERIFY(!text.contains(QStringLiteral("primarily")));
    QVERIFY(text.contains(QStringLiteral("contemporaneous divergence")));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));

    // Divergence at 4R, moving together at 13R: same guarantees, other order.
    CftcInterpretationResult reverse = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* reverse_primary = primary_participant(reverse);
    reverse_primary->states << make_state(QStringLiteral("NET_SHORT"), reverse_primary->participant_key);
    add_price_assessment(reverse, reverse_primary->participant_key, 4,
                         QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"), {QStringLiteral("SHORT_COVERING")},
                         -100.0, 2000.0);
    add_price_assessment(reverse, reverse_primary->participant_key, 13,
                         QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"), {}, -300.0, -4000.0);
    const QString reverse_text = join_sentences(cftc_compose_interpretation(reverse));
    QVERIFY(reverse_text.contains(QStringLiteral("relationship differs across horizons")));
    QVERIFY(reverse_text.contains(QStringLiteral("Price fell materially over four reports")));
    QVERIFY(reverse_text.contains(QStringLiteral("The positioning change included short covering.")));
    QVERIFY(reverse_text.contains(QStringLiteral("contemporaneous divergence")));

    // A conflict between two moving-together states carries no divergence
    // claim: the contemporaneous-only statement must not be invented.
    CftcInterpretationResult moving = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* moving_primary = primary_participant(moving);
    moving_primary->states << make_state(QStringLiteral("NET_LONG"), moving_primary->participant_key);
    add_price_assessment(moving, moving_primary->participant_key, 4,
                         QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {}, 100.0, 2000.0);
    add_price_assessment(moving, moving_primary->participant_key, 13,
                         QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"), {}, -300.0, -4000.0);
    const QString moving_text = join_sentences(cftc_compose_interpretation(moving));
    QVERIFY(moving_text.contains(QStringLiteral("relationship differs across horizons")));
    QVERIFY(!moving_text.contains(QStringLiteral("contemporaneous divergence")));
    QVERIFY(!moving_text.contains(QStringLiteral("driven primarily by")));
    QVERIFY(!moving_text.contains(QStringLiteral("positioning change included")));
}

void TstCftcPresentation::legacy_terminology_is_neutral_and_crowding_gated() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.startsWith(QStringLiteral("Non-Commercial — historically crowded long")));
    QVERIFY(!view.headline.contains(QStringLiteral("Speculator")));

    // Legacy Commercial is a neutral exposure class: no crowded wording.
    const CftcParticipantInterpretation* commercial = find_participant(result, QStringLiteral("commercial"));
    QVERIFY(commercial != nullptr);
    CftcParticipantInterpretation neutral = *commercial;
    neutral.states.clear();
    neutral.states << make_state(QStringLiteral("NET_LONG"), neutral.participant_key);
    neutral.states << make_state(QStringLiteral("HISTORICALLY_HIGH_NET"), neutral.participant_key);
    neutral.states << make_state(QStringLiteral("CROWDED_LONG"), neutral.participant_key);
    const QString phrase = cftc_exposure_phrase(neutral);
    QVERIFY2(phrase.contains(QStringLiteral("historically large net long")), qPrintable(phrase));
    QVERIFY(!phrase.contains(QStringLiteral("crowded")));

    const CftcParticipantInterpretation* non_reportable = find_participant(result, QStringLiteral("non_reportable"));
    QVERIFY(non_reportable != nullptr);
    CftcParticipantInterpretation neutral_nr = *non_reportable;
    neutral_nr.states.clear();
    neutral_nr.states << make_state(QStringLiteral("NET_SHORT"), neutral_nr.participant_key);
    QCOMPARE(cftc_exposure_phrase(neutral_nr), QStringLiteral("net short"));
}

void TstCftcPresentation::disaggregated_terminology() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    QCOMPARE(primary->participant_key, QStringLiteral("managed_money"));
    QVERIFY(primary->crowding_terminology_allowed);
    primary->states << make_state(QStringLiteral("CROWDED_SHORT"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.startsWith(QStringLiteral("Managed Money — historically crowded short")));

    const CftcParticipantInterpretation* producer = find_participant(result, QStringLiteral("producer_merchant"));
    QVERIFY(producer != nullptr);
    QVERIFY(!producer->crowding_terminology_allowed);
    CftcParticipantInterpretation neutral = *producer;
    neutral.states.clear();
    neutral.states << make_state(QStringLiteral("NET_SHORT"), neutral.participant_key);
    neutral.states << make_state(QStringLiteral("HISTORICALLY_LOW_NET"), neutral.participant_key);
    QVERIFY(cftc_exposure_phrase(neutral).contains(QStringLiteral("historically large net short")));
    QCOMPARE(cftc_participant_display_name(neutral), QStringLiteral("Producer/Merchant/Processor/User"));

    const CftcParticipantInterpretation* swap = find_participant(result, QStringLiteral("swap_dealer"));
    QVERIFY(swap != nullptr);
    CftcParticipantInterpretation neutral_swap = *swap;
    neutral_swap.states.clear();
    neutral_swap.states << make_state(QStringLiteral("NET_LONG"), neutral_swap.participant_key);
    QCOMPARE(cftc_exposure_phrase(neutral_swap), QStringLiteral("net long"));
}

void TstCftcPresentation::tff_terminology() {
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    QCOMPARE(primary->participant_key, QStringLiteral("leveraged_funds"));
    QVERIFY(primary->crowding_terminology_allowed);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.startsWith(QStringLiteral("Leveraged Funds — historically crowded long")));

    const CftcParticipantInterpretation* asset_manager = find_participant(result, QStringLiteral("asset_manager"));
    QVERIFY(asset_manager != nullptr);
    QVERIFY(!asset_manager->crowding_terminology_allowed);
    QCOMPARE(cftc_participant_display_name(*asset_manager), QStringLiteral("Asset Manager/Institutional"));
    CftcParticipantInterpretation neutral = *asset_manager;
    neutral.states.clear();
    neutral.states << make_state(QStringLiteral("NET_LONG"), neutral.participant_key);
    neutral.states << make_state(QStringLiteral("HISTORICALLY_HIGH_NET"), neutral.participant_key);
    QVERIFY(cftc_exposure_phrase(neutral).contains(QStringLiteral("historically large net long")));

    const CftcParticipantInterpretation* dealer = find_participant(result, QStringLiteral("dealer"));
    QVERIFY(dealer != nullptr);
    QCOMPARE(cftc_participant_display_name(*dealer), QStringLiteral("Dealer/Intermediary"));
}

void TstCftcPresentation::tff_is_never_reconstructed_into_commercial_speculator() {
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                         {QStringLiteral("LONG_LIQUIDATION")}, 100.0, -2000.0);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = conclusions_text(view) + QLatin1Char('\n') + view.context_text;
    QVERIFY(!text.contains(QStringLiteral("commercial"), Qt::CaseInsensitive));
    // The mandated Leveraged Funds caveat uses the word "speculation"; what is
    // forbidden is reconstructing a speculator category or a Commercial split.
    // The caveat must be the only sentence carrying that stem.
    QVERIFY(!text.contains(QStringLiteral("speculator"), Qt::CaseInsensitive));
    int speculation_sentences = 0;
    for (const QString& sentence : view.sentences) {
        if (sentence.contains(QStringLiteral("speculat"), Qt::CaseInsensitive))
            ++speculation_sentences;
    }
    QCOMPARE(speculation_sentences, 1);
    QVERIFY(join_sentences(view).contains(QStringLiteral("outright speculation")));
    QVERIFY(text.contains(QStringLiteral("Leveraged Funds")));
}

void TstCftcPresentation::both_divergence_directions_are_distinct() {
    CftcInterpretationResult up = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* up_primary = primary_participant(up);
    up_primary->states << make_state(QStringLiteral("NET_LONG"), up_primary->participant_key);
    add_price_assessment(up, up_primary->participant_key, 4, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                         {QStringLiteral("LONG_LIQUIDATION")}, 100.0, -2000.0);
    const CftcInterpretationView up_view = cftc_compose_interpretation(up);
    const QString up_text = join_sentences(up_view);
    QVERIFY(up_text.contains(QStringLiteral("Price rose materially over four reports")));
    QVERIFY(up_text.contains(QStringLiteral("Managed Money shifted materially shortward")));
    QVERIFY(up_text.contains(QStringLiteral("The positioning change included long liquidation.")));
    QVERIFY(up_text.contains(QStringLiteral("contemporaneous divergence")));

    CftcInterpretationResult down = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* down_primary = primary_participant(down);
    down_primary->states << make_state(QStringLiteral("NET_SHORT"), down_primary->participant_key);
    add_price_assessment(down, down_primary->participant_key, 4, QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"),
                         {QStringLiteral("SHORT_COVERING")}, -100.0, 2000.0);
    const CftcInterpretationView down_view = cftc_compose_interpretation(down);
    const QString down_text = join_sentences(down_view);
    QVERIFY(down_text.contains(QStringLiteral("Price fell materially over four reports")));
    QVERIFY(down_text.contains(QStringLiteral("Managed Money shifted materially longward")));
    QVERIFY(down_text.contains(QStringLiteral("The positioning change included short covering.")));
    QVERIFY(down_text.contains(QStringLiteral("contemporaneous divergence")));
    QVERIFY(up_text != down_text);

    const CftcEvidenceItem* relationship = nullptr;
    for (const auto& item : up_view.evidence) {
        if (item.label.contains(QStringLiteral("relationship")))
            relationship = &item;
    }
    QVERIFY(relationship != nullptr);
    QCOMPARE(relationship->status, CftcEvidenceStatus::Available);
    QVERIFY(relationship->value.contains(QStringLiteral("Divergence")));
}

void TstCftcPresentation::moving_together_is_not_predictive_confirmation() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {},
                         100.0, 2000.0);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = join_sentences(view);
    QVERIFY(text.contains(QStringLiteral("moved together upward")));
    QVERIFY(!text.contains(QStringLiteral("confirm"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("signal"), Qt::CaseInsensitive));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));

    CftcInterpretationResult down = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* down_primary = primary_participant(down);
    down_primary->states << make_state(QStringLiteral("NET_SHORT"), down_primary->participant_key);
    add_price_assessment(down, down_primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"),
                         {}, -100.0, -2000.0);
    const QString down_text = join_sentences(cftc_compose_interpretation(down));
    QVERIFY(down_text.contains(QStringLiteral("moved together downward")));
}

void TstCftcPresentation::divergence_wording_contains_no_forecast() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                         {QStringLiteral("LONG_LIQUIDATION")}, 100.0, -2000.0);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = join_sentences(view);
    QVERIFY(text.contains(QStringLiteral("contemporaneous divergence")));
    QVERIFY(text.contains(QStringLiteral("The positioning change included long liquidation.")));
    QVERIFY(!text.contains(QStringLiteral("reversal"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("should"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("catch up"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("expect"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("predict"), Qt::CaseInsensitive));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));
}

void TstCftcPresentation::missing_price_preserves_cftc_only_conclusions() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    for (int horizon : {1, 4, 13})
        add_missing_price_assessment(result, primary->participant_key, horizon);

    const CftcInterpretationView unavailable = cftc_compose_interpretation(result);
    QVERIFY(unavailable.headline.contains(QStringLiteral("net long")));
    QVERIFY(join_sentences(unavailable).contains(QStringLiteral("is net long in the latest report")));
    QVERIFY(join_sentences(unavailable).contains(QStringLiteral("Net positioning shifted longward")));
    QVERIFY(join_sentences(unavailable)
                .contains(QStringLiteral("Price relationship unavailable: no price observations were supplied.")));

    const CftcInterpretationView pending = cftc_compose_interpretation(result, CftcPriceContextState::Pending);
    QVERIFY(pending.headline.contains(QStringLiteral("net long")));
    QVERIFY(join_sentences(pending).contains(QStringLiteral("Price context is still loading")));
    QVERIFY(!join_sentences(pending).contains(QStringLiteral("Price relationship unavailable")));
    for (const auto& item : pending.evidence) {
        if (item.label.startsWith(QStringLiteral("Price")))
            QVERIFY(item.status == CftcEvidenceStatus::Pending);
    }
}

void TstCftcPresentation::concrete_price_failure_reason_is_reported() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    for (int horizon : {1, 4, 13})
        add_missing_price_assessment(result, primary->participant_key, horizon);

    const CftcInterpretationView view = cftc_compose_interpretation(
        result, CftcPriceContextState::Unavailable, QStringLiteral("the Yahoo Finance history request failed"));
    QVERIFY(join_sentences(view).contains(
        QStringLiteral("Price relationship unavailable: the Yahoo Finance history request failed.")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("no price observations were supplied")));
    QVERIFY(view.headline.contains(QStringLiteral("net long")));
    // The evidence rows carry the same concrete reason as the prose.
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Price"))) {
            QVERIFY(item.status == CftcEvidenceStatus::Unavailable);
            QVERIFY(item.value.contains(QStringLiteral("the Yahoo Finance history request failed")));
            QVERIFY(!item.value.contains(QStringLiteral("no price observations were supplied")));
        }
    }
}

void TstCftcPresentation::participant_specific_failure_reason_is_reported() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    const QString key = expected_principal_key(CftcFamily::Legacy);
    add_unavailable(result, QStringLiteral("NET_EXPOSURE"), key, CftcUnavailableReason::MissingParticipantLeg);
    add_unavailable(result, QStringLiteral("HISTORICAL_RELATIVE_STATE"), key,
                    CftcUnavailableReason::MissingParticipantLeg);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY2(view.interpreted, "a missing principal leg is a partial view, not a whole-report failure");
    QVERIFY(view.headline.contains(QStringLiteral("positioning unavailable")));
    QVERIFY(join_sentences(view).contains(QStringLiteral(
        "Current Non-Commercial net exposure is unavailable: the reported long or short leg is missing.")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("no official CFTC observations")));
}

void TstCftcPresentation::report_wide_failure_reason_is_reported() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    add_unavailable(result, QStringLiteral("NET_EXPOSURE"), QString(), CftcUnavailableReason::NoObservations);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(!view.interpreted);
    QVERIFY(join_sentences(view).contains(QStringLiteral("no official CFTC observations are available")));
}

void TstCftcPresentation::participant_specific_reason_wins_over_report_wide_record() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    const QString key = expected_principal_key(CftcFamily::Legacy);
    // A report-wide record must not shadow the primary participant's own
    // blocked reading, even when it appears first in the unavailable list.
    add_unavailable(result, QStringLiteral("NET_EXPOSURE"), QString(), CftcUnavailableReason::NoObservations);
    add_unavailable(result, QStringLiteral("NET_EXPOSURE"), key, CftcUnavailableReason::MissingParticipantLeg);
    // Another participant carries states (the mixed-report case): the primary
    // participant's own reading is still what the headline reports on.
    for (auto& participant : result.participants) {
        if (participant.participant_key == QStringLiteral("commercial"))
            participant.states << make_state(QStringLiteral("NET_LONG"), participant.participant_key);
    }

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.interpreted);
    QVERIFY(join_sentences(view).contains(QStringLiteral("the reported long or short leg is missing")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("no official CFTC observations")));
}

void TstCftcPresentation::net_share_disagreement_wording_is_exact() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), primary->participant_key, 4);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = join_sentences(view);
    QVERIFY(text.contains(QStringLiteral("the change in Net %OI moved opposite to the raw net flow")));
    QVERIFY(!text.contains(QStringLiteral("%%")));
}

void TstCftcPresentation::principal_mapping_follows_terminology_contract() {
    const CftcFamily families[] = {CftcFamily::Legacy, CftcFamily::Disaggregated, CftcFamily::Tff};
    for (CftcFamily family : families) {
        const QString key = cftc_principal_participant_key(family);
        QCOMPARE(key, expected_principal_key(family));
        QVERIFY(!key.isEmpty());
        // The mapping is proven through the finalized Batch 4A terminology
        // metadata, not through the generic speculative flag.
        QCOMPARE(cftc_participant_terminology(family, key), expected_principal_terminology(family));
    }
    // A key from another family is never reassigned by the terminology table.
    QCOMPARE(cftc_participant_terminology(CftcFamily::Tff, QStringLiteral("non_commercial")),
             CftcTerminologyClass::Unknown);
}

void TstCftcPresentation::missing_principal_participant_is_unavailable_not_fallback() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    for (int i = result.participants.size() - 1; i >= 0; --i) {
        if (result.participants[i].participant_key == QStringLiteral("managed_money"))
            result.participants.removeAt(i);
    }
    // Another participant still carries a state; the composer must not
    // substitute it for the missing principal participant.
    for (auto& participant : result.participants) {
        if (participant.participant_key == QStringLiteral("producer_merchant"))
            participant.states << make_state(QStringLiteral("NET_LONG"), participant.participant_key);
    }

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(!view.interpreted);
    QVERIFY(view.headline.contains(QStringLiteral("interpretation unavailable")));
    QVERIFY(join_sentences(view).contains(QStringLiteral("principal participant could not be resolved")));
    QVERIFY(!conclusions_text(view).contains(QStringLiteral("Producer/Merchant")));
}

void TstCftcPresentation::evidence_flow_metrics_use_exact_units() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);

    CftcInterpretationState long_state = make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    add_metric(long_state, QStringLiteral("move_value"), 12.5);
    primary->states << long_state;

    CftcInterpretationState short_state = make_state(QStringLiteral("SHORT_COVERING"), primary->participant_key, 13);
    add_metric(short_state, QStringLiteral("move_value"), -8.25);
    primary->states << short_state;

    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    add_metric(net_state, QStringLiteral("net_flow"), 3.5);
    net_state.has_move_rank = true;
    net_state.move_rank = 0.75;
    net_state.move_rank_reference_count = 156;
    primary->states << net_state;

    CftcInterpretationState oi_state = make_state(QStringLiteral("OI_EXPANSION"), QString());
    oi_state.has_horizon = true;
    oi_state.horizon_reports = 4;
    add_metric(oi_state, QStringLiteral("oi_change"), 7.5);
    result.market_context << oi_state;

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    auto row = [&view](const QString& label, int horizon = -1) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label != label)
                continue;
            if (horizon >= 0 && item.horizon_reports != horizon)
                continue;
            return &item;
        }
        return nullptr;
    };

    const CftcEvidenceItem* long_row = row(QStringLiteral("Long leg flow (% of prior OI)"), 4);
    QVERIFY(long_row != nullptr);
    QCOMPARE(long_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(long_row->value, QStringLiteral("12.50%"));
    QCOMPARE(long_row->horizon_reports, 4);

    const CftcEvidenceItem* short_row = row(QStringLiteral("Short leg flow (% of prior OI)"), 13);
    QVERIFY(short_row != nullptr);
    QCOMPARE(short_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(short_row->value, QStringLiteral("-8.25%"));
    QCOMPARE(short_row->horizon_reports, 13);

    const CftcEvidenceItem* net_row = row(QStringLiteral("Net flow (% of prior OI)"), 4);
    QVERIFY(net_row != nullptr);
    QCOMPARE(net_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(net_row->value, QStringLiteral("+3.50%"));

    const CftcEvidenceItem* rank_row = row(QStringLiteral("Move materiality rank (% of prior moves)"), 4);
    QVERIFY(rank_row != nullptr);
    QCOMPARE(rank_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(rank_row->value, QStringLiteral("75.0% (n=156)"));

    const CftcEvidenceItem* oi_row = row(QStringLiteral("Open Interest change (% change) (four reports)"));
    QVERIFY(oi_row != nullptr);
    QCOMPARE(oi_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(oi_row->value, QStringLiteral("+7.50%"));

    const CftcEvidenceItem* net_pct_row = row(QStringLiteral("Net %OI (% of current OI)"));
    QVERIFY(net_pct_row != nullptr);
    QCOMPARE(net_pct_row->value, QStringLiteral("12.50%"));

    // No evidence row relabels a normalized Batch 4A metric as contract counts.
    for (const auto& item : view.evidence) {
        QVERIFY(!item.label.contains(QStringLiteral("contracts"), Qt::CaseInsensitive));
        QVERIFY(!item.value.contains(QStringLiteral("contracts"), Qt::CaseInsensitive));
    }
}

void TstCftcPresentation::evidence_distinguishes_no_material_state_from_unavailable() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    // A 4-report price assessment that was evaluated but produced no state.
    CftcPricePositionAssessment assessed;
    assessed.participant_key = primary->participant_key;
    assessed.horizon_reports = 4;
    assessed.evaluated = true;
    assessed.has_price_move = true;
    assessed.price_move = 5.0;
    assessed.has_positioning_move = true;
    assessed.positioning_move = 2.0;
    result.price_context << assessed;

    // The 13-report horizon is genuinely unevaluable.
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::BrokenReportSequence, 13);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    auto row = [&view](const QString& label, int horizon = -1) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label != label)
                continue;
            if (horizon >= 0 && item.horizon_reports != horizon)
                continue;
            return &item;
        }
        return nullptr;
    };

    const CftcEvidenceItem* long_4 = row(QStringLiteral("Long leg flow (% of prior OI)"), 4);
    QVERIFY(long_4 != nullptr);
    QCOMPARE(long_4->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(long_4->value, QStringLiteral("no material state at this horizon"));

    const CftcEvidenceItem* net_13 = row(QStringLiteral("Net flow (% of prior OI)"), 13);
    QVERIFY(net_13 != nullptr);
    QCOMPARE(net_13->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(net_13->value.contains(QStringLiteral("unavailable")));
    QVERIFY(net_13->value.contains(QStringLiteral("weekly report sequence is broken")));

    const CftcEvidenceItem* relationship = row(QStringLiteral("Price / positioning relationship"));
    QVERIFY(relationship != nullptr);
    QCOMPARE(relationship->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(relationship->value, QStringLiteral("not material enough for a relationship state"));

    const CftcEvidenceItem* price = row(QStringLiteral("Price change (quoted price units) (four reports)"));
    QVERIFY(price != nullptr);
    QCOMPARE(price->status, CftcEvidenceStatus::Available);
    QCOMPARE(price->value, QStringLiteral("+5.00"));
}

void TstCftcPresentation::open_interest_evidence_uses_the_state_horizon() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcInterpretationState oi_state = make_state(QStringLiteral("OI_EXPANSION"), QString());
    oi_state.has_horizon = true;
    oi_state.horizon_reports = 4;
    add_metric(oi_state, QStringLiteral("oi_change"), 7.5);
    result.market_context << oi_state;
    // An unavailable record at a different horizon must not relabel the value.
    add_unavailable(result, QStringLiteral("OI_CONTEXT"), QString(), CftcUnavailableReason::InsufficientHistory, 13);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const CftcEvidenceItem* oi_row = nullptr;
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Open Interest change")))
            oi_row = &item;
    }
    QVERIFY(oi_row != nullptr);
    QCOMPARE(oi_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(oi_row->value, QStringLiteral("+7.50%"));
    QVERIFY(oi_row->label.contains(QStringLiteral("four reports")));
    QVERIFY(!oi_row->label.contains(QStringLiteral("thirteen reports")));
}

void TstCftcPresentation::terminology_caveats_render_only_where_required() {
    CftcInterpretationResult legacy = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* legacy_primary = primary_participant(legacy);
    legacy_primary->states << make_state(QStringLiteral("CROWDED_LONG"), legacy_primary->participant_key);
    const CftcInterpretationView legacy_view = cftc_compose_interpretation(legacy);
    QVERIFY(join_sentences(legacy_view).contains(QStringLiteral("broad non-commercial category")));
    int caveat_count = 0;
    for (const QString& sentence : legacy_view.sentences) {
        if (sentence.contains(QStringLiteral("broad non-commercial category")))
            ++caveat_count;
    }
    QCOMPARE(caveat_count, 1);

    // The caveat is not repeated when no crowding wording is rendered.
    CftcInterpretationResult neutral_legacy = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* neutral_primary = primary_participant(neutral_legacy);
    neutral_primary->states << make_state(QStringLiteral("NET_LONG"), neutral_primary->participant_key);
    QVERIFY(!join_sentences(cftc_compose_interpretation(neutral_legacy))
                 .contains(QStringLiteral("broad non-commercial category")));

    CftcInterpretationResult tff = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* tff_primary = primary_participant(tff);
    tff_primary->states << make_state(QStringLiteral("CROWDED_SHORT"), tff_primary->participant_key);
    const CftcInterpretationView tff_view = cftc_compose_interpretation(tff);
    QVERIFY(join_sentences(tff_view).contains(QStringLiteral("leveraged-funds category")));
    QVERIFY(join_sentences(tff_view).contains(QStringLiteral("outright speculation")));

    // Managed Money and the neutral classes carry no caveat code, so no caveat
    // sentence is invented for them.
    CftcInterpretationResult managed_money = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* mm_primary = primary_participant(managed_money);
    mm_primary->states << make_state(QStringLiteral("CROWDED_LONG"), mm_primary->participant_key);
    const QString mm_text = join_sentences(cftc_compose_interpretation(managed_money));
    QVERIFY(!mm_text.contains(QStringLiteral("broad non-commercial category")));
    QVERIFY(!mm_text.contains(QStringLiteral("leveraged-funds category")));

    const CftcParticipantInterpretation* commercial = find_participant(legacy, QStringLiteral("commercial"));
    QVERIFY(commercial != nullptr);
    QVERIFY(commercial->terminology_caveat_code.isEmpty());
    QVERIFY(cftc_terminology_caveat_sentence(*commercial).isEmpty());
}

void TstCftcPresentation::severe_extreme_is_not_downgraded_to_crowding() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("SEVERE_LONG_EXTREME"), primary->participant_key);
    primary->states << make_state(QStringLiteral("HISTORICALLY_HIGH_NET"), primary->participant_key);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.contains(QStringLiteral("historically severe net long")));
    QVERIFY(!view.headline.contains(QStringLiteral("crowded")));
    QVERIFY(join_sentences(view).contains(QStringLiteral("severe historical net-long extreme")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("broad non-commercial category")));

    CftcInterpretationResult short_result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* short_primary = primary_participant(short_result);
    short_primary->states << make_state(QStringLiteral("SEVERE_SHORT_EXTREME"), short_primary->participant_key);
    short_primary->states << make_state(QStringLiteral("CROWDED_SHORT"), short_primary->participant_key);
    short_primary->states << make_state(QStringLiteral("NET_SHORT"), short_primary->participant_key);
    const CftcInterpretationView short_view = cftc_compose_interpretation(short_result);
    QVERIFY(short_view.headline.contains(QStringLiteral("historically severe net short")));
    QVERIFY(!short_view.headline.contains(QStringLiteral("crowded")));
}

void TstCftcPresentation::insufficient_history_does_not_suppress_direct_states() {
    const QVector<CftcObservation> observations = make_legacy_series(10, 500.0, 900.0, 600.0, 400.0);
    CftcInterpretationInput input = legacy_input(observations);
    const CftcInterpretationResult result = cftc_interpret(input);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.interpreted);
    QVERIFY(view.headline.contains(QStringLiteral("net long")));
    QVERIFY(!view.headline.contains(QStringLiteral("crowded")));
    QVERIFY(!view.headline.contains(QStringLiteral("historically")));

    bool percentile_unavailable = false;
    for (const auto& item : view.evidence) {
        if (item.label.contains(QStringLiteral("percentile")) && item.status == CftcEvidenceStatus::Unavailable)
            percentile_unavailable = true;
    }
    QVERIFY(percentile_unavailable);
    QVERIFY(!conclusions_text(view).contains(QStringLiteral("crowded"), Qt::CaseInsensitive));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));
}

void TstCftcPresentation::visible_range_does_not_change_interpretation() {
    // 190 reports: a quiet history followed by a sharp long build-up, so the
    // full-history result is historically crowded while a one-year window is
    // not. This makes the invariant below non-vacuous.
    QVector<CftcObservation> observations = make_legacy_series(190, 400.0, 400.0, 600.0, 600.0);
    for (int i = 170; i < observations.size(); ++i) {
        const int step = i - 169;
        observations[i].longs[1] = 400.0 + 300.0 * step;
        observations[i].shorts[1] = 600.0 - 20.0 * step;
    }

    const CftcInterpretationInput full_input = legacy_input(observations);
    const CftcInterpretationResult full_result = cftc_interpret(full_input);
    const QVector<CftcObservation> window = cftc_filter_range(observations, CftcRange::OneYear);
    QVERIFY(window.size() < observations.size());
    CftcInterpretationInput window_input = legacy_input(window);
    const CftcInterpretationResult window_result = cftc_interpret(window_input);

    const CftcInterpretationView full_view = cftc_compose_interpretation(full_result);
    const CftcInterpretationView window_view = cftc_compose_interpretation(window_result);
    QVERIFY(full_view.headline.contains(QStringLiteral("historically severe net long")));
    QVERIFY(!window_view.headline.contains(QStringLiteral("crowded")));
    QVERIFY(!window_view.headline.contains(QStringLiteral("severe")));
    QVERIFY(full_view.headline != window_view.headline);

    // The panel builds the engine input from the full validated history: the
    // constructor takes that full vector and has no range parameter, so a range
    // change cannot alter the interpreted observations.
    const CftcInterpretationInput rebuilt = cftc_make_interpretation_input(
        CftcFamily::Legacy, observations, QStringLiteral("futures_and_options_combined"), {}, QString(), false, false);
    QCOMPARE(rebuilt.observations.size(), observations.size());
    const CftcInterpretationView rebuilt_view = cftc_compose_interpretation(cftc_interpret(rebuilt));
    QCOMPARE(rebuilt_view.headline, full_view.headline);
    QCOMPARE(rebuilt_view.sentences, full_view.sentences);
}

void TstCftcPresentation::deterministic_repeatability() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("PERSISTENT_HIGH_EXTREME"), primary->participant_key);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")};
    primary->states << net_state;
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {},
                         50.0, 1500.0);

    const CftcInterpretationView first = cftc_compose_interpretation(result);
    const CftcInterpretationView second = cftc_compose_interpretation(result);
    QCOMPARE(first.headline, second.headline);
    QCOMPARE(first.sentences, second.sentences);
    QCOMPARE(first.context_text, second.context_text);
    QCOMPARE(first.evidence.size(), second.evidence.size());
    for (int i = 0; i < first.evidence.size(); ++i) {
        QCOMPARE(first.evidence[i].label, second.evidence[i].label);
        QCOMPARE(first.evidence[i].value, second.evidence[i].value);
        QCOMPARE(first.evidence[i].status, second.evidence[i].status);
    }
}

void TstCftcPresentation::vocabulary_audit() {
    CftcFamily families[] = {CftcFamily::Legacy, CftcFamily::Disaggregated, CftcFamily::Tff};
    for (CftcFamily family : families) {
        CftcInterpretationResult result = make_result(family);
        CftcParticipantInterpretation* primary = primary_participant(result);
        primary->states << make_state(QStringLiteral("SEVERE_LONG_EXTREME"), primary->participant_key);
        primary->states << make_state(QStringLiteral("PERSISTENT_HIGH_EXTREME"), primary->participant_key);
        primary->states << make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);
        primary->states << make_state(QStringLiteral("LONG_LIQUIDATION"), primary->participant_key, 4);
        primary->states << make_state(QStringLiteral("SHORT_BUILDING"), primary->participant_key, 4);
        add_price_assessment(result, primary->participant_key, 4,
                             QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                             {QStringLiteral("LONG_LIQUIDATION"), QStringLiteral("SHORT_BUILDING")}, 100.0, -2000.0);
        const CftcInterpretationView view = cftc_compose_interpretation(result);
        QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
                 qPrintable(forbidden_language(conclusions_text(view))));
        QVERIFY(view.context_text.contains(QStringLiteral("not a price forecast")));
        QVERIFY(view.context_text.contains(QStringLiteral("historical context")));
    }
}

void TstCftcPresentation::horizon_selection_changes_the_horizon_specific_interpretation() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcInterpretationState h1 = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 1);
    add_metric(h1, QStringLiteral("net_flow"), 1.5);
    h1.has_move_rank = true;
    h1.move_rank = 0.80;
    h1.move_rank_reference_count = 156;
    primary->states << h1;

    CftcInterpretationState h4 = make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);
    add_metric(h4, QStringLiteral("net_flow"), -2.5);
    h4.has_move_rank = true;
    h4.move_rank = 0.77;
    h4.move_rank_reference_count = 156;
    primary->states << h4;

    CftcInterpretationState h13 = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 13);
    add_metric(h13, QStringLiteral("net_flow"), 8.0);
    h13.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION")};
    h13.has_move_rank = true;
    h13.move_rank = 0.95;
    h13.move_rank_reference_count = 156;
    primary->states << h13;
    CftcInterpretationState accum13 = make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 13);
    add_metric(accum13, QStringLiteral("move_value"), 12.5);
    primary->states << accum13;

    const CftcInterpretationView one = cftc_compose_horizon_interpretation(result, 1);
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    const CftcInterpretationView thirteen = cftc_compose_horizon_interpretation(result, 13);

    QCOMPARE(one.horizon_reports, 1);
    QCOMPARE(four.horizon_reports, 4);
    QCOMPARE(thirteen.horizon_reports, 13);

    QVERIFY(one.headline.contains(QStringLiteral("longward repositioning over the last one report")));
    QVERIFY(four.headline.contains(QStringLiteral("shortward repositioning over the last four reports")));
    QVERIFY(thirteen.headline.contains(QStringLiteral("long accumulation over the last thirteen reports")));

    QVERIFY(join_sentences(one).contains(QStringLiteral("single weekly observation")));
    QVERIFY(join_sentences(four).contains(QStringLiteral("shortward over the last four reports")));
    QVERIFY(join_sentences(thirteen).contains(QStringLiteral("Long exposure increased materially")));

    auto evidence_value = [](const CftcInterpretationView& view, const QString& label) {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return item.value;
        }
        return QString();
    };
    const QString net_flow_label = QStringLiteral("Net flow (% of prior OI)");
    QCOMPARE(evidence_value(one, net_flow_label), QStringLiteral("+1.50%"));
    QCOMPARE(evidence_value(four, net_flow_label), QStringLiteral("-2.50%"));
    QCOMPARE(evidence_value(thirteen, net_flow_label), QStringLiteral("+8.00%"));

    // The level conclusion is full-history and identical at every selection.
    QCOMPARE(one.sentences.first(), four.sentences.first());
    QCOMPARE(four.sentences.first(), thirteen.sentences.first());
}

void TstCftcPresentation::horizon_selection_keeps_the_156_report_state_and_percentile() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("PERSISTENT_HIGH_EXTREME"), primary->participant_key);

    CftcInterpretationState h1 = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 1);
    add_metric(h1, QStringLiteral("net_flow"), 1.0);
    primary->states << h1;
    CftcInterpretationState h4 = make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);
    add_metric(h4, QStringLiteral("net_flow"), -1.0);
    primary->states << h4;
    CftcInterpretationState h13 = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 13);
    add_metric(h13, QStringLiteral("net_flow"), 2.0);
    primary->states << h13;

    const CftcInterpretationView one = cftc_compose_horizon_interpretation(result, 1);
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    const CftcInterpretationView thirteen = cftc_compose_horizon_interpretation(result, 13);

    // The crowding/persistence state and the trailing percentile are computed
    // from the full 156-prior-report history, so switching 1W | 4W | 13W cannot
    // change them.
    QCOMPARE(one.headline, four.headline);
    QCOMPARE(four.headline, thirteen.headline);
    QVERIFY(one.headline.contains(QStringLiteral("historically crowded long")));
    QVERIFY(one.headline.contains(QStringLiteral("extreme persisting")));
    QCOMPARE(one.sentences.first(), four.sentences.first());
    QCOMPARE(four.sentences.first(), thirteen.sentences.first());

    const QString percentile_label = cftc_interpretation_percentile_label();
    auto percentile_value = [&percentile_label](const CftcInterpretationView& view) {
        for (const auto& item : view.evidence) {
            if (item.label == percentile_label)
                return item.value;
        }
        return QString();
    };
    QCOMPARE(percentile_value(one), QStringLiteral("95.0% (n=156)"));
    QCOMPARE(percentile_value(four), percentile_value(one));
    QCOMPARE(percentile_value(thirteen), percentile_value(one));
}

void TstCftcPresentation::horizon_view_distinguishes_evaluated_not_material_from_unavailable() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::BrokenReportSequence, 4);

    const CftcInterpretationView one = cftc_compose_horizon_interpretation(result, 1);
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);

    QVERIFY(join_sentences(one).contains(QStringLiteral("was evaluated but did not cross the materiality threshold")));
    QVERIFY(join_sentences(four).contains(QStringLiteral("weekly report sequence is broken")));
    QVERIFY(!join_sentences(four).contains(QStringLiteral("no material state")));

    auto row = [](const CftcInterpretationView& view, const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* one_long = row(one, QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(one_long != nullptr);
    QCOMPARE(one_long->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(one_long->value, QStringLiteral("evaluated, below the materiality threshold"));

    const CftcEvidenceItem* four_long = row(four, QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(four_long != nullptr);
    QCOMPARE(four_long->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(four_long->value.contains(QStringLiteral("unavailable")));
    QVERIFY(four_long->value.contains(QStringLiteral("weekly report sequence is broken")));

    const CftcEvidenceItem* one_oi = row(one, QStringLiteral("Open Interest change (% change) (one report)"));
    QVERIFY(one_oi != nullptr);
    QCOMPARE(one_oi->status, CftcEvidenceStatus::NoMaterialState);
}

void TstCftcPresentation::horizon_view_carries_concrete_unavailable_reasons() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 13);

    const CftcInterpretationView thirteen = cftc_compose_horizon_interpretation(result, 13);
    QVERIFY(join_sentences(thirteen).contains(QStringLiteral("fewer than the 156 prior reports")));
    const CftcEvidenceItem* net_row = nullptr;
    for (const auto& item : thirteen.evidence) {
        if (item.label == QStringLiteral("Net flow (% of prior OI)"))
            net_row = &item;
    }
    QVERIFY(net_row != nullptr);
    QCOMPARE(net_row->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(net_row->value.contains(QStringLiteral("fewer than the 156 prior reports")));

    // The same participant at the four-report horizon is evaluated, so its
    // non-material statement is not confused with the thirteen-report reason.
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    QVERIFY(join_sentences(four).contains(QStringLiteral("was evaluated but did not cross the materiality threshold")));
}

void TstCftcPresentation::horizon_view_price_pending_versus_failed() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    add_metric(net_state, QStringLiteral("net_flow"), 4.0);
    primary->states << net_state;
    add_missing_price_assessment(result, primary->participant_key, 4);

    const CftcInterpretationView pending =
        cftc_compose_horizon_interpretation(result, 4, CftcPriceContextState::Pending);
    QVERIFY(join_sentences(pending).contains(QStringLiteral("Price context is still loading")));
    QVERIFY(join_sentences(pending).contains(QStringLiteral("shifted longward over the last four reports")));

    const QString failure = QStringLiteral("the Yahoo Finance history request failed");
    const CftcInterpretationView failed =
        cftc_compose_horizon_interpretation(result, 4, CftcPriceContextState::Unavailable, failure);
    QVERIFY(join_sentences(failed).contains(failure));
    QVERIFY(!join_sentences(failed).contains(QStringLiteral("still loading")));
    QVERIFY(join_sentences(failed).contains(QStringLiteral("shifted longward over the last four reports")));

    auto row = [](const CftcInterpretationView& view, const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label.startsWith(label))
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* pending_price = row(pending, QStringLiteral("Price change"));
    QVERIFY(pending_price != nullptr);
    QCOMPARE(pending_price->status, CftcEvidenceStatus::Pending);
    QVERIFY(pending_price->value.contains(QStringLiteral("pending")));

    const CftcEvidenceItem* failed_price = row(failed, QStringLiteral("Price change"));
    QVERIFY(failed_price != nullptr);
    QCOMPARE(failed_price->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(failed_price->value.contains(failure));

    const CftcEvidenceItem* failed_relationship = row(failed, QStringLiteral("Price / positioning relationship"));
    QVERIFY(failed_relationship != nullptr);
    QCOMPARE(failed_relationship->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(failed_relationship->value.contains(failure));
}

void TstCftcPresentation::horizon_view_open_interest_context() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcInterpretationState oi_state = make_state(QStringLiteral("OI_EXPANSION"), QString(), 4);
    add_metric(oi_state, QStringLiteral("oi_change"), 7.5);
    result.market_context << oi_state;
    add_unavailable(result, QStringLiteral("OI_CONTEXT"), QString(), CftcUnavailableReason::NonPositiveOpenInterest,
                    13);

    const CftcInterpretationView one = cftc_compose_horizon_interpretation(result, 1);
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    const CftcInterpretationView thirteen = cftc_compose_horizon_interpretation(result, 13);

    QVERIFY(join_sentences(four).contains(QStringLiteral("Open Interest expanded over the last four reports")));
    QVERIFY(join_sentences(one).contains(
        QStringLiteral("Open Interest change over the last one report was evaluated but did not cross the "
                       "materiality threshold")));
    QVERIFY(join_sentences(thirteen).contains(QStringLiteral("Open Interest is zero or negative")));
    QVERIFY(!join_sentences(thirteen).contains(QStringLiteral("no material state")));

    auto oi_row = [](const CftcInterpretationView& view) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label.startsWith(QStringLiteral("Open Interest change")))
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* four_oi = oi_row(four);
    QVERIFY(four_oi != nullptr);
    QCOMPARE(four_oi->status, CftcEvidenceStatus::Available);
    QCOMPARE(four_oi->value, QStringLiteral("+7.50%"));
    const CftcEvidenceItem* one_oi = oi_row(one);
    QVERIFY(one_oi != nullptr);
    QCOMPARE(one_oi->status, CftcEvidenceStatus::NoMaterialState);
    const CftcEvidenceItem* thirteen_oi = oi_row(thirteen);
    QVERIFY(thirteen_oi != nullptr);
    QCOMPARE(thirteen_oi->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(thirteen_oi->value.contains(QStringLiteral("Open Interest is zero or negative")));
}

void TstCftcPresentation::horizon_view_evidence_is_tagged_with_the_selected_horizon() {
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    bool saw_flow_row = false;
    for (const auto& item : four.evidence) {
        if (item.label == QStringLiteral("Long leg flow (% of prior OI)") ||
            item.label == QStringLiteral("Net flow (% of prior OI)")) {
            saw_flow_row = true;
            QCOMPARE(item.horizon_reports, 4);
        }
    }
    QVERIFY(saw_flow_row);
}

void TstCftcPresentation::unsupported_horizon_falls_back_to_combined_view() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    const CftcInterpretationView combined = cftc_compose_interpretation(result);
    const CftcInterpretationView invalid = cftc_compose_horizon_interpretation(result, 2);
    QCOMPARE(invalid.horizon_reports, 0);
    QCOMPARE(invalid.headline, combined.headline);
    QCOMPARE(invalid.sentences, combined.sentences);
    QCOMPARE(invalid.evidence.size(), combined.evidence.size());
}

void TstCftcPresentation::neutral_legacy_metric_label_has_no_speculator_wording() {
    const QString neutral = cftc_metric_participant_display_name(CftcFamily::Legacy, QStringLiteral("non_commercial"),
                                                                 QStringLiteral("Non-Commercial (Speculators)"));
    QCOMPARE(neutral, QStringLiteral("Non-Commercial"));
    QVERIFY(!neutral.contains(QStringLiteral("Speculator"), Qt::CaseInsensitive));

    QCOMPARE(cftc_metric_participant_display_name(CftcFamily::Legacy, QStringLiteral("commercial"),
                                                  QStringLiteral("Commercial")),
             QStringLiteral("Commercial"));
    QCOMPARE(cftc_metric_participant_display_name(CftcFamily::Disaggregated, QStringLiteral("managed_money"),
                                                  QStringLiteral("Managed Money")),
             QStringLiteral("Managed Money"));
    QCOMPARE(cftc_metric_participant_display_name(CftcFamily::Tff, QStringLiteral("leveraged_funds"),
                                                  QStringLiteral("Leveraged Funds")),
             QStringLiteral("Leveraged Funds"));
}

void TstCftcPresentation::horizon_views_keep_the_forbidden_language_audit() {
    CftcFamily families[] = {CftcFamily::Legacy, CftcFamily::Disaggregated, CftcFamily::Tff};
    for (CftcFamily family : families) {
        CftcInterpretationResult result = make_result(family);
        CftcParticipantInterpretation* primary = primary_participant(result);
        primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
        primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
        CftcInterpretationState net_state =
            make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
        net_state.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")};
        add_metric(net_state, QStringLiteral("net_flow"), 5.0);
        primary->states << net_state;
        CftcInterpretationState accum = make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
        add_metric(accum, QStringLiteral("move_value"), 5.0);
        primary->states << accum;
        add_price_assessment(result, primary->participant_key, 4,
                             QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"),
                             {QStringLiteral("LONG_ACCUMULATION")}, -50.0, 2000.0);

        for (int horizon : {1, 4, 13}) {
            const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, horizon);
            QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
                     qPrintable(forbidden_language(conclusions_text(view))));
        }
    }
}

void TstCftcPresentation::leg_evidence_uses_the_leg_own_record() {
    // The short leg is missing while the long leg is evaluated and material:
    // the long row must stay available and must not inherit the short leg's
    // missing-data reason.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    CftcInterpretationState long_state = make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    add_metric(long_state, QStringLiteral("move_value"), 5.0);
    primary->states << long_state;
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::MissingParticipantLeg, 4);
    add_unavailable(result, QStringLiteral("GROSS_FLOW"), primary->participant_key,
                    CftcUnavailableReason::MissingParticipantLeg, 4);

    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
    auto row = [&view](const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* long_row = row(QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(long_row != nullptr);
    QCOMPARE(long_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(long_row->value, QStringLiteral("5.00%"));
    const CftcEvidenceItem* short_row = row(QStringLiteral("Short leg flow (% of prior OI)"));
    QVERIFY(short_row != nullptr);
    QCOMPARE(short_row->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(short_row->value.contains(QStringLiteral("long or short leg is missing")));

    // A leg whose direction is known but whose rank reference is too short is
    // unavailable with that reason, not evaluated-not-material.
    CftcInterpretationResult short_ref = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* short_primary = primary_participant(short_ref);
    short_primary->states << make_state(QStringLiteral("NET_LONG"), short_primary->participant_key);
    add_unavailable(short_ref, QStringLiteral("NET_SHIFT"), short_primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4);
    add_unavailable(short_ref, QStringLiteral("GROSS_FLOW"), short_primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4, QStringLiteral("LONG_ACCUMULATION"));
    const CftcInterpretationView short_ref_view = cftc_compose_horizon_interpretation(short_ref, 4);
    const CftcEvidenceItem* ref_long = nullptr;
    const CftcEvidenceItem* ref_short = nullptr;
    for (const auto& item : short_ref_view.evidence) {
        if (item.label == QStringLiteral("Long leg flow (% of prior OI)"))
            ref_long = &item;
        if (item.label == QStringLiteral("Short leg flow (% of prior OI)"))
            ref_short = &item;
    }
    QVERIFY(ref_long != nullptr);
    QCOMPARE(ref_long->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(ref_long->value.contains(QStringLiteral("fewer than the 156 prior reports")));
    // The short leg has no record of its own, so the horizon-level reason is
    // the truthful fallback rather than a claimed evaluation.
    QVERIFY(ref_short != nullptr);
    QCOMPARE(ref_short->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(ref_short->value.contains(QStringLiteral("fewer than the 156 prior reports")));
}

void TstCftcPresentation::non_material_open_interest_evidence_is_explicit() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
    const CftcEvidenceItem* oi_row = nullptr;
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Open Interest change")))
            oi_row = &item;
    }
    QVERIFY(oi_row != nullptr);
    QCOMPARE(oi_row->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(oi_row->value, QStringLiteral("evaluated, below the materiality threshold"));
}

void TstCftcPresentation::price_evidence_keeps_quoted_units_precision() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {},
                         -87.8002929688, -2.0);

    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4, CftcPriceContextState::Ready);
    const CftcEvidenceItem* price_row = nullptr;
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Price change")))
            price_row = &item;
    }
    QVERIFY(price_row != nullptr);
    QCOMPARE(price_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(price_row->value, QStringLiteral("-87.80"));

    // The combined contract keeps the same quoted-price precision.
    const CftcInterpretationView combined = cftc_compose_interpretation(result, CftcPriceContextState::Ready);
    const CftcEvidenceItem* combined_price = nullptr;
    for (const auto& item : combined.evidence) {
        if (item.label.startsWith(QStringLiteral("Price change")))
            combined_price = &item;
    }
    QVERIFY(combined_price != nullptr);
    QCOMPARE(combined_price->value, QStringLiteral("-87.80"));
}

void TstCftcPresentation::horizon_evidence_uses_emitted_readings() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcHorizonFlowReading reading;
    reading.horizon_reports = 4;
    reading.evaluated = true;
    reading.has_long_flow = true;
    reading.long_flow = 0.2848;
    reading.has_short_flow = true;
    reading.short_flow = -1.7211;
    reading.has_net_flow = true;
    reading.net_flow = 2.0059;
    reading.has_long_rank = true;
    reading.long_rank = 0.25;
    reading.has_short_rank = true;
    reading.short_rank = 0.25;
    reading.has_net_rank = true;
    reading.net_rank = 0.25;
    reading.net_rank_reference_count = 156;
    primary->flow_readings << reading;

    CftcOpenInterestReading oi;
    oi.horizon_reports = 4;
    oi.evaluated = true;
    oi.has_oi_change = true;
    oi.oi_change = 0.8957;
    oi.has_rank = true;
    oi.rank = 0.30;
    oi.rank_reference_count = 156;
    result.open_interest_readings << oi;

    // Price context failed: the CFTC-only measurements must still be shown.
    const CftcInterpretationView view = cftc_compose_horizon_interpretation(
        result, 4, CftcPriceContextState::Unavailable, QStringLiteral("provider failed"));

    auto row = [&view](const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* long_row = row(QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(long_row != nullptr);
    QCOMPARE(long_row->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(long_row->value, QStringLiteral("0.28% (below threshold)"));
    const CftcEvidenceItem* short_row = row(QStringLiteral("Short leg flow (% of prior OI)"));
    QVERIFY(short_row != nullptr);
    QCOMPARE(short_row->value, QStringLiteral("-1.72% (below threshold)"));
    const CftcEvidenceItem* net_row = row(QStringLiteral("Net flow (% of prior OI)"));
    QVERIFY(net_row != nullptr);
    QCOMPARE(net_row->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(net_row->value, QStringLiteral("+2.01% (below threshold)"));
    const CftcEvidenceItem* rank_row = row(QStringLiteral("Move materiality rank (% of prior moves)"));
    QVERIFY(rank_row != nullptr);
    QCOMPARE(rank_row->value, QStringLiteral("25.0% (n=156) (below threshold)"));
    const CftcEvidenceItem* oi_row = row(QStringLiteral("Open Interest change (% change) (four reports)"));
    QVERIFY(oi_row != nullptr);
    QCOMPARE(oi_row->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(oi_row->value, QStringLiteral("+0.90% (below threshold)"));
}

void TstCftcPresentation::raw_reading_without_materiality_reference_is_not_below_threshold() {
    // The raw 4R flows exist but the strictly trailing reference is too short,
    // so no materiality comparison was performed. The rows keep the values and
    // must state that the materiality rank is unavailable; they must not claim
    // the move was compared with the threshold and found below it.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcHorizonFlowReading reading;
    reading.horizon_reports = 4;
    reading.evaluated = true;
    reading.has_long_flow = true;
    reading.long_flow = 0.2848;
    reading.has_short_flow = true;
    reading.short_flow = -1.7211;
    reading.has_net_flow = true;
    reading.net_flow = 2.0059;
    reading.has_long_rank = false;
    reading.has_short_rank = false;
    reading.has_net_rank = false;
    primary->flow_readings << reading;
    add_unavailable(result, QStringLiteral("GROSS_FLOW"), primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4, QStringLiteral("LONG_ACCUMULATION"));
    add_unavailable(result, QStringLiteral("GROSS_FLOW"), primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4, QStringLiteral("SHORT_COVERING"));
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4);

    const QString reason = QStringLiteral("fewer than the 156 prior reports this reference requires are available");
    auto row = [](const CftcInterpretationView& view, const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return &item;
        }
        return nullptr;
    };

    const CftcInterpretationView horizon_view = cftc_compose_horizon_interpretation(result, 4);
    const CftcEvidenceItem* long_row = row(horizon_view, QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(long_row != nullptr);
    QCOMPARE(long_row->status, CftcEvidenceStatus::Unavailable);
    QCOMPARE(long_row->value, QStringLiteral("0.28% (materiality unavailable — ") + reason + QLatin1Char(')'));
    QVERIFY(!long_row->value.contains(QStringLiteral("below threshold")));
    const CftcEvidenceItem* short_row = row(horizon_view, QStringLiteral("Short leg flow (% of prior OI)"));
    QVERIFY(short_row != nullptr);
    QCOMPARE(short_row->status, CftcEvidenceStatus::Unavailable);
    QCOMPARE(short_row->value, QStringLiteral("-1.72% (materiality unavailable — ") + reason + QLatin1Char(')'));
    const CftcEvidenceItem* net_row = row(horizon_view, QStringLiteral("Net flow (% of prior OI)"));
    QVERIFY(net_row != nullptr);
    QCOMPARE(net_row->status, CftcEvidenceStatus::Unavailable);
    QCOMPARE(net_row->value, QStringLiteral("+2.01% (materiality unavailable — ") + reason + QLatin1Char(')'));
    QVERIFY(!net_row->value.contains(QStringLiteral("below threshold")));
    const CftcEvidenceItem* rank_row = row(horizon_view, QStringLiteral("Move materiality rank (% of prior moves)"));
    QVERIFY(rank_row != nullptr);
    QCOMPARE(rank_row->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(rank_row->value.contains(reason));
    QVERIFY(!rank_row->value.contains(QStringLiteral("no material state")));

    // The combined contract view carries the same truthful wording.
    const CftcInterpretationView combined = cftc_compose_interpretation(result);
    const CftcEvidenceItem* combined_net = row(combined, QStringLiteral("Net flow (% of prior OI)"));
    QVERIFY(combined_net != nullptr);
    QCOMPARE(combined_net->status, CftcEvidenceStatus::Unavailable);
    QCOMPARE(combined_net->value, QStringLiteral("+2.01% (materiality unavailable — ") + reason + QLatin1Char(')'));
}

void TstCftcPresentation::net_delta_reason_never_comes_from_price() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    const QString key = expected_principal_key(CftcFamily::Legacy);
    // The CFTC net measurement is unavailable because the report sequence is
    // broken while the price assessment also reports a missing price context:
    // the positioning cell reason must come from the CFTC path.
    add_unavailable(result, QStringLiteral("NET_SHIFT"), key, CftcUnavailableReason::BrokenReportSequence, 4);
    CftcPricePositionAssessment assessment;
    assessment.participant_key = key;
    assessment.horizon_reports = 4;
    assessment.reason = CftcUnavailableReason::MissingPriceContext;
    result.price_context.append(assessment);
    QCOMPARE(cftc_net_delta_unavailable_reason(result, key, 4), QStringLiteral("the weekly report sequence is broken"));
    QVERIFY(!cftc_net_delta_unavailable_reason(result, key, 4).contains(QStringLiteral("price")));

    // Without a NET_SHIFT record the participant's own horizon reading carries
    // the reason; with no CFTC record at all the helper stays empty instead of
    // borrowing the price wording.
    CftcInterpretationResult fallback = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(fallback);
    QCOMPARE(cftc_net_delta_unavailable_reason(fallback, key, 13), QString());
    CftcHorizonFlowReading reading;
    reading.horizon_reports = 13;
    reading.evaluated = false;
    reading.reason = CftcUnavailableReason::BrokenReportSequence;
    primary->flow_readings << reading;
    QCOMPARE(cftc_net_delta_unavailable_reason(fallback, key, 13),
             QStringLiteral("the weekly report sequence is broken"));
}

// ── 2026-09-24 audit corrections ─────────────────────────────────────────────

void TstCftcPresentation::historically_high_net_short_is_not_high_exposure() {
    // M1: a high percentile of a net-short participant's Net %OI is its
    // smallest recent net short, never "historically high exposure".
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->net_pct_oi = -8.49;
    primary->states << make_state(QStringLiteral("NET_SHORT"), primary->participant_key);
    primary->states << make_state(QStringLiteral("HISTORICALLY_HIGH_NET"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
    QVERIFY2(view.headline.startsWith(QStringLiteral("Leveraged Funds — net short, historically small")),
             qPrintable(view.headline));
    QVERIFY(join_sentences(view).contains(QStringLiteral(
        "Leveraged Funds is net short, but the net short position is unusually small relative to its recent history "
        "(Net %OI in the top 10% of its previous 156 reports; MarketLab threshold).")));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));

    // The mirror: net long with a historically low Net %OI.
    CftcInterpretationResult mirror = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* mirror_primary = primary_participant(mirror);
    mirror_primary->states << make_state(QStringLiteral("NET_LONG"), mirror_primary->participant_key);
    mirror_primary->states << make_state(QStringLiteral("HISTORICALLY_LOW_NET"), mirror_primary->participant_key);
    const CftcInterpretationView mirror_view = cftc_compose_horizon_interpretation(mirror, 4);
    QVERIFY(mirror_view.headline.startsWith(QStringLiteral("Managed Money — net long, historically small")));
    QVERIFY(join_sentences(mirror_view)
                .contains(QStringLiteral("Managed Money is net long, but the net long position is unusually small")));
    QVERIFY(join_sentences(mirror_view).contains(QStringLiteral("bottom 10% of its previous 156 reports")));
}

void TstCftcPresentation::unavailable_level_and_persistence_are_stated() {
    const QString key = expected_principal_key(CftcFamily::Legacy);

    // M2 (a): the historical comparison could not be evaluated.
    CftcInterpretationResult level = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* level_primary = primary_participant(level);
    level.open_interest_available = false;
    level_primary->historical_percentile_available = false;
    level_primary->has_net_pct_oi = false;
    level_primary->states << make_state(QStringLiteral("NET_LONG"), key);
    add_unavailable(level, QStringLiteral("HISTORICAL_RELATIVE_STATE"), key,
                    CftcUnavailableReason::MissingOpenInterest);
    const CftcInterpretationView level_view = cftc_compose_horizon_interpretation(level, 4);
    QVERIFY2(level_view.headline.contains(QStringLiteral("net long (historical level unavailable)")),
             qPrintable(level_view.headline));
    QVERIFY(join_sentences(level_view)
                .contains(QStringLiteral("Non-Commercial is net long in the latest report; its historical level could "
                                         "not be evaluated: Open Interest is missing.")));

    // M2 (b): in the band, but persistence could not be evaluated.
    CftcInterpretationResult persistence = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* persistence_primary = primary_participant(persistence);
    persistence_primary->states << make_state(QStringLiteral("NET_LONG"), key);
    persistence_primary->states << make_state(QStringLiteral("HISTORICALLY_HIGH_NET"), key);
    persistence_primary->states << make_state(QStringLiteral("CROWDED_LONG"), key);
    add_unavailable(persistence, QStringLiteral("EXTREME_TRANSITION"), key, CftcUnavailableReason::InsufficientHistory,
                    -1, QStringLiteral("PERSISTENT_HIGH_EXTREME"));
    const QString persistence_text = join_sentences(cftc_compose_horizon_interpretation(persistence, 4));
    QVERIFY(persistence_text.contains(QStringLiteral("is unusually net long")));
    QVERIFY2(!persistence_text.contains(QStringLiteral("remains")), "persistence was not established");
    QVERIFY(persistence_text.contains(
        QStringLiteral("Whether the extreme reading has persisted for 3 consecutive reports could not be evaluated: "
                       "fewer than the 156 prior reports this reference requires are available.")));

    // M2 (c): a displayed net shift whose sustained check was unevaluable.
    CftcInterpretationResult sustained = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* sustained_primary = primary_participant(sustained);
    sustained_primary->states << make_state(QStringLiteral("NET_LONG"), key);
    sustained_primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), key, 13);
    add_unavailable(sustained, QStringLiteral("SUSTAINED_REPOSITIONING"), key,
                    CftcUnavailableReason::MissingParticipantLeg, 13);
    QVERIFY(join_sentences(cftc_compose_horizon_interpretation(sustained, 13))
                .contains(QStringLiteral("Whether the move was sustained across the weekly reports could not be "
                                         "evaluated: the reported long or short leg is missing.")));
}

void TstCftcPresentation::remains_requires_established_persistence() {
    // L1: the first report in the band "is" unusually net long; only an
    // established persistent extreme "remains" there.
    CftcInterpretationResult first = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* first_primary = primary_participant(first);
    first_primary->states << make_state(QStringLiteral("NET_LONG"), first_primary->participant_key);
    first_primary->states << make_state(QStringLiteral("CROWDED_LONG"), first_primary->participant_key);
    const QString first_text = join_sentences(cftc_compose_horizon_interpretation(first, 4));
    QVERIFY(first_text.contains(QStringLiteral("Managed Money is unusually net long relative to its recent history")));
    QVERIFY(!first_text.contains(QStringLiteral("remains")));

    first_primary->states << make_state(QStringLiteral("PERSISTENT_HIGH_EXTREME"), first_primary->participant_key);
    const QString persistent_text = join_sentences(cftc_compose_horizon_interpretation(first, 4));
    QVERIFY(persistent_text.contains(
        QStringLiteral("Managed Money remains unusually net long relative to its recent history")));
    QVERIFY(persistent_text.contains(QStringLiteral("has persisted for at least 3 consecutive reports")));
}

void TstCftcPresentation::divergence_names_the_leg_that_supplied_the_move() {
    // L2: the audit's 10-Year case. Long accumulation (+5.62 % of prior OI) was
    // the only individually material leg, but short covering (-8.48 %)
    // supplied 60 % of the +14.10 % net move, so the long leg is not the
    // primary driver.
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_SHORT"), primary->participant_key);
    add_flow_reading(*primary, 13, 5.62, -8.48);
    add_price_assessment(result, primary->participant_key, 13, QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"),
                         {QStringLiteral("LONG_ACCUMULATION")}, -4.0, 14.10);
    const QString text = join_sentences(cftc_compose_horizon_interpretation(result, 13));
    QVERIFY2(text.contains(QStringLiteral("The longward shift came mostly from short covering (60% of the net move), "
                                          "with long accumulation contributing 40%.")),
             qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("primarily")));

    // A leg that worked against the move is named as an offset, not a share.
    CftcInterpretationResult offset = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* offset_primary = primary_participant(offset);
    offset_primary->states << make_state(QStringLiteral("NET_SHORT"), offset_primary->participant_key);
    add_flow_reading(*offset_primary, 4, 10.0, 4.0);
    add_price_assessment(offset, offset_primary->participant_key, 4,
                         QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"), {QStringLiteral("LONG_ACCUMULATION")},
                         -4.0, 6.0);
    QVERIFY(join_sentences(cftc_compose_horizon_interpretation(offset, 4))
                .contains(QStringLiteral(
                    "The longward shift came from long accumulation; short building partly offset it.")));
}

void TstCftcPresentation::disagreement_sentence_states_both_values() {
    // L3: the disagreement is only narrated with its two measured values and
    // the Open Interest change that explains it.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    CftcInterpretationState disagreement =
        make_state(QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), primary->participant_key, 4);
    add_metric(disagreement, QStringLiteral("net_flow"), 5.0);
    add_metric(disagreement, QStringLiteral("net_share_change"), -2.5);
    add_metric(disagreement, QStringLiteral("oi_change"), 100.0);
    primary->states << disagreement;
    QVERIFY(join_sentences(cftc_compose_horizon_interpretation(result, 4))
                .contains(QStringLiteral("Over four reports the raw net flow was +5.00% of prior Open Interest, but "
                                         "Net %OI changed by -2.50 percentage points, because Open Interest changed "
                                         "by +100.00%.")));
}

void TstCftcPresentation::extreme_transitions_do_not_claim_continuation() {
    // L4: an exit or unwind describes the latest transition, not a process
    // that is still "developing".
    CftcInterpretationResult unwind = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* unwind_primary = primary_participant(unwind);
    unwind_primary->states << make_state(QStringLiteral("NET_LONG"), unwind_primary->participant_key);
    unwind_primary->states << make_state(QStringLiteral("UNWINDING_HIGH_EXTREME"), unwind_primary->participant_key);
    const CftcInterpretationView unwind_view = cftc_compose_horizon_interpretation(unwind, 4);
    QVERIFY(unwind_view.headline.contains(QStringLiteral("moved well back from a recent extreme")));
    QVERIFY(join_sentences(unwind_view)
                .contains(QStringLiteral("Net %OI has fallen back to or below the 75th percentile within 13 reports "
                                         "of a persistent high extreme (MarketLab threshold).")));

    CftcInterpretationResult exit = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* exit_primary = primary_participant(exit);
    exit_primary->states << make_state(QStringLiteral("NET_LONG"), exit_primary->participant_key);
    exit_primary->states << make_state(QStringLiteral("EXITED_HIGH_EXTREME"), exit_primary->participant_key);
    const CftcInterpretationView exit_view = cftc_compose_horizon_interpretation(exit, 4);
    QVERIFY(exit_view.headline.contains(QStringLiteral("left the extreme band in the latest report")));
    QVERIFY(join_sentences(exit_view).contains(
        QStringLiteral("Net %OI moved back below the high extreme band in the latest report.")));
    for (const CftcInterpretationView* view : {&unwind_view, &exit_view}) {
        QVERIFY(!conclusions_text(*view).contains(QStringLiteral("developing")));
        QVERIFY2(forbidden_language(conclusions_text(*view)).isEmpty(),
                 qPrintable(forbidden_language(conclusions_text(*view))));
    }
}

void TstCftcPresentation::sustained_wording_states_the_actual_count() {
    // L5: "sustained" states the actual share of weekly steps and the rule's
    // requirement instead of claiming every weekly report moved that way.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 13);
    CftcInterpretationState sustained =
        make_state(QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R"), primary->participant_key, 13);
    add_metric(sustained, QStringLiteral("confirming_steps"), 11.0);
    primary->states << sustained;
    const QString text = join_sentences(cftc_compose_horizon_interpretation(result, 13));
    QVERIFY(text.contains(QStringLiteral("The move was sustained: net positioning moved longward in 11 of the last 13 "
                                         "weekly reports (at least 9 required; MarketLab threshold).")));
    QVERIFY(!text.contains(QStringLiteral("across the individual weekly reports")));
}

void TstCftcPresentation::net_pct_unavailable_reason_names_the_cause() {
    // L6: zero Open Interest, missing Open Interest and a missing leg are
    // different causes and keep different reasons.
    auto net_pct_row = [](const CftcInterpretationView& view) -> QString {
        for (const auto& item : view.evidence) {
            if (item.label.startsWith(QStringLiteral("Net %OI")))
                return item.value;
        }
        return {};
    };
    CftcInterpretationResult zero_oi = make_result(CftcFamily::Legacy);
    zero_oi.open_interest = 0.0;
    CftcParticipantInterpretation* zero_primary = primary_participant(zero_oi);
    zero_primary->has_net_pct_oi = false;
    zero_primary->states << make_state(QStringLiteral("NET_LONG"), zero_primary->participant_key);
    QCOMPARE(net_pct_row(cftc_compose_horizon_interpretation(zero_oi, 4)),
             QStringLiteral("unavailable — Open Interest is zero or negative"));

    CftcInterpretationResult missing_oi = zero_oi;
    missing_oi.open_interest_available = false;
    QCOMPARE(net_pct_row(cftc_compose_horizon_interpretation(missing_oi, 4)),
             QStringLiteral("unavailable — Open Interest is missing"));

    CftcInterpretationResult missing_leg = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* leg_primary = primary_participant(missing_leg);
    leg_primary->has_net_pct_oi = false;
    leg_primary->net_available = false;
    add_unavailable(missing_leg, QStringLiteral("NET_EXPOSURE"), leg_primary->participant_key,
                    CftcUnavailableReason::MissingParticipantLeg);
    QCOMPARE(net_pct_row(cftc_compose_horizon_interpretation(missing_leg, 4)),
             QStringLiteral("unavailable — the reported long or short leg is missing"));

    // The per-cause history reasons read differently.
    const QString reference = cftc_unavailable_reason_wording(CftcUnavailableReason::InsufficientHistory);
    const QString anchor = cftc_unavailable_reason_wording(CftcUnavailableReason::InsufficientHorizonHistory);
    const QString price = cftc_unavailable_reason_wording(CftcUnavailableReason::InsufficientPriceHistory);
    const QString concentration =
        cftc_unavailable_reason_wording(CftcUnavailableReason::InsufficientConcentrationHistory);
    QVERIFY(reference != anchor && reference != price && reference != concentration && anchor != price);
    QVERIFY(!anchor.contains(QStringLiteral("156")));
    QVERIFY(concentration.contains(QStringLiteral("8 prior concentration readings")));
    QVERIFY(price.contains(QStringLiteral("price moves")));
}

void TstCftcPresentation::one_report_sentence_matches_percentile_availability() {
    // L7: the one-report note describes the actual reference, and only when
    // the historical level exists.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    const QString with_level = join_sentences(cftc_compose_horizon_interpretation(result, 1));
    QVERIFY(with_level.contains(QStringLiteral("the historical level above compares the latest Net %OI with the "
                                               "previous 156 reports and does not depend on the selected horizon.")));
    QVERIFY(!with_level.contains(QStringLiteral("full validated history")));

    primary->historical_percentile_available = false;
    const CftcInterpretationView without = cftc_compose_horizon_interpretation(result, 1);
    QVERIFY(without.sentences.contains(QStringLiteral("The one-report reading is a single weekly observation.")));
    QVERIFY(!join_sentences(without).contains(QStringLiteral("does not depend on the selected horizon")));
}

void TstCftcPresentation::missing_leg_composes_a_partial_view() {
    // L8: a missing principal leg keeps the rest of the report (the other leg,
    // Open Interest) instead of collapsing the view, the same way for either
    // leg.
    for (bool long_missing : {true, false}) {
        CftcInterpretationResult result = make_result(CftcFamily::Legacy);
        CftcParticipantInterpretation* primary = primary_participant(result);
        const QString key = primary->participant_key;
        primary->net_available = false;
        primary->has_net_pct_oi = false;
        primary->historical_percentile_available = false;
        add_unavailable(result, QStringLiteral("NET_EXPOSURE"), key, CftcUnavailableReason::MissingParticipantLeg);
        add_unavailable(result, QStringLiteral("HISTORICAL_RELATIVE_STATE"), key,
                        CftcUnavailableReason::MissingParticipantLeg);
        for (int horizon : {1, 4, 13})
            add_unavailable(result, QStringLiteral("NET_SHIFT"), key, CftcUnavailableReason::MissingParticipantLeg,
                            horizon);
        CftcInterpretationState oi = make_state(QStringLiteral("OI_EXPANSION"), QString(), 4);
        oi.participant_key.clear();
        add_metric(oi, QStringLiteral("oi_change"), 12.0);
        result.market_context << oi;
        if (!long_missing)
            primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), key, 4);

        const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
        QVERIFY2(view.interpreted, long_missing ? "missing long leg" : "missing short leg");
        QVERIFY(view.headline.contains(QStringLiteral("positioning unavailable")));
        const QString text = join_sentences(view);
        QVERIFY(text.contains(QStringLiteral(
            "Current Non-Commercial net exposure is unavailable: the reported long or short leg is missing.")));
        QVERIFY(text.contains(QStringLiteral("Open Interest expanded over the last four reports.")));
    }
}

void TstCftcPresentation::market_concentration_is_surfaced_market_level() {
    // L13: the computed market-concentration states reach the card, as a
    // market-wide statement that is never attributed to a participant.
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    CftcConcentrationAssessment concentration;
    concentration.field_key = QStringLiteral("concentration_gross_4_long");
    concentration.has_current = true;
    concentration.current = 38.5;
    concentration.has_percentile = true;
    concentration.percentile = 0.95;
    concentration.percentile_reference_count = 800;
    concentration.has_change = true;
    concentration.change = 1.4;
    concentration.has_change_rank = true;
    concentration.change_rank = 0.955;
    concentration.change_reference_count = 156;
    result.concentration << concentration;
    CftcInterpretationState high = make_state(QStringLiteral("HIGH_MARKET_CONCENTRATION"), QString());
    high.participant_key.clear();
    CftcInterpretationState rising = make_state(QStringLiteral("CONCENTRATION_RISING"), QString());
    rising.participant_key.clear();
    result.market_context << high << rising;

    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
    QString high_sentence;
    QString rising_sentence;
    for (const QString& sentence : view.sentences) {
        if (sentence.startsWith(QStringLiteral("Market concentration is high")))
            high_sentence = sentence;
        if (sentence.startsWith(QStringLiteral("Market concentration rose")))
            rising_sentence = sentence;
    }
    QCOMPARE(high_sentence,
             QStringLiteral("Market concentration is high: the four largest long traders hold 38.5% of Open Interest, "
                            "in the top 10% of that ratio's own history (market-wide, not a participant category; "
                            "MarketLab threshold)."));
    QVERIFY(rising_sentence.contains(QStringLiteral("rose by +1.40 percentage points in the latest weekly report")));
    for (const QString& sentence : {high_sentence, rising_sentence})
        QVERIFY(!sentence.contains(QStringLiteral("Managed Money")));
    bool evidence_row = false;
    for (const auto& item : view.evidence) {
        if (item.label == QStringLiteral("Market concentration (largest 4 long traders, % of OI)")) {
            evidence_row = true;
            QCOMPARE(item.status, CftcEvidenceStatus::Available);
            QVERIFY(item.value.startsWith(QStringLiteral("38.5%")));
        }
    }
    QVERIFY(evidence_row);
}

void TstCftcPresentation::combined_view_below_threshold_legs_are_not_available() {
    // L14: in the combined view a raw leg flow that fired no state is an
    // evaluated-below-threshold row, never "Available".
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_flow_reading(*primary, 4, 0.28, -1.72, 0.25);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    int rows = 0;
    for (const auto& item : view.evidence) {
        if (item.horizon_reports != 4)
            continue;
        if (item.label.startsWith(QStringLiteral("Long leg flow")) ||
            item.label.startsWith(QStringLiteral("Short leg flow"))) {
            ++rows;
            QCOMPARE(item.status, CftcEvidenceStatus::NoMaterialState);
            QVERIFY(item.value.contains(QStringLiteral("(below threshold)")));
        }
    }
    QCOMPARE(rows, 2);
}

void TstCftcPresentation::heuristic_thresholds_are_labelled_inline() {
    // L15: historical conclusions carry their MarketLab threshold inline and
    // the card states once what "material" means.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
    QVERIFY(view.sentences.first().contains(
        QStringLiteral("(Net %OI in the top 10% of its previous 156 reports; MarketLab threshold)")));
    QCOMPARE(view.sentences.last(),
             QStringLiteral("Method: “material” means a move at or above the 75th percentile of the previous 156 "
                            "comparable moves, and historical levels compare Net %OI with the previous 156 reports. "
                            "These thresholds are MarketLab heuristics, not CFTC definitions."));

    CftcInterpretationResult severe = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* severe_primary = primary_participant(severe);
    severe_primary->states << make_state(QStringLiteral("NET_SHORT"), severe_primary->participant_key);
    severe_primary->states << make_state(QStringLiteral("SEVERE_SHORT_EXTREME"), severe_primary->participant_key);
    QVERIFY(join_sentences(cftc_compose_horizon_interpretation(severe, 4))
                .contains(QStringLiteral("(Net %OI in the bottom 2.5% of its previous 156 reports; MarketLab "
                                         "threshold)")));
}

void TstCftcPresentation::price_conclusions_name_the_series_and_sessions() {
    // H2 / M5: the price conclusion names the proxy, its roll caveat, the two
    // closing sessions and the move in quoted units and percent.
    auto evaluated_result = [](bool spot) {
        CftcInterpretationResult result = make_result(CftcFamily::Legacy);
        CftcParticipantInterpretation* primary = primary_participant(result);
        primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
        result.price_requested = true;
        result.price_source = spot ? QStringLiteral("Yahoo Finance — ^VIX spot index")
                                   : QStringLiteral("Yahoo Finance — GC=F front-month continuous futures");
        result.price_continuous_proxy = !spot;
        result.price_spot_index = spot;
        CftcPricePositionAssessment assessment;
        assessment.participant_key = primary->participant_key;
        assessment.horizon_reports = 4;
        assessment.evaluated = true;
        assessment.has_price_move = true;
        assessment.price_move = -87.8;
        assessment.price_move_pct = -1.99;
        assessment.price_anchor_date = QDate(2026, 8, 18);
        assessment.price_latest_date = QDate(2026, 9, 15);
        assessment.has_price_move_rank = true;
        assessment.price_move_rank = 0.442;
        assessment.price_reference_count = 156;
        assessment.has_positioning_move = true;
        assessment.positioning_move = 2.01;
        result.price_context << assessment;
        return result;
    };
    const CftcInterpretationView view = cftc_compose_horizon_interpretation(evaluated_result(false), 4);
    QVERIFY2(join_sentences(view).contains(QStringLiteral(
                 "Price series: Yahoo Finance — GC=F front-month continuous futures (not roll-adjusted, so a contract "
                 "roll inside the window is part of the move); closes 2026-08-18 → 2026-09-15: -87.80 (-1.99%).")),
             qPrintable(join_sentences(view)));
    bool price_row = false;
    bool rank_row = false;
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Price change"))) {
            price_row = true;
            QCOMPARE(item.value, QStringLiteral("-87.80 (-1.99%; 2026-08-18 → 2026-09-15)"));
        }
        if (item.label.startsWith(QStringLiteral("Price move materiality rank"))) {
            rank_row = true;
            QCOMPARE(item.status, CftcEvidenceStatus::NoMaterialState);
            QCOMPARE(item.value, QStringLiteral("44.2% (n=156) (below threshold)"));
        }
    }
    QVERIFY(price_row);
    QVERIFY(rank_row);

    const QString spot = join_sentences(cftc_compose_horizon_interpretation(evaluated_result(true), 4));
    QVERIFY(spot.contains(QStringLiteral(
        "Price series: Yahoo Finance — ^VIX spot index (a spot index, not the futures contract whose positions are "
        "reported); closes 2026-08-18 → 2026-09-15: -87.80 (-1.99%).")));
}

void TstCftcPresentation::two_material_legs_without_net_move_headline() {
    // M6: the audit's 10-Year 2011-05-31 case. Longs +10.44 % and shorts
    // +13.70 % both material, net -3.27 % not material: the headline is a gross
    // expansion, never "long accumulation".
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_SHORT"), primary->participant_key);
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 13);
    primary->states << make_state(QStringLiteral("SHORT_BUILDING"), primary->participant_key, 13);
    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 13);
    QVERIFY2(view.headline.endsWith(QStringLiteral("long and short exposure both increased over the last thirteen "
                                                   "reports")),
             qPrintable(view.headline));
    QVERIFY(!view.headline.contains(QStringLiteral("long accumulation")));

    CftcInterpretationResult single = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* single_primary = primary_participant(single);
    single_primary->states << make_state(QStringLiteral("NET_SHORT"), single_primary->participant_key);
    single_primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), single_primary->participant_key, 13);
    QVERIFY(cftc_compose_horizon_interpretation(single, 13)
                .headline.endsWith(QStringLiteral(
                    "long exposure increased without a material net change over the last thirteen reports")));
}

void TstCftcPresentation::outdated_report_is_flagged_in_headline_and_body() {
    // M7: a discontinued contract's last report is never presented as current.
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    result.report_date = QDate(2026, 3, 3);
    result.report_age_available = true;
    result.report_age_days = 205;
    result.report_outdated = true;
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_SHORT"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
    QVERIFY2(view.headline.endsWith(QStringLiteral(" · out-of-date report (2026-03-03, 205 days old)")),
             qPrintable(view.headline));
    QVERIFY(view.sentences.first().startsWith(
        QStringLiteral("Out of date: this is the latest report the history carries, dated 2026-03-03, 205 days before "
                       "today.")));
    QVERIFY(view.context_text.startsWith(QStringLiteral("Out of date:")));

    CftcInterpretationResult current = make_result(CftcFamily::Tff);
    current.report_age_available = true;
    current.report_age_days = 9;
    CftcParticipantInterpretation* current_primary = primary_participant(current);
    current_primary->states << make_state(QStringLiteral("NET_SHORT"), current_primary->participant_key);
    const CftcInterpretationView current_view = cftc_compose_horizon_interpretation(current, 4);
    QVERIFY(!current_view.headline.contains(QStringLiteral("out-of-date")));
    QVERIFY(!current_view.sentences.first().startsWith(QStringLiteral("Out of date")));
}

void TstCftcPresentation::golden_real_gold_legacy_report() {
    // Tests-audit item 14: a frozen real CFTC payload through the production
    // path (provider-shaped rows -> parser -> basis check -> engine ->
    // composer). Every expected value below was computed independently from
    // the raw CFTC rows by a separate implementation of the v2 rules (exact
    // rational ranks), not taken from this engine's output.
    const CftcHistory history = cftc_parse_history(gold_legacy_futures_only_rows(), CftcFamily::Legacy);
    QVERIFY2(history.error.isEmpty(), qPrintable(history.error));
    QCOMPARE(history.observations.size(), 200);
    QVERIFY(cftc_history_basis_error(history.observations, /*futures_only=*/true).isEmpty());
    QVERIFY(!cftc_history_basis_error(history.observations, /*futures_only=*/false).isEmpty());

    const CftcInterpretationInput input =
        cftc_make_interpretation_input(CftcFamily::Legacy, history.observations, QStringLiteral("futures_only"), {},
                                       QString(), false, false, QDate(2026, 9, 24));
    const CftcInterpretationResult result = cftc_interpret(input);
    QCOMPARE(result.rule_set_version, QStringLiteral("cftc-descriptive-interpretation-v2"));
    QCOMPARE(result.report_date, QDate(2026, 9, 15));
    QVERIFY(result.report_basis_verified);
    QVERIFY(result.report_age_available);
    QCOMPARE(result.report_age_days, 9);
    QVERIFY(!result.report_outdated);
    QCOMPARE(result.open_interest, 409899.0);

    const CftcParticipantInterpretation* non_commercial = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(non_commercial);
    QCOMPARE(non_commercial->net_position, 230338.0);
    QVERIFY(qAbs(non_commercial->net_pct_oi - 56.19384287348834) < 1e-9);
    QCOMPARE(non_commercial->percentile_reference_count, 156);
    QCOMPARE(non_commercial->percentile, 148.0 / 156.0);
    QCOMPARE(state_keys(*non_commercial),
             (QSet<QString>{QStringLiteral("NET_LONG"), QStringLiteral("HISTORICALLY_HIGH_NET"),
                            QStringLiteral("CROWDED_LONG"), QStringLiteral("PERSISTENT_HIGH_EXTREME"),
                            QStringLiteral("LONG_ACCUMULATION@13"), QStringLiteral("NET_LONGWARD_SHIFT@13")}));
    for (const auto& reading : non_commercial->flow_readings) {
        QVERIFY(reading.evaluated);
        if (reading.horizon_reports == 13) {
            QVERIFY(qAbs(reading.long_flow - 13.830784) < 1e-6);
            QVERIFY(qAbs(reading.short_flow - (-0.938909)) < 1e-6);
            QVERIFY(qAbs(reading.net_flow - 14.769693) < 1e-6);
            QCOMPARE(reading.net_rank, 118.0 / 156.0);
        } else if (reading.horizon_reports == 4) {
            QVERIFY(qAbs(reading.long_flow - 0.284793) < 1e-6);
            QVERIFY(qAbs(reading.short_flow - (-1.721065)) < 1e-6);
            QVERIFY(qAbs(reading.net_flow - 2.005858) < 1e-6);
            QCOMPARE(reading.net_rank, 0.25);
        } else {
            QVERIFY(qAbs(reading.net_flow - (-0.394429)) < 1e-6);
            QCOMPARE(reading.net_rank, 22.0 / 156.0);
        }
    }

    const CftcParticipantInterpretation* commercial = find_participant(result, QStringLiteral("commercial"));
    QVERIFY(commercial);
    QCOMPARE(commercial->net_position, -261721.0);
    QCOMPARE(commercial->percentile, 2.0 / 156.0);
    QCOMPARE(state_keys(*commercial),
             (QSet<QString>{QStringLiteral("NET_SHORT"), QStringLiteral("HISTORICALLY_LOW_NET"),
                            QStringLiteral("SEVERE_SHORT_EXTREME"), QStringLiteral("PERSISTENT_LOW_EXTREME"),
                            QStringLiteral("SHORT_BUILDING@13"), QStringLiteral("NET_SHORTWARD_SHIFT@13")}));

    const CftcParticipantInterpretation* non_reportable = find_participant(result, QStringLiteral("non_reportable"));
    QVERIFY(non_reportable);
    QCOMPARE(non_reportable->net_position, 31383.0);
    QCOMPARE(non_reportable->percentile, 108.0 / 156.0);
    QCOMPARE(state_keys(*non_reportable),
             (QSet<QString>{QStringLiteral("NET_LONG"), QStringLiteral("EXITED_HIGH_EXTREME"),
                            QStringLiteral("LONG_LIQUIDATION@1"), QStringLiteral("LONG_LIQUIDATION@4"),
                            QStringLiteral("NET_SHORTWARD_SHIFT@1"), QStringLiteral("NET_SHORTWARD_SHIFT@4")}));

    QStringList market_states;
    for (const auto& state : result.market_context)
        market_states << QStringLiteral("%1@%2").arg(state.state_id).arg(state.horizon_reports);
    QCOMPARE(market_states, QStringList{QStringLiteral("OI_EXPANSION@13")});
    for (const auto& reading : result.open_interest_readings) {
        if (reading.horizon_reports == 13) {
            QVERIFY(qAbs(reading.oi_change - 20.79656971090089) < 1e-9);
            QCOMPARE(reading.rank, 140.0 / 156.0);
        }
    }

    // The composed conclusions the page shows for this report.
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(
        result, 4, CftcPriceContextState::Unavailable, QStringLiteral("no price series in this test"));
    QCOMPARE(four.headline, QStringLiteral("Non-Commercial — historically crowded long; extreme persisting"));
    QCOMPARE(four.sentences.value(0),
             QStringLiteral("Non-Commercial remains unusually net long relative to its recent history (Net %OI in the "
                            "top 10% of its previous 156 reports; MarketLab threshold)."));
    QCOMPARE(four.sentences.value(1),
             QStringLiteral("Non-Commercial is the CFTC's broad non-commercial category; the crowding description "
                            "applies to aggregate reported positioning only."));
    QCOMPARE(four.sentences.value(2),
             QStringLiteral("The elevated reading has persisted for at least 3 consecutive reports."));
    QCOMPARE(four.sentences.value(3), QStringLiteral("Net repositioning over the last four reports was evaluated but "
                                                     "did not cross the materiality threshold."));
    QVERIFY(four.sentences.contains(
        QStringLiteral("Price relationship over the last four reports is unavailable: no price series in this test.")));
    const QString thirteen = join_sentences(cftc_compose_horizon_interpretation(result, 13));
    QVERIFY(thirteen.contains(QStringLiteral("Net positioning shifted longward over the last thirteen reports.")));
    QVERIFY(thirteen.contains(QStringLiteral("Long exposure increased materially.")));
    QVERIFY(thirteen.contains(QStringLiteral("Open Interest expanded over the last thirteen reports.")));
    for (int horizon : {1, 4, 13}) {
        const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, horizon);
        QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
                 qPrintable(forbidden_language(conclusions_text(view))));
    }
}

QTEST_GUILESS_MAIN(TstCftcPresentation)
#include "tst_cftc_presentation.moc"
