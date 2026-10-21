#include "screens/dashboard/widgets/FedWatchHighlightsWidget.h"

#include "core/config/ProfileManager.h"
#include "screens/economics/panels/FedWatchViewModel.h"
#include "services/ResearchDataEvents.h"
#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace fincept::screens::widgets {

namespace fw = fincept::screens::fedwatch;

namespace {

QJsonObject fw_payload(const QJsonObject& envelope) {
    return envelope["data"].isObject() ? envelope["data"].toObject() : envelope;
}

QLabel* fw_cell(const QString& text, const QString& object_name, Qt::Alignment align = Qt::AlignLeft) {
    auto* l = new QLabel(text);
    l->setObjectName(object_name);
    l->setAlignment(align | Qt::AlignVCenter);
    return l;
}

/// One outcome row of the next meeting: the futures and Polymarket
/// probabilities of the same move (either may be missing).
struct FwOutcomeRow {
    int bp = 0;
    bool open = false;
    fw::Value fed, poly;
};

QVector<FwOutcomeRow> fw_outcome_rows(const fw::Meeting& m) {
    QVector<FwOutcomeRow> rows;
    auto find = [&rows](int bp, bool open) -> FwOutcomeRow& {
        for (auto& r : rows)
            if (r.bp == bp && r.open == open)
                return r;
        rows.append({bp, open, std::nullopt, std::nullopt});
        return rows.last();
    };
    for (const auto& o : m.fed.local)
        find(o.bp, o.open).fed = o.pct;
    for (const auto& o : m.poly.outcomes)
        find(o.bp, o.open).poly = o.pct;
    std::sort(rows.begin(), rows.end(), [](const FwOutcomeRow& a, const FwOutcomeRow& b) {
        return a.fed.value_or(-1) != b.fed.value_or(-1) ? a.fed.value_or(-1) > b.fed.value_or(-1)
                                                        : a.poly.value_or(-1) > b.poly.value_or(-1);
    });
    return rows;
}

} // namespace

FedWatchHighlightsWidget::FedWatchHighlightsWidget(QWidget* parent)
    : BaseWidget(tr("FEDWATCH · RATE OUTLOOK"), parent, ui::colors::INFO()) {
    body_ = new QWidget(this);
    body_->setObjectName("fwHighlights");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    // Headline: the target range now, and the next decision.
    auto* hero = new QWidget(body_);
    hero->setObjectName("fwHero");
    hero->setAttribute(Qt::WA_StyledBackground, true);
    auto* hl = new QHBoxLayout(hero);
    hl->setContentsMargins(10, 6, 10, 6);
    auto* left = new QVBoxLayout;
    left->setSpacing(0);
    target_caption_ = fw_cell(tr("FED TARGET RANGE"), QStringLiteral("fwHead"));
    target_ = fw_cell(QStringLiteral("--"), QStringLiteral("fwBig"));
    left->addWidget(target_caption_);
    left->addWidget(target_);
    auto* right = new QVBoxLayout;
    right->setSpacing(0);
    next_caption_ = fw_cell(tr("NEXT FOMC DECISION"), QStringLiteral("fwHead"), Qt::AlignRight);
    next_ = fw_cell(QStringLiteral("--"), QStringLiteral("fwMid"), Qt::AlignRight);
    right->addWidget(next_caption_);
    right->addWidget(next_);
    hl->addLayout(left);
    hl->addStretch();
    hl->addLayout(right);
    vl->addWidget(hero);

    expected_ = fw_cell(QString(), QStringLiteral("fwSub"));
    expected_->setWordWrap(true);
    vl->addWidget(expected_);

    outcomes_title_ = fw_cell(tr("NEXT MEETING · OUTCOME PROBABILITIES"), QStringLiteral("fwSection"));
    vl->addWidget(outcomes_title_);
    outcomes_ = new charts::PairBarList(tr("Fed funds futures"), tr("Polymarket"), body_);
    outcomes_->set_empty_text(tr("No outcome probabilities stored for this meeting."));
    vl->addWidget(outcomes_);

    path_title_ = fw_cell(tr("EXPECTED RATE AFTER EACH MEETING · SHADED = TODAY'S RANGE"), QStringLiteral("fwSection"));
    vl->addWidget(path_title_);
    path_ = new charts::RatePathChart(body_);
    vl->addWidget(path_, 1);
    content_layout()->addWidget(body_);

    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &FedWatchHighlightsWidget::on_result);
    // Refresh = the FedWatch screen's REFRESH: collect current observations,
    // update the Polymarket history, then re-read the workspace.
    connect(this, &BaseWidget::refresh_requested, this, [this]() { request(QStringLiteral("collect")); });
    // A refresh on the FedWatch screen saved new data: show it here too.
    connect(&services::ResearchDataEvents::instance(), &services::ResearchDataEvents::fedwatch_saved, this,
            [this](QObject* origin) {
                if (origin != this && pending_id_.isEmpty() && loaded_once_)
                    request(QStringLiteral("workspace"));
            });
    apply_styles();
    set_loading(true);
}

void FedWatchHighlightsWidget::apply_styles() {
    body_->setStyleSheet(QString("#fwHighlights QLabel{background:transparent;font-size:10px;color:%1;}"
                                 "#fwHighlights #fwHero{background:%5;border-radius:2px;}"
                                 "#fwHighlights QLabel#fwBig{font-size:20px;font-weight:bold;color:%1;}"
                                 "#fwHighlights QLabel#fwMid{font-size:13px;font-weight:bold;color:%1;}"
                                 "#fwHighlights QLabel#fwSub{color:%2;}"
                                 "#fwHighlights QLabel#fwHead{font-size:9px;font-weight:bold;color:%3;"
                                 "letter-spacing:0.5px;}"
                                 "#fwHighlights QLabel#fwSection{font-size:9px;font-weight:bold;color:%4;"
                                 "padding-top:8px;letter-spacing:0.5px;}")
                             .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(), ui::colors::TEXT_TERTIARY(),
                                  ui::colors::INFO(), ui::colors::BG_RAISED()));
    outcomes_->update();
    path_->update();
}

void FedWatchHighlightsWidget::on_theme_changed() {
    apply_styles();
}

void FedWatchHighlightsWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("FEDWATCH · RATE OUTLOOK"));
    target_caption_->setText(tr("FED TARGET RANGE"));
    next_caption_->setText(tr("NEXT FOMC DECISION"));
    outcomes_title_->setText(tr("NEXT MEETING · OUTCOME PROBABILITIES"));
    outcomes_->set_names(tr("Fed funds futures"), tr("Polymarket"));
    outcomes_->set_empty_text(tr("No outcome probabilities stored for this meeting."));
    path_title_->setText(tr("EXPECTED RATE AFTER EACH MEETING · SHADED = TODAY'S RANGE"));
    if (!last_data_.isEmpty())
        render(last_data_);
}

void FedWatchHighlightsWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!loaded_once_) {
        loaded_once_ = true;
        request(QStringLiteral("workspace")); // stored data only
    }
}

void FedWatchHighlightsWidget::request(const QString& command) {
    QStringList args;
    args << "--db" << ProfileManager::instance().profile_root() + "/fedwatch/fedwatch_history.db";
    pending_id_ = QStringLiteral("dash-fedwatch:%1:%2").arg(quintptr(this)).arg(++sequence_);
    pending_command_ = command;
    set_loading(true);
    services::EconomicsService::instance().execute("fedwatch", "fedwatch_data.py", command, args, pending_id_, true);
}

void FedWatchHighlightsWidget::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (request_id != pending_id_)
        return;
    pending_id_.clear();
    const QJsonObject data = fw_payload(result.data);
    const bool usable = result.success || (result.data["success"].toBool() && result.data["data"].isObject());
    if (pending_command_ == QLatin1String("collect")) {
        // Kept until a collection succeeds, so the re-read below cannot hide it.
        collect_error_ = usable ? QString()
                                : tr("Collection failed: %1 — stored data shown")
                                      .arg(result.error.isEmpty() ? tr("no usable response") : result.error.left(120));
        request(QStringLiteral("history_backfill"));
        return;
    }
    if (pending_command_ == QLatin1String("history_backfill")) {
        if (!usable)
            collect_error_ += (collect_error_.isEmpty() ? QString() : QStringLiteral(" · ")) +
                              tr("Polymarket history update failed: %1")
                                  .arg(result.error.isEmpty() ? tr("no usable response") : result.error.left(120));
        request(QStringLiteral("workspace"));
        emit services::ResearchDataEvents::instance().fedwatch_saved(this);
        return;
    }
    set_loading(false);
    if (!usable || !data["meetings"].isArray()) {
        set_footer(tr("Stored FedWatch data could not be read: %1").arg(result.error.left(120)), result.error, true);
        return;
    }
    last_data_ = data;
    render(data);
}

void FedWatchHighlightsWidget::render(const QJsonObject& data) {
    const fw::Workspace ws = fw::parse_workspace(data);
    const auto upcoming = ws.upcoming();

    if (ws.target_lower && ws.target_upper)
        target_->setText(fw::band_label(*ws.target_lower, *ws.target_upper));
    else
        target_->setText(QStringLiteral("—"));
    target_->setToolTip(
        ws.target_date.isValid()
            ? tr("Target range as of %1 (%2)").arg(ws.target_date.toString(Qt::ISODate), ws.target_status)
            : QString());

    if (upcoming.isEmpty()) {
        next_->setText(QStringLiteral("—"));
        expected_->setText(tr("No upcoming meeting in the stored data."));
        outcomes_->set_rows({});
        path_->set_data({}, std::nullopt, std::nullopt);
        set_footer(tr("Open Economics → FedWatch and press REFRESH to collect data."), QString(), true);
        return;
    }

    const fw::Meeting& m = *upcoming.first();
    next_->setText(m.days_until == 0 ? tr("%1 · today").arg(m.date.toString(QStringLiteral("MMM d")))
                   : m.days_until == 1
                       ? tr("%1 · tomorrow").arg(m.date.toString(QStringLiteral("MMM d")))
                       : tr("%1 · in %2 days").arg(m.date.toString(QStringLiteral("MMM d"))).arg(m.days_until));
    next_->setToolTip(fw::meeting_label(m.date));
    QString exp = tr("Futures expect %1 after this meeting").arg(fw::rate(m.fed.expected));
    QStringList moves;
    if (m.fed.expected && m.fed.expected_previous_day)
        moves << tr("%1 since yesterday").arg(fw::bp_change(*m.fed.expected - *m.fed.expected_previous_day));
    if (m.fed.expected && m.fed.expected_previous_week)
        moves << tr("%1 in a week").arg(fw::bp_change(*m.fed.expected - *m.fed.expected_previous_week));
    if (!moves.isEmpty())
        exp += QStringLiteral(" (") + moves.join(QStringLiteral(", ")) + QStringLiteral(")");
    expected_->setText(exp);

    // Outcome probabilities of the next meeting, highest futures side first.
    // Futures come from the meeting's own move distribution (`fed.local`), so
    // they show even where Polymarket lists no market.
    QVector<charts::PairRow> pairs;
    for (const auto& c : fw_outcome_rows(m)) {
        if (pairs.size() >= 4)
            break;
        if (c.fed.value_or(0) < 0.5 && c.poly.value_or(0) < 0.5)
            continue; // negligible on both sides
        pairs.append({fw::outcome_label(c.bp, c.open), c.fed, c.poly,
                      tr("%1 at the %2 meeting: futures %3, Polymarket %4")
                          .arg(fw::outcome_label(c.bp, c.open), fw::meeting_label(m.date), fw::pct(c.fed, 1),
                               fw::pct(c.poly, 1))});
    }
    outcomes_->set_rows(pairs);

    // The path over the upcoming meetings: the expected rate after each one
    // (futures side) and its most likely move (futures, else Polymarket).
    // The expected rate is probability-weighted, so it rises with any chance
    // of a hike even where a hold is most likely. The most likely path moves
    // only by each meeting's most likely outcome, from the middle of today's
    // range; a meeting without a distribution ends it.
    QVector<charts::PathPoint> points;
    std::optional<double> likely;
    if (ws.target_lower && ws.target_upper)
        likely = (*ws.target_lower + *ws.target_upper) / 2.0;
    for (int i = 0; i < upcoming.size() && i < 8; ++i) {
        const fw::Meeting& mm = *upcoming[i];
        const bool futures = !mm.fed.local.isEmpty();
        QVector<fw::Outcome> ranked = futures ? mm.fed.local : mm.poly.outcomes;
        std::sort(ranked.begin(), ranked.end(),
                  [](const fw::Outcome& a, const fw::Outcome& b) { return a.pct > b.pct; });
        auto label = [&](int k) {
            return k < ranked.size() ? QStringLiteral("%1 %2%3").arg(fw::outcome_label(ranked[k].bp, ranked[k].open),
                                                                     fw::pct(ranked[k].pct, 0),
                                                                     futures ? QString() : QStringLiteral(" (PM)"))
                                     : QString();
        };
        const QString note = ranked.isEmpty() ? QStringLiteral("—") : label(0);
        const QString note2 = label(1);
        if (likely && !ranked.isEmpty() && !ranked[0].open)
            likely = *likely + ranked[0].bp / 100.0;
        else
            likely.reset();
        points.append(
            {mm.date.toString(mm.date.year() == m.date.year() ? QStringLiteral("MMM d") : QStringLiteral("MMM d ''yy")),
             mm.fed.expected, note,
             tr("%1\nExpected (probability-weighted) rate: %2\nMost likely: %3%4\nMost likely path: %5\nSource: %6")
                 .arg(fw::meeting_label(mm.date), fw::rate(mm.fed.expected), note,
                      note2.isEmpty() ? QString() : tr(" · next: %1").arg(note2), fw::rate(likely),
                      futures ? tr("futures") : tr("Polymarket; no futures distribution stored")),
             note2, likely});
    }
    path_->set_legend(tr("Expected (average)"), tr("Most likely path"));
    path_->set_data(points, ws.target_lower, ws.target_upper);

    // Data state: which side is current and when it was observed.
    const bool fed_current = m.fed.state == QLatin1String("CURRENT");
    const bool poly_current = m.poly.state == QLatin1String("CURRENT");
    QString footer =
        tr("Futures: %1 %2 · Polymarket: %3")
            .arg(fw::state_label(m.fed.state), fw::age_text(m.fed.age_days), fw::state_label(m.poly.state));
    if (!collect_error_.isEmpty())
        footer = collect_error_ + QStringLiteral(" · ") + footer;
    set_footer(footer.simplified(),
               tr("Futures side observed %1; Polymarket observed %2. Same stored data as Economics → FedWatch.")
                   .arg(fw::utc(m.fed.observed_at), fw::utc(m.poly.observed_at)),
               !fed_current || !poly_current || !collect_error_.isEmpty());
}

} // namespace fincept::screens::widgets
