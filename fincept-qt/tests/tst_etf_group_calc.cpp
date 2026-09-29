#include "services/etf/EtfGroupModel.h"

#include <QJsonDocument>
#include <QTest>

using namespace fincept::services::etf;

namespace {
QDateTime utc(const char* s) {
    return QDateTime::fromString(QLatin1String(s), Qt::ISODateWithMs).toUTC();
}

struct SecFixture {
    QVector<StoredObservation> observations;
    SecFilingLineageMap filings;
    qint64 next_id = 1;

    void value(const char* accession, const char* accepted, const char* available, const char* first_seen,
               const char* measure, const QDate& date, double number, bool amended = false, const char* amends = "") {
        StoredObservation o;
        o.observation_id = next_id++;
        o.subject_type = QStringLiteral("reporting_entity");
        o.subject_id = 1;
        o.measure = QLatin1String(measure);
        o.measurement_kind = o.measure == QLatin1String(kNportNetAssets) ? QStringLiteral("aum_observation")
                                                                         : QStringLiteral("regulatory_reported_flow");
        o.units = QStringLiteral("USD");
        o.basis = o.measure == QLatin1String(kNportNetAssets) ? QStringLiteral("regulatory_quarter_end_net_assets")
                                                              : QStringLiteral("nport_monthly_flow");
        o.source_type = QStringLiteral("sec_nport");
        o.source_document = QLatin1String(accession);
        o.effective_date = date;
        o.value = FieldValue::reported_value(number);
        o.accepted_at = utc(accepted);
        o.available_from = utc(available);
        o.first_seen_at = utc(first_seen);
        o.point_in_time_status = QStringLiteral("conservative_rule");
        o.revision_state = amended ? QStringLiteral("amended_filing") : QStringLiteral("original");
        observations.append(o);
        filings.insert(o.source_document,
                       SecFilingLineage{o.source_document,
                                        amended ? QStringLiteral("NPORT-P/A") : QStringLiteral("NPORT-P"),
                                        QLatin1String(amends)});
    }
    void flow(const char* accession, const char* accepted, const char* available, const char* first_seen, double sales,
              double redemption, double reinvestment, bool amended = false, const char* amends = "") {
        for (const auto& v : {std::pair{kNportSales, sales}, std::pair{kNportRedemption, redemption},
                              std::pair{kNportReinvestment, reinvestment}})
            value(accession, accepted, available, first_seen, v.first, QDate(2026, 6, 30), v.second, amended, amends);
    }
    RegulatoryFlowAnalytics calculate(const DerivedTimeFrame& tf) const {
        return compute_regulatory_flow_analytics(observations, tf, filings);
    }
};

GroupFlowMember sec(const QString& key, const RegulatoryFlowAnalytics& analytics) {
    GroupFlowMember member;
    member.subject_type = QStringLiteral("reporting_entity");
    member.stable_key = key;
    member.reporting_key = key;
    member.identity_basis = QStringLiteral("curated_reporting_identity");
    member.analytics = analytics;
    return member;
}

GroupFlowMonth month(const DerivedTimeFrame& tf, const QVector<GroupFlowMember>& members) {
    return aggregate_group_regulatory_month(QStringLiteral("complex"), QStringLiteral("sp500"),
                                            QStringLiteral("etf-taxonomy-v1"), QDate(2026, 6, 1), tf, members);
}
} // namespace

class EtfGroupCalcTest : public QObject {
    Q_OBJECT
  private slots:
    void monthly_sum_is_not_a_daily_or_rotation_value();
    void partial_flow_and_regulatory_assets_coverage_are_distinct();
    void unavailable_denominator_never_becomes_full_coverage();
    void duplicate_sec_reporting_identity_is_counted_once();
    void amendment_obeys_as_of_and_known_at_and_replays();
    void excluded_and_unlinked_subjects_remain_visible();
    void unresolved_members_never_disappear_from_coverage();
    void mixed_denominator_dates_and_overflow_fail_closed();
    void zero_flow_is_measured_and_order_is_deterministic();
};

void EtfGroupCalcTest::monthly_sum_is_not_a_daily_or_rotation_value() {
    const DerivedTimeFrame tf{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    SecFixture a, b;
    a.flow("a", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 100, 30, 5);
    b.flow("b", "2026-08-02T12:00:00.000Z", "2026-08-04T14:30:00.000Z", "2026-08-04T15:00:00.000Z", 50, 10, 0);
    const auto result = month(tf, {sec(QStringLiteral("0000000001/"), a.calculate(tf)),
                                   sec(QStringLiteral("0000000002/S1"), b.calculate(tf))});
    QVERIFY(result.observed_net_flow_usd);
    QCOMPARE(*result.observed_net_flow_usd, 115.0);
    QVERIFY(result.complete_net_flow_usd);
    QCOMPARE(result.available_from, utc("2026-08-04T14:30:00.000Z"));
    QCOMPARE(result.quality, QStringLiteral("CONFIRMED"));
    const auto json = group_flow_month_json(result);
    QCOMPARE(json.value(QStringLiteral("frequency")).toString(), QStringLiteral("calendar_month"));
    QCOMPARE(json.value(QStringLiteral("measurement_kind")).toString(), QStringLiteral("regulatory_reported_flow"));
    QCOMPARE(json.value(QStringLiteral("period_end")).toString(), QStringLiteral("2026-06-30"));
    const auto rows = json.value(QStringLiteral("constituents")).toArray();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].toObject().value(QStringLiteral("selected_accession")).toString(), QStringLiteral("a"));
    QVERIFY(
        rows[0].toObject().value(QStringLiteral("sales")).toObject().value(QStringLiteral("observation_id")).toInt() >
        0);
}

void EtfGroupCalcTest::partial_flow_and_regulatory_assets_coverage_are_distinct() {
    const DerivedTimeFrame tf{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    SecFixture a, b;
    a.flow("a", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 100, 30, 5);
    a.value("na-a", "2026-05-01T12:00:00.000Z", "2026-05-04T14:30:00.000Z", "2026-05-04T15:00:00.000Z", kNportNetAssets,
            QDate(2026, 3, 31), 1000);
    b.value("na-b", "2026-05-01T12:00:00.000Z", "2026-05-04T14:30:00.000Z", "2026-05-04T15:00:00.000Z", kNportNetAssets,
            QDate(2026, 3, 31), 3000);
    const auto result =
        month(tf, {sec(QStringLiteral("a/"), a.calculate(tf)), sec(QStringLiteral("b/"), b.calculate(tf))});
    QCOMPARE(result.quality, QStringLiteral("PARTIAL"));
    QVERIFY(result.observed_net_flow_usd);
    QCOMPARE(*result.observed_net_flow_usd, 75.0);
    QVERIFY(!result.complete_net_flow_usd);
    QVERIFY(result.regulatory_assets_coverage);
    QCOMPARE(*result.regulatory_assets_coverage, 0.25);
    QCOMPARE(result.observed_reporting_identities, 1);
    QCOMPARE(result.unique_reporting_identities, 2);
    QCOMPARE(result.members[1].reason, QStringLiteral("month_not_available"));
}

void EtfGroupCalcTest::unavailable_denominator_never_becomes_full_coverage() {
    const DerivedTimeFrame tf{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    SecFixture a, b;
    a.flow("a", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 10, 0, 0);
    b.flow("b", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 20, 0, 0);
    a.value("na-a", "2026-05-01T12:00:00.000Z", "2026-05-04T14:30:00.000Z", "2026-05-04T15:00:00.000Z", kNportNetAssets,
            QDate(2026, 3, 31), 1000);
    const auto result =
        month(tf, {sec(QStringLiteral("a/"), a.calculate(tf)), sec(QStringLiteral("b/"), b.calculate(tf))});
    QVERIFY(result.complete_net_flow_usd);
    QCOMPARE(*result.complete_net_flow_usd, 30.0);
    QVERIFY(!result.regulatory_assets_coverage);
    QCOMPARE(result.coverage_reason, QStringLiteral("regulatory_assets_unavailable"));
}

void EtfGroupCalcTest::duplicate_sec_reporting_identity_is_counted_once() {
    const DerivedTimeFrame tf{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    SecFixture a;
    a.flow("a", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 100, 0, 0);
    const auto computed = a.calculate(tf);
    GroupFlowMember linked = sec(QStringLiteral("0000000001/"), computed);
    linked.subject_type = QStringLiteral("listed_instrument");
    linked.stable_key = QStringLiteral("ibkr:123");
    linked.identity_basis = QStringLiteral("declared_link");
    const auto result = month(tf, {linked, sec(QStringLiteral("0000000001/"), computed)});
    QCOMPARE(*result.observed_net_flow_usd, 100.0);
    QCOMPARE(result.unique_reporting_identities, 1);
    QCOMPARE(result.members[1].status, QStringLiteral("duplicate_reporting_identity"));
}

void EtfGroupCalcTest::amendment_obeys_as_of_and_known_at_and_replays() {
    SecFixture a;
    a.flow("original", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 100, 0, 0);
    const DerivedTimeFrame prior{utc("2026-08-10T00:00:00.000Z"), utc("2026-08-10T00:00:00.000Z")};
    const QByteArray earlier =
        QJsonDocument(group_flow_month_json(month(prior, {sec(QStringLiteral("a/"), a.calculate(prior))})))
            .toJson(QJsonDocument::Compact);
    a.flow("amendment", "2026-08-20T12:00:00.000Z", "2026-08-21T14:30:00.000Z", "2026-08-21T15:00:00.000Z", 120, 0, 0,
           true, "original");
    QCOMPARE(QJsonDocument(group_flow_month_json(month(prior, {sec(QStringLiteral("a/"), a.calculate(prior))})))
                 .toJson(QJsonDocument::Compact),
             earlier);
    const DerivedTimeFrame current{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    const auto revised = month(current, {sec(QStringLiteral("a/"), a.calculate(current))});
    QCOMPARE(*revised.observed_net_flow_usd, 120.0);
    QCOMPARE(revised.quality, QStringLiteral("REVISED"));
    QCOMPARE(revised.members[0].month->inputs.selected_accession, QStringLiteral("amendment"));
    QCOMPARE(revised.members[0].month->inputs.amendment.amends_accession, QStringLiteral("original"));
}

void EtfGroupCalcTest::excluded_and_unlinked_subjects_remain_visible() {
    const DerivedTimeFrame tf{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    SecFixture a;
    a.flow("a", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 50, 0, 0);
    GroupFlowMember unlinked{
        QStringLiteral("listed_instrument"), QStringLiteral("ibkr:4"), {}, QStringLiteral("none"), {}, {}};
    GroupFlowMember leveraged{QStringLiteral("listed_instrument"),
                              QStringLiteral("ibkr:5"),
                              {},
                              QStringLiteral("none"),
                              QStringLiteral("leveraged_inverse_excluded_default"),
                              {}};
    const auto result = month(tf, {sec(QStringLiteral("a/"), a.calculate(tf)), unlinked, leveraged});
    QCOMPARE(*result.observed_net_flow_usd, 50.0);
    QVERIFY(!result.complete_net_flow_usd);
    QVERIFY(!result.regulatory_assets_coverage);
    QCOMPARE(result.coverage_reason, QStringLiteral("unresolved_constituent"));
    QCOMPARE(result.unresolved_subjects, 1);
    QCOMPARE(result.excluded_subjects, 1);
}

void EtfGroupCalcTest::unresolved_members_never_disappear_from_coverage() {
    const DerivedTimeFrame tf{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    SecFixture a;
    a.flow("a", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 50, 0, 0);
    a.value("na-a", "2026-05-01T12:00:00.000Z", "2026-05-04T14:30:00.000Z", "2026-05-04T15:00:00.000Z", kNportNetAssets,
            QDate(2026, 3, 31), 1000);
    for (const auto* reason : {"instrument_not_recorded_by_known_at", "classification_history_unverified",
                               "ambiguous_identity_links", "multi_class_series_not_etf_flow"}) {
        auto missing = sec(QStringLiteral("b/"), {});
        missing.exclusion_reason = QLatin1String(reason);
        const auto result = month(tf, {sec(QStringLiteral("a/"), a.calculate(tf)), missing});
        QCOMPARE(result.quality, QStringLiteral("PARTIAL"));
        QVERIFY(!result.complete_net_flow_usd);
        QVERIFY(!result.regulatory_assets_coverage);
        QCOMPARE(result.unresolved_subjects, 1);
        QCOMPARE(result.excluded_subjects, 0);
    }
}

void EtfGroupCalcTest::mixed_denominator_dates_and_overflow_fail_closed() {
    const DerivedTimeFrame tf{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    SecFixture a, b;
    for (auto* f : {&a, &b})
        f->flow("a", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 1e308, 0, 0);
    a.value("na-a", "2026-05-01T12:00:00.000Z", "2026-05-04T14:30:00.000Z", "2026-05-04T15:00:00.000Z", kNportNetAssets,
            QDate(2026, 3, 31), 1000);
    b.value("na-b", "2026-05-01T12:00:00.000Z", "2026-05-04T14:30:00.000Z", "2026-05-04T15:00:00.000Z", kNportNetAssets,
            QDate(2026, 4, 30), 2000);
    const auto result =
        month(tf, {sec(QStringLiteral("a/"), a.calculate(tf)), sec(QStringLiteral("b/"), b.calculate(tf))});
    QVERIFY(!result.complete_net_flow_usd);
    QVERIFY(!result.regulatory_assets_coverage);
    QCOMPARE(result.coverage_reason, QStringLiteral("mixed_regulatory_report_dates"));
    QCOMPARE(result.members.last().reason, QStringLiteral("group_sum_overflow"));
    QVERIFY(std::isfinite(*result.observed_net_flow_usd));
}

void EtfGroupCalcTest::zero_flow_is_measured_and_order_is_deterministic() {
    const DerivedTimeFrame tf{utc("2026-09-01T00:00:00.000Z"), utc("2026-09-01T00:00:00.000Z")};
    SecFixture a;
    a.flow("a", "2026-08-01T12:00:00.000Z", "2026-08-03T14:30:00.000Z", "2026-08-03T15:00:00.000Z", 50, 50, 0);
    const auto known = sec(QStringLiteral("a/"), a.calculate(tf));
    const auto missing = sec(QStringLiteral("b/"), {});
    const auto partial = month(tf, {known, missing});
    QVERIFY(partial.observed_net_flow_usd);
    QCOMPARE(*partial.observed_net_flow_usd, 0.0);
    QCOMPARE(partial.quality, QStringLiteral("PARTIAL"));
    QVERIFY(!month(tf, {missing}).observed_net_flow_usd);
    QCOMPARE(QJsonDocument(group_flow_month_json(partial)).toJson(),
             QJsonDocument(group_flow_month_json(month(tf, {missing, known}))).toJson());
}

QTEST_GUILESS_MAIN(EtfGroupCalcTest)
#include "tst_etf_group_calc.moc"
