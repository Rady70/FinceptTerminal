#include "screens/dashboard/widgets/CnnFearGreedWidget.h"

#include "services/markets/CnnFearGreedFeed.h"
#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace fincept::screens::widgets {

namespace {

/// "extreme fear" -> "EXTREME FEAR".
QString cnn_rating_text(const QJsonValue& v) {
    return v.toString().trimmed().toUpper();
}

QString cnn_time(const QString& iso) {
    const QDateTime t = QDateTime::fromString(iso, Qt::ISODate);
    return t.isValid() ? t.toUTC().toString(QStringLiteral("d MMM HH:mm 'UTC'")) : QStringLiteral("—");
}

} // namespace

CnnFearGreedWidget::CnnFearGreedWidget(QWidget* parent)
    : BaseWidget(tr("CNN FEAR & GREED INDEX"), parent, ui::colors::AMBER()) {
    body_ = new QWidget(this);
    body_->setObjectName("cnnFg");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    auto* hero = new QHBoxLayout;
    score_ = new QLabel(QStringLiteral("--"));
    score_->setObjectName("cnnBig");
    hero->addWidget(score_);
    auto* right = new QVBoxLayout;
    right->setSpacing(0);
    rating_ = new QLabel(QStringLiteral("—"));
    rating_->setObjectName("cnnRating");
    updated_ = new QLabel;
    updated_->setObjectName("cnnSub");
    right->addStretch();
    right->addWidget(rating_);
    right->addWidget(updated_);
    hero->addSpacing(8);
    hero->addLayout(right);
    hero->addStretch();
    vl->addLayout(hero);

    gauge_ = new charts::ScoreGauge(body_);
    gauge_->setToolTip(tr("0–100. Below 25 extreme fear, 25–45 fear, 45–55 neutral, 55–75 greed, above 75 extreme "
                          "greed. Hollow marker: previous close."));
    vl->addWidget(gauge_);
    previous_ = new QLabel;
    previous_->setObjectName("cnnSub");
    previous_->setWordWrap(true);
    vl->addWidget(previous_);

    parts_title_ = new QLabel(tr("CNN'S SEVEN INDICATORS · SCORE 0–100"));
    parts_title_->setObjectName("cnnHead");
    vl->addWidget(parts_title_);
    parts_ = new charts::BarList(body_);
    parts_->set_fixed_scale(100.0);
    parts_->set_empty_text(tr("No indicator scores in CNN's data."));
    vl->addWidget(parts_, 3);

    auto* chart_row = new QHBoxLayout;
    history_title_ = new QLabel(tr("PAST YEAR"));
    history_title_->setObjectName("cnnHead");
    chart_row->addWidget(history_title_);
    chart_row->addStretch();
    chart_pick_ = new QComboBox(body_);
    chart_pick_->setAccessibleName(tr("Chart: the index or one of CNN's indicators"));
    chart_pick_->addItem(tr("Fear & Greed index"));
    chart_row->addWidget(chart_pick_, 0, Qt::AlignBottom);
    vl->addLayout(chart_row);
    history_ = new charts::ScoreLine(body_);
    vl->addWidget(history_, 2);
    series_ = new charts::SeriesLine(body_);
    series_->hide();
    vl->addWidget(series_, 2);
    series_note_ = new QLabel;
    series_note_->setObjectName("cnnSub");
    series_note_->setWordWrap(true);
    series_note_->hide();
    vl->addWidget(series_note_);
    connect(chart_pick_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CnnFearGreedWidget::render_chart);
    content_layout()->addWidget(body_);

    connect(&services::CnnFearGreedFeed::instance(), &services::CnnFearGreedFeed::changed, this,
            &CnnFearGreedWidget::on_feed_changed);
    connect(this, &BaseWidget::refresh_requested, this, []() { services::CnnFearGreedFeed::instance().request(); });
    apply_styles();
    on_feed_changed();
}

void CnnFearGreedWidget::request_bulk_refresh() {
    services::CnnFearGreedFeed::instance().request();
}

void CnnFearGreedWidget::apply_styles() {
    body_->setStyleSheet(QString("#cnnFg QLabel{background:transparent;font-size:10px;color:%1;}"
                                 "#cnnFg QLabel#cnnBig{font-size:30px;font-weight:bold;}"
                                 "#cnnFg QLabel#cnnRating{font-size:13px;font-weight:bold;}"
                                 "#cnnFg QLabel#cnnSub{color:%2;}"
                                 "#cnnFg QLabel#cnnHead{font-size:9px;font-weight:bold;color:%3;"
                                 "padding-top:8px;letter-spacing:0.5px;}"
                                 "#cnnFg QComboBox{background:%4;color:%1;border:1px solid %5;font-size:10px;"
                                 "padding:1px 6px;}"
                                 "#cnnFg QComboBox QAbstractItemView{background:%4;color:%1;border:1px solid %5;}")
                             .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(), ui::colors::AMBER(),
                                  ui::colors::BG_BASE(), ui::colors::BORDER_MED()));
    series_->update();
    gauge_->update();
    parts_->update();
    history_->update();
    if (!data_.isEmpty())
        render(); // score colours come from the theme
}

void CnnFearGreedWidget::on_theme_changed() {
    apply_styles();
}

void CnnFearGreedWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("CNN FEAR & GREED INDEX"));
    parts_title_->setText(tr("CNN'S SEVEN INDICATORS · SCORE 0–100"));
    history_title_->setText(tr("PAST YEAR"));
    if (!data_.isEmpty())
        render();
}

void CnnFearGreedWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    services::CnnFearGreedFeed::instance().request_if_stale();
}

void CnnFearGreedWidget::on_feed_changed() {
    const auto& feed = services::CnnFearGreedFeed::instance();
    set_loading(feed.running() && feed.data().isEmpty());
    data_ = feed.data();
    error_ = feed.error().left(160);
    if (data_.isEmpty()) {
        if (!error_.isEmpty())
            set_footer(tr("CNN could not be read: %1").arg(error_), feed.error(), true);
        return;
    }
    render();
}

void CnnFearGreedWidget::render() {
    const QJsonValue score_v = data_.value(QStringLiteral("score"));
    const std::optional<double> score = score_v.isDouble() ? std::optional<double>(score_v.toDouble()) : std::nullopt;
    const QString color = score ? QString((*charts::score_color(*score))()) : QString(ui::colors::TEXT_DIM());
    score_->setText(score ? QString::number(*score, 'f', 0) : QStringLiteral("--"));
    score_->setStyleSheet(QString("color:%1;").arg(color));
    rating_->setText(cnn_rating_text(data_.value(QStringLiteral("rating"))));
    rating_->setStyleSheet(QString("color:%1;").arg(color));
    updated_->setText(tr("CNN updated %1").arg(cnn_time(data_.value(QStringLiteral("updated_at")).toString())));

    std::optional<double> prev_close;
    QStringList prev;
    for (const auto& v : data_.value(QStringLiteral("previous")).toArray()) {
        const QJsonObject p = v.toObject();
        const QJsonValue s = p.value(QStringLiteral("score"));
        if (p.value(QStringLiteral("key")).toString() == QLatin1String("previous_close") && s.isDouble())
            prev_close = s.toDouble();
        prev << QStringLiteral("%1 %2").arg(p.value(QStringLiteral("label")).toString(),
                                            s.isDouble() ? QString::number(s.toDouble(), 'f', 0) : QStringLiteral("—"));
    }
    previous_->setText(prev.join(QStringLiteral("  ·  ")));
    gauge_->set_score(score, prev_close);

    QVector<charts::BarRow> rows;
    for (const auto& v : data_.value(QStringLiteral("components")).toArray()) {
        const QJsonObject c = v.toObject();
        const QJsonValue s = c.value(QStringLiteral("score"));
        const QString label = c.value(QStringLiteral("label")).toString();
        if (!s.isDouble()) {
            rows.append({label, QStringLiteral("—"), tr("missing"), 0.0,
                         tr("%1: no score in CNN's data").arg(c.value(QStringLiteral("detail")).toString()),
                         &ui::colors::TEXT_DIM});
            continue;
        }
        const double sc = s.toDouble();
        rows.append({label, QString::number(sc, 'f', 0), cnn_rating_text(c.value(QStringLiteral("rating"))), sc,
                     tr("%1 — %2\nScore %3 (%4), updated %5")
                         .arg(label, c.value(QStringLiteral("detail")).toString())
                         .arg(sc, 0, 'f', 1)
                         .arg(c.value(QStringLiteral("rating")).toString(),
                              cnn_time(c.value(QStringLiteral("updated_at")).toString())),
                     charts::score_color(sc)});
    }
    parts_->set_rows(rows, false);

    // Chart choices: the index, then each indicator by name (kept across
    // re-reads; rebuilt only when the indicator list changes).
    QStringList names{tr("Fear & Greed index")};
    for (const auto& v : data_.value(QStringLiteral("components")).toArray())
        names << v.toObject().value(QStringLiteral("label")).toString();
    QStringList current;
    for (int i = 0; i < chart_pick_->count(); ++i)
        current << chart_pick_->itemText(i);
    if (current != names) {
        const QSignalBlocker block(chart_pick_);
        const int keep = chart_pick_->currentIndex();
        chart_pick_->clear();
        chart_pick_->addItems(names);
        chart_pick_->setCurrentIndex(keep >= 0 && keep < names.size() ? keep : 0);
    }
    render_chart();

    const int missing = int(data_.value(QStringLiteral("missing")).toArray().size());
    QString footer = tr("Source: CNN · read %1").arg(cnn_time(data_.value(QStringLiteral("retrieved_at")).toString()));
    if (missing > 0)
        footer += tr(" · %1 value(s) missing").arg(missing);
    if (!error_.isEmpty())
        footer = tr("Last read failed (%1) · showing earlier data · ").arg(error_) + footer;
    set_footer(footer, data_.value(QStringLiteral("source_url")).toString(), missing > 0 || !error_.isEmpty());
}

void CnnFearGreedWidget::render_chart() {
    const int pick = chart_pick_->currentIndex();
    const QJsonArray components = data_.value(QStringLiteral("components")).toArray();
    if (pick <= 0 || pick > components.size()) {
        QVector<charts::ScorePoint> points;
        for (const auto& v : data_.value(QStringLiteral("history")).toArray()) {
            const QJsonObject h = v.toObject();
            const QDate d = QDate::fromString(h.value(QStringLiteral("date")).toString(), Qt::ISODate);
            if (d.isValid() && h.value(QStringLiteral("score")).isDouble())
                points.append({d, h.value(QStringLiteral("score")).toDouble()});
        }
        history_->set_points(points);
        history_title_->setText(tr("PAST YEAR · INDEX 0–100"));
        series_->hide();
        series_note_->hide();
        history_->show();
        return;
    }
    // One indicator: CNN's own series for it, as plotted on CNN's page.
    const QJsonObject c = components.at(pick - 1).toObject();
    auto read = [](const QJsonValue& arr) {
        QVector<charts::ValuePoint> out;
        for (const auto& v : arr.toArray()) {
            const QJsonObject o = v.toObject();
            const QDate d = QDate::fromString(o.value(QStringLiteral("date")).toString(), Qt::ISODate);
            if (d.isValid() && o.value(QStringLiteral("value")).isDouble())
                out.append({d, o.value(QStringLiteral("value")).toDouble()});
        }
        return out;
    };
    series_->set_series(c.value(QStringLiteral("series_label")).toString(), read(c.value(QStringLiteral("series"))),
                        c.value(QStringLiteral("companion_label")).toString(),
                        read(c.value(QStringLiteral("companion"))));
    const QJsonValue s = c.value(QStringLiteral("score"));
    history_title_->setText(tr("PAST YEAR · %1").arg(c.value(QStringLiteral("label")).toString().toUpper()));
    series_note_->setText(tr("%1 · score %2 (%3)")
                              .arg(c.value(QStringLiteral("detail")).toString(),
                                   s.isDouble() ? QString::number(s.toDouble(), 'f', 0) : QStringLiteral("—"),
                                   cnn_rating_text(c.value(QStringLiteral("rating")))));
    history_->hide();
    series_->show();
    series_note_->show();
}

// ── One CNN indicator ────────────────────────────────────────────────────────

CnnIndicatorWidget::CnnIndicatorWidget(const QString& key, const QString& label, QWidget* parent)
    : BaseWidget(tr("CNN · %1").arg(label.toUpper()), parent, ui::colors::AMBER()), key_(key), label_(label) {
    body_ = new QWidget(this);
    body_->setObjectName("cnnInd");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 6, 10, 6);
    vl->setSpacing(3);

    auto* hero = new QHBoxLayout;
    score_ = new QLabel(QStringLiteral("--"));
    score_->setObjectName("ciBig");
    hero->addWidget(score_);
    auto* right = new QVBoxLayout;
    right->setSpacing(0);
    rating_ = new QLabel(QStringLiteral("—"));
    rating_->setObjectName("ciRating");
    detail_ = new QLabel;
    detail_->setObjectName("ciSub");
    right->addStretch();
    right->addWidget(rating_);
    right->addWidget(detail_);
    hero->addSpacing(8);
    hero->addLayout(right, 1);
    vl->addLayout(hero);

    now_ = new QLabel;
    now_->setObjectName("ciSub");
    now_->setWordWrap(true);
    vl->addWidget(now_);
    chart_ = new charts::SeriesLine(body_);
    vl->addWidget(chart_, 1);
    content_layout()->addWidget(body_);

    connect(&services::CnnFearGreedFeed::instance(), &services::CnnFearGreedFeed::changed, this,
            &CnnIndicatorWidget::render);
    connect(this, &BaseWidget::refresh_requested, this, []() { services::CnnFearGreedFeed::instance().request(); });
    apply_styles();
    render();
}

void CnnIndicatorWidget::request_bulk_refresh() {
    services::CnnFearGreedFeed::instance().request();
}

void CnnIndicatorWidget::apply_styles() {
    body_->setStyleSheet(QString("#cnnInd QLabel{background:transparent;font-size:10px;color:%1;}"
                                 "#cnnInd QLabel#ciBig{font-size:24px;font-weight:bold;}"
                                 "#cnnInd QLabel#ciRating{font-size:11px;font-weight:bold;}"
                                 "#cnnInd QLabel#ciSub{color:%2;}")
                             .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY()));
    chart_->update();
}

void CnnIndicatorWidget::on_theme_changed() {
    apply_styles();
    render();
}

void CnnIndicatorWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    services::CnnFearGreedFeed::instance().request_if_stale();
}

void CnnIndicatorWidget::render() {
    const auto& feed = services::CnnFearGreedFeed::instance();
    set_loading(feed.running() && feed.data().isEmpty());
    const QJsonObject c = feed.component(key_);
    if (c.isEmpty()) {
        if (!feed.error().isEmpty())
            set_footer(tr("CNN could not be read: %1").arg(feed.error().left(160)), feed.error(), true);
        else if (!feed.data().isEmpty())
            set_footer(tr("CNN's data has no \"%1\" indicator").arg(label_), QString(), true);
        return;
    }
    const QJsonValue s = c.value(QStringLiteral("score"));
    const std::optional<double> score = s.isDouble() ? std::optional<double>(s.toDouble()) : std::nullopt;
    const QString color = score ? QString((*charts::score_color(*score))()) : QString(ui::colors::TEXT_DIM());
    score_->setText(score ? QString::number(*score, 'f', 0) : QStringLiteral("--"));
    score_->setStyleSheet(QString("color:%1;").arg(color));
    rating_->setText(score ? cnn_rating_text(c.value(QStringLiteral("rating"))) : tr("NO SCORE"));
    rating_->setStyleSheet(QString("color:%1;").arg(color));
    detail_->setText(c.value(QStringLiteral("detail")).toString());

    auto read = [](const QJsonValue& arr) {
        QVector<charts::ValuePoint> out;
        for (const auto& v : arr.toArray()) {
            const QJsonObject o = v.toObject();
            const QDate d = QDate::fromString(o.value(QStringLiteral("date")).toString(), Qt::ISODate);
            if (d.isValid() && o.value(QStringLiteral("value")).isDouble())
                out.append({d, o.value(QStringLiteral("value")).toDouble()});
        }
        return out;
    };
    const QString name = c.value(QStringLiteral("series_label")).toString();
    const QString second = c.value(QStringLiteral("companion_label")).toString();
    const auto a = read(c.value(QStringLiteral("series")));
    const auto b = read(c.value(QStringLiteral("companion")));
    chart_->set_series(name, a, second, b);
    QString now =
        a.isEmpty() ? tr("No series in CNN's data.") : tr("%1: %2").arg(name, QString::number(a.last().value, 'g', 6));
    if (!b.isEmpty())
        now += tr(" · %1: %2").arg(second, QString::number(b.last().value, 'g', 6));
    now_->setText(now);

    QString footer =
        tr("Source: CNN · indicator updated %1").arg(cnn_time(c.value(QStringLiteral("updated_at")).toString()));
    if (!feed.error().isEmpty())
        footer = tr("Last read failed · showing earlier data · ") + footer;
    set_footer(footer,
               feed.error().isEmpty() ? feed.data().value(QStringLiteral("source_url")).toString() : feed.error(),
               !feed.error().isEmpty() || !score);
}

} // namespace fincept::screens::widgets
