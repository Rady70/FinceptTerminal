#include "screens/dashboard/widgets/MarketSentimentWidget.h"

#include "services/markets/MarketBreadthFeed.h"
#include "services/markets/ShortInterestFeed.h"
#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QLocale>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>

namespace fincept::screens::widgets {

namespace {

constexpr int kStaleAfterSec = 15 * 60;

QString ms_signed(double v, int places, const QString& unit) {
    const QString text = QString::number(std::abs(v), 'f', places);
    if (text.toDouble() == 0.0)
        return text + unit;
    return (v > 0 ? QStringLiteral("+") : QString::fromUtf8("−")) + text + unit;
}

QString ms_day(const QString& iso, const QString& format = QStringLiteral("d MMM")) {
    return QLocale::c().toString(QDate::fromString(iso, Qt::ISODate), format);
}

} // namespace

MarketSentimentWidget::MarketSentimentWidget(QWidget* parent) : BaseWidget(tr("MARKET SENTIMENT"), parent) {
    // The body scrolls when the tile is shorter than its content, so the
    // sections never squeeze each other out.
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}"
                          "QScrollBar:vertical{width:4px;background:transparent;}"
                          "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}");
    body_ = new QWidget;
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
    basis_ = new QLabel;
    basis_->setObjectName("msSub");
    right->addStretch();
    right->addWidget(rating_);
    right->addWidget(basis_);
    hero->addSpacing(8);
    hero->addLayout(right, 1);
    vl->addLayout(hero);

    gauge_ = new charts::ScoreGauge(body_);
    vl->addWidget(gauge_);
    spread_ = new QLabel;
    spread_->setObjectName("msSub");
    spread_->setWordWrap(true);
    vl->addWidget(spread_);

    parts_title_ = new QLabel;
    parts_title_->setObjectName("msHead");
    vl->addWidget(parts_title_);
    parts_ = new charts::BarList(body_);
    parts_->set_fixed_scale(100.0);
    parts_->setMinimumHeight(3 * 20);
    vl->addWidget(parts_, 2);

    history_title_ = new QLabel;
    history_title_->setObjectName("msHead");
    vl->addWidget(history_title_);
    history_ = new charts::ScoreLine(body_);
    history_->setMinimumHeight(80);
    vl->addWidget(history_, 2);

    si_title_ = new QLabel;
    si_title_->setObjectName("msHead");
    vl->addWidget(si_title_);
    si_rows_ = new charts::BarList(body_);
    si_rows_->set_fixed_scale(100.0);
    si_rows_->setMinimumHeight(2 * 20);
    vl->addWidget(si_rows_, 1);
    si_note_ = new QLabel;
    si_note_->setObjectName("msSub");
    si_note_->setWordWrap(true);
    si_note_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    vl->addWidget(si_note_);
    scroll->setWidget(body_);
    scroll->viewport()->setAutoFillBackground(false);
    content_layout()->addWidget(scroll);

    auto& feed = services::MarketBreadthFeed::instance();
    connect(&feed, &services::MarketBreadthFeed::changed, this, &MarketSentimentWidget::render);
    connect(&services::ShortInterestFeed::instance(), &services::ShortInterestFeed::changed, this,
            &MarketSentimentWidget::render_short_interest);
    connect(this, &BaseWidget::refresh_requested, this, []() {
        services::MarketBreadthFeed::instance().request();
        services::ShortInterestFeed::instance().request();
    });

    apply_styles();
    retranslateUi();
}

void MarketSentimentWidget::request_bulk_refresh() {
    services::MarketBreadthFeed::instance().request();
    services::ShortInterestFeed::instance().request();
}

void MarketSentimentWidget::apply_styles() {
    body_->setStyleSheet(
        QString("#mktSent{background:transparent;}#mktSent QLabel{background:transparent;font-size:10px;color:%1;}"
                "#mktSent QLabel#msBig{font-size:30px;font-weight:bold;}"
                "#mktSent QLabel#msRating{font-size:13px;font-weight:bold;}"
                "#mktSent QLabel#msSub{color:%2;}"
                "#mktSent QLabel#msHead{font-size:9px;font-weight:bold;color:%3;"
                "padding-top:8px;letter-spacing:0.5px;}")
            .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(), ui::colors::INFO()));
    gauge_->update();
    parts_->update();
    history_->update();
    si_rows_->update();
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
    services::ShortInterestFeed::instance().request_if_stale();
}

void MarketSentimentWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("MARKET SENTIMENT"));
    basis_->setText(tr("Risky vs safe S&P 500 stocks · 20 sessions"));
    const QString why =
        tr("Risk appetite: are investors buying the riskiest stocks or retreating into the safest? Idea from "
           "Kumar & Persaud (2002) and Baker & Wurgler (2006, 2007): investor mood moves volatile, speculative "
           "stocks most. Each day the S&P 500 members are sorted by how volatile they were over the 60 sessions "
           "before the last 20; the score ranks the riskiest fifth's 20-session return minus the safest fifth's "
           "against the previous 252 sessions.\n\nKnown limitation (Misina 2003, Bank of Canada): when stocks "
           "move together, a rise in risk itself can look like a fall in appetite.");
    basis_->setToolTip(why);
    gauge_->setToolTip(why + tr("\n\nHollow marker: the previous session."));
    gauge_->set_end_labels(tr("AVOIDING RISK"), tr("SEEKING RISK"));
    parts_title_->setText(tr("RISK APPETITE · RANK VS PAST YEAR (0–100)"));
    parts_->set_empty_text(tr("Waiting for the first reading over all S&P 500 stocks (about a minute)…"));
    history_title_->setText(tr("SCORE · PAST 60 SESSIONS"));
    si_title_->setText(tr("SHORT INTEREST · FINRA · TWICE A MONTH"));
    si_title_->setToolTip(
        tr("Shares sold short and not yet bought back, as a % of each company's shares, averaged over the S&P "
           "500 (Rapach, Ringgenberg & Zhou 2016, Journal of Financial Economics: among the strongest known "
           "predictors of the market's return; a high reading has been followed by weak returns). FINRA counts "
           "positions mid-month and at month-end and publishes them about eight business days later."));
    si_rows_->set_empty_text(tr("Waiting for FINRA's counts…"));
    render();
    render_short_interest();
}

void MarketSentimentWidget::render() {
    const auto& feed = services::MarketBreadthFeed::instance();
    const QJsonObject& d = feed.data();
    set_loading(feed.running() && d.isEmpty());
    if (d.isEmpty()) {
        if (!feed.error().isEmpty())
            set_footer(tr("Risk appetite could not be computed: %1").arg(feed.error().left(160)), feed.error(), true);
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

    const QString day_text = ms_day(d.value(QStringLiteral("as_of")).toString());
    const QJsonValue risky = d.value(QStringLiteral("risky_return"));
    const QJsonValue safe = d.value(QStringLiteral("safe_return"));
    spread_->setText(risky.isDouble() && safe.isDouble()
                         ? tr("20 sessions to %1: riskiest fifth %2 · safest fifth %3")
                               .arg(day_text, ms_signed(risky.toDouble(), 1, QStringLiteral("%")),
                                    ms_signed(safe.toDouble(), 1, QStringLiteral("%")))
                         : QString());

    QVector<charts::BarRow> rows;
    for (const auto& v : d.value(QStringLiteral("components")).toArray()) {
        const QJsonObject c = v.toObject();
        const QString label = c.value(QStringLiteral("label")).toString();
        const QString unit = c.value(QStringLiteral("unit")).toString();
        const QJsonValue val = c.value(QStringLiteral("value"));
        const QJsonValue pct = c.value(QStringLiteral("percentile"));
        const QString reading = !val.isDouble()                 ? QStringLiteral("—")
                                : unit == QLatin1String("corr") ? ms_signed(val.toDouble(), 2, QString())
                                                                : ms_signed(val.toDouble(), 1, QStringLiteral(" pp"));
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
    const int ranked = d.value(QStringLiteral("stocks_ranked")).toInt();
    const QJsonArray missing = d.value(QStringLiteral("missing")).toArray();
    const QDateTime got = QDateTime::fromString(d.value(QStringLiteral("retrieved_at")).toString(), Qt::ISODate);
    QString footer = tr("%1 of %2 stocks · session %3 · updated %4 UTC")
                         .arg(ranked)
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

void MarketSentimentWidget::render_short_interest() {
    const auto& feed = services::ShortInterestFeed::instance();
    const QJsonObject& d = feed.data();
    const QString status = d.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("needs_key")) {
        si_rows_->set_rows({}, false);
        si_rows_->set_empty_text(tr("Needs your FINRA API key"));
        si_note_->setText(tr("Put your FINRA API key in %1 as {\"client_id\": \"…\", \"client_secret\": \"…\"} "
                             "(a free Individual account's Public credential). The file is never committed.")
                              .arg(services::ShortInterestFeed::key_file()));
        return;
    }
    if (status != QLatin1String("ok")) {
        si_rows_->set_rows({}, false);
        si_rows_->set_empty_text(feed.running() ? tr("Reading FINRA's counts…") : tr("No short-interest reading"));
        si_note_->setText(feed.error().isEmpty()
                              ? QString()
                              : tr("Short interest could not be read: %1").arg(feed.error().left(200)));
        return;
    }

    // High short interest = heavy betting against the market: red at the top.
    auto row = [](const QString& label, const QJsonValue& pct, const QString& reading, const QString& tip) {
        if (!pct.isDouble())
            return charts::BarRow{label, QStringLiteral("—"), reading, 0.0, tip, &ui::colors::TEXT_DIM};
        return charts::BarRow{label, QString::number(pct.toDouble(), 'f', 0),    reading, pct.toDouble(),
                              tip,   charts::score_color(100.0 - pct.toDouble())};
    };
    // The bar ranks each reading's gap to its own two-year trend (the level
    // itself has drifted for years); the readings show level and trend.
    const int earlier = d.value(QStringLiteral("counts_ranked")).toInt();
    const int trend_counts = d.value(QStringLiteral("trend_counts")).toInt();
    const QJsonValue avg = d.value(QStringLiteral("average_pct"));
    const QJsonValue prev = d.value(QStringLiteral("previous_pct"));
    const QJsonValue trend = d.value(QStringLiteral("trend_pct"));
    const QJsonValue level_rank = d.value(QStringLiteral("level_percentile"));
    const QJsonValue dtc = d.value(QStringLiteral("days_to_cover"));
    const QJsonValue dtc_trend = d.value(QStringLiteral("days_to_cover_trend"));
    QString avg_text =
        avg.isDouble() ? QString::number(avg.toDouble(), 'f', 2) + QStringLiteral("%") : QStringLiteral("—");
    if (trend.isDouble())
        avg_text += tr(" · trend %1%").arg(trend.toDouble(), 0, 'f', 2);
    QString dtc_text = dtc.isDouble() ? tr("%1 d").arg(dtc.toDouble(), 0, 'f', 1) : QStringLiteral("—");
    if (dtc_trend.isDouble())
        dtc_text += tr(" · trend %1").arg(dtc_trend.toDouble(), 0, 'f', 1);
    const QString rank_tip =
        tr("Bar: how far this count sits above or below the straight-line trend of the %1 counts before it, "
           "ranked against %2 earlier counts' gaps (0–100; 50 = an ordinary gap).")
            .arg(trend_counts)
            .arg(earlier);
    QString level_tip;
    if (prev.isDouble())
        level_tip += tr("\nPrevious count: %1%.").arg(prev.toDouble(), 0, 'f', 2);
    if (level_rank.isDouble())
        level_tip += tr("\nThe plain level is higher than %1% of the counts since %2.")
                         .arg(level_rank.toDouble(), 0, 'f', 0)
                         .arg(ms_day(d.value(QStringLiteral("history_start")).toString(), QStringLiteral("MMM yyyy")));
    si_rows_->set_rows(
        {row(tr("Shorted vs its trend"), d.value(QStringLiteral("percentile")), avg_text,
             tr("Equal-weight average over S&P 500 companies of shares sold short ÷ shares outstanding.\n") + rank_tip +
                 level_tip),
         row(tr("Days to cover vs trend"), d.value(QStringLiteral("days_to_cover_percentile")), dtc_text,
             tr("All members' short interest ÷ their average daily volume: how many days of normal trading it "
                "would take short sellers to buy back.\n") +
                 rank_tip)},
        false);

    QStringList tip;
    for (const auto& n : d.value(QStringLiteral("notes")).toArray())
        tip << n.toString();
    QStringList no_count;
    for (const auto& v : d.value(QStringLiteral("no_count")).toArray())
        no_count << v.toString();
    if (!no_count.isEmpty())
        tip << tr("No FINRA count for: %1").arg(no_count.join(QStringLiteral(", ")));
    QStringList no_shares;
    for (const auto& v : d.value(QStringLiteral("no_shares")).toArray())
        no_shares << v.toString();
    if (!no_shares.isEmpty())
        tip << tr("No share count for: %1").arg(no_shares.join(QStringLiteral(", ")));
    if (!feed.error().isEmpty())
        tip << tr("Last update failed: %1").arg(feed.error());
    si_note_->setText(tr("Count of %1 · %2 of %3 companies · high = short sellers betting heavily against the "
                         "market%4")
                          .arg(ms_day(d.value(QStringLiteral("as_of")).toString(), QStringLiteral("d MMM yyyy")))
                          .arg(d.value(QStringLiteral("companies")).toInt())
                          .arg(d.value(QStringLiteral("members")).toInt()) // companies, not tickers
                          .arg(feed.error().isEmpty() ? QString() : tr(" · last update failed")));
    si_note_->setToolTip(tip.join(QStringLiteral("\n")));
}

} // namespace fincept::screens::widgets
