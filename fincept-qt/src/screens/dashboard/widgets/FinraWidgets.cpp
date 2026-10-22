#include "screens/dashboard/widgets/FinraWidgets.h"

#include "core/config/ProfileManager.h"
#include "services/economics/EconomicsService.h"
#include "services/markets/ShortInterestFeed.h"
#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>

namespace fincept::screens::widgets {

namespace {

constexpr int kBondStaleMs = 60 * 60 * 1000;

QString finra_signed(double v, int places, const QString& unit) {
    const QString text = QString::number(std::abs(v), 'f', places);
    if (text.toDouble() == 0.0)
        return text + unit;
    return (v > 0 ? QStringLiteral("+") : QString::fromUtf8("−")) + text + unit;
}

QString finra_day(const QString& iso, const QString& format = QStringLiteral("d MMM yyyy")) {
    return QLocale::c().toString(QDate::fromString(iso, Qt::ISODate), format);
}

QString finra_count(double v) {
    return QLocale(QLocale::English, QLocale::UnitedStates).toString(qint64(std::llround(v)));
}

QString finra_cache_dir() {
    return fincept::ProfileManager::instance().profile_root() + QStringLiteral("/short_interest");
}

QString finra_needs_key_text() {
    return QCoreApplication::translate(
               "fincept::screens::widgets::FinraWidgets",
               "Needs your FINRA API key: put it in %1 as {\"client_id\": \"…\", \"client_secret\": \"…\"} (a free "
               "Individual account's Public credential). The file is never committed.")
        .arg(services::ShortInterestFeed::key_file());
}

QScrollArea* finra_scroll(QWidget* parent, QWidget* body) {
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}"
                          "QScrollBar:vertical{width:4px;background:transparent;}"
                          "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}");
    scroll->setWidget(body);
    scroll->viewport()->setAutoFillBackground(false);
    return scroll;
}

QString finra_styles(const QString& id) {
    return QString("#%1{background:transparent;}#%1 QLabel{background:transparent;font-size:10px;color:%2;}"
                   "#%1 QLabel#fBig{font-size:26px;font-weight:bold;}"
                   "#%1 QLabel#fName{font-size:11px;font-weight:bold;}"
                   "#%1 QLabel#fSub{color:%3;}"
                   "#%1 QLabel#fHead{font-size:9px;font-weight:bold;color:%4;padding-top:6px;letter-spacing:0.5px;}"
                   "#%1 QLineEdit{background:%5;color:%2;border:1px solid %6;padding:2px 6px;font-size:11px;}"
                   "#%1 QPushButton{background:%5;color:%2;border:1px solid %6;padding:2px 8px;font-size:10px;}")
        .arg(id, ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(), ui::colors::INFO(), ui::colors::BG_RAISED(),
             ui::colors::BORDER_DIM());
}

} // namespace

// ── Short interest lookup ────────────────────────────────────────────────────

ShortInterestLookupWidget::ShortInterestLookupWidget(const QJsonObject& cfg, QWidget* parent)
    : BaseWidget(tr("SHORT INTEREST LOOKUP"), parent) {
    body_ = new QWidget;
    body_->setObjectName("finraLookup");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    auto* row = new QHBoxLayout;
    input_ = new QLineEdit;
    input_->setMaxLength(10);
    row->addWidget(input_, 1);
    auto* go = new QPushButton(tr("Look up"));
    go->setCursor(Qt::PointingHandCursor);
    row->addWidget(go);
    vl->addLayout(row);

    name_ = new QLabel;
    name_->setObjectName("fName");
    vl->addWidget(name_);
    pct_ = new QLabel(QStringLiteral("--"));
    pct_->setObjectName("fBig");
    vl->addWidget(pct_);
    detail_ = new QLabel;
    detail_->setObjectName("fSub");
    detail_->setWordWrap(true);
    vl->addWidget(detail_);
    chart_ = new charts::SeriesLine(body_);
    chart_->setMinimumHeight(90);
    vl->addWidget(chart_, 1);
    note_ = new QLabel;
    note_->setObjectName("fSub");
    note_->setWordWrap(true);
    vl->addWidget(note_);
    content_layout()->addWidget(finra_scroll(this, body_));

    auto submit = [this]() { look_up(input_->text()); };
    connect(input_, &QLineEdit::returnPressed, this, submit);
    connect(go, &QPushButton::clicked, this, submit);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &ShortInterestLookupWidget::on_result);
    connect(this, &BaseWidget::refresh_requested, this, [this]() {
        if (!symbol_.isEmpty())
            look_up(symbol_);
    });

    apply_styles();
    retranslateUi();
    apply_config(cfg);
}

QJsonObject ShortInterestLookupWidget::config() const {
    QJsonObject o;
    if (!symbol_.isEmpty())
        o.insert(QStringLiteral("symbol"), symbol_);
    return o;
}

void ShortInterestLookupWidget::apply_config(const QJsonObject& cfg) {
    const QString sym = cfg.value(QStringLiteral("symbol")).toString();
    if (!sym.isEmpty() && sym != symbol_) {
        input_->setText(sym);
        look_up(sym);
    }
}

void ShortInterestLookupWidget::request_bulk_refresh() {
    // Bulk refresh does not read FINRA for a stock nobody asked about; the
    // shown stock is re-read (the script asks FINRA at most every 12 hours).
    if (!symbol_.isEmpty())
        look_up(symbol_);
}

void ShortInterestLookupWidget::look_up(const QString& raw) {
    const QString sym = raw.trimmed().toUpper();
    static const QRegularExpression kSymbol(QStringLiteral("^[A-Z][A-Z0-9.\\-]{0,9}$"));
    if (!kSymbol.match(sym).hasMatch()) {
        error_ = tr("Type a US stock symbol, e.g. TSLA or BRK-B.");
        render();
        return;
    }
    const bool changed = sym != symbol_;
    symbol_ = sym;
    input_->setText(sym);
    if (changed) {
        data_ = {};
        emit config_changed(config());
    }
    error_.clear();
    pending_id_ = QStringLiteral("finra-stock:%1").arg(++sequence_);
    services::EconomicsService::instance().execute(
        QStringLiteral("finra_short_interest"), QStringLiteral("finra_short_interest.py"), QStringLiteral("stock"),
        {QStringLiteral("--symbol"), sym, QStringLiteral("--cache-dir"), finra_cache_dir(),
         QStringLiteral("--key-file"), services::ShortInterestFeed::key_file()},
        pending_id_, /*bypass_cache=*/true);
    render();
}

void ShortInterestLookupWidget::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (request_id != pending_id_)
        return;
    pending_id_.clear();
    const QJsonObject data = result.data.value(QStringLiteral("data")).toObject();
    if (result.success && !data.isEmpty()) {
        data_ = data;
        error_.clear();
    } else {
        error_ = result.error.isEmpty() ? tr("no usable response") : result.error;
    }
    render();
}

void ShortInterestLookupWidget::apply_styles() {
    body_->setStyleSheet(finra_styles(QStringLiteral("finraLookup")));
    chart_->update();
}

void ShortInterestLookupWidget::on_theme_changed() {
    apply_styles();
    render();
}

void ShortInterestLookupWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("SHORT INTEREST LOOKUP"));
    input_->setPlaceholderText(tr("Symbol, e.g. TSLA"));
    input_->setToolTip(tr("Reads this one stock's FINRA short-interest counts (about 18 KB the first time, then "
                          "only new counts). Nothing is read for stocks you do not look up."));
    render();
}

void ShortInterestLookupWidget::render() {
    set_loading(!pending_id_.isEmpty() && data_.isEmpty());
    const QString status = data_.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("needs_key")) {
        name_->clear();
        pct_->setText(QStringLiteral("--"));
        detail_->setText(finra_needs_key_text());
        chart_->set_series(QString(), {});
        note_->clear();
        set_footer(QString());
        return;
    }
    if (symbol_.isEmpty() || data_.isEmpty() || status == QLatin1String("no_data")) {
        name_->setText(symbol_);
        pct_->setText(QStringLiteral("--"));
        detail_->setText(!error_.isEmpty() ? tr("Could not read: %1").arg(error_.left(200))
                         : status == QLatin1String("no_data")
                             ? tr("FINRA has no short-interest count for %1.").arg(symbol_)
                         : !pending_id_.isEmpty() ? tr("Reading FINRA's counts for %1…").arg(symbol_)
                                                  : tr("Type a symbol and press Enter."));
        chart_->set_series(QString(), {});
        note_->clear();
        set_footer(QString());
        return;
    }

    const QString name = data_.value(QStringLiteral("name")).toString();
    name_->setText(name.isEmpty() ? symbol_ : QStringLiteral("%1 · %2").arg(symbol_, name));
    const QJsonValue pct = data_.value(QStringLiteral("pct"));
    pct_->setText(pct.isDouble() ? tr("%1% shorted").arg(pct.toDouble(), 0, 'f', 2) : tr("— % shorted"));
    const QJsonValue dtc = data_.value(QStringLiteral("days_to_cover"));
    const QJsonValue change = data_.value(QStringLiteral("change_pct"));
    QStringList parts;
    parts << tr("%1 shares short").arg(finra_count(data_.value(QStringLiteral("short_qty")).toDouble()));
    if (dtc.isDouble())
        parts << tr("%1 days to cover").arg(dtc.toDouble(), 0, 'f', 1);
    if (change.isDouble())
        parts << tr("%1 since %2")
                     .arg(finra_signed(change.toDouble(), 1, QStringLiteral("%")),
                          finra_day(data_.value(QStringLiteral("previous_date")).toString(), QStringLiteral("d MMM")));
    detail_->setText(parts.join(QStringLiteral(" · ")));

    QVector<charts::ValuePoint> pct_points, dtc_points;
    for (const auto& v : data_.value(QStringLiteral("history")).toArray()) {
        const QJsonObject h = v.toObject();
        const QDate day = QDate::fromString(h.value(QStringLiteral("date")).toString(), Qt::ISODate);
        if (!day.isValid())
            continue;
        if (h.value(QStringLiteral("pct")).isDouble())
            pct_points.append({day, h.value(QStringLiteral("pct")).toDouble()});
        if (h.value(QStringLiteral("days_to_cover")).isDouble())
            dtc_points.append({day, h.value(QStringLiteral("days_to_cover")).toDouble()});
    }
    if (pct_points.size() >= 2)
        chart_->set_series(tr("% shorted"), pct_points, tr("days to cover"), dtc_points);
    else
        chart_->set_series(tr("days to cover"), dtc_points);
    note_->setText(tr("Count of %1 (FINRA counts mid-month and month-end, published about 8 business days later). "
                      "%2 counts since %3.")
                       .arg(finra_day(data_.value(QStringLiteral("as_of")).toString()))
                       .arg(data_.value(QStringLiteral("counts")).toInt())
                       .arg(finra_day(data_.value(QStringLiteral("history"))
                                          .toArray()
                                          .first()
                                          .toObject()
                                          .value(QStringLiteral("date"))
                                          .toString(),
                                      QStringLiteral("MMM yyyy"))));
    note_->setToolTip(tr("% shorted = shares short ÷ Yahoo's latest share count on or before the count date (for a "
                         "company with several share classes Yahoo's count may cover all classes). Days to cover = "
                         "shares short ÷ FINRA's average daily volume (FINRA's own figure stops at 1)."));
    const QDateTime got = QDateTime::fromString(data_.value(QStringLiteral("retrieved_at")).toString(), Qt::ISODate);
    QString footer = tr("FINRA short interest · read %1 UTC").arg(got.toUTC().toString(QStringLiteral("d MMM HH:mm")));
    if (!pending_id_.isEmpty())
        footer += tr(" · updating…");
    if (!error_.isEmpty())
        footer = tr("Last read failed · ") + footer;
    set_footer(footer, error_, !error_.isEmpty());
}

// ── Bond Pulse ───────────────────────────────────────────────────────────────

BondPulseWidget::BondPulseWidget(QWidget* parent) : BaseWidget(tr("BOND PULSE"), parent) {
    body_ = new QWidget;
    body_->setObjectName("finraBonds");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);
    basis_ = new QLabel;
    basis_->setObjectName("fSub");
    basis_->setWordWrap(true);
    vl->addWidget(basis_);
    hy_ = make_block(body_, vl);
    ig_ = make_block(body_, vl);
    chart_title_ = new QLabel;
    chart_title_->setObjectName("fHead");
    vl->addWidget(chart_title_);
    chart_ = new charts::SeriesLine(body_);
    chart_->setMinimumHeight(90);
    vl->addWidget(chart_, 1);
    content_layout()->addWidget(finra_scroll(this, body_));

    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &BondPulseWidget::on_result);
    connect(this, &BaseWidget::refresh_requested, this, &BondPulseWidget::request);

    apply_styles();
    retranslateUi();
}

BondPulseWidget::Block BondPulseWidget::make_block(QWidget* parent, QVBoxLayout* into) {
    Block b;
    b.title = new QLabel;
    b.title->setObjectName("fHead");
    into->addWidget(b.title);
    b.split = new charts::SplitBar(parent);
    into->addWidget(b.split);
    b.line = new QLabel;
    b.line->setObjectName("fSub");
    b.line->setWordWrap(true);
    into->addWidget(b.line);
    b.rows = new charts::BarList(parent);
    b.rows->set_fixed_scale(100.0);
    b.rows->setMinimumHeight(3 * 20);
    into->addWidget(b.rows);
    return b;
}

void BondPulseWidget::request_bulk_refresh() {
    request();
}

void BondPulseWidget::request() {
    if (!pending_id_.isEmpty())
        return;
    pending_id_ = QStringLiteral("finra-bonds:%1").arg(++sequence_);
    services::EconomicsService::instance().execute(
        QStringLiteral("finra_bonds"), QStringLiteral("finra_bonds.py"), QStringLiteral("pulse"),
        {QStringLiteral("--cache-dir"), fincept::ProfileManager::instance().profile_root() + QStringLiteral("/bonds"),
         QStringLiteral("--key-file"), services::ShortInterestFeed::key_file()},
        pending_id_, /*bypass_cache=*/true);
    render();
}

void BondPulseWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (pending_id_.isEmpty() &&
        (!last_success_.isValid() || last_success_.msecsTo(QDateTime::currentDateTimeUtc()) > kBondStaleMs))
        request();
}

void BondPulseWidget::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (request_id != pending_id_)
        return;
    pending_id_.clear();
    const QJsonObject data = result.data.value(QStringLiteral("data")).toObject();
    if (result.success && !data.isEmpty()) {
        data_ = data;
        error_.clear();
        last_success_ = QDateTime::currentDateTimeUtc();
    } else {
        error_ = result.error.isEmpty() ? tr("no usable response") : result.error;
    }
    render();
}

void BondPulseWidget::apply_styles() {
    body_->setStyleSheet(finra_styles(QStringLiteral("finraBonds")));
    for (auto* b : {&hy_, &ig_}) {
        b->split->update();
        b->rows->update();
    }
    chart_->update();
}

void BondPulseWidget::on_theme_changed() {
    apply_styles();
    render();
}

void BondPulseWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("BOND PULSE"));
    basis_->setToolTip(
        tr("FINRA TRACE aggregates of US corporate bonds traded each day. Credit markets often show stress "
           "before stocks; high-yield (junk) bonds react first. Up vs down = bonds whose price rose vs fell that "
           "day; highs/lows = new 52-week price highs/lows; customer net buying = (customer buy − customer sell "
           "volume) ÷ both. Rows: 10-trade-date averages ranked against the previous 252 (0–100)."));
    hy_.title->setText(tr("HIGH YIELD (JUNK) BONDS"));
    ig_.title->setText(tr("INVESTMENT-GRADE BONDS"));
    hy_.rows->set_empty_text(tr("Waiting for FINRA…"));
    ig_.rows->set_empty_text(tr("Waiting for FINRA…"));
    chart_title_->setText(tr("UP VS DOWN BONDS, 10-DAY AVERAGE · % NET"));
    render();
}

void BondPulseWidget::fill_block(Block& b, const QJsonObject& c) {
    const double up = c.value(QStringLiteral("advances")).toDouble();
    const double down = c.value(QStringLiteral("declines")).toDouble();
    const double flat = c.value(QStringLiteral("unchanged")).toDouble();
    b.split->set_parts(
        {{tr("up"), int(up), &ui::colors::POSITIVE, tr("%1 bonds rose in price").arg(finra_count(up))},
         {QString(), int(flat), nullptr, tr("%1 unchanged").arg(finra_count(flat))},
         {tr("down"), int(down), &ui::colors::NEGATIVE, tr("%1 bonds fell in price").arg(finra_count(down))}});
    const QJsonObject r = c.value(QStringLiteral("readings")).toObject();
    const double buy = c.value(QStringLiteral("customer_buy")).toDouble();
    const double sell = c.value(QStringLiteral("customer_sell")).toDouble();
    const QJsonValue flow_today = r.value(QStringLiteral("net_buying")).toObject().value(QStringLiteral("today"));
    b.line->setText(
        tr("52-week highs %1 · lows %2 · customers %3")
            .arg(finra_count(c.value(QStringLiteral("highs")).toDouble()))
            .arg(finra_count(c.value(QStringLiteral("lows")).toDouble()))
            .arg(flow_today.isDouble()
                     ? (flow_today.toDouble() >= 0
                            ? tr("net buyers %1").arg(finra_signed(flow_today.toDouble(), 1, QStringLiteral("%")))
                            : tr("net sellers %1").arg(finra_signed(flow_today.toDouble(), 1, QStringLiteral("%"))))
                     : QStringLiteral("—")));
    b.line->setToolTip(tr("Customer buys %1 · sells %2 (FINRA volume)").arg(buy, 0, 'f', 0).arg(sell, 0, 'f', 0));

    struct RowDef {
        const char* key;
        QString label;
        QString unit;
        int places;
    };
    const RowDef defs[] = {{"net_advances", tr("Up vs down bonds, 10d"), QStringLiteral("% net"), 1},
                           {"net_highs", tr("52-wk highs − lows, 10d"), QString(), 0},
                           {"net_buying", tr("Customer net buying, 10d"), QStringLiteral("%"), 1}};
    QVector<charts::BarRow> rows;
    for (const auto& d : defs) {
        const QJsonObject o = r.value(QLatin1String(d.key)).toObject();
        const QJsonValue avg = o.value(QStringLiteral("average"));
        const QJsonValue pct = o.value(QStringLiteral("percentile"));
        const QJsonValue today = o.value(QStringLiteral("today"));
        const QString reading = avg.isDouble() ? finra_signed(avg.toDouble(), d.places, d.unit) : QStringLiteral("—");
        const QString tip =
            tr("10-trade-date average %1 · today %2 · higher than %3% of the previous 252 averages")
                .arg(reading, today.isDouble() ? finra_signed(today.toDouble(), d.places, d.unit) : QStringLiteral("—"),
                     pct.isDouble() ? QString::number(pct.toDouble(), 'f', 0) : QStringLiteral("—"));
        if (!pct.isDouble())
            rows.append({d.label, QStringLiteral("—"), reading, 0.0, tip, &ui::colors::TEXT_DIM});
        else
            rows.append({d.label, QString::number(pct.toDouble(), 'f', 0), reading, pct.toDouble(), tip,
                         charts::score_color(pct.toDouble())});
    }
    b.rows->set_rows(rows, false);
}

void BondPulseWidget::render() {
    set_loading(!pending_id_.isEmpty() && data_.isEmpty());
    const QString status = data_.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("needs_key")) {
        basis_->setText(finra_needs_key_text());
        return;
    }
    if (data_.isEmpty()) {
        basis_->setText(error_.isEmpty() ? tr("Reading FINRA's corporate bond data…")
                                         : tr("Could not read: %1").arg(error_.left(200)));
        return;
    }
    basis_->setText(tr("US corporate bonds traded on %1 (FINRA TRACE)")
                        .arg(finra_day(data_.value(QStringLiteral("as_of")).toString())));
    const QJsonObject cats = data_.value(QStringLiteral("categories")).toObject();
    fill_block(hy_, cats.value(QStringLiteral("high_yield")).toObject());
    fill_block(ig_, cats.value(QStringLiteral("investment_grade")).toObject());

    auto series = [&](const QString& key) {
        QVector<charts::ValuePoint> pts;
        for (const auto& v : cats.value(key).toObject().value(QStringLiteral("history")).toArray()) {
            const QJsonObject h = v.toObject();
            const QDate day = QDate::fromString(h.value(QStringLiteral("date")).toString(), Qt::ISODate);
            if (day.isValid() && h.value(QStringLiteral("net_advances_avg")).isDouble())
                pts.append({day, h.value(QStringLiteral("net_advances_avg")).toDouble()});
        }
        return pts;
    };
    chart_->set_series(tr("high yield"), series(QStringLiteral("high_yield")), tr("investment grade"),
                       series(QStringLiteral("investment_grade")));

    const QDateTime got = QDateTime::fromString(data_.value(QStringLiteral("retrieved_at")).toString(), Qt::ISODate);
    QString footer = tr("FINRA TRACE · read %1 UTC").arg(got.toUTC().toString(QStringLiteral("d MMM HH:mm")));
    if (!pending_id_.isEmpty())
        footer += tr(" · updating…");
    if (!error_.isEmpty())
        footer = tr("Last read failed · ") + footer;
    set_footer(footer, error_, !error_.isEmpty());
}

} // namespace fincept::screens::widgets
