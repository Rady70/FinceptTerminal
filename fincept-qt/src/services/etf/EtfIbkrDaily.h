// src/services/etf/EtfIbkrDaily.h
//
// The IBKR boundary of the ETF data foundation (ETF Capital Flows Batch B):
// completed-session daily TRADES bars, regular trading hours, from the
// existing read-only wrapper (scripts/ibkr_tws_data.py), checked against the
// U.S. equity session calendar before anything is stored.
//
// Batch A2 qualified the regular-hours close and IBKR's filtered regular-hours
// volume as RAW inputs only (A2 section 7.2). This header stores neither a
// rotation measure nor an adjusted price: open/high/low/close are IBKR's
// split-adjusted, not dividend-adjusted TRADES prices, and volume is IBKR's
// filtered volume, which is not consolidated volume and never ETF flow.
// ADJUSTED_LAST and WAP are not read (not qualified).
//
// What the assessment guarantees:
//   * a bar of a session that had not closed when the request was made is
//     never accepted (IN_PROGRESS_SESSION, A2 section 7.7);
//   * a bar on a holiday, a weekend or a date the calendar does not cover is
//     refused, never re-dated;
//   * every calendar session in the returned window without a bar is recorded
//     as MISSING, never filled;
//   * history that ends before the last completed session is STALE;
//   * a response whose parameters, contract identity or bar rows cannot be
//     trusted is refused as a whole (SOURCE_ERROR), never partially used.
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfDataModel.h"
#include "services/etf/EtfSessionCalendar.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fincept::services::etf {

inline constexpr const char* kIbkrDailyInterpretation = "ibkr_trades_rth_daily_v1";
inline constexpr const char* kIbkrWhatToShow = "TRADES";
inline constexpr const char* kIbkrBarSize = "1 day";

struct IbkrDailyBarRow {
    QString date_text; ///< the bar date exactly as IBKR reported it ("20260924")
    QDate session_date;
    FieldValue open;
    FieldValue high;
    FieldValue low;
    FieldValue close;
    FieldValue volume; ///< shares, IBKR-filtered, regular hours
};

struct IbkrDailyEnvelope {
    bool wrapper_ok = false; ///< the envelope's own ok flag
    QString failure_type;
    QString failure_stage;
    QString failure_message;
    QString retrieved_at_raw;
    QDateTime retrieved_at; ///< UTC; the wrapper's read time
    QString symbol;
    qint64 con_id = 0;
    QString contract_symbol;
    QString security_type;
    QString exchange;
    QString primary_exchange;
    QString currency;
    QString param_end_date_time;
    QString param_duration;
    QString param_bar_size;
    QString param_what_to_show;
    std::optional<bool> param_use_rth;
    bool usable = false;
    QString status;
    QString entitlement;
    QString validation_reason;
    QString error_message;
    QJsonValue error_code;
    QVector<IbkrDailyBarRow> bars;
    int malformed_bars = 0;
    QString first_malformed_reason;
    QJsonObject adapter; ///< runtime identity (adapter commit, ibapi and TWS versions, client id)
};

namespace ibkr_daily_detail {

inline FieldValue json_number(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull())
        return FieldValue::missing();
    if (!v.isDouble() || !std::isfinite(v.toDouble())) {
        const QJsonDocument d = QJsonDocument(QJsonArray{v});
        return FieldValue::unparseable(QString::fromUtf8(d.toJson(QJsonDocument::Compact)));
    }
    return FieldValue::reported_value(v.toDouble());
}

inline QString value_text(const QJsonValue& v) {
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 17);
    return {};
}

inline QString text(const QJsonObject& o, const char* key) {
    return value_text(o.value(QLatin1String(key)));
}

} // namespace ibkr_daily_detail

/// Read a wrapper `history` envelope. Validation of the envelope's own shape
/// (source, command, adapter identity) already happened at the service
/// boundary (ibkr_wrapper_payload_valid); this reads its content.
inline IbkrDailyEnvelope parse_ibkr_daily_envelope(const QJsonObject& payload) {
    using namespace ibkr_daily_detail;
    IbkrDailyEnvelope e;
    e.wrapper_ok = payload.value(QLatin1String("ok")).toBool(false);
    e.retrieved_at_raw = payload.value(QLatin1String("retrieved_at")).toString();
    const QDateTime parsed = QDateTime::fromString(e.retrieved_at_raw, Qt::ISODate);
    e.retrieved_at = parsed.isValid() ? parsed.toUTC() : QDateTime();
    e.adapter = payload.value(QLatin1String("adapter")).toObject();
    if (!e.wrapper_ok) {
        const QJsonObject failure = payload.value(QLatin1String("failure")).toObject();
        e.failure_type = failure.value(QLatin1String("type")).toString();
        e.failure_stage = failure.value(QLatin1String("stage")).toString();
        e.failure_message = failure.value(QLatin1String("message")).toString();
        return e;
    }
    e.symbol = payload.value(QLatin1String("symbol")).toString();
    const QJsonObject contract = payload.value(QLatin1String("contract")).toObject();
    const QJsonValue con_id = contract.value(QLatin1String("con_id"));
    e.con_id = con_id.isDouble() ? static_cast<qint64>(con_id.toDouble()) : 0;
    if (con_id.isDouble() && con_id.toDouble() != static_cast<double>(e.con_id))
        e.con_id = 0; // a fractional conId is not an identity
    e.contract_symbol = text(contract, "symbol");
    e.security_type = text(contract, "security_type");
    e.exchange = text(contract, "exchange");
    e.primary_exchange = text(contract, "primary_exchange");
    e.currency = text(contract, "currency");
    const QJsonObject params = payload.value(QLatin1String("parameters")).toObject();
    e.param_end_date_time = text(params, "end_date_time");
    e.param_duration = text(params, "duration");
    e.param_bar_size = text(params, "bar_size");
    e.param_what_to_show = text(params, "what_to_show");
    if (params.value(QLatin1String("use_rth")).isBool())
        e.param_use_rth = params.value(QLatin1String("use_rth")).toBool();
    const QJsonObject cls = payload.value(QLatin1String("classification")).toObject();
    e.usable = cls.value(QLatin1String("usable")).toBool(false);
    e.status = cls.value(QLatin1String("status")).toString();
    e.entitlement = cls.value(QLatin1String("entitlement")).toString();
    e.validation_reason = cls.value(QLatin1String("validation_reason")).toString();
    e.error_message = cls.value(QLatin1String("error_message")).toString();
    e.error_code = cls.value(QLatin1String("error_code"));
    for (const QJsonValue& v : payload.value(QLatin1String("bars")).toArray()) {
        if (!v.isObject()) {
            ++e.malformed_bars;
            if (e.first_malformed_reason.isEmpty())
                e.first_malformed_reason = QStringLiteral("bar_row_not_an_object");
            continue;
        }
        const QJsonObject b = v.toObject();
        IbkrDailyBarRow row;
        row.date_text = b.value(QLatin1String("date")).toString().trimmed();
        // A daily bar's date is the exchange session date, yyyyMMdd. Anything
        // else (an intraday stamp, an empty field) is not re-interpreted.
        if (row.date_text.size() == 8)
            row.session_date = QDate::fromString(row.date_text, QStringLiteral("yyyyMMdd"));
        row.open = json_number(b, "open");
        row.high = json_number(b, "high");
        row.low = json_number(b, "low");
        row.close = json_number(b, "close");
        row.volume = json_number(b, "volume");
        QString reason;
        if (!row.session_date.isValid())
            reason = QStringLiteral("bar_date_not_a_session_date");
        else if (!row.close.reported() || !(row.close.value > 0.0))
            reason = QStringLiteral("bar_close_missing_or_not_positive");
        else if (!row.volume.reported() || row.volume.value < 0.0)
            reason = QStringLiteral("bar_volume_missing_or_negative");
        if (!reason.isEmpty()) {
            ++e.malformed_bars;
            if (e.first_malformed_reason.isEmpty())
                e.first_malformed_reason = reason;
            continue;
        }
        e.bars.append(row);
    }
    return e;
}

struct IbkrDailyIssue {
    QDate session_date; ///< invalid for a retrieval-level issue
    QualityState state = QualityState::SourceError;
    QString code;
    QString detail;
};

struct IbkrDailyAssessment {
    RetrievalStatus status = RetrievalStatus::SourceError;
    QString detail_code;
    QString detail;
    QVector<IbkrDailyBarRow> accepted; ///< in date order
    QVector<IbkrDailyIssue> issues;
    QDate window_first;                    ///< first accepted session (invalid when none)
    QDate window_last;                     ///< the last completed session the request asked for
    QVector<MarketSessionDay> window_days; ///< every weekday in the window, sessions and holidays
};

/// Judge one daily-history response against what was asked for and against
/// the session calendar. `requested_last_session` is the last completed
/// session the request asked for (kIbkrRequestEndRule) and `requested_at_utc`
/// the moment the request was made.
inline IbkrDailyAssessment assess_ibkr_daily(const IbkrDailyEnvelope& e, const QString& requested_symbol,
                                             const QString& requested_duration, const QString& requested_end,
                                             const QDate& requested_last_session, const QDateTime& requested_at_utc) {
    IbkrDailyAssessment a;
    a.window_last = requested_last_session;
    auto fail = [&a](RetrievalStatus status, const QString& code, const QString& detail) {
        a.status = status;
        a.detail_code = code;
        a.detail = detail;
        a.accepted.clear();
        return a;
    };
    if (!e.wrapper_ok) {
        // TWS not running, not logged in, pin or dependency mismatch, a killed
        // or crashed process: a typed wrapper failure, never data.
        return fail(RetrievalStatus::SourceError,
                    e.failure_type.isEmpty() ? QStringLiteral("IBKR_FAILURE") : e.failure_type,
                    QStringLiteral("%1: %2").arg(e.failure_stage, e.failure_message));
    }
    if (e.status == QLatin1String("NOT_ENTITLED"))
        return fail(RetrievalStatus::NotEntitled, QStringLiteral("NOT_ENTITLED"), e.error_message);
    if (e.status == QLatin1String("STALE"))
        return fail(RetrievalStatus::Stale,
                    e.validation_reason.isEmpty() ? QStringLiteral("HISTORY_STALE") : e.validation_reason,
                    QStringLiteral("the wrapper's own freshness rule withheld the series"));
    if (!e.usable || e.status != QLatin1String("OK")) {
        const QString code = e.status.isEmpty() ? QStringLiteral("UNUSABLE") : e.status;
        QString detail = e.error_message;
        if (!e.error_code.isUndefined() && !e.error_code.isNull())
            detail =
                QStringLiteral("IBKR error %1: %2").arg(ibkr_daily_detail::value_text(e.error_code), e.error_message);
        return fail(RetrievalStatus::SourceError, code, detail);
    }
    if (e.param_what_to_show != QLatin1String(kIbkrWhatToShow) || e.param_bar_size != QLatin1String(kIbkrBarSize) ||
        !e.param_use_rth.has_value() || !*e.param_use_rth || e.param_end_date_time != requested_end ||
        e.param_duration != requested_duration) {
        const QString rth = e.param_use_rth.has_value()
                                ? (*e.param_use_rth ? QStringLiteral("true") : QStringLiteral("false"))
                                : QStringLiteral("missing");
        return fail(RetrievalStatus::SourceError, QStringLiteral("parameters_mismatch"),
                    QStringLiteral("returned parameters (%1, %2, rth=%3, end '%4', duration '%5') differ from the "
                                   "qualified request (TRADES, 1 day, rth=true, end '%6', duration '%7')")
                        .arg(e.param_what_to_show, e.param_bar_size, rth, e.param_end_date_time, e.param_duration,
                             requested_end, requested_duration));
    }
    if (e.con_id <= 0 || e.contract_symbol.compare(requested_symbol, Qt::CaseInsensitive) != 0 ||
        e.currency.isEmpty() || e.security_type.isEmpty()) {
        return fail(RetrievalStatus::SourceError, QStringLiteral("contract_identity_invalid"),
                    QStringLiteral("contract conId %1, symbol '%2', currency '%3', type '%4'")
                        .arg(e.con_id)
                        .arg(e.contract_symbol, e.currency, e.security_type));
    }
    if (e.malformed_bars > 0) {
        return fail(
            RetrievalStatus::SourceError, QStringLiteral("bars_malformed"),
            QStringLiteral("%1 bar row(s) malformed, first: %2").arg(e.malformed_bars).arg(e.first_malformed_reason));
    }
    QSet<QDate> seen;
    for (const IbkrDailyBarRow& b : e.bars) {
        if (seen.contains(b.session_date))
            return fail(RetrievalStatus::SourceError, QStringLiteral("bar_dates_duplicated"),
                        QStringLiteral("date %1 appears more than once").arg(b.date_text));
        seen.insert(b.session_date);
    }
    if (!requested_last_session.isValid() || !requested_at_utc.isValid())
        return fail(RetrievalStatus::SourceError, QStringLiteral("request_window_unknown"),
                    QStringLiteral("the last completed session could not be determined"));

    QVector<IbkrDailyBarRow> bars = e.bars;
    std::sort(bars.begin(), bars.end(),
              [](const IbkrDailyBarRow& l, const IbkrDailyBarRow& r) { return l.session_date < r.session_date; });
    for (const IbkrDailyBarRow& b : bars) {
        const MarketSessionDay day = UsEquityCalendar::day(b.session_date);
        if (day.type == SessionDayType::OutsideCoverage) {
            a.issues.append({b.session_date, QualityState::SourceError, QStringLiteral("bar_outside_calendar_coverage"),
                             QStringLiteral("no verified session calendar for %1").arg(b.date_text)});
            continue;
        }
        if (!day.is_session()) {
            a.issues.append(
                {b.session_date, QualityState::SourceError, QStringLiteral("bar_on_non_session_date"),
                 QStringLiteral("%1 is a %2").arg(b.date_text, QLatin1String(session_day_type_id(day.type)))});
            continue;
        }
        if (day.close_utc > requested_at_utc) {
            a.issues.append({b.session_date, QualityState::InProgressSession,
                             QStringLiteral("session_not_completed_at_request"),
                             QStringLiteral("the %1 session closes at %2 UTC, after the request")
                                 .arg(b.date_text, day.close_utc.toString(Qt::ISODate))});
            continue;
        }
        if (b.session_date > requested_last_session) {
            a.issues.append({b.session_date, QualityState::SourceError, QStringLiteral("bar_after_requested_end"),
                             QStringLiteral("%1 is after the requested last session %2")
                                 .arg(b.date_text, requested_last_session.toString(Qt::ISODate))});
            continue;
        }
        a.accepted.append(b);
    }
    if (a.accepted.isEmpty()) {
        a.status = RetrievalStatus::SourceError;
        a.detail_code = QStringLiteral("no_accepted_bars");
        a.detail = QStringLiteral("%1 bar(s) returned, none acceptable").arg(e.bars.size());
        return a;
    }
    a.window_first = a.accepted.first().session_date;
    bool complete = false;
    a.window_days = UsEquityCalendar::weekdays_in(a.window_first, a.window_last, &complete);
    QSet<QDate> accepted_dates;
    for (const IbkrDailyBarRow& b : a.accepted)
        accepted_dates.insert(b.session_date);
    for (const MarketSessionDay& d : a.window_days) {
        if (d.is_session() && !accepted_dates.contains(d.date))
            a.issues.append({d.date, QualityState::Missing, QStringLiteral("expected_session_without_bar"),
                             QStringLiteral("the calendar has a session on %1 and no bar was accepted")
                                 .arg(d.date.toString(Qt::ISODate))});
    }
    if (a.accepted.last().session_date < a.window_last) {
        a.status = RetrievalStatus::Stale;
        a.detail_code = QStringLiteral("newest_bar_before_last_completed_session");
        a.detail = QStringLiteral("newest accepted bar %1, last completed session %2")
                       .arg(a.accepted.last().session_date.toString(Qt::ISODate), a.window_last.toString(Qt::ISODate));
    } else {
        a.status = RetrievalStatus::Ok;
    }
    return a;
}

} // namespace fincept::services::etf
