// Native widget + shipped backend CLI integration with deterministic captured
// provider data. No live HTTP and no substitute history/analytics implementation.
#include "screens/economics/panels/FedWatchHistoryChart.h"
#include "screens/economics/panels/FedWatchPanel.h"

#include <QComboBox>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

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
    QString python_, database_;
    QJsonObject snapshot_;
    QList<Request> pending_, sent_;
    std::unique_ptr<FedWatchPanel> panel_;
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
        result.success = envelope["success"].toBool();
        result.data = envelope;
        result.error = envelope["error"].toObject()["error"].toString();
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
    void openUpcoming() {
        panel_->restore_panel_state({{"meeting", "2026-10-28"},
                                     {"outcome_bp", 25},
                                     {"open_ended", false},
                                     {"fed_method", "LIVE_INVESTING_DERIVED"},
                                     {"range_days", 0}});
        panel_->activate();
        flush();
    }
  private slots:
    void initTestCase() {
        python_ = qEnvironmentVariable("FEDWATCH_TEST_PYTHON");
        QVERIFY2(!python_.isEmpty(), "Set FEDWATCH_TEST_PYTHON to the existing MarketLab Python runtime");
        QVERIFY(temporary_.isValid());
        database_ = temporary_.filePath("history.db");
        snapshot_ = process(QStringLiteral(FEDWATCH_TEST_SOURCE_DIR "/tests/fedwatch_ui_fixture.py"), {database_});
        QVERIFY(snapshot_["success"].toBool());
    }
    void init() {
        pending_.clear();
        sent_.clear();
        panel_ =
            std::make_unique<FedWatchPanel>([this](const QString& command, const QStringList& args, const QString& id) {
                const Request request{command, args, id};
                pending_.append(request);
                sent_.append(request);
            });
    }
    void cleanup() { panel_.reset(); }

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
        QVERIFY(text("fedwatchStatus").contains("RESOLVED", Qt::CaseInsensitive));
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
        QVERIFY(chart("fedwatchProbabilityChart")->series()[1].points.isEmpty());
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
        QVERIFY(text("fedwatchStatus").contains("No meetings"));
        QVERIFY(text("fedwatchSourceStatus").contains("FRED target unavailable"));
        QVERIFY(text("fedwatchSummary").contains("Unavailable"));
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
        QVERIFY(text("fedwatchStatus").contains("stale"));
        QVERIFY(text("fedwatchStatus").contains("not found"));
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
        for (const auto& request : old)
            deliver(request, backend(request));
        QCOMPARE(panel_->save_panel_state(), saved);
        QCOMPARE(tableText("fedwatchDistribution"), before);
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
        QVERIFY(text("fedwatchStatus").contains("Investing"));
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
        QVERIFY(text("fedwatchStatus").contains("ambiguous"));
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
        panel_->activateWindow();
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
        panel_->resize(1280, 800);
        panel_->show();
        panel_->activateWindow();
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
        for (const char* name : {"fedwatchProbabilityChart", "fedwatchDivergenceChart"}) {
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
        deliver(request, QJsonObject{{"success", true}, {"data", QJsonObject{{"errors", QJsonArray{}}}}});
        flush();
        QVERIFY(button->isEnabled());
        QVERIFY(panel_->findChild<QPushButton*>("econFetchBtn")->isEnabled());
        QVERIFY(text("fedwatchCoverage").contains("Polymarket observations"));
    }
    void historyFailureAndSelectionRacePreserveLocalResolvedView() {
        openUpcoming();
        auto* button = panel_->findChild<QPushButton*>("fedwatchLoadHistory");
        button->click();
        const auto request = pending_.takeFirst();
        control("fedwatchMeeting")->selectIndex(control("fedwatchMeeting")->findData("2026-09-16"));
        flush();
        const auto before = sent_.size();
        deliver(
            request,
            QJsonObject{{"success", true},
                        {"data", QJsonObject{{"errors", QJsonArray{QJsonObject{{"provider", "polymarket"},
                                                                               {"code", "POLYMARKET_HTTP_ERROR"},
                                                                               {"error", "provider unavailable"}}}}}}});
        flush();
        QCOMPARE(panel_->save_panel_state()["meeting"].toString(), QString("2026-09-16"));
        QVERIFY(!button->isEnabled());
        QVERIFY(panel_->findChild<QPushButton*>("econFetchBtn")->isEnabled());
        QVERIFY(text("fedwatchSourceStatus").contains("POLYMARKET_HTTP_ERROR"));
        QVERIFY(!chart("fedwatchProbabilityChart")->series().isEmpty());
        for (int i = before; i < sent_.size(); ++i)
            QVERIFY(sent_[i].command != "collect");
    }
    void stateRestorationAndNarrowLayout() {
        openUpcoming();
        control("fedwatchRange")->selectIndex(control("fedwatchRange")->findData(90));
        const auto state = panel_->save_panel_state();
        panel_.reset();
        panel_ = std::make_unique<FedWatchPanel>([this](const QString& c, const QStringList& a, const QString& i) {
            const Request r{c, a, i};
            pending_.append(r);
            sent_.append(r);
        });
        panel_->restore_panel_state(state);
        panel_->activate();
        flush();
        QCOMPARE(panel_->save_panel_state()["meeting"], state["meeting"]);
        QCOMPARE(panel_->save_panel_state()["outcome_bp"], state["outcome_bp"]);
        QCOMPARE(panel_->save_panel_state()["range_days"], state["range_days"]);
        panel_->resize(640, 480);
        panel_->show();
        QTest::qWait(50);
        QVERIFY(!panel_->grab().isNull());
        QVERIFY(control("fedwatchMeeting")->isVisible());
    }
};
QTEST_MAIN(TestFedWatchPanel)
#include "tst_fedwatch_panel.moc"
