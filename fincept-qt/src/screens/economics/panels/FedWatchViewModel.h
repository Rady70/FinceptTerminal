#pragma once

// Display model for the FedWatch research workspace. Everything here is parsed
// from the backend `workspace` read; nothing is computed that the backend did
// not supply except display formatting. Missing values stay std::nullopt and
// are never drawn as zero.

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTimeZone>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fincept::screens::fedwatch {

using Value = std::optional<double>;

inline Value value(const QJsonValue& v) {
    if (!v.isDouble() || !std::isfinite(v.toDouble()))
        return std::nullopt;
    return v.toDouble();
}
inline QDateTime instant(const QJsonValue& v) {
    auto parsed = QDateTime::fromString(v.toString(), Qt::ISODate);
    return parsed.isValid() ? parsed.toUTC() : QDateTime{};
}

struct Band {
    double low = 0, high = 0, pct = 0;
    Value raw, previous_day, previous_week;
};
struct Outcome {
    int bp = 0;
    bool open = false;
    double pct = 0;
    QDateTime observed_at;
};
struct FedState {
    QString state = QStringLiteral("UNAVAILABLE"); // CURRENT | STALE | HISTORICAL | UNAVAILABLE
    QDateTime observed_at, source_updated_at;
    Value age_days;
    QVector<Band> bands;
    Value expected, expected_previous_day, expected_previous_week, futures_price, futures_rate;
    QString previous_status, local_status, freshness_status;
    QVector<Outcome> local;
    bool copy_conflict = false, unverified = false;
};
struct PolyState {
    QString state = QStringLiteral("UNAVAILABLE"), mapping_status, event_title, event_id;
    QDateTime observed_at;
    QVector<Outcome> outcomes;
};
struct Comparison {
    int bp = 0;
    bool open = false;
    Value fed, poly, diff;
};
struct FedDay {
    QDate date;
    QDateTime observed_at;
    QVector<Band> bands;
    Value expected;
};
struct PolyDay {
    QDate date;
    QVector<Outcome> outcomes;
    bool complete = false;
};
struct ChangeSide {
    QString state;
    Value latest, d1, d7, d30, since_first, high, low;
    QDateTime latest_at, first_at;
    int count = 0;
    QDateTime ref1, ref7, ref30; // reference observation of each lookback change
};
struct ChangeRow {
    int bp = 0;
    bool open = false;
    ChangeSide fed, poly;
    Value diff;
    QString diff_state;
};
struct Meeting {
    QString id; // ISO meeting (decision) date
    QDate date, start;
    QString status;
    int days_until = 0;
    std::optional<int> actual_bp;
    std::optional<bool> projections;
    FedState fed;
    PolyState poly;
    QVector<Comparison> comparison;
    QVector<FedDay> fed_days;
    QVector<PolyDay> poly_days;
    QVector<ChangeRow> changes;
    bool upcoming() const { return status == QLatin1String("UPCOMING") && days_until >= 0; }
};
struct Workspace {
    bool loaded = false;
    QDateTime generated_at;
    Value target_lower, target_upper;
    QString target_status, target_reason;
    QDate target_date;
    bool target_carried_forward = false;
    QDateTime acquired_at, last_refresh_at;
    Value acquisition_age_hours;
    QString next_meeting;
    QVector<Meeting> meetings;
    QJsonArray sources, errors, warnings, notes;
    QString cme_note, cme_url;

    const Meeting* find(const QString& id) const {
        for (const auto& m : meetings)
            if (m.id == id)
                return &m;
        return nullptr;
    }
    QVector<const Meeting*> upcoming() const {
        QVector<const Meeting*> out;
        for (const auto& m : meetings)
            if (m.upcoming())
                out.push_back(&m);
        return out;
    }
    Value target_mid() const {
        if (!target_lower || !target_upper)
            return std::nullopt;
        return (*target_lower + *target_upper) / 2.0;
    }
};

inline QVector<Band> bands(const QJsonArray& rows) {
    QVector<Band> out;
    for (const auto& item : rows) {
        const auto row = item.toObject();
        const auto low = value(row["rate_low"]), high = value(row["rate_high"]), pct = value(row["probability_pct"]);
        if (!low || !high || !pct)
            continue;
        out.push_back({*low, *high, *pct, value(row["raw_probability_pct"]), value(row["previous_day_pct"]),
                       value(row["previous_week_pct"])});
    }
    std::sort(out.begin(), out.end(), [](const Band& a, const Band& b) { return a.low < b.low; });
    return out;
}
inline QVector<Outcome> outcomes(const QJsonArray& rows) {
    QVector<Outcome> out;
    for (const auto& item : rows) {
        const auto row = item.toObject();
        const auto pct = value(row["probability_pct"]);
        if (!row["outcome_bp"].isDouble() || !pct)
            continue;
        out.push_back({row["outcome_bp"].toInt(), row["open_ended"].toBool(), *pct, instant(row["observed_at"])});
    }
    std::sort(out.begin(), out.end(),
              [](const Outcome& a, const Outcome& b) { return a.bp != b.bp ? a.bp < b.bp : a.open < b.open; });
    return out;
}
inline ChangeSide change_side(const QJsonObject& o) {
    return {o["state"].toString(),           value(o["latest_pct"]),         value(o["change_1d_pp"]),
            value(o["change_7d_pp"]),        value(o["change_30d_pp"]),      value(o["change_since_first_pp"]),
            value(o["observed_high_pct"]),   value(o["observed_low_pct"]),   instant(o["latest_observed_at"]),
            instant(o["first_observed_at"]), o["observation_count"].toInt(), instant(o["reference_1d_at"]),
            instant(o["reference_7d_at"]),   instant(o["reference_30d_at"])};
}

inline Workspace parse_workspace(const QJsonObject& data) {
    Workspace ws;
    ws.loaded = true;
    ws.generated_at = instant(data["generated_at"]);
    const auto target = data["current_target_range"].toObject();
    ws.target_lower = value(target["lower"]);
    ws.target_upper = value(target["upper"]);
    ws.target_status = target["status"].toString();
    ws.target_reason = target["status_reason"].toString();
    ws.target_date = QDate::fromString(target["latest_observation_date"].toString(), Qt::ISODate);
    ws.target_carried_forward = target["carried_forward"].toBool();
    const auto acquisition = data["acquisition"].toObject();
    ws.acquired_at = instant(acquisition["acquired_at"]);
    ws.acquisition_age_hours = value(acquisition["age_hours"]);
    ws.last_refresh_at = instant(data["last_refresh_at"]);
    ws.next_meeting = data["next_meeting_date"].toString();
    ws.sources = data["sources"].toArray();
    ws.errors = data["errors"].toArray();
    ws.warnings = data["warnings"].toArray();
    ws.notes = data["method_notes"].toArray();
    ws.cme_note = data["cme"].toObject()["note"].toString();
    ws.cme_url = data["cme"].toObject()["public_tool_url"].toString();
    for (const auto& item : data["meetings"].toArray()) {
        const auto m = item.toObject();
        Meeting meeting;
        meeting.id = m["meeting_date"].toString();
        meeting.date = QDate::fromString(meeting.id, Qt::ISODate);
        meeting.start = QDate::fromString(m["start_date"].toString(), Qt::ISODate);
        meeting.status = m["status"].toString();
        meeting.days_until = m["days_until"].toInt();
        if (m["actual_outcome_bp"].isDouble())
            meeting.actual_bp = m["actual_outcome_bp"].toInt();
        if (m["has_projection_materials"].isBool())
            meeting.projections = m["has_projection_materials"].toBool();
        const auto fed = m["fed"].toObject();
        meeting.fed.state = fed["state"].toString(QStringLiteral("UNAVAILABLE"));
        meeting.fed.observed_at = instant(fed["observed_at"]);
        meeting.fed.source_updated_at = instant(fed["source_updated_at"]);
        meeting.fed.age_days = value(fed["age_days"]);
        meeting.fed.bands = bands(fed["distribution"].toArray());
        meeting.fed.expected = value(fed["expected_rate"]);
        meeting.fed.expected_previous_day = value(fed["expected_rate_previous_day"]);
        meeting.fed.expected_previous_week = value(fed["expected_rate_previous_week"]);
        meeting.fed.futures_price = value(fed["futures_price"]);
        meeting.fed.futures_rate = value(fed["futures_implied_rate"]);
        meeting.fed.previous_status = fed["previous_status"].toString();
        meeting.fed.local_status = fed["local_status"].toString();
        meeting.fed.freshness_status = fed["freshness_status"].toString();
        meeting.fed.local = outcomes(fed["local"].toArray());
        meeting.fed.copy_conflict = fed["copy_conflict"].toBool();
        meeting.fed.unverified = fed["target_range_unverified"].toBool();
        const auto poly = m["polymarket"].toObject();
        meeting.poly.state = poly["state"].toString(QStringLiteral("UNAVAILABLE"));
        meeting.poly.mapping_status = poly["mapping_status"].toString();
        meeting.poly.event_title = poly["event_title"].toString();
        meeting.poly.event_id = poly["event_id"].toString();
        meeting.poly.observed_at = instant(poly["observed_at"]);
        meeting.poly.outcomes = outcomes(poly["outcomes"].toArray());
        for (const auto& c : m["comparison"].toArray()) {
            const auto row = c.toObject();
            meeting.comparison.push_back({row["outcome_bp"].toInt(), row["open_ended"].toBool(),
                                          value(row["fed_probability_pct"]), value(row["polymarket_probability_pct"]),
                                          value(row["probability_diff_pp"])});
        }
        const auto history = m["history"].toObject();
        for (const auto& d : history["fed_days"].toArray()) {
            const auto row = d.toObject();
            meeting.fed_days.push_back({QDate::fromString(row["date"].toString(), Qt::ISODate),
                                        instant(row["observed_at"]), bands(row["distribution"].toArray()),
                                        value(row["expected_rate"])});
        }
        for (const auto& d : history["polymarket_days"].toArray()) {
            const auto row = d.toObject();
            meeting.poly_days.push_back({QDate::fromString(row["date"].toString(), Qt::ISODate),
                                         outcomes(row["outcomes"].toArray()), row["complete"].toBool()});
        }
        for (const auto& c : history["changes"].toArray()) {
            const auto row = c.toObject();
            meeting.changes.push_back({row["outcome_bp"].toInt(), row["open_ended"].toBool(),
                                       change_side(row["fed"].toObject()), change_side(row["polymarket"].toObject()),
                                       value(row["current_difference_pp"]), row["difference_state"].toString()});
        }
        ws.meetings.push_back(meeting);
    }
    std::sort(ws.meetings.begin(), ws.meetings.end(),
              [](const Meeting& a, const Meeting& b) { return a.date < b.date; });
    return ws;
}

// ── Formatting ───────────────────────────────────────────────────────────────
inline QString pct(Value v, int decimals = 1) {
    return v ? QString::number(*v, 'f', decimals) + QStringLiteral("%") : QStringLiteral("—");
}
inline QString signed_number(double v, int decimals) {
    const QString text = QString::number(std::abs(v), 'f', decimals);
    if (std::abs(v) < 0.5 * std::pow(10.0, -decimals))
        return QString::number(0.0, 'f', decimals);
    return (v > 0 ? QStringLiteral("+") : QString::fromUtf8("−")) + text;
}
inline QString pp(Value v, int decimals = 1) {
    return v ? signed_number(*v, decimals) + QStringLiteral(" pp") : QStringLiteral("—");
}
inline QString bp_change(Value rate_change_pct) {
    return rate_change_pct ? signed_number(*rate_change_pct * 100.0, 0) + QStringLiteral(" bp") : QStringLiteral("—");
}
inline QString rate(Value v, int decimals = 2) {
    return v ? QString::number(*v, 'f', decimals) + QStringLiteral("%") : QStringLiteral("—");
}
inline QString band_label(double low, double high) {
    return QString::number(low, 'f', 2) + QString::fromUtf8("–") + QString::number(high, 'f', 2) + "%";
}
inline QString outcome_label(int bp, bool open) {
    if (bp == 0 && !open)
        return QStringLiteral("Hold");
    const QString size = QString::number(std::abs(bp)) + (open ? QStringLiteral("+") : QString{}) + " bp";
    return (bp < 0 ? QStringLiteral("Cut ") : QStringLiteral("Hike ")) + size;
}
inline QString outcome_key(int bp, bool open) {
    return QString::number(bp) + (open ? ":tail" : ":exact");
}
inline QString meeting_label(const QDate& date) {
    return date.isValid() ? date.toString(QStringLiteral("MMM d, yyyy")) : QStringLiteral("—");
}
inline QString short_meeting(const QDate& date) {
    return date.isValid() ? date.toString(QStringLiteral("MMM d ''yy")) : QStringLiteral("—");
}
inline QString utc(const QDateTime& t) {
    return t.isValid() ? t.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'")) : QStringLiteral("—");
}
inline QString age_text(Value days) {
    if (!days)
        return {};
    const double hours = *days * 24.0;
    if (hours < 1)
        return QStringLiteral("<1 h ago");
    if (hours < 48)
        return QString::number(std::round(hours)) + QStringLiteral(" h ago");
    return QString::number(std::round(*days)) + QStringLiteral(" days ago");
}
inline QString state_label(const QString& state) {
    if (state == QLatin1String("CURRENT"))
        return QStringLiteral("current");
    if (state == QLatin1String("STALE"))
        return QStringLiteral("stale");
    if (state == QLatin1String("HISTORICAL"))
        return QStringLiteral("historical");
    return QStringLiteral("unavailable");
}
/// A lookback change, naming its reference date when that is older than the
/// nominal lookback (the approved rule uses the latest observation at or
/// before it, which may be days earlier).
inline QString lookback(Value change, const QDateTime& reference, const QDateTime& latest, int days) {
    QString text = pp(change);
    if (change && reference.isValid() && latest.isValid() && reference.daysTo(latest) > days + 1)
        text += QStringLiteral(" (since ") + reference.toUTC().toString(QStringLiteral("MMM d")) + ")";
    return text;
}

} // namespace fincept::screens::fedwatch
