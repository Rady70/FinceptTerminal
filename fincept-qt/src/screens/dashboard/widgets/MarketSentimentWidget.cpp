#include "screens/dashboard/widgets/MarketSentimentWidget.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/dashboard/widgets/MarketGauges.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "ui/theme/Theme.h"

namespace fincept::screens::widgets {

namespace {
inline const QStringList kSentimentSymbols = {"^VIX", "AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "META", "TSLA", "NFLX",
                                              "AMD",  "INTC", "JPM",  "GS",    "BAC",  "WMT",  "DIS",  "BA",   "XOM",
                                              "CVX",  "COIN", "PLTR", "SOFI",  "NKE",  "PFE",  "PYPL"};
}

MarketSentimentWidget::MarketSentimentWidget(QWidget* parent) : BaseWidget(tr("MARKET SENTIMENT"), parent) {
    auto* vl = content_layout();
    vl->setContentsMargins(8, 8, 8, 8);
    vl->setSpacing(8);

    // ── Score banner ──
    banner_ = new QWidget(this);
    auto* bl = new QVBoxLayout(banner_);
    bl->setContentsMargins(12, 10, 12, 10);
    bl->setSpacing(4);
    bl->setAlignment(Qt::AlignCenter);

    auto* score_row = new QWidget(this);
    auto* srl = new QHBoxLayout(score_row);
    srl->setContentsMargins(0, 0, 0, 0);
    srl->setAlignment(Qt::AlignCenter);
    srl->setSpacing(8);

    arrow_label_ = new QLabel(QChar(0x2014)); // em-dash placeholder
    srl->addWidget(arrow_label_);

    score_label_ = new QLabel("--");
    srl->addWidget(score_label_);

    bl->addWidget(score_row);

    verdict_label_ = new QLabel(tr("LOADING..."));
    verdict_label_->setAlignment(Qt::AlignCenter);
    bl->addWidget(verdict_label_);

    vl->addWidget(banner_);

    // ── Sentiment bar ──
    auto* bar_container = new QWidget(this);
    bar_container->setFixedHeight(8);
    auto* bar_layout = new QHBoxLayout(bar_container);
    bar_layout->setContentsMargins(0, 0, 0, 0);
    bar_layout->setSpacing(1);

    bull_bar_ = new QFrame;
    bar_layout->addWidget(bull_bar_, 1);

    neutral_bar_ = new QFrame;
    bar_layout->addWidget(neutral_bar_, 1);

    bear_bar_ = new QFrame;
    bar_layout->addWidget(bear_bar_, 1);

    vl->addWidget(bar_container);

    // Labels under bar
    auto* bar_labels = new QWidget(this);
    auto* bll = new QHBoxLayout(bar_labels);
    bll->setContentsMargins(0, 0, 0, 0);

    bull_label_ = new QLabel(tr("-- UP"));
    bll->addWidget(bull_label_);
    bll->addStretch();

    neutral_label_ = new QLabel(tr("-- FLAT"));
    bll->addWidget(neutral_label_);
    bll->addStretch();

    bear_label_ = new QLabel(tr("-- DOWN"));
    bll->addWidget(bear_label_);

    vl->addWidget(bar_labels);

    // ── VIX and Breadth ──
    separator_ = new QFrame;
    separator_->setFixedHeight(1);
    vl->addWidget(separator_);

    auto* stats = new QWidget(this);
    auto* stl = new QVBoxLayout(stats);
    stl->setContentsMargins(0, 4, 0, 0);
    stl->setSpacing(4);

    auto make_stat_row = [&](const QString& label, QLabel*& title_out, QLabel*& val_out) {
        auto* row = new QWidget(this);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        title_out = new QLabel(label);
        rl->addWidget(title_out);
        rl->addStretch();

        val_out = new QLabel("--");
        rl->addWidget(val_out);

        stl->addWidget(row);
    };

    make_stat_row(tr("VIX (Fear Gauge)"), vix_title_label_, vix_label_);
    make_stat_row(tr("Up / down (beyond ±0.3%)"), breadth_title_label_, breadth_label_);

    vl->addWidget(stats);
    vl->addStretch();

    connect(this, &BaseWidget::refresh_requested, this, &MarketSentimentWidget::refresh_data);

    apply_styles();
    set_loading(true);
}

void MarketSentimentWidget::apply_styles() {
    banner_->setStyleSheet(QString("background: %1; border-radius: 2px;").arg(ui::colors::BG_RAISED()));
    arrow_label_->setStyleSheet(
        QString("font-size: 18px; background: transparent; color: %1;").arg(ui::colors::TEXT_TERTIARY()));
    score_label_->setStyleSheet(QString("color: %1; font-size: 24px; font-weight: bold; background: transparent;")
                                    .arg(ui::colors::TEXT_TERTIARY()));
    verdict_label_->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
                                      .arg(ui::colors::TEXT_TERTIARY()));

    bull_bar_->setStyleSheet(QString("background: %1; border-radius: 0;").arg(ui::colors::POSITIVE()));
    neutral_bar_->setStyleSheet(QString("background: %1; border-radius: 0;").arg(ui::colors::TEXT_TERTIARY()));
    bear_bar_->setStyleSheet(QString("background: %1; border-radius: 0;").arg(ui::colors::NEGATIVE()));

    bull_label_->setStyleSheet(
        QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::POSITIVE()));
    neutral_label_->setStyleSheet(
        QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::TEXT_TERTIARY()));
    bear_label_->setStyleSheet(
        QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::NEGATIVE()));

    separator_->setStyleSheet(QString("background: %1;").arg(ui::colors::BORDER_DIM()));

    vix_title_label_->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
                                        .arg(ui::colors::TEXT_TERTIARY()));
    breadth_title_label_->setStyleSheet(
        QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
            .arg(ui::colors::TEXT_TERTIARY()));
    vix_label_->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                  .arg(ui::colors::TEXT_PRIMARY()));
    breadth_label_->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                      .arg(ui::colors::TEXT_PRIMARY()));
}

void MarketSentimentWidget::on_theme_changed() {
    apply_styles();
}

void MarketSentimentWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_subscribe_all();
}

void MarketSentimentWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void MarketSentimentWidget::refresh_data() {
    // User-triggered refresh — force past min_interval; producer rate limit still paces upstream.
    auto& hub = datahub::DataHub::instance();
    QStringList topics;
    topics.reserve(kSentimentSymbols.size());
    for (const auto& sym : kSentimentSymbols)
        topics.append(QStringLiteral("market:quote:") + sym);
    hub.request(topics, /*force=*/true);
}

void MarketSentimentWidget::hub_subscribe_all() {
    show_feed_status(this, feed_.summary(kSentimentSymbols), static_cast<int>(kSentimentSymbols.size()));
    // One redraw per delivery burst, not one per symbol.
    feed_.subscribe(this, kSentimentSymbols, [this]() { schedule_render([this]() { rebuild_from_cache(); }); });
    hub_active_ = true;
}

void MarketSentimentWidget::hub_unsubscribe_all() {
    QuoteSubscriptions::unsubscribe(this);
    hub_active_ = false;
}

void MarketSentimentWidget::rebuild_from_cache() {
    QVector<services::QuoteData> quotes;
    quotes.reserve(feed_.cache().size());
    for (const auto& sym : kSentimentSymbols) {
        auto it = feed_.cache().constFind(sym);
        if (it != feed_.cache().constEnd())
            quotes.append(it.value());
    }
    if (!quotes.isEmpty())
        populate(quotes);
    show_feed_status(this, feed_.summary(kSentimentSymbols), static_cast<int>(kSentimentSymbols.size()));
}

void MarketSentimentWidget::populate(const QVector<services::QuoteData>& quotes) {
    int up = 0, down = 0, flat = 0;
    bool has_vix = false;
    double vix_price = 0.0;

    for (const auto& q : quotes) {
        if (q.symbol == "^VIX") {
            has_vix = q.has_price;
            vix_price = q.price;
            continue;
        }
        // Without a change reading this symbol is not an "unchanged" stock —
        // it is not an observation, and must not pad the total.
        if (!q.has_change_pct)
            continue;
        const int move = gauges::classify_move(q.change_pct);
        if (move > 0)
            ++up;
        else if (move < 0)
            ++down;
        else
            ++flat;
    }

    const QString dim = ui::colors::TEXT_DIM();
    auto tone_color = [](gauges::Tone t) -> QString {
        switch (t) {
            case gauges::Tone::Positive:
                return ui::colors::POSITIVE();
            case gauges::Tone::Negative:
                return ui::colors::NEGATIVE();
            case gauges::Tone::Warning:
                return ui::colors::WARNING();
            case gauges::Tone::Neutral:
                break;
        }
        return ui::colors::TEXT_PRIMARY();
    };

    // VIX first — independent of the breadth observation set. A missing price
    // is unavailable, never "VIX 0.00". Same regime rule as Risk Metrics.
    if (has_vix) {
        const auto regime = gauges::vix_regime(vix_price);
        vix_label_->setText(
            QString("%1  %2").arg(QString::number(vix_price, 'f', 2), tr(gauges::vix_regime_key(regime))));
        vix_label_->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                      .arg(tone_color(gauges::vix_tone(regime))));
    } else {
        vix_label_->setText(QStringLiteral("--"));
        vix_label_->setStyleSheet(
            QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;").arg(dim));
    }

    auto* bar_layout = qobject_cast<QHBoxLayout*>(bull_bar_->parentWidget()->layout());

    const auto scored = gauges::basket_score(up, down, flat, has_vix ? std::optional<double>(vix_price) : std::nullopt);
    if (!scored) {
        // No observations: no score, no verdict, and no directional breadth.
        score_label_->setText(QStringLiteral("--"));
        score_label_->setStyleSheet(
            QString("color: %1; font-size: 24px; font-weight: bold; background: transparent;").arg(dim));
        verdict_label_->setText(QStringLiteral("—"));
        verdict_label_->setStyleSheet(
            QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;").arg(dim));
        if (bar_layout) {
            bar_layout->setStretch(0, 1);
            bar_layout->setStretch(1, 1);
            bar_layout->setStretch(2, 1);
        }
        bull_label_->setText(QString("%1 -- ").arg(QChar(0x25B2)) + tr("UP"));
        neutral_label_->setText("-- " + tr("FLAT"));
        bear_label_->setText(QString("%1 -- ").arg(QChar(0x25BC)) + tr("DOWN"));
        breadth_label_->setText(QStringLiteral("--"));
        breadth_label_->setStyleSheet(
            QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;").arg(dim));
        return;
    }

    // Same 0..100 basket score and verdict words as the Market Pulse panel.
    const QString score_color = tone_color(scored->tone);
    score_label_->setText(QString("%1 /100").arg(scored->score));
    score_label_->setStyleSheet(
        QString("color: %1; font-size: 24px; font-weight: bold; background: transparent;").arg(score_color));
    verdict_label_->setText(tr(scored->key));
    verdict_label_->setStyleSheet(
        QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;").arg(score_color));
    arrow_label_->setText(up > down   ? QString(QChar(0x25B2))
                          : up < down ? QString(QChar(0x25BC))
                                      : QString(QChar(0x2022)));

    // Bar proportions. A zero stretch would fall back to the size hint and
    // still draw a sliver, so an empty group is hidden instead.
    if (bar_layout) {
        bar_layout->setStretch(0, up);
        bar_layout->setStretch(1, flat);
        bar_layout->setStretch(2, down);
    }
    bull_bar_->setVisible(up > 0);
    neutral_bar_->setVisible(flat > 0);
    bear_bar_->setVisible(down > 0);

    bull_label_->setText(QString("%1 %2 ").arg(QChar(0x25B2)).arg(up) + tr("UP"));
    neutral_label_->setText(QString::number(flat) + " " + tr("FLAT"));
    bear_label_->setText(QString("%1 %2 ").arg(QChar(0x25BC)).arg(down) + tr("DOWN"));

    // An exact tie is neutral, not a bearish claim.
    const QString breadth_color = up > down   ? ui::colors::POSITIVE()
                                  : up < down ? ui::colors::NEGATIVE()
                                              : ui::colors::TEXT_PRIMARY();
    breadth_label_->setText(QString("%1 / %2").arg(up).arg(down));
    breadth_label_->setStyleSheet(
        QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;").arg(breadth_color));
}

void MarketSentimentWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("MARKET SENTIMENT"));
    rebuild_from_cache(); // re-derives all derived text labels from cached quotes
}

} // namespace fincept::screens::widgets
