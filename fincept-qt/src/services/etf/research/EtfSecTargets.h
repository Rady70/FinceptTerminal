#pragma once
// EtfSecTargets — which SEC N-PORT reporting identities the ETF research
// refresh reads.
//
// The refresh used to read only identities already stored in
// etf_reporting_entities, and an identity is stored only when its first
// filing is ingested — which only the headless `--etf-data sec-nport --cik`
// command did. A profile where nobody ran that command read no SEC data at
// all, although the research universe declares SEC identities (with
// evidence) for its funds. The refresh now reads the stored identities plus
// every declared identity not stored yet; the first read stores it.
#include "services/etf/research/EtfResearchUniverse.h"
#include "storage/repositories/EtfDataRepository.h"

#include <QSet>
#include <QString>
#include <QVector>

namespace fincept::services::etf::research {

struct SecTarget {
    QString cik;          ///< as stored, or the declared 10-digit CIK
    QString series_id;    ///< empty for a registrant that reports without series
    qint64 entity_id = 0; ///< 0 = declared, not stored yet
    QString symbol;       ///< the declaring fund ('' for a stored identity)
};

/// A CIK as 10 digits (leading zeros), for comparing stored and declared CIKs.
inline QString sec_cik10(const QString& cik) {
    QString digits;
    for (const QChar c : cik)
        if (c.isDigit())
            digits.append(c);
    return digits.rightJustified(10, QLatin1Char('0'));
}

/// Stored identities first (in their order), then each declared identity of
/// `instruments` that is not stored, once.
inline QVector<SecTarget> sec_targets(const QVector<etf_store::ReportingEntityRow>& stored,
                                      const QVector<UniverseInstrument>& instruments) {
    QVector<SecTarget> out;
    QSet<QString> seen;
    auto key = [](const QString& cik, const QString& series) { return sec_cik10(cik) + QLatin1Char('/') + series; };
    for (const auto& e : stored) {
        if (seen.contains(key(e.cik, e.series_id)))
            continue;
        seen.insert(key(e.cik, e.series_id));
        out.append({e.cik, e.series_id, e.entity_id, QString()});
    }
    for (const auto& inst : instruments)
        for (const auto& id : inst.sec_reporting) {
            const QString k = key(id.cik, id.series_id);
            if (seen.contains(k))
                continue;
            seen.insert(k);
            out.append({sec_cik10(id.cik), id.series_id, 0, inst.symbol});
        }
    return out;
}

/// A first read of a declared identity asks for this many filings (about
/// two years of public N-PORT reports); later reads ask only for what is new.
inline constexpr int kSecFirstReadFilings = 8;
/// At most this many declared identities are read for the first time in one
/// refresh, so seeding many funds is spread over several days instead of one
/// multi-hour run. Stored identities are always read.
inline constexpr int kSecNewTargetsPerRun = 16;

struct SecRunPlan {
    QVector<SecTarget> targets; ///< this refresh's reads
    int waiting = 0;            ///< declared identities left for a later refresh
};

inline SecRunPlan sec_run_plan(const QVector<SecTarget>& all, int max_new = kSecNewTargetsPerRun) {
    SecRunPlan plan;
    int fresh = 0;
    for (const auto& t : all) {
        if (t.entity_id > 0) {
            plan.targets.append(t);
        } else if (fresh < max_new) {
            plan.targets.append(t);
            ++fresh;
        } else {
            ++plan.waiting;
        }
    }
    return plan;
}

} // namespace fincept::services::etf::research
