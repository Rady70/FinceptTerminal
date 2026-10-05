#include "screens/economics/panels/FedWatchPanel.h"

#include "core/config/ProfileManager.h"
#include "core/session/ScreenStateManager.h"
#include "network/http/ExternalUrlGuard.h"
#include "screens/common/IStatefulScreen.h"
#include "screens/economics/panels/FedWatchCharts.h"
#include "services/economics/EconomicsEnvelopeParse.h"
#include "ui/theme/Theme.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMap>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace fincept::screens {
using namespace fedwatch;

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
// Retained-data notes, not failures: the value was kept and is labelled on
// its meeting (e.g. a lagging duplicate card that disagrees with the main one).
bool informational(const QJsonObject& error) {
    return error["code"].toString() == QLatin1String("INVESTING_COPY_CONFLICT");
}
QString error_lines(const QJsonArray& errors, int limit = 6, bool skip_informational = false) {
    QStringList lines;
    QSet<QString> seen;
    for (const auto& item : errors) {
        const auto e = item.toObject();
        if (skip_informational && informational(e))
            continue;
        const QString line = e["provider"].toString(QStringLiteral("fedwatch")) + " · " + e["code"].toString() + ": " +
                             e["error"].toString();
        if (seen.contains(line))
            continue;
        seen.insert(line);
        lines << line;
    }
    if (lines.size() > limit)
        return (lines.mid(0, limit) + QStringList{QObject::tr("… %1 more").arg(lines.size() - limit)}).join("\n");
    return lines.join("\n");
}
QTableWidget* make_table(QWidget* parent, const char* name, const QStringList& headings) {
    auto* table = new QTableWidget(parent);
    table->setObjectName(name);
    table->setColumnCount(headings.size());
    table->setHorizontalHeaderLabels(headings);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setWordWrap(true);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return table;
}
void set_cell(QTableWidget* table, int row, int column, const QString& text, Qt::Alignment align = Qt::AlignRight) {
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(align | Qt::AlignVCenter);
    item->setToolTip(text);
    table->setItem(row, column, item);
}
void fit_height(QTableWidget* table) {
    table->resizeRowsToContents();
    int height = table->horizontalHeader()->height() + 4;
    for (int row = 0; row < table->rowCount(); ++row)
        height += table->rowHeight(row);
    table->setFixedHeight(height);
}
QString status_word(const QString& status) {
    if (status == QLatin1String("OK"))
        return QObject::tr("OK");
    if (status == QLatin1String("PARTIAL"))
        return QObject::tr("Partial");
    if (status == QLatin1String("ERROR"))
        return QObject::tr("Failed");
    if (status == QLatin1String("SKIPPED"))
        return QObject::tr("Skipped");
    return status.isEmpty() ? QObject::tr("Not refreshed") : status;
}
} // namespace

FedWatchPanel::FedWatchPanel(QWidget* parent) : FedWatchPanel(Dispatch{}, parent) {}

FedWatchPanel::FedWatchPanel(Dispatch dispatch, QWidget* parent)
    : EconPanelBase("fedwatch", ui::colors::AMBER(), parent), dispatch_(std::move(dispatch)) {
    setObjectName("fedwatchPanel");
    build_base_ui(this);
    set_stats_visible(false);
    export_btn_->hide();
    fetch_btn_->setObjectName("econFetchBtn");
    fetch_btn_->setText(tr("REFRESH"));
    fetch_btn_->setAccessibleName(tr("Refresh FedWatch"));
    fetch_btn_->setToolTip(tr("Collect current observations for every upcoming meeting, update Polymarket daily "
                              "history for validated markets, then reload stored data"));
    if (auto* toolbar = findChild<QWidget*>("econToolbar")) {
        toolbar->setFixedHeight(48);
        auto* layout = static_cast<QHBoxLayout*>(toolbar->layout());
        open_cme_ = new QPushButton(tr("CME FEDWATCH ↗"), toolbar);
        open_cme_->setObjectName("fedwatchOpenCme");
        open_cme_->setCursor(Qt::PointingHandCursor);
        open_cme_->setAccessibleName(tr("Open the official CME FedWatch tool in a browser"));
        open_cme_->setToolTip(tr("Opens CME's own FedWatch page in your browser for personal viewing. MarketLab does "
                                 "not collect CME website data (CME terms prohibit automated access)."));
        // A fixed public page, handed to the user's browser through the fork's
        // deny-list guard; MarketLab itself requests nothing from CME.
        connect(open_cme_, &QPushButton::clicked, this, [this] {
            network::ExternalUrlGuard::open_external(QUrl(QStringLiteral("https://www.cmegroup.com/fedwatch")), this);
        });
        layout->insertWidget(layout->indexOf(fetch_btn_), open_cme_);
    }

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    content->setObjectName("fedwatchContent");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 10, 12, 16);
    layout->setSpacing(12);

    status_ = new QLabel(content);
    status_->setObjectName("fedwatchStatus");
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_);

    // KPI row.
    kpi_row_ = new QWidget(content);
    kpi_layout_ = new QGridLayout(kpi_row_);
    kpi_layout_->setContentsMargins(0, 0, 0, 0);
    kpi_layout_->setSpacing(8);
    const QStringList kpi_names{"Target", "Next", "Likely", "Shift", "Path"};
    for (const auto& name : kpi_names) {
        Kpi kpi;
        kpi.frame = card("fedwatchKpi" + name);
        kpi.frame->setProperty("kpi", true);
        auto* v = new QVBoxLayout(kpi.frame);
        v->setContentsMargins(12, 8, 12, 9);
        v->setSpacing(2);
        kpi.caption = new QLabel(kpi.frame);
        kpi.caption->setObjectName("fedwatchKpiCaption");
        kpi.value = new QLabel(QStringLiteral("—"), kpi.frame);
        kpi.value->setObjectName("fedwatchKpi" + name + "Value");
        kpi.value->setProperty("kpiValue", true);
        kpi.sub = new QLabel(kpi.frame);
        kpi.sub->setObjectName("fedwatchKpiSub");
        kpi.sub->setWordWrap(true);
        v->addWidget(kpi.caption);
        v->addWidget(kpi.value);
        v->addWidget(kpi.sub);
        v->addStretch();
        kpis_.push_back(kpi);
    }
    layout->addWidget(kpi_row_);

    // Rate path: matrix and bubble path.
    auto* path_card = card("fedwatchPathCard");
    auto* path_layout = new QVBoxLayout(path_card);
    path_layout->setContentsMargins(12, 10, 12, 12);
    path_layout->setSpacing(8);
    path_layout->addWidget(section(path_card, tr("POLICY PATH · PROBABILITY OF EACH TARGET RANGE BY MEETING"),
                                   tr("Fed-side probabilities from the Investing.com Fed Rate Monitor (CME 30-Day Fed "
                                      "Funds futures). Click a meeting to focus it below.")));
    matrix_scroll_ = new QScrollArea(path_card);
    matrix_scroll_->setObjectName("fedwatchMatrixScroll");
    matrix_scroll_->setFrameShape(QFrame::NoFrame);
    matrix_scroll_->setWidgetResizable(true);
    matrix_scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    matrix_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    matrix_ = new FedWatchMatrix(matrix_scroll_);
    matrix_->setObjectName("fedwatchMatrix");
    matrix_scroll_->setWidget(matrix_);
    path_layout->addWidget(matrix_scroll_);
    path_ = new FedWatchPathChart(path_card);
    path_->setObjectName("fedwatchPathChart");
    path_->setMinimumHeight(380);
    path_layout->addWidget(path_);
    layout->addWidget(path_card);
    connect(matrix_, &FedWatchMatrix::meeting_selected, this, &FedWatchPanel::select_meeting);
    connect(path_, &FedWatchPathChart::meeting_selected, this, &FedWatchPanel::select_meeting);

    // Selected meeting focus.
    auto* focus_card = card("fedwatchFocusCard");
    auto* focus = new QVBoxLayout(focus_card);
    focus->setContentsMargins(12, 10, 12, 12);
    focus->setSpacing(8);
    chips_ = new QWidget(focus_card);
    chips_->setObjectName("fedwatchMeetingChips");
    auto* chip_layout = new QHBoxLayout(chips_);
    chip_layout->setContentsMargins(0, 0, 0, 0);
    chip_layout->setSpacing(4);
    auto* chip_scroll = new QScrollArea(focus_card);
    chip_scroll->setObjectName("fedwatchMeetingChipScroll");
    chip_scroll->setFrameShape(QFrame::NoFrame);
    chip_scroll->setWidgetResizable(true);
    chip_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chip_scroll->setFixedHeight(40);
    chip_scroll->setWidget(chips_);
    focus->addWidget(chip_scroll);
    focus_title_ = new QLabel(focus_card);
    focus_title_->setObjectName("fedwatchFocusTitle");
    focus_meta_ = new QLabel(focus_card);
    focus_meta_->setObjectName("fedwatchFocusMeta");
    focus_meta_->setWordWrap(true);
    focus_meta_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    focus->addWidget(focus_title_);
    focus->addWidget(focus_meta_);
    auto* focus_grid = new QWidget(focus_card);
    focus_layout_ = new QGridLayout(focus_grid);
    focus_layout_->setContentsMargins(0, 0, 0, 0);
    focus_layout_->setHorizontalSpacing(16);
    focus_layout_->setVerticalSpacing(12);
    band_card_ = new QWidget(focus_grid);
    auto* band_layout = new QVBoxLayout(band_card_);
    band_layout->setContentsMargins(0, 0, 0, 0);
    band_layout->addWidget(section(band_card_, tr("TARGET RANGE AFTER THIS MEETING"),
                                   tr("Fed-side probability now, with Investing.com's previous-day and previous-week "
                                      "values")));
    bands_ = new FedWatchBandBars(band_card_);
    bands_->setObjectName("fedwatchBandBars");
    band_layout->addWidget(bands_);
    band_note_ = new QLabel(band_card_);
    band_note_->setObjectName("fedwatchBandNote");
    band_note_->setWordWrap(true);
    band_note_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    band_layout->addWidget(band_note_);
    band_layout->addStretch();
    outcome_card_ = new QWidget(focus_grid);
    auto* outcome_layout = new QVBoxLayout(outcome_card_);
    outcome_layout->setContentsMargins(0, 0, 0, 0);
    outcome_layout->addWidget(section(outcome_card_, tr("DECISION AT THIS MEETING · FED-SIDE VS POLYMARKET"),
                                      tr("Change of the target range at this meeting only")));
    outcomes_ = new FedWatchOutcomeBars(outcome_card_);
    outcomes_->setObjectName("fedwatchOutcomeBars");
    outcome_layout->addWidget(outcomes_);
    outcome_note_ = new QLabel(outcome_card_);
    outcome_note_->setObjectName("fedwatchOutcomeNote");
    outcome_note_->setWordWrap(true);
    outcome_note_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outcome_layout->addWidget(outcome_note_);
    outcome_layout->addStretch();
    focus->addWidget(focus_grid);
    layout->addWidget(focus_card);

    // History of expectations.
    auto* history_card = card("fedwatchHistoryCard");
    auto* history = new QVBoxLayout(history_card);
    history->setContentsMargins(12, 10, 12, 12);
    history->setSpacing(8);
    history->addWidget(section(history_card, tr("HOW EXPECTATIONS FOR THIS MEETING EVOLVED"),
                               tr("One row per outcome, one cell per day (the day's latest accepted observation); "
                                  "brighter = more likely. Days without an observation stay dark; nothing is "
                                  "interpolated."),
                               &history_note_));
    history->addWidget(
        section(history_card, tr("Fed-side · probability of each target range, by day"), QString{}, &fed_mix_hint_));
    fed_mix_ = new FedWatchHeatStrip(0, history_card);
    fed_mix_->setObjectName("fedwatchFedHistory");
    history->addWidget(fed_mix_);
    history->addWidget(
        section(history_card, tr("Polymarket · probability of each decision, by day"), QString{}, &poly_mix_hint_));
    poly_mix_ = new FedWatchHeatStrip(1, history_card);
    poly_mix_->setObjectName("fedwatchPolyHistory");
    history->addWidget(poly_mix_);
    history->addWidget(section(history_card, tr("CHANGE BY OUTCOME"),
                               tr("Approved stored-history calculations: a change needs an observation at or before "
                                  "the lookback; otherwise it stays blank.")));
    changes_ = make_table(history_card, "fedwatchChanges",
                          {tr("Outcome"), tr("Fed now"), tr("Fed 1D"), tr("Fed 7D"), tr("Fed 30D"), tr("Poly now"),
                           tr("Poly 1D"), tr("Poly 7D"), tr("Poly 30D"), tr("Poly − Fed now"), tr("Observations")});
    changes_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    history->addWidget(changes_);
    layout->addWidget(history_card);

    // Sources and as-of.
    auto* sources_card = card("fedwatchSourcesCard");
    auto* sources = new QVBoxLayout(sources_card);
    sources->setContentsMargins(12, 10, 12, 12);
    sources->setSpacing(8);
    sources->addWidget(section(sources_card, tr("SOURCES · AS OF"), tr("What each source supplies and when")));
    sources_ =
        make_table(sources_card, "fedwatchSources",
                   {tr("Source"), tr("Provides"), tr("Status"), tr("Source time"), tr("Retrieved"), tr("Notes")});
    sources->addWidget(sources_);
    diagnostics_ = new QLabel(sources_card);
    diagnostics_->setObjectName("fedwatchDiagnostics");
    diagnostics_->setWordWrap(true);
    diagnostics_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sources->addWidget(diagnostics_);
    notes_ = new QLabel(sources_card);
    notes_->setObjectName("fedwatchMethodNotes");
    notes_->setWordWrap(true);
    notes_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sources->addWidget(notes_);
    audit_toggle_ = new QToolButton(sources_card);
    audit_toggle_->setObjectName("fedwatchAuditToggle");
    audit_toggle_->setText(tr("Stored data (JSON)"));
    audit_toggle_->setCheckable(true);
    audit_toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    audit_toggle_->setArrowType(Qt::RightArrow);
    sources->addWidget(audit_toggle_);
    audit_ = new QPlainTextEdit(sources_card);
    audit_->setObjectName("fedwatchAudit");
    audit_->setReadOnly(true);
    audit_->setMinimumHeight(320);
    audit_->hide();
    sources->addWidget(audit_);
    connect(audit_toggle_, &QToolButton::toggled, this, [this](bool on) {
        audit_->setVisible(on);
        audit_toggle_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        if (on)
            audit_->setPlainText(QString::fromUtf8(QJsonDocument(raw_workspace_).toJson(QJsonDocument::Indented)));
        notify_state(this);
    });
    layout->addWidget(sources_card);
    layout->addStretch();

    scroll->setWidget(content);
    workspace_page_ = add_content_page(scroll);
    show_content_page(workspace_page_);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &FedWatchPanel::accept_result);
    arrange();
    render();
    refresh_panel_theme();
}

QFrame* FedWatchPanel::card(const QString& object_name) {
    auto* frame = new QFrame(this);
    frame->setObjectName(object_name);
    frame->setProperty("fedwatchCard", true);
    return frame;
}
QWidget* FedWatchPanel::section(QWidget* parent, const QString& title, const QString& hint, QLabel** hint_label) {
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* heading = new QLabel(title, box);
    heading->setProperty("fedwatchSection", true);
    v->addWidget(heading);
    auto* note = new QLabel(hint, box);
    note->setProperty("fedwatchHint", true);
    note->setWordWrap(true);
    note->setVisible(!hint.isEmpty() || hint_label);
    v->addWidget(note);
    if (hint_label)
        *hint_label = note;
    return box;
}

void FedWatchPanel::build_controls(QHBoxLayout* toolbar) {
    auto* title = new QLabel(tr("FED POLICY EXPECTATIONS"), this);
    title->setObjectName("fedwatchTitle");
    toolbar->addWidget(title);
    as_of_ = new QLabel(this);
    as_of_->setObjectName("fedwatchAsOf");
    as_of_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Long provenance must wrap, never widen the panel past its window.
    as_of_->setWordWrap(true);
    as_of_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toolbar->addSpacing(14);
    toolbar->addWidget(as_of_, 1);
}

void FedWatchPanel::request(const QString& command, const QStringList& args) {
    QStringList full = args;
    full << "--db" << ProfileManager::instance().profile_root() + "/fedwatch/fedwatch_history.db";
    const QString id = QString("fedwatch:%1:%2").arg(quintptr(this)).arg(++sequence_);
    pending_.insert(id, command);
    if (command == QLatin1String("workspace"))
        latest_workspace_request_ = id;
    emit command_requested(command, full, id);
    if (dispatch_)
        dispatch_(command, full, id);
    else
        services::EconomicsService::instance().execute("fedwatch", "fedwatch_data.py", command, full, id, true);
}

void FedWatchPanel::activate() {
    if (activated_)
        return;
    activated_ = true;
    load_workspace();
}

void FedWatchPanel::load_workspace() {
    if (stage_ == Stage::Idle)
        stage_ = Stage::Loading;
    render_status();
    request("workspace");
}

void FedWatchPanel::on_fetch() {
    if (stage_ == Stage::Collecting || stage_ == Stage::Backfilling)
        return;
    stage_ = Stage::Collecting;
    refresh_issues_.clear();
    refresh_summary_.clear();
    render_status();
    request("collect");
}

void FedWatchPanel::accept_result(const QString& id, const services::EconomicsResult& result) {
    on_result(id, result);
}

void FedWatchPanel::on_result(const QString& id, const services::EconomicsResult& result) {
    const auto it = pending_.find(id);
    if (it == pending_.end())
        return;
    const QString command = it.value();
    pending_.erase(it);
    const auto data = payload(result.data);
    const bool usable = result.success || (result.data["success"].toBool() && result.data["data"].isObject());
    if (command == QLatin1String("collect")) {
        if (!usable) {
            refresh_issues_ << tr("Current collection failed: %1")
                                   .arg(result.error.isEmpty() ? tr("no usable response") : result.error);
        } else {
            int fed = 0, poly = 0, total = 0;
            for (const auto& item : data["meetings"].toArray()) {
                const auto m = item.toObject();
                ++total;
                if (m["fed_side"].toObject()["normalized_probabilities"].isArray())
                    ++fed;
                if (m["polymarket"].toObject()["data_status"].toString() == QLatin1String("CURRENT"))
                    ++poly;
            }
            refresh_summary_ = tr("Collected %1 upcoming meetings · Fed-side %2 · Polymarket current %3")
                                   .arg(total)
                                   .arg(fed)
                                   .arg(poly);
            const QString errors = error_lines(data["errors"].toArray(), 6, true);
            if (!errors.isEmpty())
                refresh_issues_ << errors;
        }
        stage_ = Stage::Backfilling;
        render_status();
        request("history_backfill");
        return;
    }
    if (command == QLatin1String("history_backfill")) {
        if (!usable) {
            refresh_issues_ << tr("Polymarket history update failed: %1").arg(result.error);
        } else {
            QMap<QString, int> statuses;
            for (const auto& item : data["backfills"].toArray())
                ++statuses[item.toObject()["status"].toString()];
            QStringList parts;
            for (auto s = statuses.begin(); s != statuses.end(); ++s)
                parts << QStringLiteral("%1 %2").arg(s.value()).arg(s.key().toLower());
            if (!parts.isEmpty())
                refresh_summary_ += tr(" · Polymarket history: %1").arg(parts.join(", "));
            const QString errors = error_lines(data["errors"].toArray());
            if (!errors.isEmpty())
                refresh_issues_ << errors;
        }
        stage_ = Stage::Loading;
        render_status();
        request("workspace");
        return;
    }
    if (command == QLatin1String("workspace")) {
        if (id != latest_workspace_request_)
            return;
        if (stage_ == Stage::Loading)
            stage_ = Stage::Idle;
        if (!usable || !data["meetings"].isArray()) {
            load_error_ = tr("Stored FedWatch data could not be read: %1")
                              .arg(result.error.isEmpty() ? error_lines(data["errors"].toArray()) : result.error);
        } else {
            load_error_.clear();
            raw_workspace_ = data;
            workspace_ = parse_workspace(data);
        }
        render();
    }
}

void FedWatchPanel::select_meeting(const QString& meeting) {
    if (meeting.isEmpty() || meeting == selected_ || !workspace_.find(meeting))
        return;
    selected_ = meeting;
    restored_selection_.clear();
    render();
    notify_state(this);
}

void FedWatchPanel::render() {
    if (!restored_selection_.isEmpty() && workspace_.find(restored_selection_))
        selected_ = restored_selection_;
    if (!workspace_.find(selected_)) {
        selected_ = workspace_.next_meeting;
        if (!workspace_.find(selected_) && !workspace_.meetings.isEmpty())
            selected_ = workspace_.meetings.last().id;
    }
    render_status();
    render_kpis();
    matrix_->set_workspace(workspace_, selected_);
    path_->set_workspace(workspace_, selected_);
    matrix_scroll_->setFixedHeight(matrix_->height() + 12);
    render_chips();
    render_focus();
    render_sources();
    if (audit_->isVisible())
        audit_->setPlainText(QString::fromUtf8(QJsonDocument(raw_workspace_).toJson(QJsonDocument::Indented)));
    const bool idle = stage_ == Stage::Idle || stage_ == Stage::Loading;
    fetch_btn_->setEnabled(idle);
    show_content_page(workspace_page_);
}

void FedWatchPanel::render_status() {
    QString text;
    switch (stage_) {
        case Stage::Collecting:
            text = tr("Refreshing: collecting current Investing.com, FRED, Federal Reserve calendar and Polymarket "
                      "observations for every upcoming meeting…");
            break;
        case Stage::Backfilling:
            text = tr("Refreshing: updating Polymarket daily history for validated markets…");
            break;
        case Stage::Loading:
            text = workspace_.loaded ? tr("Reloading stored data…") : tr("Loading stored FedWatch data…");
            break;
        case Stage::Idle:
            break;
    }
    QStringList lines;
    if (!text.isEmpty())
        lines << text;
    if (!load_error_.isEmpty())
        lines << load_error_;
    if (stage_ == Stage::Idle) {
        if (!refresh_summary_.isEmpty())
            lines << tr("Last refresh: ") + refresh_summary_;
        if (!refresh_issues_.isEmpty())
            lines << tr("Refresh issues (stored values remain available): ") + refresh_issues_.join("\n");
        if (workspace_.loaded && workspace_.meetings.isEmpty())
            lines << tr("No FedWatch observations are stored yet. Press REFRESH to collect current observations.");
        else if (workspace_.loaded) {
            int stale = 0;
            for (const auto* m : workspace_.upcoming())
                if (m->fed.state != QLatin1String("CURRENT"))
                    ++stale;
            if (stale > 0)
                lines << tr("%1 upcoming meeting(s) show their last stored values, marked stale. Press REFRESH for "
                            "current observations.")
                             .arg(stale);
        }
    }
    status_->setText(lines.join("\n"));
    status_->setVisible(!lines.isEmpty());
    status_->setProperty("busy", stage_ != Stage::Idle);
    fetch_btn_->setEnabled(stage_ == Stage::Idle || stage_ == Stage::Loading);
    fetch_btn_->setText(stage_ == Stage::Collecting || stage_ == Stage::Backfilling ? tr("REFRESHING…")
                                                                                    : tr("REFRESH"));
}

void FedWatchPanel::render_kpis() {
    const auto set = [this](int index, const QString& caption, const QString& value, const QString& sub,
                            const QString& tip = {}) {
        kpis_[index].caption->setText(caption);
        kpis_[index].value->setText(value);
        kpis_[index].sub->setText(sub);
        kpis_[index].frame->setToolTip(tip.isEmpty() ? sub : tip);
        kpis_[index].frame->setAccessibleName(caption + ": " + value + ". " + sub);
    };
    // Current target.
    if (workspace_.target_lower && workspace_.target_upper) {
        QString sub = tr("FRED · latest %1").arg(workspace_.target_date.toString(Qt::ISODate));
        if (workspace_.target_carried_forward)
            sub += tr(" · carried forward, no decision since");
        if (workspace_.target_status == QLatin1String("STALE"))
            sub += tr(" · STALE");
        set(0, tr("CURRENT TARGET RANGE"), band_label(*workspace_.target_lower, *workspace_.target_upper), sub);
    } else {
        set(0, tr("CURRENT TARGET RANGE"), QStringLiteral("—"), tr("No current FRED range in the last stored refresh"));
    }
    const Meeting* next = workspace_.find(workspace_.next_meeting);
    if (next) {
        QString sub = next->days_until == 0 ? tr("today") : tr("in %1 days").arg(next->days_until);
        if (next->start.isValid() && next->start != next->date)
            sub += tr(" · %1–%2").arg(next->start.toString("MMM d"), next->date.toString("MMM d"));
        if (next->projections && *next->projections)
            sub += tr(" · projections (SEP)");
        set(1, tr("NEXT FOMC DECISION"), meeting_label(next->date), sub);
        // Most likely decision at the next meeting.
        const Outcome* best = nullptr;
        for (const auto& o : next->fed.local)
            if (!best || o.pct > best->pct)
                best = &o;
        const Outcome* poly_best = nullptr;
        for (const auto& o : next->poly.outcomes)
            if (!poly_best || o.pct > poly_best->pct)
                poly_best = &o;
        if (best) {
            QString poly_text = tr("Polymarket: %1").arg(next->poly.outcomes.isEmpty() ? tr("no market") : QString{});
            if (poly_best)
                poly_text =
                    tr("Polymarket %1 %2").arg(outcome_label(poly_best->bp, poly_best->open), pct(poly_best->pct));
            set(2, tr("MOST LIKELY AT NEXT MEETING"), outcome_label(best->bp, best->open) + " · " + pct(best->pct),
                tr("Fed-side (%1) · %2").arg(state_label(next->fed.state), poly_text));
        } else if (!next->fed.bands.isEmpty()) {
            const Band* top = &next->fed.bands.first();
            for (const auto& b : next->fed.bands)
                if (b.pct > top->pct)
                    top = &b;
            set(2, tr("MOST LIKELY AT NEXT MEETING"), band_label(top->low, top->high) + " · " + pct(top->pct),
                tr("Fed-side target range (%1)").arg(state_label(next->fed.state)));
        } else {
            set(2, tr("MOST LIKELY AT NEXT MEETING"), QStringLiteral("—"), tr("No stored Fed-side distribution"));
        }
        // One-week shift of the next meeting's expected rate.
        if (next->fed.expected && next->fed.expected_previous_week)
            set(3, tr("1-WEEK SHIFT · NEXT MEETING"),
                bp_change(*next->fed.expected - *next->fed.expected_previous_week),
                tr("Expected %1 vs %2 a week earlier (Investing previous week)")
                    .arg(rate(next->fed.expected), rate(next->fed.expected_previous_week)));
        else
            set(3, tr("1-WEEK SHIFT · NEXT MEETING"), QStringLiteral("—"),
                tr("Needs a current Investing card with previous-week values"));
    } else {
        set(1, tr("NEXT FOMC DECISION"), QStringLiteral("—"), tr("No upcoming meeting stored"));
        set(2, tr("MOST LIKELY AT NEXT MEETING"), QStringLiteral("—"), QString{});
        set(3, tr("1-WEEK SHIFT · NEXT MEETING"), QStringLiteral("—"), QString{});
    }
    // Path to the furthest meeting with a distribution.
    const Meeting* last = nullptr;
    for (const auto* m : workspace_.upcoming())
        if (m->fed.expected)
            last = m;
    const auto mid = workspace_.target_mid();
    if (last && mid) {
        const double change = (*last->fed.expected - *mid) * 100.0;
        set(4, tr("PRICED PATH TO %1").arg(last->date.toString("MMM yyyy").toUpper()), rate(last->fed.expected),
            tr("%1 vs current midpoint · ≈ %2 × 25 bp moves")
                .arg(bp_change(*last->fed.expected - *mid), signed_number(change / 25.0, 1)),
            tr("Probability-weighted expected policy rate after the %1 meeting (%2) compared with the current "
               "target midpoint %3")
                .arg(meeting_label(last->date), state_label(last->fed.state), rate(mid, 3)));
    } else {
        set(4, tr("PRICED PATH"), QStringLiteral("—"), tr("Needs stored distributions and the current target"));
    }
    // As-of line in the toolbar.
    QStringList as_of;
    if (next && next->fed.source_updated_at.isValid())
        as_of << tr("Investing updated %1").arg(utc(next->fed.source_updated_at));
    if (next && next->poly.observed_at.isValid())
        as_of << tr("Polymarket %1").arg(utc(next->poly.observed_at));
    if (workspace_.last_refresh_at.isValid())
        as_of << tr("last refresh %1").arg(utc(workspace_.last_refresh_at));
    as_of_->setText(as_of.isEmpty() ? tr("No stored refresh yet") : as_of.join("  ·  "));
}

void FedWatchPanel::render_chips() {
    auto* layout = static_cast<QHBoxLayout*>(chips_->layout());
    while (auto* item = layout->takeAt(0)) {
        if (auto* w = item->widget())
            w->deleteLater();
        delete item;
    }
    for (const auto& m : workspace_.meetings) {
        QString text = m.date.toString("MMM d ''yy");
        if (m.status == QLatin1String("RESOLVED") && m.actual_bp)
            text += " · " + outcome_label(*m.actual_bp, false);
        else if (m.status == QLatin1String("PENDING"))
            text += tr(" · pending");
        auto* chip = new QPushButton(text, chips_);
        chip->setObjectName("fedwatchMeetingChip_" + m.id);
        chip->setProperty("meetingChip", true);
        chip->setProperty("meeting", m.id);
        chip->setCheckable(true);
        chip->setChecked(m.id == selected_);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setAccessibleName(tr("Focus the %1 FOMC meeting").arg(meeting_label(m.date)));
        chip->setToolTip(
            tr("%1 · %2 · Fed-side %3 · Polymarket %4")
                .arg(meeting_label(m.date), m.status, state_label(m.fed.state), state_label(m.poly.state)));
        connect(chip, &QPushButton::clicked, this, [this, chip, id = m.id] {
            if (id == selected_)
                chip->setChecked(true); // A chip never toggles its own meeting off.
            else
                select_meeting(id);
        });
        layout->addWidget(chip);
    }
    if (workspace_.meetings.isEmpty())
        layout->addWidget(new QLabel(tr("Meetings appear after the first refresh"), chips_));
    layout->addStretch();
}

void FedWatchPanel::render_focus() {
    const Meeting* m = workspace_.find(selected_);
    if (!m) {
        focus_title_->setText(tr("NO MEETING SELECTED"));
        focus_meta_->setText(tr("Press REFRESH to collect observations."));
        bands_->set_meeting({}, std::nullopt);
        outcomes_->set_meeting({});
        band_note_->clear();
        outcome_note_->clear();
        render_history({});
        render_changes({});
        return;
    }
    QString title = tr("%1 FOMC").arg(meeting_label(m->date)).toUpper();
    if (m->upcoming())
        title += tr("  ·  in %1 days").arg(m->days_until);
    focus_title_->setText(title);
    QStringList meta;
    if (m->start.isValid() && m->start != m->date)
        meta << tr("Meeting %1–%2").arg(m->start.toString("MMM d"), m->date.toString("MMM d, yyyy"));
    if (m->projections)
        meta << (*m->projections ? tr("with Summary of Economic Projections") : tr("no projections"));
    meta << tr("Status %1").arg(m->status.toLower());
    if (m->actual_bp)
        meta << tr("Decision: %1").arg(outcome_label(*m->actual_bp, false));
    meta << tr("Fed-side %1%2")
                .arg(state_label(m->fed.state),
                     m->fed.observed_at.isValid() ? tr(" (retrieved %1)").arg(utc(m->fed.observed_at)) : QString{});
    meta << tr("Polymarket %1%2")
                .arg(m->poly.mapping_status == QLatin1String("VALIDATED") || !m->poly.outcomes.isEmpty()
                         ? state_label(m->poly.state)
                         : tr("no validated market"),
                     m->poly.observed_at.isValid() ? tr(" (latest point %1)").arg(utc(m->poly.observed_at))
                                                   : QString{});
    focus_meta_->setText(meta.join("  ·  "));

    bands_->set_meeting(*m, workspace_.target_lower);
    QStringList band_notes;
    if (m->fed.expected) {
        QString line = tr("Expected rate after this meeting: %1").arg(rate(m->fed.expected, 3));
        QStringList deltas;
        if (m->fed.expected_previous_day)
            deltas << tr("%1 vs previous day").arg(bp_change(*m->fed.expected - *m->fed.expected_previous_day));
        if (m->fed.expected_previous_week)
            deltas << tr("%1 vs previous week").arg(bp_change(*m->fed.expected - *m->fed.expected_previous_week));
        if (!deltas.isEmpty())
            line += " (" + deltas.join(", ") + ")";
        band_notes << line;
    }
    if (m->fed.futures_price)
        band_notes << tr("CME 30-Day Fed Funds futures price shown by Investing.com for this meeting: %1 → implied "
                         "average rate %2 (100 − price; contract-month average, not the post-meeting target)")
                          .arg(QString::number(*m->fed.futures_price, 'f', 3), rate(m->fed.futures_rate, 3));
    if (m->fed.previous_status == QLatin1String("MISMATCH"))
        band_notes << tr("Investing's previous-day/week table did not repeat the displayed bars exactly, so its "
                         "previous values are withheld.");
    if (m->fed.copy_conflict)
        band_notes << tr("Investing page copies disagreed for this meeting; the first intact distribution is shown.");
    if (m->fed.state == QLatin1String("STALE"))
        band_notes << tr("Stale: last stored distribution, retrieved %1. Press REFRESH for current values.")
                          .arg(utc(m->fed.observed_at));
    band_note_->setText(band_notes.join("\n"));

    outcomes_->set_meeting(*m);
    QStringList outcome_notes;
    if (m->fed.local.isEmpty() && !m->fed.bands.isEmpty())
        outcome_notes << tr("Fed-side local step unavailable for this meeting (%1); the target-range distribution "
                            "on the left is unaffected.")
                             .arg(m->fed.local_status.isEmpty() ? tr("not stored") : m->fed.local_status);
    if (m->fed.unverified)
        outcome_notes << tr("First local step uses an older FRED target pair (target_range_unverified).");
    if (m->comparison.isEmpty() && !m->fed.local.isEmpty() && !m->poly.outcomes.isEmpty())
        outcome_notes << tr("Poly − Fed is shown only when both sources are current.");
    outcome_notes << tr("Fed-side local step is binary (at most two adjacent outcomes); Polymarket prices "
                        "broader tails. Gaps are descriptive, not trading signals.");
    outcome_note_->setText(outcome_notes.join("\n"));
    render_history(*m);
    render_changes(*m);
}

void FedWatchPanel::render_history(const Meeting& m) {
    // Shared date domain across both sources so columns line up.
    QDate first, last;
    const auto extend = [&](const QDate& d) {
        if (!d.isValid())
            return;
        if (!first.isValid() || d < first)
            first = d;
        if (!last.isValid() || d > last)
            last = d;
    };
    for (const auto& d : m.fed_days)
        extend(d.date);
    for (const auto& d : m.poly_days)
        extend(d.date);
    // Fed-side bands bottom (lowest rate) to top.
    QMap<double, QString> band_order;
    for (const auto& d : m.fed_days)
        for (const auto& b : d.bands)
            band_order.insert(b.low, band_label(b.low, b.high));
    QVector<QPair<QString, QString>> fed_order;
    for (auto it = band_order.begin(); it != band_order.end(); ++it)
        fed_order.push_back({QString::number(it.key(), 'f', 2), it.value()});
    QVector<FedWatchHeatStrip::Day> fed_days;
    for (const auto& d : m.fed_days) {
        FedWatchHeatStrip::Day day{
            d.date, {}, tr("Expected %1 · retrieved %2").arg(rate(d.expected, 3), utc(d.observed_at))};
        for (const auto& b : d.bands)
            day.segments.push_back({QString::number(b.low, 'f', 2), band_label(b.low, b.high), b.pct});
        fed_days.push_back(day);
    }
    fed_mix_->set_data(fed_days, fed_order, first, last, tr("No stored Fed-side observations for this meeting yet"));
    fed_mix_hint_->setText(
        m.fed_days.isEmpty()
            ? tr("MarketLab archives one Fed-side observation per refresh; none is stored for this meeting.")
            : tr("%1 day(s) stored (%2 → %3). MarketLab's Fed-side archive grows with each refresh.")
                  .arg(m.fed_days.size())
                  .arg(m.fed_days.first().date.toString(Qt::ISODate), m.fed_days.last().date.toString(Qt::ISODate)));

    QMap<QPair<int, bool>, QString> outcome_order;
    for (const auto& d : m.poly_days)
        for (const auto& o : d.outcomes)
            outcome_order.insert({o.bp, o.open}, outcome_label(o.bp, o.open));
    QVector<QPair<QString, QString>> poly_order;
    for (auto it = outcome_order.begin(); it != outcome_order.end(); ++it)
        poly_order.push_back({outcome_key(it.key().first, it.key().second), it.value()});
    QVector<FedWatchHeatStrip::Day> poly_days;
    for (const auto& d : m.poly_days) {
        FedWatchHeatStrip::Day day{d.date, {}, d.complete ? QString{} : tr("Some outcomes have no point this day")};
        for (const auto& o : d.outcomes)
            day.segments.push_back({outcome_key(o.bp, o.open), outcome_label(o.bp, o.open), o.pct});
        poly_days.push_back(day);
    }
    poly_mix_->set_data(poly_days, poly_order, first, last,
                        m.poly.mapping_status == QLatin1String("VALIDATED")
                            ? tr("No Polymarket history stored yet; REFRESH loads available daily history")
                            : tr("No validated Polymarket market for this meeting"));
    poly_mix_hint_->setText(m.poly_days.isEmpty()
                                ? QString{}
                                : tr("%1 day(s) of daily CLOB prices (%2 → %3)%4")
                                      .arg(m.poly_days.size())
                                      .arg(m.poly_days.first().date.toString(Qt::ISODate),
                                           m.poly_days.last().date.toString(Qt::ISODate),
                                           m.poly.event_title.isEmpty() ? QString{} : " · " + m.poly.event_title));
}

void FedWatchPanel::render_changes(const Meeting& m) {
    changes_->setRowCount(m.changes.size());
    int row = 0;
    for (const auto& c : m.changes) {
        set_cell(changes_, row, 0, outcome_label(c.bp, c.open), Qt::AlignLeft);
        set_cell(changes_, row, 1, pct(c.fed.latest));
        set_cell(changes_, row, 2, lookback(c.fed.d1, c.fed.ref1, c.fed.latest_at, 1));
        set_cell(changes_, row, 3, lookback(c.fed.d7, c.fed.ref7, c.fed.latest_at, 7));
        set_cell(changes_, row, 4, lookback(c.fed.d30, c.fed.ref30, c.fed.latest_at, 30));
        set_cell(changes_, row, 5, pct(c.poly.latest));
        set_cell(changes_, row, 6, lookback(c.poly.d1, c.poly.ref1, c.poly.latest_at, 1));
        set_cell(changes_, row, 7, lookback(c.poly.d7, c.poly.ref7, c.poly.latest_at, 7));
        set_cell(changes_, row, 8, lookback(c.poly.d30, c.poly.ref30, c.poly.latest_at, 30));
        set_cell(changes_, row, 9,
                 c.diff ? pp(c.diff) : (c.diff_state.isEmpty() ? QStringLiteral("—") : c.diff_state.toLower()));
        set_cell(changes_, row, 10, tr("%1 / %2").arg(c.fed.count).arg(c.poly.count));
        ++row;
    }
    if (m.changes.isEmpty()) {
        changes_->setRowCount(1);
        set_cell(changes_, 0, 0, tr("No stored outcome history for this meeting"), Qt::AlignLeft);
    }
    fit_height(changes_);
}

void FedWatchPanel::render_sources() {
    // Latest entry per provider from the retained acquisitions.
    QMap<QString, QJsonObject> latest;
    for (const auto& item : workspace_.sources) {
        const auto s = item.toObject();
        const QString provider = s["provider"].toString();
        if (!latest.contains(provider) || s["retrieved_at"].toString() > latest[provider]["retrieved_at"].toString())
            latest[provider] = s;
    }
    const Meeting* next = workspace_.find(workspace_.next_meeting);
    struct Row {
        QString source, provides, status, source_time, retrieved, notes;
    };
    QVector<Row> rows;
    const auto investing = latest.value("investing");
    rows.push_back(
        {tr("Investing.com Fed Rate Monitor"),
         tr("Target-range probabilities for each upcoming meeting, previous day/week values and the futures "
            "price — computed by Investing from CME 30-Day Fed Funds futures"),
         status_word(investing["status"].toString()) +
             (next && !next->fed.freshness_status.isEmpty() ? " · " + next->fed.freshness_status.toLower() : QString{}),
         next ? utc(next->fed.source_updated_at) : QStringLiteral("—"), utc(instant(investing["retrieved_at"])),
         tr("Rounded display values, normalized to 100%; Fed-side local step derived with the CME "
            "method")});
    const auto fred = latest.value("fred");
    rows.push_back(
        {tr("FRED DFEDTARU / DFEDTARL"), tr("Current federal funds target range"),
         status_word(fred["status"].toString()), fred["latest_observation_date"].toString(QStringLiteral("—")),
         utc(instant(fred["retrieved_at"])),
         fred["carried_forward"].toBool() ? tr("Carried forward: no FOMC decision since the latest pair") : QString{}});
    const auto calendar = latest.value("fomc_calendar");
    rows.push_back({tr("Federal Reserve FOMC calendar"), tr("Meeting dates and projection meetings"),
                    status_word(calendar["status"].toString()), QStringLiteral("—"),
                    utc(instant(calendar["retrieved_at"])),
                    calendar["coverage_complete"].toBool(true) ? tr("Complete coverage") : tr("Partial coverage")});
    const auto poly = latest.value("polymarket");
    int validated = 0;
    for (const auto& m : workspace_.meetings)
        if (m.poly.mapping_status == QLatin1String("VALIDATED"))
            ++validated;
    rows.push_back(
        {tr("Polymarket (Gamma + CLOB, public read-only)"),
         tr("Decision-market prices for validated FOMC events, current and daily history"),
         status_word(poly["status"].toString()), next ? utc(next->poly.observed_at) : QStringLiteral("—"),
         utc(instant(poly["retrieved_at"])),
         tr("%1 meeting(s) with a validated market; unmatched meetings have no listed market").arg(validated)});
    rows.push_back({tr("CME FedWatch tool"), tr("CME's own published probabilities"), tr("Not collected"),
                    QStringLiteral("—"), QStringLiteral("—"),
                    tr("CME terms prohibit automated access to cmegroup.com data; the FedWatch API is paid. Use "
                       "\"CME FEDWATCH ↗\" to view it in a browser.")});
    sources_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        set_cell(sources_, i, 0, rows[i].source, Qt::AlignLeft);
        set_cell(sources_, i, 1, rows[i].provides, Qt::AlignLeft);
        set_cell(sources_, i, 2, rows[i].status, Qt::AlignLeft);
        set_cell(sources_, i, 3, rows[i].source_time, Qt::AlignLeft);
        set_cell(sources_, i, 4, rows[i].retrieved, Qt::AlignLeft);
        set_cell(sources_, i, 5, rows[i].notes, Qt::AlignLeft);
    }
    auto* header = sources_->horizontalHeader();
    const int widths[] = {250, 400, 130, 170, 170};
    for (int column = 0; column < 5; ++column) {
        header->setSectionResizeMode(column, QHeaderView::Interactive);
        sources_->setColumnWidth(column, widths[column]);
    }
    header->setSectionResizeMode(5, QHeaderView::Stretch);
    fit_height(sources_);

    QStringList diagnostics;
    const QString errors = error_lines(workspace_.errors, 12);
    if (!errors.isEmpty())
        diagnostics << tr("Provider issues retained with the stored refresh:\n") + errors;
    QStringList warnings;
    for (const auto& w : workspace_.warnings)
        if (!warnings.contains(w.toString()))
            warnings << w.toString();
    if (!warnings.isEmpty())
        diagnostics << tr("Warnings:\n") + warnings.mid(0, 8).join("\n");
    if (diagnostics.isEmpty())
        diagnostics << tr("No provider errors or warnings retained with the stored refresh.");
    diagnostics_->setText(diagnostics.join("\n\n"));
    QStringList notes;
    for (const auto& n : workspace_.notes)
        notes << QString::fromUtf8("• ") + n.toString();
    if (!workspace_.cme_note.isEmpty())
        notes << QString::fromUtf8("• ") + workspace_.cme_note;
    notes_->setText(notes.join("\n"));
}

void FedWatchPanel::arrange() {
    if (!focus_layout_ || !kpi_layout_)
        return;
    const bool wide = width() >= 1300;
    focus_layout_->removeWidget(band_card_);
    focus_layout_->removeWidget(outcome_card_);
    focus_layout_->addWidget(band_card_, 0, 0);
    focus_layout_->addWidget(outcome_card_, wide ? 0 : 1, wide ? 1 : 0);
    focus_layout_->setColumnStretch(0, 1);
    focus_layout_->setColumnStretch(1, wide ? 1 : 0);
    const int columns = width() >= 1300 ? 5 : width() >= 760 ? 3 : 2;
    for (const auto& kpi : kpis_)
        kpi_layout_->removeWidget(kpi.frame);
    for (int i = 0; i < kpis_.size(); ++i)
        kpi_layout_->addWidget(kpis_[i].frame, i / columns, i % columns);
    for (int c = 0; c < 5; ++c)
        kpi_layout_->setColumnStretch(c, c < columns ? 1 : 0);
}

void FedWatchPanel::resizeEvent(QResizeEvent* event) {
    EconPanelBase::resizeEvent(event);
    arrange();
}

QVariantMap FedWatchPanel::save_panel_state() const {
    return {{"meeting", selected_}, {"audit_visible", audit_toggle_ && audit_toggle_->isChecked()}};
}

void FedWatchPanel::restore_panel_state(const QVariantMap& state) {
    restored_selection_ = state.value("meeting").toString();
    if (audit_toggle_)
        audit_toggle_->setChecked(state.value("audit_visible").toBool());
    if (workspace_.loaded)
        render();
}

void FedWatchPanel::refresh_panel_theme() {
    using namespace ui::colors;
    color_ = AMBER();
    EconPanelBase::refresh_panel_theme();
    setStyleSheet(
        panel_style() +
        QString("#fedwatchContent { background:%1; }"
                "QFrame[fedwatchCard=\"true\"] { background:%2; border:1px solid %3; border-radius:3px; }"
                "QFrame[kpi=\"true\"] { background:%4; }"
                "QLabel { color:%5; font-size:11px; background:transparent; }"
                "QLabel#fedwatchTitle { color:%6; font-size:12px; font-weight:700; letter-spacing:1px; }"
                "QLabel#fedwatchAsOf { color:%7; font-size:10px; }"
                "QLabel#fedwatchKpiCaption { color:%7; font-size:9px; font-weight:700; letter-spacing:1px; }"
                "QLabel[kpiValue=\"true\"] { color:%8; font-size:17px; font-weight:700; }"
                "QLabel#fedwatchKpiSub { color:%7; font-size:10px; }"
                "QLabel[fedwatchSection=\"true\"] { color:%6; font-size:11px; font-weight:700; letter-spacing:1px; }"
                "QLabel[fedwatchHint=\"true\"] { color:%7; font-size:10px; }"
                "QLabel#fedwatchFocusTitle { color:%8; font-size:15px; font-weight:700; letter-spacing:1px; }"
                "QLabel#fedwatchFocusMeta, QLabel#fedwatchBandNote, QLabel#fedwatchOutcomeNote, "
                "QLabel#fedwatchDiagnostics, QLabel#fedwatchMethodNotes { color:%7; font-size:10px; }"
                "QLabel#fedwatchStatus { color:%8; font-size:11px; background:%4; border:1px solid %3;"
                " border-left:3px solid %6; padding:6px 10px; }"
                "QPushButton#fedwatchOpenCme { background:transparent; color:%7; border:1px solid %3;"
                " font-size:10px; font-weight:700; padding:4px 10px; }"
                "QPushButton#fedwatchOpenCme:hover { color:%8; background:%9; border-color:%10; }"
                "QPushButton[meetingChip=\"true\"] { background:%4; color:%7; border:1px solid %3;"
                " border-radius:3px; font-size:10px; padding:4px 10px; }"
                "QPushButton[meetingChip=\"true\"]:hover { color:%8; background:%9; border-color:%10; }"
                "QPushButton[meetingChip=\"true\"]:checked { background:%6; color:%11; border-color:%6;"
                " font-weight:700; }"
                "QToolButton#fedwatchAuditToggle { background:transparent; color:%7; border:1px solid %3;"
                " font-size:10px; padding:3px 8px; }"
                "QToolButton#fedwatchAuditToggle:hover { color:%8; background:%9; }"
                "QPlainTextEdit#fedwatchAudit { background:%1; color:%5; border:1px solid %3; font-size:10px; }"
                "QScrollArea#fedwatchMatrixScroll, QScrollArea#fedwatchMeetingChipScroll { background:transparent; }"
                "QScrollArea#fedwatchMatrixScroll > QWidget > QWidget, "
                "QScrollArea#fedwatchMeetingChipScroll > QWidget > QWidget { background:transparent; }")
            .arg(BG_BASE(), BG_SURFACE(), BORDER_DIM(), BG_RAISED(), TEXT_PRIMARY(), AMBER(), TEXT_SECONDARY(),
                 TEXT_PRIMARY(), BG_HOVER())
            .arg(BORDER_BRIGHT(), TEXT_ON_ACCENT()));
    for (QWidget* chart : std::initializer_list<QWidget*>{matrix_, path_, bands_, outcomes_, fed_mix_, poly_mix_})
        if (chart)
            chart->update();
}

} // namespace fincept::screens
