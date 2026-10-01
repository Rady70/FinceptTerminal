// Shipped native screen + SQLite loader. No source transport or TWS linked.
#include "etf_ui_fixtures.h"
#include "screens/etf/EtfFlowsScreen.h"
#include "screens/etf/EtfMonthlyChart.h"
#include "screens/etf/EtfPresentation.h"
#include "screens/etf/EtfResearchLoader.h"
#include "screens/etf/EtfResearchVisuals.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"
#include "ui/tables/NumericTableWidgetItem.h"
#include "ui/theme/ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTimeEdit>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalSpy>
#include <QSplitter>
#include <QSqlRecord>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

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
QTableView* detail_view(EtfFlowsScreen& screen) {
    auto* button = widget<QPushButton>(screen, "etfExactIndividual");
    button->setChecked(true);
    return widget<QTableView>(screen, "etfIndividual");
}
int row_containing(QTableView* view, int column, const QString& text) {
    for (int row = 0; row < view->model()->rowCount(); ++row)
        if (view->model()->index(row, column).data().toString().contains(text))
            return row;
    return -1;
}
QString provenance_text(EtfFlowsScreen& screen) {
    auto* tabs = widget<QTabWidget>(screen, "etfTabs");
    const int previous = tabs->currentIndex();
    tabs->setCurrentIndex(5);
    const auto text = widget<QPlainTextEdit>(screen, "etfProvenance")->toPlainText();
    tabs->setCurrentIndex(previous);
    return text;
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
void capture_synthetic(EtfFlowsScreen& screen, const QString& name) {
    const QString output = qEnvironmentVariable("MARKETLAB_ETF_UI_SYNTHETIC_CAPTURE_DIR");
    if (output.isEmpty())
        return;
    QVERIFY(QDir().mkpath(output));
    auto* banner = screen.findChild<QLabel*>("syntheticFixtureBanner");
    if (!banner) {
        banner = new QLabel(
            "SYNTHETIC TEST FIXTURE · invented observations / identity declarations · not market evidence", &screen);
        banner->setObjectName("syntheticFixtureBanner");
        banner->setWordWrap(true);
        static_cast<QVBoxLayout*>(screen.layout())->insertWidget(0, banner);
    }
    widget<QPushButton>(screen, "etfExactIndividual")->setChecked(false);
    screen.show();
    QTest::qWait(30);
    QVERIFY(screen.grab().save(output + '/' + name + ".png"));
}
bool seed_rotation(qint64 con_id, const char* symbol, const QDate& first = QDate(2026, 8, 3),
                   const QDate& last = QDate(2026, 11, 30), const char* seen = "2026-12-02T23:00:00.000Z") {
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
    const auto days = UsEquityCalendar::weekdays_in(first, last);
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
        screen.setAttribute(Qt::WA_DontShowOnScreen);
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
        screen.setAttribute(Qt::WA_DontShowOnScreen);
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
        auto* individual = detail_view(screen);
        const int net = row_containing(individual, 1, "Net flow");
        QVERIFY(net >= 0);
        QCOMPARE(individual->model()->index(net, 2).data().toString(), "USD 0.00");
        const auto provenance = provenance_text(screen);
        QVERIFY(provenance.contains("regulatory_flow_analytics_v1"));
        QVERIFY(provenance.contains("0001193125-26-990001"));
        QCOMPARE(store_digest(), before);
        QVERIFY(filing(spy, "0000884394", "0001193125-26-990002", "NPORT-P/A", "2026-12-04T15:00:00.000Z",
                       "2026-12-06T00:00:00.000Z", 10.0, "0001193125-26-990001"));
        widget<QPushButton>(screen, "etfRecompute")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 15000);
        QCOMPARE(overview->item(0, 1)->text(), "USD 10.00");
        QVERIFY(overview->item(0, 3)->text().contains("revised inputs"));
        QVERIFY(provenance_text(screen).contains("filing_vintages"));
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
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen);
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        screen.resize(1440, 1000);
        capture_synthetic(screen, "cross_asset_synthetic");
        select_group(screen, "Commodity");
        const auto* overview = widget<QTableWidget>(screen, "etfOverview");
        const int commodity = row_containing(widget<QTableWidget>(screen, "etfOverview"), 0, "Commodity");
        QCOMPARE(overview->item(commodity, 1)->text(), "Unavailable");
        QCOMPARE(overview->item(commodity, 2)->text(), "Unavailable");
        QVERIFY(overview->item(commodity, 6)->text().contains("1 stale"));
        QVERIFY(!overview->item(commodity, 6)->text().contains("USD"));
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
        QVERIFY(detail_view(screen)->model()->rowCount() > 0);
        QVERIFY(widget<QLabel>(screen, "etfIndividualStatus")->text().contains("exchange sessions"));
        // The monthly attribution row remains missing rather than zero.
        const int missing_flow = row_containing(detail_view(screen), 1, "Net flow");
        QVERIFY(missing_flow >= 0);
        QCOMPARE(detail_view(screen)->model()->index(missing_flow, 2).data().toString(), "Unavailable / not counted");
        capture_synthetic(screen, "individual_rotation_synthetic");
        QCOMPARE(widget<QTableWidget>(screen, "etfUnclassified")->rowCount(), 1);
        select_group(screen, "Equity");
        const int inverse = row_containing(rotation, 0, "738523410");
        QVERIFY(inverse >= 0);
        QVERIFY(rotation->item(inverse, 1)->text().contains("Excluded"));
        QCOMPARE(rotation->item(inverse, 3)->text(), "Unavailable");
        QVERIFY(rotation->item(inverse, 8)->text().contains("-1x"));
        rotation->setCurrentCell(inverse, 0);
        emit rotation->itemActivated(rotation->item(inverse, 0));
        QCOMPARE(detail_view(screen)->model()->rowCount(), 0);
        QVERIFY(widget<QLabel>(screen, "etfIndividualStatus")->text().contains("excluded"));
        capture_synthetic(screen, "leveraged_inverse_excluded_synthetic");
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
        screen.setAttribute(Qt::WA_DontShowOnScreen);
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
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen, "category", "physical_gold");
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        widget<QTabWidget>(screen, "etfTabs")->setCurrentIndex(1);
        screen.resize(1440, 1000);
        screen.show();
        QTest::qWait(30);
        auto* splitter = screen.findChild<QSplitter*>();
        splitter->setChildrenCollapsible(false);
        splitter->setHandleWidth(11);
        const auto saved = screen.save_state();
        QCOMPARE(saved.value("group").toString(), "physical_gold");
        QCOMPARE(saved.value("tab").toInt(), 1);
        auto* other = new EtfFlowsScreen;
        other->setAttribute(Qt::WA_DontShowOnScreen);
        QJsonObject json;
        for (auto it = saved.constBegin(); it != saved.constEnd(); ++it)
            json.insert(it.key(), QJsonValue::fromVariant(it.value()));
        const auto persisted = QJsonDocument::fromJson(QJsonDocument(json).toJson()).object().toVariantMap();
        QCOMPARE(QByteArray::fromBase64(persisted.value("splitter_base64").toString().toLatin1()),
                 splitter->saveState());
        QSignalSpy other_loaded(other, &EtfFlowsScreen::research_loaded);
        other->restore_state(persisted);
        QCOMPARE(other->save_state().value("group"), saved.value("group"));
        other->resize(screen.size());
        other->show();
        QTRY_COMPARE_WITH_TIMEOUT(other_loaded.count(), 1, 15000);
        QTest::qWait(30);
        auto* restored_splitter = other->findChild<QSplitter*>();
        QCOMPARE(restored_splitter->childrenCollapsible(), splitter->childrenCollapsible());
        QCOMPARE(restored_splitter->handleWidth(), splitter->handleWidth());
        QCOMPARE(widget<QTabWidget>(*other, "etfTabs")->currentIndex(), 1);
        QCOMPARE(restored_splitter->sizes(), splitter->sizes());
        for (const auto* key : {"from", "to"})
            QCOMPARE(other->save_state().value(key).toDate(), saved.value(key).toDate());
        for (const auto* key : {"as_of", "known_at"})
            QCOMPARE(other->save_state().value(key).toDateTime(), saved.value(key).toDateTime());
        other->resize(740, 850);
        other->show();
        QTest::qWait(20);
        other->resize(1600, 1100);
        delete other;
        QThreadPool::globalInstance()->waitForDone();
    }
    void linked_listed_regulatory_history_and_rotation() {
        const char* seen = "2026-12-02T00:00:00.000Z";
        const qint64 reporting = entity("0009999999", "", seen);
        QVERIFY(seed_rotation(51529211, "GLD"));
        const qint64 gold = instrument(51529211, "GLD", seen);
        QVERIFY(filing(reporting, "0009999999", "0001193125-26-991001", "NPORT-P", "2026-11-25T15:00:00.000Z", seen,
                       123.0));
        QVERIFY(repo()
                    .declare_link(gold, reporting, {}, LinkRelationship::RegistrantIsInstrument,
                                  "synthetic one-to-one declaration", utc("2026-12-04T00:00:00.000Z"))
                    .is_ok());
        auto req = request("2026-12-07T22:00:00.000Z");
        req.group_level = "category";
        req.group_id = "physical_gold";
        const auto grouped = etf_ui::load_groups(req);
        QVERIFY(grouped.is_ok());
        const auto monthly = etf_ui::last_month(grouped.value().value("groups").toArray()[0].toObject());
        const auto member = monthly.value("constituents").toArray()[0].toObject();
        QCOMPARE(member.value("status").toString(), "observed");
        QCOMPARE(member.value("identity_basis").toString(), "declared_link");
        const auto before = store_digest();
        EtfFlowsScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen, "category", "physical_gold");
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        auto* members = widget<QTableWidget>(screen, "etfConstituents");
        members->setCurrentCell(0, 0);
        emit members->itemActivated(members->item(0, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 15000);
        auto* individual = detail_view(screen);
        const int net = row_containing(individual, 1, "Net flow");
        QVERIFY2(net >= 0, "Batch D has observed linked regulatory flow, but individual ETF history drops it");
        QCOMPARE(individual->model()->index(net, 0).data().toString(), "2026-10");
        QCOMPARE(individual->model()->index(net, 2).data().toString(), "USD 123.00");
        QVERIFY(individual->model()->index(net, 5).data().toString().contains(
            member.value("net_flow").toObject().value("available_from").toString()));
        QVERIFY(row_containing(individual, 1, "Price return 21") >= 0);
        const auto provenance = provenance_text(screen);
        QVERIFY(provenance.contains("synthetic one-to-one declaration"));
        QVERIFY(provenance.contains("0001193125-26-991001"));
        QCOMPARE(individual->model()->index(net, 6).data().toString(), "SEC regulatory flow (monthly)");
        const int price = row_containing(individual, 1, "Price return 21");
        QCOMPARE(individual->model()->index(price, 6).data().toString(), "Market rotation (sessions)");
        QCOMPARE(individual->model()->index(net, 0).data(Qt::UserRole).toJsonObject().value("constituent").toObject(),
                 member);
        screen.resize(1440, 1000);
        widget<QPushButton>(screen, "etfIndividualMonthly")->click();
        capture_synthetic(screen, "individual_linked_regulatory_synthetic");
        configure(screen, "category", "physical_gold", QDate(2026, 10, 1), "2026-12-03T22:00:00.000Z");
        widget<QPushButton>(screen, "etfRecompute")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 15000);
        members->setCurrentCell(0, 0);
        emit members->itemActivated(members->item(0, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 4, 15000);
        detail_view(screen);
        const int before_link = row_containing(individual, 1, "Net flow");
        QVERIFY(before_link >= 0);
        QCOMPARE(individual->model()->index(before_link, 2).data().toString(), "Unavailable / not counted");
        QCOMPARE(store_digest(), before);
    }
    void linked_listed_refusals_data() {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("ambiguous") << QString("ambiguous");
        QTest::newRow("multi_class") << QString("multi_class");
        QTest::newRow("duplicate") << QString("duplicate");
    }
    void linked_listed_refusals() {
        QFETCH(QString, scenario);
        const bool duplicate = scenario == "duplicate";
        const char* seen = "2026-12-02T00:00:00.000Z";
        const char* cik = duplicate ? "0000884394" : "0009999999";
        const qint64 reporting = entity(cik, scenario == "multi_class" ? "S000999999" : "", seen);
        const qint64 con_id = duplicate ? 756733 : 51529211;
        QVERIFY(seed_rotation(con_id, "SYNTHETIC"));
        const qint64 listed = instrument(con_id, "SYNTHETIC", seen);
        QVERIFY(filing(reporting, cik, "0001193125-26-991002", "NPORT-P", "2026-11-25T15:00:00.000Z", seen, 456.0));
        QVERIFY(repo()
                    .declare_link(listed, reporting, scenario == "multi_class" ? "C000001" : "",
                                  scenario == "multi_class" ? LinkRelationship::ClassOfMultiClassSeries
                                                            : LinkRelationship::RegistrantIsInstrument,
                                  "synthetic refusal fixture", utc("2026-12-04T00:00:00.000Z"))
                    .is_ok());
        if (scenario == "ambiguous") {
            const qint64 competitor = instrument(15547841, "SYNTHETIC", seen);
            QVERIFY(repo()
                        .declare_link(competitor, reporting, {}, LinkRelationship::RegistrantIsInstrument,
                                      "synthetic competing claim", utc("2026-12-04T00:00:00.000Z"))
                        .is_ok());
        }
        const auto before = store_digest();
        EtfFlowsScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen, "category", duplicate ? "us_large_cap" : "physical_gold");
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        auto* members = widget<QTableWidget>(screen, "etfConstituents");
        const int row = row_containing(members, 0, QString::number(con_id));
        QVERIFY(row >= 0);
        const auto member = members->item(row, 0)->data(Qt::UserRole).toJsonObject();
        QCOMPARE(member.value("status").toString(), duplicate ? "duplicate_reporting_identity" : "unresolved");
        members->setCurrentCell(row, 0);
        emit members->itemActivated(members->item(row, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 15000);
        auto* individual = detail_view(screen);
        const int net = row_containing(individual, 1, "Net flow");
        QVERIFY(net >= 0);
        QCOMPARE(individual->model()->index(net, 2).data().toString(), "Unavailable / not counted");
        QVERIFY(individual->model()->index(net, 3).data().toString().contains(duplicate ? "Duplicate reporting identity"
                                                                                        : "Unresolved"));
        QCOMPARE(individual->model()->index(net, 0).data(Qt::UserRole).toJsonObject().value("constituent").toObject(),
                 member);
        QCOMPARE(store_digest(), before);
    }
    void multi_month_history_chart_and_synthetic_captures() {
        const char* seen = "2026-12-02T00:00:00.000Z";
        const qint64 spy = entity("0000884394", "", seen);
        const QDate first(2025, 9, 1);
        for (int i = 0; i < 14; ++i) {
            if (i == 12) // September 2026 remains genuinely missing.
                continue;
            const QByteArray accession =
                QString("0001193125-26-%1").arg(992000 + i, 6, 10, QLatin1Char('0')).toLatin1();
            const double net = i == 10 ? -50.0 : i == 11 ? 0.0 : i == 13 ? 25.0 : 100.0;
            QVERIFY(filing(spy, "0000884394", accession.constData(), "NPORT-P", "2026-11-25T15:00:00.000Z", seen, net,
                           "", first.addMonths(i)));
        }
        const auto before = store_digest();
        EtfFlowsScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen, "complex", "sp500");
        screen.restore_state({{"from", first}, {"to", QDate(2026, 10, 1)}});
        screen.resize(1440, 1100);
        screen.show();
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        auto* months = widget<QTableWidget>(screen, "etfMonths");
        QCOMPARE(months->rowCount(), 14);
        auto req = request("2026-12-07T22:00:00.000Z");
        req.output_from = first;
        const auto expected = etf_ui::load_groups(req);
        QVERIFY(expected.is_ok());
        const auto history =
            expected.value().value("groups").toArray()[0].toObject().value("regulatory_months").toArray();
        for (int i = 0; i < 14; ++i) {
            QCOMPARE(months->item(i, 0)->text(), first.addMonths(i).toString("yyyy-MM"));
            QCOMPARE(months->item(i, 0)->data(Qt::UserRole).toJsonObject(), history[i].toObject());
        }
        QCOMPARE(months->item(10, 1)->text(), "USD -50.00");
        QCOMPARE(months->item(11, 1)->text(), "USD 0.00");
        QCOMPARE(months->item(12, 1)->text(), "Unavailable");
        QCOMPARE(months->item(13, 1)->text(), "USD 25.00");
        QVERIFY(months->item(13, 3)->text().contains("Partial"));
        auto* chart = widget<EtfMonthlyChart>(screen, "etfMonthlyHistoryChart");
        QVERIFY(chart);
        chart->resize(1300, 360);
        const auto displayed = chart->displayed_months();
        QCOMPARE(displayed.size(), 12);
        QCOMPARE(displayed[0].toObject().value("month").toString(), "2025-11");
        QCOMPARE(displayed.last().toObject().value("month").toString(), "2026-10");
        QVERIFY(chart->toolTip().contains("2026-08: USD 0.00"));
        QVERIFY(chart->toolTip().contains("2026-09: Unavailable"));
        // The real painter must put signed bars on opposite sides of its axis;
        // missing retains a gap and zero only the baseline marker. Partial
        // hatch has both accent and background pixels within the bar region.
        // Fix the paint viewport independently of the screen's layout manager
        // and the desktop's maximum window height / high-DPI scaling.
        EtfMonthlyChart painted;
        painted.setAttribute(Qt::WA_DontShowOnScreen);
        painted.set_months(history);
        painted.resize(1300, 360);
        painted.show();
        QTest::qWait(30);
        const auto image = painted.grab().toImage();
        if (!qEnvironmentVariable("MARKETLAB_ETF_UI_SYNTHETIC_CAPTURE_DIR").isEmpty())
            image.save(qEnvironmentVariable("MARKETLAB_ETF_UI_SYNTHETIC_CAPTURE_DIR") + "/chart_painter_synthetic.png");
        const QColor accent(ui::ThemeManager::instance().tokens().accent);
        const auto accent_pixels = [&](int month_index, bool above) {
            const auto plot = painted.plot_rect();
            const double step = plot.width() / 12;
            const int x = qRound(plot.left() + month_index * step + step / 2);
            const int mid = qRound(plot.center().y());
            int count = 0;
            for (int y = above ? mid - 35 : mid + 3; y < (above ? mid - 3 : mid + 35); ++y)
                if (image.pixelColor(qRound(x * image.devicePixelRatio()) + 1, qRound(y * image.devicePixelRatio()))
                        .rgb() == accent.rgb())
                    ++count;
            return count;
        };
        QVERIFY(accent_pixels(8, false) > 0); // July negative
        QCOMPARE(accent_pixels(8, true), 0);
        QCOMPARE(accent_pixels(9, false), 0);  // August zero
        QCOMPARE(accent_pixels(10, true), 0);  // September missing
        QVERIFY(accent_pixels(11, true) > 0);  // October partial positive
        QVERIFY(accent_pixels(11, true) < 32); // hatched, not a solid full-group bar
        capture_synthetic(screen, "monthly_partial_missing_synthetic");
        auto* members = widget<QTableWidget>(screen, "etfConstituents");
        const int row = row_containing(members, 0, "0000884394/");
        QVERIFY(row >= 0);
        members->setCurrentCell(row, 0);
        emit members->itemActivated(members->item(row, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 15000);
        capture_synthetic(screen, "individual_sec_synthetic");
        widget<QTabWidget>(screen, "etfTabs")->setCurrentIndex(0);
        screen.resize(740, 900);
        capture_synthetic(screen, "narrow_monthly_synthetic");
        chart->resize(650, 360);
        QCOMPARE(chart->displayed_months().size(), 6);
        QCOMPARE(chart->displayed_months()[0].toObject().value("month").toString(), "2026-05");
        QCOMPARE(store_digest(), before);
    }
    void graphical_hierarchy_navigation_and_replay() {
        const char* seen = "2026-12-02T00:00:00.000Z";
        const qint64 spy = entity("0000884394", "", seen);
        QVERIFY(filing(spy, "0000884394", "0001193125-26-993001", "NPORT-P", "2026-11-25T15:00:00.000Z", seen, 321.0));
        QVERIFY(seed_rotation(51529211, "GLD"));
        QVERIFY(seed_rotation(738523410, "SH"));
        const auto before = store_digest();
        EtfFlowsScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen);
        screen.resize(1440, 1000);
        screen.show();
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        auto* board = screen.findChild<EtfGroupBoard*>();
        QVERIFY(board);
        QCOMPARE(board->groups().size(), 6);
        QVERIFY(widget<QTableWidget>(screen, "etfOverview")->isHidden());
        QVERIFY(widget<QDateTimeEdit>(screen, "etfAsOf")->isHidden() ||
                !widget<QDateTimeEdit>(screen, "etfAsOf")->isVisible());
        QSet<QString> classes;
        for (int i = 0; i < 6; ++i) {
            const auto group = board->groups()[i].toObject();
            classes.insert(group.value("group_id").toString());
            const auto monthly = board->month_for(group);
            QVERIFY(monthly.contains("observed_net_flow_usd"));
            QVERIFY(board->rect().contains(board->card_rect(i)));
            QCOMPARE(board->rotation_summary(group), etf_ui::rotation_availability(group));
            if (group.value("group_id").toString() == QLatin1String("equity"))
                QVERIFY(board->rotation_summary(group).contains("2 excluded"));
        }
        QCOMPARE(classes, QSet<QString>({"equity", "fixed_income", "commodity", "currency", "crypto", "real_estate"}));
        QVERIFY(board->accessibleDescription().contains("USD 321.00"));
        capture_synthetic(screen, "cross_asset_all_classes_synthetic");
        screen.resize(740, 950);
        QTest::qWait(30);
        for (int i = 0; i < 6; ++i)
            QVERIFY(board->rect().contains(board->card_rect(i)));
        QCOMPARE(widget<QScrollArea>(screen, "etfBoardScroll")->horizontalScrollBar()->maximum(), 0);
        QCOMPARE(widget<QScrollArea>(screen, "etfResearchScroll")->horizontalScrollBar()->maximum(), 0);
        auto* tabs = widget<QTabWidget>(screen, "etfTabs");
        QCOMPARE(tabs->tabText(0), "Flow");
        QCOMPARE(tabs->tabText(1), "Rotation");
        for (int tab = 0; tab < tabs->count(); ++tab) {
            QVERIFY(!tabs->tabText(tab).contains("..."));
            QVERIFY(!tabs->tabToolTip(tab).isEmpty());
            QVERIFY(tabs->tabBar()->tabRect(tab).width() >=
                    tabs->tabBar()->fontMetrics().horizontalAdvance(tabs->tabText(tab)));
        }
        capture_synthetic(screen, "narrow_cross_asset_synthetic");
        screen.resize(1440, 1000);
        int equity = -1;
        for (int i = 0; i < 6; ++i)
            if (board->groups()[i].toObject().value("group_id").toString() == "equity")
                equity = i;
        QVERIFY(equity >= 0);
        QTest::mouseClick(board, Qt::LeftButton, Qt::NoModifier, board->card_rect(equity).center());
        QVERIFY(widget<QPushButton>(screen, "etfNavigate_us_large_cap"));
        QVERIFY(widget<QPushButton>(screen, "etfNavigate_sp500"));
        widget<QPushButton>(screen, "etfNavigate_us_large_cap")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 15000);
        QCOMPARE(board->groups().size(), 1);
        QCOMPARE(board->groups()[0].toObject().value("group_id").toString(), "us_large_cap");
        capture_synthetic(screen, "category_drilldown_synthetic");
        QVERIFY(widget<QPushButton>(screen, "etfNavigate_sp500"));
        widget<QPushButton>(screen, "etfNavigate_sp500")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 15000);
        QCOMPARE(board->groups()[0].toObject().value("group_id").toString(), "sp500");
        capture_synthetic(screen, "complex_drilldown_synthetic");
        widget<QPushButton>(screen, "etfBrowse_category")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 4, 15000);
        auto* groups = widget<QComboBox>(screen, "etfGroup");
        groups->setCurrentIndex(groups->findData("inverse_us_large_cap"));
        widget<QPushButton>(screen, "etfRecompute")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 5, 15000);
        widget<QTabWidget>(screen, "etfTabs")->setCurrentIndex(1);
        QVERIFY(widget<QPushButton>(screen, "etfRotationSubject_738523410")->text().contains("Excluded"));
        QVERIFY(widget<QLabel>(screen, "etfRotationStatus")->text().contains("Excluded is not zero"));
        QVERIFY(widget<QTabWidget>(screen, "etfTabs")->maximumHeight() < 400);
        QTest::qWait(30);
        auto* compact_tabs = widget<QTabWidget>(screen, "etfTabs");
        auto* selection = widget<QLabel>(screen, "etfSelection");
        QVERIFY(compact_tabs->geometry().top() - selection->geometry().bottom() < 30);
        auto* compact_splitter = screen.findChild<QSplitter*>();
        QVERIFY2(compact_splitter->height() < 650,
                 qPrintable(QString("splitter %1 min %2 max %3; tabs %4 max %5; board min %6")
                                .arg(compact_splitter->height())
                                .arg(compact_splitter->minimumHeight())
                                .arg(compact_splitter->maximumHeight())
                                .arg(compact_tabs->height())
                                .arg(compact_tabs->maximumHeight())
                                .arg(widget<QScrollArea>(screen, "etfBoardScroll")->minimumHeight())));
        capture_synthetic(screen, "leveraged_inverse_excluded_synthetic");

        widget<QPushButton>(screen, "etfAdvancedReplay")->click();
        QVERIFY(widget<QDateTimeEdit>(screen, "etfAsOf")->isVisible());
        widget<QPushButton>(screen, "etfAdvancedReplay")->click();
        QVERIFY(!widget<QDateTimeEdit>(screen, "etfAsOf")->isVisible());
        QCOMPARE(store_digest(), before);
    }
    void session_chart_observed_ranges_and_references_data() {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("flat") << QStringLiteral("flat");
        QTest::newRow("signed_and_gap") << QStringLiteral("mixed");
        QTest::newRow("all_missing") << QStringLiteral("missing");
    }
    void session_chart_observed_ranges_and_references() {
        QFETCH(QString, scenario);
        const QStringList fields{"price_return_21", "trend_efficiency_21", "return_acceleration_21",
                                 "volume_ratio_5_63"};
        const QStringList units{"price_return_ratio", "efficiency_ratio_minus1_1", "price_return_difference",
                                "self_relative_volume_ratio"};
        const double flat[]{0.2, 1.0, 0.0, 1.0};
        const double mixed[4][4]{
            {-0.1, 0.2, 0.0, 0.05}, {1.0, 1.0, 0.0, 1.0}, {0.02, -0.04, 0.0, 0.01}, {0.8, 1.2, 0.0, 1.05}};
        QJsonArray sessions;
        for (int day = 0; day < 4; ++day) {
            QJsonObject values;
            for (int panel = 0; panel < 4; ++panel) {
                const bool missing = scenario == "missing" || (scenario == "mixed" && day == 2);
                values.insert(
                    fields[panel],
                    QJsonObject{{"value", missing ? QJsonValue()
                                                  : QJsonValue(scenario == "flat" ? flat[panel] : mixed[panel][day])},
                                {"units", units[panel]},
                                {"state", missing ? "MISSING" : "PROXY"}});
            }
            sessions.append(
                QJsonObject{{"session", QDate(2026, 10, 1).addDays(day).toString(Qt::ISODate)}, {"values", values}});
        }
        EtfSessionChart chart;
        chart.setAttribute(Qt::WA_DontShowOnScreen);
        chart.set_sessions(sessions);
        chart.resize(1200, 500);
        chart.show();
        QTest::qWait(30);
        QCOMPARE(chart.sessions(), sessions);
        QVERIFY(chart.toolTip().size() < 1500);
        for (int panel = 0; panel < 4; ++panel) {
            const auto state = chart.panel_state(panel);
            QCOMPARE(state.value("reference").toDouble(), panel == 3 ? 1.0 : 0.0);
            if (scenario == "missing") {
                QVERIFY(state.value("observed_min").isNull());
                QVERIFY(state.value("observed_max").isNull());
                QCOMPARE(state.value("value_label").toString(), "Unavailable");
                QCOMPARE(state.value("range_label").toString(), "Observed range unavailable");
            } else if (scenario == "flat") {
                QCOMPARE(state.value("observed_min").toDouble(), flat[panel]);
                QCOMPARE(state.value("observed_max").toDouble(), flat[panel]);
                const auto formatted = etf_ui::number(flat[panel], units[panel]);
                QCOMPARE(state.value("value_label").toString(), formatted);
                QCOMPARE(state.value("range_label").toString(), "Observed: " + formatted + " to " + formatted);
            } else {
                const double minimum = std::min({mixed[panel][0], mixed[panel][1], mixed[panel][3]});
                const double maximum = std::max({mixed[panel][0], mixed[panel][1], mixed[panel][3]});
                QCOMPARE(state.value("observed_min").toDouble(), minimum);
                QCOMPARE(state.value("observed_max").toDouble(), maximum);
                QCOMPARE(state.value("range_label").toString(), "Observed: " + etf_ui::number(minimum, units[panel]) +
                                                                    " to " + etf_ui::number(maximum, units[panel]));
            }
        }
        if (scenario == "flat") {
            QCOMPARE(chart.panel_state(0).value("range_label").toString(), "Observed: 20.00% to 20.00%");
            QCOMPARE(chart.panel_state(1).value("range_label").toString(), "Observed: 1.00 to 1.00");
            QCOMPARE(chart.panel_state(2).value("value_label").toString(), "0.00 pp");
            QCOMPARE(chart.panel_state(3).value("value_label").toString(), "1.00x");
            QVERIFY(!chart.accessibleDescription().contains("0.5 to 1.5"));
        }
        const auto pixels = chart.grab().toImage();
        const auto plot = chart.panel_plot_rect(0);
        const auto accent = QColor(ui::ThemeManager::instance().tokens().accent);
        if (scenario == "mixed") {
            // No segment may bridge the null third session to the last observed point.
            const int x = qRound(plot.left() + plot.width() * 2.0 / 3.0);
            int colored = 0;
            for (int y = plot.top(); y <= plot.bottom(); ++y)
                colored +=
                    pixels.pixelColor(qRound(x * pixels.devicePixelRatio()), qRound(y * pixels.devicePixelRatio()))
                        .rgb() == accent.rgb();
            QCOMPARE(colored, 0);
        }
    }
    void linked_listed_revision_is_visible_in_chart_and_exact_model_data() {
        QTest::addColumn<double>("amended_flow");
        QTest::newRow("positive") << 173.0;
        QTest::newRow("zero") << 0.0;
    }
    void linked_listed_revision_is_visible_in_chart_and_exact_model() {
        QFETCH(double, amended_flow);
        const char* seen = "2026-12-02T00:00:00.000Z";
        const qint64 reporting = entity("0009999999", "", seen);
        QVERIFY(seed_rotation(51529211, "GLD"));
        const qint64 listed = instrument(51529211, "GLD", seen);
        QVERIFY(filing(reporting, "0009999999", "0001193125-26-994001", "NPORT-P", "2026-11-25T15:00:00.000Z", seen,
                       123.0));
        QVERIFY(repo()
                    .declare_link(listed, reporting, {}, LinkRelationship::RegistrantIsInstrument,
                                  "synthetic amended one-to-one link", utc("2026-12-04T00:00:00.000Z"))
                    .is_ok());
        QVERIFY(filing(reporting, "0009999999", "0001193125-26-994002", "NPORT-P/A", "2026-12-04T15:00:00.000Z",
                       "2026-12-06T00:00:00.000Z", amended_flow, "0001193125-26-994001"));
        const auto before = store_digest();
        EtfFlowsScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen, "category", "physical_gold");
        screen.resize(1440, 1000);
        screen.show();
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        auto* members = widget<QTableWidget>(screen, "etfConstituents");
        const auto expected = members->item(0, 0)->data(Qt::UserRole).toJsonObject();
        QCOMPARE(expected.value("net_flow").toObject().value("state").toString(), "REVISED");
        members->setCurrentCell(0, 0);
        emit members->itemActivated(members->item(0, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 15000);
        auto* chart = widget<EtfMonthlyChart>(screen, "etfIndividualFlowChart");
        QCOMPARE(chart->displayed_months().size(), 1);
        QCOMPARE(chart->displayed_months()[0].toObject().value("quality").toString(), "REVISED");
        QVERIFY(chart->displayed_months()[0].toObject().value("has_revised_inputs").toBool());
        QVERIFY(chart->accessibleDescription().contains("Revised"));
        auto* exact = detail_view(screen);
        const int net = row_containing(exact, 1, "Net flow");
        QVERIFY(net >= 0);
        QCOMPARE(exact->model()->index(net, 2).data().toString(), etf_ui::number(amended_flow, "USD"));
        QVERIFY(exact->model()->index(net, 3).data().toString().contains("Revised"));
        QCOMPARE(exact->model()->index(net, 0).data(Qt::UserRole).toJsonObject().value("constituent").toObject(),
                 expected);
        widget<QPushButton>(screen, "etfIndividualMonthly")->click();
        capture_synthetic(screen, amended_flow == 0.0 ? "individual_linked_revised_zero_regulatory_synthetic"
                                                      : "individual_linked_revised_regulatory_synthetic");
        QCOMPARE(store_digest(), before);
    }
    void selected_month_status_tracks_missing_and_measured_months() {
        const auto reporting = entity("0000884394", "", "2026-12-02T00:00:00.000Z");
        QVERIFY(filing(reporting, "0000884394", "0001193125-26-994010", "NPORT-P", "2026-11-25T15:00:00.000Z",
                       "2026-12-02T00:00:00.000Z", 17.0));
        EtfFlowsScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen, "complex", "sp500", QDate(2026, 9, 1));
        widget<QDateEdit>(screen, "etfTo")->setDate(QDate(2026, 10, 31));
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 15000);
        auto* month = widget<QComboBox>(screen, "etfSelectedMonth");
        QVERIFY(widget<QLabel>(screen, "etfStatus")->text().contains("2026-10: stored research ready"));
        month->setCurrentText("2026-09");
        QVERIFY(widget<QLabel>(screen, "etfStatus")->text().contains("2026-09: No measured regulatory flow"));
        month->setCurrentText("2026-10");
        QVERIFY(widget<QLabel>(screen, "etfStatus")->text().contains("2026-10: stored research ready"));
    }
    void maximum_window_rotation_detail_is_lazy_and_lossless() {
        QVERIFY(Database::instance().execute("BEGIN").is_ok());
        QVERIFY(seed_rotation(51529211, "GLD", QDate(2019, 1, 1), QDate(2028, 12, 31), "2029-01-03T23:00:00.000Z"));
        QVERIFY(Database::instance().execute("COMMIT").is_ok());
        const auto before = store_digest();
        EtfFlowsScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen);
        configure(screen, "category", "physical_gold", QDate(2019, 1, 1), "2029-02-10T22:00:00.000Z");
        widget<QDateEdit>(screen, "etfTo")->setDate(QDate(2028, 12, 31));
        screen.resize(1440, 1000);
        screen.show();
        QSignalSpy loaded(&screen, &EtfFlowsScreen::research_loaded);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 45000);
        auto* rotation = widget<QTableWidget>(screen, "etfRotation");
        const int row = row_containing(rotation, 0, "51529211");
        QVERIFY(row >= 0);
        QElapsedTimer heartbeat;
        heartbeat.start();
        qint64 longest_gap = 0;
        QTimer timer;
        timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &screen, [&] { longest_gap = std::max(longest_gap, heartbeat.restart()); });
        timer.start();
        rotation->setCurrentCell(row, 0);
        emit rotation->itemActivated(rotation->item(row, 0));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 45000);
        timer.stop();
        auto* chart = widget<EtfSessionChart>(screen, "etfSessionChart");
        auto* view = widget<QTableView>(screen, "etfIndividual");
        QVERIFY(chart->sessions().size() > 2000);
        QCOMPARE(view->model()->rowCount(), 0);
        QVERIFY(view->isHidden());
        QCOMPARE(widget<QPlainTextEdit>(screen, "etfProvenance")->toPlainText().size(), 0);
        QVERIFY(chart->toolTip().size() < 1500);
        int expected_rows = 0;
        for (const auto& session : chart->sessions())
            expected_rows += static_cast<int>(session.toObject().value("values").toObject().size());
        // One exact Batch D regulatory status per requested month is also retained.
        expected_rows += widget<QTableWidget>(screen, "etfMonths")->rowCount();
        QElapsedTimer opening;
        opening.start();
        widget<QPushButton>(screen, "etfExactIndividual")->setChecked(true);
        const auto opening_ms = opening.elapsed();
        QCOMPARE(view->model()->rowCount(), expected_rows);
        const auto last_row = view->model()->index(expected_rows - 1, 0);
        QCOMPARE(last_row.data().toString(), chart->sessions().last().toObject().value("session").toString());
        const auto exact_json = provenance_text(screen);
        const auto context = QJsonDocument::fromJson(exact_json.toUtf8()).object();
        const auto individual = context.value("selected_record").toObject().value("individual_research").toObject();
        const auto retained = individual.value("rotation_proxy")
                                  .toArray()[0]
                                  .toObject()
                                  .value("measures")
                                  .toObject()
                                  .value("sessions")
                                  .toArray();
        QCOMPARE(retained, chart->sessions());
        view->setCurrentIndex(last_row);
        const auto selected_context = QJsonDocument::fromJson(provenance_text(screen).toUtf8()).object();
        QCOMPARE(selected_context.value("individual_research").toObject(), individual);
        QCOMPARE(store_digest(), before);
        const auto output = qEnvironmentVariable("MARKETLAB_ETF_UI_SYNTHETIC_CAPTURE_DIR");
        if (!output.isEmpty()) {
            QFile report(output + "/maximum-window-performance.json");
            QVERIFY(report.open(QIODevice::WriteOnly));
            report.write(QJsonDocument(QJsonObject{{"requested_months", 120},
                                                   {"sessions", chart->sessions().size()},
                                                   {"exact_rows", expected_rows},
                                                   {"hidden_exact_rows_before_disclosure", 0},
                                                   {"hidden_provenance_chars", 0},
                                                   {"chart_summary_chars", chart->toolTip().size()},
                                                   {"open_model_elapsed_ms", opening_ms},
                                                   {"largest_heartbeat_gap_ms", longest_gap},
                                                   {"full_provenance_sessions_equal", true},
                                                   {"fixture", "synthetic"}})
                             .toJson());
        }
    }
    void representative_profile() {
        if (!representative_)
            QSKIP("Existing-profile replay is a separate configured local run");
        const auto before = store_digest();
        QVERIFY(!before.isEmpty());
        EtfFlowsScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen);
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
                QVERIFY(detail_view(screen)->model()->rowCount() > 0);
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
        QVERIFY(detail_view(screen)->model()->rowCount() > 0);
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
