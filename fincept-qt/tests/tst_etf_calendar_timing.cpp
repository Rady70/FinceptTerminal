// tst_etf_calendar_timing.cpp — ETF Capital Flows Batch B: the session
// calendar, the timing rules, the route policy and the vintage read model.
//
// Pure, header-only rules over Qt Core (services/etf/*.h). What is pinned:
//   * the calendar reproduces NYSE's published holidays, the unscheduled
//     2025-01-09 closure and 1:00 p.m. early closes, and the exact 501-session
//     set Batch A2 observed for 62 ETFs from 2024-09-25 to 2026-09-24;
//   * session hours convert to UTC across daylight-saving changes;
//   * dates outside the calendar are never guessed;
//   * sec_next_session_after_acceptance_v1, ibkr_next_session_v1, the
//     backfill / forward split and the request end rule behave as A2 states;
//   * the route policy enables only the qualified routes;
//   * vintage selection and derived quality never overwrite history.

#include "services/etf/EtfDataModel.h"
#include "services/etf/EtfReadModel.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "services/etf/EtfTiming.h"

#include <QSet>
#include <QTest>

using namespace fincept::services::etf;

namespace {

QDateTime utc(const char* iso) {
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toUTC();
}

StoredObservation vintage(const char* source, int revision, double value, const char* accepted = nullptr,
                          const char* available = nullptr) {
    StoredObservation o;
    o.source_type = QString::fromLatin1(source);
    o.source_revision = revision;
    o.value = FieldValue::reported_value(value);
    if (accepted)
        o.accepted_at = utc(accepted);
    if (available)
        o.available_from = utc(available);
    return o;
}

} // namespace

class TstEtfCalendarTiming : public QObject {
    Q_OBJECT

  private slots:
    void exchange_zone_resolves();
    void published_holidays_and_closures();
    void observed_holiday_conventions();
    void early_closes_at_one_pm();
    void session_hours_follow_daylight_saving();
    void outside_coverage_is_never_guessed();
    void a2_ibkr_window_matches_exactly();
    void next_and_last_session();
    void weekdays_in_reports_partial_coverage();
    void sec_availability_rule();
    void sec_vintage_classification();
    void ibkr_vintage_classification();
    void ibkr_request_end_rule();
    void route_policy_enables_only_qualified_routes();
    void flow_route_availability_keeps_states_apart();
    void decimal_fields_keep_missing_unparseable_and_zero_apart();
    void vintage_order_and_selection();
    void derived_quality_is_judged_against_earlier_vintages();
};

void TstEtfCalendarTiming::exchange_zone_resolves() {
    QVERIFY(UsEquityCalendar::exchange_zone().isValid());
    QCOMPARE(UsEquityCalendar::exchange_date(utc("2026-09-25T03:00:00.000Z")), QDate(2026, 9, 24));
    QCOMPARE(UsEquityCalendar::exchange_date(utc("2026-09-25T04:30:00.000Z")), QDate(2026, 9, 25));
}

void TstEtfCalendarTiming::published_holidays_and_closures() {
    // One date per holiday kind, from the NYSE Group schedules the calendar cites.
    const QDate holidays[] = {QDate(2019, 4, 19), QDate(2020, 7, 3),  QDate(2021, 12, 24), QDate(2022, 6, 20),
                              QDate(2023, 1, 2),  QDate(2024, 3, 29), QDate(2025, 1, 9),   QDate(2025, 11, 27),
                              QDate(2026, 4, 3),  QDate(2026, 9, 7),  QDate(2027, 3, 26),  QDate(2028, 11, 23)};
    for (const QDate& d : holidays)
        QCOMPARE(UsEquityCalendar::day(d).type, SessionDayType::Holiday);
    // 2025-01-09 is the unscheduled closure (National Day of Mourning).
    QVERIFY(!UsEquityCalendar::day(QDate(2025, 1, 9)).is_session());
    QCOMPARE(UsEquityCalendar::day(QDate(2025, 1, 8)).type, SessionDayType::Regular);
    QCOMPARE(UsEquityCalendar::day(QDate(2026, 9, 26)).type, SessionDayType::Weekend);
}

void TstEtfCalendarTiming::observed_holiday_conventions() {
    // New Year's Day on a Saturday is not observed on the Friday (NYSE Rule 7.2).
    QCOMPARE(UsEquityCalendar::day(QDate(2021, 12, 31)).type, SessionDayType::Regular);
    QCOMPARE(UsEquityCalendar::day(QDate(2027, 12, 31)).type, SessionDayType::Regular);
    // Saturday holidays move to Friday, Sunday holidays to Monday.
    QCOMPARE(UsEquityCalendar::day(QDate(2026, 7, 3)).type, SessionDayType::Holiday);   // July 4 is a Saturday
    QCOMPARE(UsEquityCalendar::day(QDate(2027, 7, 5)).type, SessionDayType::Holiday);   // July 4 is a Sunday
    QCOMPARE(UsEquityCalendar::day(QDate(2027, 6, 18)).type, SessionDayType::Holiday);  // Juneteenth, Saturday
    QCOMPARE(UsEquityCalendar::day(QDate(2022, 12, 26)).type, SessionDayType::Holiday); // Christmas, Sunday
    // Juneteenth is a holiday from 2022 only.
    QCOMPARE(UsEquityCalendar::day(QDate(2021, 6, 18)).type, SessionDayType::Regular);
}

void TstEtfCalendarTiming::early_closes_at_one_pm() {
    for (const QDate& d : {QDate(2019, 7, 3), QDate(2024, 11, 29), QDate(2024, 12, 24), QDate(2025, 7, 3),
                           QDate(2025, 12, 24), QDate(2026, 11, 27), QDate(2026, 12, 24), QDate(2028, 7, 3)}) {
        const MarketSessionDay s = UsEquityCalendar::day(d);
        QVERIFY2(s.is_early_close(), qPrintable(d.toString(Qt::ISODate)));
        QCOMPARE(s.close_local, QTime(13, 0));
    }
    // No early close before a Friday Independence Day holiday (2026) or a
    // Monday observed one (2027), nor on 2022-12-23 or 2023-12-22.
    for (const QDate& d : {QDate(2026, 7, 2), QDate(2027, 7, 2), QDate(2022, 12, 23), QDate(2023, 12, 22)})
        QCOMPARE(UsEquityCalendar::day(d).type, SessionDayType::Regular);
    QCOMPARE(UsEquityCalendar::day(QDate(2025, 7, 3)).close_utc, utc("2025-07-03T17:00:00.000Z"));   // EDT
    QCOMPARE(UsEquityCalendar::day(QDate(2026, 11, 27)).close_utc, utc("2026-11-27T18:00:00.000Z")); // EST
}

void TstEtfCalendarTiming::session_hours_follow_daylight_saving() {
    QCOMPARE(UsEquityCalendar::day(QDate(2026, 3, 6)).open_utc, utc("2026-03-06T14:30:00.000Z"));    // EST
    QCOMPARE(UsEquityCalendar::day(QDate(2026, 3, 9)).open_utc, utc("2026-03-09T13:30:00.000Z"));    // EDT
    QCOMPARE(UsEquityCalendar::day(QDate(2026, 10, 30)).close_utc, utc("2026-10-30T20:00:00.000Z")); // EDT
    QCOMPARE(UsEquityCalendar::day(QDate(2026, 11, 2)).close_utc, utc("2026-11-02T21:00:00.000Z"));  // EST
}

void TstEtfCalendarTiming::outside_coverage_is_never_guessed() {
    for (const QDate& d : {QDate(2018, 12, 31), QDate(2029, 1, 2), QDate()}) {
        const MarketSessionDay s = UsEquityCalendar::day(d);
        QCOMPARE(s.type, SessionDayType::OutsideCoverage);
        QVERIFY(!s.is_session());
        QVERIFY(!s.open_utc.isValid());
    }
    QVERIFY(!UsEquityCalendar::next_session_after(QDate(2028, 12, 29)).has_value());
    QVERIFY(!UsEquityCalendar::last_session_before(QDate(2019, 1, 2)).has_value());
}

void TstEtfCalendarTiming::a2_ibkr_window_matches_exactly() {
    // A2 (ibkr_calendar.csv): 62 ETFs, 501 identical session dates from
    // 2024-09-25 to 2026-09-24, and exactly these 21 weekdays without one.
    const QSet<QDate> a2_no_session = {
        QDate(2024, 11, 28), QDate(2024, 12, 25), QDate(2025, 1, 1),   QDate(2025, 1, 9),  QDate(2025, 1, 20),
        QDate(2025, 2, 17),  QDate(2025, 4, 18),  QDate(2025, 5, 26),  QDate(2025, 6, 19), QDate(2025, 7, 4),
        QDate(2025, 9, 1),   QDate(2025, 11, 27), QDate(2025, 12, 25), QDate(2026, 1, 1),  QDate(2026, 1, 19),
        QDate(2026, 2, 16),  QDate(2026, 4, 3),   QDate(2026, 5, 25),  QDate(2026, 6, 19), QDate(2026, 7, 3),
        QDate(2026, 9, 7)};
    bool complete = false;
    const auto days = UsEquityCalendar::weekdays_in(QDate(2024, 9, 25), QDate(2026, 9, 24), &complete);
    QVERIFY(complete);
    int sessions = 0;
    QSet<QDate> no_session;
    for (const MarketSessionDay& d : days) {
        if (d.is_session())
            ++sessions;
        else
            no_session.insert(d.date);
    }
    QCOMPARE(sessions, 501);
    QCOMPARE(no_session, a2_no_session);
    // The early closes A2 found among the lowest-volume sessions are labelled.
    for (const QDate& d : {QDate(2024, 12, 24), QDate(2024, 11, 29), QDate(2025, 12, 24)})
        QVERIFY(UsEquityCalendar::day(d).is_early_close());
}

void TstEtfCalendarTiming::next_and_last_session() {
    QCOMPARE(UsEquityCalendar::next_session_after(QDate(2026, 8, 28))->date, QDate(2026, 8, 31));
    QCOMPARE(UsEquityCalendar::next_session_after(QDate(2026, 9, 4))->date, QDate(2026, 9, 8)); // Labor Day
    QCOMPARE(UsEquityCalendar::last_session_before(QDate(2026, 9, 26))->date, QDate(2026, 9, 25));
    QCOMPARE(UsEquityCalendar::last_session_before(QDate(2026, 9, 8))->date, QDate(2026, 9, 4));
    QCOMPARE(UsEquityCalendar::last_session_before(QDate(2025, 1, 10))->date, QDate(2025, 1, 8));
}

void TstEtfCalendarTiming::weekdays_in_reports_partial_coverage() {
    bool complete = true;
    const auto days = UsEquityCalendar::weekdays_in(QDate(2018, 12, 27), QDate(2019, 1, 4), &complete);
    QVERIFY(!complete);
    QVERIFY(days.isEmpty());
    const auto week = UsEquityCalendar::weekdays_in(QDate(2026, 9, 7), QDate(2026, 9, 13), &complete);
    QVERIFY(complete);
    QCOMPARE(week.size(), 5);
    QCOMPARE(week.first().type, SessionDayType::Holiday);
}

void TstEtfCalendarTiming::sec_availability_rule() {
    // A2 section 5.3's example: SPY, accepted 08:25 ET on Friday 2026-08-28.
    QCOMPARE(*sec_next_session_available_from(utc("2026-08-28T12:25:47.000Z")), utc("2026-08-31T13:30:00.000Z"));
    // Accepted in the Friday evening (still Friday in ET): the Monday open.
    QCOMPARE(*sec_next_session_available_from(utc("2026-08-29T02:00:00.000Z")), utc("2026-08-31T13:30:00.000Z"));
    // Accepted on a Saturday: the Monday open.
    QCOMPARE(*sec_next_session_available_from(utc("2026-08-29T15:00:00.000Z")), utc("2026-08-31T13:30:00.000Z"));
    // Accepted before a holiday weekend: the Tuesday open.
    QCOMPARE(*sec_next_session_available_from(utc("2026-09-04T15:00:00.000Z")), utc("2026-09-08T13:30:00.000Z"));
    // Before the calendar's coverage the rule cannot be evaluated.
    QVERIFY(!sec_next_session_available_from(utc("2018-11-26T15:00:00.000Z")).has_value());
}

void TstEtfCalendarTiming::sec_vintage_classification() {
    const QDateTime start = utc("2026-09-01T10:00:00.000Z");
    const TimingAssessment historical =
        classify_sec_vintage(utc("2026-08-28T12:25:47.000Z"), start, utc("2026-09-01T10:00:05.000Z"));
    QCOMPARE(historical.history_type, HistoryType::RegulatoryFiling);
    QCOMPARE(historical.point_in_time_status, PointInTimeStatus::ConservativeRule);
    QCOMPARE(historical.availability_basis, AvailabilityBasis::SecNextSessionAfterAcceptance);
    QCOMPARE(historical.available_from, utc("2026-08-31T13:30:00.000Z"));

    const TimingAssessment forward =
        classify_sec_vintage(utc("2026-11-25T15:00:00.000Z"), start, utc("2026-11-30T09:00:00.000Z"));
    QCOMPARE(forward.history_type, HistoryType::ForwardObserved);
    QCOMPARE(forward.point_in_time_status, PointInTimeStatus::Observed);
    QCOMPARE(forward.availability_basis, AvailabilityBasis::RecordedFirstSeen);
    QCOMPARE(forward.available_from, utc("2026-11-30T09:00:00.000Z")); // never before the first sighting

    const TimingAssessment uncovered =
        classify_sec_vintage(utc("2018-11-26T15:00:00.000Z"), start, utc("2026-09-01T10:00:05.000Z"));
    QCOMPARE(uncovered.history_type, HistoryType::RegulatoryFiling);
    QCOMPARE(uncovered.point_in_time_status, PointInTimeStatus::NotPointInTime);
    QCOMPARE(uncovered.availability_basis, AvailabilityBasis::None);
    QVERIFY(!uncovered.available_from.isValid());
}

void TstEtfCalendarTiming::ibkr_vintage_classification() {
    const QDateTime start = utc("2026-09-25T14:52:00.000Z"); // during the 2026-09-25 session
    // A bar of a session that closed before the start: backfill, an assumption.
    const TimingAssessment backfill =
        classify_ibkr_bar_vintage(QDate(2026, 9, 24), start, utc("2026-09-25T14:52:10.000Z"), true);
    QCOMPARE(backfill.history_type, HistoryType::MarketBackfill);
    QCOMPARE(backfill.point_in_time_status, PointInTimeStatus::HistoricalAssumption);
    QCOMPARE(backfill.availability_basis, AvailabilityBasis::SessionCloseAssumption);
    QCOMPARE(backfill.available_from, utc("2026-09-24T20:00:00.000Z"));
    // Its later revision is never dated before MarketLab saw it, and is never observed.
    const TimingAssessment revised =
        classify_ibkr_bar_vintage(QDate(2026, 9, 24), start, utc("2026-09-26T09:00:00.000Z"), false);
    QCOMPARE(revised.history_type, HistoryType::MarketBackfill);
    QCOMPARE(revised.point_in_time_status, PointInTimeStatus::HistoricalAssumption);
    QCOMPARE(revised.availability_basis, AvailabilityBasis::RecordedFirstSeen);
    QCOMPARE(revised.available_from, utc("2026-09-26T09:00:00.000Z"));
    // The session in progress at the start closes after it: a forward observation.
    const TimingAssessment forward =
        classify_ibkr_bar_vintage(QDate(2026, 9, 25), start, utc("2026-09-26T09:00:00.000Z"), true);
    QCOMPARE(forward.history_type, HistoryType::ForwardObserved);
    QCOMPARE(forward.point_in_time_status, PointInTimeStatus::Observed);
    QCOMPARE(forward.availability_basis, AvailabilityBasis::RecordedFirstSeenAndIbkrNextSession);
    QCOMPARE(forward.available_from, utc("2026-09-28T13:30:00.000Z")); // the next session's open
    // Seen after the next open: available from the sighting.
    const TimingAssessment late =
        classify_ibkr_bar_vintage(QDate(2026, 9, 25), start, utc("2026-09-28T15:00:00.000Z"), true);
    QCOMPARE(late.available_from, utc("2026-09-28T15:00:00.000Z"));
    // A date that is not a session has no availability at all.
    const TimingAssessment holiday =
        classify_ibkr_bar_vintage(QDate(2026, 9, 7), start, utc("2026-09-26T09:00:00.000Z"), true);
    QCOMPARE(holiday.point_in_time_status, PointInTimeStatus::NotPointInTime);
    QVERIFY(!holiday.available_from.isValid());
}

void TstEtfCalendarTiming::ibkr_request_end_rule() {
    // During the 2026-09-25 session (15:49 ET): the session still trading is not requested.
    QCOMPARE(ibkr_last_requestable_session(utc("2026-09-25T19:49:00.000Z"))->date, QDate(2026, 9, 24));
    // After the close but on the same exchange date: still the prior session.
    QCOMPARE(ibkr_last_requestable_session(utc("2026-09-25T21:00:00.000Z"))->date, QDate(2026, 9, 24));
    // After midnight ET: the session of the previous exchange date.
    QCOMPARE(ibkr_last_requestable_session(utc("2026-09-26T04:30:00.000Z"))->date, QDate(2026, 9, 25));
    // Across a holiday.
    QCOMPARE(ibkr_last_requestable_session(utc("2026-09-08T12:00:00.000Z"))->date, QDate(2026, 9, 4));
    QCOMPARE(ibkr_end_date_time_for(QDate(2026, 9, 24)), QStringLiteral("20260924 23:59:59 US/Eastern"));
}

void TstEtfCalendarTiming::route_policy_enables_only_qualified_routes() {
    QVERIFY(acquisition_route(SourceType::SecNport, AcquisitionMode::RegulatoryApi).enabled);
    QVERIFY(acquisition_route(SourceType::SecSubmissions, AcquisitionMode::RegulatoryApi).enabled);
    QVERIFY(acquisition_route(SourceType::IbkrTwsReadonly, AcquisitionMode::IbkrReadonlyWrapper).enabled);
    const RouteDecision xbrl = acquisition_route(SourceType::SecPeriodicXbrl, AcquisitionMode::RegulatoryApi);
    QVERIFY(!xbrl.enabled);
    QVERIFY(xbrl.reason.startsWith(QLatin1String("d7_")));
    const RouteDecision import = acquisition_route(SourceType::IssuerFile, AcquisitionMode::UserInitiatedImport);
    QVERIFY(!import.enabled);
    QVERIFY(import.reason.startsWith(QLatin1String("d1d_")));
    // The stored reason states what A2 established: no permission basis for
    // automated issuer collection, not that every issuer prohibits it.
    const RouteDecision automated = acquisition_route(SourceType::IssuerFile, AcquisitionMode::IssuerAutomated);
    QVERIFY(!automated.enabled);
    QVERIFY(automated.reason.startsWith(QLatin1String("issuer_automation_permission_not_established:")));
    QVERIFY(!acquisition_route(SourceType::SecNport, AcquisitionMode::IbkrReadonlyWrapper).enabled);
    QVERIFY(!acquisition_route(SourceType::IbkrTwsReadonly, AcquisitionMode::RegulatoryApi).enabled);
    QVERIFY(!acquisition_route(SourceType::IssuerFile, AcquisitionMode::RegulatoryApi).enabled);
    QVERIFY(!measurement_kind_is_source_fact(MeasurementKind::RotationProxy));
    QVERIFY(!measurement_kind_is_source_fact(MeasurementKind::CalculatedCreationRedemptionFlow));
}

void TstEtfCalendarTiming::flow_route_availability_keeps_states_apart() {
    auto find = [](const QVector<FlowRouteStatus>& all, FlowRoute r) {
        for (const auto& s : all)
            if (s.route == r)
                return s;
        return FlowRouteStatus{};
    };
    const auto none = flow_route_availability(std::nullopt);
    QCOMPARE(find(none, FlowRoute::RegulatoryMonthly).availability, FlowRouteAvailability::IdentityNotEstablished);
    const auto vanguard = flow_route_availability(LinkRelationship::ClassOfMultiClassSeries);
    QCOMPARE(find(vanguard, FlowRoute::RegulatoryMonthly).availability, FlowRouteAvailability::NotApplicable);
    QCOMPARE(*flow_route_quality_state(FlowRouteAvailability::NotApplicable), QualityState::NotApplicable);
    for (auto rel : {LinkRelationship::RegistrantIsInstrument, LinkRelationship::SoleClassOfSeries}) {
        const auto all = flow_route_availability(rel);
        QCOMPARE(find(all, FlowRoute::RegulatoryMonthly).availability, FlowRouteAvailability::Available);
        QCOMPARE(find(all, FlowRoute::RegulatoryQuarterly).availability, FlowRouteAvailability::NotQualified);
        QCOMPARE(find(all, FlowRoute::CalculatedDaily).availability, FlowRouteAvailability::RouteDisabled);
    }
    QCOMPARE(*flow_route_quality_state(FlowRouteAvailability::RouteDisabled), QualityState::RouteDisabled);
    QVERIFY(!flow_route_quality_state(FlowRouteAvailability::Available).has_value());
}

void TstEtfCalendarTiming::decimal_fields_keep_missing_unparseable_and_zero_apart() {
    QCOMPARE(parse_decimal_field(QString(), false).state, ValueState::Missing);
    const FieldValue zero = parse_decimal_field(QStringLiteral("0.00000000"), true);
    QCOMPARE(zero.state, ValueState::Reported); // a reported zero is a value
    QCOMPARE(zero.value, 0.0);
    for (const char* bad : {"", "N/A", "12,5", "1.2.3", "inf", "nan", "--1"}) {
        const FieldValue f = parse_decimal_field(QString::fromLatin1(bad), true);
        QCOMPARE(f.state, ValueState::Unparseable);
        QCOMPARE(f.raw, QString::fromLatin1(bad));
    }
    const FieldValue sci = parse_decimal_field(QStringLiteral("1.5E+3"), true);
    QCOMPARE(sci.value, 1500.0);
    const FieldValue exact = parse_decimal_field(QStringLiteral("121393713302.60000000"), true);
    QCOMPARE(exact.value, 121393713302.60);
    QCOMPARE(exact.raw, QStringLiteral("121393713302.60000000"));
    QVERIFY(parse_decimal_field(QStringLiteral("100.0"), true).same_value(parse_decimal_field("100.00", true)));
    QVERIFY(!FieldValue::missing().same_value(zero));
    QVERIFY(FieldValue::unparseable("a").same_value(FieldValue::unparseable("b")));
}

void TstEtfCalendarTiming::vintage_order_and_selection() {
    // SEC vintages follow acceptance, even when recorded out of order.
    QVector<StoredObservation> sec = {
        vintage("sec_nport", 1, 101.0, "2026-09-10T15:00:00.000Z", "2026-09-11T13:30:00.000Z"),
        vintage("sec_nport", 2, 100.0, "2026-08-28T12:25:47.000Z", "2026-08-31T13:30:00.000Z")};
    QCOMPARE(current_vintage_index(sec), 0);
    QCOMPARE(vintage_as_of_index(sec, utc("2026-09-01T00:00:00.000Z")), 1);
    QCOMPARE(vintage_as_of_index(sec, utc("2026-09-12T00:00:00.000Z")), 0);
    QCOMPARE(vintage_as_of_index(sec, utc("2026-08-30T00:00:00.000Z")), -1);
    // A vintage with no availability time is never used point in time.
    sec[1].available_from = QDateTime();
    QCOMPARE(vintage_as_of_index(sec, utc("2026-09-01T00:00:00.000Z")), -1);
    // IBKR vintages follow the recording order.
    QVector<StoredObservation> ibkr = {vintage("ibkr_tws_readonly", 1, 10.0), vintage("ibkr_tws_readonly", 2, 10.5)};
    QCOMPARE(current_vintage_index(ibkr), 1);
    QCOMPARE(current_vintage_index({}), -1);
}

void TstEtfCalendarTiming::derived_quality_is_judged_against_earlier_vintages() {
    QVector<StoredObservation> v = {vintage("ibkr_tws_readonly", 1, 10.0), vintage("ibkr_tws_readonly", 2, 10.5)};
    QCOMPARE(derived_quality(v, 0), QualityState::Confirmed); // the later revision does not rewrite it
    QCOMPARE(derived_quality(v, 1), QualityState::Revised);
    QVector<StoredObservation> same = {vintage("sec_nport", 1, 5.0, "2025-11-26T17:01:17.000Z"),
                                       vintage("sec_nport", 2, 5.0, "2026-07-13T14:48:14.000Z")};
    QCOMPARE(derived_quality(same, 1), QualityState::Confirmed); // an amendment with unchanged values
    v[1].value = FieldValue::missing();
    QCOMPARE(derived_quality(v, 1), QualityState::Missing);
    QCOMPARE(derived_quality(v, 7), QualityState::Missing);
}

QTEST_GUILESS_MAIN(TstEtfCalendarTiming)
#include "tst_etf_calendar_timing.moc"
