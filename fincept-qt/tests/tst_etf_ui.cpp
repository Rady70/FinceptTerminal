// Shipped native screen + SQLite loader. No source transport or TWS linked.
#include "etf_ui_fixtures.h"
#include "screens/etf/EtfFlowsScreen.h"
#include "screens/etf/EtfPresentation.h"
#include "screens/etf/EtfResearchLoader.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"
#include "ui/tables/NumericTableWidgetItem.h"
#include "ui/theme/ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlRecord>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

using namespace fincept;
using namespace fincept::services::etf;
using namespace fincept::screens;
using namespace etf_ui_fixtures;

namespace {
template <class T>
T* widget(EtfFlowsScreen& screen, const char* name) {
    return screen.findChild<T*>(QLatin1String(name));
}
QByteArray store_digest() {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    auto names = Database::instance().execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'etf_%' ORDER BY name");
    if (names.is_err())
        return {};
    QStringList tables;
    auto query = names.value();
    while (query.next())
        tables.append(query.value(0).toString());
    for (const auto& name : tables) {
        auto read = Database::instance().execute("SELECT * FROM " + name + " ORDER BY rowid");
        if (read.is_err())
            return {};
        hash.addData(name.toUtf8());
        auto rows = read.value();
        while (rows.next()) {
            QJsonArray row;
            for (int i = 0; i < rows.record().count(); ++i)
                row.append(QJsonValue::fromVariant(rows.value(i)));
            hash.addData(QJsonDocument(row).toJson(QJsonDocument::Compact));
        }
    }
    return hash.result().toHex();
}
int row_containing(QTableWidget* table, int column, const QString& text) {
    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, column)->text().contains(text))
            return row;
    return -1;
}
void select_group(EtfFlowsScreen& screen, const QString& id) {
    auto* overview = widget<QTableWidget>(screen, "etfOverview");
    const int row = row_containing(overview, 0, id);
    QVERIFY2(row >= 0, qPrintable(id));
    overview->selectRow(row);
}
void configure(EtfFlowsScreen& screen, const QString& level = "cross_asset", const QString& group = "",
               const QDate& month = QDate(2026, 10, 1), const char* cutoff = "2026-12-07T22:00:00.000Z") {
    screen.restore_state({{"level", level},
                          {"group", group},
                          {"from", month},
                          {"to", month},
                          {"as_of", utc(cutoff)},
                          {"known_at", utc(cutoff)}});
}
bool seed_rotation(qint64 con_id, const char* symbol) {
    const char* seen = "2026-12-02T23:00:00.000Z";
    const qint64 listed = instrument(con_id, symbol, seen);
    if (listed <= 0)
        return false;
    etf_store::RetrievalRecord ret;
    ret.run_id = "ui-synthetic";
    ret.source_type = SourceType::IbkrTwsReadonly;
    ret.acquisition_mode = AcquisitionMode::IbkrReadonlyWrapper;
    ret.endpoint = "synthetic";
    ret.requested_at = utc(seen);
    ret.retrieved_at = utc(seen);
    ret.status = RetrievalStatus::Ok;
    ret.response_sha256 = "synthetic";
    ret.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    ret.interpretation = "synthetic";
    const auto id = repo().record_retrieval(ret);
    if (id.is_err())
        return false;
    const auto days = UsEquityCalendar::weekdays_in(QDate(2026, 8, 3), QDate(2026, 11, 30));
    if (repo().upsert_sessions(days).is_err())
        return false;
    int n = 0;
    for (const auto& day : days) {
        if (!day.is_session())
            continue;
        ++n;
        for (const char* measure : {"bar_close", "bar_volume"}) {
            etf_store::ObservationInput in;
            in.subject_type = SubjectType::ListedInstrument;
            in.subject_id = listed;
            in.kind = MeasurementKind::MarketBar;
            in.measure = QLatin1String(measure);
            in.units = in.measure == QLatin1String("bar_close") ? "USD_per_share" : "shares_ibkr_filtered";
            in.basis = "ibkr_trades_rth_daily_split_adjusted";
            in.source_type = ret.source_type;
            in.acquisition_mode = ret.acquisition_mode;
            in.effective_date = day.date;
            in.value = FieldValue::reported_value(100.0 + n);
            in.retrieval_id = id.value();
            in.seen_at = utc(seen);
            in.observation_start = utc(seen);
            if (repo().record_observation(in).is_err())
                return false;
        }
    }
    return true;
}
} // namespace

class EtfUiTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    bool representative_ = false;
  private slots:
    void initTestCase() {
        register_migration_v052();
        const QString existing = qEnvironmentVariable("MARKETLAB_ETF_UI_VALIDATION_DB");
        representative_ = !existing.isEmpty();
        QVERIFY(Database::instance().open(representative_ ? existing : dir_.filePath("ui.db")).is_ok());
        ui::ThemeManager::instance().apply_theme("Obsidian");
    }
    void cleanupTestCase() {
        QThreadPool::globalInstance()->waitForDone();
        Database::instance().close();
    }
    void init() {
        if (representative_) {
            if (QString::fromLatin1(QTest::currentTestFunction()) != QLatin1String("representative_profile"))
                QSKIP("Profile replay mode never seeds synthetic observations");
            return;
        }
        QVERIFY(Database::instance().exec("PRAGMA foreign_keys=OFF").is_ok());
        const auto names =
            Database::instance().execute("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'etf_%'");
        QVERIFY(names.is_ok());
        QStringList tables;
        auto query = names.value();
        while (query.next())
            tables.append(query.value(0).toString());
        for (const auto& name : tables)
            QVERIFY(Database::instance().exec("DELETE FROM " + name).is_ok());
        QVERIFY(Database::instance().exec("PRAGMA foreign_keys=ON").is_ok());
    }
    void presentation_preserves_missing_zero_partial_and_coverage() {
        QCOMPARE(etf_ui::number(QJsonValue(QJsonValue::Null), "USD"), "Unavailable");
        QCOMPARE(etf_ui::number(0.0, "USD"), "USD 0.00");
        QJsonObject month{{"quality", "PARTIAL"},
                          {"has_revised_inputs", true},
                          {"coverage", QJsonObject{{"observed_reporting_identities", 1},
                                                   {"unique_reporting_identities", 3},
                                                   {"unresolved_subjects", 2},
                                                   {"regulatory_assets_estimate", QJsonValue(QJsonValue::Null)},
                                                   {"basis", "unresolved_constituent"}}}};
        QCOMPARE(etf_ui::quality(month), "Partial · revised inputs");
        QVERIFY(etf_ui::coverage(month).startsWith("1 / 3 SEC identities measured"));
        QVERIFY(etf_ui::assets_coverage(month).startsWith("Unavailable"));
        month["coverage"] = QJsonObject{{"regulatory_assets_estimate", 0.5}};
        QCOMPARE(etf_ui::assets_coverage(month), "50.00% · prior regulatory net assets estimate");
        QVERIFY(etf_ui::component_label("price_return_21").endsWith("sessions"));
        QVERIFY(etf_ui::reason("ambiguous_reporting_identity_links").contains("attribution refused"));
        QVERIFY(etf_ui::reason("classification_history_unverified").contains("historical"));
    }
    void empty_profile_invalid_range_and_unknown_group() {
        EtfFlowsScreen screen;
        configure(screen);
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        QVERIFY(loaded.last()[0].toBool());
        auto* overview = widget<QTableWidget>(screen, "etfOverview");
        QCOMPARE(overview->rowCount(), 6);
        for (int row = 0; row < overview->rowCount(); ++row)
            QCOMPARE(overview->item(row, 1)->text(), "Unavailable");
        QVERIFY(widget<QLabel>(screen, "etfStatus")->text().contains("No measured regulatory flow"));
        QVERIFY(widget<QLabel>(screen, "etfContext")->text().contains("2026-12-07T22:00:00.000Z"));
        QVERIFY(!widget<QCheckBox>(screen, "etfLeveraged")->isChecked());
        configure(screen, "complex", "sp500", QDate(2027, 1, 1));
        QCOMPARE(overview->rowCount(), 0);
        widget<QPushButton>(screen, "etfRecompute")->click();
        QVERIFY(widget<QLabel>(screen, "etfStatus")->text().contains("ends after as_of"));
        auto req = request("2026-12-07T22:00:00.000Z");
        req.group_id = "unsupported";
        QVERIFY(etf_ui::load_groups(req).is_err());
        auto* groups = widget<QComboBox>(screen, "etfGroup");
        groups->addItem("Unsupported fixture group", "unsupported");
        groups->setCurrentIndex(groups->count() - 1);
        configure(screen, "complex", "sp500");
        groups->addItem("Unsupported fixture group", "unsupported");
        groups->setCurrentIndex(groups->count() - 1);
        widget<QPushButton>(screen, "etfRecompute")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 15000);
        QVERIFY(!loaded.last()[0].toBool());
        QCOMPARE(overview->rowCount(), 0);
        QVERIFY(widget<QLabel>(screen, "etfStatus")->text().contains("failed"));
    }
    void monthly_zero_revisions_constituents_and_individual_history() {
        const qint64 spy = entity("0000884394", "", "2026-12-02T00:00:00.000Z");
        QVERIFY(spy > 0);
        QVERIFY(filing(spy, "0000884394", "0001193125-26-990001", "NPORT-P", "2026-11-25T15:00:00.000Z",
                       "2026-12-02T00:00:00.000Z", 0.0));
        const auto before = store_digest();
        EtfFlowsScreen screen;
        configure(screen, "complex", "sp500");
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        auto* overview = widget<QTableWidget>(screen, "etfOverview");
        QCOMPARE(overview->item(0, 1)->text(), "USD 0.00");
        QCOMPARE(overview->item(0, 2)->text(), "Unavailable");
        QVERIFY(overview->item(0, 3)->text().contains("Partial"));
        auto* numeric = dynamic_cast<ui::NumericTableWidgetItem*>(overview->item(0, 1));
        QVERIFY(numeric && numeric->has_numeric_value());
        QCOMPARE(widget<QTableWidget>(screen, "etfMonths")->item(0, 0)->text(), "2026-10");
        QVERIFY(widget<QLabel>(screen, "etfMonthStatus")->text().contains("monthly SEC N-PORT"));
        auto* constituents = widget<QTableWidget>(screen, "etfConstituents");
        const int row = row_containing(constituents, 0, "0000884394/");
        QVERIFY(row >= 0);
        QCOMPARE(constituents->item(row, 2)->text(), "USD 0.00");
        constituents->setCurrentCell(row, 0);
        emit constituents->itemActivated(constituents->item(row, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 15000);
        auto* individual = widget<QTableWidget>(screen, "etfIndividual");
        const int net = row_containing(individual, 1, "Net flow");
        QVERIFY(net >= 0);
        QCOMPARE(individual->item(net, 2)->text(), "USD 0.00");
        const auto provenance = widget<QPlainTextEdit>(screen, "etfProvenance")->toPlainText();
        QVERIFY(provenance.contains("regulatory_flow_analytics_v1"));
        QVERIFY(provenance.contains("0001193125-26-990001"));
        QCOMPARE(store_digest(), before);
        QVERIFY(filing(spy, "0000884394", "0001193125-26-990002", "NPORT-P/A", "2026-12-04T15:00:00.000Z",
                       "2026-12-06T00:00:00.000Z", 10.0, "0001193125-26-990001"));
        widget<QPushButton>(screen, "etfRecompute")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 15000);
        QCOMPARE(overview->item(0, 1)->text(), "USD 10.00");
        QVERIFY(overview->item(0, 3)->text().contains("revised inputs"));
        QVERIFY(widget<QPlainTextEdit>(screen, "etfProvenance")->toPlainText().contains("filing_vintages"));
        configure(screen, "complex", "sp500", QDate(2026, 10, 1), "2026-12-03T22:00:00.000Z");
        widget<QPushButton>(screen, "etfRecompute")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 4, 15000);
        QCOMPARE(overview->item(0, 1)->text(), "USD 0.00");
    }
    void rotation_only_stale_unclassified_and_leverage_policy() {
        QVERIFY(seed_rotation(51529211, "GLD"));
        QVERIFY(seed_rotation(738523410, "SH"));
        QVERIFY(instrument(99999999, "UNCLASSIFIED", "2026-12-02T23:00:00.000Z") > 0);
        const auto before = store_digest();
        EtfFlowsScreen screen;
        configure(screen);
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        select_group(screen, "Commodity");
        auto* rotation = widget<QTableWidget>(screen, "etfRotation");
        const int gold = row_containing(rotation, 0, "51529211");
        QVERIFY(gold >= 0);
        QVERIFY(rotation->item(gold, 1)->text().contains("Stale"));
        QVERIFY(rotation->item(gold, 1)->text().contains("D5"));
        QVERIFY(rotation->item(gold, 3)->text().endsWith('%'));
        QVERIFY(!rotation->item(gold, 3)->text().contains("USD"));
        rotation->setCurrentCell(gold, 0);
        emit rotation->itemActivated(rotation->item(gold, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 15000);
        QVERIFY(widget<QTableWidget>(screen, "etfIndividual")->rowCount() > 0);
        QVERIFY(widget<QLabel>(screen, "etfIndividualStatus")->text().contains("exchange sessions"));
        QCOMPARE(widget<QTableWidget>(screen, "etfUnclassified")->rowCount(), 1);
        select_group(screen, "Equity");
        const int inverse = row_containing(rotation, 0, "738523410");
        QVERIFY(inverse >= 0);
        QVERIFY(rotation->item(inverse, 1)->text().contains("Excluded"));
        QCOMPARE(rotation->item(inverse, 3)->text(), "Unavailable");
        QVERIFY(rotation->item(inverse, 8)->text().contains("-1x"));
        rotation->setCurrentCell(inverse, 0);
        emit rotation->itemActivated(rotation->item(inverse, 0));
        QCOMPARE(widget<QTableWidget>(screen, "etfIndividual")->rowCount(), 0);
        QVERIFY(widget<QLabel>(screen, "etfIndividualStatus")->text().contains("excluded"));
        widget<QCheckBox>(screen, "etfLeveraged")->setChecked(true);
        widget<QPushButton>(screen, "etfRecompute")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 15000);
        select_group(screen, "Equity");
        const int included = row_containing(rotation, 0, "738523410");
        QVERIFY(included >= 0);
        QVERIFY(!rotation->item(included, 1)->text().contains("Excluded"));
        QVERIFY(rotation->item(included, 3)->text().endsWith('%'));
        QCOMPARE(store_digest(), before);
    }
    void taxonomy_history_ambiguity_and_missing_subject() {
        const qint64 reporting = entity("0009999999", "", "2026-12-02T00:00:00.000Z");
        const qint64 gold = instrument(51529211, "GLD", "2026-12-02T00:00:00.000Z");
        const qint64 bonds = instrument(15547841, "TLT", "2026-12-02T00:00:00.000Z");
        QVERIFY(repo()
                    .declare_link(gold, reporting, {}, LinkRelationship::RegistrantIsInstrument, "synthetic",
                                  utc("2026-12-04T00:00:00.000Z"))
                    .is_ok());
        QVERIFY(repo()
                    .declare_link(bonds, reporting, {}, LinkRelationship::RegistrantIsInstrument, "synthetic",
                                  utc("2026-12-06T00:00:00.000Z"))
                    .is_ok());
        EtfFlowsScreen screen;
        configure(screen, "category", "physical_gold");
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        auto* members = widget<QTableWidget>(screen, "etfConstituents");
        const int row = row_containing(members, 0, "51529211");
        QVERIFY(row >= 0);
        QVERIFY(members->item(row, 3)->text().contains("attribution refused"));
        QCOMPARE(members->item(row, 2)->text(), "Unavailable / not counted");
        configure(screen, "category", "nasdaq_100", QDate(2025, 12, 1));
        widget<QPushButton>(screen, "etfRecompute")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 15000);
        QVERIFY(row_containing(members, 3, "historical exposure") >= 0);
        const int missing = row_containing(members, 0, "0001067839/S000101292");
        QVERIFY(missing >= 0);
        members->setCurrentCell(missing, 0);
        emit members->itemActivated(members->item(missing, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 15000);
        QVERIFY(!loaded.last()[0].toBool());
        QVERIFY(widget<QLabel>(screen, "etfIndividualStatus")->text().contains("not recorded"));
    }
    void lifecycle_restore_resize_and_destroy_during_work() {
        EtfFlowsScreen screen;
        configure(screen, "category", "physical_gold");
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        widget<QTabWidget>(screen, "etfTabs")->setCurrentIndex(1);
        const auto saved = screen.save_state();
        QCOMPARE(saved.value("group").toString(), "physical_gold");
        QCOMPARE(saved.value("tab").toInt(), 1);
        auto* other = new EtfFlowsScreen;
        other->restore_state(saved);
        QCOMPARE(other->save_state().value("group"), saved.value("group"));
        other->resize(740, 850);
        other->show();
        QTest::qWait(20);
        other->resize(1600, 1100);
        delete other;
        QThreadPool::globalInstance()->waitForDone();
    }
    void representative_profile() {
        if (!representative_)
            QSKIP("Existing-profile replay is a separate configured local run");
        const auto before = store_digest();
        QVERIFY(!before.isEmpty());
        EtfFlowsScreen screen;
        configure(screen, "cross_asset", "", QDate(2026, 6, 1), "2026-09-27T20:00:00.000Z");
        screen.resize(1600, 1100);
        screen.show();
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 60000);
        QVERIFY(loaded.last()[0].toBool());
        const QString output = qEnvironmentVariable("MARKETLAB_ETF_UI_CAPTURE_DIR");
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            QVERIFY(screen.grab().save(output + "/cross_asset.png"));
        }
        QJsonArray cases;
        widget<QComboBox>(screen, "etfLevel")->setCurrentIndex(2);
        auto* group_combo = widget<QComboBox>(screen, "etfGroup");
        QStringList supported;
        for (int i = 1; i < group_combo->count(); ++i)
            supported.append(group_combo->itemData(i).toString());
        int expected = loaded.count();
        for (const auto& id : supported) {
            group_combo->setCurrentIndex(group_combo->findData(id));
            widget<QPushButton>(screen, "etfRecompute")->click();
            ++expected;
            QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), expected, 60000);
            QVERIFY(loaded.last()[0].toBool());
            auto* overview = widget<QTableWidget>(screen, "etfOverview");
            QCOMPARE(overview->rowCount(), 1);
            auto expected_request = request("2026-09-27T20:00:00.000Z");
            expected_request.group_level = "category";
            expected_request.group_id = id;
            expected_request.output_from = QDate(2026, 6, 1);
            expected_request.output_to = QDate(2026, 6, 30);
            const auto expected_result = etf_ui::load_groups(expected_request);
            QVERIFY(expected_result.is_ok());
            const auto expected_group = expected_result.value().value("groups").toArray()[0].toObject();
            const auto expected_month = etf_ui::last_month(expected_group);
            QCOMPARE(overview->item(0, 1)->text(),
                     etf_ui::number(expected_month.value("observed_net_flow_usd"), "USD"));
            QCOMPARE(overview->item(0, 2)->text(),
                     etf_ui::number(expected_month.value("complete_net_flow_usd"), "USD"));
            QCOMPARE(widget<QTableWidget>(screen, "etfMonths")->item(0, 0)->data(Qt::UserRole).toJsonObject(),
                     expected_month);
            auto* rotation = widget<QTableWidget>(screen, "etfRotation");
            for (int row = 0; row < rotation->rowCount(); ++row) {
                const auto subject = rotation->item(row, 0)->data(Qt::UserRole).toJsonObject();
                const auto value =
                    subject.value("latest").toObject().value("values").toObject().value("price_return_21").toObject();
                QCOMPARE(rotation->item(row, 3)->text(), etf_ui::component(value));
            }
            cases.append(QJsonObject{{"category", id},
                                     {"monthly_rows", widget<QTableWidget>(screen, "etfMonths")->rowCount()},
                                     {"constituents", widget<QTableWidget>(screen, "etfConstituents")->rowCount()},
                                     {"rotation_subjects", widget<QTableWidget>(screen, "etfRotation")->rowCount()},
                                     {"quality", overview->item(0, 3)->text()},
                                     {"regulatory_present", overview->item(0, 1)->text() != "Unavailable"}});
            if (!output.isEmpty() && (id.contains("gold") || id.contains("income") || id.contains("leveraged"))) {
                widget<QTabWidget>(screen, "etfTabs")->setCurrentIndex(1);
                QVERIFY(screen.grab().save(output + "/" + id + ".png"));
            }
            if (id == QLatin1String("physical_gold")) {
                const int gold = row_containing(rotation, 0, "51529211");
                QVERIFY(gold >= 0);
                rotation->setCurrentCell(gold, 0);
                emit rotation->itemActivated(rotation->item(gold, 0));
                ++expected;
                QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), expected, 60000);
                QVERIFY(loaded.last()[0].toBool());
                QVERIFY(widget<QTableWidget>(screen, "etfIndividual")->rowCount() > 0);
                if (!output.isEmpty())
                    QVERIFY(screen.grab().save(output + "/individual_rotation.png"));
            }
        }
        configure(screen, "complex", "sp500", QDate(2026, 6, 1), "2026-09-27T20:00:00.000Z");
        widget<QPushButton>(screen, "etfRecompute")->click();
        ++expected;
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), expected, 60000);
        QVERIFY(loaded.last()[0].toBool());
        auto* constituents = widget<QTableWidget>(screen, "etfConstituents");
        QVERIFY(constituents->rowCount() >= 3);
        const int spy = row_containing(constituents, 0, "0000884394/");
        QVERIFY(spy >= 0);
        constituents->setCurrentCell(spy, 0);
        emit constituents->itemActivated(constituents->item(spy, 0));
        ++expected;
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), expected, 60000);
        QVERIFY(loaded.last()[0].toBool());
        QVERIFY(widget<QTableWidget>(screen, "etfIndividual")->rowCount() > 0);
        if (!output.isEmpty()) {
            QVERIFY(screen.grab().save(output + "/individual_sec.png"));
            widget<QTabWidget>(screen, "etfTabs")->setCurrentIndex(2);
            QVERIFY(screen.grab().save(output + "/complex.png"));
            screen.resize(740, 850);
            QVERIFY(screen.grab().save(output + "/narrow.png"));
            QFile evidence(output + "/summary.json");
            QVERIFY(evidence.open(QIODevice::WriteOnly));
            evidence.write(QJsonDocument(QJsonObject{{"cases", cases},
                                                     {"source_tables_before", QString::fromLatin1(before)},
                                                     {"source_tables_after", QString::fromLatin1(store_digest())},
                                                     {"synthetic", false}})
                               .toJson());
        }
        QCOMPARE(store_digest(), before);
    }
};

QTEST_MAIN(EtfUiTest)
#include "tst_etf_ui.moc"
