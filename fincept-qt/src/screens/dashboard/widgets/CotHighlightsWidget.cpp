#include "screens/dashboard/widgets/CotHighlightsWidget.h"

#include "screens/economics/panels/CftcInterpretationPresentation.h"
#include "screens/economics/panels/CftcResearchModel.h"
#include "services/ResearchDataEvents.h"
#include "services/economics/CftcMetricModel.h"
#include "services/economics/CftcMonitorModel.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QLocale>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fincept::screens::widgets {

namespace {

// The dashboard summarises one report family: Legacy, futures only — the
// Economics → CFTC screen's default, covering every market it lists.
constexpr services::CftcFamily kCotFamily = services::CftcFamily::Legacy;
constexpr bool kCotFuturesOnly = true;
constexpr const char* kCotMaxRows = "25000"; // the CFTC screen's overview read

QString cot_names(const QStringList& list, int limit = 4) {
    if (list.isEmpty())
        return QStringLiteral("—");
    QString text = QStringList(list.mid(0, limit)).join(QStringLiteral(", "));
    if (list.size() > limit)
        text += QCoreApplication::translate("CotHighlightsWidget", " and %1 more").arg(list.size() - limit);
    return text;
}

} // namespace

CotHighlightsWidget::CotHighlightsWidget(QWidget* parent)
    : BaseWidget(tr("COT · SPECULATORS' POSITIONING"), parent, ui::colors::WARNING()) {
    body_ = new QWidget(this);
    body_->setObjectName("cotHighlights");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    auto make_label = [this](const QString& object_name) {
        auto* l = new QLabel(body_);
        l->setObjectName(object_name);
        l->setWordWrap(true);
        return l;
    };

    report_ = make_label(QStringLiteral("cotSub"));
    vl->addWidget(report_);

    lean_title_ = make_label(QStringLiteral("cotHead"));
    lean_title_->setText(tr("WHICH WAY SPECULATORS LEAN · NUMBER OF MARKETS"));
    vl->addWidget(lean_title_);
    lean_ = new charts::SplitBar(body_);
    vl->addWidget(lean_);

    extremes_title_ = make_label(QStringLiteral("cotHead"));
    extremes_title_->setText(tr("AT AN EXTREME · NET POSITION, % OF OPEN INTEREST"));
    extremes_title_->setToolTip(tr("Crowded long: net long and in the top 10% of its own history. Crowded short: net "
                                   "short and in the bottom 10%. Hover a bar for the exact rule."));
    vl->addWidget(extremes_title_);
    extremes_ = new charts::BarList(body_);
    extremes_->set_empty_text(tr("No market is at an extreme this week."));
    vl->addWidget(extremes_, 1);

    flipped_ = make_label(QStringLiteral("cotSub"));
    flipped_->setToolTip(tr("Speculators' net position changed side in the newest report"));
    vl->addWidget(flipped_);

    moves_title_ = make_label(QStringLiteral("cotHead"));
    moves_title_->setText(tr("BIGGEST CHANGES THIS WEEK · % OF OPEN INTEREST"));
    moves_title_->setToolTip(tr("Change of the speculators' net position over one week, as % of open interest. "
                                "\"2.0× usual\" = twice the market's typical one-week change."));
    vl->addWidget(moves_title_);
    moves_ = new charts::BarList(body_);
    moves_->set_empty_text(tr("No sized weekly change at the newest report."));
    vl->addWidget(moves_, 1);
    content_layout()->addWidget(body_);

    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &CotHighlightsWidget::on_result);
    // Refresh = update the whole CFTC archive the CFTC screen reads: this
    // widget's report first (Legacy, futures only), then the other report
    // families and bases the screen can show.
    connect(this, &BaseWidget::refresh_requested, this, [this]() {
        refresh_queue_.clear();
        for (const auto family : {services::CftcFamily::Disaggregated, services::CftcFamily::Tff})
            refresh_queue_.append({family, true});
        for (const auto family :
             {services::CftcFamily::Legacy, services::CftcFamily::Disaggregated, services::CftcFamily::Tff})
            refresh_queue_.append({family, false});
        refresh_total_ = int(refresh_queue_.size()) + 1;
        request(true);
    });
    // A refresh on the CFTC screen updated the archive: re-read it here.
    connect(&services::ResearchDataEvents::instance(), &services::ResearchDataEvents::cot_saved, this,
            [this](QObject* origin) {
                if (origin != this && pending_id_.isEmpty() && pending_other_.isEmpty() && loaded_once_)
                    request(false);
            });
    apply_styles();
    set_loading(true);
}

void CotHighlightsWidget::apply_styles() {
    body_->setStyleSheet(QString("#cotHighlights QLabel{background:transparent;font-size:10px;color:%1;}"
                                 "#cotHighlights QLabel#cotSub{color:%2;}"
                                 "#cotHighlights QLabel#cotHead{font-size:9px;font-weight:bold;color:%3;"
                                 "padding-top:8px;letter-spacing:0.5px;}")
                             .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(), ui::colors::WARNING()));
    lean_->update();
    extremes_->update();
    moves_->update();
}

void CotHighlightsWidget::on_theme_changed() {
    apply_styles();
}

void CotHighlightsWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("COT · SPECULATORS' POSITIONING"));
    lean_title_->setText(tr("WHICH WAY SPECULATORS LEAN · NUMBER OF MARKETS"));
    extremes_title_->setText(tr("AT AN EXTREME · NET POSITION, % OF OPEN INTEREST"));
    moves_title_->setText(tr("BIGGEST CHANGES THIS WEEK · % OF OPEN INTEREST"));
    if (!last_data_.isEmpty())
        render(last_data_);
}

void CotHighlightsWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!loaded_once_) {
        loaded_once_ = true;
        request(false); // the stored archive only
    }
}

void CotHighlightsWidget::request(bool refresh) {
    pending_id_ = QStringLiteral("dash-cot:%1:%2").arg(quintptr(this)).arg(++sequence_);
    pending_refresh_ = refresh;
    set_loading(true);
    services::EconomicsService::instance().execute("cftc", "cftc_data.py", QStringLiteral("cot_monitor"),
                                                   {services::cftc_family_code(kCotFamily),
                                                    kCotFuturesOnly ? QStringLiteral("true") : QStringLiteral("false"),
                                                    QStringLiteral("all"), QString::fromLatin1(kCotMaxRows),
                                                    refresh ? QStringLiteral("true") : QStringLiteral("false")},
                                                   pending_id_, /*bypass_cache=*/true);
}

void CotHighlightsWidget::refresh_next_report() {
    if (refresh_queue_.isEmpty()) {
        other_status_.clear();
        emit services::ResearchDataEvents::instance().cot_saved(this);
        if (!last_data_.isEmpty())
            render(last_data_);
        return;
    }
    const auto [family, futures_only] = refresh_queue_.takeFirst();
    pending_other_ = QStringLiteral("dash-cot-all:%1:%2").arg(quintptr(this)).arg(++sequence_);
    other_status_ = tr("updating the other CFTC reports %1/%2…")
                        .arg(refresh_total_ - int(refresh_queue_.size()))
                        .arg(refresh_total_);
    services::EconomicsService::instance().execute(
        "cftc", "cftc_data.py", QStringLiteral("cot_monitor"),
        {services::cftc_family_code(family), futures_only ? QStringLiteral("true") : QStringLiteral("false"),
         QStringLiteral("all"), QString::fromLatin1(kCotMaxRows), QStringLiteral("true")},
        pending_other_, /*bypass_cache=*/true);
    if (!last_data_.isEmpty())
        render(last_data_);
}

void CotHighlightsWidget::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (!pending_other_.isEmpty() && request_id == pending_other_) {
        pending_other_.clear();
        if (!result.success)
            other_failures_ << result.error.left(80);
        refresh_next_report();
        return;
    }
    if (request_id != pending_id_)
        return;
    pending_id_.clear();
    set_loading(false);
    if (!result.success) {
        set_footer(pending_refresh_ ? tr("The CFTC could not be reached: %1").arg(result.error.left(120))
                                    : tr("The stored reports could not be opened: %1").arg(result.error.left(120)),
                   result.error, true);
        return;
    }
    last_data_ = result.data;
    render(result.data);
    if (pending_refresh_) {
        // This report is current; tell the CFTC screen, then do the others.
        emit services::ResearchDataEvents::instance().cot_saved(this);
        other_failures_.clear();
        refresh_next_report();
    }
}

void CotHighlightsWidget::render(const QJsonObject& envelope) {
    const QJsonObject data = envelope.value(QStringLiteral("data")).toObject();
    const QString principal = cftc_principal_participant_key(kCotFamily);
    const services::CftcMonitorModel model = services::cftc_parse_monitor_payload(
        data, kCotFamily, kCotFuturesOnly, QDateTime::currentDateTimeUtc().date(), principal);
    if (!model.error.isEmpty()) {
        set_footer(model.error, QString(), true);
        return;
    }
    const QVector<CftcMarketRow> rows = cftc_market_rows(model.entries);
    const CftcOverviewFigures f = cftc_overview_figures(rows);
    if (!f.latest_report.isValid()) {
        report_->setText(tr("No stored reports yet."));
        lean_->set_parts({});
        extremes_->set_rows({}, true);
        moves_->set_rows({}, true);
        set_footer(tr("Press refresh (or open Economics → CFTC) to download the CFTC history."), QString(), true);
        return;
    }

    const QString group = cftc_group_name(kCotFamily, principal);
    report_->setText(tr("Speculators (%1) · report of %2 · %3 markets")
                         .arg(group, QLocale::c().toString(f.latest_report, QStringLiteral("d MMM yyyy")))
                         .arg(f.markets_listed));

    lean_->set_parts(
        {{tr("LONG"), int(f.leaning_long.size()), &ui::colors::POSITIVE, cot_names(f.leaning_long, 40)},
         {tr("NEUTRAL"), int(f.neutral.size()), &ui::colors::TEXT_TERTIARY, cot_names(f.neutral, 40)},
         {tr("SHORT"), int(f.leaning_short.size()), &ui::colors::NEGATIVE, cot_names(f.leaning_short, 40)}});

    // Markets at an extreme: crowded long (highest percentile first), then
    // crowded short (lowest first). The bar is the net position, % of OI.
    QVector<const CftcMarketRow*> crowded_long, crowded_short;
    for (const auto& row : rows) {
        if (!row.has_reading)
            continue;
        if (row.crowding == CftcCrowding::Long)
            crowded_long.append(&row);
        else if (row.crowding == CftcCrowding::Short)
            crowded_short.append(&row);
    }
    auto by_pct = [](bool high_first) {
        return [high_first](const CftcMarketRow* a, const CftcMarketRow* b) {
            const double pa = a->percentile.value_or(0.5), pb = b->percentile.value_or(0.5);
            return high_first ? pa > pb : pa < pb;
        };
    };
    std::sort(crowded_long.begin(), crowded_long.end(), by_pct(true));
    std::sort(crowded_short.begin(), crowded_short.end(), by_pct(false));
    QVector<charts::BarRow> extremes;
    // The percentile is a 0..1 rank within the market's own history window.
    auto rank_note = [this](const std::optional<double>& pct) {
        if (!pct)
            return QString();
        if (*pct >= 1.0)
            return tr("highest");
        if (*pct <= 0.0)
            return tr("lowest");
        return tr("pctl %1").arg(std::clamp(std::lround(*pct * 100.0), 1L, 99L));
    };
    auto add_extreme = [&](const CftcMarketRow* row) {
        extremes.append({row->label, cftc_signed_pct(row->net_pct_oi), rank_note(row->percentile), row->net_pct_oi,
                         row->crowding_reason});
    };
    for (int i = 0; i < crowded_long.size() && i < 4; ++i)
        add_extreme(crowded_long[i]);
    for (int i = 0; i < crowded_short.size() && i < 4; ++i)
        add_extreme(crowded_short[i]);
    extremes_->set_rows(extremes, true);
    const int more = std::max(0, int(crowded_long.size()) - 4) + std::max(0, int(crowded_short.size()) - 4);
    extremes_title_->setText(more > 0 ? tr("AT AN EXTREME · NET POSITION, % OF OPEN INTEREST · +%1 MORE").arg(more)
                                      : tr("AT AN EXTREME · NET POSITION, % OF OPEN INTEREST"));

    flipped_->setText(tr("Changed side this week: %1").arg(cot_names(f.flipped)));

    const CftcMoveRanking ranking = cftc_weekly_move_ranking(rows, 3);
    QVector<charts::BarRow> moves;
    for (const auto* list : {&ranking.rises, &ranking.falls})
        for (const auto& bar : *list)
            moves.append({bar.label, cftc_signed_pct(bar.net_flow),
                          bar.has_scale ? tr("%1× usual").arg(bar.scale, 0, 'f', 1) : QString(), bar.net_flow,
                          bar.tooltip});
    moves_->set_rows(moves, true);

    QString footer = tr("Legacy report, futures only · stored archive");
    bool warn = false;
    if (f.markets_behind > 0) {
        footer += tr(" · %1 market(s) a week or more behind").arg(f.markets_behind);
        warn = true;
    }
    if (f.markets_unavailable > 0) {
        footer += tr(" · %1 unavailable").arg(f.markets_unavailable);
        warn = true;
    }
    if (!other_status_.isEmpty())
        footer += QStringLiteral(" · ") + other_status_;
    else if (!other_failures_.isEmpty()) {
        footer += tr(" · %1 other report(s) could not be refreshed").arg(other_failures_.size());
        warn = true;
    }
    set_footer(footer,
               tr("Same archive and rules as Economics → CFTC. Behind: %1").arg(cot_names(f.behind, 10)) +
                   (other_failures_.isEmpty() ? QString() : QStringLiteral("\n") + other_failures_.join("\n")),
               warn);
}

} // namespace fincept::screens::widgets
