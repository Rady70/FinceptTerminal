#pragma once
// DailyUpdateRule — when the daily FedWatch / ETF Flow update is due.
//
// One update per U.S. trading session, after its close: the session is the
// latest weekday whose 16:30 New York time has passed. A job is due when the
// state file does not record that session as done, it has been tried fewer
// than kMaxAttempts times for it, and the last try is at least
// kRetryAfterMin minutes old. Exchange holidays are not known: on a holiday
// the update runs once more and stores nothing new.
//
// The same rule and the same state file (<profile>/daily_update/state.json)
// are used by the app while it is open (DailyUpdateScheduler) and by
// scripts/daily_update.py, which the Windows scheduled task runs while the
// app is closed, so a session is never updated twice.
#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QTimeZone>

namespace fincept::services::daily_update {

inline constexpr int kMaxAttempts = 3;
inline constexpr int kRetryAfterMin = 50;
inline constexpr int kCloseMinutesNy = 16 * 60 + 30; ///< 16:30 New York: close + 30 minutes

/// The trading session the next update covers (New York date).
inline QDate session_for(const QDateTime& utc) {
    const QDateTime ny = utc.toTimeZone(QTimeZone(QByteArrayLiteral("America/New_York")));
    QDate d = ny.date();
    const int minutes = ny.time().hour() * 60 + ny.time().minute();
    if (d.dayOfWeek() > 5 || minutes < kCloseMinutesNy)
        d = d.addDays(-1);
    while (d.dayOfWeek() > 5)
        d = d.addDays(-1);
    return d;
}

/// `state` is the job's object in the state file: {session, ok, attempts,
/// last_attempt (ISO UTC), ...}.
inline bool job_due(const QJsonObject& state, const QDate& session, const QDateTime& now_utc) {
    const QString s = session.toString(Qt::ISODate);
    if (state.value(QStringLiteral("done_session")).toString() == s)
        return false;
    if (state.value(QStringLiteral("attempt_session")).toString() == s) {
        if (state.value(QStringLiteral("attempts")).toInt() >= kMaxAttempts)
            return false;
        const QDateTime last =
            QDateTime::fromString(state.value(QStringLiteral("last_attempt")).toString(), Qt::ISODate);
        if (last.isValid() && last.secsTo(now_utc) < kRetryAfterMin * 60)
            return false;
    }
    return true;
}

/// The job's state after one attempt for `session`.
inline QJsonObject after_attempt(QJsonObject state, const QDate& session, const QDateTime& now_utc, bool ok,
                                 const QString& runner, const QString& detail) {
    const QString s = session.toString(Qt::ISODate);
    const int attempts = state.value(QStringLiteral("attempt_session")).toString() == s
                             ? state.value(QStringLiteral("attempts")).toInt()
                             : 0;
    state.insert(QStringLiteral("attempt_session"), s);
    state.insert(QStringLiteral("attempts"), attempts + 1);
    state.insert(QStringLiteral("last_attempt"), now_utc.toUTC().toString(Qt::ISODate));
    state.insert(QStringLiteral("last_ok"), ok);
    state.insert(QStringLiteral("last_runner"), runner);
    state.insert(QStringLiteral("last_detail"), detail.left(400));
    if (ok)
        state.insert(QStringLiteral("done_session"), s);
    return state;
}

} // namespace fincept::services::daily_update
