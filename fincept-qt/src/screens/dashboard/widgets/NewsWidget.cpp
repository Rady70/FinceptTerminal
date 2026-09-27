#include "screens/dashboard/widgets/NewsWidget.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "services/news/NewsService.h"
#include "ui/theme/Theme.h"

#include <QLabel>

namespace fincept::screens::widgets {

namespace {
constexpr const char* kTopic = "news:general";
constexpr int kMaxArticles = 30; // headline cap; NewsService publishes the full feed
} // namespace

NewsWidget::NewsWidget(QWidget* parent) : BaseWidget(tr("MARKET NEWS"), parent, ui::colors::CYAN) {
    scroll_area_ = new QScrollArea;
    scroll_area_->setWidgetResizable(true);

    auto* container = new QWidget(this);
    news_layout_ = new QVBoxLayout(container);
    news_layout_->setContentsMargins(4, 4, 4, 4);
    news_layout_->setSpacing(0);
    news_layout_->addStretch();

    scroll_area_->setWidget(container);
    content_layout()->addWidget(scroll_area_);

    // Refresh button on the title bar — force-refresh through the hub.
    // Per-producer rate limit still applies, so this can't hammer upstream.
    connect(this, &BaseWidget::refresh_requested, this,
            []() { datahub::DataHub::instance().request(QString::fromLatin1(kTopic), /*force=*/true); });

    apply_styles();
    set_loading(true);
}

void NewsWidget::apply_styles() {
    scroll_area_->setStyleSheet(QString("QScrollArea { border: none; background: transparent; }"
                                        "QScrollBar:vertical { width: 6px; background: transparent; }"
                                        "QScrollBar::handle:vertical { background: %1; border-radius: 3px; }"
                                        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
                                    .arg(ui::colors::BORDER_MED()));
}

void NewsWidget::on_theme_changed() {
    apply_styles();
    if (!last_articles_.isEmpty())
        populate(last_articles_);
}

void NewsWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_subscribe();
}

void NewsWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe();
}

void NewsWidget::hub_subscribe() {
    auto& hub = datahub::DataHub::instance();
    hub.subscribe(this, QString::fromLatin1(kTopic), [this](const QVariant& v) {
        if (!v.canConvert<QVector<services::NewsArticle>>())
            return;
        set_loading(false);
        populate(v.value<QVector<services::NewsArticle>>());
    });
    hub_active_ = true;
    // Cold-start cache fallback: if the hub already has a cached value
    // (from a producer warm-up before this widget was mounted), surface it
    // immediately. The subscribe path above also delivers it, but only if
    // the value is still fresh; peek_raw() guarantees we render even on a
    // stale-but-present cache so the panel never sits blank.
    QVariant cached = hub.peek_raw(QString::fromLatin1(kTopic));
    if (cached.canConvert<QVector<services::NewsArticle>>()) {
        set_loading(false);
        populate(cached.value<QVector<services::NewsArticle>>());
    }
}

void NewsWidget::hub_unsubscribe() {
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void NewsWidget::populate(const QVector<services::NewsArticle>& articles) {
    last_articles_ = articles;

    // Clear old items (keep the stretch)
    while (news_layout_->count() > 1) {
        auto* item = news_layout_->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    if (articles.isEmpty()) {
        auto* empty = new QLabel(tr("No news available."));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QString("color: %1; font-size: 11px; padding: 12px; background: transparent;")
                                 .arg(ui::colors::TEXT_TERTIARY()));
        news_layout_->insertWidget(0, empty);
        return;
    }

    int rendered = 0;
    for (const auto& article : articles) {
        if (rendered >= kMaxArticles)
            break;
        if (article.headline.isEmpty())
            continue;

        // Time column: show the article's age ("45s", "12m", "3h", "2d") so a
        // current headline can never read as a stale date. The provider's
        // formatted timestamp was cut to its first five characters, which
        // turned "Sep 26, 02:15" into "Sep 2" for every day of the month from
        // the 10th on. Undated articles keep the provider's own text so a
        // missing date is never promoted to "now".
        QString time_str = article.sort_ts > 0 ? services::relative_time(article.sort_ts) : article.time;

        auto* row = new QWidget(this);
        row->setStyleSheet(QString("border-bottom: 1px solid %1;").arg(ui::colors::BORDER_DIM()));
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(4, 4, 4, 4);
        rl->setSpacing(8);

        if (!time_str.isEmpty()) {
            auto* time_lbl = new QLabel(time_str);
            time_lbl->setFixedWidth(36);
            time_lbl->setToolTip(article.time.isEmpty() ? time_str : article.time);
            time_lbl->setStyleSheet(
                QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::CYAN()));
            rl->addWidget(time_lbl);
        }

        auto* headline = new QLabel(article.headline);
        headline->setWordWrap(true);
        headline->setStyleSheet(
            QString("color: %1; font-size: 11px; background: transparent;").arg(ui::colors::TEXT_PRIMARY()));
        rl->addWidget(headline, 1);

        if (!article.source.isEmpty()) {
            auto* src = new QLabel(article.source);
            src->setStyleSheet(
                QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::TEXT_TERTIARY()));
            rl->addWidget(src);
        }

        // Insert before the stretch
        news_layout_->insertWidget(news_layout_->count() - 1, row);
        ++rendered;
    }
}

void NewsWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("MARKET NEWS"));
    if (!last_articles_.isEmpty())
        populate(last_articles_); // re-render "No news available." / time strings if any
}

} // namespace fincept::screens::widgets
