#include "screens/dashboard/widgets/MarketSentimentWidget.h"

#include "services/markets/MarketBreadthFeed.h"
#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QLocale>
#include <QShowEvent>
#include <QVBoxLayout>

namespace fincept::screens::widgets {

namespace {

constexpr int kStaleAfterSec = 15 * 60;

QString ms_signed(double v, const QString& unit) {
    const QString text = QString::number(std::abs(v), 'f', 1);
    if (std::abs(v) < 0.05)
        return QStringLiteral("0.0") + unit;
    return (v > 0 ? QStringLiteral("+") : QString::fromUtf8("−")) + text + unit;
}

} // namespace

MarketSentimentWidget::MarketSentimentWidget(QWidget* parent) : BaseWidget(tr("MARKET SENTIMENT"), parent) {
    body_ = new QWidget(this);
    body_->setObjectName("mktSent");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    auto* hero = new QHBoxLayout;
    score_ = new QLabel(QStringLiteral("--"));
    score_->setObjectName("msBig");
    hero->addWidget(score_);
    auto* right = new QVBoxLayout;
    right->setSpacing(0);
    rating_ = new QLabel(QStringLiteral("—"));
    rating_->setObjectName("msRating");
    basis_ = new QLabel(tr("All S&P 500 stocks · ranked against the past year"));
    basis_->setObjectName("msSub");
    right->addStretch();
    right->addWidget(rating_);
    right->addWidget(basis_);
    hero->addSpacing(8);
    hero->addLayout(right);
    hero->addStretch();
    vl->addLayout(hero);

    gauge_ = new charts::ScoreGauge(body_);
    gauge_->setToolTip(tr("Average of the five readings' ranks against their own previous 252 sessions (0–100). "
                          "Hollow marker: the previous session."));
    vl->addWidget(gauge_);
    today_ = new QLabel;
    today_->setObjectName("msSub");
    today_->setWordWrap(true);
    vl->addWidget(today_);

    parts_title_ = new QLabel(tr("FIVE BREADTH READINGS · RANK VS PAST YEAR (0–100)"));
    parts_title_->setObjectName("msHead");
    vl->addWidget(parts_title_);
    parts_ = new charts::BarList(body_);
    parts_->set_fixed_scale(100.0);
    parts_->set_empty_text(tr("Waiting for the first market-wide reading (about a minute)…"));
    vl->addWidget(parts_, 3);

    history_title_ = new QLabel(tr("SCORE · PAST 60 SESSIONS"));
    history_title_->setObjectName("msHead");
    vl->addWidget(history_title_);
    history_ = new charts::ScoreLine(body_);
    vl->addWidget(history_, 2);
    content_layout()->addWidget(body_);

    auto& feed = services::MarketBreadthFeed::instance();
    connect(&feed, &services::MarketBreadthFeed::changed, this, &MarketSentimentWidget::render);
    connect(this, &BaseWidget::refresh_requested, this, []() { services::MarketBreadthFeed::instance().request(); });

    apply_styles();
    render();
}

void MarketSentimentWidget::request_bulk_refresh() {
    services::MarketBreadthFeed::instance().request();
}

void MarketSentimentWidget::apply_styles() {
    body_->setStyleSheet(QString("#mktSent QLabel{background:transparent;font-size:10px;color:%1;}"
                                 "#mktSent QLabel#msBig{font-size:30px;font-weight:bold;}"
                                 "#mktSent QLabel#msRating{font-size:13px;font-weight:bold;}"
                                 "#mktSent QLabel#msSub{color:%2;}"
                                 "#mktSent QLabel#msHead{font-size:9px;font-weight:bold;color:%3;"
                                 "padding-top:8px;letter-spacing:0.5px;}")
                             .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(), ui::colors::INFO()));
    gauge_->update();
    parts_->update();
    history_->update();
}

void MarketSentimentWidget::on_theme_changed() {
    apply_styles();
    render();
}

void MarketSentimentWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    auto& feed = services::MarketBreadthFeed::instance();
    const QDateTime last = feed.last_success();
    if (!feed.running() && (!last.isValid() || last.secsTo(QDateTime::currentDateTimeUtc()) > kStaleAfterSec))
        feed.request();
}

void MarketSentimentWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("MARKET SENTIMENT"));
    basis_->setText(tr("All S&P 500 stocks · ranked against the past year"));
    parts_title_->setText(tr("FIVE BREADTH READINGS · RANK VS PAST YEAR (0–100)"));
    history_title_->setText(tr("SCORE · PAST 60 SESSIONS"));
    render();
}

void MarketSentimentWidget::render() {
    const auto& feed = services::MarketBreadthFeed::instance();
    const QJsonObject& d = feed.data();
    set_loading(feed.running() && d.isEmpty());
    if (d.isEmpty()) {
        if (!feed.error().isEmpty())
            set_footer(tr("Market breadth could not be collected: %1").arg(feed.error().left(160)), feed.error(), true);
        return;
    }

    const QJsonValue score_v = d.value(QStringLiteral("score"));
    const std::optional<double> score = score_v.isDouble() ? std::optional<double>(score_v.toDouble()) : std::nullopt;
    const QString color = score ? QString((*charts::score_color(*score))()) : QString(ui::colors::TEXT_DIM());
    score_->setText(score ? QString::number(*score, 'f', 0) : QStringLiteral("--"));
    score_->setStyleSheet(QString("color:%1;").arg(color));
    rating_->setText(d.value(QStringLiteral("rating")).toString().toUpper());
    rating_->setStyleSheet(QString("color:%1;").arg(color));

    QVector<charts::ScorePoint> points;
    for (const auto& v : d.value(QStringLiteral("history")).toArray()) {
        const QJsonObject h = v.toObject();
        const QDate day = QDate::fromString(h.value(QStringLiteral("date")).toString(), Qt::ISODate);
        if (day.isValid() && h.value(QStringLiteral("score")).isDouble())
            points.append({day, h.value(QStringLiteral("score")).toDouble()});
    }
    history_->set_points(points);
    gauge_->set_score(score,
                      points.size() >= 2 ? std::optional<double>(points[points.size() - 2].score) : std::nullopt);

    const QDate as_of = QDate::fromString(d.value(QStringLiteral("as_of")).toString(), Qt::ISODate);
    const QString day_text = QLocale::c().toString(as_of, QStringLiteral("d MMM"));
    const QJsonValue move = d.value(QStringLiteral("today_avg_move"));
    today_->setText(tr("%1: %2 stocks up · %3 down · average stock %4")
                        .arg(day_text)
                        .arg(d.value(QStringLiteral("up")).toInt())
                        .arg(d.value(QStringLiteral("down")).toInt())
                        .arg(move.isDouble() ? ms_signed(move.toDouble(), QStringLiteral("%")) : QStringLiteral("—")));

    QVector<charts::BarRow> rows;
    for (const auto& v : d.value(QStringLiteral("components")).toArray()) {
        const QJsonObject c = v.toObject();
        const QString label = c.value(QStringLiteral("label")).toString();
        const QString unit = c.value(QStringLiteral("unit")).toString();
        const QJsonValue val = c.value(QStringLiteral("value"));
        const QJsonValue pct = c.value(QStringLiteral("percentile"));
        const QString reading = !val.isDouble() ? QStringLiteral("—")
                                : unit == QLatin1String("%")
                                    ? (c.value(QStringLiteral("key")).toString() == QLatin1String("above_50d")
                                           ? QString::number(val.toDouble(), 'f', 0) + "%"
                                           : ms_signed(val.toDouble(), QStringLiteral("%")))
                                    : ms_signed(val.toDouble(), QStringLiteral("% net"));
        const QString tip = tr("%1\nNow: %2 · higher than %3% of the previous 252 sessions")
                                .arg(c.value(QStringLiteral("detail")).toString(), reading,
                                     pct.isDouble() ? QString::number(pct.toDouble(), 'f', 0) : QStringLiteral("—"));
        if (!pct.isDouble()) {
            rows.append({label, QStringLiteral("—"), reading, 0.0, tip, &ui::colors::TEXT_DIM});
            continue;
        }
        rows.append({label, QString::number(pct.toDouble(), 'f', 0), reading, pct.toDouble(), tip,
                     charts::score_color(pct.toDouble())});
    }
    parts_->set_rows(rows, false);

    const int members = d.value(QStringLiteral("members")).toInt();
    const int observed = d.value(QStringLiteral("observed")).toInt();
    const QJsonArray missing = d.value(QStringLiteral("missing")).toArray();
    const QDateTime got = QDateTime::fromString(d.value(QStringLiteral("retrieved_at")).toString(), Qt::ISODate);
    QString footer = tr("%1 of %2 stocks · session %3 · updated %4 UTC")
                         .arg(observed)
                         .arg(members)
                         .arg(day_text, got.toUTC().toString(QStringLiteral("HH:mm")));
    if (feed.running())
        footer += tr(" · updating…");
    QStringList tip;
    for (const auto& n : d.value(QStringLiteral("notes")).toArray())
        tip << n.toString();
    if (!missing.isEmpty()) {
        QStringList names;
        for (const auto& m : missing)
            names << m.toString();
        tip << tr("No bar for: %1").arg(names.mid(0, 30).join(QStringLiteral(", ")));
    }
    if (!feed.error().isEmpty()) {
        footer = tr("Last update failed · showing %1 · ").arg(day_text) + footer;
        tip << feed.error();
    }
    set_footer(footer, tip.join(QStringLiteral("\n")), !missing.isEmpty() || !feed.error().isEmpty());
}

} // namespace fincept::screens::widgets
