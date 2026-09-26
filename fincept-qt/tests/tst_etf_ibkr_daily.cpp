// tst_etf_ibkr_daily.cpp — ETF Capital Flows Batch B: the IBKR daily-bar
// boundary (services/etf/EtfIbkrDaily.h).
//
// Pins what reaches storage from a wrapper `history` envelope: bar dates kept
// exactly as IBKR reported them; the in-progress session rejected; holidays,
// weekends and uncovered dates refused; missing expected sessions and stale
// history reported; entitlement, TWS/process failures and IBKR errors kept
// apart; and any response whose parameters, identity or rows cannot be
// trusted refused as a whole. Synthetic prices only.

#include "etf_test_fixtures.h"
#include "services/etf/EtfIbkrDaily.h"

#include <QTest>

using namespace fincept::services::etf;
using namespace etf_fixtures;

namespace {

const QString kEnd = QStringLiteral("20260924 23:59:59 US/Eastern");
const QDate kLast(2026, 9, 24);

QDateTime utc(const char* iso) {
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toUTC();
}

IbkrDailyAssessment assess(const QJsonObject& payload, const char* requested_at = "2026-09-26T09:00:00.000Z",
                           const QString& end = kEnd, const QDate& last = kLast) {
    return assess_ibkr_daily(parse_ibkr_daily_envelope(payload), QStringLiteral("SPY"), QStringLiteral("2 Y"), end,
                             last, utc(requested_at));
}

int count_state(const IbkrDailyAssessment& a, QualityState s) {
    int n = 0;
    for (const auto& i : a.issues)
        n += i.state == s ? 1 : 0;
    return n;
}

} // namespace

class TstEtfIbkrDaily : public QObject {
    Q_OBJECT

  private slots:
    void complete_window_is_accepted();
    void bar_values_and_dates_are_kept_exactly();
    void holiday_is_not_a_missing_session();
    void missing_expected_session_is_reported();
    void newest_bar_before_last_completed_session_is_stale();
    void in_progress_session_is_rejected();
    void early_close_session_is_completed_at_one_pm();
    void non_session_and_uncovered_bars_are_refused();
    void wrapper_stale_rule_is_kept();
    void entitlement_block_is_not_an_error();
    void tws_unavailable_is_a_source_error();
    void ibkr_error_is_a_source_error();
    void parameter_mismatch_is_refused();
    void contract_identity_is_checked();
    void malformed_or_duplicated_rows_refuse_the_response();
};

void TstEtfIbkrDaily::complete_window_is_accepted() {
    const auto a = assess(ibkr_history_envelope("SPY", 756733, bars_for_sessions(QDate(2026, 9, 14), kLast), kEnd));
    QCOMPARE(a.status, RetrievalStatus::Ok);
    QCOMPARE(a.accepted.size(), 9);
    QVERIFY(a.issues.isEmpty());
    QCOMPARE(a.window_first, QDate(2026, 9, 14));
    QCOMPARE(a.window_last, kLast);
}

void TstEtfIbkrDaily::bar_values_and_dates_are_kept_exactly() {
    BarSpec b;
    b.date = QStringLiteral("20260924");
    b.close = 612.37;
    b.volume = 81234567.0;
    const IbkrDailyEnvelope e = parse_ibkr_daily_envelope(ibkr_history_envelope("SPY", 756733, {b}, kEnd));
    QCOMPARE(e.bars.size(), 1);
    QCOMPARE(e.bars[0].date_text, QStringLiteral("20260924"));
    QCOMPARE(e.bars[0].session_date, kLast);
    QCOMPARE(e.bars[0].close.value, 612.37);
    QCOMPARE(e.bars[0].volume.value, 81234567.0);
    QCOMPARE(e.con_id, qint64(756733));
    QCOMPARE(e.primary_exchange, QStringLiteral("ARCA"));
    QCOMPARE(e.param_end_date_time, kEnd);
    QCOMPARE(e.adapter.value("commit").toString(), QStringLiteral("4a3c606e"));
}

void TstEtfIbkrDaily::holiday_is_not_a_missing_session() {
    // 2026-09-07 (Labor Day) is inside the window and has no bar: not missing.
    const auto a = assess(ibkr_history_envelope("SPY", 756733, bars_for_sessions(QDate(2026, 9, 1), kLast), kEnd));
    QCOMPARE(a.status, RetrievalStatus::Ok);
    QCOMPARE(count_state(a, QualityState::Missing), 0);
    bool holiday_in_window = false;
    for (const auto& d : a.window_days)
        holiday_in_window = holiday_in_window || (d.date == QDate(2026, 9, 7) && d.type == SessionDayType::Holiday);
    QVERIFY(holiday_in_window);
}

void TstEtfIbkrDaily::missing_expected_session_is_reported() {
    QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
    bars.removeAt(3); // 2026-09-17
    const auto a = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd));
    QCOMPARE(a.status, RetrievalStatus::Ok);
    QCOMPARE(a.accepted.size(), 8);
    QCOMPARE(count_state(a, QualityState::Missing), 1);
    QCOMPARE(a.issues[0].session_date, QDate(2026, 9, 17));
    QCOMPARE(a.issues[0].code, QStringLiteral("expected_session_without_bar"));
}

void TstEtfIbkrDaily::newest_bar_before_last_completed_session_is_stale() {
    const auto a =
        assess(ibkr_history_envelope("SPY", 756733, bars_for_sessions(QDate(2026, 9, 14), QDate(2026, 9, 21)), kEnd));
    QCOMPARE(a.status, RetrievalStatus::Stale);
    QCOMPARE(a.detail_code, QStringLiteral("newest_bar_before_last_completed_session"));
    QCOMPARE(a.accepted.size(), 6);                     // the returned history is still valid history
    QCOMPARE(count_state(a, QualityState::Missing), 3); // 22, 23, 24
}

void TstEtfIbkrDaily::in_progress_session_is_rejected() {
    // A request made at 10:51 ET on 2026-09-25 whose response carries a bar
    // for the session still trading (A2 section 7.7).
    QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 21), QDate(2026, 9, 25));
    const auto a = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd), "2026-09-25T14:51:00.000Z");
    QCOMPARE(a.status, RetrievalStatus::Ok);
    QCOMPARE(a.accepted.size(), 4);
    QCOMPARE(a.accepted.last().session_date, kLast);
    QCOMPARE(count_state(a, QualityState::InProgressSession), 1);
    // Even after the close, a bar after the requested end is not accepted.
    const auto after = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd), "2026-09-25T21:00:00.000Z");
    QCOMPARE(after.accepted.size(), 4);
    QCOMPARE(after.issues.size(), 1);
    QCOMPARE(after.issues[0].code, QStringLiteral("bar_after_requested_end"));
    // A response that holds only the in-progress bar has nothing acceptable.
    const auto only = assess(ibkr_history_envelope("SPY", 756733, {BarSpec{QStringLiteral("20260925")}}, kEnd),
                             "2026-09-25T14:51:00.000Z");
    QCOMPARE(only.status, RetrievalStatus::SourceError);
    QCOMPARE(only.detail_code, QStringLiteral("no_accepted_bars"));
    QCOMPARE(count_state(only, QualityState::InProgressSession), 1);
}

void TstEtfIbkrDaily::early_close_session_is_completed_at_one_pm() {
    const QString end = QStringLiteral("20261127 23:59:59 US/Eastern");
    const QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 11, 23), QDate(2026, 11, 27));
    // 13:30 ET on the early-close day: the session has closed at 13:00.
    const auto a =
        assess(ibkr_history_envelope("SPY", 756733, bars, end), "2026-11-27T18:30:00.000Z", end, QDate(2026, 11, 27));
    QCOMPARE(a.accepted.size(), 4); // 23, 24, 25, 27 (26 is Thanksgiving)
    QVERIFY(a.issues.isEmpty());
    bool early = false;
    for (const auto& d : a.window_days)
        early = early || (d.date == QDate(2026, 11, 27) && d.is_early_close());
    QVERIFY(early);
    // 12:30 ET the same day: still in progress.
    const auto b =
        assess(ibkr_history_envelope("SPY", 756733, bars, end), "2026-11-27T17:30:00.000Z", end, QDate(2026, 11, 27));
    QCOMPARE(count_state(b, QualityState::InProgressSession), 1);
}

void TstEtfIbkrDaily::non_session_and_uncovered_bars_are_refused() {
    QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
    bars.append(BarSpec{QStringLiteral("20260907")}); // Labor Day
    bars.append(BarSpec{QStringLiteral("20260920")}); // a Sunday
    bars.append(BarSpec{QStringLiteral("20181228")}); // before the calendar's coverage
    const auto a = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd));
    QCOMPARE(a.accepted.size(), 9);
    int non_session = 0, uncovered = 0;
    for (const auto& i : a.issues) {
        non_session += i.code == QLatin1String("bar_on_non_session_date") ? 1 : 0;
        uncovered += i.code == QLatin1String("bar_outside_calendar_coverage") ? 1 : 0;
    }
    QCOMPARE(non_session, 2);
    QCOMPARE(uncovered, 1);
}

void TstEtfIbkrDaily::wrapper_stale_rule_is_kept() {
    const auto a = assess(ibkr_history_envelope("SPY", 756733, {}, kEnd, "2 Y", "STALE", false));
    QCOMPARE(a.status, RetrievalStatus::Stale);
    QVERIFY(a.accepted.isEmpty());
}

void TstEtfIbkrDaily::entitlement_block_is_not_an_error() {
    const auto a = assess(ibkr_history_envelope("SPY", 756733, {}, kEnd, "2 Y", "NOT_ENTITLED", false));
    QCOMPARE(a.status, RetrievalStatus::NotEntitled);
    QVERIFY(a.accepted.isEmpty());
}

void TstEtfIbkrDaily::tws_unavailable_is_a_source_error() {
    const auto a = assess(
        ibkr_failure_envelope("IBKR_CONNECT_FAILED", "connect", "TWS did not accept a connection on 127.0.0.1:7496"));
    QCOMPARE(a.status, RetrievalStatus::SourceError);
    QCOMPARE(a.detail_code, QStringLiteral("IBKR_CONNECT_FAILED"));
    const auto killed = assess(ibkr_failure_envelope("IBKR_PROCESS_FAILED", "process", "exit 1"));
    QCOMPARE(killed.detail_code, QStringLiteral("IBKR_PROCESS_FAILED"));
    QCOMPARE(assess(QJsonObject{}).status, RetrievalStatus::SourceError); // no envelope at all
}

void TstEtfIbkrDaily::ibkr_error_is_a_source_error() {
    QJsonObject p = ibkr_history_envelope("TLT", 15547841, {}, kEnd, "2 Y", "ERROR", false);
    QJsonObject cls = p.value("classification").toObject();
    cls.insert("error_code", 162);
    cls.insert("error_message", "Historical Market Data Service error message");
    p.insert("classification", cls);
    const auto a =
        assess_ibkr_daily(parse_ibkr_daily_envelope(p), "TLT", "2 Y", kEnd, kLast, utc("2026-09-26T09:00:00.000Z"));
    QCOMPARE(a.status, RetrievalStatus::SourceError);
    QVERIFY(a.detail.contains(QLatin1String("162")));
}

void TstEtfIbkrDaily::parameter_mismatch_is_refused() {
    const QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
    QJsonObject p = ibkr_history_envelope("SPY", 756733, bars, kEnd);
    QJsonObject params = p.value("parameters").toObject();
    params.insert("what_to_show", "ADJUSTED_LAST");
    p.insert("parameters", params);
    QCOMPARE(assess(p).detail_code, QStringLiteral("parameters_mismatch"));
    params.insert("what_to_show", "TRADES");
    params.insert("use_rth", false);
    p.insert("parameters", params);
    QCOMPARE(assess(p).detail_code, QStringLiteral("parameters_mismatch"));
    // A different end than the one asked for.
    QCOMPARE(
        assess(ibkr_history_envelope("SPY", 756733, bars, QStringLiteral("20260925 23:59:59 US/Eastern"))).detail_code,
        QStringLiteral("parameters_mismatch"));
}

void TstEtfIbkrDaily::contract_identity_is_checked() {
    const QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
    QCOMPARE(assess(ibkr_history_envelope("SPY", 0, bars, kEnd)).detail_code,
             QStringLiteral("contract_identity_invalid"));
    // The contract answered for another symbol.
    const auto other = assess_ibkr_daily(parse_ibkr_daily_envelope(ibkr_history_envelope("IVV", 1, bars, kEnd)), "SPY",
                                         "2 Y", kEnd, kLast, utc("2026-09-26T09:00:00.000Z"));
    QCOMPARE(other.detail_code, QStringLiteral("contract_identity_invalid"));
}

void TstEtfIbkrDaily::malformed_or_duplicated_rows_refuse_the_response() {
    QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
    QJsonObject p = ibkr_history_envelope("SPY", 756733, bars, kEnd);
    QJsonArray rows = p.value("bars").toArray();
    QJsonObject bad = rows[2].toObject();
    bad.remove("close");
    rows[2] = bad;
    p.insert("bars", rows);
    QCOMPARE(assess(p).detail_code, QStringLiteral("bars_malformed"));
    QVERIFY(assess(p).accepted.isEmpty()); // never partially used

    QJsonObject intraday = ibkr_history_envelope("SPY", 756733, bars, kEnd);
    QJsonArray r2 = intraday.value("bars").toArray();
    QJsonObject stamp = r2[0].toObject();
    stamp.insert("date", "20260914 16:00:00");
    r2[0] = stamp;
    intraday.insert("bars", r2);
    QCOMPARE(assess(intraday).detail_code, QStringLiteral("bars_malformed"));

    bars.append(bars.last());
    QCOMPARE(assess(ibkr_history_envelope("SPY", 756733, bars, kEnd)).detail_code,
             QStringLiteral("bar_dates_duplicated"));
}

QTEST_GUILESS_MAIN(TstEtfIbkrDaily)
#include "tst_etf_ibkr_daily.moc"
