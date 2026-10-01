#include "screens/economics/panels/FedWatchPanel.h"

#include "core/config/ProfileManager.h"
#include "core/session/ScreenStateManager.h"
#include "screens/common/IStatefulScreen.h"
#include "screens/economics/panels/FedWatchHistoryChart.h"
#include "ui/theme/Theme.h"

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace fincept::screens {
namespace {
void notify_state(QWidget* widget) {
    for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget())
        if (auto* screen = dynamic_cast<IStatefulScreen*>(parent)) {
            ScreenStateManager::instance().notify_changed(screen);
            return;
        }
}
QJsonObject payload(const QJsonObject& envelope) {
    return envelope["data"].isObject() ? envelope["data"].toObject() : envelope;
}
QString key(int bp, bool open) {
    return QString::number(bp) + (open ? ":tail" : ":exact");
}
void cell(QTableWidget* table, int row, int column, const QString& text) {
    table->setItem(row, column, new QTableWidgetItem(text));
}
QString errors(const QJsonObject& data) {
    QStringList result;
    for (const auto& item : data["errors"].toArray()) {
        const auto error = item.toObject();
        result << error["provider"].toString() + " · " + error["code"].toString() + ": " + error["error"].toString();
    }
    return result.join("\n");
}
} // namespace
FedWatchPanel::FedWatchPanel(QWidget* parent) : FedWatchPanel(Dispatch{}, parent) {}
FedWatchPanel::FedWatchPanel(Dispatch dispatch, QWidget* parent)
    : EconPanelBase("fedwatch", "#3B82F6", parent), dispatch_(std::move(dispatch)) {
    setObjectName("fedwatchPanel");
    build_base_ui(this);
    set_stats_visible(false);
    export_btn_->hide();
    fetch_btn_->setText(tr("REFRESH"));
    fetch_btn_->setAccessibleName(tr("Refresh FedWatch"));
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setSpacing(8);
    layout->addWidget(controls_);
    auto label = [&](const char* name) {
        auto* value = new QLabel(content);
        value->setObjectName(name);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(value);
        return value;
    };
    summary_ = label("fedwatchSummary");
    status_ = label("fedwatchStatus");
    selected_current_ = label("fedwatchSelectedCurrent");
    auto* tabs = new QTabWidget(content);
    tabs->setObjectName("fedwatchViews");
    layout->addWidget(tabs);
    auto* overview = new QWidget(tabs);
    auto* overview_layout = new QVBoxLayout(overview);
    overview_layout->setContentsMargins(0, 8, 0, 0);
    tabs->addTab(overview, tr("Overview"));
    auto* graph_controls = new QWidget(overview);
    auto* graph_controls_layout = new QHBoxLayout(graph_controls);
    graph_controls_layout->setContentsMargins(0, 0, 0, 0);
    graph_controls_layout->addWidget(make_chip_row("method", methods_, 40), 1);
    graph_controls_layout->addWidget(make_chip_row("range", ranges_, 40), 1);
    overview_layout->addWidget(graph_controls);
    auto* detailed = new QWidget(tabs);
    auto* detailed_layout = new QVBoxLayout(detailed);
    tabs->addTab(detailed, tr("Details"));
    auto* sources = new QWidget(tabs);
    auto* sources_layout = new QVBoxLayout(sources);
    tabs->addTab(sources, tr("Sources"));
    auto* charts = new QWidget(overview);
    charts_layout_ = new QGridLayout(charts);
    charts_layout_->setContentsMargins(0, 0, 0, 0);
    charts_layout_->setSpacing(10);
    overview_layout->addWidget(charts);
    auto chart_section = [&](const char* name, const QString& title) {
        auto* section = new QWidget(charts);
        auto* section_layout = new QVBoxLayout(section);
        section_layout->setContentsMargins(0, 0, 0, 0);
        auto* heading = new QLabel(title, section);
        heading->setObjectName(name);
        section_layout->addWidget(heading);
        return section;
    };
    probability_section_ = chart_section("fedwatchProbabilityTitle", tr("Probability evolution (%)"));
    probability_section_->layout()->addWidget(make_chip_row("outcome", outcomes_, 40));
    probability_ = new FedWatchHistoryChart(probability_section_);
    probability_->setObjectName("fedwatchProbabilityChart");
    probability_section_->layout()->addWidget(probability_);
    divergence_section_ = chart_section("fedwatchDivergenceTitle", tr("Polymarket − Fed-side (pp)"));
    divergence_ = new FedWatchHistoryChart(divergence_section_);
    divergence_->set_probability_scale(false);
    divergence_->setObjectName("fedwatchDivergenceChart");
    divergence_section_->layout()->addWidget(divergence_);
    arrange_charts();
    coverage_ = new QLabel(overview);
    coverage_->setObjectName("fedwatchCoverage");
    coverage_->setWordWrap(true);
    overview_layout->addWidget(coverage_);
    auto* chart_note = new QLabel(
        tr("Hover to inspect observations. Lines stop at missing UTC days. Comparison is descriptive."), overview);
    chart_note->setWordWrap(true);
    overview_layout->addWidget(chart_note);
    auto table = [&](const char* name, const QStringList& headings) {
        auto* value = new QTableWidget(detailed);
        value->setObjectName(name);
        value->setColumnCount(headings.size());
        value->setHorizontalHeaderLabels(headings);
        value->setEditTriggers(QAbstractItemView::NoEditTriggers);
        value->setAlternatingRowColors(true);
        value->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        value->horizontalHeader()->setStretchLastSection(true);
        value->verticalHeader()->hide();
        value->setMinimumHeight(170);
        detailed_layout->addWidget(value);
        return value;
    };
    auto* distribution_title = new QLabel(tr("Current outcome distribution"), detailed);
    distribution_title->setObjectName("fedwatchDistributionTitle");
    detailed_layout->addWidget(distribution_title);
    distribution_ = table("fedwatchDistribution",
                          {tr("Outcome"), tr("Fed-side %"), tr("Polymarket %"), tr("Difference pp"), tr("State")});
    auto* indicators_title = new QLabel(tr("Historical probability changes (pp)"), detailed);
    indicators_title->setObjectName("fedwatchIndicatorTitle");
    detailed_layout->addWidget(indicators_title);
    indicators_ = table("fedwatchIndicators", {tr("Measure"), tr("Fed-side"), tr("Polymarket")});
    source_status_ = new QLabel(sources);
    source_status_->setObjectName("fedwatchSourceStatus");
    source_status_->setWordWrap(true);
    source_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sources_layout->addWidget(source_status_);
    auto* toggle = new QToolButton(sources);
    toggle->setObjectName("fedwatchDetailsToggle");
    toggle->setText(tr("Source / raw data / methodology details"));
    toggle->setAccessibleName(tr("Source / raw data / methodology details"));
    toggle->setCheckable(true);
    sources_layout->addWidget(toggle);
    details_ = new QPlainTextEdit(sources);
    details_->setObjectName("fedwatchDetails");
    details_->setReadOnly(true);
    details_->setMinimumHeight(280);
    details_->hide();
    sources_layout->addWidget(details_);
    connect(toggle, &QToolButton::toggled, details_, &QWidget::setVisible);
    connect(toggle, &QToolButton::toggled, this, [this] { notify_state(this); });
    auto* research = new QLabel(sources);
    research->setObjectName("fedwatchResearchNotice");
    research->setWordWrap(true);
    research->setText(tr("Read-only research. Local Fed-side distributions may be narrower than Polymarket tails. Live "
                         "Investing-derived and historical ZQ-reconstructed probabilities are separate methods. Gaps "
                         "remain missing; comparison is descriptive."));
    sources_layout->addWidget(research);
    auto* timestamp_note =
        new QLabel(tr("Countdown is unavailable from the backend. Investing publishes no source timestamp; retrieval "
                      "time is used. Source errors and raw observations are retained below."),
                   sources);
    timestamp_note->setWordWrap(true);
    sources_layout->addWidget(timestamp_note);
    layout->addStretch();
    scroll->setWidget(content);
    workspace_page_ = add_content_page(scroll);
    show_content_page(workspace_page_);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &FedWatchPanel::accept_result);
    render();
    refresh_panel_theme();
}
void FedWatchPanel::build_controls(QHBoxLayout* toolbar) {
    update_upcoming_ = new QPushButton(tr("Update upcoming meetings"), this);
    update_upcoming_->setObjectName("fedwatchUpdateUpcoming");
    update_upcoming_->setAccessibleName(tr("Update upcoming meetings"));
    update_upcoming_->setToolTip(tr("Collect current upcoming observations; resolved meeting history stays local."));
    toolbar->addWidget(update_upcoming_);
    connect(update_upcoming_, &QPushButton::clicked, this, [this] {
        if (collect_in_flight_ || backfill_in_flight_)
            return;
        current_ok_ = false;
        current_error_.clear();
        collect_after_overview_ = false;
        render();
        status_->setText(status_->text() + tr("\nUpdating upcoming meetings from current providers…"));
        request("collect");
    });
    load_history_ = new QPushButton(tr("Load history"), this);
    load_history_->setObjectName("fedwatchLoadHistory");
    load_history_->setAccessibleName(tr("Load Polymarket history"));
    load_history_->setToolTip(tr("Download available Polymarket observations for the selected upcoming meeting. "
                                 "Fed-side history remains the locally retained archive."));
    toolbar->addWidget(load_history_);
    connect(load_history_, &QPushButton::clicked, this, [this] {
        if (collect_in_flight_ || backfill_in_flight_ || resolved() || selected_meeting_.isEmpty())
            return;
        collect_after_overview_ = false;
        backfill_error_.clear();
        backfill_result_ = {};
        request("history_backfill", {"--meeting", selected_meeting_});
        render();
        status_->setText(tr("Loading Polymarket historical observations…"));
        status_->setAccessibleDescription(tr("Loading historical data"));
    });
    controls_ = new QWidget(this);
    auto* timeline_layout = new QVBoxLayout(controls_);
    timeline_layout->setContentsMargins(0, 0, 0, 0);
    meetings_ = new Selection(this);
    outcomes_ = new Selection(this);
    methods_ = new Selection(this);
    ranges_ = new Selection(this);
    methods_->addItem(tr("Live Investing-derived"), "LIVE_INVESTING_DERIVED");
    methods_->addItem(tr("Historical ZQ-reconstructed"), "HISTORICAL_ZQ_RECONSTRUCTED");
    methods_->setCurrentIndex(0);
    ranges_->addItem(tr("7D"), 7);
    ranges_->addItem(tr("30D"), 30);
    ranges_->addItem(tr("90D"), 90);
    ranges_->addItem(tr("Full retained"), 0);
    ranges_->setCurrentIndex(3);
    timeline_layout->addWidget(make_chip_row("meeting", meetings_, 66));
    meetings_->changed = [this] {
        selected_meeting_ = meetings_->currentData().toString();
        meeting_explicitly_selected_ = !selected_meeting_.isEmpty();
        ++generation_;
        analytics_ = {};
        series_ = {};
        history_error_.clear();
        load_meeting();
        notify_state(this);
    };
    auto selection = [this] {
        analytics_ = {};
        load_analytics();
        notify_state(this);
    };
    outcomes_->changed = [this, selection] {
        const auto identity = outcomes_->currentData().toString();
        if (!identity.isEmpty()) {
            selected_outcome_ = identity;
            restored_outcome_.clear();
        }
        selection();
    };
    methods_->changed = selection;
    ranges_->changed = [this] {
        render_history();
        notify_state(this);
    };
}
QWidget* FedWatchPanel::make_chip_row(const QString& kind, Selection* model, int height) {
    Q_UNUSED(model);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setFixedHeight(height);
    scroll->setObjectName("fedwatch" + kind.left(1).toUpper() + kind.mid(1) + "Strip");
    auto* content = new QWidget(scroll);
    content->setObjectName(kind == "meeting" ? "fedwatchMeetingTimeline"
                                             : "fedwatch" + kind.left(1).toUpper() + kind.mid(1) + "Segments");
    auto* row = new QHBoxLayout(content);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    scroll->setWidget(content);
    if (kind == "meeting")
        meeting_chips_ = content;
    else if (kind == "outcome")
        outcome_chips_ = content;
    else if (kind == "method")
        method_chips_ = content;
    else
        range_chips_ = content;
    if (kind != "meeting")
        return scroll;
    auto* timeline = new QWidget(this);
    auto* timeline_layout = new QHBoxLayout(timeline);
    timeline_layout->setContentsMargins(0, 0, 0, 0);
    auto* previous = new QToolButton(timeline);
    previous->setText("‹");
    previous->setAccessibleName(tr("Earlier meetings"));
    previous->setObjectName("fedwatchEarlierMeetings");
    auto* next = new QToolButton(timeline);
    next->setText("›");
    next->setAccessibleName(tr("Later meetings"));
    next->setObjectName("fedwatchLaterMeetings");
    timeline_layout->addWidget(previous);
    timeline_layout->addWidget(scroll, 1);
    timeline_layout->addWidget(next);
    connect(previous, &QToolButton::clicked, scroll,
            [scroll] { scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->value() - 250); });
    connect(next, &QToolButton::clicked, scroll,
            [scroll] { scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->value() + 250); });
    return timeline;
}
void FedWatchPanel::sync_chips() {
    const QVector<QPair<QString, Selection*>> groups{
        {"meeting", meetings_}, {"outcome", outcomes_}, {"method", methods_}, {"range", ranges_}};
    const QVector<QWidget*> containers{meeting_chips_, outcome_chips_, method_chips_, range_chips_};
    for (int group = 0; group < groups.size(); ++group) {
        auto* container = containers[group];
        if (!container)
            continue;
        auto* row = qobject_cast<QHBoxLayout*>(container->layout());
        const auto& kind = groups[group].first;
        auto* model = groups[group].second;
        QVariantList signature;
        for (int index = 0; index < model->count(); ++index)
            signature.push_back(QVariantMap{{"text", model->itemText(index)}, {"value", model->itemData(index)}});
        auto* scroll = qobject_cast<QScrollArea*>(container->parentWidget()->parentWidget());
        const int scroll_position = scroll ? scroll->horizontalScrollBar()->value() : 0;
        const bool selection_changed = container->property("selected_value") != model->currentData();
        if (container->property("choices_signature").toList() == signature &&
            container->property("choices_signature").isValid()) {
            for (auto* button : container->findChildren<QPushButton*>(QString{}, Qt::FindDirectChildrenOnly)) {
                if (button->isHidden())
                    continue;
                const bool selected = button->property("selection_value") == model->currentData();
                button->setChecked(selected);
                if (selected && selection_changed && scroll)
                    scroll->ensureWidgetVisible(button, 8, 0);
            }
            container->setProperty("selected_value", model->currentData());
            continue;
        }
        QVariant focused_identity;
        if (auto* focus = QApplication::focusWidget(); focus && focus->parentWidget() == container)
            focused_identity = focus->property("selection_value");
        while (auto* item = row->takeAt(0)) {
            if (auto* widget = item->widget()) {
                widget->hide();
                widget->deleteLater();
            }
            delete item;
        }
        QPushButton* active_button = nullptr;
        QPushButton* focused_button = nullptr;
        QPushButton* previous_button = nullptr;
        for (int index = 0; index < model->count(); ++index) {
            QString text = model->itemText(index);
            if (kind == "meeting")
                text.replace(" · ", "\n");
            if (kind == "method")
                text = index == 0 ? tr("Investing-derived") : tr("ZQ reconstructed");
            auto* button = new QPushButton(text, container);
            button->setObjectName("fedwatch" + kind.left(1).toUpper() + kind.mid(1) + "Chip_" + QString::number(index));
            button->setProperty("selection_kind", kind);
            button->setProperty("selection_value", model->itemData(index));
            button->setProperty("selection_index", index);
            button->setCheckable(true);
            button->setChecked(model->currentIndex() == index);
            button->setAccessibleName(model->itemText(index));
            button->setAccessibleDescription(tr("Select %1").arg(kind));
            button->setCursor(Qt::PointingHandCursor);
            if (kind == "meeting")
                button->setMinimumWidth(126);
            connect(button, &QPushButton::clicked, this, [this, model, index] {
                if (model->currentIndex() == index) {
                    if (model->changed)
                        model->changed(); // A user activation still establishes explicit selection intent.
                } else
                    model->setCurrentIndex(index);
                sync_chips();
            });
            row->addWidget(button);
            if (previous_button)
                QWidget::setTabOrder(previous_button, button);
            previous_button = button;
            if (button->isChecked())
                active_button = button;
            if (focused_identity.isValid() && button->property("selection_value") == focused_identity)
                focused_button = button;
        }
        if (model->count() == 0)
            row->addWidget(new QLabel(
                kind == "meeting" ? tr("Meetings appear after updating") : tr("No outcomes available"), container));
        row->addStretch();
        row->activate();
        container->adjustSize();
        if (focused_button)
            focused_button->setFocus(Qt::OtherFocusReason);
        if (scroll) {
            scroll->horizontalScrollBar()->setValue(scroll_position);
            if (active_button && (selection_changed || !container->property("choices_signature").isValid())) {
                scroll->ensureWidgetVisible(active_button, 8, 0);
                // Scrollbar ranges settle after the layout event for a newly rebuilt timeline.
                QPointer<QScrollArea> guarded_scroll(scroll);
                QPointer<QPushButton> guarded_button(active_button);
                QTimer::singleShot(0, scroll, [guarded_scroll, guarded_button] {
                    if (guarded_scroll && guarded_button && guarded_button->isChecked() && !guarded_button->isHidden())
                        guarded_scroll->ensureWidgetVisible(guarded_button, 8, 0);
                });
            }
        }
        container->setProperty("choices_signature", signature);
        container->setProperty("selected_value", model->currentData());
    }
}
void FedWatchPanel::request(const QString& command, const QStringList& args) {
    if (command == "collect" || command == "history_backfill") {
        if (collect_in_flight_ || backfill_in_flight_)
            return;
        if (command == "collect")
            collect_in_flight_ = true;
        else
            backfill_in_flight_ = true;
        fetch_btn_->setEnabled(false);
        update_upcoming_->setEnabled(false);
        load_history_->setEnabled(false);
    }
    QStringList durable_args = args;
    durable_args << "--db" << ProfileManager::instance().profile_root() + "/fedwatch/fedwatch_history.db";
    const QString id = QString("fedwatch:%1:%2").arg(quintptr(this)).arg(++sequence_);
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it.value().command != "collect" && it.value().command != "history_backfill" &&
            (it.value().command == command || it.value().generation != generation_))
            it = pending_.erase(it);
        else
            ++it;
    }
    pending_[id] = {command, generation_};
    emit command_requested(command, durable_args, id);
    if (dispatch_)
        dispatch_(command, durable_args, id);
    else
        services::EconomicsService::instance().execute("fedwatch", "fedwatch_data.py", command, durable_args, id, true);
}
void FedWatchPanel::activate() {
    if (!activated_) {
        activated_ = true;
        on_fetch();
    }
}
void FedWatchPanel::on_fetch() {
    if (collect_in_flight_ || backfill_in_flight_)
        return;
    ++generation_;
    history_error_.clear();
    current_error_.clear();
    current_ok_ = false;
    analytics_ = {};
    collect_after_overview_ = true;
    render();
    status_->setText(tr("Loading meeting inventory from local history…"));
    request("history_meetings");
}
QJsonObject FedWatchPanel::meeting() const {
    for (const auto& item : overview_["meetings"].toArray())
        if (item.toObject()["meeting_date"].toString() == selected_meeting_)
            return item.toObject();
    return {};
}
QJsonObject FedWatchPanel::current_meeting() const {
    for (const auto& item : snapshot_["meetings"].toArray())
        if (item.toObject()["meeting_date"].toString() == selected_meeting_)
            return item.toObject();
    return {};
}
bool FedWatchPanel::resolved() const {
    return meeting()["status"].toString() == "RESOLVED";
}
void FedWatchPanel::rebuild_meetings() {
    QSignalBlocker blocker(meetings_);
    meetings_->clear();
    QMap<QString, QString> dates;
    for (const auto& item : snapshot_["meetings"].toArray())
        dates[item.toObject()["meeting_date"].toString()] = item.toObject()["status"].toString();
    for (const auto& item : overview_["meetings"].toArray())
        dates[item.toObject()["meeting_date"].toString()] = item.toObject()["status"].toString();
    for (auto it = dates.begin(); it != dates.end(); ++it)
        if (!it.key().isEmpty())
            meetings_->addItem(it.key() + " · " + it.value(), it.key());
    int index = meetings_->findData(selected_meeting_);
    if (index < 0) {
        index = 0;
        for (int i = 0; i < meetings_->count(); ++i)
            if (dates[meetings_->itemData(i).toString()] == "UPCOMING") {
                index = i;
                break;
            }
    }
    meetings_->setCurrentIndex(index);
    selected_meeting_ = meetings_->currentData().toString();
}
void FedWatchPanel::load_meeting() {
    analytics_ = {};
    series_ = {};
    rebuild_outcomes();
    render();
    if (selected_meeting_.isEmpty()) {
        status_->setText(status_->text() + tr("\nNo meetings available. Refresh collects upcoming observations."));
        return;
    }
    status_->setText(tr("Loading stored meeting history…"));
    request("history_series", {"--meeting", selected_meeting_});
}
void FedWatchPanel::rebuild_outcomes() {
    const QString previous = !restored_outcome_.isEmpty()                     ? restored_outcome_
                             : !outcomes_->currentData().toString().isEmpty() ? outcomes_->currentData().toString()
                                                                              : selected_outcome_;
    QSignalBlocker blocker(outcomes_);
    outcomes_->clear();
    QMap<QString, QJsonObject> available;
    auto add = [&](const QJsonArray& rows) {
        for (const auto& value : rows) {
            const auto row = value.toObject();
            if (!row["outcome_bp"].isDouble())
                continue;
            available[key(row["outcome_bp"].toInt(), row["open_ended"].toBool())] = row;
        }
    };
    add(series_["observations"].toArray());
    add(meeting()["polymarket_mapping"].toObject()["outcomes"].toArray());
    const auto current = current_meeting();
    add(current["fed_side"].toObject()["local_probabilities"].toArray());
    add(current["polymarket"].toObject()["outcomes"].toArray());
    add(current["comparison"].toArray());
    for (auto it = available.begin(); it != available.end(); ++it)
        outcomes_->addItem(fedwatch::outcome_label(it.value()["outcome_bp"].toInt(), it.value()["open_ended"].toBool()),
                           it.key());
    int index = outcomes_->findData(previous);
    outcomes_->setCurrentIndex(index < 0 ? 0 : index);
    if (!outcomes_->currentData().toString().isEmpty() && (index >= 0 || previous.isEmpty()))
        selected_outcome_ = outcomes_->currentData().toString();
    if (index >= 0)
        restored_outcome_.clear();
}
void FedWatchPanel::load_analytics() {
    render();
    if (selected_meeting_.isEmpty() || outcomes_->currentIndex() < 0)
        return;
    QStringList args{"--meeting",    selected_meeting_,
                     "--outcome-bp", outcomes_->currentData().toString().section(':', 0, 0),
                     "--fed-method", methods_->currentData().toString()};
    if (outcomes_->currentData().toString().endsWith(":tail"))
        args << "--open-ended";
    status_->setText(tr("Loading stored probability analytics…"));
    request("history_analytics", args);
}
void FedWatchPanel::accept_result(const QString& id, const services::EconomicsResult& result) {
    on_result(id, result);
}
void FedWatchPanel::on_result(const QString& id, const services::EconomicsResult& result) {
    const auto it = pending_.find(id);
    if (it == pending_.end())
        return;
    const auto pending = it.value();
    pending_.erase(it);
    if (pending.generation != generation_ && pending.command != "collect" && pending.command != "history_backfill")
        return;
    const auto data = payload(result.data);
    const bool usable = result.success || (result.data["success"].toBool() && result.data["data"].isObject());
    if (pending.command == "history_backfill") {
        backfill_in_flight_ = false;
        analytics_ = {};
        backfill_result_ = data;
        backfill_error_ = errors(data);
        if (!usable && backfill_error_.isEmpty())
            backfill_error_ = result.error;
        collect_after_overview_ = false;
        request("history_meetings");
        render();
        return;
    }
    if (pending.command == "collect") {
        collect_in_flight_ = false;
        fetch_btn_->setEnabled(true);
        update_upcoming_->setEnabled(true);
        current_ok_ = usable;
        current_error_ = errors(data);
        if (!result.success && current_error_.isEmpty())
            current_error_ = result.error;
        snapshot_ = usable ? data : QJsonObject{};
        request("history_meetings");
        render();
        return;
    }
    if (pending.command == "history_meetings") {
        if (usable) {
            overview_ = data;
            rebuild_meetings();
            sync_chips(); // Stored meetings remain selectable while current providers are being collected.
        } else
            history_error_ = tr("Meeting history unavailable: ") + result.error;
        if (collect_after_overview_) {
            collect_after_overview_ = false;
            if (result.success && resolved() && meeting_explicitly_selected_) {
                load_meeting();
                return;
            }
            if (!result.success && !selected_meeting_.isEmpty()) {
                load_meeting();
                return;
            }
            status_->setText(tr("Refreshing current providers and retaining accepted observations…"));
            request("collect");
            return;
        }
        load_meeting();
        return;
    }
    if (pending.command == "history_series") {
        series_ = usable ? data : QJsonObject{};
        history_error_ = result.success ? errors(data) : tr("Stored series unavailable: ") + result.error;
        rebuild_outcomes();
        load_analytics();
        return;
    }
    if (pending.command == "history_analytics") {
        analytics_ = usable ? data : QJsonObject{};
        history_error_ = result.success ? errors(data) : tr("Analytics unavailable: ") + result.error;
        render();
    }
}
void FedWatchPanel::render() {
    const auto target = snapshot_["current_target_range"].toObject();
    const auto stored = meeting();
    const auto current = current_meeting();
    const auto fed = current["fed_side"].toObject();
    const auto poly = current["polymarket"].toObject();
    const bool show_current = current_ok_ && !resolved();
    QString next = tr("Unavailable");
    const auto current_meetings = snapshot_["meetings"].toArray();
    if (show_current && !current_meetings.isEmpty())
        next = current_meetings.first().toObject()["meeting_date"].toString();
    summary_->setText(
        tr("Fed target  %1 – %2    |    Next meeting  %4    |    FRED as of %3\n%5 · %6%7    |    Updated  %8")
            .arg(show_current ? fedwatch::number(target["lower"], "%") : tr("Unavailable"),
                 show_current ? fedwatch::number(target["upper"], "%") : tr("Unavailable"),
                 target["latest_observation_date"].toString(tr("Unavailable")), next, selected_meeting_,
                 stored["status"].toString(current["status"].toString(tr("Unavailable"))),
                 stored["actual_outcome_bp"].isDouble()
                     ? tr(" · Actual policy outcome: %1 bp").arg(stored["actual_outcome_bp"].toInt())
                     : QString{},
                 show_current ? snapshot_["retrieved_at"].toString(tr("Unavailable"))
                              : tr("Not refreshed; stored history only")));
    QStringList state;
    if (resolved())
        state << tr("RESOLVED · stored local history; no live collection");
    else if (!current_ok_)
        state << tr("Current providers unavailable or not refreshed. Stored observations are historical, not current.");
    if (!current_error_.isEmpty())
        state << current_error_;
    if (!history_error_.isEmpty())
        state << history_error_;
    if (!backfill_error_.isEmpty())
        state << tr("Polymarket history update: ") + backfill_error_;
    if (!selected_meeting_.isEmpty() && !resolved()) {
        state << tr("Fed-side: %1 · %2 | Polymarket mapping: %3 · data: %4 · freshness: %5")
                     .arg(fed["local_status"].toString(tr("UNAVAILABLE")),
                          fed["freshness"].toObject()["status"].toString(tr("UNAVAILABLE")),
                          poly["mapping_status"].toString(tr("UNAVAILABLE / UNVERIFIED")),
                          poly["data_status"].toString(tr("UNAVAILABLE")),
                          poly["freshness"].toObject()["status"].toString(tr("UNAVAILABLE")));
    }
    if (analytics_.isEmpty())
        state << tr("Historical analytics unavailable or loading; no values inferred.");
    else
        state << tr("Stored analytics comparison quality: %1 (historical context; current values use the refreshed "
                    "distribution)")
                     .arg(analytics_["difference"].toObject()["current_state"].toString());
    status_->setText(state.join("\n"));
    source_status_->setText(state.join("\n"));
    QStringList concise;
    if (resolved())
        concise << tr("Resolved · local history");
    else {
        const auto fed_state = fed["freshness"].toObject()["status"].toString();
        QString fed_label = tr("unavailable");
        if (current_ok_ && fed["local_status"].toString() == "OK") {
            if (fed_state == "SOURCE_TIMESTAMP_UNAVAILABLE")
                fed_label = tr("retrieved · source time unavailable");
            else if (fed_state == "STALE")
                fed_label = tr("stale");
            else if (fed_state == "CURRENT" || fed_state == "OK")
                fed_label = tr("current");
            else
                fed_label = tr("retrieved · freshness unavailable");
        }
        concise << tr("Investing %1").arg(fed_label);
        const auto mapping = poly["mapping_status"].toString();
        const QString mapping_label = mapping == "VALIDATED"   ? tr("verified")
                                      : mapping == "AMBIGUOUS" ? tr("ambiguous")
                                      : mapping == "NOT_FOUND" ? tr("not found")
                                                               : tr("unverified");
        const auto data_status = poly["data_status"].toString();
        const QString data_label = data_status == "CURRENT"   ? tr("current")
                                   : data_status == "STALE"   ? tr("stale")
                                   : data_status == "PARTIAL" ? tr("partial")
                                                              : tr("unavailable");
        concise << tr("Polymarket %1 · mapping %2").arg(data_label, mapping_label);
    }
    if (!history_error_.isEmpty() || !backfill_error_.isEmpty())
        concise << tr("History incomplete · see Sources");
    if (!current_error_.isEmpty())
        concise << tr("Provider issue · see Sources");
    if (backfill_in_flight_)
        concise << tr("Loading Polymarket history…");
    status_->setText(concise.join("  |  "));
    status_->setToolTip(state.join("\n"));
    status_->setAccessibleDescription(!backfill_in_flight_ && !analytics_.isEmpty() ? tr("Historical data loaded")
                                      : !history_error_.isEmpty()                   ? tr("Historical data unavailable")
                                                                                    : tr("Loading historical data"));
    QMap<QString, QJsonObject> rows;
    if (show_current) {
        for (const auto& value : fed["local_probabilities"].toArray()) {
            auto row = value.toObject();
            row["fed_probability_pct"] = row["probability_pct"];
            rows[key(row["outcome_bp"].toInt(), false)] = row;
        }
        if (poly["mapping_status"].toString() == "VALIDATED")
            for (const auto& value : poly["outcomes"].toArray()) {
                const auto row = value.toObject();
                auto& entry = rows[key(row["outcome_bp"].toInt(), row["open_ended"].toBool())];
                entry["outcome_bp"] = row["outcome_bp"];
                entry["open_ended"] = row["open_ended"];
                entry["polymarket_probability_pct"] = row["probability_pct"];
            }
        for (const auto& value : current["comparison"].toArray()) {
            const auto row = value.toObject();
            rows[key(row["outcome_bp"].toInt(), row["open_ended"].toBool())] = row;
        }
    }
    distribution_->setRowCount(rows.size());
    int r = 0;
    for (const auto& row : rows) {
        cell(distribution_, r, 0, fedwatch::outcome_label(row["outcome_bp"].toInt(), row["open_ended"].toBool()));
        cell(distribution_, r, 1, fedwatch::number(row["fed_probability_pct"]));
        cell(distribution_, r, 2, fedwatch::number(row["polymarket_probability_pct"]));
        cell(distribution_, r, 3, fedwatch::number(row["probability_diff_pp"]));
        cell(distribution_, r++, 4,
             tr("Fed %1; Poly %2")
                 .arg(fed["freshness"].toObject()["status"].toString(tr("UNAVAILABLE")),
                      poly["data_status"].toString(tr("UNAVAILABLE"))));
    }
    if (rows.isEmpty()) {
        distribution_->setRowCount(1);
        cell(distribution_, 0, 0,
             resolved() ? tr("Resolved meeting: inspect stored history below")
                        : tr("No current distribution available"));
    }
    const auto selected = rows.value(outcomes_->currentData().toString());
    if (outcomes_->currentIndex() >= 0) {
        selected_current_->setText(tr("%1    |    Fed-side %2    |    Polymarket %3    |    Difference %4")
                                       .arg(outcomes_->currentText(),
                                            fedwatch::number(selected["fed_probability_pct"], "%"),
                                            fedwatch::number(selected["polymarket_probability_pct"], "%"),
                                            fedwatch::number(selected["probability_diff_pp"], " pp")));
        for (int i = 0; i < distribution_->rowCount(); ++i)
            if (distribution_->item(i, 0) && distribution_->item(i, 0)->text() == outcomes_->currentText()) {
                distribution_->selectRow(i);
                break;
            }
    } else
        selected_current_->setText(tr("Select a meeting and outcome to explore probability history."));
    const auto f = analytics_["fed_side"].toObject();
    const auto p = analytics_["polymarket"].toObject();
    const QStringList captions{tr("Latest stored probability (%)"),
                               tr("Previous observation (pp)"),
                               tr("1D (pp)"),
                               tr("7D (pp)"),
                               tr("30D (pp)"),
                               tr("Since first observation (pp)"),
                               tr("Observed high (%)"),
                               tr("Observed low (%)"),
                               tr("Range position (0–1)"),
                               tr("Percentile rank (0–1)")};
    indicators_->setRowCount(captions.size());
    auto indicator = [](const QJsonObject& side, int row) {
        QJsonObject entry;
        if (row == 0)
            return fedwatch::number(side["latest"].toObject()["probability_pct"]) + " · " +
                   side["latest"].toObject()["quality_status"].toString(side["state"].toString("NO_OBSERVATIONS"));
        if (row == 1)
            entry = side["latest_change_from_previous_observation"].toObject();
        if (row >= 2 && row <= 4)
            entry = side["changes"].toObject()[QStringList{"1d", "7d", "30d"}[row - 2]].toObject();
        if (row == 5)
            entry = side["change_since_first_observation"].toObject();
        if (row <= 5)
            return entry["change_pp"].isDouble() ? fedwatch::number(entry["change_pp"])
                                                 : entry["state"].toString("INSUFFICIENT_HISTORY");
        if (row == 6 || row == 7)
            return fedwatch::number(side[row == 6 ? "observed_high" : "observed_low"].toObject()["probability_pct"]);
        if (row == 8)
            return side["range_position"].isDouble() ? fedwatch::number(side["range_position"])
                                                     : side["range_position_state"].toString("NO_OBSERVATIONS");
        return fedwatch::number(side["percentile_rank"]);
    };
    for (int i = 0; i < captions.size(); ++i) {
        cell(indicators_, i, 0, captions[i]);
        cell(indicators_, i, 1, indicator(f, i));
        cell(indicators_, i, 2, indicator(p, i));
    }
    details_->setPlainText(QString::fromUtf8(QJsonDocument(QJsonObject{{"current_snapshot", snapshot_},
                                                                       {"stored_meeting", stored},
                                                                       {"stored_observations", series_},
                                                                       {"analytics", analytics_},
                                                                       {"polymarket_history_update", backfill_result_}})
                                                 .toJson(QJsonDocument::Indented)));
    render_history();
    sync_chips();
    show_content_page(workspace_page_);
    const bool idle = !collect_in_flight_ && !backfill_in_flight_;
    fetch_btn_->setEnabled(idle);
    update_upcoming_->setEnabled(idle);
    load_history_->setEnabled(idle && !resolved() && !selected_meeting_.isEmpty());
}
void FedWatchPanel::render_history() {
    const auto fed = analytics_["fed_side"].toObject();
    const auto poly = analytics_["polymarket"].toObject();
    QVector<fedwatch::Series> probabilities{
        {fed["method"].toString(methods_->currentData().toString()),
         fedwatch::points(fed["history"].toArray(), "observed_at", "probability_pct")},
        {poly["method"].toString("POLYMARKET_CLOB"),
         fedwatch::points(poly["history"].toArray(), "observed_at", "probability_pct")}};
    QVector<fedwatch::Series> differences{
        {"Polymarket - " + methods_->currentData().toString() + " (pp)",
         fedwatch::points(analytics_["difference"].toObject()["history"].toArray(), "date", "probability_diff_pp")}};
    QDateTime anchor;
    for (const auto& group : {probabilities, differences})
        for (const auto& series : group)
            for (const auto& point : series.points)
                if (!anchor.isValid() || point.instant > anchor)
                    anchor = point.instant;
    probability_->set_series(fedwatch::filter_range(probabilities, ranges_->currentData().toInt(), anchor));
    divergence_->set_series(fedwatch::filter_range(differences, ranges_->currentData().toInt(), anchor));
    const QString full_coverage =
        tr("History method: %1 · Polymarket: %2\nFed coverage: %3 → %4 · Polymarket coverage: %5 → "
           "%6\nBoth charts share latest retained observation anchor %8. Missing periods are absent; no lines "
           "interpolate gaps. Backend time basis: %7.")
            .arg(methods_->currentData().toString(), poly["method"].toString("POLYMARKET_CLOB"),
                 fed["first_observed_at"].toString(tr("Unavailable")),
                 fed["last_observed_at"].toString(tr("Unavailable")),
                 poly["first_observed_at"].toString(tr("Unavailable")),
                 poly["last_observed_at"].toString(tr("Unavailable")), analytics_["time_basis"].toString("UTC"),
                 anchor.isValid() ? anchor.toString(Qt::ISODate) : tr("Unavailable"));
    coverage_->setToolTip(full_coverage);
    coverage_->setText(
        tr("%1 · %2 · Shared range ends %3 (UTC)\n%4 Fed-side / %5 Polymarket observations in this range%6")
            .arg(fedwatch::source_label(methods_->currentData().toString()), ranges_->currentText(),
                 anchor.isValid() ? anchor.toString("yyyy-MM-dd HH:mm") : tr("Unavailable"))
            .arg(fedwatch::filter_range(probabilities, ranges_->currentData().toInt(), anchor)[0].points.size())
            .arg(fedwatch::filter_range(probabilities, ranges_->currentData().toInt(), anchor)[1].points.size())
            .arg(poly["history"].toArray().size() <= 1 && !resolved()
                     ? tr(" · Use Load history for available Polymarket history")
                     : QString{}));
}
QVariantMap FedWatchPanel::save_panel_state() const {
    const QString selection = !restored_outcome_.isEmpty()   ? restored_outcome_
                              : !selected_outcome_.isEmpty() ? selected_outcome_
                                                             : outcomes_->currentData().toString();
    QVariantMap state{{"meeting", selected_meeting_},
                      {"fed_method", methods_->currentData()},
                      {"range_days", ranges_->currentData()},
                      {"details_visible", !details_->isHidden()}};
    if (!selection.isEmpty()) {
        state["outcome_bp"] = selection.section(':', 0, 0).toInt();
        state["open_ended"] = selection.endsWith(":tail");
    }
    return state;
}
void FedWatchPanel::restore_panel_state(const QVariantMap& state) {
    ++generation_;
    selected_meeting_ = state.value("meeting").toString();
    meeting_explicitly_selected_ = !selected_meeting_.isEmpty();
    if (state.contains("outcome_bp")) {
        restored_outcome_ = key(state.value("outcome_bp").toInt(), state.value("open_ended").toBool());
        selected_outcome_ = restored_outcome_;
    }
    {
        QSignalBlocker blocker(methods_);
        int i = methods_->findData(state.value("fed_method"));
        if (i >= 0)
            methods_->setCurrentIndex(i);
    }
    {
        QSignalBlocker blocker(ranges_);
        int i = ranges_->findData(state.value("range_days"));
        if (i >= 0)
            ranges_->setCurrentIndex(i);
    }
    if (auto* toggle = findChild<QToolButton*>("fedwatchDetailsToggle"))
        toggle->setChecked(state.value("details_visible").toBool());
    if (activated_)
        on_fetch();
    else
        render();
}
void FedWatchPanel::refresh_panel_theme() {
    EconPanelBase::refresh_panel_theme();
    setStyleSheet(
        panel_style() +
        QString(
            "QLabel { color:%1; font-size:11px; } QTabWidget::pane { border:1px solid %2; } QTabBar::tab { padding:6px "
            "16px; color:%1; background:%3; } QTabBar::tab:selected { color:%4; background:%5; } "
            "QPushButton#fedwatchUpdateUpcoming, QPushButton#fedwatchLoadHistory { padding:5px 10px; color:%1; "
            "background:%3; border:1px solid %2; }"
            "QLabel#fedwatchProbabilityTitle, QLabel#fedwatchDivergenceTitle { color:%4; font-size:12px; "
            "font-weight:600; }"
            "QPushButton[selection_kind] { padding:6px 12px; border-radius:6px; border:1px solid %2; background:%3; "
            "color:%1; font-size:11px; }"
            "QPushButton[selection_kind]:hover { border-color:#3B82F6; color:%4; }"
            "QPushButton[selection_kind]:checked { background:#1D4ED8; border-color:#3B82F6; color:white; }"
            "QToolButton#fedwatchEarlierMeetings, QToolButton#fedwatchLaterMeetings { padding:4px 8px; color:%4; "
            "background:%3; border:1px solid %2; font-size:20px; }")
            .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), ui::colors::BG_SURFACE(),
                 ui::colors::TEXT_PRIMARY(), ui::colors::BG_RAISED()));
    if (probability_)
        probability_->update();
    if (divergence_)
        divergence_->update();
}
void FedWatchPanel::arrange_charts() {
    if (!charts_layout_ || !probability_section_ || !divergence_section_)
        return;
    charts_layout_->removeWidget(probability_section_);
    charts_layout_->removeWidget(divergence_section_);
    const bool wide = width() >= 900;
    charts_layout_->addWidget(probability_section_, 0, 0);
    charts_layout_->addWidget(divergence_section_, wide ? 0 : 1, wide ? 1 : 0);
    charts_layout_->setColumnStretch(0, 1);
    charts_layout_->setColumnStretch(1, wide ? 1 : 0);
}
void FedWatchPanel::resizeEvent(QResizeEvent* event) {
    EconPanelBase::resizeEvent(event);
    arrange_charts();
}
} // namespace fincept::screens
