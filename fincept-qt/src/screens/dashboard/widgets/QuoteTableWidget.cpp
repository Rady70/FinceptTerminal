#include "screens/dashboard/widgets/QuoteTableWidget.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "ui/theme/Theme.h"

#include <QHeaderView>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace fincept::screens::widgets {

QuoteTableWidget::QuoteTableWidget(const QString& title, const QStringList& symbols,
                                   const QMap<QString, QString>& label_map, int price_decimals,
                                   const QString& accent_color, QWidget* parent)
    : BaseWidget(title, parent, accent_color),
      symbols_(symbols),
      label_map_(label_map),
      price_decimals_(price_decimals) {
    table_ = new ui::DataTable;
    table_->set_headers({tr("SYMBOL"), tr("PRICE"), tr("CHG"), tr("CHG%")});
    table_->set_column_widths({130, 100, 80, 70});
    content_layout()->addWidget(table_);
    fit_columns();

    connect(this, &BaseWidget::refresh_requested, this, &QuoteTableWidget::refresh_data);

    apply_styles();
    set_loading(true);
}

void QuoteTableWidget::apply_styles() {
    // QuoteTableWidget delegates styling to DataTable; nothing extra needed.
}

void QuoteTableWidget::on_theme_changed() {
    apply_styles();
    // Re-derive the font from the theme before re-fitting: fit_columns() may
    // have set a smaller table font, and a theme font change must win.
    if (table_) {
        table_->setFont(font());
        fit_columns();
    }
}

void QuoteTableWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    if (table_)
        table_->set_headers({tr("SYMBOL"), tr("PRICE"), tr("CHG"), tr("CHG%")});
}

void QuoteTableWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_subscribe_all();
    fit_columns(); // a tile that was hidden while narrow has no resize event to catch
}

void QuoteTableWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void QuoteTableWidget::resizeEvent(QResizeEvent* e) {
    BaseWidget::resizeEvent(e);
    fit_columns();
}

void QuoteTableWidget::fit_columns() {
    if (!table_ || table_->columnCount() < 4)
        return;
    const int avail = table_->viewport()->width();
    if (avail <= 0)
        return;

    auto* header = table_->horizontalHeader();
    // Widths are managed here; the generic DataTable stretches its last
    // section, which would fight the readings column below.
    header->setStretchLastSection(false);

    // Qt's own content sizing: the delegate measures the rendered font
    // (including the global stylesheet font) and the item padding, so the
    // fixed widths (130+100+80+70) no longer push CHG% off a narrow tile and
    // the measured widths no longer under-count the actual glyphs.
    auto numeric_width = [&]() {
        int total = 0;
        for (int c = 1; c < 4; ++c)
            total += table_->columnWidth(c);
        return total;
    };

    const QFont base = font();
    QFont fitted = base;
    table_->setFont(fitted);
    table_->resizeColumnsToContents();

    // Each candidate is derived from the base font, never from the previous
    // candidate — scaling the already-scaled font compounded to ~44% of the
    // theme size — and the size is floored at a readable minimum.
    constexpr int kMaxSteps = 4; // 0.95, 0.90, 0.85, 0.80
    for (int step = 1; step <= kMaxSteps && numeric_width() + header->minimumSectionSize() > avail; ++step) {
        // The three readings cannot share the tile at the theme font size;
        // step the font down until they can, then let the symbol column take
        // whatever width remains.
        fitted = base;
        const qreal scale = 1.0 - 0.05 * step;
        if (fitted.pointSizeF() > 0)
            fitted.setPointSizeF(std::max(8.0, fitted.pointSizeF() * scale));
        else
            fitted.setPixelSize(std::max(8, static_cast<int>(std::round(fitted.pixelSize() * scale))));
        table_->setFont(fitted);
        table_->resizeColumnsToContents();
    }
    table_->setColumnWidth(0, std::max(header->minimumSectionSize(), avail - numeric_width()));
}

void QuoteTableWidget::refresh_data() {
    // User-triggered refresh: force-bypass min_interval so a user tap on the
    // refresh button always kicks a fetch. Producer-side rate limit still
    // applies, so rage-clicking can't hammer upstream.
    auto& hub = datahub::DataHub::instance();
    QStringList topics;
    topics.reserve(symbols_.size());
    for (const auto& sym : symbols_)
        topics.append(QStringLiteral("market:quote:") + sym);
    hub.request(topics, /*force=*/true);
}

void QuoteTableWidget::hub_subscribe_all() {
    show_feed_status(this, feed_.summary(symbols_), static_cast<int>(symbols_.size()));
    // One render per delivery burst, not one per symbol (P9/P10).
    feed_.subscribe(this, symbols_, [this]() { schedule_render([this]() { render_from_cache(); }); });
    hub_active_ = true;
}

void QuoteTableWidget::hub_unsubscribe_all() {
    QuoteSubscriptions::unsubscribe(this);
    hub_active_ = false;
    // Keep the cache so a quick hide/show doesn't flash an empty table —
    // the first hub delivery on re-subscribe will refresh it anyway.
}

void QuoteTableWidget::render_from_cache() {
    table_->clear_data();
    for (const auto& sym : symbols_) {
        const services::QuoteData* quote = feed_.quote(sym);
        const QString error = feed_.error(sym);
        if (!quote) {
            if (error.isEmpty())
                continue; // not answered yet
            // A failed symbol keeps its row so it does not silently disappear.
            table_->add_row({label_map_.value(sym, sym), QStringLiteral("--"), QStringLiteral("--"), tr("failed")});
            const int row = table_->rowCount() - 1;
            table_->set_cell_color(row, 3, ui::colors::WARNING);
            for (int c = 0; c < table_->columnCount(); ++c)
                if (auto* item = table_->item(row, c))
                    item->setToolTip(fincept::screens::quote_status_line(sym, nullptr, error));
            continue;
        }
        const auto& q = *quote;
        QString display_name = label_map_.value(q.symbol, q.symbol);
        // Missing fields render as "--"; a genuine zero is a reading and keeps
        // its number. A zero change is neutral, never painted as an upward move.
        // A price below 1 gets at least four decimals (0.0793, not 0.08).
        const int decimals = fincept::screens::quote_price_decimals(q.has_price, q.price, price_decimals_);
        const QString price_str = fincept::screens::quote_field_text(q.has_price, q.price, decimals);
        const QString chg_str = fincept::screens::quote_signed_text(q.has_change, q.change, decimals);
        const QString pct_str =
            fincept::screens::quote_signed_text(q.has_change_pct, q.change_pct, 2, QStringLiteral("%"));

        table_->add_row({display_name, price_str, chg_str, pct_str});
        int row = table_->rowCount() - 1;
        const QString tip = fincept::screens::quote_status_line(sym, &q, error);
        for (int c = 0; c < table_->columnCount(); ++c)
            if (auto* item = table_->item(row, c))
                item->setToolTip(tip);
        // Each displayed field is coloured from its own flag and value; a
        // missing CHG cell is never painted with the CHG% direction, and a
        // genuine zero is neutral rather than a move.
        auto move_color = [](bool has, double value) -> QString {
            if (!has)
                return ui::colors::TEXT_DIM;
            if (value > 0)
                return ui::colors::POSITIVE;
            if (value < 0)
                return ui::colors::NEGATIVE;
            return ui::colors::TEXT_PRIMARY;
        };
        table_->set_cell_color(row, 2, move_color(q.has_change, q.change));
        table_->set_cell_color(row, 3, move_color(q.has_change_pct, q.change_pct));
    }
    show_feed_status(this, feed_.summary(symbols_), static_cast<int>(symbols_.size()));
    // A vertical scrollbar can appear once the rows arrive and shave width off
    // the viewport; re-fit after the fill so all four columns stay visible.
    fit_columns();
}

} // namespace fincept::screens::widgets
