// MarketLab ETF research taxonomy, Batch D. Project-owned, reviewed JSON only.
// A SEC reporting entity and an IBKR listed instrument are independent identities;
// taxonomy membership never creates an identity link between them.
#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fincept::services::etf {

enum class TaxonomySubjectType { Listed, Reporting };
enum class TaxonomyStatus { Classified, UnknownIdentity, HistoryUnverified };

struct TaxonomyEntry {
    TaxonomySubjectType subject_type = TaxonomySubjectType::Listed;
    qint64 con_id = 0;   // IBKR conId, never a ticker or local DB id
    QString cik10;       // SEC ten-digit CIK; exact with series_id
    QString series_id;   // empty only for registrant-level identity
    QString ticker_hint; // review aid only; never used in lookup
    QString asset_class;
    QString category;
    QString complex_id;         // empty means no defensible exposure family
    QString exposure_mechanism; // physical, equity, futures, spot_crypto, etc.
    QString fund_structure;     // open_end_etf, unit_investment_trust, etc.
    QString region;
    QString reference;      // index/underlying name, not a data-series join
    QString currency_hedge; // unhedged, hedged, not_applicable, unknown
    bool leveraged = false;
    bool inverse = false;
    bool option_overlay = false;
    // Classification code: +1 means no explicit leveraged/inverse target.
    // Otherwise this is the product's signed daily target, not realized beta.
    double leverage_multiple = 1.0;
    QDate effective_from; // first date for which evidence supports use
    QDate effective_to;   // inclusive; invalid means open-ended
    QString evidence_url; // dated issuer or regulatory provenance
};

struct TaxonomyLookup {
    TaxonomyStatus status = TaxonomyStatus::UnknownIdentity;
    std::optional<TaxonomyEntry> entry;
};

class TaxonomySnapshot {
  public:
    static std::optional<TaxonomySnapshot> load(const QByteArray& json, QString* error = nullptr) {
        auto fail = [&](const QString& why) -> std::optional<TaxonomySnapshot> {
            if (error)
                *error = why;
            return std::nullopt;
        };
        QJsonParseError parse_error;
        const QJsonDocument doc = QJsonDocument::fromJson(json, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject())
            return fail(QStringLiteral("invalid_json"));
        const QJsonObject root = doc.object();
        const QString version = root.value(QStringLiteral("version")).toString();
        static const QRegularExpression kVersion(QStringLiteral("^etf-taxonomy-v[1-9][0-9]*$"));
        if (!kVersion.match(version).hasMatch())
            return fail(QStringLiteral("invalid_version"));
        if (!root.value(QStringLiteral("entries")).isArray())
            return fail(QStringLiteral("entries_missing"));
        TaxonomySnapshot snapshot;
        snapshot.version_ = version;
        snapshot.sha256_ = QString::fromLatin1(QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex());
        const QJsonArray rows = root.value(QStringLiteral("entries")).toArray();
        if (rows.isEmpty())
            return fail(QStringLiteral("empty_taxonomy"));
        static const QRegularExpression kId(QStringLiteral("^[a-z][a-z0-9_]*$"));
        static const QRegularExpression kCik(QStringLiteral("^[0-9]{10}$"));
        static const QRegularExpression kSeries(QStringLiteral("^S[0-9]{9}$"));
        static const QSet<QString> kMechanisms = {
            QStringLiteral("equity_index"),      QStringLiteral("sector_equity"),
            QStringLiteral("producer_equity"),   QStringLiteral("crypto_equity"),
            QStringLiteral("bond_portfolio"),    QStringLiteral("reit_equity"),
            QStringLiteral("currency_futures"),  QStringLiteral("physical_commodity"),
            QStringLiteral("futures_commodity"), QStringLiteral("spot_crypto"),
            QStringLiteral("option_overlay"),    QStringLiteral("leveraged_derivative"),
            QStringLiteral("inverse_derivative")};
        static const QSet<QString> kStructures = {
            QStringLiteral("open_end_etf"),   QStringLiteral("unit_investment_trust"), QStringLiteral("grantor_trust"),
            QStringLiteral("commodity_pool"), QStringLiteral("partnership"),           QStringLiteral("etn"),
            QStringLiteral("other")};
        static const QSet<QString> kHedge = {QStringLiteral("hedged"), QStringLiteral("unhedged"),
                                             QStringLiteral("not_applicable"), QStringLiteral("unknown")};
        for (qsizetype i = 0; i < rows.size(); ++i) {
            if (!rows.at(i).isObject())
                return fail(QStringLiteral("entry_not_object:%1").arg(i));
            const QJsonObject row = rows.at(i).toObject();
            TaxonomyEntry e;
            const QString type = row.value(QStringLiteral("subject_type")).toString();
            if (type == QLatin1String("listed")) {
                e.subject_type = TaxonomySubjectType::Listed;
                const QJsonValue con = row.value(QStringLiteral("con_id"));
                if (!con.isDouble() || con.toDouble() <= 0 || con.toDouble() > 9007199254740991.0 ||
                    con.toDouble() != static_cast<qint64>(con.toDouble()))
                    return fail(QStringLiteral("invalid_con_id:%1").arg(i));
                e.con_id = static_cast<qint64>(con.toDouble());
                if (row.contains(QStringLiteral("cik10")) || row.contains(QStringLiteral("series_id")))
                    return fail(QStringLiteral("listed_has_reporting_key:%1").arg(i));
            } else if (type == QLatin1String("reporting")) {
                e.subject_type = TaxonomySubjectType::Reporting;
                e.cik10 = row.value(QStringLiteral("cik10")).toString();
                e.series_id = row.value(QStringLiteral("series_id")).toString();
                if (!kCik.match(e.cik10).hasMatch() ||
                    (!e.series_id.isEmpty() && !kSeries.match(e.series_id).hasMatch()) ||
                    !row.value(QStringLiteral("series_id")).isString() || row.contains(QStringLiteral("con_id")))
                    return fail(QStringLiteral("invalid_reporting_key:%1").arg(i));
            } else
                return fail(QStringLiteral("invalid_subject_type:%1").arg(i));
            e.ticker_hint = row.value(QStringLiteral("ticker_hint")).toString();
            e.asset_class = row.value(QStringLiteral("asset_class")).toString();
            e.category = row.value(QStringLiteral("category")).toString();
            e.complex_id = row.value(QStringLiteral("complex_id")).toString();
            e.exposure_mechanism = row.value(QStringLiteral("exposure_mechanism")).toString();
            e.fund_structure = row.value(QStringLiteral("fund_structure")).toString();
            e.region = row.value(QStringLiteral("region")).toString();
            e.reference = row.value(QStringLiteral("reference")).toString();
            e.currency_hedge = row.value(QStringLiteral("currency_hedge")).toString();
            e.evidence_url = row.value(QStringLiteral("evidence_url")).toString();
            e.effective_from = QDate::fromString(row.value(QStringLiteral("effective_from")).toString(), Qt::ISODate);
            const QString to = row.value(QStringLiteral("effective_to")).toString();
            e.effective_to = to.isEmpty() ? QDate() : QDate::fromString(to, Qt::ISODate);
            for (const auto& id : {e.asset_class, e.category, e.complex_id}) {
                if (!id.isEmpty() && !kId.match(id).hasMatch())
                    return fail(QStringLiteral("invalid_group_id:%1").arg(i));
            }
            if (e.asset_class.isEmpty() || e.category.isEmpty() || e.exposure_mechanism.isEmpty() ||
                e.fund_structure.isEmpty() || e.currency_hedge.isEmpty() || e.evidence_url.isEmpty() ||
                !e.effective_from.isValid() || (!to.isEmpty() && !e.effective_to.isValid()) ||
                (e.effective_to.isValid() && e.effective_to < e.effective_from))
                return fail(QStringLiteral("incomplete_entry:%1").arg(i));
            if (!kMechanisms.contains(e.exposure_mechanism) || !kStructures.contains(e.fund_structure) ||
                !kHedge.contains(e.currency_hedge))
                return fail(QStringLiteral("unknown_semantic_value:%1").arg(i));
            for (const auto* flag : {"leveraged", "inverse", "option_overlay"})
                if (!row.value(QLatin1String(flag)).isBool())
                    return fail(QStringLiteral("invalid_flag:%1:%2").arg(i).arg(QLatin1String(flag)));
            e.leveraged = row.value(QStringLiteral("leveraged")).toBool();
            e.inverse = row.value(QStringLiteral("inverse")).toBool();
            e.option_overlay = row.value(QStringLiteral("option_overlay")).toBool();
            const QJsonValue multiple = row.value(QStringLiteral("leverage_multiple"));
            if (!multiple.isDouble() || !std::isfinite(multiple.toDouble()) || multiple.toDouble() == 0.0)
                return fail(QStringLiteral("invalid_leverage_multiple:%1").arg(i));
            e.leverage_multiple = multiple.toDouble();
            if (e.inverse != (e.leverage_multiple < 0.0) || e.leveraged != (std::fabs(e.leverage_multiple) > 1.0) ||
                (!e.inverse && !e.leveraged && e.leverage_multiple != 1.0))
                return fail(QStringLiteral("leverage_semantics_mismatch:%1").arg(i));
            if (e.option_overlay != (e.exposure_mechanism == QLatin1String("option_overlay")))
                return fail(QStringLiteral("overlay_mechanism_mismatch:%1").arg(i));
            if ((e.inverse && e.exposure_mechanism != QLatin1String("inverse_derivative")) ||
                (e.leveraged && !e.inverse && e.exposure_mechanism != QLatin1String("leveraged_derivative")) ||
                ((!e.inverse && !e.leveraged) && (e.exposure_mechanism == QLatin1String("inverse_derivative") ||
                                                  e.exposure_mechanism == QLatin1String("leveraged_derivative"))))
                return fail(QStringLiteral("leverage_mechanism_mismatch:%1").arg(i));
            for (const auto& prior : snapshot.entries_) {
                if (e.category == prior.category && e.asset_class != prior.asset_class)
                    return fail(QStringLiteral("category_asset_conflict:%1").arg(i));
                if (e.category == prior.category &&
                    (e.exposure_mechanism != prior.exposure_mechanism || e.currency_hedge != prior.currency_hedge ||
                     e.leverage_multiple != prior.leverage_multiple || e.option_overlay != prior.option_overlay))
                    return fail(QStringLiteral("category_semantics_conflict:%1").arg(i));
                // One reviewed economic exposure per complex. Fund wrappers may
                // differ (SPY is a UIT, IVV an open-end fund); the reference,
                // mechanism and leverage/overlay behavior may not.
                if (!e.complex_id.isEmpty() && e.complex_id == prior.complex_id &&
                    (e.asset_class != prior.asset_class || e.category != prior.category ||
                     e.exposure_mechanism != prior.exposure_mechanism || e.reference != prior.reference ||
                     e.currency_hedge != prior.currency_hedge || e.leveraged != prior.leveraged ||
                     e.inverse != prior.inverse || e.option_overlay != prior.option_overlay ||
                     e.leverage_multiple != prior.leverage_multiple))
                    return fail(QStringLiteral("complex_semantics_conflict:%1").arg(i));
                const bool same_key = e.subject_type == prior.subject_type &&
                                      (e.subject_type == TaxonomySubjectType::Listed
                                           ? e.con_id == prior.con_id
                                           : e.cik10 == prior.cik10 && e.series_id == prior.series_id);
                if (!same_key)
                    continue;
                const QDate prior_end = prior.effective_to.isValid() ? prior.effective_to : QDate(9999, 12, 31);
                const QDate end = e.effective_to.isValid() ? e.effective_to : QDate(9999, 12, 31);
                if (e.effective_from <= prior_end && prior.effective_from <= end)
                    return fail(QStringLiteral("overlapping_identity_interval:%1").arg(i));
            }
            snapshot.entries_.push_back(e);
        }
        if (error)
            error->clear();
        return snapshot;
    }

    const QString& version() const { return version_; }
    const QString& sha256() const { return sha256_; }
    const QVector<TaxonomyEntry>& entries() const { return entries_; }

    QStringList group_ids(TaxonomySubjectType type, const QString& level, const QDate& date) const {
        QStringList ids;
        if (!date.isValid() || (level != QLatin1String("complex") && level != QLatin1String("category") &&
                                level != QLatin1String("asset_class")))
            return ids;
        for (const auto& e : entries_) {
            if (e.subject_type != type || !contains_date(e, date))
                continue;
            const QString& id = level == QLatin1String("complex")    ? e.complex_id
                                : level == QLatin1String("category") ? e.category
                                                                     : e.asset_class;
            if (!id.isEmpty() && !ids.contains(id))
                ids.push_back(id);
        }
        ids.sort();
        return ids;
    }

    TaxonomyLookup lookup_listed(qint64 con_id, const QDate& date) const {
        return lookup(TaxonomySubjectType::Listed, con_id, {}, {}, date);
    }
    TaxonomyLookup lookup_reporting(const QString& cik10, const QString& series_id, const QDate& date) const {
        return lookup(TaxonomySubjectType::Reporting, 0, cik10, series_id, date);
    }
    QVector<TaxonomyEntry> members(TaxonomySubjectType type, const QString& level, const QString& group_id,
                                   const QDate& date) const {
        QVector<TaxonomyEntry> out;
        if (!date.isValid() || (level != QLatin1String("complex") && level != QLatin1String("category") &&
                                level != QLatin1String("asset_class")))
            return out;
        for (const auto& e : entries_) {
            if (e.subject_type != type || !contains_date(e, date))
                continue;
            const QString& id = level == QLatin1String("complex")    ? e.complex_id
                                : level == QLatin1String("category") ? e.category
                                                                     : e.asset_class;
            if (!id.isEmpty() && id == group_id)
                out.push_back(e);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.subject_type == TaxonomySubjectType::Listed)
                return a.con_id < b.con_id;
            return a.cik10 == b.cik10 ? a.series_id < b.series_id : a.cik10 < b.cik10;
        });
        return out;
    }

  private:
    static bool contains_date(const TaxonomyEntry& e, const QDate& date) {
        return date.isValid() && e.effective_from <= date && (!e.effective_to.isValid() || date <= e.effective_to);
    }
    TaxonomyLookup lookup(TaxonomySubjectType type, qint64 con_id, const QString& cik, const QString& series,
                          const QDate& date) const {
        bool known = false;
        for (const auto& e : entries_) {
            if (e.subject_type != type ||
                (type == TaxonomySubjectType::Listed ? e.con_id != con_id : e.cik10 != cik || e.series_id != series))
                continue;
            known = true;
            if (contains_date(e, date))
                return {TaxonomyStatus::Classified, e};
        }
        return {known ? TaxonomyStatus::HistoryUnverified : TaxonomyStatus::UnknownIdentity, std::nullopt};
    }
    QString version_;
    QString sha256_;
    QVector<TaxonomyEntry> entries_;
};

} // namespace fincept::services::etf
