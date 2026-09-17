// tests/tst_news_date_parse.cpp
//
// MarketLab: news timestamp parsing. A missing or unparsable date stays
// undated; an unknown zone abbreviation must NOT be reinterpreted as local
// time; GMT/UTC and numeric offsets parse exactly; a date more than 6h in the
// future is a provider error and stays undated.
//
// Header-only over Qt Core ("services/news/NewsDateParse.h"), no app sources.

#include <QDate>
#include <QDateTime>
#include <QTest>
#include <QTime>
#include <QTimeZone>

#include "services/news/NewsDateParse.h"

using fincept::services::news_parse_datetime;

namespace {
qint64 utc_secs(int y, int m, int d, int hh, int mm, int ss) {
    return QDateTime(QDate(y, m, d), QTime(hh, mm, ss), QTimeZone(QTimeZone::UTC))
        .toSecsSinceEpoch();
}
QString utc_string(qint64 secs) {
    return QDateTime::fromSecsSinceEpoch(secs).toUTC().toString("ddd, dd MMM yyyy HH:mm:ss") +
           " GMT";
}
} // namespace

class TstNewsDateParse : public QObject {
    Q_OBJECT

  private slots:
    void gmtZoneParsesAsUtc() {
        // Tue, 15 Sep 2026 21:58:41 GMT
        const QDateTime dt = news_parse_datetime("Tue, 15 Sep 2026 21:58:41 GMT");
        QVERIFY(dt.isValid());
        QCOMPARE(dt.toSecsSinceEpoch(), utc_secs(2026, 9, 15, 21, 58, 41));
        QCOMPARE(dt.offsetFromUtc(), 0);
    }

    void noSecondsWithGmtParses() {
        const QDateTime dt = news_parse_datetime("Tue, 15 Sep 2026 21:58 GMT");
        QVERIFY(dt.isValid());
        QCOMPARE(dt.toSecsSinceEpoch(), utc_secs(2026, 9, 15, 21, 58, 0));
    }

    void isoOffsetParses() {
        const QDateTime dt = news_parse_datetime("2026-09-15T21:58:41+02:00");
        QVERIFY(dt.isValid());
        QCOMPARE(dt.toSecsSinceEpoch(), utc_secs(2026, 9, 15, 19, 58, 41));
    }

    void rfc2822NumericOffsetParses() {
        const QDateTime dt = news_parse_datetime("Tue, 15 Sep 2026 21:58:41 +0200");
        QVERIFY(dt.isValid());
        QCOMPARE(dt.toSecsSinceEpoch(), utc_secs(2026, 9, 15, 19, 58, 41));
    }

    void unknownZoneStaysUndated() {
        // An unknown abbreviation must not become a local-time guess.
        QVERIFY(!news_parse_datetime("Tue, 15 Sep 2026 21:58:41 XYZ").isValid());
        QVERIFY(!news_parse_datetime("Tue, 15 Sep 2026 21:58:41 CEST").isValid());
        QVERIFY(!news_parse_datetime("15 Sep 2026 21:58:41 CEST").isValid());
    }

    void missingOrMalformedStaysUndated() {
        QVERIFY(!news_parse_datetime("").isValid());
        QVERIFY(!news_parse_datetime("   ").isValid());
        QVERIFY(!news_parse_datetime("not a date").isValid());
        QVERIFY(!news_parse_datetime("2026-13-45 99:99:99").isValid());
    }

    void futureBeyondToleranceStaysUndated() {
        const qint64 now = 1800000000; // fixed instant keeps the test deterministic
        const QDateTime beyond = news_parse_datetime(utc_string(now + 7 * 3600), now);
        QVERIFY(!beyond.isValid());
    }

    void futureWithinToleranceParses() {
        const qint64 now = 1800000000;
        const qint64 target = now + 5 * 3600;
        const QDateTime within = news_parse_datetime(utc_string(target), now);
        QVERIFY(within.isValid());
        QCOMPARE(within.toSecsSinceEpoch(), target);
    }

    void epochWithinToleranceParses() {
        const qint64 now = 1800000000;
        const qint64 target = now + 5 * 3600;
        const QDateTime within = news_parse_datetime(QString::number(target), now);
        QVERIFY(within.isValid());
        QCOMPARE(within.toSecsSinceEpoch(), target);
    }

    void epochFutureBeyondToleranceStaysUndated() {
        const qint64 now = 1800000000;
        // Seconds and milliseconds must both obey the same future tolerance.
        QVERIFY(!news_parse_datetime(QString::number(now + 7 * 3600), now).isValid());
        QVERIFY(!news_parse_datetime(QString::number((now + 7 * 3600) * 1000), now).isValid());
    }
};

QTEST_MAIN(TstNewsDateParse)
#include "tst_news_date_parse.moc"
