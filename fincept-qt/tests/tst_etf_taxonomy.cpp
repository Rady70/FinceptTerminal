// Batch D taxonomy contract. This suite reads the shipped, reviewed snapshot
// and mutates it to prove exclusions and identity/date guards fail closed.
#include "services/etf/EtfTaxonomy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>
#include <QUrl>

using namespace fincept::services::etf;

namespace {
QByteArray shipped_json() {
    QFile file(QStringLiteral(":/etf/taxonomy_v2.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        file.setFileName(QFileInfo(QString::fromUtf8(__FILE__))
                             .absoluteDir()
                             .filePath(QStringLiteral("../resources/etf_taxonomy_v2.json")));
        if (!file.open(QIODevice::ReadOnly))
            return {};
    }
    return file.readAll();
}

QJsonObject shipped_object() {
    return QJsonDocument::fromJson(shipped_json()).object();
}

std::optional<TaxonomySnapshot> load_object(const QJsonObject& object, QString* error = nullptr) {
    return TaxonomySnapshot::load(QJsonDocument(object).toJson(QJsonDocument::Compact), error);
}

QDate date(const char* iso) {
    return QDate::fromString(QString::fromLatin1(iso), Qt::ISODate);
}
} // namespace

class EtfTaxonomyTest : public QObject {
    Q_OBJECT
  private slots:
    void shipped_version_and_exact_complex();
    void no_ticker_identity_and_unknown_state();
    void classification_history_is_bounded();
    void special_exposures_are_separate();
    void qqq_structure_transition();
    void duplicate_and_overlapping_identity_refused();
    void complex_contamination_refused();
    void category_and_leverage_mutations_refused();
    void synthetic_hedged_and_crypto_equity_boundaries();
    void new_version_changes_only_its_own_interval();
    void malformed_entries_refused();
    void linked_policy_and_history_are_checked_without_a_group_level();
    void shipped_provenance_is_fund_specific();
};

void EtfTaxonomyTest::shipped_version_and_exact_complex() {
    const auto bytes = shipped_json();
    QVERIFY2(!bytes.isEmpty(), "taxonomy resource or source file missing");
    QString error;
    auto taxonomy = TaxonomySnapshot::load(bytes, &error);
    QVERIFY2(taxonomy.has_value(), qPrintable(error));
    QCOMPARE(taxonomy->version(), QStringLiteral("etf-taxonomy-v2"));
    QCOMPARE(taxonomy->sha256().size(), 64);
    QCOMPARE(taxonomy->entries().size(), 31);
    const auto listed = taxonomy->members(TaxonomySubjectType::Listed, QStringLiteral("complex"),
                                          QStringLiteral("sp500"), date("2026-09-25"));
    QCOMPARE(listed.size(), 2);
    QCOMPARE(listed.at(0).con_id, qint64(756733));  // SPY
    QCOMPARE(listed.at(1).con_id, qint64(8991352)); // IVV
    const auto reporting = taxonomy->members(TaxonomySubjectType::Reporting, QStringLiteral("complex"),
                                             QStringLiteral("sp500"), date("2026-06-30"));
    QCOMPARE(reporting.size(), 2);
    QSet<QString> keys;
    for (const auto& entry : reporting)
        keys.insert(entry.cik10 + QLatin1Char('/') + entry.series_id);
    QCOMPARE(keys, QSet<QString>({QStringLiteral("0000884394/"), QStringLiteral("0001100663/S000004310")}));
    QCOMPARE(taxonomy->group_ids(TaxonomySubjectType::Listed, QStringLiteral("complex"), date("2026-09-25")),
             QStringList({QStringLiteral("sp500")}));
    QVERIFY(taxonomy
                ->members(TaxonomySubjectType::Listed, QStringLiteral("complex"), QStringLiteral("sp500"),
                          date("2026-06-30"))
                .isEmpty());
}

void EtfTaxonomyTest::shipped_provenance_is_fund_specific() {
    const QJsonObject object = shipped_object();
    QVERIFY(!object.value(QStringLiteral("evidence_conventions")).toObject().isEmpty());
    const QSet<QString> corrected = {QStringLiteral("EFA"), QStringLiteral("LQD"),  QStringLiteral("TLT"),
                                     QStringLiteral("IEF"), QStringLiteral("SHY"),  QStringLiteral("TIP"),
                                     QStringLiteral("BIL"), QStringLiteral("VNQ"),  QStringLiteral("XLE"),
                                     QStringLiteral("XLK"), QStringLiteral("SGOV"), QStringLiteral("HYG"),
                                     QStringLiteral("EEM"), QStringLiteral("IWM")};
    int reviewed = 0;
    for (const auto& value : object.value(QStringLiteral("entries")).toArray()) {
        const auto row = value.toObject();
        const QUrl url(row.value(QStringLiteral("evidence_url")).toString());
        QVERIFY(url.isValid() && url.scheme() == QLatin1String("https"));
        // Project category tables cannot serve as primary fund evidence.
        QVERIFY(url.host() != QLatin1String("github.com"));
        if (row.value(QStringLiteral("subject_type")) != QLatin1String("listed") ||
            !corrected.contains(row.value(QStringLiteral("ticker_hint")).toString()))
            continue;
        ++reviewed;
        QCOMPARE(url.host(), QStringLiteral("www.sec.gov"));
        const auto evidence = row.value(QStringLiteral("evidence_detail")).toObject();
        const QDate issued = QDate::fromString(evidence.value(QStringLiteral("document_date")).toString(), Qt::ISODate);
        QVERIFY(issued.isValid());
        QVERIFY(issued <= QDate::fromString(row.value(QStringLiteral("effective_from")).toString(), Qt::ISODate));
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
                    .match(evidence.value(QStringLiteral("sha256")).toString())
                    .hasMatch());
        QVERIFY(!evidence.value(QStringLiteral("locators")).toArray().isEmpty());
    }
    QCOMPARE(reviewed, corrected.size());
    const auto taxonomy = TaxonomySnapshot::load(shipped_json());
    QVERIFY(taxonomy);
    for (qint64 con_id : {qint64(15547816), qint64(43652089)})
        QCOMPARE(taxonomy->lookup_listed(con_id, date("2026-09-25")).entry->region, QStringLiteral("us_dollar_bonds"));
}

void EtfTaxonomyTest::no_ticker_identity_and_unknown_state() {
    QJsonObject object = shipped_object();
    QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto e = entries.at(i).toObject();
        if (e.value(QStringLiteral("con_id")).toInteger() == 756733) {
            e.insert(QStringLiteral("ticker_hint"), QStringLiteral("NEWSPY"));
            entries.replace(i, e);
            break;
        }
    }
    object.insert(QStringLiteral("entries"), entries);
    const auto taxonomy = load_object(object);
    QVERIFY(taxonomy.has_value());
    const auto known = taxonomy->lookup_listed(756733, date("2026-09-25"));
    QCOMPARE(known.status, TaxonomyStatus::Classified);
    QCOMPARE(known.entry->ticker_hint, QStringLiteral("NEWSPY"));
    QCOMPARE(taxonomy->lookup_listed(999999999, date("2026-09-25")).status, TaxonomyStatus::UnknownIdentity);
    QCOMPARE(taxonomy->lookup_reporting(QStringLiteral("0000884394"), QStringLiteral("S000UNKNOWN"), date("2026-06-30"))
                 .status,
             TaxonomyStatus::UnknownIdentity);
}

void EtfTaxonomyTest::classification_history_is_bounded() {
    const auto taxonomy = TaxonomySnapshot::load(shipped_json());
    QVERIFY(taxonomy.has_value());
    QCOMPARE(taxonomy->lookup_reporting(QStringLiteral("0000884394"), {}, date("2024-01-25")).status,
             TaxonomyStatus::HistoryUnverified);
    QCOMPARE(taxonomy->lookup_reporting(QStringLiteral("0000884394"), {}, date("2024-01-26")).status,
             TaxonomyStatus::Classified);
    QCOMPARE(taxonomy->lookup_reporting(QStringLiteral("0001100663"), QStringLiteral("S000004310"), date("2025-07-31"))
                 .status,
             TaxonomyStatus::HistoryUnverified);
    QCOMPARE(taxonomy->lookup_reporting(QStringLiteral("0001100663"), QStringLiteral("S000004310"), date("2026-06-30"))
                 .status,
             TaxonomyStatus::Classified);
    QCOMPARE(taxonomy->lookup_listed(756733, date("2026-06-30")).status, TaxonomyStatus::HistoryUnverified);
    QCOMPARE(taxonomy->lookup_listed(756733, date("2026-09-25")).status, TaxonomyStatus::Classified);
    QCOMPARE(taxonomy->lookup_listed(756733, {}).status, TaxonomyStatus::HistoryUnverified);
}

void EtfTaxonomyTest::special_exposures_are_separate() {
    const auto taxonomy = TaxonomySnapshot::load(shipped_json());
    QVERIFY(taxonomy.has_value());
    const QDate on = date("2026-09-25");
    const auto gold = taxonomy->lookup_listed(51529211, on).entry.value();
    const auto energy = taxonomy->lookup_listed(4215217, on).entry.value();
    const auto oil = taxonomy->lookup_listed(418893644, on).entry.value();
    const auto bitcoin = taxonomy->lookup_listed(677037673, on).entry.value();
    const auto income = taxonomy->lookup_listed(423216410, on).entry.value();
    const auto ordinary = taxonomy->lookup_listed(756733, on).entry.value();
    const auto long2x = taxonomy->lookup_listed(39622943, on).entry.value();
    const auto short1x = taxonomy->lookup_listed(738523410, on).entry.value();
    QCOMPARE(gold.exposure_mechanism, QStringLiteral("physical_commodity"));
    QCOMPARE(oil.exposure_mechanism, QStringLiteral("futures_commodity"));
    QCOMPARE(energy.exposure_mechanism, QStringLiteral("sector_equity"));
    QCOMPARE(bitcoin.exposure_mechanism, QStringLiteral("spot_crypto"));
    QVERIFY(income.option_overlay);
    QVERIFY(!ordinary.option_overlay);
    QVERIFY(long2x.leveraged && !long2x.inverse);
    QCOMPARE(long2x.leverage_multiple, 2.0);
    QVERIFY(short1x.inverse && !short1x.leveraged);
    QCOMPARE(short1x.leverage_multiple, -1.0);
    QVERIFY(!ordinary.leveraged && !ordinary.inverse);
    QCOMPARE(ordinary.leverage_multiple, 1.0);
    QVERIFY(gold.asset_class != energy.asset_class && bitcoin.asset_class != ordinary.asset_class);
    QCOMPARE(taxonomy->lookup_listed(13002510, on).entry->currency_hedge, QStringLiteral("unhedged"));
}

void EtfTaxonomyTest::qqq_structure_transition() {
    const auto taxonomy = TaxonomySnapshot::load(shipped_json());
    QVERIFY(taxonomy.has_value());
    const auto cik = QStringLiteral("0001067839");
    const auto series = QStringLiteral("S000101292");
    QCOMPARE(taxonomy->lookup_reporting(cik, {}, date("2025-09-30")).entry->fund_structure,
             QStringLiteral("unit_investment_trust"));
    QCOMPARE(taxonomy->lookup_reporting(cik, {}, date("2025-10-01")).status, TaxonomyStatus::HistoryUnverified);
    QCOMPARE(taxonomy->lookup_reporting(cik, series, date("2025-10-31")).entry->fund_structure,
             QStringLiteral("unit_investment_trust"));
    QCOMPARE(taxonomy->lookup_reporting(cik, series, date("2025-12-19")).entry->fund_structure,
             QStringLiteral("unit_investment_trust"));
    QCOMPARE(taxonomy->lookup_reporting(cik, series, date("2025-12-20")).entry->fund_structure,
             QStringLiteral("open_end_etf"));
}

void EtfTaxonomyTest::duplicate_and_overlapping_identity_refused() {
    QJsonObject object = shipped_object();
    QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    entries.append(entries.at(0));
    object.insert(QStringLiteral("entries"), entries);
    QString error;
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("overlapping_identity_interval")));
    entries.removeLast();
    auto overlap = entries.at(0).toObject();
    overlap.insert(QStringLiteral("effective_from"), QStringLiteral("2026-09-26"));
    entries.append(overlap);
    object.insert(QStringLiteral("entries"), entries);
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("overlapping_identity_interval")));
}

void EtfTaxonomyTest::complex_contamination_refused() {
    QJsonObject object = shipped_object();
    QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto e = entries.at(i).toObject();
        if (e.value(QStringLiteral("ticker_hint")) == QLatin1String("JEPI") &&
            e.value(QStringLiteral("subject_type")) == QLatin1String("listed")) {
            e.insert(QStringLiteral("complex_id"), QStringLiteral("sp500"));
            entries.replace(i, e);
            break;
        }
    }
    object.insert(QStringLiteral("entries"), entries);
    QString error;
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("complex_semantics_conflict")));
}

void EtfTaxonomyTest::category_and_leverage_mutations_refused() {
    const QJsonObject baseline = shipped_object();
    QJsonObject object = baseline;
    QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    auto e = entries.at(0).toObject();
    e.insert(QStringLiteral("category"), QStringLiteral("physical_gold"));
    entries.replace(0, e);
    object.insert(QStringLiteral("entries"), entries);
    QString error;
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("category_asset_conflict")));

    object = baseline;
    entries = object.value(QStringLiteral("entries")).toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        e = entries.at(i).toObject();
        if (e.value(QStringLiteral("ticker_hint")) == QLatin1String("SH") &&
            e.value(QStringLiteral("subject_type")) == QLatin1String("listed")) {
            e.insert(QStringLiteral("leverage_multiple"), 1.0);
            entries.replace(i, e);
            break;
        }
    }
    object.insert(QStringLiteral("entries"), entries);
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("leverage_semantics_mismatch")));

    object = baseline;
    entries = object.value(QStringLiteral("entries")).toArray();
    e = entries.at(0).toObject();
    e.insert(QStringLiteral("exposure_mechanism"), QStringLiteral("magic"));
    entries.replace(0, e);
    object.insert(QStringLiteral("entries"), entries);
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("unknown_semantic_value")));
}

void EtfTaxonomyTest::synthetic_hedged_and_crypto_equity_boundaries() {
    // The tracked seed has no hedged international or crypto-equity product.
    // A new reviewed entry can represent either, but may not claim it is the
    // same exposure complex as unhedged EFA or spot IBIT.
    QJsonObject object = shipped_object();
    QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    QJsonObject efa;
    QJsonObject ibit;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const auto e = entries.at(i).toObject();
        if (e.value(QStringLiteral("ticker_hint")) == QLatin1String("EFA")) {
            efa = e;
            efa.insert(QStringLiteral("complex_id"), QStringLiteral("developed_equity"));
            entries.replace(i, efa);
        }
        if (e.value(QStringLiteral("ticker_hint")) == QLatin1String("IBIT")) {
            ibit = e;
            ibit.insert(QStringLiteral("complex_id"), QStringLiteral("spot_bitcoin"));
            entries.replace(i, ibit);
        }
    }
    QVERIFY(!efa.isEmpty());
    QVERIFY(!ibit.isEmpty());
    auto hedged = efa;
    hedged.insert(QStringLiteral("con_id"), 90010001);
    hedged.insert(QStringLiteral("ticker_hint"), QStringLiteral("FIXTURE_HEDGED"));
    hedged.insert(QStringLiteral("currency_hedge"), QStringLiteral("hedged"));
    hedged.insert(QStringLiteral("evidence_url"), QStringLiteral("https://example.test/hedged-fixture"));
    entries.append(hedged);
    object.insert(QStringLiteral("entries"), entries);
    QString error;
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("category_semantics_conflict")));

    hedged.insert(QStringLiteral("category"), QStringLiteral("developed_international_hedged"));
    entries.replace(entries.size() - 1, hedged);
    object.insert(QStringLiteral("entries"), entries);
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("complex_semantics_conflict")));

    hedged.insert(QStringLiteral("complex_id"), QStringLiteral("developed_equity_hedged"));
    entries.replace(entries.size() - 1, hedged);
    auto crypto_equity = ibit;
    crypto_equity.insert(QStringLiteral("con_id"), 90010002);
    crypto_equity.insert(QStringLiteral("ticker_hint"), QStringLiteral("FIXTURE_CRYPTO_EQUITY"));
    crypto_equity.insert(QStringLiteral("asset_class"), QStringLiteral("equity"));
    crypto_equity.insert(QStringLiteral("category"), QStringLiteral("crypto_equity"));
    crypto_equity.insert(QStringLiteral("exposure_mechanism"), QStringLiteral("crypto_equity"));
    crypto_equity.insert(QStringLiteral("fund_structure"), QStringLiteral("open_end_etf"));
    crypto_equity.insert(QStringLiteral("evidence_url"), QStringLiteral("https://example.test/crypto-equity-fixture"));
    entries.append(crypto_equity);
    object.insert(QStringLiteral("entries"), entries);
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("complex_semantics_conflict")));

    crypto_equity.insert(QStringLiteral("complex_id"), QStringLiteral("crypto_equity_complex"));
    entries.replace(entries.size() - 1, crypto_equity);
    object.insert(QStringLiteral("entries"), entries);
    const auto taxonomy = load_object(object, &error);
    QVERIFY2(taxonomy.has_value(), qPrintable(error));
    QCOMPARE(taxonomy->lookup_listed(90010001, date("2026-09-25")).entry->currency_hedge, QStringLiteral("hedged"));
    QCOMPARE(taxonomy->lookup_listed(90010002, date("2026-09-25")).entry->exposure_mechanism,
             QStringLiteral("crypto_equity"));
    QCOMPARE(taxonomy
                 ->members(TaxonomySubjectType::Listed, QStringLiteral("complex"), QStringLiteral("spot_bitcoin"),
                           date("2026-09-25"))
                 .size(),
             1);

    // A gold-miner equity cannot be admitted to a physical-gold complex.
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto gold = entries.at(i).toObject();
        if (gold.value(QStringLiteral("ticker_hint")) != QLatin1String("GLD"))
            continue;
        gold.insert(QStringLiteral("complex_id"), QStringLiteral("gold_fixture"));
        entries.replace(i, gold);
        auto miner = gold;
        miner.insert(QStringLiteral("con_id"), 90010003);
        miner.insert(QStringLiteral("ticker_hint"), QStringLiteral("FIXTURE_GOLD_MINER"));
        miner.insert(QStringLiteral("asset_class"), QStringLiteral("equity"));
        miner.insert(QStringLiteral("category"), QStringLiteral("gold_miners"));
        miner.insert(QStringLiteral("exposure_mechanism"), QStringLiteral("producer_equity"));
        miner.insert(QStringLiteral("fund_structure"), QStringLiteral("open_end_etf"));
        miner.insert(QStringLiteral("evidence_url"), QStringLiteral("https://example.test/gold-miner-fixture"));
        entries.append(miner);
        object.insert(QStringLiteral("entries"), entries);
        QVERIFY(!load_object(object, &error));
        QVERIFY(error.startsWith(QStringLiteral("complex_semantics_conflict")));
        miner.insert(QStringLiteral("complex_id"), QStringLiteral("gold_miners_fixture"));
        entries.replace(entries.size() - 1, miner);
        object.insert(QStringLiteral("entries"), entries);
        QVERIFY2(load_object(object, &error).has_value(), qPrintable(error));
        break;
    }
}

void EtfTaxonomyTest::new_version_changes_only_its_own_interval() {
    QJsonObject object = shipped_object();
    QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto e = entries.at(i).toObject();
        if (e.value(QStringLiteral("con_id")).toInteger() == 51529211) {
            e.insert(QStringLiteral("effective_to"), QStringLiteral("2026-09-30"));
            entries.replace(i, e);
            auto successor = e;
            successor.insert(QStringLiteral("effective_from"), QStringLiteral("2026-10-01"));
            successor.insert(QStringLiteral("effective_to"), QString());
            successor.insert(QStringLiteral("category"), QStringLiteral("precious_metal"));
            entries.append(successor);
            break;
        }
    }
    object.insert(QStringLiteral("entries"), entries);
    object.insert(QStringLiteral("version"), QStringLiteral("etf-taxonomy-v3"));
    QString error;
    const auto taxonomy = load_object(object, &error);
    QVERIFY2(taxonomy.has_value(), qPrintable(error));
    QCOMPARE(taxonomy->lookup_listed(51529211, date("2026-09-30")).entry->category, QStringLiteral("physical_gold"));
    QCOMPARE(taxonomy->lookup_listed(51529211, date("2026-10-01")).entry->category, QStringLiteral("precious_metal"));
    QCOMPARE(taxonomy
                 ->members(TaxonomySubjectType::Listed, QStringLiteral("category"), QStringLiteral("physical_gold"),
                           date("2026-10-01"))
                 .size(),
             0);
    QCOMPARE(taxonomy->version(), QStringLiteral("etf-taxonomy-v3"));
    QVERIFY(taxonomy->sha256() != TaxonomySnapshot::load(shipped_json())->sha256());
    QCOMPARE(TaxonomySnapshot::load(shipped_json())->lookup_listed(51529211, date("2026-10-01")).entry->category,
             QStringLiteral("physical_gold"));
}

void EtfTaxonomyTest::malformed_entries_refused() {
    QJsonObject object = shipped_object();
    QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    auto e = entries.at(0).toObject();
    e.insert(QStringLiteral("option_overlay"), QStringLiteral("false"));
    entries.replace(0, e);
    object.insert(QStringLiteral("entries"), entries);
    QString error;
    QVERIFY(!load_object(object, &error));
    QVERIFY(error.startsWith(QStringLiteral("invalid_flag")));
    object.insert(QStringLiteral("version"), QStringLiteral("today"));
    QVERIFY(!load_object(object, &error));
    QCOMPARE(error, QStringLiteral("invalid_version"));
}

void EtfTaxonomyTest::linked_policy_and_history_are_checked_without_a_group_level() {
    const auto snapshot = TaxonomySnapshot::load(shipped_json());
    QVERIFY(snapshot);
    const auto listed = snapshot->lookup_listed(756733, date("2026-10-01")).entry.value();
    const auto original = snapshot->lookup_reporting(QStringLiteral("0000884394"), {}, date("2026-10-01"));
    const QDate first = date("2026-10-01"), last = date("2026-10-31");
    QVERIFY(linked_taxonomy_problem(listed, original, first, last).isEmpty());
    for (int variant = 0; variant < 11; ++variant) {
        auto changed = original;
        switch (variant) {
            case 0:
                changed.entry->category = QStringLiteral("another_category");
                break;
            case 1:
                changed.entry->complex_id.clear();
                break;
            case 2:
                changed.entry->exposure_mechanism = QStringLiteral("producer_equity");
                break;
            case 3:
                changed.entry->fund_structure = QStringLiteral("open_end_etf");
                break;
            case 4:
                changed.entry->currency_hedge = QStringLiteral("hedged");
                break;
            case 5:
                changed.entry->leveraged = true;
                changed.entry->leverage_multiple = 2.0;
                break;
            case 6:
                changed.entry->inverse = true;
                changed.entry->leverage_multiple = -1.0;
                break;
            case 7:
                changed.entry->option_overlay = true;
                break;
            case 8:
                changed.entry->region = QStringLiteral("different_region");
                break;
            case 9:
                changed.entry->reference = QStringLiteral("different_index");
                break;
            case 10:
                changed.entry->asset_class = QStringLiteral("commodity");
                break;
        }
        QCOMPARE(linked_taxonomy_problem(listed, changed, first, last), QStringLiteral("taxonomy_identity_conflict"));
    }
    auto partial = original;
    partial.entry->effective_to = date("2026-10-15");
    QCOMPARE(linked_taxonomy_problem(listed, partial, first, last),
             QStringLiteral("linked_classification_month_partial"));
    QCOMPARE(linked_taxonomy_problem(listed, {TaxonomyStatus::HistoryUnverified, std::nullopt}, first, last),
             QStringLiteral("linked_classification_history_unverified"));
}

QTEST_MAIN(EtfTaxonomyTest)
#include "tst_etf_taxonomy.moc"
