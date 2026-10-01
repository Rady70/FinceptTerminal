#include "screens/etf/EtfFlowsScreen.h"

#include "screens/etf/EtfMonthlyChart.h"
#include "screens/etf/EtfPresentation.h"
#include "screens/etf/EtfResearchLoader.h"
#include "ui/tables/DataTable.h"
#include "ui/theme/ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeEdit>
#include <QFile>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace fincept::screens {
namespace {
constexpr int kJsonRole = Qt::UserRole;

QLabel* text_label(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QTableWidget* table(const QString& name, const QStringList& headers, QWidget* parent) {
    auto* t = new ui::DataTable(parent);
    t->setColumnCount(headers.size());
    t->setObjectName(name);
    t->setAccessibleName(name);
    t->setHorizontalHeaderLabels(headers);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setAlternatingRowColors(true);
    t->verticalHeader()->hide();
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    t->horizontalHeader()->setDefaultSectionSize(180);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setSortingEnabled(true);
    return t;
}

void cell(QTableWidget* t, int row, int col, const QString& text, const QJsonObject& detail = {}) {
    auto* item = new QTableWidgetItem(text);
    item->setToolTip(text);
    if (!detail.isEmpty())
        item->setData(kJsonRole, detail);
    t->setItem(row, col, item);
}

QJsonObject selected_row(QTableWidget* t) {
    const auto* item = t->item(t->currentRow(), 0);
    return item ? item->data(kJsonRole).toJsonObject() : QJsonObject{};
}

void numeric_cell(QTableWidget* t, int row, int col, const QJsonValue& value, const QString& units) {
    cell(t, row, col, etf_ui::number(value, units));
    static_cast<ui::DataTable*>(t)->set_cell_numeric(row, col, value.toDouble(), value.isDouble());
}

QString timing(const QJsonObject& v) {
    const auto available = v.value("available_from");
    return (available.isString() ? available.toString() : QStringLiteral("Availability unavailable")) +
           QStringLiteral(" · ") + etf_ui::label(v.value("point_in_time_status").toString());
}
} // namespace

EtfFlowsScreen::EtfFlowsScreen(QWidget* parent) : QWidget(parent) {
    setObjectName("etfFlowsScreen");
    QFile resource(QStringLiteral(":/etf/taxonomy_v2.json"));
    if (resource.open(QIODevice::ReadOnly))
        taxonomy_ = services::etf::TaxonomySnapshot::load(resource.readAll(), &taxonomy_error_);
    else
        taxonomy_error_ = tr("Bundled ETF taxonomy cannot be opened");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->addWidget(text_label(tr("ETF CAPITAL FLOWS  ·  stored research"), this));
    root->addWidget(text_label(
        tr("SEC N-PORT: delayed monthly creation/redemption activity, partial coverage. Market rotation: separate "
           "IBKR session components, never dollar fund flow. Daily creation/redemption flow is unavailable."),
        this));

    controls_ = new QWidget(this);
    auto* grid = new QGridLayout(controls_);
    grid->setContentsMargins(0, 0, 0, 0);
    level_ = new QComboBox(controls_);
    level_->setObjectName("etfLevel");
    level_->addItem(tr("Cross-asset"), "cross_asset");
    level_->addItem(tr("Asset class"), "asset_class");
    level_->addItem(tr("Category"), "category");
    level_->addItem(tr("ETF complex"), "complex");
    group_ = new QComboBox(controls_);
    group_->setObjectName("etfGroup");
    const QDate today = QDateTime::currentDateTimeUtc().date();
    const QDate last_month = QDate(today.year(), today.month(), 1).addDays(-1);
    from_ = new QDateEdit(last_month.addMonths(-5), controls_);
    from_->setObjectName("etfFrom");
    to_ = new QDateEdit(last_month, controls_);
    to_->setObjectName("etfTo");
    for (auto* date : {from_, to_}) {
        date->setDisplayFormat("yyyy-MM");
        date->setDateRange(QDate(2019, 1, 1), QDate(2100, 12, 31));
    }
    as_of_ = new QDateTimeEdit(QDateTime::currentDateTimeUtc(), controls_);
    as_of_->setObjectName("etfAsOf");
    known_at_ = new QDateTimeEdit(as_of_->dateTime(), controls_);
    known_at_->setObjectName("etfKnownAt");
    for (auto* cutoff : {as_of_, known_at_}) {
        cutoff->setTimeZone(QTimeZone::UTC);
        cutoff->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
        cutoff->setMinimumDate(QDate(2019, 1, 1));
        cutoff->setToolTip(
            tr("Explicit UTC replay cutoff. As of controls availability; known at controls recorded knowledge."));
    }
    leveraged_ = new QCheckBox(tr("Include leveraged / inverse"), controls_);
    leveraged_->setObjectName("etfLeveraged");
    leveraged_->setToolTip(
        tr("Explicit backend policy. Signed daily targets remain distinct from ordinary long exposure."));
    recompute_ = new QPushButton(tr("Recompute from store"), controls_);
    recompute_->setObjectName("etfRecompute");
    const auto control = [grid](int row, int col, const QString& title, QWidget* widget) {
        auto* label = new QLabel(title);
        label->setBuddy(widget);
        widget->setAccessibleName(title);
        grid->addWidget(label, row, col);
        grid->addWidget(widget, row, col + 1);
    };
    control(0, 0, tr("Hierarchy"), level_);
    control(0, 2, tr("Group"), group_);
    control(1, 0, tr("First month"), from_);
    control(1, 2, tr("Last month"), to_);
    control(2, 0, tr("As of (UTC)"), as_of_);
    control(2, 2, tr("Known at (UTC)"), known_at_);
    grid->addWidget(leveraged_, 3, 0, 1, 2);
    grid->addWidget(recompute_, 3, 2, 1, 2);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    root->addWidget(controls_);
    status_ = text_label({}, this);
    status_->setObjectName("etfStatus");
    root->addWidget(status_);
    context_ = text_label({}, this);
    context_->setObjectName("etfContext");
    root->addWidget(context_);

    splitter_ = new QSplitter(Qt::Vertical, this);
    overview_ = table("etfOverview",
                      {tr("Group / last month"), tr("Observed subset (USD)"), tr("Complete group (USD)"),
                       tr("Quality / revisions"), tr("SEC identity coverage"), tr("Regulatory assets coverage")},
                      splitter_);
    overview_->setMinimumHeight(200);
    auto* detail = new QWidget(splitter_);
    auto* detail_layout = new QVBoxLayout(detail);
    detail_layout->setContentsMargins(0, 0, 0, 0);
    selection_ = text_label(tr("Select a group to inspect its monthly history and exact constituents."), detail);
    selection_->setObjectName("etfSelection");
    detail_layout->addWidget(selection_);
    tabs_ = new QTabWidget(detail);
    tabs_->setObjectName("etfTabs");
    detail_layout->addWidget(tabs_);
    auto* regulatory = new QWidget(tabs_);
    auto* reg_layout = new QVBoxLayout(regulatory);
    month_status_ = text_label({}, regulatory);
    month_status_->setObjectName("etfMonthStatus");
    reg_layout->addWidget(month_status_);
    chart_ = new EtfMonthlyChart(regulatory);
    reg_layout->addWidget(chart_);
    months_ =
        table("etfMonths",
              {tr("Calendar month"), tr("Observed subset (USD)"), tr("Complete group (USD)"), tr("Quality / revisions"),
               tr("SEC identity coverage"), tr("Assets coverage basis"), tr("Available from / timing")},
              regulatory);
    reg_layout->addWidget(months_, 1);
    tabs_->addTab(regulatory, tr("Monthly regulatory flow"));

    auto* rotation_page = new QWidget(tabs_);
    auto* rot_layout = new QVBoxLayout(rotation_page);
    rot_layout->addWidget(text_label(
        tr("Market rotation · latest available exchange session at As of (independent of the monthly range). "
           "Split-adjusted price returns, not total returns. D5 unresolved: no universal cross-asset ranking or group "
           "score. "
           "Volume is self-relative. Select a row for provenance; double-click or press Enter for session history."),
        rotation_page));
    rotation_ =
        table("etfRotation",
              {tr("Listed ETF (conId)"), tr("Status"), tr("Latest session"), tr("Price return 21 sessions"),
               tr("Trend efficiency 21 sessions"), tr("Return acceleration 21 sessions"),
               tr("Volume ratio 5 / 63 sessions"), tr("Classification / comparability"), tr("Structure / target")},
              rotation_page);
    rot_layout->addWidget(rotation_, 1);
    tabs_->addTab(rotation_page, tr("Market rotation"));
    constituents_ =
        table("etfConstituents",
              {tr("Exact subject"), tr("Status"), tr("Monthly net flow (USD)"), tr("Identity / exclusion reason"),
               tr("Asset class / category / complex"), tr("Structure / mechanism / daily target")},
              tabs_);
    constituents_->setToolTip(
        tr("Selected month's exact membership. SEC entities and listed ETFs are separate identities. "
           "Double-click or press Enter to inspect the exact subject."));
    tabs_->addTab(constituents_, tr("Exact constituents"));
    unresolved_ = table("etfUnclassified", {tr("Exact tracked subject"), tr("Classification state")}, tabs_);
    tabs_->addTab(unresolved_, tr("Unclassified / history gaps"));
    auto* individual_page = new QWidget(tabs_);
    auto* ind_layout = new QVBoxLayout(individual_page);
    individual_status_ = text_label(tr("Open an exact constituent to inspect individual research."), individual_page);
    individual_status_->setObjectName("etfIndividualStatus");
    ind_layout->addWidget(individual_status_);
    individual_ = table("etfIndividual",
                        {tr("Month / exchange session"), tr("Component"), tr("Value / units"),
                         tr("Quality / missing reason"), tr("Window / input count"), tr("Available from / timing")},
                        individual_page);
    ind_layout->addWidget(individual_, 1);
    tabs_->addTab(individual_page, tr("Individual research"));
    provenance_ = new QPlainTextEdit(tabs_);
    provenance_->setObjectName("etfProvenance");
    provenance_->setAccessibleName(tr("Research provenance and exact input lineage"));
    provenance_->setReadOnly(true);
    provenance_->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs_->addTab(provenance_, tr("Provenance / versions"));
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 4);
    root->addWidget(splitter_, 1);
    root->addWidget(
        text_label(tr("Research only. Flows describe ETF vehicles, not cash entering referenced physical markets. "
                      "Overlapping ETFs are not independent signals; groups at different levels must not be added. "
                      "Curated taxonomy is not a complete historical universe. No source ingestion or polling."),
                   this));

    connect(recompute_, &QPushButton::clicked, this, &EtfFlowsScreen::recompute);
    connect(level_, &QComboBox::currentIndexChanged, this, [this]() {
        populate_groups();
        invalidate();
    });
    connect(group_, &QComboBox::currentIndexChanged, this, &EtfFlowsScreen::invalidate);
    connect(from_, &QDateEdit::dateChanged, this, &EtfFlowsScreen::invalidate);
    connect(to_, &QDateEdit::dateChanged, this, &EtfFlowsScreen::invalidate);
    connect(as_of_, &QDateTimeEdit::dateTimeChanged, this, &EtfFlowsScreen::invalidate);
    connect(known_at_, &QDateTimeEdit::dateTimeChanged, this, &EtfFlowsScreen::invalidate);
    connect(leveraged_, &QCheckBox::toggled, this, &EtfFlowsScreen::invalidate);
    connect(overview_, &QTableWidget::itemSelectionChanged, this, &EtfFlowsScreen::select_group);
    connect(overview_, &QTableWidget::itemActivated, this, [this]() {
        if (level_->currentData().toString() != QLatin1String("cross_asset"))
            return;
        const QString id = selected_row(overview_).value("group_id").toString();
        if (id.isEmpty())
            return;
        level_->setCurrentIndex(level_->findData("asset_class"));
        group_->setCurrentIndex(group_->findData(id));
        recompute();
    });
    connect(months_, &QTableWidget::itemSelectionChanged, this, &EtfFlowsScreen::select_month);
    for (auto* t : {constituents_, rotation_, unresolved_}) {
        connect(t, &QTableWidget::itemSelectionChanged, this, [this, t]() { show_provenance(selected_row(t)); });
        connect(t, &QTableWidget::itemActivated, this, [this, t]() { inspect_subject(t); });
    }
    connect(individual_, &QTableWidget::itemSelectionChanged, this,
            [this]() { show_provenance(selected_row(individual_)); });
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, &EtfFlowsScreen::refresh_theme);
    populate_groups();
    refresh_theme();
    // Lets DockScreenRouter restore lightweight controls before the first read.
    QTimer::singleShot(0, this, &EtfFlowsScreen::recompute);
}

void EtfFlowsScreen::populate_groups() {
    const QSignalBlocker blocker(group_);
    const QString selected = group_->currentData().toString();
    group_->clear();
    const QString level = level_->currentData().toString();
    if (level == QLatin1String("cross_asset")) {
        group_->addItem(tr("All supported asset classes"), "");
        return;
    }
    group_->addItem(tr("All supported groups"), "");
    if (!taxonomy_)
        return;
    // Enumerate the finalized catalog, including groups with no classification
    // in the requested history. Only Batch D decides actual membership.
    QStringList ids;
    for (const auto& e : taxonomy_->entries()) {
        const QString id = level == QLatin1String("complex")    ? e.complex_id
                           : level == QLatin1String("category") ? e.category
                                                                : e.asset_class;
        if (!id.isEmpty() && !ids.contains(id))
            ids.append(id);
    }
    ids.sort();
    for (const auto& id : ids)
        group_->addItem(etf_ui::label(id), id);
    const int index = group_->findData(selected);
    group_->setCurrentIndex(index < 0 ? 0 : index);
}

void EtfFlowsScreen::clear_results() {
    result_ = {};
    group_result_ = {};
    selected_subject_ = {};
    for (auto* t : {overview_, months_, constituents_, rotation_, unresolved_, individual_}) {
        const QSignalBlocker blocker(t);
        t->setRowCount(0);
    }
    chart_->set_months({});
    context_->clear();
    month_status_->clear();
    provenance_->clear();
    selection_->setText(tr("Select a group to inspect its monthly history and exact constituents."));
    individual_status_->setText(tr("Open an exact constituent to inspect individual research."));
}

void EtfFlowsScreen::invalidate() {
    ++generation_;
    clear_results();
    status_->setText(tr("Controls changed. Recompute from store to apply this exact range and UTC cutoffs."));
}

void EtfFlowsScreen::set_busy(bool busy) {
    busy_ = busy;
    controls_->setEnabled(!busy);
    for (auto* t : {overview_, months_, constituents_, rotation_, unresolved_, individual_})
        t->setEnabled(!busy);
}

void EtfFlowsScreen::recompute() {
    if (busy_)
        return;
    clear_results();
    if (!taxonomy_) {
        status_->setText(tr("ETF research unavailable: %1").arg(taxonomy_error_));
        emit research_loaded(false);
        return;
    }
    services::etf::GroupRunRequest request;
    request.group_level = level_->currentData().toString();
    request.group_id = group_->currentData().toString();
    request.expected_taxonomy_version = taxonomy_->version();
    request.output_from = QDate(from_->date().year(), from_->date().month(), 1);
    request.output_to = QDate(to_->date().year(), to_->date().month(), to_->date().daysInMonth());
    request.frame = {as_of_->dateTime().toUTC(), known_at_->dateTime().toUTC()};
    request.include_leveraged = leveraged_->isChecked();
    if (const QString problem = services::etf::group_request_problem(request); !problem.isEmpty()) {
        status_->setText(tr("Cannot recompute: %1").arg(problem));
        emit research_loaded(false);
        return;
    }
    const quint64 generation = ++generation_;
    set_busy(true);
    status_->setText(tr("Computing stored research… no source requests"));
    watcher_ = new QFutureWatcher<Result<QJsonObject>>(this);
    auto* watcher = watcher_;
    connect(watcher, &QFutureWatcher<Result<QJsonObject>>::finished, this, [this, watcher, request, generation]() {
        const auto response = watcher->result();
        watcher->deleteLater();
        watcher_ = nullptr;
        set_busy(false);
        if (generation != generation_)
            return;
        if (response.is_err()) {
            status_->setText(tr("Stored ETF research failed: %1. Recompute to retry.")
                                 .arg(QString::fromStdString(response.error())));
            emit research_loaded(false);
            return;
        }
        loaded_request_ = request;
        result_ = response.value();
        render_groups();
        emit research_loaded(true);
    });
    watcher->setFuture(QtConcurrent::run([request]() { return etf_ui::load_groups(request); }));
}

QString EtfFlowsScreen::subject_name(const QJsonObject& row) const {
    QString hint;
    if (taxonomy_) {
        const QString key = row.value("stable_key").toString();
        for (const auto& e : taxonomy_->entries()) {
            const bool listed = row.value("subject_type").toString() == QLatin1String("listed_instrument");
            if ((listed && e.subject_type == services::etf::TaxonomySubjectType::Listed &&
                 QString::number(e.con_id) == key) ||
                (!listed && e.subject_type == services::etf::TaxonomySubjectType::Reporting &&
                 e.cik10 + '/' + e.series_id == key)) {
                hint = e.ticker_hint;
                break;
            }
        }
    }
    const QString type =
        row.value("subject_type").toString() == QLatin1String("listed_instrument") ? tr("conId ") : tr("SEC ");
    return (hint.isEmpty() ? QString() : hint + QStringLiteral(" · ")) + type + row.value("stable_key").toString() +
           (hint.isEmpty() ? QString() : tr(" (catalog hint)"));
}

void EtfFlowsScreen::render_groups() {
    context_->setText(tr("Displayed frame · As of %1 · Known at %2 · monthly range %3 to %4 · %5 · %6")
                          .arg(result_.value("as_of").toString(), result_.value("known_at").toString(),
                               result_.value("output_from").toString(), result_.value("output_to").toString(),
                               result_.value("taxonomy_version").toString(), result_.value("method").toString()));
    const auto groups = result_.value("groups").toArray();
    // One-group drill-down should give its space to the research detail; the
    // cross-asset view keeps all six classes visible. Larger catalogs scroll.
    const int summary_height = 42 + 26 * qBound(1, static_cast<int>(groups.size()), 6);
    overview_->setMinimumHeight(summary_height);
    overview_->setMaximumHeight(summary_height);
    overview_->setSortingEnabled(false);
    const QSignalBlocker blocker(overview_);
    overview_->setRowCount(groups.size());
    bool measured = false;
    for (int row = 0; row < groups.size(); ++row) {
        const auto group = groups[row].toObject();
        const auto m = etf_ui::last_month(group);
        measured = measured || m.value("observed_net_flow_usd").isDouble();
        cell(overview_, row, 0,
             etf_ui::label(group.value("group_id").toString()) + " · " +
                 (m.isEmpty() ? tr("No classified monthly history") : m.value("month").toString()),
             group);
        numeric_cell(overview_, row, 1, m.value("observed_net_flow_usd"), "USD");
        numeric_cell(overview_, row, 2, m.value("complete_net_flow_usd"), "USD");
        cell(overview_, row, 3, etf_ui::quality(m));
        cell(overview_, row, 4, etf_ui::coverage(m));
        cell(overview_, row, 5, etf_ui::assets_coverage(m));
    }
    overview_->setSortingEnabled(true);
    overview_->resizeColumnToContents(0);
    status_->setText(groups.isEmpty() ? tr("No supported group exists for this selection.")
                     : measured ? tr("Stored research ready. Observed subsets are not extrapolated to complete groups.")
                                : tr("No measured regulatory flow in the last selected month. Inspect history, "
                                     "rotation and coverage gaps."));
    unresolved_->setSortingEnabled(false);
    unresolved_->setRowCount(0);
    for (const auto& key : {QStringLiteral("unclassified_tracked_at_as_of"),
                            QStringLiteral("classification_history_unverified_at_as_of")}) {
        for (const auto& v : result_.value(key).toArray()) {
            auto s = v.toObject();
            s.insert("classification_state",
                     key.startsWith("unclassified") ? "unclassified" : "classification_history_unverified");
            const int row = unresolved_->rowCount();
            unresolved_->insertRow(row);
            cell(unresolved_, row, 0, subject_name(s), s);
            cell(unresolved_, row, 1,
                 key.startsWith("unclassified") ? tr("Unclassified tracked identity")
                                                : tr("Classification history unavailable"));
        }
    }
    unresolved_->setSortingEnabled(true);
    if (!groups.isEmpty()) {
        overview_->selectRow(0);
        select_group();
    }
}

void EtfFlowsScreen::select_group() {
    group_result_ = selected_row(overview_);
    selected_subject_ = {};
    individual_->setRowCount(0);
    individual_status_->setText(tr("Open an exact constituent to inspect individual research."));
    selection_->setText(tr("%1 / %2 · select a month for membership; activate a constituent for individual research")
                            .arg(etf_ui::label(group_result_.value("level").toString()),
                                 etf_ui::label(group_result_.value("group_id").toString())));
    const auto months = group_result_.value("regulatory_months").toArray();
    chart_->set_months(months);
    {
        const QSignalBlocker blocker(months_);
        months_->setSortingEnabled(false);
        months_->setRowCount(months.size());
        for (int row = 0; row < months.size(); ++row) {
            const auto m = months[row].toObject();
            cell(months_, row, 0, m.value("month").toString(), m);
            numeric_cell(months_, row, 1, m.value("observed_net_flow_usd"), "USD");
            numeric_cell(months_, row, 2, m.value("complete_net_flow_usd"), "USD");
            cell(months_, row, 3, etf_ui::quality(m));
            cell(months_, row, 4, etf_ui::coverage(m));
            cell(months_, row, 5, etf_ui::assets_coverage(m));
            cell(months_, row, 6, timing(m));
        }
        // Chronological history is deliberate; preserve frequency and gaps.
        months_->setSortingEnabled(false);
        if (!months.isEmpty())
            months_->selectRow(months.size() - 1);
    }
    select_month();
    rotation_->setSortingEnabled(false);
    const auto rotation = group_result_.value("rotation_constituents").toArray();
    rotation_->setRowCount(rotation.size());
    for (int row = 0; row < rotation.size(); ++row) {
        const auto r = rotation[row].toObject();
        const auto latest = r.value("latest").toObject();
        const auto values = latest.value("values").toObject();
        cell(rotation_, row, 0, subject_name(r), r);
        cell(rotation_, row, 1,
             etf_ui::label(r.value("status").toString()) + " · " + etf_ui::reason(r.value("reason").toString()));
        cell(rotation_, row, 2, latest.value("latest_session").toString(tr("Unavailable")));
        const QStringList fields{"price_return_21", "trend_efficiency_21", "return_acceleration_21",
                                 "volume_ratio_5_63"};
        for (int col = 0; col < fields.size(); ++col) {
            const auto v = values.value(fields[col]).toObject();
            numeric_cell(rotation_, row, col + 3, v.value("value"), v.value("units").toString());
            rotation_->item(row, col + 3)
                ->setToolTip(etf_ui::label(v.value("state").toString()) + " · " +
                             etf_ui::reason(v.value("reason").toString()) + " · " + timing(v));
        }
        cell(rotation_, row, 7,
             etf_ui::label(r.value("classification_lookback").toString()) + " · " +
                 etf_ui::label(r.value("comparability").toString()));
        cell(rotation_, row, 8,
             etf_ui::label(r.value("fund_structure").toString()) + " · " +
                 QString::number(r.value("leverage_multiple").toDouble()) + "x target");
    }
    rotation_->setSortingEnabled(true);
}

void EtfFlowsScreen::select_month() {
    const auto month = selected_row(months_);
    month_status_->setText(month.isEmpty()
                               ? tr("No regulatory history within the requested taxonomy interval.")
                               : tr("%1 · monthly SEC N-PORT · %2 · %3 · %4. Coverage is not daily AUM coverage.")
                                     .arg(month.value("month").toString(), etf_ui::quality(month),
                                          etf_ui::coverage(month), etf_ui::assets_coverage(month)));
    const auto members = month.value("constituents").toArray();
    constituents_->setSortingEnabled(false);
    constituents_->setRowCount(members.size());
    for (int row = 0; row < members.size(); ++row) {
        const auto m = members[row].toObject();
        const auto taxonomy = m.value("taxonomy").toObject();
        cell(constituents_, row, 0, subject_name(m), m);
        cell(constituents_, row, 1, etf_ui::label(m.value("status").toString()));
        // Only an observed contribution is counted. An excluded/duplicate row
        // may retain input evidence; it must not look like another contribution.
        cell(constituents_, row, 2,
             m.value("status").toString() == QLatin1String("observed")
                 ? etf_ui::component(m.value("net_flow").toObject())
                 : tr("Unavailable / not counted"));
        cell(constituents_, row, 3,
             etf_ui::reason(m.value("reason").toString()) + " · " +
                 etf_ui::label(m.value("identity_basis").toString()));
        cell(constituents_, row, 4,
             etf_ui::label(taxonomy.value("asset_class").toString()) + " / " +
                 etf_ui::label(taxonomy.value("category").toString()) + " / " +
                 (taxonomy.value("complex_id").toString().isEmpty()
                      ? tr("No supported complex")
                      : etf_ui::label(taxonomy.value("complex_id").toString())));
        cell(constituents_, row, 5,
             etf_ui::label(taxonomy.value("fund_structure").toString()) + " / " +
                 etf_ui::label(taxonomy.value("exposure_mechanism").toString()) + " / " +
                 QString::number(taxonomy.value("leverage_multiple").toDouble()) + "x target");
    }
    constituents_->setSortingEnabled(true);
    show_provenance(month);
}

void EtfFlowsScreen::show_provenance(const QJsonObject& object) {
    QJsonObject context = result_;
    context.remove("groups");
    context.insert("selected_record", object);
    if (!selected_subject_.isEmpty())
        context.insert("individual_subject", selected_subject_);
    provenance_->setPlainText(QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Indented)));
}

void EtfFlowsScreen::inspect_subject(QTableWidget* table) {
    if (busy_ || result_.isEmpty())
        return;
    const auto subject = selected_row(table);
    if (subject.isEmpty())
        return;
    // Excluded products remain inspectable as metadata, without circumventing
    // the selected backend inclusion policy for their rotation calculations.
    tabs_->setCurrentIndex(4);
    individual_->setRowCount(0);
    selected_subject_ = subject;
    show_provenance(subject);
    if (!loaded_request_.include_leveraged &&
        (subject.value("leveraged").toBool() || subject.value("inverse").toBool() ||
         subject.value("taxonomy").toObject().value("leveraged").toBool() ||
         subject.value("taxonomy").toObject().value("inverse").toBool())) {
        individual_status_->setText(tr("%1 · excluded by the default leveraged/inverse policy. Metadata is in "
                                       "Provenance; explicitly include to compute.")
                                        .arg(subject_name(subject)));
        return;
    }
    individual_status_->setText(tr("Loading exact subject history from store…"));
    const auto request = loaded_request_;
    const quint64 generation = generation_;
    set_busy(true);
    watcher_ = new QFutureWatcher<Result<QJsonObject>>(this);
    auto* watcher = watcher_;
    connect(watcher, &QFutureWatcher<Result<QJsonObject>>::finished, this, [this, watcher, subject, generation]() {
        const auto response = watcher->result();
        watcher->deleteLater();
        watcher_ = nullptr;
        set_busy(false);
        if (generation != generation_)
            return;
        if (response.is_err()) {
            individual_status_->setText(tr("%1 · individual history unavailable: %2")
                                            .arg(subject_name(subject), QString::fromStdString(response.error())));
            emit research_loaded(false);
            return;
        }
        render_subject(subject, response.value());
        emit research_loaded(true);
    });
    watcher->setFuture(QtConcurrent::run([request, subject]() { return etf_ui::load_subject(request, subject); }));
}

void EtfFlowsScreen::render_subject(const QJsonObject& subject, const QJsonObject& result) {
    const bool listed = subject.value("subject_type").toString() == QLatin1String("listed_instrument");
    individual_status_->setText(
        tr("%1 · %2 · same UTC cutoffs and displayed range · %3")
            .arg(subject_name(subject),
                 listed ? tr("Market rotation, exchange sessions; regulatory attribution only through Batch D")
                        : tr("SEC reporting entity, monthly regulatory flow; no inferred listed-ETF link"),
                 listed ? tr("Price return, not total return; D5 unresolved")
                        : tr("Delayed filing availability and revisions retained")));
    if (subject.contains("classification_state"))
        individual_status_->setText(individual_status_->text() + QStringLiteral(" · ") +
                                    etf_ui::label(subject.value("classification_state").toString()));
    if (subject.contains("classification_lookback"))
        individual_status_->setText(individual_status_->text() + QStringLiteral(" · ") +
                                    etf_ui::label(subject.value("classification_lookback").toString()));
    individual_->setSortingEnabled(false);
    const auto append = [this](const QJsonArray& series, const QString& time_key) {
        for (const auto& entry : series) {
            const auto s = entry.toObject();
            const auto values = s.value("values").toObject();
            for (auto it = values.begin(); it != values.end(); ++it) {
                const auto v = it.value().toObject();
                const int row = individual_->rowCount();
                individual_->insertRow(row);
                cell(individual_, row, 0, s.value(time_key).toString(), s);
                cell(individual_, row, 1,
                     time_key == QLatin1String("session") ? etf_ui::component_label(it.key())
                                                          : etf_ui::label(it.key()));
                cell(individual_, row, 2, etf_ui::component(v));
                cell(individual_, row, 3,
                     etf_ui::label(v.value("state").toString()) + " · " + etf_ui::reason(v.value("reason").toString()));
                cell(individual_, row, 4,
                     v.value("window_first").toString() + " to " + v.value("window_last").toString() + " · " +
                         QString::number(v.value("input_count").toInt()) + tr(" inputs"));
                cell(individual_, row, 5, timing(v));
            }
        }
    };
    for (const auto& v : result.value(listed ? "rotation_proxy" : "regulatory_flow").toArray()) {
        const auto row = v.toObject();
        const auto data = row.value(listed ? "measures" : "analytics").toObject();
        append(data.value(listed ? "sessions" : "months").toArray(), listed ? "session" : "month");
    }
    individual_->setSortingEnabled(true);
    if (individual_->rowCount() == 0)
        individual_status_->setText(individual_status_->text() + tr(" · No available history in this range."));
    show_provenance(QJsonObject{{"subject", subject}, {"individual_research", result}});
}

QVariantMap EtfFlowsScreen::save_state() const {
    return {{"level", level_->currentData()},
            {"group", group_->currentData()},
            {"from", from_->date()},
            {"to", to_->date()},
            {"as_of", as_of_->dateTime()},
            {"known_at", known_at_->dateTime()},
            {"include_leveraged", leveraged_->isChecked()},
            {"tab", tabs_->currentIndex()},
            {"splitter", splitter_->saveState()}};
}

void EtfFlowsScreen::restore_state(const QVariantMap& state) {
    const int level = level_->findData(state.value("level"));
    if (level >= 0)
        level_->setCurrentIndex(level);
    populate_groups();
    const int group = group_->findData(state.value("group"));
    if (group >= 0)
        group_->setCurrentIndex(group);
    if (state.value("from").toDate().isValid())
        from_->setDate(state.value("from").toDate());
    if (state.value("to").toDate().isValid())
        to_->setDate(state.value("to").toDate());
    if (state.value("as_of").toDateTime().isValid())
        as_of_->setDateTime(state.value("as_of").toDateTime().toUTC());
    if (state.value("known_at").toDateTime().isValid())
        known_at_->setDateTime(state.value("known_at").toDateTime().toUTC());
    leveraged_->setChecked(state.value("include_leveraged", false).toBool());
    const int tab = state.value("tab", 0).toInt();
    if (tab >= 0 && tab < tabs_->count())
        tabs_->setCurrentIndex(tab);
    if (state.contains("splitter"))
        splitter_->restoreState(state.value("splitter").toByteArray());
}

void EtfFlowsScreen::refresh_theme() {
    const auto& t = ui::ThemeManager::instance().tokens();
    setStyleSheet(QStringLiteral(
                      "QWidget#etfFlowsScreen { background: %1; color: %2; } "
                      "QTableWidget { background: %3; alternate-background-color: %4; color: %2; gridline-color: %5; } "
                      "QHeaderView::section { color: %2; } QTabBar::tab { color: %6; } "
                      "QTabBar::tab:selected { color: %2; } QCheckBox, QPushButton { color: %2; } "
                      "QPushButton:disabled, QCheckBox:disabled { color: %6; }")
                      .arg(t.bg_base, t.text_primary, t.bg_surface, t.row_alt, t.border_dim, t.text_secondary));
}
} // namespace fincept::screens
