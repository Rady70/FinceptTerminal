// src/services/etf/research/EtfResearchUniverse.h
//
// The reviewed research universe (resources/etf_research_universe_v1.json,
// bundled as :/etf/research_universe_v1.json). One row per listed instrument
// keyed by its Yahoo symbol (the free research sources' key), with every role
// it plays (cross-asset, U.S. sector, theme, country, benchmark): an exposure
// that appears in several reference lists is ONE row, never duplicated.
//
// Identity domains are not joined by ticker at runtime. A row's IBKR conId
// and SEC reporting identity exist only when the resource declares them
// (copied from taxonomy v2 with their intervals and evidence).
//
// Header-only over Qt Core.
#pragma once
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace fincept::services::etf::research {

inline constexpr const char* kUniverseVersion = "etf-research-universe-v1";
inline constexpr const char* kUniverseResourcePath = ":/etf/research_universe_v1.json";

struct SecReportingIdentity {
    QString cik;
    QString series_id;
    QString relationship;
    QString identity_basis;
    QDate effective_from;
    QDate effective_to; ///< invalid: open
    QString fund_structure;
    QString evidence_url;

    bool covers(const QDate& d) const {
        return d.isValid() && d >= effective_from && (!effective_to.isValid() || d <= effective_to);
    }
};

struct UniverseInstrument {
    QString symbol;
    QString name;
    QString instrument_type; ///< etf | index
    QString asset_class;
    QString exposure;
    QString structure;
    QString structure_basis; ///< taxonomy_v2 | research_curation
    QString mechanism;
    QString region;
    QString sector;
    QString theme;
    QString country;
    QString country_region;
    QString country_type; ///< single | regional
    QString wb_code;
    QString bucket; ///< defensive | cyclical | unaligned (U.S. sectors only)
    double leverage_multiple = 1.0;
    bool leveraged = false;
    bool inverse = false;
    bool option_overlay = false;
    QString benchmark;
    QString history; ///< long | standard
    QStringList roles;
    std::optional<qint64> taxonomy_con_id;
    QString taxonomy_category;
    QVector<SecReportingIdentity> sec_reporting;
    QString note;

    bool has_role(const char* r) const { return roles.contains(QLatin1String(r)); }
    bool is_fund() const { return instrument_type == QLatin1String("etf"); }
    /// Leveraged/inverse products stay out of default long-only rotation aggregates.
    bool policy_excluded_from_aggregates() const { return leveraged || inverse; }
};

struct Basket {
    QString id;
    QString name;
    QString market;
    QString benchmark;
    QString benchmark_fallback;
    QString construction;
    double min_member_fraction = 0.5;
    QStringList members;
};

struct IntlStock {
    QString symbol;
    QString label;
    QString sector;
};

struct ResearchUniverse {
    QString version;
    QString reference_repository;
    QString reference_commit;
    QString bench_us;
    QString bench_global;
    QString bench_thailand;
    QString bench_thailand_fallback;
    QVector<UniverseInstrument> instruments;
    QVector<Basket> baskets;
    QHash<QString, QVector<IntlStock>> intl; ///< market -> stocks
    QStringList intl_markets;                ///< resource order

    const UniverseInstrument* find(const QString& symbol) const {
        for (const auto& i : instruments)
            if (i.symbol == symbol)
                return &i;
        return nullptr;
    }

    QVector<const UniverseInstrument*> with_role(const char* role) const {
        QVector<const UniverseInstrument*> out;
        for (const auto& i : instruments)
            if (i.has_role(role))
                out.append(&i);
        return out;
    }

    /// Every symbol whose daily history the research engine needs.
    QStringList history_symbols(const QString& depth) const {
        QStringList out;
        for (const auto& i : instruments)
            if (i.history == depth)
                out.append(i.symbol);
        if (depth == QLatin1String("standard")) {
            for (const auto& b : baskets)
                for (const auto& m : b.members)
                    if (!out.contains(m))
                        out.append(m);
        }
        return out;
    }
};

namespace universe_detail {
inline QDate date_or_invalid(const QJsonValue& v) {
    const QString s = v.toString();
    return s.isEmpty() ? QDate() : QDate::fromString(s, Qt::ISODate);
}
} // namespace universe_detail

/// Parse and validate a universe document. Returns nullopt with `error` set
/// on any structural problem: unknown version, duplicate symbol, a role or
/// structure outside the declared vocabulary, a U.S. sector without a bucket,
/// a basket member that is also a listed fund row, or a malformed SEC interval.
inline std::optional<ResearchUniverse> parse_universe(const QJsonObject& doc, QString* error) {
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return std::optional<ResearchUniverse>();
    };
    ResearchUniverse u;
    u.version = doc.value(QStringLiteral("version")).toString();
    if (u.version != QLatin1String(kUniverseVersion))
        return fail(QStringLiteral("unsupported universe version '%1'").arg(u.version));
    const QJsonObject ref = doc.value(QStringLiteral("reference")).toObject();
    u.reference_repository = ref.value(QStringLiteral("repository")).toString();
    u.reference_commit = ref.value(QStringLiteral("commit")).toString();
    const QJsonObject b = doc.value(QStringLiteral("benchmarks")).toObject();
    u.bench_us = b.value(QStringLiteral("us_equity")).toString();
    u.bench_global = b.value(QStringLiteral("global_equity")).toString();
    u.bench_thailand = b.value(QStringLiteral("thailand")).toString();
    u.bench_thailand_fallback = b.value(QStringLiteral("thailand_fallback")).toString();

    const QJsonObject vocab = doc.value(QStringLiteral("vocabularies")).toObject();
    QSet<QString> structures, roles, buckets;
    for (const auto& v : vocab.value(QStringLiteral("structure")).toArray())
        structures.insert(v.toString());
    for (const auto& v : vocab.value(QStringLiteral("roles")).toArray())
        roles.insert(v.toString());
    for (const auto& v : vocab.value(QStringLiteral("bucket")).toArray())
        buckets.insert(v.toString());

    QSet<QString> seen;
    for (const auto& iv : doc.value(QStringLiteral("instruments")).toArray()) {
        const QJsonObject o = iv.toObject();
        UniverseInstrument i;
        i.symbol = o.value(QStringLiteral("symbol")).toString();
        if (i.symbol.isEmpty())
            return fail(QStringLiteral("instrument without symbol"));
        if (seen.contains(i.symbol))
            return fail(QStringLiteral("duplicate instrument row %1").arg(i.symbol));
        seen.insert(i.symbol);
        i.name = o.value(QStringLiteral("name")).toString();
        i.instrument_type = o.value(QStringLiteral("instrument_type")).toString();
        i.asset_class = o.value(QStringLiteral("asset_class")).toString();
        i.exposure = o.value(QStringLiteral("exposure")).toString();
        i.structure = o.value(QStringLiteral("structure")).toString();
        if (!structures.contains(i.structure))
            return fail(QStringLiteral("%1: structure '%2' outside vocabulary").arg(i.symbol, i.structure));
        i.structure_basis = o.value(QStringLiteral("structure_basis")).toString();
        i.mechanism = o.value(QStringLiteral("mechanism")).toString();
        i.region = o.value(QStringLiteral("region")).toString();
        i.sector = o.value(QStringLiteral("sector")).toString();
        i.theme = o.value(QStringLiteral("theme")).toString();
        i.country = o.value(QStringLiteral("country")).toString();
        i.country_region = o.value(QStringLiteral("country_region")).toString();
        i.country_type = o.value(QStringLiteral("country_type")).toString();
        i.wb_code = o.value(QStringLiteral("wb_code")).toString();
        i.bucket = o.value(QStringLiteral("bucket")).toString();
        i.leverage_multiple = o.value(QStringLiteral("leverage_multiple")).toDouble(1.0);
        i.leveraged = o.value(QStringLiteral("leveraged")).toBool();
        i.inverse = o.value(QStringLiteral("inverse")).toBool();
        i.option_overlay = o.value(QStringLiteral("option_overlay")).toBool();
        i.benchmark = o.value(QStringLiteral("benchmark")).toString();
        i.history = o.value(QStringLiteral("history")).toString();
        i.note = o.value(QStringLiteral("note")).toString();
        for (const auto& r : o.value(QStringLiteral("roles")).toArray()) {
            if (!roles.contains(r.toString()))
                return fail(QStringLiteral("%1: role '%2' outside vocabulary").arg(i.symbol, r.toString()));
            i.roles.append(r.toString());
        }
        if (i.roles.isEmpty())
            return fail(QStringLiteral("%1 has no role").arg(i.symbol));
        if (i.has_role("us_sector") && !buckets.contains(i.bucket))
            return fail(QStringLiteral("%1: U.S. sector without a defensive/cyclical/unaligned bucket").arg(i.symbol));
        if (i.has_role("country") && i.country.isEmpty())
            return fail(QStringLiteral("%1: country role without a country").arg(i.symbol));
        if (i.leveraged && i.leverage_multiple <= 1.0 && !i.inverse)
            return fail(QStringLiteral("%1: leveraged flag inconsistent with its multiple").arg(i.symbol));
        const QJsonObject tax = o.value(QStringLiteral("taxonomy_v2")).toObject();
        if (!tax.isEmpty()) {
            i.taxonomy_con_id = static_cast<qint64>(tax.value(QStringLiteral("con_id")).toDouble());
            i.taxonomy_category = tax.value(QStringLiteral("category")).toString();
        }
        for (const auto& sv : o.value(QStringLiteral("sec_reporting")).toArray()) {
            const QJsonObject s = sv.toObject();
            SecReportingIdentity id;
            id.cik = s.value(QStringLiteral("cik")).toString();
            id.series_id = s.value(QStringLiteral("series_id")).toString();
            id.relationship = s.value(QStringLiteral("relationship")).toString();
            id.identity_basis = s.value(QStringLiteral("identity_basis")).toString();
            id.effective_from = universe_detail::date_or_invalid(s.value(QStringLiteral("effective_from")));
            id.effective_to = universe_detail::date_or_invalid(s.value(QStringLiteral("effective_to")));
            id.fund_structure = s.value(QStringLiteral("fund_structure")).toString();
            id.evidence_url = s.value(QStringLiteral("evidence_url")).toString();
            if (id.cik.size() != 10 || !id.effective_from.isValid() ||
                (id.effective_to.isValid() && id.effective_to < id.effective_from) || id.evidence_url.isEmpty())
                return fail(QStringLiteral("%1: malformed SEC reporting identity").arg(i.symbol));
            if (id.relationship == QLatin1String("class_of_multi_class_series"))
                return fail(QStringLiteral("%1: a multi-class series flow is not the ETF's flow").arg(i.symbol));
            i.sec_reporting.append(id);
        }
        // Intervals of one row must not overlap: one month, one reporting identity.
        for (int a = 0; a < i.sec_reporting.size(); ++a)
            for (int c = a + 1; c < i.sec_reporting.size(); ++c) {
                const auto& x = i.sec_reporting[a];
                const auto& y = i.sec_reporting[c];
                const QDate xe = x.effective_to.isValid() ? x.effective_to : QDate(9999, 12, 31);
                const QDate ye = y.effective_to.isValid() ? y.effective_to : QDate(9999, 12, 31);
                if (x.effective_from <= ye && y.effective_from <= xe)
                    return fail(QStringLiteral("%1: overlapping SEC reporting intervals").arg(i.symbol));
            }
        u.instruments.append(i);
    }
    for (const char* need : {"us_sector", "theme", "country"}) {
        if (u.with_role(need).isEmpty())
            return fail(QStringLiteral("universe has no %1 rows").arg(QLatin1String(need)));
    }
    for (const QString& bench : {u.bench_us, u.bench_global}) {
        if (!u.find(bench))
            return fail(QStringLiteral("benchmark %1 is not a universe row").arg(bench));
    }
    for (const auto& i : u.instruments) {
        if (!i.benchmark.isEmpty() && !u.find(i.benchmark))
            return fail(QStringLiteral("%1 declares unknown benchmark %2").arg(i.symbol, i.benchmark));
    }
    for (const auto& bv : doc.value(QStringLiteral("baskets")).toArray()) {
        const QJsonObject o = bv.toObject();
        Basket k;
        k.id = o.value(QStringLiteral("id")).toString();
        k.name = o.value(QStringLiteral("name")).toString();
        k.market = o.value(QStringLiteral("market")).toString();
        k.benchmark = o.value(QStringLiteral("benchmark")).toString();
        k.benchmark_fallback = o.value(QStringLiteral("benchmark_fallback")).toString();
        k.construction = o.value(QStringLiteral("construction")).toString();
        k.min_member_fraction = o.value(QStringLiteral("min_member_fraction")).toDouble(0.5);
        for (const auto& m : o.value(QStringLiteral("members")).toArray())
            k.members.append(m.toString());
        if (k.members.isEmpty() || k.construction != QLatin1String("equal_weight_daily_rebalanced"))
            return fail(QStringLiteral("basket %1 malformed").arg(k.id));
        for (const auto& m : k.members)
            if (u.find(m) && u.find(m)->is_fund())
                return fail(QStringLiteral("basket %1 member %2 is a fund row").arg(k.id, m));
        u.baskets.append(k);
    }
    const QJsonObject intl = doc.value(QStringLiteral("intl_sector_stocks")).toObject();
    for (auto it = intl.begin(); it != intl.end(); ++it) {
        QVector<IntlStock> stocks;
        for (const auto& sv : it.value().toArray()) {
            const QJsonObject s = sv.toObject();
            stocks.append({s.value(QStringLiteral("symbol")).toString(), s.value(QStringLiteral("label")).toString(),
                           s.value(QStringLiteral("sector")).toString()});
        }
        u.intl.insert(it.key(), stocks);
        u.intl_markets.append(it.key());
    }
    return u;
}

/// Load the bundled resource (or another file for tests).
inline std::optional<ResearchUniverse> load_universe(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot open %1").arg(path);
        return std::nullopt;
    }
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject()) {
        if (error)
            *error = QStringLiteral("universe JSON invalid: %1").arg(pe.errorString());
        return std::nullopt;
    }
    return parse_universe(d.object(), error);
}

} // namespace fincept::services::etf::research
