#include "screens/dashboard/widgets/MarketQuoteStripWidget.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "services/markets/MarketDataService.h"
#include "ui/theme/Theme.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>

namespace fincept::screens::widgets {

namespace {
const QStringList kDefaultSymbols = {"AAPL", "MSFT", "GOOGL", "AMZN", "NVDA"};
} // namespace

MarketQuoteStripWidget::MarketQuoteStripWidget(const QJsonObject& cfg, QWidget* parent)
    : BaseWidget(tr("QUOTE STRIP"), parent) {
    auto* vl = content_layout();
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    set_configurable(true);
    apply_styles();
    apply_config(cfg);
}

QJsonObject MarketQuoteStripWidget::config() const {
    QJsonObject o;
    QJsonArray arr;
    for (const auto& s : symbols_)
        arr.append(s);
    o.insert("symbols", arr);
    return o;
}

void MarketQuoteStripWidget::apply_config(const QJsonObject& cfg) {
    QStringList next;
    const QJsonArray arr = cfg.value("symbols").toArray();
    for (const auto& v : arr) {
        const QString s = v.toString().trimmed().toUpper();
        if (!s.isEmpty() && !next.contains(s)) // a duplicate would orphan a row
            next.append(s);
    }
    if (next.isEmpty())
        next = kDefaultSymbols;

    if (next != symbols_)
        feed_.clear();
    symbols_ = next;
    build_rows();
    render_all();

    if (isVisible())
        hub_resubscribe();
}

void MarketQuoteStripWidget::build_rows() {
    auto* vl = content_layout();

    // Clear any existing row widgets
    while (QLayoutItem* it = vl->takeAt(0)) {
        if (auto* w = it->widget())
            w->deleteLater();
        else if (auto* sub = it->layout()) {
            while (QLayoutItem* ci = sub->takeAt(0)) {
                if (auto* cw = ci->widget())
                    cw->deleteLater();
                delete ci;
            }
            delete sub;
        }
        delete it;
    }
    rows_.clear();

    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(3);

    for (int i = 0; i < symbols_.size(); ++i) {
        const QString& sym = symbols_[i];
        Row r;
        r.symbol = new QLabel(sym);
        r.symbol->setObjectName("quoteStripSymbol");
        r.price = new QLabel("—");
        r.price->setObjectName("quoteStripPrice");
        r.price->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        r.change = new QLabel("—");
        r.change->setObjectName("quoteStripChange");
        r.change->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        grid->addWidget(r.symbol, i, 0);
        grid->addWidget(r.price, i, 1);
        grid->addWidget(r.change, i, 2);
        rows_.insert(sym, r);
    }
    vl->addLayout(grid);
    vl->addStretch(1);
    apply_styles();
}

void MarketQuoteStripWidget::hub_resubscribe() {
    QuoteSubscriptions::unsubscribe(this);
    show_feed_status(this, feed_.summary(symbols_), static_cast<int>(symbols_.size()));
    feed_.subscribe(this, symbols_, [this]() { schedule_render([this]() { render_all(); }); });
    hub_active_ = true;
}

void MarketQuoteStripWidget::hub_unsubscribe_all() {
    QuoteSubscriptions::unsubscribe(this);
    hub_active_ = false;
}

void MarketQuoteStripWidget::render_all() {
    for (const auto& sym : symbols_) {
        if (const auto* q = feed_.quote(sym)) {
            on_quote(*q);
            continue;
        }
        const QString error = feed_.error(sym);
        auto it = rows_.find(sym);
        if (error.isEmpty() || it == rows_.end())
            continue;
        it->price->setText(QStringLiteral("--"));
        it->change->setText(tr("failed"));
        it->change->setStyleSheet(
            QString("color:%1;font-size:11px;font-weight:600;background:transparent;").arg(ui::colors::WARNING()));
        const QString tip = fincept::screens::quote_status_line(sym, nullptr, error);
        it->symbol->setToolTip(tip);
        it->change->setToolTip(tip);
    }
    show_feed_status(this, feed_.summary(symbols_), static_cast<int>(symbols_.size()));
}

void MarketQuoteStripWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_resubscribe();
}

void MarketQuoteStripWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void MarketQuoteStripWidget::on_quote(const fincept::services::QuoteData& q) {
    auto it = rows_.find(q.symbol);
    if (it == rows_.end())
        return;
    Row& r = it.value();
    const int decimals =
        fincept::screens::quote_price_decimals(q.has_price, q.price, q.symbol.endsWith(QLatin1String("=X")) ? 4 : 2);
    r.price->setText(fincept::screens::quote_field_text(q.has_price, q.price, decimals));
    const QString tip = fincept::screens::quote_status_line(q.symbol, &q, feed_.error(q.symbol));
    r.symbol->setToolTip(tip);
    r.price->setToolTip(tip);
    r.change->setToolTip(tip);
    r.change->setText(fincept::screens::quote_signed_text(q.has_change_pct, q.change_pct, 2, QStringLiteral("%")));
    // This cell displays the percent change only, so its colour follows that
    // field's own presence and value: missing stays dim, a genuine zero is
    // neutral, and a missing percent is never coloured by the absolute change.
    const QColor col = !q.has_change_pct  ? QColor(ui::colors::TEXT_DIM())
                       : q.change_pct > 0 ? QColor(ui::colors::POSITIVE())
                       : q.change_pct < 0 ? QColor(ui::colors::NEGATIVE())
                                          : QColor(ui::colors::TEXT_PRIMARY());
    r.change->setStyleSheet(QString("color:%1;font-size:11px;font-weight:600;background:transparent;").arg(col.name()));
}

QDialog* MarketQuoteStripWidget::make_config_dialog(QWidget* parent) {
    auto* dlg = new QDialog(parent);
    dlg->setWindowTitle(tr("Configure — Quote Strip"));
    auto* form = new QFormLayout(dlg);

    auto* edit = new QLineEdit(dlg);
    edit->setText(symbols_.join(", "));
    edit->setPlaceholderText(tr("e.g. AAPL, MSFT, GOOGL"));
    form->addRow(tr("Symbols"), edit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, dlg, [this, dlg, edit]() {
        QJsonArray arr;
        const QStringList pieces = edit->text().split(',', Qt::SkipEmptyParts);
        for (const auto& p : pieces) {
            const QString s = p.trimmed().toUpper();
            if (!s.isEmpty())
                arr.append(s);
        }
        QJsonObject cfg;
        cfg.insert("symbols", arr);
        apply_config(cfg);
        emit config_changed(cfg);
        dlg->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    return dlg;
}

void MarketQuoteStripWidget::on_theme_changed() {
    apply_styles();
}

void MarketQuoteStripWidget::apply_styles() {
    const QString sym_css =
        QString("color:%1;font-size:11px;font-weight:700;background:transparent;").arg(ui::colors::TEXT_PRIMARY());
    const QString price_css =
        QString("color:%1;font-size:11px;font-weight:600;background:transparent;").arg(ui::colors::TEXT_PRIMARY());
    const QString chg_css =
        QString("color:%1;font-size:11px;font-weight:600;background:transparent;").arg(ui::colors::TEXT_TERTIARY());
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        if (it->symbol)
            it->symbol->setStyleSheet(sym_css);
        if (it->price)
            it->price->setStyleSheet(price_css);
        if (it->change && it->change->text() == "—")
            it->change->setStyleSheet(chg_css);
    }
}

void MarketQuoteStripWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("QUOTE STRIP"));
    build_rows(); // re-renders any placeholder text in the new language
    render_all(); // ...and puts the cached readings back
}

} // namespace fincept::screens::widgets
