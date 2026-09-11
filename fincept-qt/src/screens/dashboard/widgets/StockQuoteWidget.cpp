#include "screens/dashboard/widgets/StockQuoteWidget.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "ui/theme/Theme.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLineEdit>

namespace fincept::screens::widgets {

namespace {
QString normalize_symbol(const QString& s) {
    const QString t = s.trimmed().toUpper();
    return t.isEmpty() ? QStringLiteral("AAPL") : t;
}
} // namespace

StockQuoteWidget::StockQuoteWidget(const QString& symbol, QWidget* parent)
    : BaseWidget(tr("QUOTE: %1").arg(symbol.toUpper()), parent), symbol_(symbol.toUpper()) {
    auto* vl = content_layout();
    vl->setContentsMargins(12, 8, 12, 8);
    vl->setSpacing(8);

    // ── Price row ──
    auto* price_row = new QWidget(this);
    auto* prl = new QHBoxLayout(price_row);
    prl->setContentsMargins(0, 0, 0, 0);
    prl->setSpacing(8);

    price_label_ = new QLabel("--");
    prl->addWidget(price_label_);

    auto* change_col = new QWidget(this);
    auto* ccl = new QVBoxLayout(change_col);
    ccl->setContentsMargins(0, 4, 0, 0);
    ccl->setSpacing(0);

    arrow_label_ = new QLabel;
    ccl->addWidget(arrow_label_);

    change_label_ = new QLabel("--");
    ccl->addWidget(change_label_);

    prl->addWidget(change_col);
    prl->addStretch();

    ticker_label_ = new QLabel(symbol_);
    prl->addWidget(ticker_label_);

    vl->addWidget(price_row);

    // ── Separator ──
    sep_ = new QFrame;
    sep_->setFixedHeight(1);
    vl->addWidget(sep_);

    // ── Stats grid ──
    auto* stats = new QWidget(this);
    auto* gl = new QGridLayout(stats);
    gl->setContentsMargins(0, 0, 0, 0);
    gl->setSpacing(6);

    auto make_stat = [&](int row, int col, const char* key, QLabel*& val_out) {
        auto* cell = new QWidget(this);
        stat_cells_.append(cell);
        auto* cl = new QVBoxLayout(cell);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(2);

        auto* lbl = new QLabel(tr(key));
        stat_labels_.append(lbl);
        stat_label_keys_.append(QString::fromLatin1(key));
        cl->addWidget(lbl);

        val_out = new QLabel("--");
        stat_values_.append(val_out);
        cl->addWidget(val_out);

        gl->addWidget(cell, row, col);
    };

    make_stat(0, 0, QT_TR_NOOP("OPEN"), open_val_);
    make_stat(0, 1, QT_TR_NOOP("PREV CLOSE"), prev_val_);
    make_stat(1, 0, QT_TR_NOOP("HIGH"), high_val_);
    make_stat(1, 1, QT_TR_NOOP("LOW"), low_val_);
    make_stat(2, 0, QT_TR_NOOP("VOLUME"), volume_val_);

    vl->addWidget(stats);
    vl->addStretch();

    set_configurable(true);
    connect(this, &BaseWidget::refresh_requested, this, &StockQuoteWidget::refresh_data);

    apply_styles();
    set_loading(true);
}

QJsonObject StockQuoteWidget::config() const {
    QJsonObject o;
    o.insert("symbol", symbol_);
    return o;
}

void StockQuoteWidget::apply_config(const QJsonObject& cfg) {
    const QString next = normalize_symbol(cfg.value("symbol").toString(symbol_));
    if (next == symbol_)
        return;
    set_symbol(next);
}

QDialog* StockQuoteWidget::make_config_dialog(QWidget* parent) {
    auto* dlg = new QDialog(parent);
    dlg->setWindowTitle(tr("Configure — Stock Quote"));
    auto* form = new QFormLayout(dlg);

    auto* edit = new QLineEdit(dlg);
    edit->setText(symbol_);
    edit->setPlaceholderText(tr("e.g. AAPL"));
    edit->setAccessibleName(tr("Symbol"));
    form->addRow(tr("Symbol"), edit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    form->addRow(buttons);
    dlg->setTabOrder(edit, buttons);

    connect(buttons, &QDialogButtonBox::accepted, dlg, [this, dlg, edit]() {
        QJsonObject cfg;
        cfg.insert("symbol", normalize_symbol(edit->text()));
        apply_config(cfg);
        emit config_changed(cfg);
        dlg->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    return dlg;
}

void StockQuoteWidget::apply_styles() {
    price_label_->setStyleSheet(QString("color: %1; font-size: 28px; font-weight: bold; background: transparent;")
                                    .arg(ui::colors::TEXT_PRIMARY()));
    arrow_label_->setStyleSheet("font-size: 12px; background: transparent;");
    // The change line's colours are per-field rich-text spans (see populate()),
    // so its stylesheet deliberately carries no colour of its own.
    change_label_->setStyleSheet("font-size: 11px; font-weight: bold; background: transparent;");
    ticker_label_->setStyleSheet(
        QString("color: %1; font-size: 14px; font-weight: bold; background: transparent;").arg(ui::colors::AMBER()));
    sep_->setStyleSheet(QString("background: %1;").arg(ui::colors::BORDER_DIM()));

    for (auto* cell : stat_cells_)
        cell->setStyleSheet(QString("background: %1; border-radius: 2px;").arg(ui::colors::BG_RAISED()));
    for (auto* lbl : stat_labels_)
        lbl->setStyleSheet(
            QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::TEXT_TERTIARY()));
    for (auto* val : stat_values_)
        val->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; background: transparent;")
                               .arg(ui::colors::TEXT_PRIMARY()));
}

void StockQuoteWidget::on_theme_changed() {
    apply_styles();
}

void StockQuoteWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_resubscribe();
}

void StockQuoteWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void StockQuoteWidget::set_symbol(const QString& symbol) {
    symbol_ = normalize_symbol(symbol);
    set_title(tr("QUOTE: %1").arg(symbol_));
    ticker_label_->setText(symbol_);

    // Blank the previous symbol's numbers immediately — leaving AAPL's price
    // under an "MSFT" heading until the first delivery is actively misleading.
    price_label_->setText(QStringLiteral("--"));
    change_label_->setText(QStringLiteral("--"));
    arrow_label_->setText(QString());
    for (auto* v : stat_values_)
        v->setText(QStringLiteral("--"));

    // Re-subscribe to the new topic; old sub for previous symbol is
    // dropped wholesale by `unsubscribe(this)` inside hub_resubscribe().
    if (isVisible()) {
        set_loading(true);
        hub_resubscribe();
    }
}

void StockQuoteWidget::refresh_data() {
    datahub::DataHub::instance().request(QStringLiteral("market:quote:") + symbol_);
}

void StockQuoteWidget::hub_resubscribe() {
    auto& hub = datahub::DataHub::instance();
    hub.unsubscribe(this);
    const QString topic = QStringLiteral("market:quote:") + symbol_;
    hub.subscribe(this, topic, [this](const QVariant& v) {
        if (!v.canConvert<services::QuoteData>())
            return;
        set_loading(false);
        populate(v.value<services::QuoteData>());
    });
    hub_active_ = true;
}

void StockQuoteWidget::hub_unsubscribe_all() {
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void StockQuoteWidget::populate(const services::QuoteData& q) {
    // Each part of the combined change line is styled from its own field: the
    // absolute change and the percent change carry separate presence flags and
    // colours, so a missing part stays dim and a present part is never styled
    // from the other field.
    auto dir_color = [](bool has, double value) -> QString {
        if (!has)
            return ui::colors::TEXT_DIM();
        if (value > 0)
            return ui::colors::POSITIVE();
        if (value < 0)
            return ui::colors::NEGATIVE();
        return ui::colors::TEXT_PRIMARY();
    };
    const bool has_arrow = q.has_change || q.has_change_pct;
    const double arrow_value = q.has_change ? q.change : q.change_pct;
    const QString arrow = !has_arrow        ? QStringLiteral("—")
                          : arrow_value > 0 ? QString(QChar(0x25B2))
                          : arrow_value < 0 ? QString(QChar(0x25BC))
                                            : QString(QChar(0x2022));

    price_label_->setText(fincept::screens::quote_field_text(q.has_price, q.price, 2, QStringLiteral("$")));
    arrow_label_->setText(arrow);
    arrow_label_->setStyleSheet(
        QString("color: %1; font-size: 12px; background: transparent;").arg(dir_color(has_arrow, arrow_value)));

    const QString abs_text = fincept::screens::quote_signed_text(q.has_change, q.change, 2);
    const QString pct_text =
        fincept::screens::quote_signed_text(q.has_change_pct, q.change_pct, 2, QStringLiteral("%"));
    change_label_->setText(QStringLiteral("<span style='color:%1;'>%2</span> "
                                          "<span style='color:%3;'>(%4)</span>")
                               .arg(dir_color(q.has_change, q.change), abs_text.toHtmlEscaped(),
                                    dir_color(q.has_change_pct, q.change_pct), pct_text.toHtmlEscaped()));

    // A missing price placeholder stays unavailable; a valid price is tinted by
    // whichever move field is present and is never dimmed because the other is
    // missing.
    const QString price_color = !q.has_price       ? QString(ui::colors::TEXT_DIM())
                                : q.has_change     ? dir_color(true, q.change)
                                : q.has_change_pct ? dir_color(true, q.change_pct)
                                                   : QString(ui::colors::TEXT_PRIMARY());
    price_label_->setStyleSheet(
        QString("color: %1; font-size: 28px; font-weight: bold; background: transparent;").arg(price_color));

    // The batch quote snapshot carries last/change/high/low/volume but no
    // session open. Showing `high` here (as this did previously) prints a
    // wrong number under an "OPEN" heading — on a trading terminal that is
    // worse than showing nothing.
    open_val_->setText(QStringLiteral("--"));
    open_val_->setToolTip(tr("Session open is not available in the batch quote feed"));
    high_val_->setText(fincept::screens::quote_field_text(q.has_high, q.high, 2, QStringLiteral("$")));
    low_val_->setText(fincept::screens::quote_field_text(q.has_low, q.low, 2, QStringLiteral("$")));
    prev_val_->setText(q.has_price && q.has_change
                           ? fincept::screens::quote_field_text(true, q.price - q.change, 2, QStringLiteral("$"))
                           : QStringLiteral("--"));

    // Format volume; missing stays "--" and a genuine zero is "0". A negative
    // volume is malformed and renders unavailable, never as a real zero.
    if (!q.has_volume || q.volume < 0)
        volume_val_->setText(QStringLiteral("--"));
    else if (q.volume == 0)
        volume_val_->setText(QStringLiteral("0"));
    else if (q.volume >= 1e9)
        volume_val_->setText(QString("%1B").arg(q.volume / 1e9, 0, 'f', 1));
    else if (q.volume >= 1e6)
        volume_val_->setText(QString("%1M").arg(q.volume / 1e6, 0, 'f', 1));
    else if (q.volume >= 1e3)
        volume_val_->setText(QString("%1K").arg(q.volume / 1e3, 0, 'f', 1));
    else
        volume_val_->setText(QString::number(static_cast<int>(q.volume)));
}

void StockQuoteWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("QUOTE: %1").arg(symbol_));
    // Re-run tr() on the kept stat-cell headings. (This previously called
    // refresh_data(), which fires a network request and never touched a
    // single label.)
    for (int i = 0; i < stat_labels_.size() && i < stat_label_keys_.size(); ++i)
        stat_labels_[i]->setText(tr(stat_label_keys_[i].toUtf8().constData()));
}

} // namespace fincept::screens::widgets
