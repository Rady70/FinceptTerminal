// tst_etf_ibkr_daily.cpp — ETF Capital Flows Batch B: the IBKR daily-bar
// boundary (services/etf/EtfIbkrDaily.h).
//
// Pins what reaches storage from a wrapper `history` envelope: bar dates kept
// exactly as IBKR reported them; the in-progress session rejected on its own;
// missing expected sessions and stale history reported; entitlement,
// TWS/process failures and IBKR errors kept apart; an ordinary share never
// taken for an ETF; a response whose parameters, identity or dating (a bar on
// a holiday or a weekend, two different bars for one date) cannot be trusted
// refused as a whole; and an uncertain row or field (a bad close, an unusable
// volume, an uncovered date, a session after the requested end, an identical
// repeat) rejected on its own while the valid rows are kept, also from a
// series the wrapper withheld (`retained_bars`). Synthetic prices only.

#include "etf_test_fixtures.h"
#include "services/etf/EtfIbkrDaily.h"

#include <QTest>

#include <functional>

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
    void untrusted_bar_dates_refuse_the_response();
    void wrapper_stale_rule_is_kept();
    void entitlement_block_is_not_an_error();
    void tws_unavailable_is_a_source_error();
    void ibkr_error_is_a_source_error();
    void parameter_mismatch_is_refused();
    void contract_identity_is_checked();
    void an_ordinary_stock_is_not_an_etf();
    void uncertain_rows_and_fields_are_rejected_alone();
    void conflicting_duplicate_refuses_the_response();
    void withheld_series_keeps_its_valid_rows();
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
    // After the close, the same bar is a completed session the request did not
    // ask for: that bar alone is not used (and recorded); the four sessions the
    // request asked for are kept.
    const auto after = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd), "2026-09-25T21:00:00.000Z");
    QCOMPARE(after.status, RetrievalStatus::Ok);
    QCOMPARE(after.detail_code, QStringLiteral("bars_partially_kept"));
    QCOMPARE(after.accepted.size(), 4);
    QCOMPARE(after.accepted.last().session_date, kLast);
    int after_end = 0;
    for (const auto& i : after.issues)
        after_end += i.code == QLatin1String("bar_after_requested_end") ? 1 : 0;
    QCOMPARE(after_end, 1);
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

void TstEtfIbkrDaily::untrusted_bar_dates_refuse_the_response() {
    // A bar on a holiday or a weekend means the response's dating cannot be
    // trusted: the whole response is refused, and the bar is never re-dated.
    for (const char* date : {"20260907", "20260920"}) { // Labor Day, a Sunday
        QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
        bars.append(BarSpec{QString::fromLatin1(date)});
        const auto a = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd), "2026-09-26T09:00:00.000Z");
        QCOMPARE(a.status, RetrievalStatus::SourceError);
        QCOMPARE(a.detail_code, QStringLiteral("bar_on_non_session_date"));
        QVERIFY(a.detail.contains(QString::fromLatin1(date)));
        QVERIFY(a.accepted.isEmpty());
        QVERIFY(a.issues.isEmpty());
        QVERIFY(a.window_days.isEmpty());
    }
    // A bar the calendar does not cover, or a completed session after the
    // requested end, is excluded on its own; the nine valid sessions are kept.
    const struct {
        const char* date;
        const char* code;
    } alone[] = {{"20181228", "bar_outside_calendar_coverage"}, {"20260925", "bar_after_requested_end"}};
    for (const auto& c : alone) {
        QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
        bars.append(BarSpec{QString::fromLatin1(c.date)});
        const auto a = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd), "2026-09-26T09:00:00.000Z");
        QCOMPARE(a.status, RetrievalStatus::Ok);
        QCOMPARE(a.accepted.size(), 9);
        QCOMPARE(a.detail_code, QStringLiteral("bars_partially_kept"));
        int coded = 0;
        for (const auto& i : a.issues)
            coded += i.code == QLatin1String(c.code) ? 1 : 0;
        QCOMPARE(coded, 1);
    }
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

void TstEtfIbkrDaily::an_ordinary_stock_is_not_an_etf() {
    // secType STK is an ETF and an ordinary share alike. A valid, resolved
    // ordinary-share contract (AAPL-like) must not become ETF data.
    const QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
    const QJsonObject aapl = with_stock_type(ibkr_history_envelope("AAPL", 265598, bars, kEnd), "COMMON");
    const auto common =
        assess_ibkr_daily(parse_ibkr_daily_envelope(aapl), "AAPL", "2 Y", kEnd, kLast, utc("2026-09-26T09:00:00.000Z"));
    QCOMPARE(common.status, RetrievalStatus::SourceError);
    QCOMPARE(common.detail_code, QStringLiteral("etf_identity_not_established"));
    QVERIFY(common.detail.contains(QLatin1String("COMMON")));
    QVERIFY(common.accepted.isEmpty());
    // An adapter that does not report stockType cannot establish an ETF,
    // whatever the symbol: SPY is refused too, never assumed.
    const auto unreported =
        assess(with_stock_type(ibkr_history_envelope("SPY", 756733, bars, kEnd), QJsonValue(QJsonValue::Undefined)));
    QCOMPARE(unreported.detail_code, QStringLiteral("etf_identity_not_established"));
    QVERIFY(unreported.accepted.isEmpty());
    for (const QJsonValue& blank : {QJsonValue(""), QJsonValue(QJsonValue::Null)})
        QCOMPARE(assess(with_stock_type(ibkr_history_envelope("SPY", 756733, bars, kEnd), blank)).detail_code,
                 QStringLiteral("etf_identity_not_established"));
    // IBKR's own ETF classification is what makes it one.
    const auto etf = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd));
    QCOMPARE(etf.status, RetrievalStatus::Ok);
    QCOMPARE(parse_ibkr_daily_envelope(ibkr_history_envelope("SPY", 756733, bars, kEnd)).stock_type,
             QStringLiteral("ETF"));
}

void TstEtfIbkrDaily::uncertain_rows_and_fields_are_rejected_alone() {
    const QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast); // nine sessions
    auto with_row = [&](int index, const std::function<void(QJsonObject&)>& edit) {
        QJsonObject p = ibkr_history_envelope("SPY", 756733, bars, kEnd);
        QJsonArray rows = p.value("bars").toArray();
        QJsonObject r = rows[index].toObject();
        edit(r);
        rows[index] = r;
        p.insert("bars", rows);
        return p;
    };
    auto issue_codes = [](const IbkrDailyAssessment& a) {
        QStringList c;
        for (const auto& i : a.issues)
            c << i.code;
        return c;
    };
    {
        // One bar without a close: that row is excluded (and its session is
        // missing); the other eight are kept.
        const auto a = assess(with_row(2, [](QJsonObject& r) { r.remove("close"); }));
        QCOMPARE(a.status, RetrievalStatus::Ok);
        QCOMPARE(a.detail_code, QStringLiteral("bars_partially_kept"));
        QCOMPARE(a.accepted.size(), 8);
        QVERIFY(issue_codes(a).contains(QStringLiteral("bar_row_excluded")));
        QCOMPARE(count_state(a, QualityState::Missing), 1);
    }
    {
        // An intraday stamp instead of a session date: excluded, never re-dated.
        const auto a = assess(with_row(0, [](QJsonObject& r) { r.insert("date", "20260914 16:00:00"); }));
        QCOMPARE(a.accepted.size(), 8);
        QVERIFY(issue_codes(a).contains(QStringLiteral("bar_row_excluded")));
    }
    {
        // A missing volume does not make the prices uncertain: all nine bars are
        // kept, that bar's volume is stored as missing, never zero.
        const auto a = assess(with_row(4, [](QJsonObject& r) { r.remove("volume"); }));
        QCOMPARE(a.status, RetrievalStatus::Ok);
        QCOMPARE(a.accepted.size(), 9);
        QVERIFY(issue_codes(a).contains(QStringLiteral("bar_field_unusable")));
        QCOMPARE(a.accepted[4].volume.state, ValueState::Missing);
        QVERIFY(a.accepted[4].close.reported());
        // A negative volume is kept as unparseable, not as a number.
        const auto n = assess(with_row(4, [](QJsonObject& r) { r.insert("volume", -5.0); }));
        QCOMPARE(n.accepted.size(), 9);
        QCOMPARE(n.accepted[4].volume.state, ValueState::Unparseable);
    }
    {
        // An identical repeat of a bar is kept once.
        QVector<BarSpec> twice = bars;
        twice.append(bars[3]);
        const auto a = assess(ibkr_history_envelope("SPY", 756733, twice, kEnd));
        QCOMPARE(a.status, RetrievalStatus::Ok);
        QCOMPARE(a.accepted.size(), 9);
        QVERIFY(issue_codes(a).contains(QStringLiteral("bar_row_duplicated")));
    }
}

void TstEtfIbkrDaily::conflicting_duplicate_refuses_the_response() {
    // Two different bars for one date: the response's dating cannot be trusted.
    QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), kLast);
    BarSpec other = bars[3];
    other.close = 120.0;
    bars.append(other);
    const auto a = assess(ibkr_history_envelope("SPY", 756733, bars, kEnd));
    QCOMPARE(a.status, RetrievalStatus::SourceError);
    QCOMPARE(a.detail_code, QStringLiteral("bar_dates_conflicting"));
    QVERIFY(a.accepted.isEmpty());
    QVERIFY(a.issues.isEmpty());
}

void TstEtfIbkrDaily::withheld_series_keeps_its_valid_rows() {
    // The wrapper withholds a series it judges stale or invalid from `bars`
    // (its envelope contract) and returns the individually valid rows apart.
    const QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), QDate(2026, 9, 22)); // 7, ends early
    auto withheld = [&](const char* status, const char* reason, const QJsonArray& problems) {
        QJsonObject p = ibkr_history_envelope("SPY", 756733, bars, kEnd);
        const QJsonArray rows = p.value("bars").toArray();
        p.insert("bars", QJsonArray());
        p.insert("retained_bars", rows);
        p.insert("row_problems", problems);
        QJsonObject cls = p.value("classification").toObject();
        cls.insert("usable", false);
        cls.insert("status", QLatin1String(status));
        cls.insert("validation_reason", QLatin1String(reason));
        p.insert("classification", cls);
        return p;
    };
    {
        // Stale by the wrapper's own rule: the seven completed sessions are kept,
        // the two newest expected sessions are missing, the retrieval is STALE.
        const auto a = assess(withheld("STALE", "HISTORY_STALE", {}));
        QCOMPARE(a.status, RetrievalStatus::Stale);
        QCOMPARE(a.accepted.size(), 7);
        QCOMPARE(count_state(a, QualityState::Missing), 2);
        QVERIFY(a.detail.contains(QStringLiteral("HISTORY_STALE")));
    }
    {
        // One row the wrapper excluded (crossed OHLC): recorded, the rest kept.
        const QJsonArray problems{
            QJsonObject{{"index", 7}, {"date", "20260923"}, {"reason", "BAR_OHLC_RELATION_INVALID"}, {"scope", "row"}}};
        const auto a = assess(withheld("VALUES_INVALID", "BAR_OHLC_RELATION_INVALID", problems));
        QCOMPARE(a.accepted.size(), 7);
        int excluded = 0;
        for (const auto& i : a.issues)
            excluded += i.code == QLatin1String("bar_row_excluded") && i.session_date == QDate(2026, 9, 23) ? 1 : 0;
        QCOMPARE(excluded, 1);
    }
    {
        // Without retained rows nothing is kept, as before.
        QJsonObject p = withheld("STALE", "HISTORY_STALE", {});
        p.remove("retained_bars");
        const auto a = assess(p);
        QCOMPARE(a.status, RetrievalStatus::Stale);
        QVERIFY(a.accepted.isEmpty());
    }
}

QTEST_GUILESS_MAIN(TstEtfIbkrDaily)
#include "tst_etf_ibkr_daily.moc"
