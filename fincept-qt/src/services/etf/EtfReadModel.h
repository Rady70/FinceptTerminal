// src/services/etf/EtfReadModel.h
//
// Reading the vintages the ETF data foundation keeps (ETF Capital Flows
// Batch B). A key is (subject, measure, effective date, source); every
// retrieval that delivered a different value for a key, and every separate SEC
// filing (an amendment is a separate filing), is its own stored vintage. Nothing
// is overwritten, so these rules only choose among stored rows:
//
//   * the current vintage: for SEC, the filing accepted last (an NPORT-P/A
//     supersedes its original for "latest" reads, and the original stays);
//     for IBKR, the vintage recorded last;
//   * the vintage as of a decision time: the newest vintage whose
//     `available_from` is at or before that time (A2 section 5.5). A vintage
//     with no availability time is never used point in time;
//   * the derived quality of a selected vintage: MISSING when the source gave
//     no number, REVISED when an earlier vintage of the same key held a
//     different value, CONFIRMED otherwise. It is derived, never stored, so an
//     SEC amendment that arrives out of order cannot leave a stale flag behind.
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfDataModel.h"

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <algorithm>

namespace fincept::services::etf {

/// One stored observation vintage, as read back from etf_observations.
/// Enumerations are kept as their stored ids: this is what the database holds.
struct StoredObservation {
    qint64 observation_id = 0;
    QString subject_type;
    qint64 subject_id = 0;
    QString measurement_kind;
    QString measure;
    QString units;
    QString basis;
    QString source_type;
    QString acquisition_mode;
    QString source_document; ///< SEC accession; empty for IBKR
    qint64 filing_id = 0;    ///< 0 when NULL
    QDate effective_date;
    QDate period_start;    ///< invalid when NULL
    QDate period_end;      ///< invalid when NULL
    QDate report_period;   ///< invalid when NULL
    QDateTime accepted_at; ///< invalid when NULL
    FieldValue value;
    int source_revision = 0;
    QString revision_state;
    QString history_type;
    QString point_in_time_status;
    QString availability_basis;
    QDateTime available_from; ///< invalid when NULL
    QDateTime first_seen_at;
    QDateTime last_seen_at;
    qint64 first_retrieval_id = 0;
    qint64 last_retrieval_id = 0;
    int seen_count = 0;
};

/// Vintage order within one key. SEC vintages are separate filings and follow
/// their acceptance time; IBKR vintages follow the order MarketLab recorded
/// them (source_revision).
inline bool vintage_before(const StoredObservation& a, const StoredObservation& b) {
    if (a.source_type == QLatin1String(source_type_id(SourceType::SecNport)) && a.accepted_at.isValid() &&
        b.accepted_at.isValid() && a.accepted_at != b.accepted_at)
        return a.accepted_at < b.accepted_at;
    return a.source_revision < b.source_revision;
}

inline QVector<StoredObservation> vintages_in_order(QVector<StoredObservation> key_vintages) {
    std::stable_sort(key_vintages.begin(), key_vintages.end(), vintage_before);
    return key_vintages;
}

/// Index of the current vintage in `key_vintages`, or -1 when empty.
inline qsizetype current_vintage_index(const QVector<StoredObservation>& key_vintages) {
    qsizetype best = -1;
    for (qsizetype i = 0; i < key_vintages.size(); ++i) {
        if (best < 0 || vintage_before(key_vintages[best], key_vintages[i]))
            best = i;
    }
    return best;
}

/// Index of the newest vintage available at `decision_time`, or -1 when none
/// was. Vintages without an availability time are skipped (not point in time).
inline qsizetype vintage_as_of_index(const QVector<StoredObservation>& key_vintages, const QDateTime& decision_time) {
    qsizetype best = -1;
    for (qsizetype i = 0; i < key_vintages.size(); ++i) {
        const StoredObservation& v = key_vintages[i];
        if (!v.available_from.isValid() || !decision_time.isValid() || v.available_from > decision_time)
            continue;
        if (best < 0 || vintage_before(key_vintages[best], v))
            best = i;
    }
    return best;
}

/// The derived quality of `key_vintages[selected]`, judged only against the
/// vintages that precede it (a later revision does not change what an earlier
/// selection was).
inline QualityState derived_quality(const QVector<StoredObservation>& key_vintages, qsizetype selected) {
    if (selected < 0 || selected >= key_vintages.size())
        return QualityState::Missing;
    const StoredObservation& s = key_vintages[selected];
    if (!s.value.reported())
        return QualityState::Missing;
    for (qsizetype i = 0; i < key_vintages.size(); ++i) {
        if (i == selected || !vintage_before(key_vintages[i], s))
            continue;
        if (!key_vintages[i].value.same_value(s.value))
            return QualityState::Revised;
    }
    return QualityState::Confirmed;
}

} // namespace fincept::services::etf
