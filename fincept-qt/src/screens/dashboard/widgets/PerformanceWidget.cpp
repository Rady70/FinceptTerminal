#include "screens/dashboard/widgets/PerformanceWidget.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "ui/theme/Theme.h"

#include <QFrame>

#include <cmath>

namespace {
inline const QStringList kPerfSymbols = {"^GSPC", "^IXIC", "^DJI", "^RUT", "^VIX", "GC=F"};
}

namespace fincept::screens::widgets {

PerformanceWidget::PerformanceWidget(QWidget* parent)
    : BaseWidget(tr("PERFORMANCE TRACKER"), parent, ui::colors::POSITIVE()) {
    auto* vl = content_layout();

    // We'll show metrics derived from real benchmark ETF data
    QStringList labels = {
        tr("S&P 500 Daily"),         tr("NASDAQ Daily"),         tr("DOW Daily"), tr("Russell 2000 Daily"),
        tr("S&P 500 vs DOW Spread"), tr("NASDAQ vs S&P Spread"), tr("VIX Level"), tr("Gold Daily")};

    for (const auto& label : labels) {
        auto* row = new QWidget(this);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(8, 4, 8, 4);

        MetricRow mr;
        mr.row_widget = row;
        mr.label = new QLabel(label);
        rl->addWidget(mr.label);
        rl->addStretch();

        mr.period = new QLabel(tr("TODAY"));
        rl->addWidget(mr.period);

        mr.value = new QLabel("--");
        rl->addWidget(mr.value);

        rows_.append(mr);
        vl->addWidget(row);
    }
    vl->addStretch();

    connect(this, &BaseWidget::refresh_requested, this, &PerformanceWidget::refresh_data);

    apply_styles();
    set_loading(true);
}

void PerformanceWidget::apply_styles() {
    for (const auto& mr : rows_) {
        mr.row_widget->setStyleSheet(QString("border-bottom: 1px solid %1;").arg(ui::colors::BORDER_DIM()));
        mr.label->setStyleSheet(
            QString("color: %1; font-size: 11px; background: transparent;").arg(ui::colors::TEXT_SECONDARY()));
        mr.period->setStyleSheet(
            QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::TEXT_TERTIARY()));
        mr.value->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; background: transparent;")
                                    .arg(ui::colors::TEXT_PRIMARY()));
    }
}

void PerformanceWidget::on_theme_changed() {
    apply_styles();
}

void PerformanceWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_subscribe_all();
}

void PerformanceWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void PerformanceWidget::refresh_data() {
    auto& hub = datahub::DataHub::instance();
    QStringList topics;
    topics.reserve(kPerfSymbols.size());
    for (const auto& sym : kPerfSymbols)
        topics.append(QStringLiteral("market:quote:") + sym);
    hub.request(topics, /*force=*/true); // user-triggered: bypass min_interval
}

void PerformanceWidget::hub_subscribe_all() {
    auto& hub = datahub::DataHub::instance();
    set_loading_progress(row_cache_.size(), kPerfSymbols.size());
    for (const auto& sym : kPerfSymbols) {
        const QString topic = QStringLiteral("market:quote:") + sym;
        hub.subscribe(this, topic, [this, sym](const QVariant& v) {
            if (!v.canConvert<services::QuoteData>())
                return;
            row_cache_.insert(sym, v.value<services::QuoteData>());
            set_loading_progress(row_cache_.size(), kPerfSymbols.size());
            // One redraw per delivery burst, not one per symbol.
            schedule_render([this]() { rebuild_from_cache(); });
        });
    }
    hub_active_ = true;
}

void PerformanceWidget::hub_unsubscribe_all() {
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void PerformanceWidget::rebuild_from_cache() {
    QVector<services::QuoteData> quotes;
    quotes.reserve(row_cache_.size());
    for (const auto& sym : kPerfSymbols) {
        auto it = row_cache_.constFind(sym);
        if (it != row_cache_.constEnd())
            quotes.append(it.value());
    }
    if (!quotes.isEmpty())
        populate(quotes);
}

void PerformanceWidget::populate(const QVector<services::QuoteData>& quotes) {
    // Build lookup by symbol
    QMap<QString, const services::QuoteData*> map;
    for (const auto& q : quotes)
        map[q.symbol] = &q;

    auto set_row = [&](int idx, bool available, double val) {
        if (idx >= rows_.size())
            return;
        if (!available) {
            // Missing reading is unavailable, not a fabricated +0.00%.
            rows_[idx].value->setText(QStringLiteral("--"));
            rows_[idx].value->setStyleSheet(
                QString("color: %1; font-size: 11px; font-weight: bold; background: transparent;")
                    .arg(ui::colors::TEXT_DIM()));
            return;
        }
        rows_[idx].value->setText(fincept::screens::quote_signed_text(true, val, 2, QStringLiteral("%")));
        const QString color = val > 0   ? ui::colors::POSITIVE()
                              : val < 0 ? ui::colors::NEGATIVE()
                                        : ui::colors::TEXT_PRIMARY();
        rows_[idx].value->setStyleSheet(
            QString("color: %1; font-size: 11px; font-weight: bold; background: transparent;").arg(color));
    };
    auto has_pct = [&](const QString& sym) { return map.contains(sym) && map[sym]->has_change_pct; };
    auto pct = [&](const QString& sym) { return map[sym]->change_pct; };

    // S&P 500 daily
    set_row(0, has_pct("^GSPC"), has_pct("^GSPC") ? pct("^GSPC") : 0.0);
    // NASDAQ daily
    set_row(1, has_pct("^IXIC"), has_pct("^IXIC") ? pct("^IXIC") : 0.0);
    // DOW daily
    set_row(2, has_pct("^DJI"), has_pct("^DJI") ? pct("^DJI") : 0.0);
    // Russell daily
    set_row(3, has_pct("^RUT"), has_pct("^RUT") ? pct("^RUT") : 0.0);

    // Spreads — only meaningful when both operands carry a change reading.
    const bool sp_dow = has_pct("^GSPC") && has_pct("^DJI");
    set_row(4, sp_dow, sp_dow ? pct("^GSPC") - pct("^DJI") : 0.0);
    const bool nq_sp = has_pct("^IXIC") && has_pct("^GSPC");
    set_row(5, nq_sp, nq_sp ? pct("^IXIC") - pct("^GSPC") : 0.0);

    // VIX — show absolute value, not change
    if (map.contains("^VIX") && map["^VIX"]->has_price && rows_.size() > 6) {
        const double vix = map["^VIX"]->price;
        rows_[6].value->setText(QString::number(vix, 'f', 2));
        const QString color = vix > 25   ? ui::colors::NEGATIVE()
                              : vix > 18 ? ui::colors::WARNING()
                                         : ui::colors::POSITIVE();
        rows_[6].value->setStyleSheet(
            QString("color: %1; font-size: 11px; font-weight: bold; background: transparent;").arg(color));
    } else if (rows_.size() > 6) {
        rows_[6].value->setText(QStringLiteral("--"));
        rows_[6].value->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; background: transparent;")
                                          .arg(ui::colors::TEXT_DIM()));
    }

    // Gold daily
    set_row(7, has_pct("GC=F"), has_pct("GC=F") ? pct("GC=F") : 0.0);
}

void PerformanceWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("PERFORMANCE TRACKER"));
    rebuild_from_cache(); // re-derives metric labels from cached quotes
}

} // namespace fincept::screens::widgets
