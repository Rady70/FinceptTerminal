// tests/tst_cftc_monitor_visual.cpp
//
// MarketLab CFTC UI/UX batch: the presentation-only cross-market visual model
// (screens/economics/panels/CftcMonitorVisualModel.h). It pins:
//   * the primary metric readings are read from the qualified monitor fields:
//     a measure the engine did not form stays unavailable (never zero), an
//     exact zero stays a formed zero, and an unevaluated flow horizon never
//     yields a plotting value;
//   * exact value text: "—" unavailable, unsigned "0.00%" at zero, signed
//     otherwise, percentile in display units;
//   * asset-class grouping follows the qualified catalog order and keeps an
//     unknown-class market in an explicit trailing group instead of dropping
//     it;
//   * the presentation filters never turn a data-quality state into an
//     ordinary one (notable-only keeps every alerting market);
//   * one shared absolute scale across groups, with a safe non-zero floor;
//   * the summary keeps current, stored, outdated and unavailable separate;
//   * the attention-class tone mapping is the top alert class, and the status
//     tone is fail-closed;
//   * every label the model can produce is descriptive and contains no
//     BUY/HOLD/SELL, bullish/bearish, forecast, confidence or recommendation
//     vocabulary.
// Header-only over Qt Core; no app sources (tests/ HARD RULE).
#include "screens/economics/panels/CftcMonitorVisualModel.h"
#include "services/economics/CftcMarketCatalog.h"
#include "services/economics/CftcMonitorModel.h"

#include <QRegularExpression>
#include <QtTest>

#include <cmath>

using namespace fincept::screens;
using namespace fincept::services;

namespace {

CftcMonitorEntry make_entry(const QString& key, CftcMonitorStatus status) {
    CftcMonitorEntry entry;
    entry.market_key = key;
    const CftcMarketDefinition definition = cftc_market_definition(key);
    entry.label = definition.label;
    entry.asset_class = definition.asset_class;
    entry.known_market = cftc_market_is_known(key);
    entry.status = status;
    entry.family = CftcFamily::Legacy;
    entry.futures_only = true;
    return entry;
}

void add_alert(CftcMonitorEntry& entry, CftcAttentionClass classification) {
    CftcAlert alert;
    alert.attention_class = classification;
    alert.state_id = classification == CftcAttentionClass::DataQuality ? QString() : QStringLiteral("OI_EXPANSION");
    entry.alerts.append(alert);
    entry.requires_attention = true;
}

CftcMonitorEntry make_valued_entry(const QString& key) {
    CftcMonitorEntry entry = make_entry(key, CftcMonitorStatus::Ok);
    entry.has_net_pct_oi = true;
    entry.net_pct_oi = 12.5;
    entry.has_percentile = true;
    entry.percentile = 0.875;
    entry.percentile_reference_count = 156;
    for (int horizon : {1, 4, 13}) {
        CftcHorizonFlowReading flow;
        flow.horizon_reports = horizon;
        flow.evaluated = true;
        flow.has_net_flow = true;
        flow.net_flow = horizon * 0.5;
        entry.flow_readings.append(flow);
    }
    return entry;
}

bool contains_forbidden(const QString& text) {
    const QString lower = text.toLower();
    for (const QString& phrase :
         {QStringLiteral("buy"), QStringLiteral("sell"), QStringLiteral("hold"), QStringLiteral("bullish"),
          QStringLiteral("bearish"), QStringLiteral("forecast"), QStringLiteral("expected return"),
          QStringLiteral("confidence"), QStringLiteral("recommend"), QStringLiteral("trade today"),
          QStringLiteral("sit out"), QStringLiteral("caution"), QStringLiteral("opportunity"),
          QStringLiteral("conviction"), QStringLiteral("composite score"), QStringLiteral("ranking")}) {
        if (lower.contains(phrase))
            return true;
    }
    // The standalone strategy token "GO" must not appear as a judgment either.
    static const QRegularExpression standalone_go(QStringLiteral("\\bgo\\b"));
    return standalone_go.match(lower).hasMatch();
}

} // namespace

class TstCftcMonitorVisual : public QObject {
    Q_OBJECT
  private slots:
    void metric_reading_distinguishes_missing_from_zero();
    void metric_text_formats_exact_values();
    void groups_follow_catalog_order_and_keep_unknown();
    void filters_keep_data_quality_states_notable();
    void scale_extent_is_shared_across_groups();
    void summary_counts_states_separately();
    void attention_tone_is_the_top_class();
    void status_tone_is_fail_closed();
    void uninterpretable_report_is_not_current();
    void labels_are_descriptive_only();
};

void TstCftcMonitorVisual::metric_reading_distinguishes_missing_from_zero() {
    CftcMonitorEntry missing = make_entry(QStringLiteral("gold"), CftcMonitorStatus::NoData);
    missing.net_pct_oi = 0.0;
    missing.percentile = 0.0;
    for (CftcMonitorMetric metric : {CftcMonitorMetric::NetPctOi, CftcMonitorMetric::Percentile,
                                     CftcMonitorMetric::Net1R, CftcMonitorMetric::Net4R, CftcMonitorMetric::Net13R}) {
        const CftcMonitorMetricReading reading = cftc_monitor_metric_reading(missing, metric);
        QVERIFY2(!reading.has_value, qPrintable(cftc_monitor_metric_label(metric)));
        QCOMPARE(reading.value, 0.0);
    }

    CftcMonitorEntry zero = make_entry(QStringLiteral("gold"), CftcMonitorStatus::Ok);
    zero.has_net_pct_oi = true;
    zero.net_pct_oi = 0.0;
    zero.has_percentile = true;
    zero.percentile = 0.0;
    const CftcMonitorMetricReading zero_net = cftc_monitor_metric_reading(zero, CftcMonitorMetric::NetPctOi);
    QVERIFY(zero_net.has_value);
    QCOMPARE(zero_net.value, 0.0);
    const CftcMonitorMetricReading zero_percentile = cftc_monitor_metric_reading(zero, CftcMonitorMetric::Percentile);
    QVERIFY(zero_percentile.has_value);
    QCOMPARE(zero_percentile.value, 0.0);

    // An unevaluated horizon is unavailable even if a stale flag says a flow
    // exists; a formed horizon reads the emitted value.
    CftcMonitorEntry flow = make_entry(QStringLiteral("gold"), CftcMonitorStatus::Ok);
    CftcHorizonFlowReading unevaluated;
    unevaluated.horizon_reports = 1;
    unevaluated.evaluated = false;
    unevaluated.has_net_flow = true;
    unevaluated.net_flow = 4.0;
    CftcHorizonFlowReading evaluated;
    evaluated.horizon_reports = 4;
    evaluated.evaluated = true;
    evaluated.has_net_flow = true;
    evaluated.net_flow = -3.5;
    flow.flow_readings = {unevaluated, evaluated};
    QVERIFY(!cftc_monitor_metric_reading(flow, CftcMonitorMetric::Net1R).has_value);
    const CftcMonitorMetricReading four = cftc_monitor_metric_reading(flow, CftcMonitorMetric::Net4R);
    QVERIFY(four.has_value);
    QCOMPARE(four.value, -3.5);
    QVERIFY(!cftc_monitor_metric_reading(flow, CftcMonitorMetric::Net13R).has_value);
}

void TstCftcMonitorVisual::metric_text_formats_exact_values() {
    CftcMonitorMetricReading missing;
    QCOMPARE(cftc_monitor_metric_text(missing, CftcMonitorMetric::NetPctOi), QStringLiteral("—"));
    QCOMPARE(cftc_monitor_metric_text(missing, CftcMonitorMetric::Percentile), QStringLiteral("—"));

    CftcMonitorMetricReading zero;
    zero.has_value = true;
    zero.value = 0.0;
    QCOMPARE(cftc_monitor_metric_text(zero, CftcMonitorMetric::NetPctOi), QStringLiteral("0.00%"));
    QCOMPARE(cftc_monitor_metric_text(zero, CftcMonitorMetric::Net1R), QStringLiteral("0.00%"));

    CftcMonitorMetricReading positive;
    positive.has_value = true;
    positive.value = 2.5;
    QCOMPARE(cftc_monitor_metric_text(positive, CftcMonitorMetric::NetPctOi), QStringLiteral("+2.50%"));

    CftcMonitorMetricReading negative;
    negative.has_value = true;
    negative.value = -2.5;
    QCOMPARE(cftc_monitor_metric_text(negative, CftcMonitorMetric::Net13R), QStringLiteral("-2.50%"));

    CftcMonitorMetricReading percentile;
    percentile.has_value = true;
    percentile.value = 87.5;
    QCOMPARE(cftc_monitor_metric_text(percentile, CftcMonitorMetric::Percentile), QStringLiteral("87.5%"));
}

void TstCftcMonitorVisual::groups_follow_catalog_order_and_keep_unknown() {
    QVector<CftcMonitorEntry> entries;
    entries.append(make_entry(QStringLiteral("corn"), CftcMonitorStatus::Ok));
    entries.append(make_entry(QStringLiteral("gold"), CftcMonitorStatus::Ok));
    entries.append(make_entry(QStringLiteral("silver"), CftcMonitorStatus::Ok));
    entries.append(make_entry(QStringLiteral("us_dollar_index"), CftcMonitorStatus::Ok));
    CftcMonitorEntry unknown = make_entry(QStringLiteral("micro_gold"), CftcMonitorStatus::UnknownMarket);
    entries.append(unknown);

    const QVector<CftcMonitorGroup> groups = cftc_monitor_groups(entries);
    QStringList labels;
    for (const CftcMonitorGroup& group : groups)
        labels << group.label;
    QCOMPARE(labels, QStringList({QStringLiteral("Metals"), QStringLiteral("Agriculture"),
                                  QStringLiteral("US Dollar Index"), QStringLiteral("Unknown market")}));
    // Catalog (not encounter) order inside the group.
    const CftcMonitorGroup& metals = groups.first();
    QCOMPARE(metals.entry_indexes, QVector<int>({1, 2}));
    QCOMPARE(groups.last().entry_indexes, QVector<int>({4}));

    // A group filter selects only that class; an unknown-class market is not
    // silently merged into a catalog group.
    const QVector<CftcMonitorGroup> filtered = cftc_monitor_groups(entries, QStringLiteral("metals"), false);
    QCOMPARE(filtered.size(), 1);
    QCOMPARE(filtered.first().asset_class, QStringLiteral("metals"));
    QCOMPARE(filtered.first().entry_indexes, QVector<int>({1, 2}));

    // Notable-only keeps only entries requiring attention, and a data-quality
    // unknown market is notable by construction.
    entries[4].requires_attention = true;
    const QVector<CftcMonitorGroup> notable = cftc_monitor_groups(entries, QString(), true);
    QCOMPARE(notable.size(), 1);
    QCOMPARE(notable.first().entry_indexes, QVector<int>({4}));
}

void TstCftcMonitorVisual::filters_keep_data_quality_states_notable() {
    CftcMonitorEntry problem = make_entry(QStringLiteral("gold"), CftcMonitorStatus::Unavailable);
    CftcAlert quality;
    quality.attention_class = CftcAttentionClass::DataQuality;
    quality.detail_reason = QStringLiteral("the data is unavailable");
    problem.alerts.append(quality);
    problem.requires_attention = true;
    QVERIFY(cftc_monitor_filter_matches(problem, QString(), true));
    QVERIFY(cftc_monitor_filter_matches(problem, QStringLiteral("metals"), true));

    CftcMonitorEntry quiet = make_entry(QStringLiteral("silver"), CftcMonitorStatus::Ok);
    QVERIFY(!cftc_monitor_filter_matches(quiet, QString(), true));
    QVERIFY(cftc_monitor_filter_matches(quiet, QString(), false));
    QVERIFY(!cftc_monitor_filter_matches(quiet, QStringLiteral("energy"), false));
}

void TstCftcMonitorVisual::scale_extent_is_shared_across_groups() {
    QVector<CftcMonitorEntry> entries;
    CftcMonitorEntry small = make_valued_entry(QStringLiteral("gold"));
    small.net_pct_oi = -10.0;
    CftcMonitorEntry large = make_valued_entry(QStringLiteral("corn"));
    large.net_pct_oi = 42.5;
    CftcMonitorEntry missing = make_entry(QStringLiteral("vix"), CftcMonitorStatus::NoData);
    entries = {small, large, missing};

    const double extent = cftc_monitor_metric_scale_extent(entries, CftcMonitorMetric::NetPctOi);
    QCOMPARE(extent, 42.5);
    // The same extent must hold when only one group is displayed: the scale is
    // a property of the universe, not of the visible subset.
    const QVector<CftcMonitorGroup> filtered = cftc_monitor_groups(entries, QStringLiteral("metals"), false);
    QCOMPARE(filtered.size(), 1);
    QCOMPARE(cftc_monitor_metric_scale_extent(entries, CftcMonitorMetric::NetPctOi), extent);

    // An all-missing universe still yields a usable, non-zero floor.
    QVector<CftcMonitorEntry> none;
    none.append(make_entry(QStringLiteral("gold"), CftcMonitorStatus::NoData));
    QCOMPARE(cftc_monitor_metric_scale_extent(none, CftcMonitorMetric::NetPctOi), 1.0);

    // The percentile axis is the bounded 0..100 rank, not the largest value
    // observed in the scan: the labelled 100% end must always mean 100%.
    QCOMPARE(cftc_monitor_metric_scale_extent(entries, CftcMonitorMetric::Percentile), 100.0);
    QVector<CftcMonitorEntry> high_only;
    high_only.append(make_valued_entry(QStringLiteral("corn"))); // percentile 87.5
    QCOMPARE(cftc_monitor_metric_scale_extent(high_only, CftcMonitorMetric::Percentile), 100.0);
}

void TstCftcMonitorVisual::summary_counts_states_separately() {
    QVector<CftcMonitorEntry> entries;
    entries.append(make_valued_entry(QStringLiteral("gold")));

    CftcMonitorEntry outdated = make_valued_entry(QStringLiteral("silver"));
    outdated.report_outdated = true;
    outdated.report_age_available = true;
    outdated.report_age_days = 30;
    entries.append(outdated);

    entries.append(make_entry(QStringLiteral("corn"), CftcMonitorStatus::ArchiveOnly));
    entries.append(make_entry(QStringLiteral("vix"), CftcMonitorStatus::NoData));

    CftcMonitorEntry unavailable = make_entry(QStringLiteral("bitcoin"), CftcMonitorStatus::Unavailable);
    add_alert(unavailable, CftcAttentionClass::DataQuality);
    entries.append(unavailable);

    CftcMonitorEntry extreme = make_valued_entry(QStringLiteral("copper"));
    add_alert(extreme, CftcAttentionClass::Extreme);
    entries.append(extreme);

    const CftcMonitorSummary summary = cftc_monitor_summary(entries, CftcMonitorMetric::NetPctOi);
    QCOMPARE(summary.total, 6);
    QCOMPARE(summary.current, 2); // gold, copper
    QCOMPARE(summary.outdated, 1);
    QCOMPARE(summary.stored, 1);
    QCOMPARE(summary.problem, 2);
    QCOMPARE(summary.attention, 2);
    QCOMPARE(summary.metric_available, 3); // gold, silver, copper
    QCOMPARE(summary.class_counts.size(), 6);
    for (const CftcMonitorClassCount& count : summary.class_counts) {
        if (count.classification == CftcAttentionClass::DataQuality)
            QCOMPARE(count.markets, 1);
        else if (count.classification == CftcAttentionClass::Extreme)
            QCOMPARE(count.markets, 1);
        else
            QCOMPARE(count.markets, 0);
    }
}

void TstCftcMonitorVisual::attention_tone_is_the_top_class() {
    CftcMonitorEntry entry = make_valued_entry(QStringLiteral("gold"));
    QCOMPARE(cftc_monitor_entry_tone(entry), CftcMonitorTone::Ordinary);
    add_alert(entry, CftcAttentionClass::Repositioning);
    QCOMPARE(cftc_monitor_entry_tone(entry), CftcMonitorTone::Repositioning);
    add_alert(entry, CftcAttentionClass::Extreme);
    QCOMPARE(cftc_monitor_entry_tone(entry), CftcMonitorTone::Extreme);
    add_alert(entry, CftcAttentionClass::DataQuality);
    QCOMPARE(cftc_monitor_entry_tone(entry), CftcMonitorTone::DataQuality);
    QCOMPARE(cftc_monitor_top_attention_class(entry), CftcAttentionClass::DataQuality);

    for (const CftcAttentionClass classification :
         {CftcAttentionClass::DataQuality, CftcAttentionClass::Extreme, CftcAttentionClass::ExtremeTransition,
          CftcAttentionClass::Repositioning, CftcAttentionClass::OpenInterest, CftcAttentionClass::Concentration}) {
        QVERIFY(cftc_monitor_class_tone(classification) != CftcMonitorTone::Ordinary);
        QVERIFY(!cftc_attention_class_label(classification).isEmpty());
    }
}

void TstCftcMonitorVisual::status_tone_is_fail_closed() {
    CftcMonitorEntry ok = make_entry(QStringLiteral("gold"), CftcMonitorStatus::Ok);
    QCOMPARE(cftc_monitor_status_tone(ok), CftcMonitorStatusTone::Ordinary);
    ok.report_outdated = true;
    QCOMPARE(cftc_monitor_status_tone(ok), CftcMonitorStatusTone::Warning);
    QCOMPARE(cftc_monitor_status_tone(make_entry(QStringLiteral("silver"), CftcMonitorStatus::ArchiveOnly)),
             CftcMonitorStatusTone::Warning);
    for (const CftcMonitorStatus status : {CftcMonitorStatus::NoData, CftcMonitorStatus::NoLocalHistory,
                                           CftcMonitorStatus::Unavailable, CftcMonitorStatus::UnknownMarket}) {
        QCOMPARE(cftc_monitor_status_tone(make_entry(QStringLiteral("vix"), status)), CftcMonitorStatusTone::Problem);
    }
}

void TstCftcMonitorVisual::uninterpretable_report_is_not_current() {
    // A fresh provider refresh does not authorize "current" when the frozen
    // engine could not interpret the report: the entry keeps status Ok but
    // carries report_unavailable, and the presentation must stay fail-closed.
    CftcMonitorEntry uninterpretable = make_valued_entry(QStringLiteral("gold"));
    uninterpretable.report_unavailable = true;
    uninterpretable.report_unavailable_reason = CftcUnavailableReason::MissingOpenInterest;

    QVERIFY(!cftc_monitor_report_is_current(uninterpretable));
    QCOMPARE(cftc_monitor_status_tone(uninterpretable), CftcMonitorStatusTone::Problem);
    const QString text = cftc_monitor_status_text(uninterpretable);
    QVERIFY2(text != QStringLiteral("OK"), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("unavailable"), Qt::CaseInsensitive), qPrintable(text));

    QVector<CftcMonitorEntry> entries;
    entries.append(uninterpretable);
    const CftcMonitorSummary summary = cftc_monitor_summary(entries, CftcMonitorMetric::NetPctOi);
    QCOMPARE(summary.total, 1);
    QCOMPARE(summary.current, 0);
    QCOMPARE(summary.problem, 1);
    QCOMPARE(summary.outdated, 0);

    // The ordinary case is unchanged.
    CftcMonitorEntry ordinary = make_valued_entry(QStringLiteral("silver"));
    QVERIFY(cftc_monitor_report_is_current(ordinary));
    QCOMPARE(cftc_monitor_status_tone(ordinary), CftcMonitorStatusTone::Ordinary);
    QCOMPARE(cftc_monitor_status_text(ordinary), QStringLiteral("OK"));
    entries.append(ordinary);
    const CftcMonitorSummary mixed = cftc_monitor_summary(entries, CftcMonitorMetric::NetPctOi);
    QCOMPARE(mixed.current, 1);
    QCOMPARE(mixed.problem, 1);

    // An outdated report stays a warning, and the helper that documents
    // "ordinary current data" must not call it current either.
    CftcMonitorEntry outdated = make_valued_entry(QStringLiteral("copper"));
    outdated.report_outdated = true;
    outdated.report_age_available = true;
    outdated.report_age_days = 30;
    QVERIFY(!cftc_monitor_report_is_current(outdated));
    QCOMPARE(cftc_monitor_status_tone(outdated), CftcMonitorStatusTone::Warning);
}

void TstCftcMonitorVisual::labels_are_descriptive_only() {
    const QVector<CftcMonitorMetric> metrics = {CftcMonitorMetric::NetPctOi, CftcMonitorMetric::Percentile,
                                                CftcMonitorMetric::Net1R, CftcMonitorMetric::Net4R,
                                                CftcMonitorMetric::Net13R};
    for (const CftcMonitorMetric metric : metrics) {
        QVERIFY(!cftc_monitor_metric_label(metric).isEmpty());
        QVERIFY(!cftc_monitor_metric_caption(metric).isEmpty());
        QVERIFY(!contains_forbidden(cftc_monitor_metric_label(metric)));
        QVERIFY(!contains_forbidden(cftc_monitor_metric_caption(metric)));
    }
    for (const CftcMonitorTone tone :
         {CftcMonitorTone::Ordinary, CftcMonitorTone::DataQuality, CftcMonitorTone::Extreme,
          CftcMonitorTone::ExtremeTransition, CftcMonitorTone::Repositioning, CftcMonitorTone::OpenInterest,
          CftcMonitorTone::Concentration}) {
        QVERIFY(!cftc_monitor_tone_label(tone).isEmpty());
        QVERIFY(!contains_forbidden(cftc_monitor_tone_label(tone)));
    }
    QVERIFY(!contains_forbidden(cftc_monitor_group_label(QString())));
    QVERIFY(!contains_forbidden(cftc_monitor_group_label(QStringLiteral("metals"))));
    QVERIFY(!contains_forbidden(cftc_monitor_signed_percent(0.0, 2)));
}

QTEST_GUILESS_MAIN(TstCftcMonitorVisual)
#include "tst_cftc_monitor_visual.moc"
