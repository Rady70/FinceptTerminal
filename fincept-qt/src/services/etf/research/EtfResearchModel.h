// src/services/etf/research/EtfResearchModel.h
//
// MarketLab ETF Flow & Sector Rotation (consolidated Batch E): the shared
// vocabulary of every research result the workspace shows.
//
// Every displayed result carries
//   * an evidence class — MEASURED, ESTIMATED, PROXY, MODEL or UNAVAILABLE —
//     that says what KIND of statement it is, and
//   * for calculated results, a credibility grade — HIGH, MEDIUM, LOW or
//     EXPERIMENTAL — derived by grade_credibility() from inspectable
//     conditions, each of which is recorded as a reason.
//
// Distinct states are never collapsed: no observation, an actual zero, stale,
// revised, partial, source failure, fallback, disagreement and insufficient
// history are separate flags. A value without a number is UNAVAILABLE and
// says why; it is never shown as zero. A proxy or model can never become
// MEASURED: the evidence class is fixed by the method that produced the value.
//
// Header-only over Qt Core so every calculation can be unit-tested without the
// database, the network or the UI.
#pragma once
#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fincept::services::etf::research {

inline constexpr const char* kResearchEngineVersion = "etf_research_engine_v1";

// ── Evidence class ───────────────────────────────────────────────────────────
enum class EvidenceClass { Measured, Estimated, Proxy, Model, Unavailable };

inline const char* evidence_id(EvidenceClass e) {
    switch (e) {
        case EvidenceClass::Measured:
            return "MEASURED";
        case EvidenceClass::Estimated:
            return "ESTIMATED";
        case EvidenceClass::Proxy:
            return "PROXY";
        case EvidenceClass::Model:
            return "MODEL";
        case EvidenceClass::Unavailable:
            return "UNAVAILABLE";
    }
    return "";
}

/// Short, colour-independent tag for dense tables.
inline const char* evidence_tag(EvidenceClass e) {
    switch (e) {
        case EvidenceClass::Measured:
            return "MEAS";
        case EvidenceClass::Estimated:
            return "EST";
        case EvidenceClass::Proxy:
            return "PRXY";
        case EvidenceClass::Model:
            return "MODL";
        case EvidenceClass::Unavailable:
            return "N/A";
    }
    return "";
}

// ── Credibility ──────────────────────────────────────────────────────────────
// Ordered strongest first. NotGraded is for raw source observations (their
// strength is expressed by timing/freshness flags, not a calculation grade)
// and for unavailable values.
enum class Credibility { High = 3, Medium = 2, Low = 1, Experimental = 0, NotGraded = -1 };

inline const char* credibility_id(Credibility c) {
    switch (c) {
        case Credibility::High:
            return "HIGH";
        case Credibility::Medium:
            return "MEDIUM";
        case Credibility::Low:
            return "LOW";
        case Credibility::Experimental:
            return "EXPERIMENTAL";
        case Credibility::NotGraded:
            return "";
    }
    return "";
}

inline const char* credibility_tag(Credibility c) {
    switch (c) {
        case Credibility::High:
            return "HI";
        case Credibility::Medium:
            return "MED";
        case Credibility::Low:
            return "LOW";
        case Credibility::Experimental:
            return "EXP";
        case Credibility::NotGraded:
            return "";
    }
    return "";
}

/// A condition that lowers a calculated result's credibility. Each one is an
/// observable fact about the inputs or the method, recorded verbatim.
enum class CredCondition {
    Stale,                 ///< an input is older than its freshness rule: capped at LOW
    Partial,               ///< some members/inputs of the value are missing: one step down
    Fallback,              ///< a declared fallback input replaced the primary: one step down
    EstimatorDisagreement, ///< independent estimates disagree beyond tolerance: capped at EXPERIMENTAL
    SourceTimingAmbiguous, ///< the source does not date the observation; a rule assigned it: one step down
    InputRevised,          ///< an input vintage was revised (kept, not a downgrade by itself)
    ShortHistory,          ///< a statistic uses less than its full lookback: one step down
    CrossSourceDisagree,   ///< a second source for the same quantity disagrees: one step down
    MacroStale,            ///< a slow macro input is old relative to its release cadence: one step down
    Unvalidated,           ///< the mapping/threshold has no validation evidence in MarketLab: capped at EXPERIMENTAL
    HeuristicThreshold,    ///< labels come from fixed heuristic thresholds: capped at LOW
    InSampleModel,         ///< model parameters were fitted on the same sample they label: capped at LOW
};

inline const char* cred_condition_id(CredCondition c) {
    switch (c) {
        case CredCondition::Stale:
            return "stale_input";
        case CredCondition::Partial:
            return "partial_inputs";
        case CredCondition::Fallback:
            return "fallback_input";
        case CredCondition::EstimatorDisagreement:
            return "estimator_disagreement";
        case CredCondition::SourceTimingAmbiguous:
            return "source_timing_ambiguous";
        case CredCondition::InputRevised:
            return "input_revised";
        case CredCondition::ShortHistory:
            return "short_history";
        case CredCondition::CrossSourceDisagree:
            return "cross_source_disagreement";
        case CredCondition::MacroStale:
            return "macro_input_stale";
        case CredCondition::Unvalidated:
            return "unvalidated_mapping";
        case CredCondition::HeuristicThreshold:
            return "heuristic_threshold";
        case CredCondition::InSampleModel:
            return "in_sample_model";
    }
    return "";
}

inline Credibility cred_step_down(Credibility c) {
    switch (c) {
        case Credibility::High:
            return Credibility::Medium;
        case Credibility::Medium:
            return Credibility::Low;
        default:
            return Credibility::Experimental;
    }
}

inline Credibility cred_min(Credibility a, Credibility b) {
    return static_cast<int>(a) <= static_cast<int>(b) ? a : b;
}

/// Grade a calculated result: start from the method's base grade and apply
/// every condition. The reasons list records the base and each condition in
/// order, so the UI can show exactly why a grade is what it is.
inline Credibility grade_credibility(Credibility base, const QVector<CredCondition>& conditions, QStringList* reasons) {
    Credibility c = base;
    if (base == Credibility::NotGraded) {
        // A source observation is not graded; its conditions are recorded only.
        if (reasons)
            for (CredCondition k : conditions)
                reasons->append(QStringLiteral("%1:observation").arg(QLatin1String(cred_condition_id(k))));
        return base;
    }
    if (reasons)
        reasons->append(QStringLiteral("base:%1").arg(QLatin1String(credibility_id(base))));
    for (CredCondition k : conditions) {
        const Credibility before = c;
        switch (k) {
            case CredCondition::Stale:
                c = cred_min(c, Credibility::Low);
                break;
            case CredCondition::EstimatorDisagreement:
            case CredCondition::Unvalidated:
                c = Credibility::Experimental;
                break;
            case CredCondition::HeuristicThreshold:
            case CredCondition::InSampleModel:
                c = cred_min(c, Credibility::Low);
                break;
            case CredCondition::InputRevised:
                break;
            case CredCondition::Partial:
            case CredCondition::Fallback:
            case CredCondition::SourceTimingAmbiguous:
            case CredCondition::ShortHistory:
            case CredCondition::CrossSourceDisagree:
            case CredCondition::MacroStale:
                c = cred_step_down(c);
                break;
        }
        if (reasons)
            reasons->append(QStringLiteral("%1:%2->%3")
                                .arg(QLatin1String(cred_condition_id(k)), QLatin1String(credibility_id(before)),
                                     QLatin1String(credibility_id(c))));
    }
    return c;
}

// ── Value flags ──────────────────────────────────────────────────────────────
// Flags are stable ids (provenance, never translated). The UI maps them to
// labels and tooltips.
namespace flag {
inline constexpr const char* kNoObservation = "NO_OBSERVATION";
inline constexpr const char* kStale = "STALE";
inline constexpr const char* kRevised = "REVISED";
inline constexpr const char* kPartial = "PARTIAL";
inline constexpr const char* kSourceFailure = "SOURCE_FAILURE";
inline constexpr const char* kFallback = "FALLBACK";
inline constexpr const char* kDisagreement = "DISAGREEMENT";
inline constexpr const char* kInsufficientHistory = "INSUFFICIENT_HISTORY";
/// The series restarted at a session whose close jumped by more than 3x (or fell
/// below 1/3) with no recorded distribution: an unadjusted corporate action is
/// suspected, so no window spans it.
inline constexpr const char* kHistoryRestarted = "HISTORY_RESTARTED";
inline constexpr const char* kNotApplicable = "NOT_APPLICABLE";
inline constexpr const char* kAssumedDate = "ASSUMED_EFFECTIVE_DATE";
inline constexpr const char* kGap = "CAPTURE_GAP";
inline constexpr const char* kActualZero = "ACTUAL_ZERO";
inline constexpr const char* kLowConfidence = "LOW_CONFIDENCE_ESTIMATE";
inline constexpr const char* kExperimental = "EXPERIMENTAL_MODEL";
inline constexpr const char* kMonthly = "MONTHLY";
} // namespace flag

/// One research result. `value` is set only for a usable number; an
/// UNAVAILABLE result has no value and a `reason`.
struct ResearchValue {
    std::optional<double> value;
    EvidenceClass evidence = EvidenceClass::Unavailable;
    Credibility credibility = Credibility::NotGraded;
    QString units;     ///< pct | pp | bp | usd | ratio | z | index | count | label
    QString method;    ///< method id with version
    QString source;    ///< source id(s)
    QDate effective;   ///< the observation / effective date the value describes
    QString label;     ///< categorical results (RRG quadrant, regime label)
    QString reason;    ///< why UNAVAILABLE (stable id)
    QStringList flags; ///< kNoObservation, kStale, ...
    QStringList credibility_reasons;

    bool usable() const { return value.has_value() || !label.isEmpty(); }

    bool has_flag(const char* f) const { return flags.contains(QLatin1String(f)); }

    void add_flag(const char* f) {
        if (!has_flag(f))
            flags.append(QLatin1String(f));
    }

    static ResearchValue unavailable(const QString& why, const QString& method = QString()) {
        ResearchValue v;
        v.evidence = EvidenceClass::Unavailable;
        v.reason = why;
        v.method = method;
        return v;
    }

    QJsonObject to_json() const {
        QJsonObject o;
        o.insert(QStringLiteral("value"), value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null));
        o.insert(QStringLiteral("evidence"), QLatin1String(evidence_id(evidence)));
        if (credibility != Credibility::NotGraded)
            o.insert(QStringLiteral("credibility"), QLatin1String(credibility_id(credibility)));
        if (!units.isEmpty())
            o.insert(QStringLiteral("units"), units);
        if (!method.isEmpty())
            o.insert(QStringLiteral("method"), method);
        if (!source.isEmpty())
            o.insert(QStringLiteral("source"), source);
        if (effective.isValid())
            o.insert(QStringLiteral("effective"), effective.toString(Qt::ISODate));
        if (!label.isEmpty())
            o.insert(QStringLiteral("label"), label);
        if (!reason.isEmpty())
            o.insert(QStringLiteral("reason"), reason);
        if (!flags.isEmpty())
            o.insert(QStringLiteral("flags"), QJsonArray::fromStringList(flags));
        if (!credibility_reasons.isEmpty())
            o.insert(QStringLiteral("credibility_reasons"), QJsonArray::fromStringList(credibility_reasons));
        return o;
    }
};

/// Build a graded calculated value in one call.
inline ResearchValue graded(double v, EvidenceClass e, Credibility base, const QVector<CredCondition>& conds,
                            const QString& units, const QString& method, const QString& source,
                            const QDate& effective) {
    ResearchValue r;
    r.value = v;
    r.evidence = e;
    r.units = units;
    r.method = method;
    r.source = source;
    r.effective = effective;
    r.credibility = grade_credibility(base, conds, &r.credibility_reasons);
    for (CredCondition c : conds) {
        if (c == CredCondition::Stale)
            r.add_flag(flag::kStale);
        else if (c == CredCondition::Partial)
            r.add_flag(flag::kPartial);
        else if (c == CredCondition::Fallback)
            r.add_flag(flag::kFallback);
        else if (c == CredCondition::EstimatorDisagreement)
            r.add_flag(flag::kDisagreement);
        else if (c == CredCondition::InputRevised)
            r.add_flag(flag::kRevised);
        else if (c == CredCondition::SourceTimingAmbiguous)
            r.add_flag(flag::kAssumedDate);
    }
    if (v == 0.0)
        r.add_flag(flag::kActualZero);
    if (r.credibility == Credibility::Experimental)
        r.add_flag(e == EvidenceClass::Estimated ? flag::kLowConfidence : flag::kExperimental);
    return r;
}

} // namespace fincept::services::etf::research
