#include "screens/screener/ScreenerScreen.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "ui/theme/Theme.h"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace fincept::screens {

namespace {
// Broad large-cap basket across sectors — mirrors the dashboard ScreenerWidget
// basket, extended for a full-screen view.
const QStringList kBasket = {"AAPL", "MSFT",  "GOOGL", "AMZN", "NVDA", "META", "TSLA", "NFLX", "AMD", "INTC", "AVGO",
                             "ORCL", "CRM",   "ADBE",  "CSCO", "QCOM", "TXN",  "JPM",  "GS",   "BAC", "WFC",  "MS",
                             "C",    "BRK-B", "V",     "MA",   "AXP",  "PYPL", "WMT",  "COST", "TGT", "HD",   "LOW",
                             "NKE",  "MCD",   "SBUX",  "AMGN", "PFE",  "JNJ",  "MRK",  "ABBV", "LLY", "UNH",  "XOM",
                             "CVX",  "SLB",   "COP",   "NEE",  "DUK",  "SO",   "CAT",  "GE",   "HON", "RTX",  "BA",
                             "DE",   "PLTR",  "COIN",  "SOFI", "SNAP", "UBER", "ABNB", "SHOP", "SQ"};
} // namespace

ScreenerScreen::ScreenerScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    apply_styles();
    retranslate();
}

void ScreenerScreen::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header bar ──────────────────────────────────────────────────────────
    header_bar_ = new QWidget(this);
    auto* hb = new QHBoxLayout(header_bar_);
    hb->setContentsMargins(16, 12, 16, 12);
    hb->setSpacing(10);

    auto* title_box = new QVBoxLayout;
    title_box->setSpacing(1);
    title_lbl_ = new QLabel;
    title_lbl_->setObjectName("screenerTitle");
    subtitle_lbl_ = new QLabel;
    subtitle_lbl_->setObjectName("screenerSubtitle");
    title_box->addWidget(title_lbl_);
    title_box->addWidget(subtitle_lbl_);
    hb->addLayout(title_box);
    hb->addStretch();

    search_ = new QLineEdit;
    search_->setClearButtonEnabled(true);
    search_->setFixedWidth(220);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { apply_filter(); });
    hb->addWidget(search_);

    sort_combo_ = new QComboBox;
    connect(sort_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { apply_filter(); });
    hb->addWidget(sort_combo_);

    count_lbl_ = new QLabel;
    count_lbl_->setObjectName("screenerCount");
    hb->addWidget(count_lbl_);

    refresh_btn_ = new QPushButton;
    refresh_btn_->setCursor(Qt::PointingHandCursor);
    connect(refresh_btn_, &QPushButton::clicked, this, &ScreenerScreen::refresh_now);
    hb->addWidget(refresh_btn_);

    root->addWidget(header_bar_);

    // ── Results table ───────────────────────────────────────────────────────
    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    root->addWidget(table_, 1);
}

void ScreenerScreen::apply_styles() {
    setStyleSheet(
        QString("ScreenerScreen { background: %1; }").arg(ui::colors::BG_BASE()) +
        QString("#screenerTitle { color: %1; font-size: 16px; font-weight: 700; }").arg(ui::colors::TEXT_PRIMARY()) +
        QString("#screenerSubtitle { color: %1; font-size: 11px; }").arg(ui::colors::TEXT_TERTIARY()) +
        QString("#screenerCount { color: %1; font-size: 11px; }").arg(ui::colors::TEXT_TERTIARY()) +
        QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: 4px 8px; }")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_MED()) +
        QString("QComboBox { background: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: 4px 8px; }"
                "QComboBox QAbstractItemView { background: %1; color: %2; border: 1px solid %3; }")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_MED()) +
        QString("QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: 4px 12px; }"
                "QPushButton:hover { border-color: %4; }")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_MED(), ui::colors::INFO()));

    if (header_bar_)
        header_bar_->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2;")
                                       .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));

    if (table_)
        table_->setStyleSheet(
            QString("QTableWidget { background: %1; color: %2; border: none; gridline-color: %3; }")
                .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM()) +
            QString("QTableWidget::item { padding: 6px 8px; }") +
            QString("QTableWidget::item:alternate { background: %1; }").arg(ui::colors::BG_RAISED()) +
            QString("QHeaderView::section { background: %1; color: %2; border: none; border-bottom: 1px solid %3; "
                    "padding: 6px 8px; font-size: 10px; font-weight: 700; }")
                .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_TERTIARY(), ui::colors::BORDER_MED()));
}

void ScreenerScreen::retranslate() {
    title_lbl_->setText(tr("STOCK SCREENER"));
    subtitle_lbl_->setText(tr("Filter a broad large-cap basket by change, volume, or price"));
    search_->setPlaceholderText(tr("Search symbol or name…"));
    refresh_btn_->setText(tr("REFRESH"));

    search_->setAccessibleName(tr("Filter by symbol or company name"));
    sort_combo_->setAccessibleName(tr("Sort order"));
    refresh_btn_->setAccessibleName(tr("Refresh all quotes now"));

    const int prev = sort_combo_->currentIndex();
    QSignalBlocker block(sort_combo_);
    sort_combo_->clear();
    // Index 0/1 sort by change but the old "% CHANGE ↑ / ↓" labels read as sort
    // direction while actually meaning gainers/losers — name the intent instead.
    sort_combo_->addItems({tr("TOP GAINERS"), tr("TOP LOSERS"), tr("VOLUME ↓"), tr("PRICE ↓"), tr("PRICE ↑")});
    sort_combo_->setCurrentIndex(prev < 0 ? 0 : prev);

    table_->setHorizontalHeaderLabels(
        {tr("SYMBOL"), tr("NAME"), tr("PRICE"), tr("CHG%"), tr("VOLUME")});

    rebuild_from_cache();
}

void ScreenerScreen::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (!hub_active_)
        hub_subscribe_all();
}

void ScreenerScreen::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void ScreenerScreen::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (!e)
        return;
    if (e->type() == QEvent::StyleChange || e->type() == QEvent::PaletteChange) {
        // Re-entrancy guard — WITHOUT THIS THE PROCESS DIES WITH A STACK
        // OVERFLOW (0xC00000FD).
        //
        // apply_styles() calls setStyleSheet() on header_bar_ and table_.
        // setStyleSheet re-polishes the widget, which posts a fresh StyleChange
        // — straight back into this handler, which calls apply_styles() again.
        // Nothing breaks the loop, so it recurses until the stack is exhausted.
        //
        // Found by --smoke-test, which aborted deterministically while
        // constructing this screen (the constructor calls apply_styles()
        // directly). Reproduced across runs with different walk orders.
        if (restyling_)
            return;
        restyling_ = true;
        apply_styles();
        restyling_ = false;
    } else if (e->type() == QEvent::LanguageChange) {
        retranslate();
    }
}

void ScreenerScreen::hub_subscribe_all() {
    auto& hub = datahub::DataHub::instance();
    for (const auto& sym : kBasket) {
        const QString topic = QStringLiteral("market:quote:") + sym;
        hub.subscribe(this, topic, [this, sym](const QVariant& v) {
            if (!v.canConvert<services::QuoteData>())
                return;
            row_cache_.insert(sym, v.value<services::QuoteData>());
            schedule_rebuild();
        });
    }
    hub_active_ = true;
}

void ScreenerScreen::schedule_rebuild() {
    // 62 topics deliver 62 separate callbacks per refresh sweep. Rebuilding the
    // whole table on each one meant ~62 full sorts and ~19,000 QTableWidgetItem
    // allocations per sweep, and reset the scroll position 62 times. Coalesce
    // into one rebuild on the next event-loop turn.
    //
    // This is a render coalescer, not a data-refresh timer (D3): it is a
    // zero-delay single shot armed by an incoming hub delivery, never a cadence.
    if (rebuild_pending_)
        return;
    rebuild_pending_ = true;
    QTimer::singleShot(0, this, [this]() {
        rebuild_pending_ = false;
        rebuild_from_cache();
    });
}

void ScreenerScreen::hub_unsubscribe_all() {
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void ScreenerScreen::refresh_now() {
    auto& hub = datahub::DataHub::instance();
    QStringList topics;
    topics.reserve(kBasket.size());
    for (const auto& sym : kBasket)
        topics.append(QStringLiteral("market:quote:") + sym);
    hub.request(topics, /*force=*/true); // user-triggered: bypass min_interval
}

void ScreenerScreen::rebuild_from_cache() {
    all_quotes_.clear();
    all_quotes_.reserve(row_cache_.size());
    for (const auto& sym : kBasket) {
        auto it = row_cache_.constFind(sym);
        if (it != row_cache_.constEnd())
            all_quotes_.append(it.value());
    }
    apply_filter();
}

void ScreenerScreen::apply_filter() {
    QVector<services::QuoteData> rows = all_quotes_;

    const QString needle = search_ ? search_->text().trimmed() : QString();
    if (!needle.isEmpty()) {
        QVector<services::QuoteData> filtered;
        filtered.reserve(rows.size());
        for (const auto& q : rows) {
            if (q.symbol.contains(needle, Qt::CaseInsensitive) || q.name.contains(needle, Qt::CaseInsensitive))
                filtered.append(q);
        }
        rows = filtered;
    }

    // Missing readings sort last in every ordering, so a field the provider did
    // not return can never pose as a 0.00% "neutral" or a $0 price.
    auto sort_rows = [&rows](auto has, auto value, bool descending) {
        std::stable_sort(rows.begin(), rows.end(), [&](const services::QuoteData& a, const services::QuoteData& b) {
            const bool a_has = has(a);
            const bool b_has = has(b);
            if (a_has != b_has)
                return a_has;
            if (!a_has)
                return false;
            return descending ? value(a) > value(b) : value(a) < value(b);
        });
    };

    const int idx = sort_combo_ ? sort_combo_->currentIndex() : 0;
    switch (idx) {
        case 0: // % change desc (top gainers first)
            sort_rows([](const auto& q) { return q.has_change_pct; }, [](const auto& q) { return q.change_pct; }, true);
            break;
        case 1: // % change asc (top losers first)
            sort_rows([](const auto& q) { return q.has_change_pct; }, [](const auto& q) { return q.change_pct; },
                      false);
            break;
        case 2: // volume desc
            sort_rows([](const auto& q) { return q.has_volume; }, [](const auto& q) { return q.volume; }, true);
            break;
        case 3: // price desc
            sort_rows([](const auto& q) { return q.has_price; }, [](const auto& q) { return q.price; }, true);
            break;
        case 4: // price asc
            sort_rows([](const auto& q) { return q.has_price; }, [](const auto& q) { return q.price; }, false);
            break;
        default:
            break;
    }

    render_rows(rows);
    if (count_lbl_)
        count_lbl_->setText(tr("%1 of %2 symbols").arg(rows.size()).arg(kBasket.size()));
}

void ScreenerScreen::render_rows(const QVector<services::QuoteData>& rows) {
    // Rebuilding every item drops the scroll offset and the selected row. Quotes
    // arrive continuously, so without this the list jumps back to the top under
    // the user's cursor on every sweep.
    const int scroll_pos = table_->verticalScrollBar() ? table_->verticalScrollBar()->value() : 0;
    const QString selected_symbol =
        table_->currentRow() >= 0 && table_->item(table_->currentRow(), 0)
            ? table_->item(table_->currentRow(), 0)->text()
            : QString();

    table_->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        const auto& q = rows[r];

        auto* sym = new QTableWidgetItem(q.symbol);
        sym->setForeground(
            QColor(q.status == QLatin1String(services::kQuoteStatusStale) ? ui::colors::AMBER() : ui::colors::INFO()));
        QFont bold = sym->font();
        bold.setBold(true);
        sym->setFont(bold);
        table_->setItem(r, 0, sym);

        table_->setItem(r, 1, new QTableWidgetItem(q.name));

        // A field the provider did not return renders as "--"; a genuine zero
        // still renders as 0.00. The presence flags decide, never the value.
        auto* price = new QTableWidgetItem(fincept::screens::quote_field_text(q.has_price, q.price, 2, "$"));
        price->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(r, 2, price);

        auto* chg = new QTableWidgetItem(fincept::screens::quote_signed_text(q.has_change_pct, q.change_pct, 2, "%"));
        chg->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        chg->setForeground(QColor(!q.has_change_pct  ? ui::colors::TEXT_DIM()
                                  : q.change_pct > 0 ? ui::colors::POSITIVE()
                                  : q.change_pct < 0 ? ui::colors::NEGATIVE()
                                                     : ui::colors::TEXT_PRIMARY()));
        table_->setItem(r, 3, chg);

        auto* vol = new QTableWidgetItem(fincept::screens::quote_volume_text(q));
        vol->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        vol->setForeground(QColor(ui::colors::TEXT_SECONDARY()));
        table_->setItem(r, 4, vol);

        // Provider / retrieval time / status on every cell, so a cached row is
        // never presented as a current one.
        const QString provenance = fincept::screens::quote_provenance_text(q);
        for (int c = 0; c < table_->columnCount(); ++c) {
            if (auto* cell = table_->item(r, c))
                cell->setToolTip(provenance);
        }
    }

    if (!selected_symbol.isEmpty()) {
        for (int r = 0; r < rows.size(); ++r) {
            if (rows[r].symbol == selected_symbol) {
                table_->selectRow(r);
                break;
            }
        }
    }
    if (table_->verticalScrollBar())
        table_->verticalScrollBar()->setValue(scroll_pos);
}

} // namespace fincept::screens
