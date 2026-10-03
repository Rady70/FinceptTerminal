#include "screens/economics/panels/FedWatchPanel.h"

#include "core/config/ProfileManager.h"
#include "core/session/ScreenStateManager.h"
#include "screens/common/IStatefulScreen.h"
#include "screens/economics/panels/FedWatchCurrentChart.h"
#include "screens/economics/panels/FedWatchHistoryChart.h"
#include "services/economics/EconomicsEnvelopeParse.h"
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
QList<QJsonObject> ordered_outcomes(const QMap<QString, QJsonObject>& rows) {
    auto values = rows.values();
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
        const int left = a["outcome_bp"].toInt(), right = b["outcome_bp"].toInt();
        return left != right ? left < right : a["open_ended"].toBool() < b["open_ended"].toBool();
    });
    return values;
}
QString history_issue(const services::EconomicsResult& result, bool usable, const QString& operation,
                      bool report_warnings = true) {
    QStringList issues;
    const auto data = payload(result.data);
    const QString provider_errors = errors(data);
    if (!provider_errors.isEmpty())
        issues << provider_errors;
    if (!result.success && !result.error.isEmpty() && !issues.contains(result.error))
        issues << result.error;
    const auto decision = services::economics_detail::classify(result.data);
    if (decision.partial && !issues.contains(decision.error))
        issues << decision.error;
    if (report_warnings)
        for (const auto& warning : data["warnings"].toArray())
            issues << QObject::tr("Warning: ") + warning.toString();
    if (!usable && issues.isEmpty())
        issues << QObject::tr("No usable response");
    return issues.isEmpty() ? QString{}
                            : operation + (usable ? ": " : QObject::tr(" unavailable: ")) + issues.join("\n");
}
} // namespace
FedWatchPanel::FedWatchPanel(QWidget* parent) : FedWatchPanel(Dispatch{}, parent) {}
FedWatchPanel::FedWatchPanel(Dispatch dispatch, QWidget* parent)
    : EconPanelBase("fedwatch", ui::colors::AMBER(), parent), dispatch_(std::move(dispatch)) {
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
    summary_ = new QLabel(this);
    summary_->setObjectName("fedwatchSummary");
    summary_->setWordWrap(true);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* toolbar = findChild<QWidget*>("econToolbar");
    toolbar->setFixedHeight(60);
    auto* header_layout = static_cast<QHBoxLayout*>(toolbar->layout());
    delete header_layout->takeAt(0); // Replace the base toolbar spacer with the compact summary.
    header_layout->insertWidget(0, summary_, 1);
    previous_meeting_ = new QToolButton(toolbar);
    next_meeting_ = new QToolButton(toolbar);
    previous_meeting_->setObjectName("fedwatchPreviousMeeting");
    next_meeting_->setObjectName("fedwatchNextMeeting");
    previous_meeting_->setText("‹");
    next_meeting_->setText("›");
    previous_meeting_->setAccessibleName(tr("Select previous FOMC meeting"));
    next_meeting_->setAccessibleName(tr("Select next FOMC meeting"));
    header_layout->insertWidget(0, previous_meeting_);
    header_layout->insertWidget(2, next_meeting_);
    connect(previous_meeting_, &QToolButton::clicked, this,
            [this] { meetings_->setCurrentIndex(meetings_->currentIndex() - 1); });
    connect(next_meeting_, &QToolButton::clicked, this,
            [this] { meetings_->setCurrentIndex(meetings_->currentIndex() + 1); });
    layout->addWidget(controls_);
    status_ = new QLabel(content);
    status_->setObjectName("fedwatchStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* charts = new QWidget(content);
    charts_layout_ = new QGridLayout(charts);
    charts_layout_->setContentsMargins(0, 0, 0, 0);
    charts_layout_->setSpacing(10);
    layout->addWidget(charts);
    auto chart_section = [&](const char* name, const QString& title) {
        auto* section = new QWidget(charts);
        auto* section_layout = new QVBoxLayout(section);
        section_layout->setContentsMargins(0, 0, 0, 0);
        auto* heading = new QLabel(title, section);
        heading->setObjectName(name);
        section_layout->addWidget(heading);
        return section;
    };
    probability_section_ = chart_section("fedwatchProbabilityTitle", tr("Fed-side history (%)"));
    fed_history_state_ = new QLabel(probability_section_);
    fed_history_state_->setObjectName("fedwatchFedHistoryState");
    fed_history_state_->setWordWrap(true);
    probability_section_->layout()->addWidget(fed_history_state_);
    probability_ = new FedWatchHistoryChart(probability_section_);
    probability_->setObjectName("fedwatchProbabilityChart");
    probability_->setMaximumHeight(340);
    probability_section_->layout()->addWidget(probability_);
    polymarket_section_ = chart_section("fedwatchPolymarketTitle", tr("Polymarket history (%)"));
    poly_history_state_ = new QLabel(polymarket_section_);
    poly_history_state_->setObjectName("fedwatchPolymarketHistoryState");
    poly_history_state_->setWordWrap(true);
    polymarket_section_->layout()->addWidget(poly_history_state_);
    polymarket_ = new FedWatchHistoryChart(polymarket_section_);
    polymarket_->setObjectName("fedwatchPolymarketChart");
    polymarket_->setMaximumHeight(340);
    polymarket_section_->layout()->addWidget(polymarket_);
    contextual_load_history_ = new QPushButton(tr("Load history"), polymarket_section_);
    contextual_load_history_->setObjectName("fedwatchContextLoadHistory");
    contextual_load_history_->setAccessibleName(tr("Load Polymarket history"));
    contextual_load_history_->setToolTip(load_history_->toolTip());
    polymarket_section_->layout()->addWidget(contextual_load_history_);
    static_cast<QVBoxLayout*>(probability_section_->layout())->addStretch();
    static_cast<QVBoxLayout*>(polymarket_section_->layout())->addStretch();
    connect(contextual_load_history_, &QPushButton::clicked, load_history_, &QPushButton::click);
    for (auto* source : {probability_, polymarket_}) {
        auto* peer = source == probability_ ? polymarket_ : probability_;
        connect(source, &FedWatchHistoryChart::date_hovered, peer, &FedWatchHistoryChart::set_hover_date);
        connect(source, &FedWatchHistoryChart::hover_finished, peer, [peer] { peer->set_hover_date({}); });
    }
    arrange_charts();
    selected_current_ = new QLabel(content);
    selected_current_->setObjectName("fedwatchSelectedCurrent");
    selected_current_->setWordWrap(true);
    layout->addWidget(selected_current_);
    auto* difference_toggle = new QToolButton(content);
    difference_toggle->setObjectName("fedwatchDifferenceToggle");
    difference_toggle->setText(tr("Show historical difference"));
    difference_toggle->setCheckable(true);
    layout->addWidget(difference_toggle);
    divergence_ = new FedWatchHistoryChart(content);
    divergence_->set_probability_scale(false);
    divergence_->setObjectName("fedwatchDivergenceChart");
    divergence_->setMinimumHeight(160);
    divergence_->hide();
    layout->addWidget(divergence_);
    connect(difference_toggle, &QToolButton::toggled, this, [this, difference_toggle](bool expanded) {
        divergence_->setVisible(expanded && !divergence_->series().isEmpty() &&
                                !divergence_->series()[0].points.isEmpty());
        difference_toggle->setText(expanded ? tr("Hide historical difference") : tr("Show historical difference"));
    });
    auto table = [&](QVBoxLayout* target, const char* name, const QStringList& headings) {
        auto* value = new QTableWidget(content);
        value->setObjectName(name);
        value->setColumnCount(headings.size());
        value->setHorizontalHeaderLabels(headings);
        value->setEditTriggers(QAbstractItemView::NoEditTriggers);
        value->setAlternatingRowColors(true);
        value->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        value->horizontalHeader()->setStretchLastSection(true);
        value->verticalHeader()->hide();
        value->setMinimumHeight(170);
        target->addWidget(value);
        return value;
    };
    auto* distribution_title = new QLabel(tr("CURRENT EXPECTATIONS"), content);
    distribution_title->setObjectName("fedwatchDistributionTitle");
    layout->addWidget(distribution_title);
    current_chart_ = new FedWatchCurrentChart(content);
    current_chart_->setObjectName("fedwatchCurrentChart");
    connect(current_chart_, &FedWatchCurrentChart::outcome_selected, this, [this](int bp, bool open) {
        const int index = outcomes_->findData(key(bp, open));
        if (index < 0)
            return;
        if (index == outcomes_->currentIndex())
            outcomes_->changed();
        else
            outcomes_->setCurrentIndex(index);
        sync_chips();
    });
    layout->addWidget(current_chart_);
    distribution_ = table(layout, "fedwatchDistribution",
                          {tr("Outcome"), tr("Fed-side %"), tr("Polymarket %"), tr("Difference pp")});
    distribution_->setMaximumHeight(190);
    auto* toggle = new QToolButton(content);
    toggle->setObjectName("fedwatchDetailsToggle");
    toggle->setText(tr("Research details"));
    toggle->setAccessibleName(tr("Research details"));
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(Qt::RightArrow);
    toggle->setCheckable(true);
    layout->addWidget(toggle);
    diagnostics_ = new QWidget(content);
    diagnostics_->setObjectName("fedwatchDiagnostics");
    auto* sources_layout = new QVBoxLayout(diagnostics_);
    sources_layout->setContentsMargins(0, 0, 0, 0);
    sources_layout->addWidget(new QLabel(tr("History source"), diagnostics_));
    sources_layout->addWidget(make_chip_row("method", methods_, 40));
    auto* history_actions = new QHBoxLayout;
    history_actions->addWidget(load_history_);
    history_actions->addWidget(update_upcoming_);
    history_actions->addStretch();
    sources_layout->addLayout(history_actions);
    diagnostic_status_ = new QLabel(diagnostics_);
    diagnostic_status_->setObjectName("fedwatchDiagnosticStatus");
    diagnostic_status_->setWordWrap(true);
    sources_layout->addWidget(diagnostic_status_);
    coverage_ = new QLabel(diagnostics_);
    coverage_->setObjectName("fedwatchCoverage");
    coverage_->setWordWrap(true);
    sources_layout->addWidget(coverage_);
    auto* indicators_title = new QLabel(tr("Historical probability changes (pp)"), diagnostics_);
    indicators_title->setObjectName("fedwatchIndicatorTitle");
    sources_layout->addWidget(indicators_title);
    indicators_ = table(sources_layout, "fedwatchIndicators", {tr("Measure"), tr("Fed-side"), tr("Polymarket")});
    source_status_ = new QLabel(diagnostics_);
    source_status_->setObjectName("fedwatchSourceStatus");
    source_status_->setWordWrap(true);
    source_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sources_layout->addWidget(source_status_);
    details_ = new QPlainTextEdit(diagnostics_);
    details_->setObjectName("fedwatchDetails");
    details_->setReadOnly(true);
    details_->setMinimumHeight(280);
    sources_layout->addWidget(details_);
    diagnostics_->hide();
    layout->addWidget(diagnostics_);
    connect(toggle, &QToolButton::toggled, this, [this, toggle](bool expanded) {
        diagnostics_->setVisible(expanded);
        toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        if (expanded)
            ensure_selected_controls();
    });
    connect(toggle, &QToolButton::toggled, this, [this] { notify_state(this); });
    connect(toggle, &QToolButton::toggled, this, [this] { render_history(); });
    auto* research = new QLabel(diagnostics_);
    research->setObjectName("fedwatchResearchNotice");
    research->setWordWrap(true);
    research->setText(tr("Read-only research. Local Fed-side distributions may be narrower than Polymarket tails. Live "
                         "Investing-derived and historical ZQ-reconstructed probabilities are separate methods. Gaps "
                         "remain missing; comparison is descriptive."));
    sources_layout->addWidget(research);
    auto* timestamp_note =
        new QLabel(tr("Countdown is unavailable from the backend. Investing publishes no source timestamp; retrieval "
                      "time is used. Source errors and raw observations are retained below."),
                   diagnostics_);
    timestamp_note->setWordWrap(true);
    sources_layout->addWidget(timestamp_note);
    // The default workspace presents usable observations. Full choices, exact
    // values, timestamps and quality evidence remain auditable in Research details.
    for (auto* widget : {controls_, static_cast<QWidget*>(selected_current_), static_cast<QWidget*>(distribution_)})
        layout->removeWidget(widget);
    sources_layout->insertWidget(0, controls_);
    sources_layout->insertWidget(1, selected_current_);
    sources_layout->insertWidget(2, distribution_);
    sources_layout->insertWidget(3, fed_history_state_);
    sources_layout->insertWidget(4, poly_history_state_);
    controls_layout_->removeWidget(range_control_);
    range_control_->setParent(content);
    range_control_->setFixedHeight(48);
    status_->setMaximumHeight(48);
    history_controls_ = new QWidget(content);
    history_controls_->setObjectName("fedwatchHistoryControls");
    history_controls_->setFixedHeight(48);
    auto* history_row = new QHBoxLayout(history_controls_);
    history_row->setContentsMargins(0, 0, 0, 0);
    layout->removeWidget(status_);
    history_row->addWidget(status_, 1);
    history_row->addWidget(range_control_, 1);
    layout->insertWidget(0, history_controls_);
    layout->removeWidget(distribution_title);
    layout->removeWidget(current_chart_);
    layout->insertWidget(0, current_chart_);
    layout->insertWidget(0, distribution_title);
    compact_coverage_ = new QLabel(content);
    compact_coverage_->setObjectName("fedwatchCompactCoverage");
    compact_coverage_->setWordWrap(true);
    layout->insertWidget(2, compact_coverage_);
    polymarket_section_->layout()->removeWidget(contextual_load_history_);
    contextual_load_history_->setParent(content);
    contextual_load_history_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    layout->insertWidget(layout->indexOf(charts) + 1, contextual_load_history_);
    arrange_controls();
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
    Q_UNUSED(toolbar);
    update_upcoming_ = new QPushButton(tr("Update upcoming meetings"), this);
    update_upcoming_->setObjectName("fedwatchUpdateUpcoming");
    update_upcoming_->setAccessibleName(tr("Update upcoming meetings"));
    update_upcoming_->setToolTip(tr("Collect current upcoming observations; resolved meeting history stays local."));
    connect(update_upcoming_, &QPushButton::clicked, this, [this] {
        if (collect_in_flight_ || backfill_in_flight_)
            return;
        current_ok_ = false;
        current_error_.clear();
        render();
        diagnostic_status_->setText(tr("Updating upcoming meetings from current providers…"));
        request("collect");
    });
    load_history_ = new QPushButton(tr("Load history"), this);
    load_history_->setObjectName("fedwatchLoadHistory");
    load_history_->setAccessibleName(tr("Load Polymarket history"));
    load_history_->setToolTip(tr("Download available Polymarket history for the selected meeting. "
                                 "Resolved Refresh reads local data. "
                                 "Fed-side history remains the locally retained archive."));
    connect(load_history_, &QPushButton::clicked, this, [this] {
        if (collect_in_flight_ || backfill_in_flight_ || selected_meeting_.isEmpty() || !load_history_->isEnabled())
            return;
        backfill_results_.remove(selected_meeting_);
        request("history_backfill", {"--meeting", selected_meeting_});
        render();
        diagnostic_status_->setText(tr("Loading Polymarket historical observations…"));
        status_->setAccessibleDescription(tr("Loading historical data"));
    });
    controls_ = new QWidget(this);
    controls_->setObjectName("fedwatchControlRow");
    controls_layout_ = new QGridLayout(controls_);
    controls_layout_->setContentsMargins(0, 0, 0, 0);
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
    auto group = [this](const QString& kind, const QString& label, Selection* selection) {
        auto* widget = new QWidget(controls_);
        auto* row = new QHBoxLayout(widget);
        row->setContentsMargins(0, 0, 0, 0);
        auto* caption = new QLabel(label, widget);
        caption->setObjectName("fedwatch" + kind.left(1).toUpper() + kind.mid(1) + "Label");
        row->addWidget(caption);
        row->addWidget(make_chip_row(kind, selection, 48), 1);
        return widget;
    };
    meeting_control_ = group("meeting", tr("Meeting"), meetings_);
    outcome_control_ = group("outcome", tr("Outcome"), outcomes_);
    range_control_ = group("range", tr("Range"), ranges_);
    arrange_controls();
    meetings_->changed = [this] {
        if (selected_meeting_ != meetings_->currentData().toString()) {
            series_error_.clear();
            analytics_error_.clear();
        }
        selected_meeting_ = meetings_->currentData().toString();
        meeting_explicitly_selected_ = !selected_meeting_.isEmpty();
        ++generation_;
        analytics_ = {};
        series_ = {};
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
            outcome_explicitly_selected_ = true;
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
                text = text.section(" · ", 0, 0);
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
            button->setToolTip(model->itemText(index));
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
        if (command == "history_backfill")
            backfill_meeting_ = args.value(args.indexOf("--meeting") + 1);
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
    const int meeting_option = args.indexOf("--meeting");
    pending_[id] = {command, generation_, meeting_option >= 0 ? args.value(meeting_option + 1) : QString{}};
    emit command_requested(command, durable_args, id);
    if (dispatch_)
        dispatch_(command, durable_args, id);
    else
        services::EconomicsService::instance().execute("fedwatch", "fedwatch_data.py", command, durable_args, id, true);
}
void FedWatchPanel::activate() {
    if (!activated_) {
        activated_ = true;
        load_local();
    }
}
void FedWatchPanel::load_local() {
    if (collect_in_flight_ || backfill_in_flight_)
        return;
    ++generation_;
    current_error_.clear();
    current_ok_ = false;
    analytics_ = {};
    render();
    status_->setText(tr("Loading meeting inventory from local history…"));
    request("history_meetings");
}
void FedWatchPanel::on_fetch() {
    if (collect_in_flight_ || backfill_in_flight_)
        return;
    if (!selected_meeting_.isEmpty() && resolved()) {
        load_local();
        return;
    }
    current_ok_ = false;
    current_error_.clear();
    render();
    diagnostic_status_->setText(meeting()["status"].toString() == "PENDING"
                                    ? tr("Retrying FRED meeting resolution…")
                                    : tr("Refreshing selected current observations…"));
    request("collect", selected_meeting_.isEmpty() ? QStringList{} : QStringList{"--meeting", selected_meeting_});
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
bool FedWatchPanel::local_only() const {
    const auto status = meeting()["status"].toString();
    return status == "RESOLVED" || status == "PENDING";
}
void FedWatchPanel::rebuild_meetings() {
    const QString previous = selected_meeting_;
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
    if (selected_meeting_ != previous) {
        series_error_.clear();
        analytics_error_.clear();
    }
}
void FedWatchPanel::load_meeting() {
    analytics_ = {};
    series_ = {};
    rebuild_outcomes();
    render();
    status_->setText(tr("Loading retained current observations from local storage…"));
    request("local_snapshot",
            selected_meeting_.isEmpty() ? QStringList{} : QStringList{"--meeting", selected_meeting_});
}
void FedWatchPanel::rebuild_outcomes() {
    const QString previous = !restored_outcome_.isEmpty()   ? restored_outcome_
                             : outcome_explicitly_selected_ ? selected_outcome_
                                                            : QString{};
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
    for (const auto& row : ordered_outcomes(available))
        outcomes_->addItem(fedwatch::outcome_label(row["outcome_bp"].toInt(), row["open_ended"].toBool()),
                           key(row["outcome_bp"].toInt(), row["open_ended"].toBool()));
    int index = outcomes_->findData(previous);
    const bool prior_choice_available = index >= 0;
    if (index < 0) {
        if (resolved()) {
            const auto actual = meeting()["actual_outcome_bp"];
            if (actual.isDouble())
                index = outcomes_->findData(key(actual.toInt(), false));
            if (index < 0 && outcomes_->count() > 0)
                index = 0;
        } else {
            index = outcomes_->findData(key(0, false));
            if (index < 0)
                for (const auto& row : current_outcome_rows())
                    if (FedWatchCurrentChart::has_current_value(row)) {
                        index = outcomes_->findData(key(row["outcome_bp"].toInt(), row["open_ended"].toBool()));
                        if (index >= 0)
                            break;
                    }
        }
    }
    outcomes_->setCurrentIndex(index);
    if (!outcomes_->currentData().toString().isEmpty() && (prior_choice_available || !outcome_explicitly_selected_))
        selected_outcome_ = outcomes_->currentData().toString();
    if (prior_choice_available)
        restored_outcome_.clear();
}
QList<QJsonObject> FedWatchPanel::current_outcome_rows() const {
    const auto current = current_meeting();
    const auto fed = current["fed_side"].toObject();
    const auto poly = current["polymarket"].toObject();
    const bool show_current = current_ok_ && !local_only();
    QMap<QString, QJsonObject> rows;
    const auto fed_freshness = fed["freshness"].toObject()["status"].toString();
    const bool fed_current =
        show_current && fed["local_status"].toString() == "OK" &&
        (fed_freshness == "CURRENT" || fed_freshness == "OK" || fed_freshness == "SOURCE_TIMESTAMP_UNAVAILABLE");
    const bool poly_current =
        show_current && poly["mapping_status"].toString() == "VALIDATED" && poly["data_status"].toString() == "CURRENT";
    if (show_current) {
        for (const auto& value : fed["local_probabilities"].toArray()) {
            auto row = value.toObject();
            if (fed_current)
                row["fed_probability_pct"] = row["probability_pct"];
            rows[key(row["outcome_bp"].toInt(), false)] = row;
        }
        if (poly["mapping_status"].toString() == "VALIDATED")
            for (const auto& value : poly["outcomes"].toArray()) {
                const auto row = value.toObject();
                auto& entry = rows[key(row["outcome_bp"].toInt(), row["open_ended"].toBool())];
                entry["outcome_bp"] = row["outcome_bp"];
                entry["open_ended"] = row["open_ended"];
                if (poly_current)
                    entry["polymarket_probability_pct"] = row["probability_pct"];
            }
        for (const auto& value : current["comparison"].toArray()) {
            auto row = value.toObject();
            if (!fed_current)
                row.remove("fed_probability_pct");
            if (!poly_current)
                row.remove("polymarket_probability_pct");
            if (!fed_current || !poly_current)
                row.remove("probability_diff_pp");
            rows[key(row["outcome_bp"].toInt(), row["open_ended"].toBool())] = row;
        }
    }
    return ordered_outcomes(rows);
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
        backfill_results_[pending.meeting] = {data, history_issue(result, usable, tr("History update"), false)};
        request("history_meetings");
        render();
        return;
    }
    if (pending.command == "collect" || pending.command == "local_snapshot") {
        if (pending.command == "collect")
            collect_in_flight_ = false;
        current_ok_ = usable;
        current_error_ = errors(data);
        if (!result.success && current_error_.isEmpty())
            current_error_ = result.error;
        snapshot_ = usable ? data : QJsonObject{};
        if (pending.command == "collect") {
            request("history_meetings");
        } else {
            rebuild_meetings();
            rebuild_outcomes();
            if (!selected_meeting_.isEmpty())
                request("history_series", {"--meeting", selected_meeting_});
            else
                status_->setText(tr("No retained meetings. Press Refresh or Update upcoming meetings to acquire "
                                    "upcoming observations."));
        }
        render();
        return;
    }
    if (pending.command == "history_meetings") {
        inventory_error_ = history_issue(result, usable, tr("Meeting history"));
        if (usable) {
            overview_ = data;
            rebuild_meetings();
            sync_chips(); // Stored meetings remain selectable while current providers are being collected.
        }
        load_meeting();
        return;
    }
    if (pending.command == "history_series") {
        series_ = usable ? data : QJsonObject{};
        series_error_ = history_issue(result, usable, tr("Stored series"));
        rebuild_outcomes();
        load_analytics();
        return;
    }
    if (pending.command == "history_analytics") {
        analytics_ = usable ? data : QJsonObject{};
        analytics_error_ = history_issue(result, usable, tr("Analytics"));
        render();
    }
}
void FedWatchPanel::render() {
    const QStringList context{selected_meeting_, outcomes_->currentData().toString(),
                              methods_->currentData().toString()};
    if (analytics_error_context_ != context) {
        analytics_error_.clear();
        analytics_error_context_ = context;
    }
    const auto target = snapshot_["current_target_range"].toObject();
    const auto stored = meeting();
    const auto current = current_meeting();
    const auto fed = current["fed_side"].toObject();
    const auto poly = current["polymarket"].toObject();
    const bool show_current = current_ok_ && !local_only();
    if (resolved())
        summary_->setText(tr("%1 · RESOLVED%2\nStored history · Refresh reads local data")
                              .arg(selected_meeting_,
                                   stored["actual_outcome_bp"].isDouble()
                                       ? tr(" · Actual policy outcome: %1 bp").arg(stored["actual_outcome_bp"].toInt())
                                       : QString{}));
    else {
        QString header = selected_meeting_.isEmpty() ? tr("FOMC · select a meeting") : selected_meeting_ + tr(" FOMC");
        if (show_current && target["lower"].isDouble() && target["upper"].isDouble())
            header += tr("\nTarget %1–%2")
                          .arg(fedwatch::number(target["lower"], "%"), fedwatch::number(target["upper"], "%"));
        summary_->setText(header);
    }
    previous_meeting_->setEnabled(meetings_->currentIndex() > 0);
    next_meeting_->setEnabled(meetings_->currentIndex() >= 0 && meetings_->currentIndex() + 1 < meetings_->count());
    QStringList state;
    state << tr("Snapshot retrieved: %1").arg(snapshot_["retrieved_at"].toString(tr("Not refreshed")));
    if (meeting()["status"].toString() == "PENDING")
        state << tr("PENDING · no current probabilities; Refresh retries FRED resolution; history loading is explicit");
    if (resolved())
        state << tr("RESOLVED · Refresh reads local data; Polymarket history loading is explicit");
    else if (!current_ok_)
        state << tr("Current providers unavailable or not refreshed. Stored observations are historical, not current.");
    if (!current_error_.isEmpty())
        state << current_error_;
    const auto collection = snapshot_["history_collection"].toObject();
    bool selected_lifecycle_failed = false;
    for (const auto& item : collection["lifecycle"].toObject()["pending"].toArray())
        if (item.toObject()["meeting_date"].toString() == selected_meeting_ &&
            !collection["errors"].toArray().isEmpty())
            selected_lifecycle_failed = true;
    if (selected_lifecycle_failed) {
        state.prepend(tr("Lifecycle update failed: %1").arg(errors(collection)));
        summary_->setText(summary_->text() + tr("\nFRED lifecycle update failed · press Refresh to retry"));
    } else if (meeting()["status"].toString() == "PENDING")
        summary_->setText(summary_->text() +
                          tr("\nPending resolution · Refresh retries FRED; history loading is explicit"));
    const QStringList history_issues{inventory_error_, series_error_, analytics_error_};
    bool history_issue_present = false;
    for (const auto& issue : history_issues)
        if (!issue.isEmpty()) {
            state << issue;
            history_issue_present = true;
        }
    const QString backfill = backfill_status();
    if (!backfill.isEmpty())
        state << backfill;
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
    if (selected_lifecycle_failed)
        concise << tr("FRED lifecycle update failed · Refresh retries resolution");
    if (history_issue_present)
        concise << tr("History incomplete · see Research details");
    if (!backfill.isEmpty())
        concise << backfill.section('\n', 0, 0);
    if (!current_error_.isEmpty())
        concise << tr("Provider issue · see Research details");
    const bool selected_backfill_pending = backfill_in_flight_ && backfill_meeting_ == selected_meeting_;
    if (selected_backfill_pending)
        concise << tr("Loading Polymarket history…");
    diagnostic_status_->setText(concise.join("  |  "));
    diagnostic_status_->setToolTip(state.join("\n"));
    status_->setAccessibleDescription(!selected_backfill_pending && !analytics_.isEmpty() ? tr("Historical data loaded")
                                      : history_issue_present ? tr("Historical data unavailable")
                                                              : tr("Loading historical data"));
    const auto current_rows = current_outcome_rows();
    QMap<QString, QJsonObject> rows;
    for (const auto& row : current_rows)
        rows[key(row["outcome_bp"].toInt(), row["open_ended"].toBool())] = row;
    current_chart_->set_rows(current_rows);
    current_chart_->setVisible(!resolved() && !current_chart_->rows().isEmpty());
    current_chart_->set_selected_outcome(outcomes_->currentData().toString().section(':', 0, 0).toInt(),
                                         outcomes_->currentData().toString().endsWith(":tail"));
    findChild<QLabel*>("fedwatchDistributionTitle")->setVisible(!resolved() && !current_chart_->rows().isEmpty());
    distribution_->setVisible(!resolved());
    selected_current_->setVisible(!resolved());
    distribution_->setRowCount(rows.size());
    int r = 0;
    for (const auto& row : current_rows) {
        cell(distribution_, r, 0, fedwatch::outcome_label(row["outcome_bp"].toInt(), row["open_ended"].toBool()));
        cell(distribution_, r, 1, fedwatch::number(row["fed_probability_pct"]));
        cell(distribution_, r, 2, fedwatch::number(row["polymarket_probability_pct"]));
        cell(distribution_, r, 3, fedwatch::number(row["probability_diff_pp"]));
        ++r;
    }
    if (rows.isEmpty()) {
        distribution_->setRowCount(1);
        cell(distribution_, 0, 0,
             resolved() ? tr("Resolved meeting: use the history charts above")
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
    details_->setPlainText(QString::fromUtf8(
        QJsonDocument(
            QJsonObject{{"current_snapshot", snapshot_},
                        {"provisional_imports", QJsonObject{{"cme_interchange", "PROVISIONAL_FIXTURE_ONLY"},
                                                            {"investing_monthly", "PROVISIONAL_FIXTURE_ONLY"}}},
                        {"stored_meeting", stored},
                        {"stored_observations", series_},
                        {"analytics", analytics_},
                        {"polymarket_history_update", backfill_results_.value(selected_meeting_).data},
                        {"polymarket_history_update_error", backfill_results_.value(selected_meeting_).error}})
            .toJson(QJsonDocument::Indented)));
    render_history();
    sync_chips();
    show_content_page(workspace_page_);
    const bool idle = !collect_in_flight_ && !backfill_in_flight_;
    fetch_btn_->setEnabled(idle);
    update_upcoming_->setEnabled(idle);
}
QString FedWatchPanel::backfill_status() const {
    const auto stored = meeting()["polymarket_backfill"].toObject();
    const auto immediate = backfill_results_.value(selected_meeting_);
    if (stored.isEmpty() && !backfill_results_.contains(selected_meeting_))
        return {};
    QStringList statuses, details;
    for (const auto& status : stored["statuses"].toArray())
        statuses << status.toString();
    for (const auto& item : immediate.data["backfills"].toArray()) {
        const auto row = item.toObject();
        if (row["meeting_date"].toString() != selected_meeting_)
            continue;
        const QString status = row["status"].toString();
        if (!statuses.contains(status))
            statuses << status;
        QString detail = fedwatch::outcome_label(row["outcome_bp"].toInt(), row["open_ended"].toBool()) + ": " + status;
        if (row["points_accepted"].isDouble())
            detail += tr(" · %1 accepted points").arg(row["points_accepted"].toInt());
        const auto counts = row["malformed_counts"].toObject();
        if (!counts.isEmpty())
            detail += tr(" · Dropped: %1 malformed / %2 future / %3 out of range")
                          .arg(counts["malformed"].toInt())
                          .arg(counts["future"].toInt())
                          .arg(counts["out_of_range"].toInt());
        if (row["error_code"].isString())
            detail += " · " + row["error_code"].toString();
        details << detail;
    }
    const auto warnings = immediate.data["warnings"].toArray();
    for (const auto& warning : warnings)
        details << tr("Warning: ") + warning.toString();
    if (!immediate.error.isEmpty())
        details << immediate.error;
    QString summary =
        tr("Polymarket history backfill: %1").arg(statuses.isEmpty() ? tr("status unavailable") : statuses.join(", "));
    if (stored["points"].isDouble())
        summary += tr(" · %1 backfill points").arg(stored["points"].toInt());
    if (stored["last_backfill_at"].isString())
        summary += tr(" · Last backfill %1").arg(stored["last_backfill_at"].toString());
    if (!warnings.isEmpty())
        summary += tr(" · Warnings · see Research details");
    if (!immediate.error.isEmpty())
        summary += tr(" · Update issue · see Research details");
    return (QStringList{summary} + details).join('\n');
}
void FedWatchPanel::render_history() {
    status_->setText(
        tr("HISTORY · %1").arg(outcomes_->currentIndex() >= 0 ? outcomes_->currentText() : tr("select an outcome")));
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
    const auto filtered = fedwatch::filter_range(probabilities, ranges_->currentData().toInt(), anchor);
    const bool retained_history = !probabilities[0].points.isEmpty() || !probabilities[1].points.isEmpty();
    history_controls_->setVisible(retained_history);
    probability_->set_series({filtered[0]});
    polymarket_->set_series({filtered[1]});
    const auto bounds = fedwatch::shared_probability_bounds(filtered);
    probability_->set_value_bounds(bounds.first, bounds.second);
    polymarket_->set_value_bounds(bounds.first, bounds.second);
    const QString scale =
        tr("Shared scale %1–%2%").arg(QString::number(bounds.first, 'f', 2), QString::number(bounds.second, 'f', 2));
    fed_history_state_->setText(fedwatch::history_coverage(filtered[0].points) + " · " + scale);
    poly_history_state_->setText(fedwatch::history_coverage(filtered[1].points) + " · " + scale);
    if (!series_error_.isEmpty() || !analytics_error_.isEmpty()) {
        fed_history_state_->setText(fed_history_state_->text() + tr(" · History incomplete; see Research details"));
        poly_history_state_->setText(poly_history_state_->text() + tr(" · History incomplete; see Research details"));
    }
    QStringList quality;
    for (const auto& status : meeting()["polymarket_backfill"].toObject()["statuses"].toArray())
        quality << status.toString();
    for (const auto& value : backfill_results_.value(selected_meeting_).data["backfills"].toArray()) {
        const auto row = value.toObject();
        if (row["meeting_date"].toString() == selected_meeting_)
            quality << row["status"].toString();
    }
    QString quality_note;
    if (quality.contains("PROVIDER_ERROR"))
        quality_note = tr("Meeting history provider error; retained observations shown");
    else if (quality.contains("PARTIAL"))
        quality_note = tr("Meeting history partial; accepted observations shown");
    else if (quality.contains("EMPTY"))
        quality_note = tr("Some meeting outcomes have no backfill points");
    if (!quality_note.isEmpty())
        poly_history_state_->setText(poly_history_state_->text() + " · " + quality_note);
    // Detailed source quality remains visible on expansion, away from the plots.
    for (auto* label : {fed_history_state_, poly_history_state_}) {
        label->setToolTip(label->text());
    }
    const auto mapping = meeting()["polymarket_mapping"].toObject();
    bool selected_token = false;
    for (const auto& value : mapping["outcomes"].toArray()) {
        const auto row = value.toObject();
        if (key(row["outcome_bp"].toInt(), row["open_ended"].toBool()) == outcomes_->currentData().toString() &&
            !row["external_token_id"].toString().isEmpty())
            selected_token = true;
    }
    const auto revalidation = mapping["last_revalidation_status"].toString();
    const bool history_not_loaded =
        meeting()["polymarket_backfill"].toObject().isEmpty() && !backfill_results_.contains(selected_meeting_);
    // An attempt record is not usable history. Check the full retained selected
    // series, not a range-filtered chart that may simply exclude existing points.
    const bool retry_history =
        quality.contains("PROVIDER_ERROR") && !analytics_.isEmpty() && probabilities[1].points.isEmpty();
    contextual_load_history_->setText(retry_history ? tr("Retry history") : tr("Load Polymarket history"));
    contextual_load_history_->setAccessibleName(retry_history ? tr("Retry Polymarket history")
                                                              : tr("Load Polymarket history"));
    const bool history_available = selected_token && mapping["mapping_status"].toString() == "VALIDATED" &&
                                   (local_only() || (revalidation != "NOT_FOUND" && revalidation != "AMBIGUOUS")) &&
                                   (history_not_loaded || retry_history);
    contextual_load_history_->setVisible(diagnostics_->isHidden() && history_available);
    load_history_->setEnabled(!collect_in_flight_ && !backfill_in_flight_ && !selected_meeting_.isEmpty() &&
                              (!resolved() || history_available));
    contextual_load_history_->setEnabled(!collect_in_flight_ && !backfill_in_flight_);
    int fed_count = 0, poly_count = 0;
    for (const auto& bar : current_chart_->bars())
        bar.source == 0 ? ++fed_count : ++poly_count;
    compact_coverage_->setText(
        (resolved() ? QString{}
                    : tr("Current: Fed-side %1 outcomes · Polymarket %2 outcomes\n").arg(fed_count).arg(poly_count)) +
        tr("History (%1): Fed-side %2 · Polymarket %3 observations")
            .arg(ranges_->currentText())
            .arg(filtered[0].points.size())
            .arg(filtered[1].points.size()));
    QStringList outside_range;
    if (filtered[0].points.isEmpty() && !probabilities[0].points.isEmpty())
        outside_range << tr("Fed-side %1").arg(probabilities[0].points.size());
    if (filtered[1].points.isEmpty() && !probabilities[1].points.isEmpty())
        outside_range << tr("Polymarket %1").arg(probabilities[1].points.size());
    if (!outside_range.isEmpty())
        compact_coverage_->setText(compact_coverage_->text() +
                                   tr("\nRetained outside this range: %1").arg(outside_range.join(" · ")));
    arrange_charts();
    QDateTime first, last;
    for (const auto& series : filtered)
        for (const auto& point : series.points) {
            if (!first.isValid() || point.instant < first)
                first = point.instant;
            if (!last.isValid() || point.instant > last)
                last = point.instant;
        }
    if (ranges_->currentData().toInt() > 0 && anchor.isValid()) {
        first = anchor.addDays(-ranges_->currentData().toInt());
        last = anchor;
    }
    probability_->set_time_bounds(first, last);
    polymarket_->set_time_bounds(first, last);
    findChild<QLabel*>("fedwatchProbabilityTitle")
        ->setText(tr("Fed-side history · %1 (%)").arg(fedwatch::source_label(methods_->currentData().toString())));
    const auto filtered_difference = fedwatch::filter_range(differences, ranges_->currentData().toInt(), anchor);
    divergence_->set_series(filtered_difference);
    const bool paired_points = !filtered_difference[0].points.isEmpty();
    auto* difference_toggle = findChild<QToolButton*>("fedwatchDifferenceToggle");
    difference_toggle->setVisible(paired_points);
    difference_toggle->setEnabled(paired_points);
    if (!paired_points) {
        QSignalBlocker blocker(difference_toggle);
        difference_toggle->setChecked(false);
        difference_toggle->setText(tr("Show historical difference"));
    }
    divergence_->setVisible(paired_points && difference_toggle->isChecked());
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
    const QString backfill = backfill_status();
    if (!backfill.isEmpty()) {
        coverage_->setText(coverage_->text() + "\n" + backfill.section('\n', 0, 0));
        coverage_->setToolTip(coverage_->toolTip() + "\n" + backfill);
    }
}
QVariantMap FedWatchPanel::save_panel_state() const {
    const QString selection = !restored_outcome_.isEmpty()   ? restored_outcome_
                              : !selected_outcome_.isEmpty() ? selected_outcome_
                                                             : outcomes_->currentData().toString();
    QVariantMap state{{"meeting", selected_meeting_},
                      {"fed_method", methods_->currentData()},
                      {"range_days", ranges_->currentData()},
                      {"details_visible", !diagnostics_->isHidden()}};
    if (!selection.isEmpty()) {
        state["outcome_bp"] = selection.section(':', 0, 0).toInt();
        state["open_ended"] = selection.endsWith(":tail");
    }
    return state;
}
void FedWatchPanel::restore_panel_state(const QVariantMap& state) {
    ++generation_;
    if (selected_meeting_ != state.value("meeting").toString()) {
        series_error_.clear();
        analytics_error_.clear();
    }
    selected_meeting_ = state.value("meeting").toString();
    meeting_explicitly_selected_ = !selected_meeting_.isEmpty();
    if (state.contains("outcome_bp")) {
        outcome_explicitly_selected_ = true;
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
    color_ = ui::colors::AMBER();
    EconPanelBase::refresh_panel_theme();
    setStyleSheet(
        panel_style() +
        QString(
            "QLabel { color:%1; font-size:11px; } "
            "QPushButton#fedwatchUpdateUpcoming, QPushButton#fedwatchLoadHistory, "
            "QPushButton#fedwatchContextLoadHistory { padding:5px 10px; color:%1; "
            "background:%3; border:1px solid %2; }"
            "QLabel#fedwatchProbabilityTitle, QLabel#fedwatchPolymarketTitle { color:%4; font-size:12px; "
            "font-weight:600; }"
            "QPushButton[selection_kind] { padding:6px 12px; border-radius:6px; border:1px solid %2; background:%3; "
            "color:%1; font-size:11px; }"
            "QPushButton[selection_kind]:hover { background:%5; border-color:%6; color:%4; }"
            "QPushButton[selection_kind]:checked { background:%6; border-color:%6; color:%7; }"
            "QPushButton[selection_kind]:focus { border:2px solid %6; }"
            "QToolButton { color:%4; background:%3; border:1px solid %2; padding:4px 8px; }"
            "QToolButton:hover, QPushButton#fedwatchContextLoadHistory:hover { background:%5; border-color:%6; "
            "color:%4; }"
            "QToolButton:focus, QPushButton#fedwatchContextLoadHistory:focus { border:2px solid %6; }"
            "QToolButton#fedwatchEarlierMeetings, QToolButton#fedwatchLaterMeetings { padding:4px 8px; color:%4; "
            "background:%3; border:1px solid %2; font-size:20px; }")
            .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), ui::colors::BG_SURFACE(),
                 ui::colors::TEXT_PRIMARY(), ui::colors::BG_HOVER(), ui::colors::AMBER(),
                 ui::colors::TEXT_ON_ACCENT()));
    if (probability_)
        probability_->update();
    if (divergence_)
        divergence_->update();
    if (polymarket_)
        polymarket_->update();
    if (current_chart_)
        current_chart_->update();
}
void FedWatchPanel::arrange_controls() {
    if (!controls_layout_ || !meeting_control_ || !outcome_control_ || !range_control_)
        return;
    for (auto* group : {meeting_control_, outcome_control_})
        controls_layout_->removeWidget(group);
    for (int column = 0; column < 3; ++column)
        controls_layout_->setColumnStretch(column, 0);
    if (width() >= 1100) {
        controls_layout_->addWidget(meeting_control_, 0, 0);
        controls_layout_->addWidget(outcome_control_, 0, 1);
        controls_layout_->setColumnStretch(0, 4);
        controls_layout_->setColumnStretch(1, 4);
    } else if (width() >= 700) {
        controls_layout_->addWidget(meeting_control_, 0, 0, 1, 2);
        controls_layout_->addWidget(outcome_control_, 1, 0);
        controls_layout_->setColumnStretch(0, 1);
        controls_layout_->setColumnStretch(1, 1);
    } else {
        controls_layout_->addWidget(meeting_control_, 0, 0);
        controls_layout_->addWidget(outcome_control_, 1, 0);
        controls_layout_->setColumnStretch(0, 1);
    }
}
void FedWatchPanel::arrange_charts() {
    if (!charts_layout_ || !probability_section_ || !polymarket_section_)
        return;
    charts_layout_->removeWidget(probability_section_);
    charts_layout_->removeWidget(polymarket_section_);
    const bool fed = probability_ && !probability_->series().isEmpty() && !probability_->series()[0].points.isEmpty();
    const bool poly = polymarket_ && !polymarket_->series().isEmpty() && !polymarket_->series()[0].points.isEmpty();
    probability_section_->setVisible(fed);
    polymarket_section_->setVisible(poly);
    charts_layout_->parentWidget()->setVisible(fed || poly);
    const bool wide = width() >= 900 && fed && poly;
    if (fed)
        charts_layout_->addWidget(probability_section_, 0, 0);
    if (poly)
        charts_layout_->addWidget(polymarket_section_, wide ? 0 : (fed ? 1 : 0), wide ? 1 : 0);
    charts_layout_->setColumnStretch(0, 1);
    charts_layout_->setColumnStretch(1, wide ? 1 : 0);
}
void FedWatchPanel::resizeEvent(QResizeEvent* event) {
    EconPanelBase::resizeEvent(event);
    arrange_controls();
    arrange_charts();
    ensure_selected_controls();
}
void FedWatchPanel::ensure_selected_controls() {
    // Reflow changes each strip's viewport without changing its selection.
    // Keep active choices visible once the new scrollbar ranges have settled.
    QTimer::singleShot(0, this, [this] {
        for (auto* container : {meeting_chips_, outcome_chips_, range_chips_}) {
            if (!container)
                continue;
            auto* scroll = qobject_cast<QScrollArea*>(container->parentWidget()->parentWidget());
            if (!scroll)
                continue;
            for (auto* button : container->findChildren<QPushButton*>(QString{}, Qt::FindDirectChildrenOnly))
                if (button->isChecked() && !button->isHidden())
                    scroll->ensureWidgetVisible(button, 0, 0);
        }
    });
}
} // namespace fincept::screens
