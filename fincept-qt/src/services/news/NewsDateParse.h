// src/services/news/NewsDateParse.h
//
// News article timestamp parsing, shared by NewsService_Parsing.cpp and the
// unit tests. Header-only over Qt Core: no service, network or storage types,
// so the suite can pin the missing-vs-undated date decision without linking the
// application.
//
// Rules (MarketLab semantics):
//   * A missing or unparsable date stays invalid, so the article is undated
//     (sort_ts == 0) instead of being promoted to "now".
//   * An appended zone abbreviation may only be applied when it is
//     unambiguous (GMT/UTC). Any other abbreviation is left unknown and the
//     article stays undated: guessing a local-time interpretation can make an
//     uncertain timestamp falsely current or falsely stale around the 1H/6H/24H
//     window cutoffs.
//   * Numeric offsets (+0200, +02:00) are parsed from the timestamp itself.
//   * A publication time more than 6h in the future is a provider data error
//     and leaves the article undated.
#pragma once

#include <QDateTime>
#include <QString>
#include <QTimeZone>

#include <algorithm>

namespace fincept::services {

inline QDateTime news_parse_datetime(const QString& text, qint64 now_secs = QDateTime::currentSecsSinceEpoch()) {
    constexpr qint64 kFutureToleranceSec = 6 * 3600;

    const QString t = text.trimmed();
    if (t.isEmpty())
        return {};

    // Epoch seconds/milliseconds (some JSON-backed feeds). The same future
    // tolerance applies here: an epoch seven hours or more ahead is a provider
    // data error too, not a "current" article.
    bool numeric = false;
    const qlonglong as_number = t.toLongLong(&numeric);
    if (numeric && t.size() >= 9) {
        const qint64 secs = t.size() >= 12 ? as_number / 1000 : as_number;
        const QDateTime from_epoch = QDateTime::fromSecsSinceEpoch(secs);
        if (from_epoch.isValid() && from_epoch.toSecsSinceEpoch() <= now_secs + kFutureToleranceSec)
            return from_epoch;
    }

    static const char* kFormats[] = {
        "ddd, dd MMM yyyy HH:mm:ss", "ddd, dd MMM yyyy HH:mm", "dd MMM yyyy HH:mm:ss", "dd MMM yyyy HH:mm",
        "MMM dd, yyyy HH:mm",        "yyyy-MM-dd HH:mm:ss",    "yyyy-MM-dd HH:mm",
    };

    auto try_all = [&](const QString& value) -> QDateTime {
        QDateTime parsed = QDateTime::fromString(value, Qt::RFC2822Date);
        if (!parsed.isValid())
            parsed = QDateTime::fromString(value, Qt::ISODate);
        if (parsed.isValid())
            return parsed;
        for (const char* format : kFormats) {
            parsed = QDateTime::fromString(value, QString::fromLatin1(format));
            if (parsed.isValid())
                return parsed;
        }
        return {};
    };

    QDateTime dt = try_all(t);

    // Some feeds append a zone abbreviation Qt's RFC/ISO parsers do not
    // accept. Only GMT/UTC may be applied; every other abbreviation keeps the
    // article undated rather than being reinterpreted as local time.
    if (!dt.isValid()) {
        const int space = t.lastIndexOf(QLatin1Char(' '));
        if (space > 0) {
            const QString tail = t.mid(space + 1);
            const bool looks_like_zone = tail.size() >= 2 && tail.size() <= 5 && tail != QLatin1String("AM") &&
                                         tail != QLatin1String("PM") && tail.at(0).isUpper() &&
                                         std::all_of(tail.cbegin(), tail.cend(), [](QChar c) { return c.isLetter(); });
            if (looks_like_zone) {
                const QString zone = tail.toUpper();
                if (zone == QLatin1String("GMT") || zone == QLatin1String("UTC")) {
                    QDateTime stripped = try_all(t.left(space));
                    if (stripped.isValid()) {
                        stripped.setTimeZone(QTimeZone(QTimeZone::UTC));
                        dt = stripped;
                    }
                }
            }
        }
    }

    if (dt.isValid()) {
        if (dt.toSecsSinceEpoch() > now_secs + kFutureToleranceSec)
            return {};
    }
    return dt;
}

} // namespace fincept::services
