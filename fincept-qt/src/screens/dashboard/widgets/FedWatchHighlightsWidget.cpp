#include "screens/dashboard/widgets/FedWatchHighlightsWidget.h"

#include "core/config/ProfileManager.h"
#include "screens/economics/panels/FedWatchViewModel.h"
#include "ui/theme/Theme.h"

#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace fincept::screens::widgets {

namespace fw = fincept::screens::fedwatch;

namespace {

QJsonObject fw_payload(const QJsonObject& envelope) {
    return envelope["data"].isObject() ? envelope["data"].toObject() : envelope;
}

QLabel* fw_cell(const QString& text, const QString& object_name, Qt::Alignment align = Qt::AlignLeft) {
    auto* l = new QLabel(text);
    l->setObjectName(object_name);
    l->setAlignment(align | Qt::AlignVCenter);
    return l;
}

void fw_clear_grid(QGridLayout* grid) {
    while (QLayoutItem* item = grid->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

/// The outcome with the highest probability on one side (fed or Polymarket).
const fw::Comparison* fw_most_likely(const QVector<fw::Comparison>& rows, bool fed_side) {
    const fw::Comparison* best = nullptr;
    for (const auto& c : rows) {
        const auto v = fed_side ? c.fed : c.poly;
        if (!v)
            continue;
        if (!best || *v > *(fed_side ? best->fed : best->poly))
            best = &c;
    }
    return best;
}

} // namespace

FedWatchHighlightsWidget::FedWatchHighlightsWidget(QWidget* parent)
    : BaseWidget(tr("FEDWATCH · RATE OUTLOOK"), parent, ui::colors::INFO()) {
    body_ = new QWidget(this);
    body_->setObjectName("fwHighlights");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    target_ = fw_cell(QStringLiteral("--"), QStringLiteral("fwBig"));
    next_ = fw_cell(QStringLiteral("--"), QStringLiteral("fwSub"));
    next_->setWordWrap(true);
    expected_ = fw_cell(QString(), QStringLiteral("fwSub"));
    expected_->setWordWrap(true);
    vl->addWidget(target_);
    vl->addWidget(next_);
    vl->addWidget(expected_);

    outcomes_title_ = fw_cell(tr("NEXT MEETING · OUTCOME PROBABILITIES"), QStringLiteral("fwHead"));
    vl->addWidget(outcomes_title_);
    outcomes_ = new QGridLayout;
    outcomes_->setHorizontalSpacing(10);
    outcomes_->setVerticalSpacing(2);
    vl->addLayout(outcomes_);

    path_title_ = fw_cell(tr("PATH · MOST LIKELY OUTCOME VS TODAY'S TARGET"), QStringLiteral("fwHead"));
    vl->addWidget(path_title_);
    path_ = new QGridLayout;
    path_->setHorizontalSpacing(10);
    path_->setVerticalSpacing(2);
    vl->addLayout(path_);
    vl->addStretch(1);
    content_layout()->addWidget(body_);

    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &FedWatchHighlightsWidget::on_result);
    // Refresh = collect current observations, then re-read the workspace
    // (the FedWatch screen's REFRESH, without its history backfill).
    connect(this, &BaseWidget::refresh_requested, this, [this]() { request(QStringLiteral("collect")); });
    apply_styles();
    set_loading(true);
}

void FedWatchHighlightsWidget::apply_styles() {
    body_->setStyleSheet(QString("#fwHighlights QLabel{background:transparent;font-size:10px;color:%1;}"
                                 "#fwHighlights QLabel#fwBig{font-size:16px;font-weight:bold;color:%2;}"
                                 "#fwHighlights QLabel#fwSub{color:%3;}"
                                 "#fwHighlights QLabel#fwHead{font-size:9px;font-weight:bold;color:%4;"
                                 "padding-top:6px;letter-spacing:0.5px;}"
                                 "#fwHighlights QLabel#fwNum{font-weight:bold;}")
                             .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(),
                                  ui::colors::TEXT_TERTIARY()));
}

void FedWatchHighlightsWidget::on_theme_changed() {
    apply_styles();
}

void FedWatchHighlightsWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("FEDWATCH · RATE OUTLOOK"));
    outcomes_title_->setText(tr("NEXT MEETING · OUTCOME PROBABILITIES"));
    path_title_->setText(tr("PATH · MOST LIKELY OUTCOME VS TODAY'S TARGET"));
    if (!last_data_.isEmpty())
        render(last_data_);
}

void FedWatchHighlightsWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!loaded_once_) {
        loaded_once_ = true;
        request(QStringLiteral("workspace")); // stored data only
    }
}

void FedWatchHighlightsWidget::request(const QString& command) {
    QStringList args;
    args << "--db" << ProfileManager::instance().profile_root() + "/fedwatch/fedwatch_history.db";
    pending_id_ = QStringLiteral("dash-fedwatch:%1:%2").arg(quintptr(this)).arg(++sequence_);
    pending_command_ = command;
    set_loading(true);
    services::EconomicsService::instance().execute("fedwatch", "fedwatch_data.py", command, args, pending_id_, true);
}

void FedWatchHighlightsWidget::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (request_id != pending_id_)
        return;
    pending_id_.clear();
    const QJsonObject data = fw_payload(result.data);
    const bool usable = result.success || (result.data["success"].toBool() && result.data["data"].isObject());
    if (pending_command_ == QLatin1String("collect")) {
        if (!usable)
            set_footer(tr("Collection failed: %1 — stored data shown")
                           .arg(result.error.isEmpty() ? tr("no usable response") : result.error.left(120)),
                       result.error, true);
        request(QStringLiteral("workspace"));
        return;
    }
    set_loading(false);
    if (!usable || !data["meetings"].isArray()) {
        set_footer(tr("Stored FedWatch data could not be read: %1").arg(result.error.left(120)), result.error, true);
        return;
    }
    last_data_ = data;
    render(data);
}

void FedWatchHighlightsWidget::render(const QJsonObject& data) {
    const fw::Workspace ws = fw::parse_workspace(data);
    const auto upcoming = ws.upcoming();

    if (ws.target_lower && ws.target_upper)
        target_->setText(tr("FED TARGET %1").arg(fw::band_label(*ws.target_lower, *ws.target_upper)));
    else
        target_->setText(tr("FED TARGET —"));
    target_->setToolTip(
        ws.target_date.isValid()
            ? tr("Target range as of %1 (%2)").arg(ws.target_date.toString(Qt::ISODate), ws.target_status)
            : QString());

    fw_clear_grid(outcomes_);
    fw_clear_grid(path_);
    if (upcoming.isEmpty()) {
        next_->setText(tr("No upcoming meeting in the stored data."));
        expected_->clear();
        set_footer(tr("Open Economics → FedWatch and press REFRESH to collect data."), QString(), true);
        return;
    }

    const fw::Meeting& m = *upcoming.first();
    next_->setText(tr("Next FOMC decision %1 · in %2 day(s)").arg(fw::meeting_label(m.date)).arg(m.days_until));
    QString exp = tr("Expected rate after it: %1").arg(fw::rate(m.fed.expected));
    if (m.fed.expected && m.fed.expected_previous_day)
        exp += tr(" · 1 day %1").arg(fw::bp_change(*m.fed.expected - *m.fed.expected_previous_day));
    if (m.fed.expected && m.fed.expected_previous_week)
        exp += tr(" · 1 week %1").arg(fw::bp_change(*m.fed.expected - *m.fed.expected_previous_week));
    expected_->setText(exp);

    // Outcome probabilities of the next meeting, highest fed-side first.
    QVector<fw::Comparison> rows = m.comparison;
    std::sort(rows.begin(), rows.end(), [](const fw::Comparison& a, const fw::Comparison& b) {
        return a.fed.value_or(-1) != b.fed.value_or(-1) ? a.fed.value_or(-1) > b.fed.value_or(-1)
                                                        : a.poly.value_or(-1) > b.poly.value_or(-1);
    });
    outcomes_->addWidget(fw_cell(tr("OUTCOME"), QStringLiteral("fwHead")), 0, 0);
    outcomes_->addWidget(fw_cell(tr("FUTURES"), QStringLiteral("fwHead"), Qt::AlignRight), 0, 1);
    outcomes_->addWidget(fw_cell(tr("POLYMARKET"), QStringLiteral("fwHead"), Qt::AlignRight), 0, 2);
    int r = 1;
    for (const auto& c : rows) {
        if (r > 4)
            break;
        if (c.fed.value_or(0) < 0.5 && c.poly.value_or(0) < 0.5)
            continue; // negligible on both sides
        outcomes_->addWidget(fw_cell(fw::outcome_label(c.bp, c.open), QString()), r, 0);
        outcomes_->addWidget(fw_cell(fw::pct(c.fed, 0), QStringLiteral("fwNum"), Qt::AlignRight), r, 1);
        outcomes_->addWidget(fw_cell(fw::pct(c.poly, 0), QStringLiteral("fwNum"), Qt::AlignRight), r, 2);
        ++r;
    }
    if (r == 1)
        outcomes_->addWidget(fw_cell(tr("No outcome probabilities stored for this meeting."), QStringLiteral("fwSub")),
                             1, 0, 1, 3);

    // The path over the next meetings: the most likely outcome (futures side)
    // and the expected rate after each meeting.
    path_->addWidget(fw_cell(tr("MEETING"), QStringLiteral("fwHead")), 0, 0);
    path_->addWidget(fw_cell(tr("MOST LIKELY"), QStringLiteral("fwHead")), 0, 1);
    path_->addWidget(fw_cell(tr("EXP. RATE"), QStringLiteral("fwHead"), Qt::AlignRight), 0, 2);
    for (int i = 0; i < upcoming.size() && i < 4; ++i) {
        const fw::Meeting& mm = *upcoming[i];
        const fw::Comparison* best = fw_most_likely(mm.comparison, true);
        path_->addWidget(fw_cell(fw::short_meeting(mm.date), QString()), i + 1, 0);
        path_->addWidget(
            fw_cell(best ? QStringLiteral("%1 %2").arg(fw::outcome_label(best->bp, best->open), fw::pct(best->fed, 0))
                         : QStringLiteral("—"),
                    QString()),
            i + 1, 1);
        path_->addWidget(fw_cell(fw::rate(mm.fed.expected), QStringLiteral("fwNum"), Qt::AlignRight), i + 1, 2);
    }

    // Data state: which side is current and when it was observed.
    const bool fed_current = m.fed.state == QLatin1String("CURRENT");
    const bool poly_current = m.poly.state == QLatin1String("CURRENT");
    QString footer =
        tr("Futures: %1 %2 · Polymarket: %3")
            .arg(fw::state_label(m.fed.state), fw::age_text(m.fed.age_days), fw::state_label(m.poly.state));
    set_footer(footer.simplified(),
               tr("Futures side observed %1; Polymarket observed %2. Same stored data as Economics → FedWatch.")
                   .arg(fw::utc(m.fed.observed_at), fw::utc(m.poly.observed_at)),
               !fed_current || !poly_current);
}

} // namespace fincept::screens::widgets
