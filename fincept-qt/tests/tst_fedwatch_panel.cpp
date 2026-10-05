// Native FedWatch workspace widgets + the shipped backend CLI over a temporary
// SQLite seeded from captured provider fixtures. Acquisition commands are
// answered by the test (no live HTTP); every local read runs the real Python.
#include "screens/economics/panels/FedWatchCharts.h"
#include "screens/economics/panels/FedWatchPanel.h"
#include "services/economics/EconomicsEnvelopeParse.h"
#include "ui/theme/Theme.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

using fincept::screens::FedWatchBandBars;
using fincept::screens::FedWatchHeatStrip;
using fincept::screens::FedWatchMatrix;
using fincept::screens::FedWatchOutcomeBars;
using fincept::screens::FedWatchPanel;
using fincept::screens::FedWatchPathChart;
using fincept::services::EconomicsResult;
namespace fw = fincept::screens::fedwatch;

namespace {
double luminance(const QColor& c) {
    const auto linear = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
}
double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
} // namespace

class TestFedWatchPanel : public QObject {
    Q_OBJECT
    struct Request {
        QString command;
        QStringList args;
        QString id;
    };
    QTemporaryDir temporary_;
    QString python_, database_, template_;
    QJsonObject snapshot_;
    QString clock_ = "2026-09-28T12:00:00Z";
    QJsonObject collect_override_;
    QList<Request> pending_, sent_;
    std::unique_ptr<FedWatchPanel> panel_;

    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Polish)
            if (auto* widget = qobject_cast<QWidget*>(object); widget && widget->isWindow())
                widget->setAttribute(Qt::WA_DontShowOnScreen);
        return QObject::eventFilter(object, event);
    }
    QJsonObject process(const QStringList& args) {
        QProcess child;
        child.start(python_,
                    QStringList{QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py")} + args);
        if (!child.waitForFinished(60000) || child.exitCode() != 0) {
            QTest::qFail(qPrintable("fixture failed: " + QString::fromUtf8(child.readAllStandardError())), __FILE__,
                         __LINE__);
            return {};
        }
        return QJsonDocument::fromJson(child.readAllStandardOutput()).object();
    }
    void makePanel() {
        panel_ =
            std::make_unique<FedWatchPanel>([this](const QString& command, const QStringList& args, const QString& id) {
                pending_.append({command, args, id});
                sent_.append({command, args, id});
            });
        panel_->setAttribute(Qt::WA_DontShowOnScreen);
        panel_->resize(1600, 1200);
        panel_->show();
    }
    QJsonObject backend(const Request& request) {
        if (request.command == "workspace")
            return process({database_, "workspace", clock_});
        if (request.command == "collect")
            return collect_override_.isEmpty() ? snapshot_ : collect_override_;
        if (request.command == "history_backfill")
            return QJsonObject{{"success", true},
                               {"data", QJsonObject{{"backfills", QJsonArray{QJsonObject{{"status", "OK"}},
                                                                             QJsonObject{{"status", "SKIPPED_FRESH"}}}},
                                                    {"errors", QJsonArray{}},
                                                    {"warnings", QJsonArray{}}}}};
        return QJsonObject{{"error", QJsonObject{{"error", "unexpected command"}, {"code", "TEST"}}}};
    }
    void flush() {
        int guard = 0;
        while (!pending_.isEmpty()) {
            QVERIFY2(++guard < 20, "unbounded command loop");
            const auto request = pending_.takeFirst();
            const auto envelope = backend(request);
            EconomicsResult result;
            result.source_id = "fedwatch";
            const auto decision = fincept::services::economics_detail::classify(envelope);
            result.success = decision.ok;
            result.data = envelope;
            result.error = decision.error;
            panel_->accept_result(request.id, result);
        }
    }
    void open() {
        panel_->activate();
        flush();
        QVERIFY(panel_->workspace().loaded);
    }
    QStringList commands() const {
        QStringList out;
        for (const auto& r : sent_)
            out << r.command;
        return out;
    }
    QString label(const char* name) { return panel_->findChild<QLabel*>(name)->text(); }
    QString table(const char* name) {
        auto* t = panel_->findChild<QTableWidget*>(name);
        QString text;
        for (int r = 0; r < t->rowCount(); ++r)
            for (int c = 0; c < t->columnCount(); ++c)
                if (auto* item = t->item(r, c))
                    text += item->text() + " | ";
        return text;
    }
    template <typename T>
    T* widget(const char* name) {
        return panel_->findChild<T*>(name);
    }

  private slots:
    void initTestCase() {
        qApp->installEventFilter(this);
        python_ = qEnvironmentVariable("FEDWATCH_TEST_PYTHON");
        QVERIFY2(!python_.isEmpty(), "Set FEDWATCH_TEST_PYTHON to the existing MarketLab Python runtime");
        QVERIFY(temporary_.isValid());
        template_ = temporary_.filePath("template.db");
        snapshot_ = process({template_});
        QVERIFY(snapshot_["success"].toBool());
    }
    void init() {
        database_ = temporary_.filePath(QString::fromLatin1(QTest::currentTestFunction()) + ".db");
        QFile::remove(database_);
        QVERIFY(QFile::copy(template_, database_));
        pending_.clear();
        sent_.clear();
        clock_ = "2026-09-28T12:00:00Z";
        collect_override_ = {};
        makePanel();
    }
    void cleanup() { panel_.reset(); }
    void cleanupTestCase() { qApp->removeEventFilter(this); }

    void opensFromStorageWithOneLocalRead() {
        open();
        QCOMPARE(commands(), QStringList{"workspace"});
        QVERIFY(sent_[0].args.contains("--db"));
        QCOMPARE(panel_->selected_meeting(), QString("2026-10-28"));
        auto* matrix = widget<FedWatchMatrix>("fedwatchMatrix");
        QVERIFY(matrix);
        QCOMPARE(matrix->row_ids().first(), QString("2026-10-28"));
        QCOMPARE(matrix->row_ids().size(), 10);
        QVERIFY(matrix->accessibleDescription().contains("expected"));
        QVERIFY(label("fedwatchKpiTargetValue").contains("3.75"));
        QCOMPARE(label("fedwatchKpiNextValue"), QString("Oct 28, 2026"));
        QVERIFY(label("fedwatchFocusTitle").startsWith("OCT 28, 2026 FOMC"));
        // Information is visible without any selector: no dropdowns, no line charts.
        for (auto* combo : panel_->findChildren<QComboBox*>())
            QVERIFY(!combo->isVisibleTo(panel_.get())); // only the base class's hidden table pager has one
        QVERIFY(panel_->findChildren<QWidget*>(QRegularExpression("Chart$|Matrix$")).size() >= 2);
        QVERIFY(panel_->findChildren<QObject*>(QRegularExpression("HistoryChart")).isEmpty());
    }
    void refreshCollectsBackfillsThenReloads() {
        open();
        sent_.clear();
        auto* refresh = panel_->findChild<QPushButton*>("econFetchBtn");
        refresh->click();
        QVERIFY(panel_->refreshing());
        QCOMPARE(refresh->isEnabled(), false);
        QCOMPARE(commands(), QStringList{"collect"});
        QVERIFY(!sent_[0].args.contains("--meeting")); // every upcoming meeting, one refresh
        flush();
        QCOMPARE(commands(), (QStringList{"collect", "history_backfill", "workspace"}));
        QVERIFY(!panel_->refreshing());
        QVERIFY(refresh->isEnabled());
        QVERIFY(label("fedwatchStatus").contains("Last refresh"));
        QVERIFY(label("fedwatchStatus").contains("Polymarket history: 1 ok, 1 skipped_fresh"));
    }
    void failedCollectKeepsStoredValuesVisible() {
        open();
        collect_override_ = QJsonObject{{"error", QJsonObject{{"error", "fixture outage"},
                                                              {"provider", "investing"},
                                                              {"code", "INVESTING_SOURCE_UNAVAILABLE"}}}};
        panel_->findChild<QPushButton*>("econFetchBtn")->click();
        flush();
        QVERIFY(label("fedwatchStatus").contains("Current collection failed"));
        QVERIFY(label("fedwatchStatus").contains("stored values remain available"));
        QCOMPARE(widget<FedWatchMatrix>("fedwatchMatrix")->row_ids().size(), 10);
    }
    void matrixAndChipsFocusMeetingsLocally() {
        open();
        sent_.clear();
        auto* matrix = widget<FedWatchMatrix>("fedwatchMatrix");
        matrix->repaint();
        const QRectF row = matrix->row_rect("2026-12-09");
        QVERIFY(!row.isEmpty());
        QTest::mouseClick(matrix, Qt::LeftButton, {}, row.center().toPoint());
        QCOMPARE(panel_->selected_meeting(), QString("2026-12-09"));
        QVERIFY(label("fedwatchFocusTitle").startsWith("DEC 9, 2026 FOMC"));
        QVERIFY(sent_.isEmpty()); // focus changes never acquire or re-read
        auto* chip = panel_->findChild<QPushButton*>("fedwatchMeetingChip_2026-09-16");
        QVERIFY(chip);
        chip->click();
        QCOMPARE(panel_->selected_meeting(), QString("2026-09-16"));
        QVERIFY(label("fedwatchFocusMeta").contains("Decision: Hike 25 bp"));
        chip = panel_->findChild<QPushButton*>("fedwatchMeetingChip_2026-09-16");
        chip->click(); // clicking the focused meeting keeps it focused
        QVERIFY(chip->isChecked());
        QVERIFY(sent_.isEmpty());
    }
    void currentDistributionsDriveFocusCharts() {
        open();
        const auto* october = panel_->workspace().find("2026-10-28");
        QVERIFY(october);
        QCOMPARE(october->fed.state, QString("CURRENT"));
        auto* bands = widget<FedWatchBandBars>("fedwatchBandBars");
        QCOMPARE(bands->bar_count(), october->fed.bands.size());
        auto* outcomes = widget<FedWatchOutcomeBars>("fedwatchOutcomeBars");
        QVERIFY(!outcomes->rows().isEmpty());
        bool hold_has_gap = false;
        for (const auto& row : outcomes->rows())
            if (row.bp == 0 && !row.open)
                hold_has_gap = row.diff.has_value() && row.fed.has_value() && row.poly.has_value();
        QVERIFY(hold_has_gap);
        QVERIFY(label("fedwatchBandNote").contains("Expected rate after this meeting"));
        QVERIFY(label("fedwatchBandNote").contains("30-Day Fed Funds futures price"));
    }
    void staleValuesAreLabelledAndNeverCompared() {
        clock_ = "2026-10-04T12:00:00Z";
        open();
        const auto* october = panel_->workspace().find("2026-10-28");
        QCOMPARE(october->fed.state, QString("STALE"));
        QVERIFY(!october->fed.bands.isEmpty()); // last stored values stay visible
        QVERIFY(october->comparison.isEmpty());
        for (const auto& row : widget<FedWatchOutcomeBars>("fedwatchOutcomeBars")->rows())
            QVERIFY(!row.diff.has_value());
        QVERIFY(widget<FedWatchMatrix>("fedwatchMatrix")->accessibleDescription().contains("stale"));
        QVERIFY(label("fedwatchStatus").contains("marked stale"));
        QVERIFY(label("fedwatchBandNote").contains("Stale: last stored distribution"));
    }
    void historyColumnsUseOnlyObservedDays() {
        open();
        const auto* october = panel_->workspace().find("2026-10-28");
        auto* fed = widget<FedWatchHeatStrip>("fedwatchFedHistory");
        auto* poly = widget<FedWatchHeatStrip>("fedwatchPolyHistory");
        // One row per outcome/target range; a cell per observed day only.
        QCOMPARE(poly->row_count(), 5);
        // Render off-viewport widgets directly; a clipped repaint() paints nothing.
        fed->grab();
        poly->grab();
        const auto& first_day = october->poly_days.first();
        const QString key = fw::outcome_key(first_day.outcomes.first().bp, first_day.outcomes.first().open);
        QVERIFY(poly->hit_text(poly->cell_center(first_day.date, key)).contains(first_day.date.toString(Qt::ISODate)));
        const QDate empty_day = first_day.date.addDays(-1);
        QVERIFY(empty_day < poly->domain().first || poly->hit_text(poly->cell_center(empty_day, key)).isEmpty());
        QCOMPARE(fed->day_count(), october->fed_days.size());
        QCOMPARE(poly->day_count(), october->poly_days.size());
        // Shared date domain spans both sources' observed days only.
        QCOMPARE(poly->domain(), fed->domain());
        QVERIFY(poly->domain().first <= october->poly_days.first().date);
        QVERIFY(poly->domain().second >= october->fed_days.last().date);
        // Resolved meetings show their stored history.
        panel_->select_meeting("2026-09-16");
        QCOMPARE(poly->day_count(), panel_->workspace().find("2026-09-16")->poly_days.size());
        QVERIFY(poly->day_count() > 0);
    }
    void changesAndSourcesAreTables() {
        open();
        const QString changes = table("fedwatchChanges");
        QVERIFY(changes.contains("Hold"));
        QVERIFY(changes.contains("Hike 25 bp"));
        const QString sources = table("fedwatchSources");
        QVERIFY(sources.contains("Investing.com Fed Rate Monitor"));
        QVERIFY(sources.contains("CME FedWatch tool"));
        QVERIFY(sources.contains("Not collected"));
        QVERIFY(panel_->findChild<QPushButton*>("fedwatchOpenCme"));
        QVERIFY(label("fedwatchMethodNotes").contains("not official CME"));
    }
    void stateRoundTripRestoresFocus() {
        open();
        panel_->select_meeting("2027-01-27");
        auto* audit = panel_->findChild<QToolButton*>("fedwatchAuditToggle");
        const auto state = panel_->save_panel_state();
        QCOMPARE(state.value("meeting").toString(), QString("2027-01-27"));
        cleanup();
        sent_.clear();
        makePanel();
        panel_->restore_panel_state(state);
        open();
        QCOMPARE(panel_->selected_meeting(), QString("2027-01-27"));
        Q_UNUSED(audit);
        auto* toggle = panel_->findChild<QToolButton*>("fedwatchAuditToggle");
        toggle->click();
        QVERIFY(panel_->findChild<QPlainTextEdit*>("fedwatchAudit")->toPlainText().contains("\"meetings\""));
    }
    void emptyStoreExplainsTheFirstRefresh() {
        QFile::remove(database_);
        open();
        QVERIFY(panel_->workspace().meetings.isEmpty());
        QVERIFY(label("fedwatchStatus").contains("No FedWatch observations are stored yet"));
        QCOMPARE(label("fedwatchKpiNextValue"), QString::fromUtf8("—"));
    }
    void heatScaleIsMonotoneAndVisiblePerSource() {
        const QColor surface(fincept::ui::colors::BG_SURFACE());
        for (int source : {0, 1}) {
            double previous = -1;
            for (int pct : {1, 10, 25, 50, 75, 100}) {
                const QColor c = fw::heat(pct, source);
                QVERIFY(luminance(c) > previous); // brighter = more likely
                previous = luminance(c);
            }
            QVERIFY2(contrast(fw::heat(100, source), surface) >= 3.0, "full probability stands out");
        }
        QCOMPARE(fw::heat(0), QColor(fincept::ui::colors::BG_RAISED()));
    }
};

QTEST_MAIN(TestFedWatchPanel)
#include "tst_fedwatch_panel.moc"
