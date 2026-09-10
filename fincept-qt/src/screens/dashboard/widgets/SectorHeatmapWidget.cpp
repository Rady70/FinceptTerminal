#include "screens/dashboard/widgets/SectorHeatmapWidget.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "ui/theme/Theme.h"

#include <QFrame>
#include <QLabel>

#include <cmath>

namespace fincept::screens::widgets {

QStringList SectorHeatmapWidget::sector_symbols() {
    return {"XLK", "XLV", "XLF", "XLE", "XLY", "XLI", "XLB", "XLU", "XLRE", "XLC", "XLP", "SOXX"};
}

QMap<QString, QString> SectorHeatmapWidget::sector_labels() {
    return {
        {"XLK", tr("Technology")}, {"XLV", tr("Healthcare")},     {"XLF", tr("Financials")},
        {"XLE", tr("Energy")},     {"XLY", tr("Consumer Disc.")}, {"XLI", tr("Industrials")},
        {"XLB", tr("Materials")},  {"XLU", tr("Utilities")},      {"XLRE", tr("Real Estate")},
        {"XLC", tr("Comm. Svc.")}, {"XLP", tr("Consumer Stap.")}, {"SOXX", tr("Semis")},
    };
}

SectorHeatmapWidget::SectorHeatmapWidget(QWidget* parent) : BaseWidget(tr("SECTOR HEATMAP"), parent) {
    grid_container_ = new QWidget(this);
    grid_container_->setObjectName("sectorHeatGrid");
    grid_ = new QGridLayout(grid_container_);
    grid_->setContentsMargins(4, 4, 4, 4);
    grid_->setSpacing(3);

    content_layout()->addWidget(grid_container_);

    connect(this, &BaseWidget::refresh_requested, this, &SectorHeatmapWidget::refresh_data);

    apply_styles();
    set_loading(true);
}

void SectorHeatmapWidget::apply_styles() {
    if (!grid_container_)
        return;
    // Static cell chrome lives in ONE parent stylesheet with object-name
    // selectors (P7) instead of an inline setStyleSheet per label per refresh.
    // Only the data-driven bits (tile tint, +/- colour) are still set
    // per-widget, and only when they actually change.
    grid_container_->setStyleSheet(
        QString("#sectorHeatGrid QLabel#sectorName{color:%1;font-size:9px;font-weight:bold;background:transparent;}"
                "#sectorHeatGrid QLabel#sectorChg{font-size:11px;font-weight:bold;background:transparent;}")
            .arg(ui::colors::TEXT_PRIMARY()));
    // Force the data-driven colours to be re-applied against the new tokens.
    for (auto& c : cells_) {
        c.last_bg.clear();
        c.last_sign = 0;
    }
}

void SectorHeatmapWidget::on_theme_changed() {
    apply_styles();
    // Re-render from the cache. This used to call refresh_data(), which fires
    // a forced network fetch for 12 symbols just because the palette changed.
    rebuild_from_cache();
}

void SectorHeatmapWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_subscribe_all();
}

void SectorHeatmapWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void SectorHeatmapWidget::refresh_data() {
    auto& hub = datahub::DataHub::instance();
    QStringList topics;
    for (const auto& sym : sector_symbols())
        topics.append(QStringLiteral("market:quote:") + sym);
    hub.request(topics, /*force=*/true); // user-triggered: bypass min_interval
}

void SectorHeatmapWidget::hub_subscribe_all() {
    auto& hub = datahub::DataHub::instance();
    const auto syms = sector_symbols();
    set_loading_progress(row_cache_.size(), syms.size());
    for (const auto& sym : syms) {
        const QString topic = QStringLiteral("market:quote:") + sym;
        hub.subscribe(this, topic, [this, sym, total = syms.size()](const QVariant& v) {
            if (!v.canConvert<services::QuoteData>())
                return;
            row_cache_.insert(sym, v.value<services::QuoteData>());
            set_loading_progress(row_cache_.size(), total);
            // One grid update per delivery burst, not one per sector ETF.
            schedule_render([this]() { rebuild_from_cache(); });
        });
    }
    hub_active_ = true;
}

void SectorHeatmapWidget::hub_unsubscribe_all() {
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void SectorHeatmapWidget::rebuild_from_cache() {
    QVector<services::QuoteData> quotes;
    quotes.reserve(row_cache_.size());
    for (const auto& sym : sector_symbols()) {
        auto it = row_cache_.constFind(sym);
        if (it != row_cache_.constEnd())
            quotes.append(it.value());
    }
    if (!quotes.isEmpty())
        populate(quotes);
}

SectorHeatmapWidget::Cell& SectorHeatmapWidget::cell_at(int index) {
    while (cells_.size() <= index) {
        Cell c;
        c.frame = new QFrame(grid_container_);
        c.frame->setMinimumSize(80, 40);

        auto* cl = new QVBoxLayout(c.frame);
        cl->setContentsMargins(4, 2, 4, 2);
        cl->setSpacing(0);

        c.name = new QLabel(c.frame);
        c.name->setObjectName("sectorName");
        c.name->setAlignment(Qt::AlignCenter);
        cl->addWidget(c.name);

        c.chg = new QLabel(c.frame);
        c.chg->setObjectName("sectorChg");
        c.chg->setAlignment(Qt::AlignCenter);
        cl->addWidget(c.chg);

        const int i = static_cast<int>(cells_.size());
        grid_->addWidget(c.frame, i / 3, i % 3);
        cells_.append(c);
    }
    return cells_[index];
}

void SectorHeatmapWidget::populate(const QVector<services::QuoteData>& quotes) {
    const auto labels = sector_labels();

    const int n = static_cast<int>(quotes.size());
    for (int idx = 0; idx < n; ++idx) {
        const auto& q = quotes[idx];
        Cell& c = cell_at(idx);
        c.frame->setVisible(true);

        // A missing change is not a flat sector: keep the tile neutral/dim and
        // show "--" rather than tinting and colouring a fabricated 0%.
        const bool has_change = q.has_change_pct;
        const double change_pct = has_change ? q.change_pct : 0.0;
        const int intensity = has_change ? static_cast<int>(std::min(std::abs(change_pct) * 60.0, 200.0)) : 0;
        QColor tint = !has_change      ? QColor(ui::colors::TEXT_DIM())
                      : change_pct > 0 ? QColor(ui::colors::POSITIVE())
                      : change_pct < 0 ? QColor(ui::colors::NEGATIVE())
                                       : QColor(ui::colors::TEXT_PRIMARY());
        tint.setAlpha(40 + intensity);
        const QString bg = QString("background: %1; border: 1px solid %2; border-radius: 2px;")
                               .arg(tint.name(QColor::HexArgb), ui::colors::BORDER_DIM());
        // Skip the CSS reparse when the tint is unchanged (the common case
        // for a quiet sector between two refresh ticks).
        if (bg != c.last_bg) {
            c.last_bg = bg;
            c.frame->setStyleSheet(bg);
        }

        const QString display = labels.value(q.symbol, q.symbol);
        if (c.name->text() != display)
            c.name->setText(display);
        c.name->setToolTip(QString("%1  (%2)").arg(display, q.symbol));

        c.chg->setText(fincept::screens::quote_signed_text(has_change, change_pct, 2, QStringLiteral("%")));
        // +1 up, -1 down, 0 neutral or unavailable — zero is not an upward move.
        const int sign = !has_change ? 0 : change_pct > 0 ? 1 : change_pct < 0 ? -1 : 0;
        if (sign != c.last_sign) {
            c.last_sign = sign;
            c.chg->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; background: transparent;")
                                     .arg(sign > 0   ? ui::colors::POSITIVE()
                                          : sign < 0 ? ui::colors::NEGATIVE()
                                                     : ui::colors::TEXT_PRIMARY()));
        }
    }

    // Hide (don't destroy) any cells beyond the current data set so a shorter
    // payload doesn't leave stale tiles on screen.
    for (int i = n; i < static_cast<int>(cells_.size()); ++i)
        cells_[i].frame->setVisible(false);
}

void SectorHeatmapWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("SECTOR HEATMAP"));
    rebuild_from_cache(); // re-renders sector cell labels in the new language
}

} // namespace fincept::screens::widgets
