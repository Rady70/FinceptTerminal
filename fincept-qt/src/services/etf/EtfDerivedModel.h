// src/services/etf/EtfDerivedModel.h
//
// ETF Capital Flows, Batch C: the shared vocabulary of derived values
// (control repository docs/ETF_FLOW_BATCH_C_IMPLEMENTATION.md).
//
// A derived value is a pure, versioned function of stored source vintages
// (migration v052, EtfReadModel.h) and of the time frame it is computed for.
// Nothing derived is stored: v052 refuses the calculated and proxy kinds as
// source facts, and every derived value can be recomputed exactly from the
// append-only vintage store. The time frame has two axes:
//
//   * `as_of`, the decision time: only a vintage whose `available_from` is at
//     or before it may be used (A2 section 5.5), so no rolling window, revision
//     or amendment can reach into the future;
//   * `known_at`, the knowledge cutoff: only vintages MarketLab had recorded
//     (`first_seen_at`) by then exist. A later backfill or revision therefore
//     cannot change a result recomputed for an earlier (as_of, known_at).
//
// A derived value is never stronger than its weakest input: it is available
// from the latest `available_from` of its inputs, and its point-in-time status
// is the weakest of theirs (observed > conservative_rule >
// historical_assumption > not_point_in_time; a rule and an assumption never
// meet in one calculation, because the regulatory and the rotation families
// read different sources).
//
// States reuse the Batch B quality vocabulary (EtfDataModel.h):
//   * a usable value carries its family's state, CONFIRMED for analytics of
//     SEC regulatory flow and PROXY for market-rotation measures, or REVISED
//     when a selected input vintage differs from an earlier vintage of its key
//     (the Batch B derivation, EtfReadModel.h). The family itself is always
//     named by `measurement_kind`, so REVISED never hides what a value is;
//   * an unusable value has no number and one reason from a closed set, which
//     also decides its state (MISSING, STALE, NOT_APPLICABLE or
//     ROUTE_DISABLED). Unknown is never zero.
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfDataModel.h"
#include "services/etf/EtfReadModel.h"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <cmath>
#include <optional>

namespace fincept::services::etf {

// ── Time frame ───────────────────────────────────────────────────────────────

struct DerivedTimeFrame {
    QDateTime as_of;    ///< decision time (UTC): available_from <= as_of
    QDateTime known_at; ///< knowledge cutoff (UTC): first_seen_at <= known_at

    bool valid() const { return as_of.isValid() && known_at.isValid(); }
};

/// The vintages of one key that existed at the knowledge cutoff.
inline QVector<StoredObservation> known_vintages(const QVector<StoredObservation>& key_vintages,
                                                 const QDateTime& known_at) {
    QVector<StoredObservation> out;
    for (const StoredObservation& v : key_vintages) {
        if (known_at.isValid() && v.first_seen_at.isValid() && v.first_seen_at <= known_at)
            out.append(v);
    }
    return out;
}

// ── Point-in-time status of a derived value ──────────────────────────────────

inline std::optional<PointInTimeStatus> point_in_time_status_from_id(const QString& id) {
    for (auto p : {PointInTimeStatus::Observed, PointInTimeStatus::ConservativeRule,
                   PointInTimeStatus::HistoricalAssumption, PointInTimeStatus::NotPointInTime}) {
        if (id == QLatin1String(point_in_time_status_id(p)))
            return p;
    }
    return std::nullopt;
}

inline int point_in_time_strength(PointInTimeStatus p) {
    switch (p) {
        case PointInTimeStatus::Observed:
            return 3;
        case PointInTimeStatus::ConservativeRule:
            return 2;
        case PointInTimeStatus::HistoricalAssumption:
            return 1;
        case PointInTimeStatus::NotPointInTime:
            return 0;
    }
    return 0;
}

inline PointInTimeStatus weaker_point_in_time(PointInTimeStatus a, PointInTimeStatus b) {
    return point_in_time_strength(a) <= point_in_time_strength(b) ? a : b;
}

// ── Reasons and states ───────────────────────────────────────────────────────

/// Why a derived value has no number. The reason decides the state; a usable
/// value has reason None.
enum class DerivedReason {
    None,
    InsufficientHistory,         ///< the window reaches before the first available observation (launch, short history)
    WindowIncomplete,            ///< a required period inside the stored history has no usable input
    MonthNotAvailable,           ///< no filing that reports the month is available at as_of
    ComponentMissing,            ///< the selected filing reports a flow component as missing or unparseable
    DenominatorUnavailable,      ///< no regulatory net assets at an eligible report date are available
    DenominatorMissingValue,     ///< the eligible net-assets vintage holds no number
    InvalidDenominator,          ///< a denominator is zero or negative
    SessionBarMissing,           ///< a required session inside the stored history has no available bar
    ValueMissing,                ///< the selected vintage holds no number
    InvalidInput,                ///< a price that is not positive, or a non-finite result
    EarlyCloseSessionExcluded,   ///< a volume measure on an early-close session (not a full session)
    ReferenceUnavailable,        ///< the declared reference has no available bar at a required session
    ReturnBasisNotComparable,    ///< subject and reference prices are not on one units/basis
    SubjectIsReference,          ///< a relative measure of the reference against itself
    LatestSessionBeforeExpected, ///< the latest available session is older than the freshness rule expects
    D1dNoPermissionBasis,        ///< calculated daily creation/redemption flow: the route is disabled (D1-d)
};

inline const char* derived_reason_id(DerivedReason r) {
    switch (r) {
        case DerivedReason::None:
            return "";
        case DerivedReason::InsufficientHistory:
            return "insufficient_history";
        case DerivedReason::WindowIncomplete:
            return "window_incomplete";
        case DerivedReason::MonthNotAvailable:
            return "month_not_available";
        case DerivedReason::ComponentMissing:
            return "component_missing";
        case DerivedReason::DenominatorUnavailable:
            return "denominator_unavailable";
        case DerivedReason::DenominatorMissingValue:
            return "denominator_missing_value";
        case DerivedReason::InvalidDenominator:
            return "invalid_denominator";
        case DerivedReason::SessionBarMissing:
            return "session_bar_missing";
        case DerivedReason::ValueMissing:
            return "value_missing";
        case DerivedReason::InvalidInput:
            return "invalid_input";
        case DerivedReason::EarlyCloseSessionExcluded:
            return "early_close_session_excluded";
        case DerivedReason::ReferenceUnavailable:
            return "reference_unavailable";
        case DerivedReason::ReturnBasisNotComparable:
            return "return_basis_not_comparable";
        case DerivedReason::SubjectIsReference:
            return "subject_is_reference";
        case DerivedReason::LatestSessionBeforeExpected:
            return "latest_session_before_expected";
        case DerivedReason::D1dNoPermissionBasis:
            return "d1d_no_permission_basis";
    }
    return "";
}

/// The state an unusable value takes for its reason.
inline QualityState derived_reason_state(DerivedReason r) {
    switch (r) {
        case DerivedReason::EarlyCloseSessionExcluded:
        case DerivedReason::ReturnBasisNotComparable:
        case DerivedReason::SubjectIsReference:
            return QualityState::NotApplicable;
        case DerivedReason::LatestSessionBeforeExpected:
            return QualityState::Stale;
        case DerivedReason::D1dNoPermissionBasis:
            return QualityState::RouteDisabled;
        default:
            return QualityState::Missing;
    }
}

// ── Inputs of one derived value ──────────────────────────────────────────────

/// One selected source vintage, as a derived value uses it.
struct SelectedInput {
    bool present = false; ///< a vintage was available at as_of
    qint64 observation_id = 0;
    FieldValue value;
    QualityState quality = QualityState::Missing; ///< CONFIRMED / REVISED / MISSING (Batch B derivation)
    QDateTime available_from;
    PointInTimeStatus point_in_time_status = PointInTimeStatus::NotPointInTime;
    QString revision_state;
    QString units;
    QString basis;
    QString source_document; ///< SEC accession; empty for IBKR
    QDateTime accepted_at;   ///< SEC only
    int vintages_known = 0;  ///< vintages of the key that existed at known_at

    bool usable_number() const { return present && value.reported() && std::isfinite(value.value); }
};

/// Select the vintage of one key a derived value may use: among the vintages
/// known at `tf.known_at`, the newest available at `tf.as_of`.
inline SelectedInput select_input(const QVector<StoredObservation>& key_vintages, const DerivedTimeFrame& tf) {
    SelectedInput s;
    const QVector<StoredObservation> known = known_vintages(key_vintages, tf.known_at);
    s.vintages_known = static_cast<int>(known.size());
    const qsizetype idx = vintage_as_of_index(known, tf.as_of);
    if (idx < 0)
        return s;
    const StoredObservation& v = known[idx];
    s.present = true;
    s.observation_id = v.observation_id;
    s.value = v.value;
    s.quality = derived_quality(known, idx);
    s.available_from = v.available_from;
    s.point_in_time_status =
        point_in_time_status_from_id(v.point_in_time_status).value_or(PointInTimeStatus::NotPointInTime);
    s.revision_state = v.revision_state;
    s.units = v.units;
    s.basis = v.basis;
    s.source_document = v.source_document;
    s.accepted_at = v.accepted_at;
    return s;
}

/// What a derived value inherits from the inputs it used.
struct InputAccumulator {
    QDateTime available_from; ///< the latest available_from of the inputs
    PointInTimeStatus point_in_time_status = PointInTimeStatus::Observed;
    bool revised = false;
    int count = 0;
    QDate first; ///< earliest effective date among the inputs
    QDate last;  ///< latest effective date among the inputs

    void add(const SelectedInput& in, const QDate& effective) {
        ++count;
        if (!available_from.isValid() || (in.available_from.isValid() && in.available_from > available_from))
            available_from = in.available_from;
        if (!in.available_from.isValid())
            point_in_time_status = PointInTimeStatus::NotPointInTime;
        point_in_time_status = weaker_point_in_time(point_in_time_status, in.point_in_time_status);
        revised = revised || in.quality == QualityState::Revised;
        if (effective.isValid()) {
            if (!first.isValid() || effective < first)
                first = effective;
            if (!last.isValid() || effective > last)
                last = effective;
        }
    }

    void merge(const InputAccumulator& o) {
        if (o.count == 0)
            return;
        count += o.count;
        if (!available_from.isValid() || (o.available_from.isValid() && o.available_from > available_from))
            available_from = o.available_from;
        point_in_time_status = weaker_point_in_time(point_in_time_status, o.point_in_time_status);
        revised = revised || o.revised;
        if (o.first.isValid() && (!first.isValid() || o.first < first))
            first = o.first;
        if (o.last.isValid() && (!last.isValid() || o.last > last))
            last = o.last;
    }
};

// ── A derived value ──────────────────────────────────────────────────────────

struct DerivedValue {
    std::optional<double> value;
    QualityState state = QualityState::Missing;
    DerivedReason reason = DerivedReason::InsufficientHistory;
    QDateTime available_from; ///< invalid when unusable
    PointInTimeStatus point_in_time_status = PointInTimeStatus::NotPointInTime;
    QDate window_first; ///< effective date of the earliest input (invalid when unusable)
    QDate window_last;  ///< effective date of the latest input
    int input_count = 0;

    bool usable() const { return value.has_value(); }

    /// A usable value of a family whose class state is `family_state`
    /// (CONFIRMED or PROXY); REVISED when an input was revised.
    static DerivedValue computed(double v, const InputAccumulator& in, QualityState family_state) {
        if (!std::isfinite(v))
            return unavailable(DerivedReason::InvalidInput);
        DerivedValue d;
        d.value = v;
        d.state = in.revised ? QualityState::Revised : family_state;
        d.reason = DerivedReason::None;
        d.available_from = in.available_from;
        d.point_in_time_status =
            in.available_from.isValid() ? in.point_in_time_status : PointInTimeStatus::NotPointInTime;
        d.window_first = in.first;
        d.window_last = in.last;
        d.input_count = in.count;
        return d;
    }

    static DerivedValue unavailable(DerivedReason r) {
        DerivedValue d;
        d.reason = r;
        d.state = derived_reason_state(r);
        return d;
    }
};

inline QJsonObject derived_value_json(const DerivedValue& d, const QString& measurement_kind, const QString& units) {
    QJsonObject o;
    o.insert(QStringLiteral("value"), d.value ? QJsonValue(*d.value) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("state"), QLatin1String(quality_state_id(d.state)));
    o.insert(QStringLiteral("reason"), QLatin1String(derived_reason_id(d.reason)));
    o.insert(QStringLiteral("measurement_kind"), measurement_kind);
    o.insert(QStringLiteral("units"), units);
    if (d.usable()) {
        o.insert(QStringLiteral("available_from"),
                 d.available_from.isValid()
                     ? QJsonValue(d.available_from.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'")))
                     : QJsonValue(QJsonValue::Null));
        o.insert(QStringLiteral("point_in_time_status"),
                 QLatin1String(point_in_time_status_id(d.point_in_time_status)));
        o.insert(QStringLiteral("window_first"), d.window_first.toString(Qt::ISODate));
        o.insert(QStringLiteral("window_last"), d.window_last.toString(Qt::ISODate));
        o.insert(QStringLiteral("input_count"), d.input_count);
    }
    return o;
}

inline QString derived_time_text(const QDateTime& t) {
    return t.isValid() ? t.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'")) : QString();
}

} // namespace fincept::services::etf
