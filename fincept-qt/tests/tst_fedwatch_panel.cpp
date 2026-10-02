// Native widget + shipped backend CLI integration with deterministic captured
// provider data. No live HTTP and no substitute history/analytics implementation.
#include "screens/economics/panels/FedWatchCurrentChart.h"
#include "screens/economics/panels/FedWatchHistoryChart.h"
#include "screens/economics/panels/FedWatchPanel.h"
#include "services/economics/EconomicsEnvelopeParse.h"
#include "ui/theme/Theme.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtTest>

using fincept::screens::FedWatchCurrentChart;
using fincept::screens::FedWatchHistoryChart;
using fincept::screens::FedWatchPanel;
using fincept::services::EconomicsResult;

class TestFedWatchPanel : public QObject {
    Q_OBJECT
    struct Request {
        QString command;
        QStringList args;
        QString id;
    };
    QTemporaryDir temporary_;
    QString python_, database_, database_template_;
    QJsonObject snapshot_;
    QList<Request> pending_, sent_;
    std::unique_ptr<FedWatchPanel> panel_;
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Polish)
            if (auto* widget = qobject_cast<QWidget*>(object); widget && widget->isWindow())
                widget->setAttribute(Qt::WA_DontShowOnScreen);
        return QObject::eventFilter(object, event);
    }
    void makePanel() {
        panel_ =
            std::make_unique<FedWatchPanel>([this](const QString& command, const QStringList& args, const QString& id) {
                const Request request{command, args, id};
                pending_.append(request);
                sent_.append(request);
            });
        panel_->setAttribute(Qt::WA_DontShowOnScreen);
    }
    FedWatchCurrentChart* currentChart() { return panel_->findChild<FedWatchCurrentChart*>("fedwatchCurrentChart"); }
    void captureFixture(const QString& name, int width = 1440) {
        const auto output = qEnvironmentVariable("FEDWATCH_CAPTURE_DIR");
        if (output.isEmpty())
            return;
        QVERIFY(QDir().mkpath(output));
        QWidget frame;
        frame.setAttribute(Qt::WA_DontShowOnScreen);
        QVBoxLayout layout(&frame);
        layout.addWidget(new QLabel("DETERMINISTIC TEST FIXTURE · not live market or policy-outcome evidence", &frame));
        panel_->hide();
        panel_->setParent(&frame);
        panel_->setAttribute(Qt::WA_DontShowOnScreen);
        layout.addWidget(panel_.get());
        panel_->show();
        frame.resize(width, 1440);
        frame.show();
        QTest::qWait(50);
        const bool hidden = frame.testAttribute(Qt::WA_DontShowOnScreen) &&
                            panel_->testAttribute(Qt::WA_DontShowOnScreen) && panel_->isVisible();
        const bool saved = frame.grab().save(QDir(output).filePath(name + "_fixture.png"));
        layout.removeWidget(panel_.get());
        panel_->setParent(nullptr);
        panel_->setAttribute(Qt::WA_DontShowOnScreen);
        panel_->hide();
        QVERIFY(hidden);
        QVERIFY(saved);
    }
    // Read and activate the real visible chip controls, rather than reaching
    // through the panel's internal selection models. Look up afresh after every
    // response because meeting/outcome buttons are rebuilt from backend data.
    struct SelectionControl {
        TestFedWatchPanel* test;
        QString kind;
        QList<QPushButton*> buttons() const {
            QList<QPushButton*> result;
            for (auto* button : test->panel_->findChildren<QPushButton*>())
                if (button->property("selection_kind").toString() == kind && !button->isHidden())
                    result.append(button);
            return result;
        }
        QVariant currentData() const {
            for (auto* button : buttons())
                if (button->isChecked())
                    return button->property("selection_value");
            return {};
        }
        QString currentText() const {
            for (auto* button : buttons())
                if (button->isChecked())
                    return button->text();
            return {};
        }
        int findData(const QVariant& value) const {
            const auto choices = buttons();
            for (int i = 0; i < choices.size(); ++i)
                if (choices[i]->property("selection_value") == value)
                    return i;
            return -1;
        }
        void selectIndex(int index) const {
            const auto choices = buttons();
            if (index < 0 || index >= choices.size()) {
                QTest::qFail("Requested backend choice has no actual chip control", __FILE__, __LINE__);
                return;
            }
            choices[index]->click();
        }
        int count() const { return buttons().size(); }
        bool isVisible() const {
            for (auto* button : buttons())
                if (button->isVisible())
                    return true;
            return false;
        }
    };
    QHash<QString, SelectionControl> controls_;

    QJsonObject process(const QString& script, const QStringList& args) {
        QProcess child;
        child.start(python_, QStringList{script} + args);
        if (!child.waitForFinished(30000) || child.exitCode() != 0) {
            QTest::qFail(qPrintable(QString("Backend helper failed: %1 %2\n%3\n%4")
                                        .arg(script, args.join(' '), QString::fromUtf8(child.readAllStandardError()),
                                             QString::fromUtf8(child.readAllStandardOutput()))),
                         __FILE__, __LINE__);
            return {};
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(child.readAllStandardOutput(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            QTest::qFail("Backend helper returned invalid JSON", __FILE__, __LINE__);
            return {};
        }
        return document.object();
    }
    QJsonObject backend(const Request& request) {
        if (request.command == "collect")
            return snapshot_;
        QStringList args{request.command};
        args.append(request.args);
        const int dbOption = args.indexOf("--db");
        if (dbOption >= 0)
            args[dbOption + 1] = database_;
        else
            args << "--db" << database_;
        if (request.command == "history_analytics")
            args << "--as-of" << "2026-09-28T12:00:00Z";
        return process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/scripts/fedwatch_data.py"), args);
    }
    void deliver(const Request& request, const QJsonObject& envelope) {
        EconomicsResult result;
        result.source_id = "fedwatch";
        const auto decision = fincept::services::economics_detail::classify(envelope);
        result.success = decision.ok;
        result.data = envelope;
        result.error = decision.error;
        panel_->accept_result(request.id, result);
    }
    void flush() {
        int count = 0;
        while (!pending_.isEmpty()) {
            QVERIFY2(++count < 40, "Unbounded command loop");
            const auto request = pending_.takeFirst();
            deliver(request, backend(request));
        }
    }
    SelectionControl* control(const char* name) {
        QString kind = QString::fromLatin1(name).mid(8).toLower();
        if (!controls_.contains(kind))
            controls_.insert(kind, {this, kind});
        return &controls_[kind];
    }
    QString text(const char* name) { return panel_->findChild<QLabel*>(name)->text(); }
    FedWatchHistoryChart* chart(const char* name) {
        return dynamic_cast<FedWatchHistoryChart*>(panel_->findChild<QWidget*>(name));
    }
    QString tableText(const char* name) {
        auto* table = panel_->findChild<QTableWidget*>(name);
        QString text;
        for (int row = 0; row < table->rowCount(); ++row)
            for (int col = 0; col < table->columnCount(); ++col)
                if (auto* item = table->item(row, col))
                    text += item->text() + " | ";
        return text;
    }
    QJsonObject rawDetails() {
        return QJsonDocument::fromJson(panel_->findChild<QPlainTextEdit*>("fedwatchDetails")->toPlainText().toUtf8())
            .object();
    }
    void openUpcoming() {
        panel_->restore_panel_state({{"meeting", "2026-10-28"},
                                     {"outcome_bp", 25},
                                     {"open_ended", false},
                                     {"fed_method", "LIVE_INVESTING_DERIVED"},
                                     {"range_days", 0}});
        panel_->activate();
        flush();
    }
    void changeCurrentSection(const QString& section, const QJsonObject& changes) {
        auto data = snapshot_["data"].toObject();
        auto meetings = data["meetings"].toArray();
        for (int i = 0; i < meetings.size(); ++i) {
            auto meeting = meetings[i].toObject();
            if (meeting["meeting_date"].toString() != "2026-10-28")
                continue;
            auto provider = meeting[section].toObject();
            for (auto entry = changes.begin(); entry != changes.end(); ++entry)
                provider[entry.key()] = entry.value();
            meeting[section] = provider;
            meetings[i] = meeting;
        }
        data["meetings"] = meetings;
        snapshot_["data"] = data;
    }
  private slots:
    void initTestCase() {
        qApp->installEventFilter(this);
        python_ = qEnvironmentVariable("FEDWATCH_TEST_PYTHON");
        QVERIFY2(!python_.isEmpty(), "Set FEDWATCH_TEST_PYTHON to the existing MarketLab Python runtime");
        QVERIFY(temporary_.isValid());
        database_template_ = temporary_.filePath("history-template.db");
        snapshot_ =
            process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"), {database_template_});
        QVERIFY(snapshot_["success"].toBool());
    }
    void init() {
        database_ = temporary_.filePath(QString::fromLatin1(QTest::currentTestFunction()) + "-" +
                                        QString::fromLatin1(QTest::currentDataTag()) + ".db");
        QVERIFY(QFile::copy(database_template_, database_));
        pending_.clear();
        sent_.clear();
        makePanel();
    }
    void cleanup() { panel_.reset(); }
    void cleanupTestCase() { qApp->removeEventFilter(this); }

    void upcomingConsumesRealContract() {
        openUpcoming();
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), QString("2026-10-28"));
        QVERIFY(text("fedwatchSummary").contains("3.75"));
        QVERIFY(text("fedwatchSummary").contains("4.00"));
        QVERIFY(text("fedwatchSummary").contains("2026-10-28"));
        QVERIFY(tableText("fedwatchDistribution").contains("65.50"));
        QVERIFY(
            panel_->findChild<QPlainTextEdit*>("fedwatchDetails")->toPlainText().contains("LIVE_INVESTING_DERIVED"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series().isEmpty());
        QVERIFY(sent_.first().command == "history_meetings");
        QVERIFY(std::any_of(sent_.begin(), sent_.end(), [](const auto& r) { return r.command == "collect"; }));
    }
    void rangeOnlyFiltersViewAndRestores() {
        openUpcoming();
        const auto full = chart("fedwatchProbabilityChart")->series();
        const int commands = sent_.size();
        auto* range = control("fedwatchRange");
        QVERIFY(range->findData(30) >= 0);
        range->selectIndex(range->findData(30));
        QCOMPARE(sent_.size(), commands);
        const auto shortRange = chart("fedwatchProbabilityChart")->series();
        QVERIFY(shortRange[0].points.size() < full[0].points.size());
        range->selectIndex(range->findData(0));
        QCOMPARE(chart("fedwatchProbabilityChart")->series()[0].points.size(), full[0].points.size());
        QCOMPARE(panel_->save_panel_state()["range_days"].toInt(), 0);
    }
    void resolvedSelectionAndRefreshNeverCollect() {
        panel_->restore_panel_state({{"meeting", "2026-09-16"}, {"outcome_bp", 25}});
        panel_->activate();
        flush();
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), QString("2026-09-16"));
        QVERIFY(text("fedwatchDiagnosticStatus").contains("Resolved", Qt::CaseInsensitive));
        QVERIFY(text("fedwatchSummary").contains("Actual policy outcome: 25"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
        auto* refresh = panel_->findChild<QPushButton*>("econFetchBtn");
        QVERIFY(refresh);
        refresh->click();
        flush();
        for (const auto& request : sent_)
            QVERIFY2(request.command != "collect" && request.command != "history_backfill",
                     qPrintable(request.command));
    }
    void onlyResolvedInventoryWithoutSelectionStillDiscoversUpcoming() {
        panel_->activate();
        QCOMPARE(pending_.size(), 1);
        const auto request = pending_.takeFirst();
        QCOMPARE(request.command, QString("history_meetings"));
        const auto realOverview = backend(request)["data"].toObject();
        QJsonArray resolvedOnly;
        for (const auto& item : realOverview["meetings"].toArray())
            if (item.toObject()["status"].toString() == "RESOLVED")
                resolvedOnly.append(item);
        QVERIFY(!resolvedOnly.isEmpty());
        deliver(request, QJsonObject{{"success", true}, {"data", QJsonObject{{"meetings", resolvedOnly}}}});
        QCOMPARE(pending_.size(), 1);
        QCOMPARE(pending_.first().command, QString("collect"));
        // Real captured current data and durable inventory provide upcoming
        // choices, without a second collect/discovery loop.
        flush();
        QCOMPARE(std::count_if(sent_.begin(), sent_.end(), [](const auto& item) { return item.command == "collect"; }),
                 1);
        QVERIFY(control("fedwatchMeeting")->findData("2026-10-28") >= 0);
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-10-28"));
        flush();
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), QString("2026-10-28"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
    }
    void explicitResolvedCanDiscoverUpcomingWithoutChangingLocalRefresh() {
        panel_->restore_panel_state({{"meeting", "2026-09-16"}, {"outcome_bp", 25}});
        panel_->activate();
        const auto overviewRequest = pending_.takeFirst();
        const auto realOverview = backend(overviewRequest)["data"].toObject();
        QJsonArray resolvedOnly;
        for (const auto& item : realOverview["meetings"].toArray())
            if (item.toObject()["status"].toString() == "RESOLVED")
                resolvedOnly.append(item);
        deliver(overviewRequest, QJsonObject{{"success", true}, {"data", QJsonObject{{"meetings", resolvedOnly}}}});
        flush();
        QVERIFY(std::none_of(sent_.begin(), sent_.end(), [](const auto& r) { return r.command == "collect"; }));
        auto* update = panel_->findChild<QPushButton*>("fedwatchUpdateUpcoming");
        QVERIFY(update);
        QVERIFY(update->isEnabled());
        update->click();
        QCOMPARE(pending_.size(), 1);
        QCOMPARE(pending_.first().command, QString("collect"));
        update->click();
        QCOMPARE(pending_.size(), 1);
        flush();
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), QString("2026-09-16"));
        QVERIFY(control("fedwatchMeeting")->findData("2026-10-28") >= 0);
        const int before = sent_.size();
        panel_->findChild<QPushButton*>("econFetchBtn")->click();
        flush();
        for (int i = before; i < sent_.size(); ++i) {
            QVERIFY(sent_[i].command != "collect");
            QVERIFY(sent_[i].command != "history_backfill");
        }
    }
    void repeatedRefreshCoalescesUntilDelayedCollectCompletes() {
        openUpcoming();
        auto* refresh = panel_->findChild<QPushButton*>("econFetchBtn");
        QVERIFY(refresh);
        refresh->click();
        QVERIFY(!pending_.isEmpty());
        const auto overview = pending_.takeFirst();
        QCOMPARE(overview.command, QString("history_meetings"));
        deliver(overview, backend(overview));
        QCOMPARE(pending_.size(), 1);
        QCOMPARE(pending_.first().command, QString("collect"));
        const auto commands = sent_.size();
        refresh->click();
        refresh->click();
        QCOMPARE(sent_.size(), commands);
        QCOMPARE(pending_.size(), 1);
        flush();
        QVERIFY(refresh->isEnabled());
    }
    void selectionDuringCollectStillReleasesRefreshGuard() {
        openUpcoming();
        auto* refresh = panel_->findChild<QPushButton*>("econFetchBtn");
        refresh->click();
        const auto overview = pending_.takeFirst();
        deliver(overview, backend(overview));
        QCOMPARE(pending_.first().command, QString("collect"));
        const auto delayedCollect = pending_.takeFirst();
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-09-16"));
        flush();
        deliver(delayedCollect, backend(delayedCollect));
        flush();
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), QString("2026-09-16"));
        QVERIFY(refresh->isEnabled());
        const int before = sent_.size();
        refresh->click();
        QVERIFY(sent_.size() > before);
        QCOMPARE(pending_.first().command, QString("history_meetings"));
        flush();
        for (int i = before; i < sent_.size(); ++i)
            QVERIFY(sent_[i].command != "collect");
    }
    void rangeUsesOneAnchorAcrossProbabilityAndDivergence() {
        openUpcoming();
        control("fedwatchMethod")->selectIndex(control("fedwatchMethod")->findData("HISTORICAL_ZQ_RECONSTRUCTED"));
        pending_.clear();
        control("fedwatchMethod")->selectIndex(control("fedwatchMethod")->findData("LIVE_INVESTING_DERIVED"));
        QVERIFY(!pending_.isEmpty());
        const auto request = pending_.takeFirst();
        auto envelope = backend(request);
        auto data = envelope["data"].toObject();
        // Deterministic UI-only timestamp case: latest one-sided probability is
        // Sep30 while the last actually paired observation is Aug1. Both charts
        // must use Sep30 for the same 7D window, retaining no old divergence.
        auto fed = data["fed_side"].toObject();
        fed["history"] = QJsonArray{QJsonObject{{"observed_at", "2026-09-30T12:00:00Z"}, {"probability_pct", 60}}};
        auto poly = data["polymarket"].toObject();
        poly["history"] = QJsonArray{QJsonObject{{"observed_at", "2026-08-01T12:00:00Z"}, {"probability_pct", 50}}};
        auto difference = data["difference"].toObject();
        difference["history"] = QJsonArray{QJsonObject{{"date", "2026-08-01"}, {"probability_diff_pp", -10}}};
        data["fed_side"] = fed;
        data["polymarket"] = poly;
        data["difference"] = difference;
        envelope["data"] = data;
        deliver(request, envelope);
        QCOMPARE(chart("fedwatchDivergenceChart")->series()[0].points.size(), 1);
        control("fedwatchRange")->selectIndex(control("fedwatchRange")->findData(7));
        QCOMPARE(chart("fedwatchProbabilityChart")->series()[0].points.size(), 1);
        QVERIFY(chart("fedwatchPolymarketChart")->series()[0].points.isEmpty());
        QVERIFY(chart("fedwatchDivergenceChart")->series()[0].points.isEmpty());
    }
    void methodSelectionKeepsZqSeparate() {
        openUpcoming();
        auto* method = control("fedwatchMethod");
        const auto index = method->findData("HISTORICAL_ZQ_RECONSTRUCTED");
        QVERIFY(index >= 0);
        method->selectIndex(index);
        flush();
        const auto series = chart("fedwatchProbabilityChart")->series();
        QVERIFY(series[0].label.contains("HISTORICAL_ZQ_RECONSTRUCTED"));
        QCOMPARE(series[0].points.size(), 1);
        QCOMPARE(series[0].points[0].value, 45.0);
        QVERIFY(tableText("fedwatchIndicators").contains("NO_PREVIOUS_OBSERVATION"));
        QVERIFY(tableText("fedwatchIndicators").contains("ZERO_WIDTH_RANGE"));
    }
    void outcomeSwitchRetainsBackendTailAndMissingCoverage() {
        openUpcoming();
        auto* outcome = control("fedwatchOutcome");
        QVERIFY(outcome->findData("-50:tail") >= 0);
        outcome->selectIndex(outcome->findData("-50:tail"));
        flush();
        QVERIFY(outcome->currentText().contains("or less"));
        const auto request = sent_.last();
        QCOMPARE(request.command, QString("history_analytics"));
        QVERIFY(request.args.contains("--open-ended"));
        QCOMPARE(request.args[request.args.indexOf("--outcome-bp") + 1], QString("-50"));
        QVERIFY(tableText("fedwatchIndicators").contains("INSUFFICIENT_HISTORY"));
        QCOMPARE(panel_->save_panel_state()["open_ended"].toBool(), true);
        outcome->selectIndex(outcome->findData("25:exact"));
        flush();
        QVERIFY(!sent_.last().args.contains("--open-ended"));
    }
    void divergenceMatchesActualPairedDaysAndSign() {
        openUpcoming();
        const auto series = chart("fedwatchDivergenceChart")->series();
        QVERIFY(!series.isEmpty());
        // Only Aug 1 and Sep 27 occur on both sides; Sep 26 and Sep 28
        // remain unpaired gaps, rather than being forward-filled.
        const auto envelope = backend({"history_analytics", {"--meeting", "2026-10-28", "--outcome-bp", "25"}, ""});
        const auto history = envelope["data"].toObject()["difference"].toObject()["history"].toArray();
        QCOMPARE(series[0].points.size(), history.size());
        QVERIFY(!history.isEmpty());
        QCOMPARE(series[0].points[0].value, -10.0);
        for (int i = 0; i < history.size(); ++i) {
            const auto row = history[i].toObject();
            QCOMPARE(series[0].points[i].value, row["probability_diff_pp"].toDouble());
            QCOMPARE(row["probability_diff_pp"].toDouble(),
                     row["polymarket_probability_pct"].toDouble() - row["fed_probability_pct"].toDouble());
        }
    }
    void loadingAndUnknownRepliesStayBounded() {
        panel_->activate();
        QVERIFY(text("fedwatchStatus").contains("Loading"));
        const auto before = text("fedwatchStatus");
        EconomicsResult result;
        result.success = true;
        result.source_id = "fedwatch";
        result.data = snapshot_;
        panel_->accept_result("unknown or another panel", result);
        QCOMPARE(text("fedwatchStatus"), before);
        QCOMPARE(pending_.size(), 1);
    }
    void emptyInventoryAndFailedCollectionStayExplicit() {
        panel_->activate();
        auto request = pending_.takeFirst();
        deliver(request, QJsonObject{{"success", true}, {"data", QJsonObject{{"meetings", QJsonArray{}}}}});
        QCOMPARE(pending_.size(), 1);
        request = pending_.takeFirst();
        QCOMPARE(request.command, QString("collect"));
        EconomicsResult error;
        error.source_id = "fedwatch";
        error.error = "FRED target unavailable; Investing source unavailable";
        panel_->accept_result(request.id, error);
        request = pending_.takeFirst();
        deliver(request, QJsonObject{{"success", true}, {"data", QJsonObject{{"meetings", QJsonArray{}}}}});
        QVERIFY(pending_.isEmpty());
        QVERIFY(text("fedwatchSummary").contains("select a meeting"));
        QVERIFY(text("fedwatchSourceStatus").contains("FRED target unavailable"));
        QVERIFY(!text("fedwatchSummary").contains("Unavailable"));
        QVERIFY(currentChart()->isHidden());
        QVERIFY(!chart("fedwatchProbabilityChart")->isVisible());
        QVERIFY(!chart("fedwatchPolymarketChart")->isVisible());
        for (const auto& series : chart("fedwatchProbabilityChart")->series())
            QVERIFY(series.points.isEmpty());
    }
    void staleAndUnverifiedProvidersRetainSpecificStates() {
        const auto original = snapshot_;
        auto data = snapshot_["data"].toObject();
        auto meetings = data["meetings"].toArray();
        for (int i = 0; i < meetings.size(); ++i) {
            auto meeting = meetings[i].toObject();
            if (meeting["meeting_date"].toString() != "2026-10-28")
                continue;
            auto fed = meeting["fed_side"].toObject();
            fed["freshness"] = QJsonObject{{"status", "STALE"}};
            fed["local_status"] = "UNAVAILABLE";
            fed["local_probabilities"] = QJsonArray{};
            meeting["fed_side"] = fed;
            meeting["polymarket"] = QJsonObject{{"mapping_status", "NOT_FOUND"},
                                                {"data_status", "STALE"},
                                                {"freshness", QJsonObject{{"status", "STALE"}}}};
            meeting["comparison"] = QJsonArray{};
            meetings[i] = meeting;
        }
        data["meetings"] = meetings;
        snapshot_["data"] = data;
        openUpcoming();
        snapshot_ = original;
        QVERIFY(text("fedwatchSourceStatus").contains("STALE"));
        QVERIFY(text("fedwatchSourceStatus").contains("NOT_FOUND"));
        QVERIFY(text("fedwatchDiagnosticStatus").contains("stale"));
        QVERIFY(text("fedwatchDiagnosticStatus").contains("not found"));
        QVERIFY(tableText("fedwatchDistribution").contains("No current distribution"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
    }
    void staleResponseCannotOverwriteNewMeeting() {
        openUpcoming();
        auto* meetings = control("fedwatchMeeting");
        meetings->selectIndex(meetings->findData("2026-12-09"));
        QVERIFY(!pending_.isEmpty());
        const auto old = pending_;
        pending_.clear();
        meetings->selectIndex(meetings->findData("2026-09-16"));
        flush();
        const auto saved = panel_->save_panel_state();
        const auto before = tableText("fedwatchDistribution");
        const auto beforeIssues = text("fedwatchSourceStatus");
        for (const auto& request : old) {
            deliver(request, QJsonObject{{"success", false}, {"error", "Superseded history failure"}});
            deliver(request, backend(request));
        }
        QCOMPARE(panel_->save_panel_state(), saved);
        QCOMPARE(tableText("fedwatchDistribution"), before);
        QCOMPARE(text("fedwatchSourceStatus"), beforeIssues);
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), QString("2026-09-16"));
    }
    void providerFailureLeavesStoredHistoryInspectable() {
        panel_->restore_panel_state({{"meeting", "2026-10-28"}, {"outcome_bp", 25}, {"range_days", 0}});
        panel_->activate();
        const auto overview = pending_.takeFirst();
        deliver(overview, backend(overview));
        QVERIFY(!pending_.isEmpty());
        // A current-provider outage is injected only at the transport boundary.
        // All history still comes from the shipped CLI and temporary SQLite.
        while (!pending_.isEmpty()) {
            const auto request = pending_.takeFirst();
            if (request.command == "collect") {
                EconomicsResult result;
                result.source_id = "fedwatch";
                result.error = "Investing: source unavailable; Polymarket: mapping unverified";
                panel_->accept_result(request.id, result);
            } else
                deliver(request, backend(request));
        }
        QVERIFY(text("fedwatchDiagnosticStatus").contains("Investing"));
        QVERIFY(text("fedwatchSourceStatus").contains("unverified"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
    }
    void partialProviderOutageCannotReplayStoredCurrentDifference() {
        // Keep real stored observations inside freshness, so Batch B analytics
        // legitimately says OK. Then simulate a CURRENT collect with missing
        // provider sections; the UI must gate against this fresh failure rather
        // than displaying the formerly successful stored numeric comparison.
        const auto original = snapshot_;
        const auto analytics = backend({"history_analytics", {"--meeting", "2026-10-28", "--outcome-bp", "25"}, ""});
        QCOMPARE(analytics["data"].toObject()["difference"].toObject()["current_state"].toString(), QString("OK"));
        auto failed = snapshot_["data"].toObject();
        auto meetings = failed["meetings"].toArray();
        for (int i = 0; i < meetings.size(); ++i) {
            auto meeting = meetings[i].toObject();
            if (meeting["meeting_date"].toString() != "2026-10-28")
                continue;
            meeting["fed_side"] = QJsonValue();
            meeting["polymarket"] = QJsonObject{{"mapping_status", "AMBIGUOUS"}, {"data_status", "UNAVAILABLE"}};
            meeting["comparison"] = QJsonArray{};
            meetings[i] = meeting;
        }
        failed["meetings"] = meetings;
        failed["errors"] = QJsonArray{
            QJsonObject{
                {"provider", "investing"}, {"code", "FEDWATCH_SOURCE_UNAVAILABLE"}, {"error", "source unavailable"}},
            QJsonObject{
                {"provider", "polymarket"}, {"code", "POLYMARKET_MAPPING_AMBIGUOUS"}, {"error", "unverified mapping"}}};
        snapshot_["data"] = failed;
        openUpcoming();
        snapshot_ = original;
        QVERIFY(text("fedwatchSourceStatus").contains("investing"));
        QVERIFY(text("fedwatchSourceStatus").contains("AMBIGUOUS"));
        QVERIFY(text("fedwatchDiagnosticStatus").contains("ambiguous"));
        QVERIFY(tableText("fedwatchDistribution").contains("No current distribution"));
        const auto selectedCurrent = text("fedwatchSelectedCurrent");
        QVERIFY(selectedCurrent.contains("Unavailable"));
        QVERIFY(!selectedCurrent.contains("65.50"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
    }
    void pendingRestoredOutcomeSurvivesSaveBeforeFirstResponse() {
        panel_->restore_panel_state({{"meeting", "2026-09-16"}, {"outcome_bp", -50}, {"open_ended", true}});
        panel_->activate();
        QCOMPARE(pending_.size(), 1);
        const auto saved = panel_->save_panel_state();
        QCOMPARE(saved["outcome_bp"].toInt(), -50);
        QCOMPARE(saved["open_ended"].toBool(), true);
        QCOMPARE(saved["meeting"].toString(), QString("2026-09-16"));
    }
    void failedHistoryDoesNotOverwriteRestoredOutcomeWithSyntheticZero() {
        panel_->restore_panel_state({{"meeting", "2026-09-16"}, {"outcome_bp", -50}, {"open_ended", true}});
        panel_->activate();
        const auto overview = pending_.takeFirst();
        deliver(overview, backend(overview));
        QCOMPARE(pending_.size(), 1);
        const auto history = pending_.takeFirst();
        QCOMPARE(history.command, QString("history_series"));
        EconomicsResult failure;
        failure.source_id = "fedwatch";
        failure.error = "History database temporarily unavailable";
        panel_->accept_result(history.id, failure);
        QVERIFY(pending_.isEmpty());
        QCOMPARE(control("fedwatchOutcome")->count(), 0);
        const auto saved = panel_->save_panel_state();
        QCOMPARE(saved["outcome_bp"].toInt(), -50);
        QCOMPARE(saved["open_ended"].toBool(), true);
    }
    void noOutcomeSelectionDoesNotSaveInventedBucket() {
        const auto saved = panel_->save_panel_state();
        QVERIFY(!saved.contains("outcome_bp"));
        QVERIFY(!saved.contains("open_ended"));
    }
    void explicitOutcomeSupersedesUnresolvedRestoredBucket() {
        panel_->restore_panel_state({{"meeting", "2026-10-28"}, {"outcome_bp", 999}, {"open_ended", true}});
        panel_->activate();
        flush();
        auto* outcomes = control("fedwatchOutcome");
        QVERIFY(outcomes->findData("999:tail") < 0);
        QVERIFY(outcomes->findData("25:exact") >= 0);
        outcomes->selectIndex(outcomes->findData("25:exact"));
        flush();
        QCOMPARE(panel_->save_panel_state()["outcome_bp"].toInt(), 25);
        QCOMPARE(panel_->save_panel_state()["open_ended"].toBool(), false);
        auto* refresh = panel_->findChild<QPushButton*>("econFetchBtn");
        refresh->click();
        flush();
        QCOMPARE(outcomes->currentData().toString(), QString("25:exact"));
        QCOMPARE(panel_->save_panel_state()["outcome_bp"].toInt(), 25);
    }
    void timelineAndSegmentsRespondToVisibleMouseClicks() {
        openUpcoming();
        panel_->resize(1280, 800);
        panel_->show();
        QTest::qWait(100);
        for (auto* dropdown : panel_->findChildren<QComboBox*>())
            QVERIFY(!dropdown->isVisible());
        auto click = [&](const char* kind, const QVariant& value) {
            QPushButton* selected = nullptr;
            for (auto* button : panel_->findChildren<QPushButton*>())
                if (button->property("selection_kind").toString() == QLatin1String(kind) && !button->isHidden() &&
                    button->property("selection_value") == value)
                    selected = button;
            QVERIFY2(selected, "Backend selection must have a rendered user control");
            QVERIFY(selected->isVisible());
            QVERIFY(selected->isCheckable());
            QTest::mouseClick(selected, Qt::LeftButton);
            flush();
        };
        const int beforeRange = sent_.size();
        click("range", 30);
        QCOMPARE(sent_.size(), beforeRange);
        QCOMPARE(panel_->save_panel_state()["range_days"].toInt(), 30);
        panel_->findChild<QToolButton*>("fedwatchDetailsToggle")->click();
        click("outcome", "0:exact");
        QCOMPARE(panel_->save_panel_state()["outcome_bp"].toInt(), 0);
        QVERIFY(!panel_->save_panel_state()["open_ended"].toBool());
        click("method", "HISTORICAL_ZQ_RECONSTRUCTED");
        QCOMPARE(panel_->save_panel_state()["fed_method"].toString(), QString("HISTORICAL_ZQ_RECONSTRUCTED"));
        const int beforeResolved = sent_.size();
        click("meeting", "2026-09-16");
        QCOMPARE(panel_->save_panel_state()["meeting"].toString(), QString("2026-09-16"));
        for (int i = beforeResolved; i < sent_.size(); ++i)
            QVERIFY(sent_[i].command != "collect");
    }
    void chipKeyboardFocusSurvivesSelectionAndContinues() {
        openUpcoming();
        panel_->resize(1280, 800);
        panel_->show();
        QApplication::setActiveWindow(panel_.get()); // Logical Qt focus only; the widget stays off the desktop.
        QTest::qWait(100);
        auto* ranges = control("fedwatchRange");
        auto* first = ranges->buttons()[ranges->findData(7)];
        first->setFocus();
        QCOMPARE(QApplication::focusWidget(), first);
        QTest::keyClick(first, Qt::Key_Space);
        QCOMPARE(panel_->save_panel_state()["range_days"].toInt(), 7);
        QCOMPARE(QApplication::focusWidget(), first);
        QTest::keyClick(first, Qt::Key_Tab);
        auto* next = qobject_cast<QPushButton*>(QApplication::focusWidget());
        QVERIFY(next);
        QVERIFY(next != first);
        QCOMPARE(next->property("selection_kind").toString(), QString("range"));
        QCOMPARE(next->property("selection_value").toInt(), 30);
        QTest::keyClick(next, Qt::Key_Space);
        QCOMPARE(panel_->save_panel_state()["range_days"].toInt(), 30);
    }
    void analyticsResponsePreservesFocusedChipAndTimelineScroll() {
        openUpcoming();
        panel_->findChild<QToolButton*>("fedwatchDetailsToggle")->click();
        panel_->resize(1280, 800);
        panel_->show();
        QApplication::setActiveWindow(panel_.get()); // Logical Qt focus only; the widget stays off the desktop.
        QTest::qWait(100);
        control("fedwatchMethod")->selectIndex(control("fedwatchMethod")->findData("HISTORICAL_ZQ_RECONSTRUCTED"));
        QVERIFY(!pending_.isEmpty());
        auto* range = control("fedwatchRange")->buttons()[control("fedwatchRange")->findData(30)];
        range->setFocus();
        QCOMPARE(QApplication::focusWidget(), range);
        auto* strip = panel_->findChild<QScrollArea*>("fedwatchMeetingStrip");
        QVERIFY(strip);
        strip->horizontalScrollBar()->setValue(strip->horizontalScrollBar()->maximum());
        const int browsedPosition = strip->horizontalScrollBar()->value();
        QVERIFY(browsedPosition > 0);
        flush();
        QTest::qWait(50);
        QCOMPARE(QApplication::focusWidget(), range);
        QCOMPARE(strip->horizontalScrollBar()->value(), browsedPosition);
    }
    void clickingActiveFallbackOutcomeSupersedesPendingRestoration() {
        panel_->restore_panel_state({{"meeting", "2026-10-28"}, {"outcome_bp", 999}, {"open_ended", true}});
        panel_->activate();
        flush();
        auto* outcomes = control("fedwatchOutcome");
        const auto fallback = outcomes->currentData().toString();
        QVERIFY(!fallback.isEmpty());
        QVERIFY(fallback != "999:tail");
        outcomes->selectIndex(outcomes->findData(fallback));
        flush();
        const auto saved = panel_->save_panel_state();
        QCOMPARE(saved["outcome_bp"].toInt(), fallback.section(':', 0, 0).toInt());
        QCOMPARE(saved["open_ended"].toBool(), fallback.endsWith(":tail"));
    }
    void clickingActiveImplicitResolvedMeetingMakesRefreshLocal() {
        panel_->activate();
        const auto request = pending_.takeFirst();
        const auto realOverview = backend(request)["data"].toObject();
        QJsonArray resolvedOnly;
        for (const auto& item : realOverview["meetings"].toArray())
            if (item.toObject()["status"].toString() == "RESOLVED")
                resolvedOnly.append(item);
        deliver(request, QJsonObject{{"success", true}, {"data", QJsonObject{{"meetings", resolvedOnly}}}});
        QCOMPARE(pending_.first().command, QString("collect"));
        const auto active = control("fedwatchMeeting")->currentData();
        QCOMPARE(active.toString(), QString("2026-09-16"));
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData(active));
        flush();
        const int before = sent_.size();
        panel_->findChild<QPushButton*>("econFetchBtn")->click();
        flush();
        for (int i = before; i < sent_.size(); ++i)
            QVERIFY(sent_[i].command != "collect");
    }
    void queuedScrollDoesNotRecenterSupersededMeeting() {
        openUpcoming();
        panel_->findChild<QToolButton*>("fedwatchDetailsToggle")->click();
        panel_->resize(1280, 800);
        panel_->show();
        QTest::qWait(50);
        auto* timeline = control("fedwatchMeeting");
        timeline->selectIndex(timeline->findData("2027-09-15"));
        timeline->selectIndex(timeline->findData("2026-09-16"));
        flush();
        QTest::qWait(50);
        QCOMPARE(timeline->currentData().toString(), QString("2026-09-16"));
        auto* strip = panel_->findChild<QScrollArea*>("fedwatchMeetingStrip");
        QVERIFY(strip);
        QPushButton* active = nullptr;
        for (auto* button : timeline->buttons())
            if (button->isChecked())
                active = button;
        QVERIFY(active);
        const QRect selectedBounds(active->mapTo(strip->viewport(), QPoint(0, 0)), active->size());
        QVERIFY(strip->viewport()->rect().contains(selectedBounds));
    }
    void timelineRestoresAndReachesDistantBackendMeetings() {
        panel_->resize(1280, 800);
        panel_->show();
        QTest::qWait(50);
        panel_->restore_panel_state({{"meeting", "2027-09-15"}, {"outcome_bp", 0}});
        panel_->activate();
        flush();
        panel_->findChild<QToolButton*>("fedwatchDetailsToggle")->click();
        QTest::qWait(50);
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), QString("2027-09-15"));
        QVERIFY(control("fedwatchMeeting")->count() >= 10);
        auto* strip = panel_->findChild<QScrollArea*>("fedwatchMeetingStrip");
        QVERIFY(strip);
        QVERIFY(strip->horizontalScrollBar()->maximum() > 0);
        QPushButton* active = nullptr;
        for (auto* button : control("fedwatchMeeting")->buttons())
            if (button->isChecked())
                active = button;
        QVERIFY(active);
        const QRect selectedBounds(active->mapTo(strip->viewport(), QPoint(0, 0)), active->size());
        QVERIFY2(strip->viewport()->rect().contains(selectedBounds),
                 qPrintable(QString("Restored meeting outside viewport: card %1,%2 %3x%4 viewport %5x%6 scroll %7/%8")
                                .arg(selectedBounds.x())
                                .arg(selectedBounds.y())
                                .arg(selectedBounds.width())
                                .arg(selectedBounds.height())
                                .arg(strip->viewport()->width())
                                .arg(strip->viewport()->height())
                                .arg(strip->horizontalScrollBar()->value())
                                .arg(strip->horizontalScrollBar()->maximum())));
        auto* earlier = panel_->findChild<QToolButton*>("fedwatchEarlierMeetings");
        auto* later = panel_->findChild<QToolButton*>("fedwatchLaterMeetings");
        QVERIFY(earlier);
        QVERIFY(later);
        const auto scrollValue = strip->horizontalScrollBar()->value();
        QTest::mouseClick(earlier, Qt::LeftButton);
        QVERIFY(strip->horizontalScrollBar()->value() < scrollValue);
        const int earlierPosition = strip->horizontalScrollBar()->value();
        QTest::mouseClick(later, Qt::LeftButton);
        QVERIFY(strip->horizontalScrollBar()->value() > earlierPosition);
        QCOMPARE(panel_->save_panel_state()["meeting"].toString(), QString("2027-09-15"));
    }
    void populatedChartsRenderWithinMainWorkspace() {
        openUpcoming();
        panel_->resize(1280, 800);
        panel_->show();
        QTest::qWait(100);
        const QRect workspace(QPoint(0, 0), panel_->size());
        for (const char* name : {"fedwatchProbabilityChart", "fedwatchPolymarketChart"}) {
            auto* view = chart(name);
            QVERIFY(view);
            QVERIFY(view->isVisible());
            const QRect chartBounds(view->mapTo(panel_.get(), QPoint(0, 0)), view->size());
            QVERIFY2(workspace.contains(chartBounds),
                     qPrintable(QString("%1 is clipped or below the primary workspace: %2,%3 %4x%5")
                                    .arg(name)
                                    .arg(chartBounds.x())
                                    .arg(chartBounds.y())
                                    .arg(chartBounds.width())
                                    .arg(chartBounds.height())));
            const auto populated = view->grab().toImage();
            QVERIFY(!populated.isNull());
            const auto observations = view->series();
            QVERIFY(!observations[0].points.isEmpty());
            view->set_series({});
            view->repaint();
            const auto empty = view->grab().toImage();
            QVERIFY2(populated != empty,
                     "Populated chart must visibly render observations, rather than only an empty frame");
            view->set_series(observations);
        }
    }
    void defaultResearchWorkspaceKeepsChartsAndHidesDiagnostics() {
        openUpcoming();
        panel_->resize(1280, 800);
        panel_->show();
        QTest::qWait(100);
        QVERIFY(panel_->findChildren<QTabWidget*>().isEmpty());
        auto* diagnostics = panel_->findChild<QWidget*>("fedwatchDiagnostics");
        auto* toggle = panel_->findChild<QToolButton*>("fedwatchDetailsToggle");
        QVERIFY(diagnostics->isHidden());
        QVERIFY(!toggle->isChecked());
        QVERIFY(!panel_->findChild<QPushButton*>("fedwatchLoadHistory")->isVisible());
        QVERIFY(!panel_->findChild<QPushButton*>("fedwatchUpdateUpcoming")->isVisible());
        QVERIFY(panel_->findChild<QPushButton*>("econFetchBtn")->isVisible());
        auto* fed = chart("fedwatchProbabilityChart");
        auto* poly = chart("fedwatchPolymarketChart");
        QCOMPARE(fed->series().size(), 1);
        QCOMPARE(poly->series().size(), 1);
        QVERIFY(fed->series()[0].label.contains("LIVE_INVESTING_DERIVED"));
        QCOMPARE(poly->series()[0].label, QString("POLYMARKET_CLOB"));
        QVERIFY(!fed->series()[0].points.isEmpty());
        QVERIFY(!poly->series()[0].points.isEmpty());
        QCOMPARE(fed->time_bounds(), poly->time_bounds());
        QCOMPARE(fed->time_bounds().first, QDateTime::fromString("2026-08-01T12:00:00Z", Qt::ISODate));
        QCOMPARE(fed->time_bounds().second, QDateTime::fromString("2026-09-28T12:00:00Z", Qt::ISODate));
        const auto originalFed = fed->series()[0].points.size();
        const auto originalPoly = poly->series()[0].points.size();
        control("fedwatchRange")->selectIndex(control("fedwatchRange")->findData(7));
        QCOMPARE(fed->time_bounds(), poly->time_bounds());
        QVERIFY(fed->time_bounds().first > QDateTime::fromString("2026-08-01T12:00:00Z", Qt::ISODate));
        QVERIFY(fed->series()[0].points.size() < originalFed);
        QVERIFY(poly->series()[0].points.size() < originalPoly);
        control("fedwatchRange")->selectIndex(control("fedwatchRange")->findData(0));
        QCOMPARE(fed->series()[0].points.size(), originalFed);
        QCOMPARE(poly->series()[0].points.size(), originalPoly);
        const auto fedPosition = fed->mapTo(panel_.get(), QPoint(0, 0));
        const auto polyPosition = poly->mapTo(panel_.get(), QPoint(0, 0));
        QCOMPARE(fedPosition.y(), polyPosition.y());
        QVERIFY(fedPosition.x() + fed->width() <= polyPosition.x());
        auto* table = panel_->findChild<QTableWidget*>("fedwatchDistribution");
        QVERIFY(!table->isVisible());
        QVERIFY(diagnostics->isAncestorOf(table));
        QVERIFY(currentChart()->mapTo(panel_.get(), QPoint()).y() + currentChart()->height() < fedPosition.y());
        QVERIFY(!control("fedwatchOutcome")->isVisible());
        QVERIFY(!control("fedwatchMeeting")->isVisible());
        QVERIFY(panel_->findChild<QToolButton*>("fedwatchNextMeeting")->isVisible());
        QCOMPARE(table->rowCount(), 5);
        QCOMPARE(table->columnCount(), 4);
        QVERIFY(chart("fedwatchDivergenceChart")->isHidden());
        panel_->findChild<QToolButton*>("fedwatchDifferenceToggle")->click();
        QVERIFY(chart("fedwatchDivergenceChart")->isVisible());
        QVERIFY(!chart("fedwatchDivergenceChart")->series()[0].points.isEmpty());
        toggle->click();
        QVERIFY(diagnostics->isVisible());
        QVERIFY(table->isVisible());
        QVERIFY(panel_->findChild<QLabel*>("fedwatchSourceStatus")->isVisible());
        QVERIFY(panel_->findChild<QPushButton*>("fedwatchLoadHistory")->isVisible());
        QVERIFY(panel_->save_panel_state()["details_visible"].toBool());
        toggle->click();
        QVERIFY(!panel_->save_panel_state()["details_visible"].toBool());
    }
    void fedWatchUsesExistingThemeAndSeriesPalette() {
        openUpcoming();
        auto& theme = fincept::ui::ThemeManager::instance();
        theme.apply_theme("Obsidian"); // The application currently supports this single preset.
        const auto& tokens = theme.tokens();
        QVERIFY(panel_->styleSheet().contains(
            QString("background:%1; border-color:%1; color:%2").arg(tokens.accent, tokens.text_on_accent)));
        QVERIFY(QColor(tokens.accent) != QColor(tokens.text_on_accent));
        QVERIFY(QColor(tokens.chart_colors[0]) != QColor(tokens.chart_colors[1]));
        panel_->resize(1280, 800);
        panel_->show();
        QTest::qWait(100);
        const auto images = QList<QImage>{chart("fedwatchProbabilityChart")->grab().toImage(),
                                          chart("fedwatchPolymarketChart")->grab().toImage()};
        for (int i = 0; i < images.size(); ++i) {
            bool palettePixel = false;
            for (int y = 0; y < images[i].height() && !palettePixel; ++y)
                for (int x = 0; x < images[i].width(); ++x)
                    if (images[i].pixelColor(x, y) == QColor(tokens.chart_colors[i])) {
                        palettePixel = true;
                        break;
                    }
            QVERIFY2(palettePixel, "The actual history rendering must use the active shared series palette");
        }
        const QRegularExpression hexColor("#[0-9A-Fa-f]{6}\\b");
        for (const char* source :
             {"src/screens/economics/panels/FedWatchPanel.cpp", "src/screens/economics/panels/FedWatchCurrentChart.cpp",
              "src/screens/economics/panels/FedWatchHistoryChart.cpp"}) {
            QFile file(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/") + source);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY2(!hexColor.match(QString::fromUtf8(file.readAll())).hasMatch(),
                     "FedWatch UI colors must resolve from existing theme tokens");
        }
        QFile registry(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/src/screens/economics/EconomicsScreen.cpp"));
        QVERIFY(registry.open(QIODevice::ReadOnly));
        for (const auto& line : QString::fromUtf8(registry.readAll()).split('\n'))
            if (line.contains("{\"fedwatch\", \"FedWatch\""))
                QVERIFY(!hexColor.match(line).hasMatch());
    }
    void chartCoverageAndContextualHistoryAreVisible() {
        openUpcoming();
        control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("-25:exact"));
        flush();
        QCOMPARE(chart("fedwatchPolymarketChart")->series()[0].points.size(), 1);
        QVERIFY(text("fedwatchPolymarketHistoryState").contains("1 observation retained"));
        QVERIFY(text("fedwatchFedHistoryState").contains("1 observation retained"));
        const auto bounds = chart("fedwatchProbabilityChart")->value_bounds();
        QCOMPARE(bounds, chart("fedwatchPolymarketChart")->value_bounds());
        QVERIFY(bounds.second < 5);
        auto* action = panel_->findChild<QPushButton*>("fedwatchContextLoadHistory");
        QVERIFY(!action->isHidden());
        QVERIFY(action->isEnabled());
        const auto before = sent_.size();
        action->click();
        QCOMPARE(sent_.size(), before + 1);
        QCOMPARE(pending_.first().command, QString("history_backfill"));
        QVERIFY(!action->isEnabled());
        const auto request = pending_.takeFirst();
        deliver(request, process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                                 {database_, "backfill-coverage-two"}));
        flush();
        QCOMPARE(chart("fedwatchPolymarketChart")->series()[0].points.size(), 2);
        QVERIFY(text("fedwatchPolymarketHistoryState").contains("Limited history: 2 observations"));
        QVERIFY(text("fedwatchPolymarketHistoryState").contains("missing UTC days (line breaks)"));
        QVERIFY(action->isHidden());
        // No interpolation/forward-fill: both rendered points are exact backend rows.
        const auto retained = rawDetails()["analytics"].toObject()["polymarket"].toObject()["history"].toArray();
        QCOMPARE(retained.size(), 2);
        for (int i = 0; i < retained.size(); ++i) {
            QCOMPARE(chart("fedwatchPolymarketChart")->series()[0].points[i].value,
                     retained[i].toObject()["probability_pct"].toDouble());
            QCOMPARE(chart("fedwatchPolymarketChart")->series()[0].points[i].instant,
                     QDateTime::fromString(retained[i].toObject()["observed_at"].toString(), Qt::ISODate));
        }
        control("fedwatchMethod")->selectIndex(control("fedwatchMethod")->findData("HISTORICAL_ZQ_RECONSTRUCTED"));
        flush();
        QVERIFY(text("fedwatchFedHistoryState").contains("No observations in this range"));
        QVERIFY(text("fedwatchProbabilityTitle").contains("ZQ reconstructed"));
        QVERIFY(chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
        QCOMPARE(chart("fedwatchProbabilityChart")->time_bounds(), chart("fedwatchPolymarketChart")->time_bounds());
        QCOMPARE(chart("fedwatchProbabilityChart")->value_bounds(), chart("fedwatchPolymarketChart")->value_bounds());
    }
    void normalMultiPointHistoryPreservesBackendValuesAndSharedScale() {
        process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                {database_, "backfill-coverage-normal"});
        openUpcoming();
        control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("-25:exact"));
        flush();
        const auto* fed = chart("fedwatchProbabilityChart");
        const auto* poly = chart("fedwatchPolymarketChart");
        QCOMPARE(poly->series()[0].points.size(), 9);
        QVERIFY(text("fedwatchPolymarketHistoryState").contains("9 observations retained"));
        QVERIFY(!text("fedwatchPolymarketHistoryState").contains("Limited history"));
        const auto bounds = fed->value_bounds();
        QCOMPARE(bounds, poly->value_bounds());
        for (const auto* view : {fed, poly})
            for (const auto& point : view->series()[0].points)
                QVERIFY(point.value >= bounds.first && point.value <= bounds.second);
        control("fedwatchRange")->selectIndex(control("fedwatchRange")->findData(7));
        QCOMPARE(fed->time_bounds(), poly->time_bounds());
        QCOMPARE(fed->value_bounds(), poly->value_bounds());
        QCOMPARE(poly->series()[0].points.size(), 1);
        QVERIFY(text("fedwatchPolymarketHistoryState").contains("1 observation retained"));
        QVERIFY(text("fedwatchProbabilityTitle").contains("Investing-derived"));
        QCOMPARE(fed->time_bounds().first, fed->time_bounds().second.addDays(-7));
        QVERIFY(!text("fedwatchProbabilityTitle").contains("CME"));
    }
    void hoverSynchronizesDateAndLeavesMissingPeerObservationsMissing() {
        openUpcoming();
        panel_->resize(1280, 800);
        panel_->show();
        QTest::qWait(100);
        auto* fed = chart("fedwatchProbabilityChart");
        auto* poly = chart("fedwatchPolymarketChart");
        // Hover an actual retained Polymarket point on Sep 27: Fed has no
        // observation that day. Matching by date must not supply Sep 26/28.
        const auto first = poly->time_bounds().first.toMSecsSinceEpoch();
        const auto last = poly->time_bounds().second.toMSecsSinceEpoch();
        const auto point = *std::find_if(poly->series()[0].points.begin(), poly->series()[0].points.end(),
                                         [](const auto& p) { return p.instant.date() == QDate(2026, 9, 27); });
        const auto bounds = poly->value_bounds();
        const QPoint cursor(
            65 + qRound(double(point.instant.toMSecsSinceEpoch() - first) / (last - first) * (poly->width() - 90)),
            50 + qRound((bounds.second - point.value) / (bounds.second - bounds.first) * (poly->height() - 110)));
        // Deliver the real Qt widget mouse event deterministically; external
        // desktop windows must not steal the OS cursor from this widget test.
        QSignalSpy dates(poly, &FedWatchHistoryChart::date_hovered);
        QMouseEvent move(QEvent::MouseMove, QPointF(cursor), QPointF(poly->mapToGlobal(cursor)), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(poly, &move);
        QCOMPARE(dates.size(), 1);
        QCOMPARE(fed->hover_date(), QDate(2026, 9, 27));
        QCOMPARE(poly->hover_date(), fed->hover_date());
        QVERIFY(fed->hover_text().contains("No observation"));
        QVERIFY(poly->hover_text().contains("32% @ 12:00 UTC"));
        QVERIFY(fed->accessibleDescription().contains("No observation"));
        QCOMPARE(fed->series()[0].points.size(), 3);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(poly, &leave);
        QVERIFY(!fed->hover_date().isValid());
        QVERIFY(!poly->hover_date().isValid());
    }
    void explicitPolymarketHistoryUsesBackendAndCoalescesWriters() {
        openUpcoming();
        auto* button = panel_->findChild<QPushButton*>("fedwatchLoadHistory");
        QVERIFY(button);
        QVERIFY(button->isEnabled());
        button->click();
        QVERIFY(!button->isEnabled());
        QVERIFY(!panel_->findChild<QPushButton*>("econFetchBtn")->isEnabled());
        QVERIFY(!panel_->findChild<QPushButton*>("fedwatchUpdateUpcoming")->isEnabled());
        QCOMPARE(pending_.size(), 1);
        const auto request = pending_.takeFirst();
        QCOMPARE(request.command, QString("history_backfill"));
        QCOMPARE(request.args.value(request.args.indexOf("--meeting") + 1), QString("2026-10-28"));
        button->click();
        panel_->findChild<QPushButton*>("econFetchBtn")->click();
        QVERIFY(pending_.isEmpty());
        deliver(request, process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                                 {database_, "backfill-ok"}));
        flush();
        QVERIFY(button->isEnabled());
        QVERIFY(panel_->findChild<QPushButton*>("econFetchBtn")->isEnabled());
        QVERIFY(text("fedwatchCoverage").contains("Polymarket observations"));
    }
    void historyFailureAndSelectionRacePreserveLocalResolvedView_data() {
        QTest::addColumn<QString>("target");
        QTest::newRow("resolved") << QString("2026-09-16");
        QTest::newRow("other-upcoming") << QString("2026-12-09");
    }
    void historyFailureAndSelectionRacePreserveLocalResolvedView() {
        QFETCH(QString, target);
        openUpcoming();
        auto* button = panel_->findChild<QPushButton*>("fedwatchLoadHistory");
        button->click();
        const auto request = pending_.takeFirst();
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData(target));
        flush();
        const auto before = sent_.size();
        deliver(request, process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                                 {database_, "backfill-failure"}));
        flush();
        QCOMPARE(panel_->save_panel_state()["meeting"].toString(), target);
        QCOMPARE(button->isEnabled(), target != "2026-09-16");
        QVERIFY(panel_->findChild<QPushButton*>("econFetchBtn")->isEnabled());
        QVERIFY(!text("fedwatchSourceStatus").contains("POLYMARKET_MARKET_DATA_UNAVAILABLE"));
        QVERIFY(!text("fedwatchDiagnosticStatus").contains("PROVIDER_ERROR"));
        QVERIFY(!text("fedwatchCoverage").contains("PROVIDER_ERROR"));
        QVERIFY(rawDetails()["polymarket_history_update"].toObject().isEmpty());
        QVERIFY(!chart("fedwatchProbabilityChart")->series().isEmpty());
        for (int i = before; i < sent_.size(); ++i)
            QVERIFY(sent_[i].command != "collect" && sent_[i].command != "history_backfill");
        if (target == "2026-09-16") {
            panel_->findChild<QPushButton*>("econFetchBtn")->click();
            flush();
            for (int i = before; i < sent_.size(); ++i)
                QVERIFY(sent_[i].command != "collect" && sent_[i].command != "history_backfill");
        }
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-10-28"));
        flush();
        QVERIFY(text("fedwatchSourceStatus").contains("POLYMARKET_MARKET_DATA_UNAVAILABLE"));
        QVERIFY(text("fedwatchCoverage").contains("PROVIDER_ERROR"));
    }
    void successfulHistoryCompletionAfterMeetingSwitch_data() {
        QTest::addColumn<QString>("target");
        QTest::newRow("resolved") << QString("2026-09-16");
        QTest::newRow("other-upcoming") << QString("2026-12-09");
    }
    void successfulHistoryCompletionAfterMeetingSwitch() {
        QFETCH(QString, target);
        openUpcoming();
        panel_->findChild<QPushButton*>("fedwatchLoadHistory")->click();
        const auto request = pending_.takeFirst();
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData(target));
        flush();
        QVERIFY(!text("fedwatchStatus").contains("Loading Polymarket history"));
        const int before = sent_.size();
        deliver(request, process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                                 {database_, "backfill-ok"}));
        QCOMPARE(pending_.first().command, QString("history_meetings"));
        flush();
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), target);
        QVERIFY(!text("fedwatchSourceStatus").contains("Polymarket history backfill"));
        QVERIFY(!text("fedwatchCoverage").contains("Polymarket history backfill"));
        QVERIFY(rawDetails()["polymarket_history_update"].toObject().isEmpty());
        for (int i = before; i < sent_.size(); ++i)
            QVERIFY(sent_[i].command != "collect" && sent_[i].command != "history_backfill");
        if (target == "2026-09-16") {
            panel_->findChild<QPushButton*>("econFetchBtn")->click();
            flush();
            for (int i = before; i < sent_.size(); ++i)
                QVERIFY(sent_[i].command != "collect" && sent_[i].command != "history_backfill");
        }
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-10-28"));
        flush();
        QVERIFY(text("fedwatchSourceStatus").contains("accepted points"));
        QVERIFY(std::any_of(chart("fedwatchPolymarketChart")->series()[0].points.begin(),
                            chart("fedwatchPolymarketChart")->series()[0].points.end(), [](const auto& point) {
                                return point.instant.date() == QDate(2026, 8, 2) && point.value == 42;
                            }));
    }
    void partialAcceptedBackfillRetainsDroppedPointWarnings() {
        openUpcoming();
        panel_->findChild<QPushButton*>("fedwatchLoadHistory")->click();
        const auto request = pending_.takeFirst();
        const auto response = process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                                      {database_, "backfill-partial"});
        QVERIFY(response["data"].toObject()["errors"].toArray().isEmpty());
        deliver(request, response);
        flush();
        QVERIFY(text("fedwatchDiagnosticStatus").contains("PARTIAL"));
        QVERIFY(text("fedwatchPolymarketHistoryState").contains("Meeting history partial"));
        QVERIFY(text("fedwatchCoverage").contains("PARTIAL"));
        QVERIFY(text("fedwatchSourceStatus").contains("1 accepted points"));
        QVERIFY(text("fedwatchSourceStatus").contains("Dropped: 1 malformed / 1 future / 1 out of range"));
        QVERIFY(text("fedwatchSourceStatus").contains("Warning: meeting 2026-10-28"));
        QVERIFY(text("fedwatchCoverage").contains("1 backfill points"));
        QVERIFY(std::any_of(chart("fedwatchPolymarketChart")->series()[0].points.begin(),
                            chart("fedwatchPolymarketChart")->series()[0].points.end(), [](const auto& point) {
                                return point.instant.date() == QDate(2026, 8, 2) && point.value == 42;
                            }));
    }
    void immediateBackfillStatesWithoutErrorsRemainVisible_data() {
        QTest::addColumn<QString>("status");
        for (const char* status : {"EMPTY", "PROVIDER_ERROR", "NO_TOKEN", "MAPPING_NOT_CURRENT", "PARTIAL"})
            QTest::newRow(status) << QString::fromLatin1(status);
    }
    void immediateBackfillStatesWithoutErrorsRemainVisible() {
        QFETCH(QString, status);
        openUpcoming();
        panel_->findChild<QPushButton*>("fedwatchLoadHistory")->click();
        const auto request = pending_.takeFirst();
        deliver(request,
                QJsonObject{{"success", true},
                            {"data", QJsonObject{{"errors", QJsonArray{}},
                                                 {"backfills", QJsonArray{QJsonObject{{"meeting_date", "2026-10-28"},
                                                                                      {"outcome_bp", 25},
                                                                                      {"status", status}}}}}}});
        flush();
        QVERIFY(text("fedwatchDiagnosticStatus").contains(status));
        QVERIFY(text("fedwatchSourceStatus").contains(status));
        if (status == "PARTIAL" || status == "PROVIDER_ERROR")
            QVERIFY(text("fedwatchPolymarketHistoryState")
                        .contains(status == "PARTIAL" ? "Meeting history partial" : "Meeting history provider error"));
        QVERIFY(text("fedwatchCoverage").contains(status));
    }
    void persistedBackfillQualitySurvivesFreshPanelReload_data() {
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<QString>("status");
        QTest::addColumn<int>("points");
        QTest::newRow("partial") << QString("backfill-partial") << QString("PARTIAL") << 1;
        QTest::newRow("provider-error") << QString("backfill-failure") << QString("PROVIDER_ERROR") << 0;
    }
    void persistedBackfillQualitySurvivesFreshPanelReload() {
        QFETCH(QString, fixture);
        QFETCH(QString, status);
        QFETCH(int, points);
        process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"), {database_, fixture});
        openUpcoming();
        QVERIFY(text("fedwatchDiagnosticStatus").contains(status));
        const auto saved = panel_->save_panel_state();
        panel_.reset();
        makePanel();
        panel_->restore_panel_state(saved);
        panel_->activate();
        flush();
        QVERIFY(rawDetails()["polymarket_history_update"].toObject().isEmpty());
        QVERIFY(text("fedwatchSourceStatus").contains(status));
        QVERIFY(text("fedwatchPolymarketHistoryState")
                    .contains(status == "PARTIAL" ? "Meeting history partial" : "Meeting history provider error"));
        QVERIFY(text("fedwatchCoverage").contains(QString("%1 backfill points").arg(points)));
        QVERIFY(text("fedwatchCoverage").contains("Last backfill 2026-09-28T12:00:00Z"));
        QVERIFY(!chart("fedwatchPolymarketChart")->series()[0].points.isEmpty());
        for (const auto& request : sent_)
            QVERIFY(request.command != "history_backfill");
    }
    void inventoryFailureSurvivesSuccessfulSeriesAndAnalytics() {
        openUpcoming();
        panel_->findChild<QPushButton*>("econFetchBtn")->click();
        const auto request = pending_.takeFirst();
        QCOMPARE(request.command, QString("history_meetings"));
        deliver(request, QJsonObject{{"success", false}, {"error", "inventory read failed"}});
        flush();
        QVERIFY(text("fedwatchSourceStatus").contains("Meeting history unavailable: inventory read failed"));
        QVERIFY(text("fedwatchDiagnosticStatus").contains("History incomplete"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
        panel_->findChild<QPushButton*>("econFetchBtn")->click();
        flush();
        QVERIFY(!text("fedwatchSourceStatus").contains("inventory read failed"));
    }
    void seriesFailureSurvivesSuccessfulAnalytics() {
        openUpcoming();
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-10-28"));
        const auto request = pending_.takeFirst();
        QCOMPARE(request.command, QString("history_series"));
        deliver(request, QJsonObject{{"success", false}, {"error", "series read failed"}});
        flush();
        QVERIFY(text("fedwatchSourceStatus").contains("Stored series unavailable: series read failed"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
        control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("25:exact"));
        flush();
        QVERIFY(text("fedwatchSourceStatus").contains("series read failed"));
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-10-28"));
        QVERIFY(text("fedwatchSourceStatus").contains("series read failed"));
        flush();
        QVERIFY(!text("fedwatchSourceStatus").contains("series read failed"));
    }
    void analyticsFailureWaitsForItsOwnReplacement() {
        openUpcoming();
        control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("25:exact"));
        const auto request = pending_.takeFirst();
        QCOMPARE(request.command, QString("history_analytics"));
        deliver(request, QJsonObject{{"success", false}, {"error", "analytics read failed"}});
        QVERIFY(text("fedwatchSourceStatus").contains("Analytics unavailable: analytics read failed"));
        control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("25:exact"));
        QVERIFY(text("fedwatchSourceStatus").contains("analytics read failed"));
        flush();
        QVERIFY(!text("fedwatchSourceStatus").contains("analytics read failed"));
    }
    void usablePartialSeriesRetainsDataAndProviderIssue() {
        openUpcoming();
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-10-28"));
        const auto request = pending_.takeFirst();
        auto response = backend(request);
        auto data = response["data"].toObject();
        data["errors"] = QJsonArray{QJsonObject{
            {"provider", "polymarket"}, {"code", "POLYMARKET_HISTORY_PARTIAL"}, {"error", "some rows unavailable"}}};
        response["data"] = data;
        response["partial"] = true;
        response["failed_series"] = QJsonArray{"polymarket history"};
        deliver(request, response);
        flush();
        QVERIFY(!rawDetails()["stored_observations"].toObject()["observations"].toArray().isEmpty());
        QVERIFY(text("fedwatchSourceStatus").contains("Partial provider result: polymarket history"));
        QVERIFY(text("fedwatchSourceStatus").contains("POLYMARKET_HISTORY_PARTIAL"));
        QVERIFY(!text("fedwatchSourceStatus").contains("Stored series unavailable"));
        QVERIFY(!chart("fedwatchPolymarketChart")->series()[0].points.isEmpty());
        control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("25:exact"));
        flush();
        QVERIFY(text("fedwatchSourceStatus").contains("POLYMARKET_HISTORY_PARTIAL"));
    }
    void numericOutcomeOrderPreservesExactAndTailIdentity() {
        openUpcoming();
        auto assertOrder = [&](const QStringList& expected) {
            auto buttons = control("fedwatchOutcome")->buttons();
            auto* table = panel_->findChild<QTableWidget*>("fedwatchDistribution");
            QCOMPARE(buttons.size(), expected.size());
            QCOMPARE(table->rowCount(), expected.size());
            QCOMPARE(currentChart()->rows().size(), expected.size());
            for (int i = 0; i < expected.size(); ++i) {
                QCOMPARE(buttons[i]->property("selection_value").toString(), expected[i]);
                QCOMPARE(table->item(i, 0)->text(), buttons[i]->text());
                const auto row = currentChart()->rows()[i];
                QCOMPARE(QString::number(row["outcome_bp"].toInt()) + (row["open_ended"].toBool() ? ":tail" : ":exact"),
                         expected[i]);
            }
        };
        assertOrder({"-50:tail", "-25:exact", "0:exact", "25:exact", "50:tail"});
        const auto original = snapshot_;
        auto data = snapshot_["data"].toObject();
        auto meetings = data["meetings"].toArray();
        for (int i = 0; i < meetings.size(); ++i) {
            auto meeting = meetings[i].toObject();
            if (meeting["meeting_date"].toString() != "2026-10-28")
                continue;
            auto fed = meeting["fed_side"].toObject();
            auto rows = fed["local_probabilities"].toArray();
            rows.append(QJsonObject{{"outcome_bp", 50}, {"probability_pct", 0}});
            fed["local_probabilities"] = rows;
            meeting["fed_side"] = fed;
            meetings[i] = meeting;
        }
        data["meetings"] = meetings;
        snapshot_["data"] = data;
        panel_->findChild<QPushButton*>("econFetchBtn")->click();
        flush();
        snapshot_ = original;
        assertOrder({"-50:tail", "-25:exact", "0:exact", "25:exact", "50:exact", "50:tail"});
        for (const auto& identity : {QString("50:exact"), QString("50:tail")}) {
            control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData(identity));
            flush();
            QCOMPARE(panel_->save_panel_state()["outcome_bp"].toInt(), 50);
            QCOMPARE(panel_->save_panel_state()["open_ended"].toBool(), identity.endsWith(":tail"));
        }
    }
    void currentPolymarketRequiresValidatedAndCurrent_data() {
        QTest::addColumn<QString>("mapping");
        QTest::addColumn<QString>("status");
        QTest::addColumn<bool>("available");
        QTest::newRow("validated-current") << QString("VALIDATED") << QString("CURRENT") << true;
        QTest::newRow("validated-stale") << QString("VALIDATED") << QString("STALE") << false;
        QTest::newRow("validated-partial") << QString("VALIDATED") << QString("PARTIAL") << false;
        QTest::newRow("validated-unavailable") << QString("VALIDATED") << QString("UNAVAILABLE") << false;
        QTest::newRow("unvalidated-current") << QString("UNVALIDATED") << QString("CURRENT") << false;
    }
    void currentPolymarketRequiresValidatedAndCurrent() {
        QFETCH(QString, mapping);
        QFETCH(QString, status);
        QFETCH(bool, available);
        const auto original = snapshot_;
        changeCurrentSection("polymarket", {{"mapping_status", mapping}, {"data_status", status}});
        openUpcoming();
        snapshot_ = original;
        auto* table = panel_->findChild<QTableWidget*>("fedwatchDistribution");
        int polyBars = 0;
        for (const auto& bar : currentChart()->bars())
            if (bar.source == 1)
                ++polyBars;
        QCOMPARE(polyBars > 0, available);
        for (int row = 0; row < table->rowCount(); ++row) {
            QCOMPARE(table->item(row, 2)->text() != "Unavailable", available);
            if (!available)
                QCOMPARE(table->item(row, 3)->text(), QString("Unavailable"));
        }
        QVERIFY(text("fedwatchSelectedCurrent").contains(available ? "Polymarket 65.50%" : "Polymarket Unavailable"));
        if (!available)
            QVERIFY(text("fedwatchSelectedCurrent").contains("Difference Unavailable"));
        QVERIFY(!chart("fedwatchPolymarketChart")->series()[0].points.isEmpty());
        QVERIFY(!rawDetails()["current_snapshot"].toObject().isEmpty());
        captureFixture("current_" + mapping.toLower() + "_" + status.toLower());
    }
    void currentFedAvailabilityGatesBarsAndExactValues_data() {
        QTest::addColumn<QString>("local");
        QTest::addColumn<QString>("freshness");
        QTest::newRow("unavailable") << QString("UNAVAILABLE") << QString("CURRENT");
        QTest::newRow("partial") << QString("PARTIAL") << QString("CURRENT");
        QTest::newRow("stale") << QString("OK") << QString("STALE");
        QTest::newRow("unavailable-freshness") << QString("OK") << QString("UNAVAILABLE");
        QTest::newRow("unknown-freshness") << QString("OK") << QString("FUTURE_UNKNOWN_STATE");
        QTest::newRow("empty-freshness") << QString("OK") << QString("");
        QTest::newRow("missing-freshness") << QString("OK") << QString("<missing>");
    }
    void currentFedAvailabilityGatesBarsAndExactValues() {
        QFETCH(QString, local);
        QFETCH(QString, freshness);
        const auto original = snapshot_;
        const auto state = freshness == "<missing>" ? QJsonObject{} : QJsonObject{{"status", freshness}};
        changeCurrentSection("fed_side", {{"local_status", local}, {"freshness", state}});
        openUpcoming();
        snapshot_ = original;
        for (const auto& bar : currentChart()->bars())
            QVERIFY(bar.source != 0);
        QVERIFY(!currentChart()->bars().isEmpty());
        auto* table = panel_->findChild<QTableWidget*>("fedwatchDistribution");
        for (int row = 0; row < table->rowCount(); ++row) {
            QCOMPARE(table->item(row, 1)->text(), QString("Unavailable"));
            QCOMPARE(table->item(row, 3)->text(), QString("Unavailable"));
        }
        QVERIFY(text("fedwatchSelectedCurrent").contains("Fed-side Unavailable"));
        QVERIFY(text("fedwatchSelectedCurrent").contains("Difference Unavailable"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series()[0].points.isEmpty());
    }
    void establishedFedFreshnessRemainsEligible_data() {
        QTest::addColumn<QString>("freshness");
        for (const char* status : {"CURRENT", "OK", "SOURCE_TIMESTAMP_UNAVAILABLE"})
            QTest::newRow(status) << QString::fromLatin1(status);
    }
    void establishedFedFreshnessRemainsEligible() {
        QFETCH(QString, freshness);
        const auto original = snapshot_;
        changeCurrentSection("fed_side", {{"local_status", "OK"}, {"freshness", QJsonObject{{"status", freshness}}}});
        openUpcoming();
        snapshot_ = original;
        const auto bars = currentChart()->bars();
        QCOMPARE(std::count_if(bars.begin(), bars.end(), [](const auto& bar) { return bar.source == 0; }), 5);
        QVERIFY(text("fedwatchSelectedCurrent").contains("Fed-side 70.00%"));
        QVERIFY(text("fedwatchSelectedCurrent").contains("Difference -4.50 pp"));
        auto* table = panel_->findChild<QTableWidget*>("fedwatchDistribution");
        QCOMPARE(table->item(3, 1)->text(), QString("70.00"));
        QCOMPARE(table->item(3, 3)->text(), QString("-4.50"));
    }
    void diagnosticsCopyUsesResearchDetailsDestination() {
        openUpcoming();
        QCOMPARE(panel_->findChild<QToolButton*>("fedwatchDetailsToggle")->text(), QString("Research details"));
        QFile file(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/src/screens/economics/panels/FedWatchPanel.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY2(!QString::fromUtf8(file.readAll()).contains("see Sources"),
                 "Diagnostic paths must point to the existing Research details section");
        panel_->findChild<QPushButton*>("econFetchBtn")->click();
        const auto request = pending_.takeFirst();
        QCOMPARE(request.command, QString("history_meetings"));
        deliver(request, {{"success", false}, {"error", "fixture inventory failure"}});
        flush();
        QVERIFY(text("fedwatchDiagnosticStatus").contains("History incomplete · see Research details"));
    }
    void groupedBarsUseSameBackendValuesAsExactTable() {
        openUpcoming();
        auto* table = panel_->findChild<QTableWidget*>("fedwatchDistribution");
        const auto rows = currentChart()->rows();
        QCOMPARE(rows.size(), table->rowCount());
        QCOMPARE(currentChart()->value_bounds(), qMakePair(0.0, 100.0));
        int expectedBars = 0;
        for (int i = 0; i < rows.size(); ++i) {
            for (int source = 0; source < 2; ++source) {
                const auto value = rows[i][source == 0 ? "fed_probability_pct" : "polymarket_probability_pct"];
                QVERIFY(value.isDouble());
                ++expectedBars;
                QCOMPARE(table->item(i, source + 1)->text(), QString::number(value.toDouble(), 'f', 2));
                const auto bars = currentChart()->bars();
                QVERIFY(std::any_of(bars.begin(), bars.end(), [&](const auto& bar) {
                    return bar.outcome_bp == rows[i]["outcome_bp"].toInt() &&
                           bar.open_ended == rows[i]["open_ended"].toBool() && bar.source == source &&
                           bar.value == value.toDouble();
                }));
            }
        }
        QCOMPARE(currentChart()->bars().size(), expectedBars);
        const auto meetings = snapshot_["data"].toObject()["meetings"].toArray();
        for (const auto& value : meetings) {
            const auto meeting = value.toObject();
            if (meeting["meeting_date"] != "2026-10-28")
                continue;
            QVERIFY(!meeting["comparison"].toArray().isEmpty());
            for (const auto& comparison : meeting["comparison"].toArray()) {
                const auto backendRow = comparison.toObject();
                const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
                    return row["outcome_bp"] == backendRow["outcome_bp"] &&
                           row["open_ended"].toBool() == backendRow["open_ended"].toBool();
                });
                QVERIFY(found != rows.end());
                QCOMPARE((*found)["fed_probability_pct"], backendRow["fed_probability_pct"]);
                QCOMPARE((*found)["polymarket_probability_pct"], backendRow["polymarket_probability_pct"]);
            }
        }
        panel_->resize(1440, 1000);
        panel_->show();
        QTest::qWait(30);
        const auto& colors = fincept::ui::ThemeManager::instance().tokens().chart_colors;
        QCOMPARE(currentChart()->series_color(0), QColor(colors[0]));
        QCOMPARE(currentChart()->series_color(1), QColor(colors[1]));
        QVERIFY(currentChart()->series_color(0) != currentChart()->series_color(1));
        const auto image = currentChart()->grab().toImage();
        for (int source = 0; source < 2; ++source) {
            bool found = false;
            for (int y = 0; y < image.height() && !found; ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y) == currentChart()->series_color(source)) {
                        found = true;
                        break;
                    }
            QVERIFY(found);
        }
        captureFixture("current_grouped_distribution");
    }
    void groupedBarsDistinguishMissingFromObservedZero() {
        openUpcoming();
        currentChart()->set_rows({QJsonObject{{"outcome_bp", 0}, {"fed_probability_pct", QJsonValue::Null}}});
        QVERIFY(currentChart()->bars().isEmpty());
        QVERIFY(currentChart()->rows().isEmpty());
        currentChart()->set_rows({QJsonObject{{"outcome_bp", 0}, {"fed_probability_pct", 0}}});
        QCOMPARE(currentChart()->bars().size(), 1);
        QCOMPARE(currentChart()->bars()[0].value, 0.0);
        QCOMPARE(currentChart()->bars()[0].source, 0);
        QVERIFY(currentChart()->accessibleDescription().contains("Fed-side 0%; Polymarket Unavailable"));
    }
    void currentBarsExposeExactHoverWithoutShowingDesktopWindows() {
        openUpcoming();
        panel_->resize(1440, 1000);
        panel_->show();
        QTest::qWait(30);
        auto* current = currentChart();
        // Hover the first Fed-side categorical slot using widget-local events.
        // No physical mouse, global input, window activation or desktop capture.
        const auto rows = current->rows();
        QVERIFY(!rows.isEmpty());
        const QPointF position(qMin(150.0, current->width() * 0.25) + 10, 62);
        QMouseEvent hover(QEvent::MouseMove, position, current->mapToGlobal(position.toPoint()), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(current, &hover);
        QVERIFY(QToolTip::text().contains(
            "Fed-side: " + QString::number(rows[0]["fed_probability_pct"].toDouble(), 'g', 12) + "%"));
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->isVisible())
                QVERIFY2(widget->testAttribute(Qt::WA_DontShowOnScreen),
                         "Every shown test window must stay off the desktop");
        QToolTip::hideText();
    }
    void primaryBarsFilterEmptyCategoriesAndRetainOneSidedZero() {
        openUpcoming();
        currentChart()->set_rows(
            {{{"outcome_bp", -50}, {"open_ended", true}},
             {{"outcome_bp", -25}, {"fed_probability_pct", 0}},
             {{"outcome_bp", 0}, {"polymarket_probability_pct", 69.125}},
             {{"outcome_bp", 50}, {"open_ended", true}, {"fed_probability_pct", QJsonValue::Null}}});
        QCOMPARE(currentChart()->rows().size(), 2);
        QCOMPARE(currentChart()->rows()[0]["outcome_bp"].toInt(), -25);
        QCOMPARE(currentChart()->rows()[1]["outcome_bp"].toInt(), 0);
        const auto bars = currentChart()->bars();
        QCOMPARE(bars.size(), 2);
        QCOMPARE(bars[0].source, 0);
        QCOMPARE(bars[0].value, 0.0);
        QCOMPARE(bars[1].source, 1);
        QCOMPARE(bars[1].value, 69.125);
    }
    void currentCategoryMouseAndKeyboardSelectAuthoritativeHistory() {
        openUpcoming();
        panel_->resize(1440, 1000);
        panel_->show();
        QTest::qWait(30);
        const auto rows = currentChart()->rows();
        const auto row = rows[0];
        QTest::mouseClick(currentChart(), Qt::LeftButton, Qt::NoModifier, QPoint(25, 62));
        QCOMPARE(pending_.size(), 1);
        QCOMPARE(pending_.first().command, QString("history_analytics"));
        QCOMPARE(panel_->save_panel_state()["outcome_bp"].toInt(), row["outcome_bp"].toInt());
        QCOMPARE(panel_->save_panel_state()["open_ended"].toBool(), row["open_ended"].toBool());
        flush();
        const auto analytics = rawDetails()["analytics"].toObject();
        QCOMPARE(analytics["outcome_bp"], row["outcome_bp"]);
        QCOMPARE(analytics["open_ended"].toBool(), row["open_ended"].toBool());
        QTest::keyClick(currentChart(), Qt::Key_Down);
        flush();
        QCOMPARE(panel_->save_panel_state()["outcome_bp"].toInt(), rows[1]["outcome_bp"].toInt());
        QVERIFY(text("fedwatchStatus").contains(control("fedwatchOutcome")->currentText()));
        QVERIFY(!panel_->findChild<QWidget*>("fedwatchDiagnostics")->isVisible());
    }
    void historicalPanelsAdaptToRetainedObservations_data() {
        QTest::addColumn<bool>("fed");
        QTest::addColumn<bool>("poly");
        QTest::newRow("both") << true << true;
        QTest::newRow("fed-only") << true << false;
        QTest::newRow("polymarket-only") << false << true;
        QTest::newRow("neither") << false << false;
    }
    void historicalPanelsAdaptToRetainedObservations() {
        QFETCH(bool, fed);
        QFETCH(bool, poly);
        if (!poly)
            process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                    {database_, "mapping-without-history", fed ? "25" : "-25"});
        openUpcoming();
        if (!fed) {
            control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("-25:exact"));
            flush();
            control("fedwatchMethod")->selectIndex(control("fedwatchMethod")->findData("HISTORICAL_ZQ_RECONSTRUCTED"));
            flush();
        }
        panel_->resize(1440, 1000);
        panel_->show();
        QTest::qWait(50);
        auto* left = chart("fedwatchProbabilityChart");
        auto* right = chart("fedwatchPolymarketChart");
        QCOMPARE(left->isVisible(), fed);
        QCOMPARE(right->isVisible(), poly);
        QCOMPARE(left->series()[0].points.isEmpty(), !fed);
        QCOMPARE(right->series()[0].points.isEmpty(), !poly);
        QCOMPARE(left->time_bounds(), right->time_bounds());
        QCOMPARE(left->value_bounds(), right->value_bounds());
        if (fed && poly) {
            QCOMPARE(left->mapTo(panel_.get(), QPoint()).y(), right->mapTo(panel_.get(), QPoint()).y());
            QVERIFY(left->mapTo(panel_.get(), QPoint()).x() + left->width() < right->mapTo(panel_.get(), QPoint()).x());
        } else if (fed || poly) {
            auto* populated = fed ? left : right;
            QVERIFY(populated->width() > panel_->width() * 0.85);
        } else {
            QVERIFY(!left->parentWidget()->parentWidget()->isVisible());
        }
        const auto expected = QString("History: Fed-side %1 observations · Polymarket %2 observations")
                                  .arg(left->series()[0].points.size())
                                  .arg(right->series()[0].points.size());
        QVERIFY(text("fedwatchCompactCoverage").contains(expected));
        if (!poly) {
            auto* action = panel_->findChild<QPushButton*>("fedwatchContextLoadHistory");
            QVERIFY(action->isVisible());
            QCOMPARE(action->text(), QString("Load Polymarket history"));
        }
        captureFixture(QString("adaptive_history_") + QTest::currentDataTag());
    }
    void compactCurrentCoverageKeepsQualityAuditable() {
        const auto original = snapshot_;
        changeCurrentSection("polymarket", {{"data_status", "PARTIAL"}});
        openUpcoming();
        snapshot_ = original;
        panel_->resize(1440, 1000);
        panel_->show();
        QTest::qWait(30);
        QVERIFY(text("fedwatchCompactCoverage").contains("Current: Fed-side 5 outcomes · Polymarket 0 outcomes"));
        QCOMPARE(currentChart()->bars().size(), 5);
        for (const auto& bar : currentChart()->bars())
            QCOMPARE(bar.source, 0);
        QVERIFY(!text("fedwatchStatus").contains("PARTIAL"));
        auto* table = panel_->findChild<QTableWidget*>("fedwatchDistribution");
        QVERIFY(!table->isVisible());
        QCOMPARE(table->rowCount(), 5);
        for (int i = 0; i < table->rowCount(); ++i)
            QCOMPARE(table->item(i, 2)->text(), QString("Unavailable"));
        panel_->findChild<QToolButton*>("fedwatchDetailsToggle")->click();
        QVERIFY(table->isVisible());
        QVERIFY(text("fedwatchSourceStatus").contains("PARTIAL"));
        QVERIFY(rawDetails()["current_snapshot"].isObject());
    }
    void compactMeetingNavigationSelectsWithoutRibbon() {
        openUpcoming();
        panel_->resize(1440, 1000);
        panel_->show();
        QTest::qWait(30);
        auto* next = panel_->findChild<QToolButton*>("fedwatchNextMeeting");
        auto* previous = panel_->findChild<QToolButton*>("fedwatchPreviousMeeting");
        QVERIFY(next->isVisible());
        QVERIFY(previous->isVisible());
        QVERIFY(!control("fedwatchMeeting")->isVisible());
        QTest::mouseClick(next, Qt::LeftButton);
        flush();
        QCOMPARE(panel_->save_panel_state()["meeting"].toString(), QString("2026-12-09"));
        QTest::mouseClick(previous, Qt::LeftButton);
        flush();
        QCOMPARE(panel_->save_panel_state()["meeting"].toString(), QString("2026-10-28"));
        QCOMPARE(control("fedwatchMeeting")->currentData().toString(), QString("2026-10-28"));
    }
    void analyticsFailuresBelongToOutcomeAndMethodContext_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("next");
        QTest::newRow("outcome") << QString("Outcome") << QString("0:exact");
        QTest::newRow("method") << QString("Method") << QString("HISTORICAL_ZQ_RECONSTRUCTED");
    }
    void analyticsFailuresBelongToOutcomeAndMethodContext() {
        QFETCH(QString, kind);
        QFETCH(QString, next);
        openUpcoming();
        control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("25:exact"));
        const auto failed = pending_.takeFirst();
        deliver(failed, {{"success", false}, {"error", "old selection analytics failed"}});
        QVERIFY(text("fedwatchSourceStatus").contains("old selection analytics failed"));
        control("fedwatchOutcome")->selectIndex(control("fedwatchOutcome")->findData("25:exact"));
        const auto obsoleteRetry = pending_.takeFirst();
        QVERIFY(text("fedwatchSourceStatus").contains("old selection analytics failed"));
        auto* selection = control(qPrintable("fedwatch" + kind));
        selection->selectIndex(selection->findData(next));
        QVERIFY(!text("fedwatchSourceStatus").contains("old selection analytics failed"));
        deliver(obsoleteRetry, {{"success", false}, {"error", "obsolete retry failed"}});
        QVERIFY(!text("fedwatchSourceStatus").contains("obsolete retry failed"));
        const auto current = pending_.takeFirst();
        deliver(current, {{"success", false}, {"error", "current selection analytics failed"}});
        QVERIFY(text("fedwatchSourceStatus").contains("current selection analytics failed"));
        selection->selectIndex(selection->findData(next));
        QVERIFY(text("fedwatchSourceStatus").contains("current selection analytics failed"));
        flush();
        QVERIFY(!text("fedwatchSourceStatus").contains("current selection analytics failed"));
    }
    void failedEmptySelectedHistoryOffersDurableRetry() {
        process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                {database_, "mapping-without-history"});
        openUpcoming();
        QVERIFY(chart("fedwatchPolymarketChart")->series()[0].points.isEmpty());
        auto* retry = panel_->findChild<QPushButton*>("fedwatchContextLoadHistory");
        QVERIFY(!retry->isHidden());
        retry->click();
        auto request = pending_.takeFirst();
        QCOMPARE(request.command, QString("history_backfill"));
        QVERIFY(!request.args.contains("--force"));
        deliver(request, process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                                 {database_, "backfill-failure"}));
        flush();
        QVERIFY(chart("fedwatchPolymarketChart")->series()[0].points.isEmpty());
        QVERIFY(!retry->isHidden());
        QCOMPARE(retry->text(), QString("Retry history"));
        QVERIFY(retry->isEnabled());
        captureFixture("failed_history_retry");
        panel_.reset();
        makePanel();
        openUpcoming();
        retry = panel_->findChild<QPushButton*>("fedwatchContextLoadHistory");
        QCOMPARE(retry->text(), QString("Retry history"));
        QVERIFY(!retry->isHidden());
        QVERIFY(text("fedwatchSourceStatus").contains("PROVIDER_ERROR"));
        retry->click();
        request = pending_.takeFirst();
        QCOMPARE(request.command, QString("history_backfill"));
        QVERIFY(!request.args.contains("--force"));
        panel_->findChild<QPushButton*>("fedwatchLoadHistory")->click();
        QVERIFY(pending_.isEmpty());
        deliver(request, process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"),
                                 {database_, "backfill-ok"}));
        flush();
        QCOMPARE(chart("fedwatchPolymarketChart")->series()[0].points.size(), 1);
        QCOMPARE(chart("fedwatchPolymarketChart")->series()[0].points[0].value, 42.0);
        QVERIFY(retry->isHidden());
        captureFixture("retried_history_loaded");
    }
    void resolvedWorkspaceEmphasizesStoredHistoryWithoutCurrentBars() {
        openUpcoming();
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-09-16"));
        flush();
        const auto header = text("fedwatchSummary");
        QVERIFY(header.contains("2026-09-16"));
        QVERIFY(header.contains("RESOLVED"));
        QVERIFY(header.contains("25 bp"));
        QVERIFY(header.contains("Stored history only · no live refresh"));
        QVERIFY(!header.contains("Snapshot retrieved"));
        QVERIFY(!header.contains("Target"));
        QVERIFY(!header.contains("Unavailable"));
        QVERIFY(currentChart()->isHidden());
        QVERIFY(currentChart()->rows().isEmpty());
        QVERIFY(currentChart()->bars().isEmpty());
        QVERIFY(panel_->findChild<QTableWidget*>("fedwatchDistribution")->isHidden());
        QCOMPARE(chart("fedwatchProbabilityChart")->series()[0].points.size(), 4);
        QCOMPARE(chart("fedwatchPolymarketChart")->series()[0].points.size(), 4);
        QVERIFY(panel_->findChild<QPushButton*>("fedwatchContextLoadHistory")->isHidden());
        captureFixture("resolved_stored_history");
    }
    void partialSnapshotUsesCompositeRetrievalWording() {
        const auto original = snapshot_;
        snapshot_["partial"] = true;
        snapshot_["failed_components"] = QJsonArray{"FRED context"};
        openUpcoming();
        snapshot_ = original;
        QVERIFY(!text("fedwatchSummary").contains("Snapshot retrieved"));
        QVERIFY(text("fedwatchSourceStatus").contains("Snapshot retrieved"));
        QVERIFY(text("fedwatchSourceStatus").contains(original["data"].toObject()["retrieved_at"].toString()));
        QVERIFY(!text("fedwatchSummary").contains("Updated"));
        QVERIFY(text("fedwatchSourceStatus").contains("Partial provider result: FRED context"));
    }
    void stateRestorationAndNarrowLayout() {
        openUpcoming();
        control("fedwatchRange")->selectIndex(control("fedwatchRange")->findData(90));
        const auto state = panel_->save_panel_state();
        panel_.reset();
        makePanel();
        panel_->restore_panel_state(state);
        panel_->activate();
        flush();
        QCOMPARE(panel_->save_panel_state()["meeting"], state["meeting"]);
        QCOMPARE(panel_->save_panel_state()["outcome_bp"], state["outcome_bp"]);
        QCOMPARE(panel_->save_panel_state()["range_days"], state["range_days"]);
        control("fedwatchRange")->selectIndex(control("fedwatchRange")->findData(0));
        panel_->resize(1280, 800);
        panel_->show();
        QTest::qWait(50);
        panel_->resize(640, 480);
        QTest::qWait(50);
        QVERIFY(!panel_->grab().isNull());
        QVERIFY(!control("fedwatchMeeting")->isVisible());
        QVERIFY(panel_->findChild<QToolButton*>("fedwatchPreviousMeeting")->isVisible());
        QVERIFY(panel_->findChild<QToolButton*>("fedwatchNextMeeting")->isVisible());
        auto* header = panel_->findChild<QLabel*>("fedwatchSummary");
        QVERIFY2(header->heightForWidth(header->width()) <= header->height(),
                 "The compact meeting/target/retrieval header must not clip at narrow panel width");
        auto* strip = panel_->findChild<QScrollArea*>("fedwatchRangeStrip");
        auto* selected = control("fedwatchRange")->buttons()[control("fedwatchRange")->findData(0)];
        const QRect selectedBounds(selected->mapTo(strip->viewport(), QPoint(0, 0)), selected->size());
        QVERIFY2(strip->viewport()->rect().contains(selectedBounds),
                 "The selected last range must remain physically inside its strip after narrowing");
        captureFixture("narrow_workspace", 640);
        panel_->show();
        panel_->findChild<QToolButton*>("fedwatchDetailsToggle")->click();
        QTest::qWait(50);
        auto* mainScroll =
            panel_->findChild<QWidget*>("fedwatchDiagnostics")->parentWidget()->parentWidget()->parentWidget();
        auto* areaMain = qobject_cast<QScrollArea*>(mainScroll);
        QVERIFY(areaMain);
        areaMain->ensureWidgetVisible(panel_->findChild<QWidget*>("fedwatchControlRow"));
        QTest::qWait(50);
        auto* viewport = areaMain->viewport();
        int previousY = -1;
        for (const auto& kind : {QString("Meeting"), QString("Outcome")}) {
            const auto name = "fedwatch" + kind;
            auto* label = panel_->findChild<QLabel*>(name + "Label");
            QVERIFY(label->isVisible());
            const QRect labelBounds(label->mapTo(viewport, QPoint()), label->size());
            QVERIFY2(viewport->rect().contains(labelBounds), qPrintable(name + " label must fit the main viewport"));
            QVERIFY(labelBounds.y() > previousY);
            previousY = labelBounds.y();
            const auto choices = control(qPrintable(name))->buttons();
            const auto choice = std::find_if(choices.begin(), choices.end(), [](auto* b) { return b->isChecked(); });
            QVERIFY(choice != choices.end());
            auto* chip = *choice;
            auto* area = panel_->findChild<QScrollArea*>(name + "Strip");
            QVERIFY(area->viewport()->rect().contains(QRect(chip->mapTo(area->viewport(), QPoint()), chip->size())));
            QVERIFY2(viewport->rect().contains(QRect(chip->mapTo(viewport, QPoint()), chip->size())),
                     qPrintable(name + " selected choice must fit the main viewport"));
        }
        // Primary range selection remains independently available above history.
        areaMain->ensureWidgetVisible(strip);
        QTest::qWait(50);
        QVERIFY(strip->isVisible());
        QVERIFY(viewport->rect().contains(QRect(selected->mapTo(viewport, QPoint()), selected->size())));
    }
};
QTEST_MAIN(TestFedWatchPanel)
#include "tst_fedwatch_panel.moc"
