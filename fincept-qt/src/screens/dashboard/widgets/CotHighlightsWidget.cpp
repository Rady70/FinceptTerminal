#include "screens/dashboard/widgets/CotHighlightsWidget.h"

#include "screens/economics/panels/CftcInterpretationPresentation.h"
#include "screens/economics/panels/CftcResearchModel.h"
#include "services/economics/CftcMetricModel.h"
#include "services/economics/CftcMonitorModel.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QLocale>
#include <QShowEvent>
#include <QVBoxLayout>

namespace fincept::screens::widgets {

namespace {

// The dashboard summarises one report family: Legacy, futures only — the
// Economics → CFTC screen's default, covering every market it lists.
constexpr services::CftcFamily kCotFamily = services::CftcFamily::Legacy;
constexpr bool kCotFuturesOnly = true;
constexpr const char* kCotMaxRows = "25000"; // the CFTC screen's overview read

QLabel* cot_make_label(const QString& object_name) {
    auto* l = new QLabel;
    l->setObjectName(object_name);
    l->setWordWrap(true);
    return l;
}

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

    report_ = cot_make_label(QStringLiteral("cotSub"));
    lean_ = cot_make_label(QStringLiteral("cotBig"));
    crowded_long_ = cot_make_label(QString());
    crowded_short_ = cot_make_label(QString());
    flipped_ = cot_make_label(QString());
    for (QLabel* l : {report_, lean_, crowded_long_, crowded_short_, flipped_})
        vl->addWidget(l);
    crowded_long_->setToolTip(tr("Speculators' net position in the top 10% of the last 52 weeks"));
    crowded_short_->setToolTip(tr("Speculators' net position in the bottom 10% of the last 52 weeks"));
    flipped_->setToolTip(tr("Speculators' net position changed side in the newest report"));

    moves_title_ = cot_make_label(QStringLiteral("cotHead"));
    moves_title_->setText(tr("LARGEST WEEKLY CHANGES · % OF OPEN INTEREST"));
    vl->addWidget(moves_title_);
    moves_ = new QGridLayout;
    moves_->setHorizontalSpacing(10);
    moves_->setVerticalSpacing(2);
    vl->addLayout(moves_);
    vl->addStretch(1);
    content_layout()->addWidget(body_);

    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &CotHighlightsWidget::on_result);
    connect(this, &BaseWidget::refresh_requested, this, [this]() { request(true); });
    apply_styles();
    set_loading(true);
}

void CotHighlightsWidget::apply_styles() {
    body_->setStyleSheet(QString("#cotHighlights QLabel{background:transparent;font-size:10px;color:%1;}"
                                 "#cotHighlights QLabel#cotBig{font-size:12px;font-weight:bold;}"
                                 "#cotHighlights QLabel#cotSub{color:%2;}"
                                 "#cotHighlights QLabel#cotHead{font-size:9px;font-weight:bold;color:%3;"
                                 "padding-top:6px;letter-spacing:0.5px;}"
                                 "#cotHighlights QLabel#cotUp{color:%4;font-weight:bold;}"
                                 "#cotHighlights QLabel#cotDown{color:%5;font-weight:bold;}")
                             .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(), ui::colors::TEXT_TERTIARY(),
                                  ui::colors::POSITIVE(), ui::colors::NEGATIVE()));
}

void CotHighlightsWidget::on_theme_changed() {
    apply_styles();
}

void CotHighlightsWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("COT · SPECULATORS' POSITIONING"));
    moves_title_->setText(tr("LARGEST WEEKLY CHANGES · % OF OPEN INTEREST"));
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

void CotHighlightsWidget::on_result(const QString& request_id, const services::EconomicsResult& result) {
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
}

void CotHighlightsWidget::render(const QJsonObject& envelope) {
    const QJsonObject data = envelope.value(QStringLiteral("data")).toObject();
    const QString principal = cftc_principal_participant_key(kCotFamily);
    const services::CftcMonitorModel model = services::cftc_parse_monitor_payload(
        data, kCotFamily, kCotFuturesOnly, QDateTime::currentDateTimeUtc().date(), principal);
    while (QLayoutItem* item = moves_->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    if (!model.error.isEmpty()) {
        set_footer(model.error, QString(), true);
        return;
    }
    const QVector<CftcMarketRow> rows = cftc_market_rows(model.entries);
    const CftcOverviewFigures f = cftc_overview_figures(rows);
    if (!f.latest_report.isValid()) {
        report_->setText(tr("No stored reports yet."));
        set_footer(tr("Press refresh (or open Economics → CFTC) to download the CFTC history."), QString(), true);
        return;
    }

    const QString group = cftc_group_name(kCotFamily, principal);
    report_->setText(tr("%1 · report of %2 · %3 markets")
                         .arg(group, QLocale::c().toString(f.latest_report, QStringLiteral("d MMM yyyy")))
                         .arg(f.markets_listed));
    lean_->setText(tr("Leaning long %1 · short %2 · neutral %3")
                       .arg(f.leaning_long.size())
                       .arg(f.leaning_short.size())
                       .arg(f.neutral.size()));
    crowded_long_->setText(tr("CROWDED LONG: %1").arg(cot_names(f.crowded_long)));
    crowded_short_->setText(tr("CROWDED SHORT: %1").arg(cot_names(f.crowded_short)));
    flipped_->setText(tr("FLIPPED THIS WEEK: %1").arg(cot_names(f.flipped)));

    const CftcMoveRanking ranking = cftc_weekly_move_ranking(rows, 3);
    int r = 0;
    auto add = [&](const CftcMoveBar& bar, bool rise) {
        auto* name = new QLabel(bar.label);
        name->setToolTip(bar.tooltip);
        auto* value = new QLabel(cftc_signed_pct(bar.net_flow));
        value->setObjectName(rise ? QStringLiteral("cotUp") : QStringLiteral("cotDown"));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setToolTip(bar.tooltip);
        auto* size = new QLabel(bar.has_scale ? QStringLiteral("%1×").arg(bar.scale, 0, 'f', 1) : QString());
        size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        size->setToolTip(tr("Size against the typical one-week change"));
        moves_->addWidget(name, r, 0);
        moves_->addWidget(value, r, 1);
        moves_->addWidget(size, r, 2);
        ++r;
    };
    for (const auto& bar : ranking.rises)
        add(bar, true);
    for (const auto& bar : ranking.falls)
        add(bar, false);
    if (r == 0)
        moves_->addWidget(new QLabel(tr("No sized weekly change at the newest report.")), 0, 0, 1, 3);

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
    set_footer(footer, tr("Same archive and rules as Economics → CFTC. Behind: %1").arg(cot_names(f.behind, 10)), warn);
}

} // namespace fincept::screens::widgets
