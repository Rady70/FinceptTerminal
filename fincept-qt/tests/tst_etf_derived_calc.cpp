// tst_etf_derived_calc.cpp — ETF Capital Flows Batch C: the derived values
// (services/etf/EtfDerivedModel.h, EtfRegulatoryFlowAnalytics.h,
// EtfRotationMeasures.h) over hand-built vintages. Header-only over Qt Core:
// no database, no network, no TWS.
//
// Expected values are written out by hand (a net flow of 100 - 30 + 5, a
// return of 132 / 110 - 1 minus 110 / 100 - 1), or from a closed form of the
// fixture (closes growing 1 % per session give 1.01^k - 1 over k sessions
// whatever the calendar in between). The fixtures carry holidays, early
// closes, missing sessions, revisions and amendments, and each test says which
// research rule it pins.

#include "services/etf/EtfDerivedModel.h"
#include "services/etf/EtfRegulatoryFlowAnalytics.h"
#include "services/etf/EtfRotationMeasures.h"
#include "services/etf/EtfSessionCalendar.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace fincept::services::etf;

namespace {

QDateTime utc(const char* iso) {
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toUTC();
}

/// A decision time, seen with the knowledge of 2026-09-27 unless another
/// cutoff is named: every fixture vintage is recorded by then, so a frame in
/// the past is a point-in-time study over today's store.
DerivedTimeFrame frame(const char* as_of, const char* known_at = "2026-09-27T00:00:00.000Z") {
    return DerivedTimeFrame{utc(as_of), utc(known_at)};
}

bool near(double a, double b) {
    return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b));
}

// ── SEC fixtures ─────────────────────────────────────────────────────────────

struct Filing {
    const char* accession;
    const char* accepted;
    const char* available;
    const char* first_seen;
    const char* pit = "conservative_rule";
    const char* revision_state = "original";
};

class SecStore {
  public:
    QVector<StoredObservation> rows;

    void flows(const Filing& f, const QDate& month_end, const FieldValue& s, const FieldValue& r, const FieldValue& d) {
        add(f, "nport_sales", month_end, s);
        add(f, "nport_redemption", month_end, r);
        add(f, "nport_reinvestment", month_end, d);
    }
    void flows(const Filing& f, const QDate& month_end, double s, double r, double d) {
        flows(f, month_end, FieldValue::reported_value(s), FieldValue::reported_value(r),
              FieldValue::reported_value(d));
    }
    void net_assets(const Filing& f, const QDate& date, const FieldValue& v) { add(f, "nport_net_assets", date, v); }
    /// One component alone (a filing that does not carry the others).
    void one(const Filing& f, const char* measure, const QDate& date, const FieldValue& v) { add(f, measure, date, v); }
    void net_assets(const Filing& f, const QDate& date, double v) {
        net_assets(f, date, FieldValue::reported_value(v));
    }

  private:
    qint64 next_id_ = 1;

    void add(const Filing& f, const char* measure, const QDate& date, const FieldValue& v) {
        StoredObservation o;
        o.observation_id = next_id_++;
        o.subject_type = QStringLiteral("reporting_entity");
        o.subject_id = 1;
        o.measure = QLatin1String(measure);
        o.measurement_kind = QLatin1String(measure) == QLatin1String("nport_net_assets")
                                 ? QStringLiteral("aum_observation")
                                 : QStringLiteral("regulatory_reported_flow");
        o.units = QStringLiteral("USD");
        o.basis = o.measurement_kind == QLatin1String("aum_observation")
                      ? QStringLiteral("regulatory_quarter_end_net_assets")
                      : QStringLiteral("nport_monthly_flow");
        o.source_type = QStringLiteral("sec_nport");
        o.acquisition_mode = QStringLiteral("regulatory_api");
        o.source_document = QLatin1String(f.accession);
        o.effective_date = date;
        o.accepted_at = utc(f.accepted);
        o.value = v;
        int revision = 1;
        for (const StoredObservation& e : rows) {
            if (e.measure == o.measure && e.effective_date == date)
                ++revision;
        }
        o.source_revision = revision;
        o.revision_state = QLatin1String(f.revision_state);
        o.point_in_time_status = QLatin1String(f.pit);
        o.available_from = utc(f.available);
        o.first_seen_at = utc(f.first_seen);
        o.last_seen_at = o.first_seen_at;
        rows.append(o);
    }
};

/// The month's result. An absent month returns an empty sentinel (every value
/// unusable) so that the checks fail instead of the suite crashing.
const RegulatoryMonthResult* month_of(const RegulatoryFlowAnalytics& a, int year, int month) {
    for (const RegulatoryMonthResult& r : a.months) {
        if (r.inputs.month == QDate(year, month, 1))
            return &r;
    }
    qWarning("month %d-%02d is not in the result", year, month);
    static const RegulatoryMonthResult kAbsent;
    return &kAbsent;
}

// Two filing quarters of a series whose fiscal quarters are calendar ones:
// Q1 2026 (accepted 2026-05-28, usable 2026-05-29) and Q2 2026 (accepted
// 2026-08-25, usable 2026-08-26), and the quarter-end net assets of 2025-12-31
// from the filing before them.
const Filing kQ4{"0000000001-26-000001", "2026-02-25T15:00:00.000Z", "2026-02-26T14:30:00.000Z",
                 "2026-09-26T00:00:00.000Z"};
const Filing kQ1{"0000000001-26-000002", "2026-05-28T15:00:00.000Z", "2026-05-29T13:30:00.000Z",
                 "2026-09-26T00:00:00.000Z"};
const Filing kQ2{"0000000001-26-000003", "2026-08-25T15:00:00.000Z", "2026-08-26T13:30:00.000Z",
                 "2026-09-26T00:00:00.000Z"};

SecStore two_quarters() {
    SecStore s;
    s.net_assets(kQ4, QDate(2025, 12, 31), 1000.0);
    s.flows(kQ1, QDate(2026, 1, 31), 100.0, 30.0, 5.0); // +75
    s.flows(kQ1, QDate(2026, 2, 28), 10.0, 50.0, 0.0);  // -40
    s.flows(kQ1, QDate(2026, 3, 31), 20.0, 20.0, 0.0);  // 0
    s.net_assets(kQ1, QDate(2026, 3, 31), 1100.0);
    s.flows(kQ2, QDate(2026, 4, 30), 60.0, 5.0, 0.0); // +55
    s.flows(kQ2, QDate(2026, 5, 31), 40.0, 0.0, 0.0); // +40
    s.flows(kQ2, QDate(2026, 6, 30), 0.0, 44.0, 0.0); // -44
    s.net_assets(kQ2, QDate(2026, 6, 30), 1200.0);
    return s;
}

// ── IBKR fixtures ────────────────────────────────────────────────────────────

QVector<QDate> sessions_between(const QDate& a, const QDate& b) {
    QVector<QDate> out;
    for (const MarketSessionDay& d : UsEquityCalendar::weekdays_in(a, b)) {
        if (d.is_session())
            out.append(d.date);
    }
    return out;
}

class BarStore {
  public:
    QVector<StoredObservation> rows;

    /// A bar vintage available from its session close (backfill, recorded on
    /// 2026-01-02) unless `available` is given.
    void bar(const QDate& date, const char* measure, double value, const char* available = nullptr,
             const char* first_seen = "2026-01-02T00:00:00.000Z", const char* units = "USD_per_share",
             const char* pit = "historical_assumption") {
        StoredObservation o;
        o.observation_id = next_id_++;
        o.subject_type = QStringLiteral("listed_instrument");
        o.subject_id = 1;
        o.measure = QLatin1String(measure);
        o.measurement_kind = QStringLiteral("market_bar");
        o.units = QLatin1String(measure) == QLatin1String("bar_volume") ? QStringLiteral("shares_ibkr_filtered")
                                                                        : QLatin1String(units);
        o.basis = QStringLiteral("ibkr_trades_rth_daily_split_adjusted");
        o.source_type = QStringLiteral("ibkr_tws_readonly");
        o.acquisition_mode = QStringLiteral("ibkr_readonly_wrapper");
        o.effective_date = date;
        o.value = FieldValue::reported_value(value);
        int revision = 1;
        for (const StoredObservation& e : rows) {
            if (e.measure == o.measure && e.effective_date == date)
                ++revision;
        }
        o.source_revision = revision;
        o.revision_state = revision == 1 ? QStringLiteral("original") : QStringLiteral("revised");
        o.point_in_time_status = QLatin1String(pit);
        o.available_from = available ? utc(available) : UsEquityCalendar::day(date).close_utc;
        o.first_seen_at = utc(first_seen);
        o.last_seen_at = o.first_seen_at;
        rows.append(o);
    }

    void remove_session(const QDate& date) {
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const StoredObservation& o) { return o.effective_date == date; }),
                   rows.end());
    }

  private:
    qint64 next_id_ = 1;
};

/// Closes growing by `growth` per session (100 at the first session), volume
/// 1000 on a full session and 400 on an early close.
BarStore growing_series(const QVector<QDate>& sessions, double growth = 1.01) {
    BarStore b;
    for (int i = 0; i < sessions.size(); ++i) {
        b.bar(sessions[i], "bar_close", 100.0 * std::pow(growth, i));
        b.bar(sessions[i], "bar_volume", UsEquityCalendar::day(sessions[i]).is_early_close() ? 400.0 : 1000.0);
    }
    return b;
}

const RotationSessionResult* session_of(const RotationMeasures& m, const QDate& d) {
    for (const RotationSessionResult& r : m.sessions) {
        if (r.session == d)
            return &r;
    }
    return nullptr; // callers that expect a session check the pointer first
}

int index_of(const QVector<QDate>& sessions, const QDate& d) {
    return static_cast<int>(sessions.indexOf(d));
}

} // namespace

class TstEtfDerivedCalc : public QObject {
    Q_OBJECT

  private slots:
    // Method identity
    void method_versions_pin_their_parameters();
    // Regulatory flow analytics
    void net_flow_is_sales_minus_redemptions_plus_reinvestment();
    void missing_or_unparseable_component_is_missing_never_zero();
    void components_are_never_mixed_across_filings();
    void amendment_supersedes_only_from_its_availability();
    void amendment_repeating_the_original_is_not_revised();
    void known_at_hides_vintages_recorded_later();
    void denominator_is_the_latest_prior_report_date_with_its_lag();
    void denominator_absent_missing_zero_or_too_old();
    void rolling_sums_and_a_missing_month();
    void quarter_normalization_and_acceleration();
    void percentile_is_mid_rank_over_prior_months_only();
    void sign_balance_counts_zero_as_neither();
    void availability_is_the_latest_input_and_status_the_weakest();
    void nothing_is_available_before_the_first_filing();
    void other_sources_are_ignored();
    // Rotation proxy measures
    void price_returns_count_sessions_not_calendar_days();
    void a_holiday_is_not_a_missing_session();
    void missing_session_is_never_filled_or_skipped();
    void trend_efficiency_values_and_flat_path();
    void return_acceleration_from_three_closes();
    void volume_windows_use_full_sessions_only();
    void zero_and_extreme_volume();
    void extreme_and_invalid_prices();
    void launch_and_short_history();
    void bar_revision_follows_as_of_and_known_at();
    void freshness_and_the_in_progress_session();
    void relative_return_needs_the_declared_reference();
    // Both families
    void output_is_deterministic_and_labelled();
    void value_state_invariants_hold();
};

// ── Method identity ──────────────────────────────────────────────────────────

void TstEtfDerivedCalc::method_versions_pin_their_parameters() {
    // A parameter change without a new version id fails here: the version
    // names the method, and the method is these exact parameters.
    QCOMPARE(QString::fromLatin1(kRegulatoryFlowAnalyticsVersion), QStringLiteral("regulatory_flow_analytics_v1"));
    QCOMPARE(regulatory_flow_parameters_text(),
             QStringLiteral("net_flow=sales-redemption+reinvestment(one_filing);"
                            "denominator=latest_regulatory_net_assets_before_window,max_lag_months=2;"
                            "windows=3,12;acceleration=3;percentile=36m,min_prior=12,mid_rank;sign_balance=12"));
    QCOMPARE(QString::fromLatin1(kRotationProxyMeasuresVersion), QStringLiteral("rotation_proxy_measures_v1"));
    QCOMPARE(
        rotation_parameters_text(),
        QStringLiteral("inputs=bar_close,bar_volume;return_basis=price_return_ibkr_trades_rth_close_split_adjusted;"
                       "horizons=5,21,63,126,252;trend_efficiency=21,63;acceleration=21;"
                       "volume=5/63,21/126,full_sessions_only,early_close_not_applicable;"
                       "relative=caller_declared_reference,same_units_and_basis;freshness=ibkr_next_session_v1"));
}

// ── Regulatory flow analytics ────────────────────────────────────────────────

void TstEtfDerivedCalc::net_flow_is_sales_minus_redemptions_plus_reinvestment() {
    const SecStore s = two_quarters();
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    QCOMPARE(a.months.size(), 6);
    const auto* jan = month_of(a, 2026, 1);
    QVERIFY(jan && jan->net_flow.usable());
    QCOMPARE(*jan->net_flow.value, 75.0); // 100 - 30 + 5
    QCOMPARE(jan->net_flow.state, QualityState::Confirmed);
    QCOMPARE(jan->net_flow.input_count, 3);
    QCOMPARE(jan->net_flow.window_first, QDate(2026, 1, 31));
    QCOMPARE(*month_of(a, 2026, 2)->net_flow.value, -40.0); // an outflow is negative
    // A reported zero net flow is a number, not a missing value.
    QVERIFY(month_of(a, 2026, 3)->net_flow.usable());
    QCOMPARE(*month_of(a, 2026, 3)->net_flow.value, 0.0);
    QCOMPARE(*month_of(a, 2026, 6)->net_flow.value, -44.0);
    QCOMPARE(jan->inputs.selected_accession, QString::fromLatin1(kQ1.accession));
}

void TstEtfDerivedCalc::missing_or_unparseable_component_is_missing_never_zero() {
    SecStore s;
    s.net_assets(kQ4, QDate(2025, 12, 31), 1000.0);
    s.flows(kQ1, QDate(2026, 1, 31), FieldValue::reported_value(100.0), FieldValue::missing(),
            FieldValue::reported_value(0.0));
    s.flows(kQ1, QDate(2026, 2, 28), FieldValue::reported_value(10.0), FieldValue::unparseable(QStringLiteral("N/A")),
            FieldValue::reported_value(0.0));
    s.flows(kQ1, QDate(2026, 3, 31), 20.0, 5.0, 0.0);
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    for (int m : {1, 2}) {
        const auto* r = month_of(a, 2026, m);
        QVERIFY(!r->net_flow.usable());
        QCOMPARE(r->net_flow.state, QualityState::Missing);
        QCOMPARE(r->net_flow.reason, DerivedReason::ComponentMissing);
        QCOMPARE(r->flow_pct_prior_net_assets.reason, DerivedReason::ComponentMissing);
        QCOMPARE(r->flow_pct_percentile_36m.reason, DerivedReason::ComponentMissing);
    }
    const auto* mar = month_of(a, 2026, 3);
    QCOMPARE(*mar->net_flow.value, 15.0);
    // A window over a missing month is incomplete, not a partial sum.
    QVERIFY(!mar->net_flow_3m.usable());
    QCOMPARE(mar->net_flow_3m.reason, DerivedReason::WindowIncomplete);
}

void TstEtfDerivedCalc::components_are_never_mixed_across_filings() {
    SecStore s;
    s.flows(kQ1, QDate(2026, 3, 31), 20.0, 5.0, 0.0);
    // The amendment reports the month without a redemption value. The
    // original's redemption is NOT borrowed to complete it.
    const Filing amendment{"0000000001-26-000009",     "2026-07-01T15:00:00.000Z", "2026-07-02T13:30:00.000Z",
                           "2026-09-26T00:00:00.000Z", "conservative_rule",        "amended_filing"};
    s.flows(amendment, QDate(2026, 3, 31), FieldValue::reported_value(21.0), FieldValue::missing(),
            FieldValue::reported_value(0.0));
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    const auto* mar = month_of(a, 2026, 3);
    QVERIFY(mar);
    QCOMPARE(mar->inputs.selected_accession, QString::fromLatin1(amendment.accession));
    QCOMPARE(mar->net_flow.reason, DerivedReason::ComponentMissing);
    QCOMPARE(mar->inputs.vintages.size(), 2);
    QCOMPARE(*mar->inputs.vintages[0].net_flow, 15.0);
    QVERIFY(!mar->inputs.vintages[1].net_flow.has_value());
    QVERIFY(!mar->inputs.revision_delta.has_value());

    // A filing that carries only February's sales: its redemptions and
    // reinvestment are absent, and still not taken from the original.
    SecStore t;
    t.flows(kQ1, QDate(2026, 2, 28), 10.0, 50.0, 0.0);
    t.one(amendment, "nport_sales", QDate(2026, 2, 28), FieldValue::reported_value(12.0));
    const auto b = compute_regulatory_flow_analytics(t.rows, frame("2026-09-27T00:00:00.000Z"));
    const auto* feb = month_of(b, 2026, 2);
    QCOMPARE(feb->inputs.selected_accession, QString::fromLatin1(amendment.accession));
    QVERIFY(feb->inputs.sales.present);
    QVERIFY(!feb->inputs.redemption.present);
    QVERIFY(!feb->inputs.reinvestment.present);
    QCOMPARE(feb->net_flow.reason, DerivedReason::ComponentMissing);
}

void TstEtfDerivedCalc::amendment_supersedes_only_from_its_availability() {
    SecStore s = two_quarters();
    const Filing amendment{"0000000001-26-000010",     "2026-09-01T15:00:00.000Z", "2026-09-02T13:30:00.000Z",
                           "2026-09-26T00:00:00.000Z", "conservative_rule",        "amended_filing"};
    s.flows(amendment, QDate(2026, 1, 31), 110.0, 30.0, 5.0); // +85 instead of +75
    s.flows(amendment, QDate(2026, 2, 28), 10.0, 50.0, 0.0);
    s.flows(amendment, QDate(2026, 3, 31), 20.0, 20.0, 0.0);
    s.net_assets(amendment, QDate(2026, 3, 31), 1100.0);

    // Before the amendment is usable: the original, confirmed.
    const auto before = compute_regulatory_flow_analytics(s.rows, frame("2026-09-01T00:00:00.000Z"));
    const auto* jan_before = month_of(before, 2026, 1);
    QCOMPARE(jan_before->inputs.selected_accession, QString::fromLatin1(kQ1.accession));
    QCOMPARE(*jan_before->net_flow.value, 75.0);
    QCOMPARE(jan_before->net_flow.state, QualityState::Confirmed);
    QCOMPARE(jan_before->net_flow.available_from, utc(kQ1.available));
    QCOMPARE(jan_before->inputs.vintages.size(), 1);

    // After: the amendment, REVISED, available only from the amendment's time.
    const auto after = compute_regulatory_flow_analytics(s.rows, frame("2026-09-03T00:00:00.000Z"));
    const auto* jan = month_of(after, 2026, 1);
    QCOMPARE(jan->inputs.selected_accession, QString::fromLatin1(amendment.accession));
    QCOMPARE(*jan->net_flow.value, 85.0);
    QCOMPARE(jan->net_flow.state, QualityState::Revised);
    QCOMPARE(jan->net_flow.available_from, utc(amendment.available));
    QCOMPARE(jan->inputs.vintages.size(), 2);
    QCOMPARE(*jan->inputs.revision_delta, 10.0);
    // A window that contains the amended month inherits both the revision and
    // the amendment's availability, even when it ends in a later quarter.
    const auto* apr = month_of(after, 2026, 4);
    QVERIFY(apr->net_flow_3m.usable());
    QCOMPARE(apr->net_flow_3m.state, QualityState::Confirmed); // Feb-Apr: unchanged values
    const auto* jun = month_of(after, 2026, 6);
    QVERIFY(!jun->net_flow_12m.usable()); // only six months of history
    QCOMPARE(jun->net_flow_12m.reason, DerivedReason::InsufficientHistory);
    const auto* mar = month_of(after, 2026, 3);
    QCOMPARE(mar->net_flow_3m.state, QualityState::Revised); // Jan-Mar contains the revised January
    QCOMPARE(*mar->net_flow_3m.value, 85.0 - 40.0 + 0.0);
    QCOMPARE(mar->net_flow_3m.available_from, utc(amendment.available));
}

void TstEtfDerivedCalc::amendment_repeating_the_original_is_not_revised() {
    SecStore s = two_quarters();
    const Filing amendment{"0000000001-26-000011",     "2026-09-01T15:00:00.000Z", "2026-09-02T13:30:00.000Z",
                           "2026-09-26T00:00:00.000Z", "conservative_rule",        "amended_filing"};
    s.flows(amendment, QDate(2026, 1, 31), 100.0, 30.0, 5.0);
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    const auto* jan = month_of(a, 2026, 1);
    QCOMPARE(jan->inputs.selected_accession, QString::fromLatin1(amendment.accession));
    QCOMPARE(jan->net_flow.state, QualityState::Confirmed);
    QCOMPARE(*jan->inputs.revision_delta, 0.0);
    QCOMPARE(jan->inputs.sales.revision_state, QStringLiteral("amended_filing"));
}

void TstEtfDerivedCalc::known_at_hides_vintages_recorded_later() {
    SecStore s = two_quarters();
    // An amendment usable under the conservative rule from 2026-09-02 but
    // recorded by MarketLab only on 2026-10-05 (a later backfill).
    const Filing amendment{"0000000001-26-000012",     "2026-09-01T15:00:00.000Z", "2026-09-02T13:30:00.000Z",
                           "2026-10-05T00:00:00.000Z", "conservative_rule",        "amended_filing"};
    s.flows(amendment, QDate(2026, 1, 31), 110.0, 30.0, 5.0);
    const auto earlier =
        compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z", "2026-09-27T00:00:00.000Z"));
    QCOMPARE(*month_of(earlier, 2026, 1)->net_flow.value, 75.0);
    const auto later =
        compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z", "2026-10-06T00:00:00.000Z"));
    QCOMPARE(*month_of(later, 2026, 1)->net_flow.value, 85.0);
    // The earlier result is reproduced exactly from the grown store.
    const QByteArray a = QJsonDocument(regulatory_flow_analytics_json(earlier)).toJson(QJsonDocument::Compact);
    const QByteArray b = QJsonDocument(regulatory_flow_analytics_json(compute_regulatory_flow_analytics(
                                           s.rows, frame("2026-09-27T00:00:00.000Z", "2026-09-27T00:00:00.000Z"))))
                             .toJson(QJsonDocument::Compact);
    QCOMPARE(a, b);
}

void TstEtfDerivedCalc::denominator_is_the_latest_prior_report_date_with_its_lag() {
    const SecStore s = two_quarters();
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    // April, May and June all divide by the 2026-03-31 net assets: the June
    // 30 value is the same filing's end of period, after the flows it holds.
    const auto* apr = month_of(a, 2026, 4);
    QCOMPARE(apr->denominator_1m.report_date, QDate(2026, 3, 31));
    QCOMPARE(apr->denominator_1m.lag_months, 0);
    QVERIFY(near(*apr->flow_pct_prior_net_assets.value, 55.0 / 1100.0));
    const auto* may = month_of(a, 2026, 5);
    QCOMPARE(may->denominator_1m.report_date, QDate(2026, 3, 31));
    QCOMPARE(may->denominator_1m.lag_months, 1);
    QVERIFY(near(*may->flow_pct_prior_net_assets.value, 40.0 / 1100.0));
    const auto* jun = month_of(a, 2026, 6);
    QCOMPARE(jun->denominator_1m.report_date, QDate(2026, 3, 31));
    QCOMPARE(jun->denominator_1m.lag_months, 2);
    QVERIFY(near(*jun->flow_pct_prior_net_assets.value, -44.0 / 1100.0));
    // The denominator is an input: the value is usable only once both
    // filings are.
    QCOMPARE(apr->flow_pct_prior_net_assets.available_from, utc(kQ2.available));
    QCOMPARE(apr->flow_pct_prior_net_assets.input_count, 4);
    QCOMPARE(apr->flow_pct_prior_net_assets.window_first, QDate(2026, 3, 31));
}

void TstEtfDerivedCalc::denominator_absent_missing_zero_or_too_old() {
    const auto reason_for = [](const FieldValue& na, const QDate& na_date) {
        SecStore s;
        s.net_assets(kQ4, na_date, na);
        s.flows(kQ1, QDate(2026, 1, 31), 100.0, 30.0, 5.0);
        s.flows(kQ1, QDate(2026, 2, 28), 10.0, 50.0, 0.0);
        s.flows(kQ1, QDate(2026, 3, 31), 20.0, 20.0, 0.0);
        const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
        return month_of(a, 2026, 1)->flow_pct_prior_net_assets;
    };
    QCOMPARE(reason_for(FieldValue::missing(), QDate(2025, 12, 31)).reason, DerivedReason::DenominatorMissingValue);
    QCOMPARE(reason_for(FieldValue::reported_value(0.0), QDate(2025, 12, 31)).reason,
             DerivedReason::InvalidDenominator);
    QCOMPARE(reason_for(FieldValue::reported_value(-5.0), QDate(2025, 12, 31)).reason,
             DerivedReason::InvalidDenominator);
    // Net assets four months before the month are too old to stand for it.
    QCOMPARE(reason_for(FieldValue::reported_value(1000.0), QDate(2025, 9, 30)).reason,
             DerivedReason::DenominatorUnavailable);
    // A fund's first quarter has no earlier report date at all (a launch).
    SecStore launch;
    launch.flows(kQ1, QDate(2026, 1, 31), 100.0, 0.0, 0.0);
    launch.net_assets(kQ1, QDate(2026, 3, 31), 100.0);
    const auto a = compute_regulatory_flow_analytics(launch.rows, frame("2026-09-27T00:00:00.000Z"));
    const auto* jan = month_of(a, 2026, 1);
    QCOMPARE(jan->flow_pct_prior_net_assets.reason, DerivedReason::DenominatorUnavailable);
    QCOMPARE(jan->flow_pct_prior_net_assets.state, QualityState::Missing);
    QVERIFY(!jan->flow_pct_prior_net_assets.value.has_value());
}

void TstEtfDerivedCalc::rolling_sums_and_a_missing_month() {
    SecStore s;
    // Three consecutive quarterly filings (2024-10 .. 2025-06, net flows 1..9),
    // a quarter never filed (2025-07 .. 2025-09), then one more (100 a month).
    const Filing f1{"0000000002-25-000001", "2025-02-25T15:00:00.000Z", "2025-02-26T14:30:00.000Z",
                    "2026-09-26T00:00:00.000Z"};
    const Filing f2{"0000000002-25-000002", "2025-05-28T15:00:00.000Z", "2025-05-29T13:30:00.000Z",
                    "2026-09-26T00:00:00.000Z"};
    const Filing f3{"0000000002-25-000003", "2025-08-27T15:00:00.000Z", "2025-08-28T13:30:00.000Z",
                    "2026-09-26T00:00:00.000Z"};
    const Filing f5{"0000000002-26-000005", "2026-02-25T15:00:00.000Z", "2026-02-26T14:30:00.000Z",
                    "2026-09-26T00:00:00.000Z"};
    double v = 1.0;
    for (const Filing* f : {&f1, &f2, &f3}) {
        const QDate first = f == &f1 ? QDate(2024, 10, 1) : (f == &f2 ? QDate(2025, 1, 1) : QDate(2025, 4, 1));
        for (int k = 0; k < 3; ++k) {
            s.flows(*f, first.addMonths(k + 1).addDays(-1), v, 0.0, 0.0);
            v += 1.0;
        }
    }
    for (int k = 0; k < 3; ++k) // 2025-10..12
        s.flows(f5, QDate(2025, 10, 1).addMonths(k + 1).addDays(-1), 100.0, 0.0, 0.0);
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    QCOMPARE(a.months.size(), 15); // 2024-10 .. 2025-12, the gap listed
    QCOMPARE(month_of(a, 2024, 10)->net_flow_3m.reason, DerivedReason::InsufficientHistory);
    QCOMPARE(*month_of(a, 2024, 12)->net_flow_3m.value, 1.0 + 2.0 + 3.0);
    QCOMPARE(*month_of(a, 2025, 6)->net_flow_3m.value, 7.0 + 8.0 + 9.0);
    const auto* aug = month_of(a, 2025, 8);
    QVERIFY(!aug->inputs.available);
    QCOMPARE(aug->net_flow.reason, DerivedReason::MonthNotAvailable);
    QCOMPARE(aug->net_flow.state, QualityState::Missing);
    // A month that is itself missing passes its own reason to its windows.
    QCOMPARE(month_of(a, 2025, 9)->net_flow_3m.reason, DerivedReason::MonthNotAvailable);
    QCOMPARE(month_of(a, 2025, 9)->net_flow_12m.reason, DerivedReason::MonthNotAvailable);
    // A usable month whose window holds the gap: incomplete, never a partial sum.
    QCOMPARE(month_of(a, 2025, 10)->net_flow_3m.reason, DerivedReason::WindowIncomplete);
    QCOMPARE(month_of(a, 2025, 12)->net_flow_3m.value.value_or(-1), 300.0);
    QCOMPARE(month_of(a, 2025, 12)->net_flow_12m.reason, DerivedReason::WindowIncomplete);
    QCOMPARE(month_of(a, 2025, 6)->net_flow_12m.reason, DerivedReason::InsufficientHistory);
}

void TstEtfDerivedCalc::quarter_normalization_and_acceleration() {
    const SecStore s = two_quarters();
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    const auto* mar = month_of(a, 2026, 3);
    // Q1: (75 - 40 + 0) over the 2025-12-31 net assets, exactly quarter-start.
    QCOMPARE(mar->denominator_3m.report_date, QDate(2025, 12, 31));
    QCOMPARE(mar->denominator_3m.lag_months, 0);
    QVERIFY(near(*mar->flow_pct_3m.value, 35.0 / 1000.0));
    const auto* jun = month_of(a, 2026, 6);
    QCOMPARE(jun->denominator_3m.report_date, QDate(2026, 3, 31));
    QVERIFY(near(*jun->flow_pct_3m.value, (55.0 + 40.0 - 44.0) / 1100.0));
    // Acceleration: this quarter's normalized flow minus the previous one.
    QVERIFY(jun->normalized_flow_acceleration_3m.usable());
    QVERIFY(near(*jun->normalized_flow_acceleration_3m.value, 51.0 / 1100.0 - 35.0 / 1000.0));
    QCOMPARE(jun->normalized_flow_acceleration_3m.input_count, 3 * 6 + 2);
    // The previous quarter has no window before it.
    QCOMPARE(mar->normalized_flow_acceleration_3m.reason, DerivedReason::InsufficientHistory);
    // A 3-month window that is not a filing quarter uses the latest report
    // date before its first month, with its lag.
    const auto* may = month_of(a, 2026, 5); // Mar-May
    QCOMPARE(may->denominator_3m.report_date, QDate(2025, 12, 31));
    QCOMPARE(may->denominator_3m.lag_months, 2);
    QVERIFY(near(*may->flow_pct_3m.value, (0.0 + 55.0 + 40.0) / 1000.0));
}

void TstEtfDerivedCalc::percentile_is_mid_rank_over_prior_months_only() {
    // Monthly net assets of 1000 at every month end make each month's
    // normalized flow its net flow / 1000 with lag 0.
    SecStore s;
    const Filing f{"0000000003-26-000001", "2026-01-10T15:00:00.000Z", "2026-01-12T14:30:00.000Z",
                   "2026-09-26T00:00:00.000Z"};
    const QDate start(2024, 12, 1);
    s.net_assets(f, start.addDays(-1), 1000.0);
    const double flows[13] = {5, 1, 4, 2, 8, 3, 7, 6, 9, 10, 11, 12, 4};
    for (int i = 0; i < 13; ++i) {
        const QDate end = start.addMonths(i + 1).addDays(-1);
        s.flows(f, end, flows[i], 0.0, 0.0);
        s.net_assets(f, end, 1000.0);
    }
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    QCOMPARE(a.months.size(), 13);
    // Twelve prior months: the 13th value (4) is above 1, 2, 3 and ties one 4.
    const auto* last = month_of(a, 2025, 12);
    QVERIFY(last->flow_pct_percentile_36m.usable());
    QVERIFY(near(*last->flow_pct_percentile_36m.value, (3.0 + 0.5 * 1.0) / 12.0));
    // Eleven prior months are not enough.
    QCOMPARE(month_of(a, 2025, 11)->flow_pct_percentile_36m.reason, DerivedReason::InsufficientHistory);
    // The current month is never part of its own baseline: a new maximum is
    // at the top of the prior distribution, 1.0, not 12.5 / 13.
    s.flows(f, QDate(2026, 1, 31), 99.0, 0.0, 0.0);
    const auto b = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    QVERIFY(near(*month_of(b, 2026, 1)->flow_pct_percentile_36m.value, 1.0));
}

void TstEtfDerivedCalc::sign_balance_counts_zero_as_neither() {
    SecStore s;
    const Filing f{"0000000004-26-000001", "2026-01-10T15:00:00.000Z", "2026-01-12T14:30:00.000Z",
                   "2026-09-26T00:00:00.000Z"};
    const double flows[12] = {5, -1, 4, 0, 8, -3, 7, 6, -9, 10, -11, 12}; // 7 in, 4 out, 1 zero
    for (int i = 0; i < 12; ++i)
        s.flows(f, QDate(2025, 1, 1).addMonths(i + 1).addDays(-1), std::max(flows[i], 0.0), std::max(-flows[i], 0.0),
                0.0);
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    const auto* dec = month_of(a, 2025, 12);
    QVERIFY(near(*dec->net_flow_sign_balance_12m.value, (7.0 - 4.0) / 12.0));
    QCOMPARE(*dec->net_flow_12m.value, 5 - 1 + 4 + 0 + 8 - 3 + 7 + 6 - 9 + 10 - 11 + 12.0);
    QCOMPARE(month_of(a, 2025, 11)->net_flow_sign_balance_12m.reason, DerivedReason::InsufficientHistory);
}

void TstEtfDerivedCalc::availability_is_the_latest_input_and_status_the_weakest() {
    SecStore s = two_quarters();
    // Replace Q2 by a filing MarketLab recorded going forward: observed.
    SecStore forward;
    for (StoredObservation o : s.rows) {
        if (o.source_document == QLatin1String(kQ2.accession)) {
            o.point_in_time_status = QStringLiteral("observed");
            o.available_from = utc("2026-08-25T16:00:00.000Z");
        }
        forward.rows.append(o);
    }
    const auto a = compute_regulatory_flow_analytics(forward.rows, frame("2026-09-27T00:00:00.000Z"));
    const auto* jun = month_of(a, 2026, 6);
    QCOMPARE(jun->net_flow.point_in_time_status, PointInTimeStatus::Observed);
    // Q2's normalized flow divides by the Q1 net assets (conservative rule):
    // the weaker status wins, and the later availability.
    QCOMPARE(jun->flow_pct_prior_net_assets.point_in_time_status, PointInTimeStatus::ConservativeRule);
    QCOMPARE(jun->flow_pct_prior_net_assets.available_from, utc("2026-08-25T16:00:00.000Z"));
    // A window over both quarters is available only from the later one.
    QCOMPARE(month_of(a, 2026, 4)->net_flow_3m.available_from, utc("2026-08-25T16:00:00.000Z"));
    QCOMPARE(month_of(a, 2026, 4)->net_flow_3m.point_in_time_status, PointInTimeStatus::ConservativeRule);
}

void TstEtfDerivedCalc::nothing_is_available_before_the_first_filing() {
    const SecStore s = two_quarters();
    QVERIFY(compute_regulatory_flow_analytics(s.rows, frame("2026-05-29T13:00:00.000Z")).months.isEmpty());
    // Between the two filings only Q1 exists: no month of Q2 is invented.
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-06-01T00:00:00.000Z"));
    QCOMPARE(a.months.size(), 3);
    QCOMPARE(a.months.last().inputs.month, QDate(2026, 3, 1));
}

void TstEtfDerivedCalc::other_sources_are_ignored() {
    SecStore s = two_quarters();
    BarStore b = growing_series(sessions_between(QDate(2026, 1, 2), QDate(2026, 1, 30)));
    s.rows += b.rows;
    const auto a = compute_regulatory_flow_analytics(s.rows, frame("2026-09-27T00:00:00.000Z"));
    QCOMPARE(a.months.size(), 6);
    const auto r = compute_rotation_measures(two_quarters().rows, frame("2026-09-27T00:00:00.000Z"));
    QVERIFY(r.sessions.isEmpty());
    QVERIFY(!r.snapshot.has_data);
}

// ── Rotation proxy measures ──────────────────────────────────────────────────

void TstEtfDerivedCalc::price_returns_count_sessions_not_calendar_days() {
    const QVector<QDate> sessions = sessions_between(QDate(2024, 9, 3), QDate(2025, 12, 31));
    const BarStore b = growing_series(sessions);
    const auto m = compute_rotation_measures(b.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QCOMPARE(m.sessions.size(), sessions.size());
    // Around Thanksgiving 2025 (a holiday, then an early close): five sessions
    // back from 2025-12-02 is 2025-11-24, and the return is 1.01^5 - 1.
    const auto* d = session_of(m, QDate(2025, 12, 2));
    QVERIFY(d && d->price_return[0].usable());
    QVERIFY(near(*d->price_return[0].value, std::pow(1.01, 5) - 1.0));
    QCOMPARE(d->price_return[0].window_first, QDate(2025, 11, 24));
    QCOMPARE(d->price_return[0].state, QualityState::Proxy);
    for (std::size_t h = 0; h < kRotationReturnHorizons.size(); ++h) {
        const auto& last = m.sessions.last().price_return[h];
        QVERIFY(last.usable());
        QVERIFY(near(*last.value, std::pow(1.01, kRotationReturnHorizons[h]) - 1.0));
    }
    // The first k sessions have no session k back.
    QCOMPARE(m.sessions[4].price_return[0].reason, DerivedReason::InsufficientHistory);
    QVERIFY(m.sessions[5].price_return[0].usable());
    QCOMPARE(m.sessions[251].price_return[4].reason, DerivedReason::InsufficientHistory);
    QVERIFY(m.sessions[252].price_return[4].usable());
}

void TstEtfDerivedCalc::a_holiday_is_not_a_missing_session() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 11, 3), QDate(2025, 12, 5));
    const auto m = compute_rotation_measures(growing_series(sessions).rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QVERIFY(!session_of(m, QDate(2025, 11, 27))); // Thanksgiving: not a session, not listed
    const auto* early = session_of(m, QDate(2025, 11, 28));
    QVERIFY(early);
    QCOMPARE(early->session_type, SessionDayType::EarlyClose);
    QVERIFY(early->price_return[0].usable()); // an early close is a completed close
    for (const RotationSessionResult& r : m.sessions) {
        for (const DerivedValue& v : r.price_return)
            QVERIFY(v.reason != DerivedReason::SessionBarMissing);
    }
}

void TstEtfDerivedCalc::missing_session_is_never_filled_or_skipped() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 6, 2), QDate(2025, 12, 31));
    BarStore b = growing_series(sessions);
    const QDate gap(2025, 10, 15);
    b.remove_session(gap);
    const auto m = compute_rotation_measures(b.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QCOMPARE(m.sessions.size(), sessions.size()); // the gap is listed
    const auto* g = session_of(m, gap);
    QVERIFY(g && !g->price_return[0].usable());
    QCOMPARE(g->price_return[0].reason, DerivedReason::SessionBarMissing);
    QCOMPARE(g->volume_ratio[0].reason, DerivedReason::SessionBarMissing);
    // Five sessions later the gap is the start of the window: missing, not
    // replaced by the session before it.
    const int gi = index_of(sessions, gap);
    const auto* five_after = session_of(m, sessions[gi + 5]);
    QVERIFY(five_after);
    QCOMPARE(five_after->price_return[0].reason, DerivedReason::SessionBarMissing);
    // A return whose endpoints are both present is unaffected: point to point.
    const auto* spanning = session_of(m, sessions[gi + 2]);
    QVERIFY(spanning);
    QVERIFY(spanning->price_return[0].usable());
    QVERIFY(near(*spanning->price_return[0].value, std::pow(1.01, 5) - 1.0));
    // Path and volume windows need every session.
    QCOMPARE(spanning->trend_efficiency[0].reason, DerivedReason::SessionBarMissing);
    QCOMPARE(spanning->volume_ratio[0].reason, DerivedReason::SessionBarMissing);
}

void TstEtfDerivedCalc::trend_efficiency_values_and_flat_path() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 6, 2), QDate(2025, 12, 31));
    const auto rising = compute_rotation_measures(growing_series(sessions).rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!rising.sessions.isEmpty());
    QVERIFY(near(*rising.sessions.last().trend_efficiency[0].value, 1.0));
    QVERIFY(near(*rising.sessions.last().trend_efficiency[1].value, 1.0));
    const auto falling =
        compute_rotation_measures(growing_series(sessions, 1.0 / 1.01).rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!falling.sessions.isEmpty());
    QVERIFY(near(*falling.sessions.last().trend_efficiency[0].value, -1.0));
    // A zigzag 100, 101, 100, 101 ...: over 21 steps from an even session to
    // an odd one the net move is +1 and the path 21.
    BarStore zig;
    for (int i = 0; i < sessions.size(); ++i) {
        zig.bar(sessions[i], "bar_close", i % 2 == 0 ? 100.0 : 101.0);
        zig.bar(sessions[i], "bar_volume", 1000.0);
    }
    const auto z = compute_rotation_measures(zig.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!z.sessions.isEmpty());
    const auto& odd = z.sessions[43]; // from session 22 (even) to 43 (odd)
    QVERIFY(near(*odd.trend_efficiency[0].value, 1.0 / 21.0));
    QVERIFY(near(*z.sessions[42].trend_efficiency[0].value, -1.0 / 21.0));
    // A flat path has no direction to measure: not zero, but no value.
    BarStore flat;
    for (const QDate& d : sessions) {
        flat.bar(d, "bar_close", 50.0);
        flat.bar(d, "bar_volume", 1000.0);
    }
    const auto f = compute_rotation_measures(flat.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!f.sessions.isEmpty());
    QCOMPARE(f.sessions.last().trend_efficiency[0].reason, DerivedReason::InvalidDenominator);
    QCOMPARE(*f.sessions.last().price_return[0].value, 0.0); // a flat return is a genuine zero
}

void TstEtfDerivedCalc::return_acceleration_from_three_closes() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 6, 2), QDate(2025, 12, 31));
    BarStore b;
    const int n = static_cast<int>(sessions.size());
    for (int i = 0; i < n; ++i) {
        double close = 77.0; // anything positive: only three closes matter
        if (i == n - 43)
            close = 100.0;
        if (i == n - 22)
            close = 110.0;
        if (i == n - 1)
            close = 132.0;
        b.bar(sessions[i], "bar_close", close);
        b.bar(sessions[i], "bar_volume", 1000.0);
    }
    const auto m = compute_rotation_measures(b.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    // (132 / 110 - 1) - (110 / 100 - 1) = 0.2 - 0.1
    QVERIFY(near(*m.sessions.last().return_acceleration.value, 0.1));
    QCOMPARE(m.sessions.last().return_acceleration.input_count, 3);
    QCOMPARE(m.sessions[41].return_acceleration.reason, DerivedReason::InsufficientHistory);
}

void TstEtfDerivedCalc::volume_windows_use_full_sessions_only() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 3, 3), QDate(2025, 12, 31));
    BarStore b = growing_series(sessions); // 1000 per full session, 400 per early close
    // The last five full sessions trade 2000.
    int marked = 0;
    for (int i = static_cast<int>(sessions.size()) - 1; i >= 0 && marked < 5; --i) {
        if (UsEquityCalendar::day(sessions[i]).is_early_close())
            continue;
        b.remove_session(sessions[i]);
        b.bar(sessions[i], "bar_close", 100.0 * std::pow(1.01, i));
        b.bar(sessions[i], "bar_volume", 2000.0);
        ++marked;
    }
    const auto m = compute_rotation_measures(b.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    // 2025-12-24 is an early close inside the recent window, and 2025-11-28
    // inside the baseline: both are skipped, so the ratio is exactly 2.
    const auto& last = m.sessions.last();
    QCOMPARE(last.session, QDate(2025, 12, 31));
    QVERIFY(near(*last.volume_ratio[0].value, 2.0));
    QCOMPARE(last.volume_ratio[0].input_count, 5 + 63);
    // On an early close the volume measure does not apply; its price does.
    const auto* early = session_of(m, QDate(2025, 12, 24));
    QVERIFY(early);
    QCOMPARE(early->volume_ratio[0].state, QualityState::NotApplicable);
    QCOMPARE(early->volume_ratio[0].reason, DerivedReason::EarlyCloseSessionExcluded);
    QVERIFY(early->price_return[0].usable());
    // 21/126 needs 147 full sessions, more than the fixture's first months.
    QCOMPARE(m.sessions[100].volume_ratio[1].reason, DerivedReason::InsufficientHistory);
    QVERIFY(last.volume_ratio[1].usable());
}

void TstEtfDerivedCalc::zero_and_extreme_volume() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 6, 2), QDate(2025, 12, 31));
    BarStore zero;
    for (int i = 0; i < sessions.size(); ++i) {
        zero.bar(sessions[i], "bar_close", 100.0);
        zero.bar(sessions[i], "bar_volume", 0.0);
    }
    const auto z = compute_rotation_measures(zero.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!z.sessions.isEmpty());
    QCOMPARE(z.sessions.last().volume_ratio[0].reason, DerivedReason::InvalidDenominator);
    // Zero recent volume over a normal baseline is a genuine ratio of zero.
    BarStore quiet = growing_series(sessions);
    int changed = 0;
    for (int i = static_cast<int>(sessions.size()) - 1; changed < 5; --i) {
        if (UsEquityCalendar::day(sessions[i]).is_early_close())
            continue;
        quiet.remove_session(sessions[i]);
        quiet.bar(sessions[i], "bar_close", 100.0);
        quiet.bar(sessions[i], "bar_volume", changed == 0 ? 1.0e12 : 0.0);
        ++changed;
    }
    const auto q = compute_rotation_measures(quiet.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!q.sessions.isEmpty());
    // One extreme session is kept as it is: (1e12 + 0 * 4) / 5 over 1000.
    QVERIFY(near(*q.sessions.last().volume_ratio[0].value, (1.0e12 / 5.0) / 1000.0));
}

void TstEtfDerivedCalc::extreme_and_invalid_prices() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 11, 3), QDate(2025, 12, 31));
    BarStore b;
    for (int i = 0; i < sessions.size(); ++i) {
        double close = 100.0;
        if (i == sessions.size() - 1)
            close = 300.0; // a tripling in five sessions is kept as it is
        b.bar(sessions[i], "bar_close", close);
        b.bar(sessions[i], "bar_volume", 1000.0);
    }
    const auto m = compute_rotation_measures(b.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QVERIFY(near(*m.sessions.last().price_return[0].value, 2.0));
    BarStore bad = b;
    bad.remove_session(sessions[sessions.size() - 6]);
    bad.bar(sessions[sessions.size() - 6], "bar_close", 0.0);
    bad.bar(sessions[sessions.size() - 6], "bar_volume", 1000.0);
    const auto x = compute_rotation_measures(bad.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!x.sessions.isEmpty());
    QCOMPARE(x.sessions.last().price_return[0].reason, DerivedReason::InvalidInput);
}

void TstEtfDerivedCalc::launch_and_short_history() {
    // Thirty sessions of a new fund.
    const QVector<QDate> sessions = sessions_between(QDate(2025, 11, 3), QDate(2025, 12, 15));
    QVERIFY(sessions.size() >= 30);
    const auto m =
        compute_rotation_measures(growing_series(sessions.mid(0, 30)).rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    const auto& last = m.sessions.last();
    QVERIFY(last.price_return[1].usable()); // 21 sessions
    QCOMPARE(last.price_return[2].reason, DerivedReason::InsufficientHistory);
    QCOMPARE(last.volume_ratio[0].reason, DerivedReason::InsufficientHistory);
    QCOMPARE(last.return_acceleration.reason, DerivedReason::InsufficientHistory);
    QCOMPARE(m.sessions.first().price_return[0].reason, DerivedReason::InsufficientHistory);
}

void TstEtfDerivedCalc::bar_revision_follows_as_of_and_known_at() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 11, 3), QDate(2025, 12, 31));
    BarStore b = growing_series(sessions);
    const QDate last = sessions.last();
    // A revision of the last close, seen 2026-01-10 (available from then).
    b.bar(last, "bar_close", 1.0, "2026-01-10T12:00:00.000Z", "2026-01-10T12:00:00.000Z");
    const double base = 100.0 * std::pow(1.01, sessions.size() - 6);
    const auto before = compute_rotation_measures(b.rows, frame("2026-01-05T00:00:00.000Z"));
    QVERIFY(!before.sessions.isEmpty());
    QVERIFY(near(*before.sessions.last().price_return[0].value, std::pow(1.01, 5) - 1.0));
    QCOMPARE(before.sessions.last().price_return[0].state, QualityState::Proxy);
    const auto after = compute_rotation_measures(b.rows, frame("2026-01-11T00:00:00.000Z"));
    QVERIFY(!after.sessions.isEmpty());
    QVERIFY(near(*after.sessions.last().price_return[0].value, 1.0 / base - 1.0));
    QCOMPARE(after.sessions.last().price_return[0].state, QualityState::Revised);
    QCOMPARE(after.sessions.last().price_return[0].available_from, utc("2026-01-10T12:00:00.000Z"));
    // Knowledge before the revision was recorded reproduces the first result.
    const auto reproduced =
        compute_rotation_measures(b.rows, frame("2026-01-11T00:00:00.000Z", "2026-01-09T00:00:00.000Z"));
    QVERIFY(!reproduced.sessions.isEmpty());
    QVERIFY(near(*reproduced.sessions.last().price_return[0].value, std::pow(1.01, 5) - 1.0));
}

void TstEtfDerivedCalc::freshness_and_the_in_progress_session() {
    // Bars through Friday 2025-12-12, available at each session's close.
    const QVector<QDate> sessions = sessions_between(QDate(2025, 11, 3), QDate(2025, 12, 12));
    const BarStore b = growing_series(sessions);
    // Saturday: Friday's successor (Monday) has not opened, so the rule
    // expects Thursday; the data (Friday) is fresh.
    auto m = compute_rotation_measures(b.rows, frame("2025-12-13T15:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QCOMPARE(m.snapshot.expected_session, QDate(2025, 12, 11));
    QCOMPARE(m.snapshot.freshness, RotationFreshness::Fresh);
    QCOMPARE(m.snapshot.latest_session, QDate(2025, 12, 12));
    // Monday 11:00 ET, Monday's session in progress: Friday is expected and
    // present; the in-progress session is neither expected nor listed.
    m = compute_rotation_measures(b.rows, frame("2025-12-15T16:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QCOMPARE(m.snapshot.expected_session, QDate(2025, 12, 12));
    QCOMPARE(m.snapshot.freshness, RotationFreshness::Fresh);
    QCOMPARE(m.sessions.last().session, QDate(2025, 12, 12));
    // Wednesday: Tuesday is expected, Friday is the latest: stale. The values
    // keep their numbers and say STALE.
    m = compute_rotation_measures(b.rows, frame("2025-12-17T16:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QCOMPARE(m.snapshot.expected_session, QDate(2025, 12, 16));
    QCOMPARE(m.snapshot.freshness, RotationFreshness::Stale);
    QVERIFY(m.snapshot.values.price_return[0].usable());
    QCOMPARE(m.snapshot.values.price_return[0].state, QualityState::Stale);
    QCOMPARE(m.snapshot.values.price_return[0].reason, DerivedReason::LatestSessionBeforeExpected);
    QCOMPARE(m.sessions.last().price_return[0].state, QualityState::Proxy); // the series itself is not aged
    // A forward-observed bar of Friday is usable only from Monday's open.
    BarStore fwd = growing_series(sessions.mid(0, sessions.size() - 1));
    fwd.bar(sessions.last(), "bar_close", 100.0 * std::pow(1.01, sessions.size() - 1), "2025-12-15T14:30:00.000Z",
            "2025-12-12T21:10:00.000Z", "USD_per_share", "observed");
    fwd.bar(sessions.last(), "bar_volume", 1000.0, "2025-12-15T14:30:00.000Z", "2025-12-12T21:10:00.000Z",
            "USD_per_share", "observed");
    m = compute_rotation_measures(fwd.rows, frame("2025-12-13T15:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QCOMPARE(m.snapshot.latest_session, QDate(2025, 12, 11));
    QCOMPARE(m.snapshot.freshness, RotationFreshness::Fresh);
    m = compute_rotation_measures(fwd.rows, frame("2025-12-15T15:00:00.000Z"));
    QVERIFY(!m.sessions.isEmpty());
    QCOMPARE(m.snapshot.latest_session, QDate(2025, 12, 12));
    QCOMPARE(m.sessions.last().price_return[0].point_in_time_status, PointInTimeStatus::HistoricalAssumption);
    QCOMPARE(m.sessions.last().price_return[0].available_from, utc("2025-12-15T14:30:00.000Z"));
}

void TstEtfDerivedCalc::relative_return_needs_the_declared_reference() {
    const QVector<QDate> sessions = sessions_between(QDate(2025, 6, 2), QDate(2025, 12, 31));
    const BarStore subject = growing_series(sessions, 1.01);
    BarStore reference = growing_series(sessions, 1.005);
    const auto m = compute_rotation_measures(subject.rows, frame("2026-01-15T00:00:00.000Z"), &reference.rows);
    QVERIFY(!m.sessions.isEmpty());
    QVERIFY(m.sessions.last().has_reference);
    QVERIFY(near(*m.sessions.last().relative_price_return[1].value, std::pow(1.01 / 1.005, 21) - 1.0));
    QCOMPARE(m.sessions.last().relative_price_return[1].input_count, 4);
    // No reference declared: no relative measure at all (not a missing one).
    const auto none = compute_rotation_measures(subject.rows, frame("2026-01-15T00:00:00.000Z"));
    QVERIFY(!none.sessions.isEmpty());
    QVERIFY(!none.sessions.last().has_reference);
    QVERIFY(!rotation_values_json(none.sessions.last()).contains(QStringLiteral("relative_price_return_21")));
    // The reference lacks the session five back: unavailable, never re-dated.
    BarStore gappy = reference;
    gappy.remove_session(sessions[sessions.size() - 6]);
    const auto g = compute_rotation_measures(subject.rows, frame("2026-01-15T00:00:00.000Z"), &gappy.rows);
    QVERIFY(!g.sessions.isEmpty());
    QCOMPARE(g.sessions.last().relative_price_return[0].reason, DerivedReason::ReferenceUnavailable);
    QVERIFY(g.sessions.last().relative_price_return[1].usable());
    // A reference priced in another currency is not on the same basis.
    BarStore cad;
    for (int i = 0; i < sessions.size(); ++i) {
        cad.bar(sessions[i], "bar_close", 50.0, nullptr, "2026-09-26T00:00:00.000Z", "CAD_per_share");
        cad.bar(sessions[i], "bar_volume", 1000.0);
    }
    const auto c = compute_rotation_measures(subject.rows, frame("2026-01-15T00:00:00.000Z"), &cad.rows);
    QVERIFY(!c.sessions.isEmpty());
    QCOMPARE(c.sessions.last().relative_price_return[0].state, QualityState::NotApplicable);
    QCOMPARE(c.sessions.last().relative_price_return[0].reason, DerivedReason::ReturnBasisNotComparable);
    // The reference against itself.
    const auto self = compute_rotation_measures(reference.rows, frame("2026-01-15T00:00:00.000Z"), &reference.rows,
                                                /*subject_is_reference=*/true);
    QVERIFY(!self.sessions.isEmpty());
    QCOMPARE(self.sessions.last().relative_price_return[0].reason, DerivedReason::SubjectIsReference);
    // A reference not yet available at as_of is unavailable, not stale data.
    BarStore late;
    for (int i = 0; i < sessions.size(); ++i)
        late.bar(sessions[i], "bar_close", 50.0, "2026-02-01T00:00:00.000Z");
    const auto l = compute_rotation_measures(subject.rows, frame("2026-01-15T00:00:00.000Z"), &late.rows);
    QVERIFY(!l.sessions.isEmpty());
    QCOMPARE(l.sessions.last().relative_price_return[0].reason, DerivedReason::ReferenceUnavailable);
}

// ── Both families ────────────────────────────────────────────────────────────

void TstEtfDerivedCalc::output_is_deterministic_and_labelled() {
    const SecStore s = two_quarters();
    const QVector<QDate> sessions = sessions_between(QDate(2025, 6, 2), QDate(2025, 12, 31));
    const BarStore b = growing_series(sessions);
    const auto tf = frame("2026-09-27T00:00:00.000Z");
    const QByteArray reg1 =
        QJsonDocument(regulatory_flow_analytics_json(compute_regulatory_flow_analytics(s.rows, tf))).toJson();
    const QByteArray reg2 =
        QJsonDocument(regulatory_flow_analytics_json(compute_regulatory_flow_analytics(s.rows, tf))).toJson();
    QCOMPARE(reg1, reg2);
    const QByteArray rot1 = QJsonDocument(rotation_measures_json(compute_rotation_measures(b.rows, tf))).toJson();
    const QByteArray rot2 = QJsonDocument(rotation_measures_json(compute_rotation_measures(b.rows, tf))).toJson();
    QCOMPARE(rot1, rot2);

    // Every regulatory value is regulatory flow; every rotation value is a
    // rotation proxy without dollar units.
    const QJsonObject reg = QJsonDocument::fromJson(reg1).object();
    for (const QJsonValue& m : reg.value(QLatin1String("months")).toArray()) {
        const QJsonObject values = m.toObject().value(QLatin1String("values")).toObject();
        QCOMPARE(values.size(), 9);
        for (auto it = values.begin(); it != values.end(); ++it)
            QCOMPARE(it.value().toObject().value(QLatin1String("measurement_kind")).toString(),
                     QStringLiteral("regulatory_reported_flow"));
    }
    const QJsonObject rot = QJsonDocument::fromJson(rot1).object();
    QCOMPARE(rot.value(QLatin1String("total_return")).toBool(true), false);
    QCOMPARE(rot.value(QLatin1String("return_basis")).toString(),
             QStringLiteral("price_return_ibkr_trades_rth_close_split_adjusted"));
    for (const QJsonValue& sv : rot.value(QLatin1String("sessions")).toArray()) {
        const QJsonObject values = sv.toObject().value(QLatin1String("values")).toObject();
        QCOMPARE(values.size(), 10);
        for (auto it = values.begin(); it != values.end(); ++it) {
            const QJsonObject v = it.value().toObject();
            QCOMPARE(v.value(QLatin1String("measurement_kind")).toString(), QStringLiteral("rotation_proxy"));
            QVERIFY(!v.value(QLatin1String("units")).toString().contains(QLatin1String("USD")));
        }
    }
    // The output range bounds the listing only; the first listed session
    // still has its full lookback.
    const QJsonObject bounded =
        rotation_measures_json(compute_rotation_measures(b.rows, tf), QDate(2025, 12, 1), QDate(2025, 12, 31));
    const QJsonArray listed = bounded.value(QLatin1String("sessions")).toArray();
    QCOMPARE(listed.first().toObject().value(QLatin1String("session")).toString(), QStringLiteral("2025-12-01"));
    QVERIFY(listed.first()
                .toObject()
                .value(QLatin1String("values"))
                .toObject()
                .value(QLatin1String("price_return_63"))
                .toObject()
                .value(QLatin1String("value"))
                .isDouble());
}

void TstEtfDerivedCalc::value_state_invariants_hold() {
    SecStore s = two_quarters();
    s.flows(kQ2, QDate(2026, 7, 31), FieldValue::missing(), FieldValue::missing(), FieldValue::missing());
    const QVector<QDate> sessions = sessions_between(QDate(2025, 6, 2), QDate(2025, 12, 31));
    BarStore b = growing_series(sessions);
    b.remove_session(QDate(2025, 10, 15));
    BarStore ref = growing_series(sessions, 1.002);
    const auto tf = frame("2026-09-27T00:00:00.000Z");
    QVector<DerivedValue> all;
    for (const auto& r : compute_regulatory_flow_analytics(s.rows, tf).months) {
        all << r.net_flow << r.flow_pct_prior_net_assets << r.net_flow_3m << r.net_flow_12m << r.flow_pct_3m
            << r.flow_pct_12m << r.normalized_flow_acceleration_3m << r.flow_pct_percentile_36m
            << r.net_flow_sign_balance_12m;
    }
    const auto m = compute_rotation_measures(b.rows, tf, &ref.rows);
    QVERIFY(!m.sessions.isEmpty());
    for (const auto& r : m.sessions) {
        for (const auto& v : r.price_return)
            all << v;
        for (const auto& v : r.trend_efficiency)
            all << v;
        all << r.return_acceleration;
        for (const auto& v : r.volume_ratio)
            all << v;
        for (const auto& v : r.relative_price_return)
            all << v;
    }
    int usable = 0;
    int missing = 0;
    for (const DerivedValue& v : all) {
        const bool family =
            v.state == QualityState::Confirmed || v.state == QualityState::Proxy || v.state == QualityState::Revised;
        // A number exactly when the state is a usable one, and a reason
        // exactly when it is not; an unusable value is never zero.
        QCOMPARE(v.value.has_value(), family);
        QCOMPARE(v.reason == DerivedReason::None, family);
        QCOMPARE(v.state, family ? v.state : derived_reason_state(v.reason));
        if (family) {
            ++usable;
            QVERIFY(v.available_from.isValid());
            QVERIFY(v.input_count > 0);
        } else {
            ++missing;
            QVERIFY(!v.available_from.isValid());
        }
    }
    QVERIFY(usable > 100);
    QVERIFY(missing > 100);
}

QTEST_GUILESS_MAIN(TstEtfDerivedCalc)
#include "tst_etf_derived_calc.moc"
