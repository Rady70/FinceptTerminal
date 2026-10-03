// MarketLab ETF Flow & Sector Rotation (consolidated Batch E) — the shipped
// native workspace widgets over a deterministic synthetic snapshot. The two
// service bindings are stubbed and counted, so the suite proves that showing
// the screen reads stored data and starts no acquisition, and that the only
// acquisition path is the explicit Refresh button. Runs on the offscreen QPA.
#include "etf_research_fixtures.h"
#include "screens/etf_research/EtfResearchBindings.h"
#include "screens/etf_research/EtfResearchScreen.h"
#include "screens/etf_research/EtfResearchTableModel.h"
#include "screens/etf_research/EtfResearchWidgets.h"
#include "services/etf/research/EtfResearchEngine.h"
#include "services/etf/research/EtfResearchUniverse.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSplitter>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

using namespace fincept;
using namespace fincept::screens;
using namespace fincept::services::etf::research;
using namespace etfr_fixture;

namespace {
int g_loads = 0;
int g_refreshes = 0;
ResearchSnapshot g_snapshot;

ResearchSnapshot build_snapshot() {
    QString err;
    const auto u = load_universe(QLatin1String(kUniverseResourcePath), &err);
    ResearchInputs in;
    in.universe = *u;
    in.as_of = QDateTime(QDate(2026, 10, 1), QTime(12, 0), QTimeZone::UTC);
    in.known_at = in.as_of;
    in.expected_us_session = QDate(2026, 9, 30);
    const auto d = nyse_sessions(QDate(2026, 9, 30), 320);
    int k = 0;
    for (const auto& i : u->instruments) {
        if (i.symbol == QLatin1String("EWT") || i.symbol == QLatin1String("^SET.BK"))
            continue;
        in.bars.insert(i.symbol,
                       series(i.symbol, d, 50 + k, 0.0003 * (k % 7) - 0.0009, 0.03, 15 + k % 5, 1e6 * (1 + k % 3)));
        ++k;
    }
    {
        auto& xb = in.bars[QStringLiteral("XLK")].bars;
        xb[316].close = xb[318].close * 0.992; // distinct closes so each NAV dates to one session
        xb[317].close = xb[318].close * 0.996;
        xb[319].close = xb[318].close * 1.004;
        const double n0 = xb[318].close, n1 = xb[319].close;
        in.funds.insert(QStringLiteral("XLK"),
                        {capture(d[318], 6.1e8 * n0, n0, 6.1e8), capture(d[319], 6.13e8 * n1, n1, 6.13e8)});
    }
    HoldingsCapture h;
    h.captured_at = QDateTime(d[319], QTime(20, 0), QTimeZone::UTC);
    h.holdings = {{1, QStringLiteral("NVDA"), QStringLiteral("NVIDIA"), 0.14},
                  {2, QStringLiteral("AAPL"), QStringLiteral("Apple"), 0.12}};
    h.sector_weights = {{QStringLiteral("technology"), 0.99}};
    in.holdings.insert(QStringLiteral("XLK"), h);
    MeasuredMonth mm;
    mm.month = QDate(2026, 6, 1);
    mm.flow_usd = 6.7e9;
    mm.quality = QStringLiteral("CONFIRMED");
    mm.reporting_key = QStringLiteral("0000884394/");
    in.measured.insert(QStringLiteral("SPY"), {mm});
    SourceStageStatus st;
    st.stage = QStringLiteral("yahoo_history");
    st.status = QStringLiteral("PARTIAL");
    st.items_requested = 124;
    st.items_ok = 123;
    st.failed_subjects = {QStringLiteral("INTUCH.BK")};
    in.last_refresh = {st};
    in.last_refresh_run_id = QStringLiteral("etfr-fixture");
    in.last_refresh_finished = in.as_of;
    return compute_snapshot(in);
}

QStringList column_texts(QTableView* v, int col) {
    QStringList out;
    for (int r = 0; r < v->model()->rowCount(); ++r)
        out << v->model()->index(r, col).data().toString();
    return out;
}

int column_of(QTableView* v, const QString& header) {
    for (int c = 0; c < v->model()->columnCount(); ++c)
        if (v->model()->headerData(c, Qt::Horizontal).toString() == header)
            return c;
    return -1;
}
} // namespace

// Stubs for the two service bindings (the application defines them in EtfResearchBindings.cpp).
namespace fincept::screens::etfr {
Result<ResearchSnapshot> load_research_snapshot(const QDateTime&, const QDateTime&) {
    ++g_loads;
    return Result<ResearchSnapshot>::ok(g_snapshot);
}
void start_manual_refresh(RefreshProgress, std::function<void(const RefreshResult&)> done) {
    ++g_refreshes;
    RefreshResult r;
    r.ok = true;
    r.run_id = QStringLiteral("etfr-stub");
    done(r);
}
bool manual_refresh_running() {
    return false;
}
} // namespace fincept::screens::etfr

class TstEtfResearchUi : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { g_snapshot = build_snapshot(); }
    void opening_reads_store_and_starts_no_acquisition();
    void universe_first_dense_table();
    void missing_values_sort_last_in_both_directions();
    void role_chips_and_search_filter();
    void evidence_is_text_not_only_colour();
    void detail_opens_on_selection_only();
    void views_populate_lazily_with_heatmaps_and_rrg();
    void flow_heatmap_keeps_evidence_modes_apart();
    void regime_models_intl_sources_render();
    void refresh_is_the_only_acquisition_path();
    void narrow_window_stacks_detail();
    void large_universe_stays_responsive();
    void capture_writes_every_view_without_acquisition();
};

void TstEtfResearchUi::opening_reads_store_and_starts_no_acquisition() {
    g_loads = g_refreshes = 0;
    EtfResearchScreen s;
    s.resize(1500, 900);
    QSignalSpy applied(&s, &EtfResearchScreen::snapshot_applied);
    s.show();
    QVERIFY(applied.wait(10000));
    QCOMPARE(g_loads, 1);
    QCOMPARE(g_refreshes, 0);
    QCOMPARE(s.current_view(), QStringLiteral("universe"));
    // Showing it again does not read again (no polling either).
    s.hide();
    s.show();
    QTest::qWait(50);
    QCOMPARE(g_loads, 1);
}

void TstEtfResearchUi::universe_first_dense_table() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.resize(1500, 900);
    s.show();
    s.set_snapshot(g_snapshot);
    QTableView* t = s.table(QStringLiteral("universe"));
    QVERIFY(t);
    QCOMPARE(t->model()->rowCount(), g_snapshot.rows.size()); // the whole universe, no hierarchy first
    for (const QString& h : {QStringLiteral("1M"), QStringLiteral("RRG"), QStringLiteral("Flow"), QStringLiteral("Ev"),
                             QStringLiteral("Cred"), QStringLiteral("Fresh")})
        QVERIFY2(column_of(t, h) >= 0, qPrintable(h));
    QVERIFY(t->verticalHeader()->defaultSectionSize() <= 22); // dense rows
}

void TstEtfResearchUi::missing_values_sort_last_in_both_directions() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.show();
    s.set_snapshot(g_snapshot);
    QTableView* t = s.table(QStringLiteral("universe"));
    const int c1m = column_of(t, QStringLiteral("1M"));
    const int ctk = column_of(t, QStringLiteral("Ticker"));
    for (Qt::SortOrder o : {Qt::AscendingOrder, Qt::DescendingOrder}) {
        t->sortByColumn(c1m, o);
        const QStringList m = column_texts(t, c1m);
        const QStringList tk = column_texts(t, ctk);
        // EWT and ^SET.BK have no 1M value: always at the bottom, shown as a dash.
        QCOMPARE(m.last(), QStringLiteral("—"));
        QVERIFY(tk.mid(tk.size() - 2).contains(QStringLiteral("EWT")));
        QVERIFY(tk.mid(tk.size() - 2).contains(QStringLiteral("^SET.BK")));
        QVERIFY(m.first() != QStringLiteral("—"));
    }
}

void TstEtfResearchUi::role_chips_and_search_filter() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.show();
    s.set_snapshot(g_snapshot);
    QTableView* t = s.table(QStringLiteral("universe"));
    QToolButton* sectors = nullptr;
    for (auto* b : s.findChildren<QToolButton*>(QStringLiteral("etfrChip")))
        if (b->text() == QStringLiteral("US SECTORS"))
            sectors = b;
    QVERIFY(sectors);
    sectors->click();
    QCOMPARE(t->model()->rowCount(), 11);
    for (auto* b : s.findChildren<QToolButton*>(QStringLiteral("etfrChip")))
        if (b->text() == QStringLiteral("ALL"))
            b->click();
    auto* search = s.findChild<QLineEdit*>(QStringLiteral("etfrSearch"));
    QTest::keyClicks(search, QStringLiteral("Semiconductors"));
    QCOMPARE(t->model()->rowCount(), 1);
    search->clear();
    QCOMPARE(t->model()->rowCount(), g_snapshot.rows.size());
}

void TstEtfResearchUi::evidence_is_text_not_only_colour() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.show();
    s.set_snapshot(g_snapshot);
    QTableView* t = s.table(QStringLiteral("universe"));
    const int ev = column_of(t, QStringLiteral("Ev"));
    const int tk = column_of(t, QStringLiteral("Ticker"));
    const int flow = column_of(t, QStringLiteral("Flow"));
    QHash<QString, QString> evs, flows;
    for (int r = 0; r < t->model()->rowCount(); ++r) {
        evs[t->model()->index(r, tk).data().toString()] = t->model()->index(r, ev).data().toString();
        flows[t->model()->index(r, tk).data().toString()] = t->model()->index(r, flow).data().toString();
    }
    QCOMPARE(evs.value(QStringLiteral("SPY")), QStringLiteral("MEAS"));
    QVERIFY(flows.value(QStringLiteral("SPY")).startsWith(QStringLiteral("+$6.70B")));
    QCOMPARE(evs.value(QStringLiteral("XLK")), QStringLiteral("EST")); // two captures: estimated, never measured
    QCOMPARE(evs.value(QStringLiteral("XLF")), QStringLiteral("PRXY"));
    QCOMPARE(flows.value(QStringLiteral("XLF")), QStringLiteral("—")); // compact, not zero
    QCOMPARE(evs.value(QStringLiteral("EWT")), QStringLiteral("N/A"));
    // The tooltip carries method, grade and reasons.
    for (int r = 0; r < t->model()->rowCount(); ++r)
        if (t->model()->index(r, tk).data().toString() == QLatin1String("XLK"))
            QVERIFY(
                t->model()->index(r, flow).data(Qt::ToolTipRole).toString().contains(QStringLiteral("Credibility")));
}

void TstEtfResearchUi::detail_opens_on_selection_only() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.resize(1500, 900);
    s.show();
    s.set_snapshot(g_snapshot);
    QVERIFY(!s.detail_visible());
    s.select_symbol(QStringLiteral("XLK"));
    QVERIFY(s.detail_visible());
    const auto labels = s.findChildren<QLabel*>(QStringLiteral("etfrPanelText"));
    bool overview = false;
    for (auto* l : labels)
        overview = overview || (l->text().contains(QStringLiteral("Flow evidence")) &&
                                l->text().contains(QStringLiteral("Estimated, latest interval")));
    QVERIFY(overview);
    s.findChild<QToolButton*>(QStringLiteral("etfrDetailClose"))->click();
    QVERIFY(!s.detail_visible());
    // Unknown keys never open a detail.
    s.select_symbol(QStringLiteral("NOPE"));
    QVERIFY(!s.detail_visible());
}

void TstEtfResearchUi::views_populate_lazily_with_heatmaps_and_rrg() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.resize(1500, 900);
    s.show();
    s.set_snapshot(g_snapshot);
    auto heat_in = [&](const QString& view) {
        return s.view_widget(view)->findChildren<etfr::HeatmapWidget*>().value(0);
    };
    QCOMPARE(heat_in(QStringLiteral("sectors"))->tile_count(), 0); // nothing built before the view is shown
    s.show_view(QStringLiteral("sectors"));
    QCOMPARE(heat_in(QStringLiteral("sectors"))->tile_count(), 11);
    QVERIFY(s.view_widget(QStringLiteral("sectors"))->findChildren<etfr::RrgWidget*>().value(0)->series_count() >= 10);
    s.show_view(QStringLiteral("themes"));
    QCOMPARE(heat_in(QStringLiteral("themes"))->tile_count(), 24);
    s.show_view(QStringLiteral("countries"));
    QCOMPARE(heat_in(QStringLiteral("countries"))->tile_count(), 14);
    QCOMPARE(s.table(QStringLiteral("baskets"))->model()->rowCount(), 13);
    s.show_view(QStringLiteral("rrg"));
    QVERIFY(s.table(QStringLiteral("rrg"))->model()->rowCount() == 11);
}

void TstEtfResearchUi::flow_heatmap_keeps_evidence_modes_apart() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.resize(1500, 900);
    s.show();
    s.set_snapshot(g_snapshot);
    s.show_view(QStringLiteral("flow"));
    auto* mode = s.findChild<QComboBox*>(QStringLiteral("etfrFlowMode"));
    auto* heat = s.view_widget(QStringLiteral("flow"))->findChildren<etfr::HeatmapWidget*>().value(0);
    QVERIFY(heat->tile_count() > 60);
    // Default: best available evidence. Every fund with any evidence has a tile
    // value; classes keep their own tags and units, and a proxy is never dollars.
    QCOMPARE(mode->currentData().toString(), QStringLiteral("best"));
    int meas = 0, est = 0, prxy = 0, missing = 0;
    for (const auto& t : heat->tiles()) {
        if (t.tag == QLatin1String("MEAS"))
            ++meas;
        else if (t.tag == QLatin1String("EST"))
            ++est;
        else if (t.tag == QLatin1String("PRXY")) {
            ++prxy;
            QVERIFY2(!t.value_text.contains(QLatin1Char('$')), qPrintable(t.key));
            QVERIFY2(t.value_text.endsWith(QLatin1String("pp")), qPrintable(t.value_text));
        } else
            ++missing;
        if (t.value)
            QVERIFY(std::abs(*t.value) <= 1.0 + 1e-12); // scaled within its class
    }
    QVERIFY(meas >= 1); // SPY: SEC measured
    QVERIFY(est >= 1);  // XLK: two captures
    QVERIFY(prxy > 50); // everything else falls back to the labelled proxy
    QVERIFY(missing <= 3);
    mode->setCurrentIndex(mode->findData(QStringLiteral("measured")));
    QCOMPARE(s.table(QStringLiteral("flow"))->model()->rowCount(), heat->tile_count());
    auto* label = s.findChild<QLabel*>(QStringLiteral("etfrHint"));
    Q_UNUSED(label);
    // In measured mode only SPY has a value; the note counts it explicitly.
    bool found = false;
    for (auto* l : s.view_widget(QStringLiteral("flow"))->findChildren<QLabel*>())
        found = found || l->text().startsWith(QStringLiteral("1 of "));
    QVERIFY(found);
}

void TstEtfResearchUi::regime_models_intl_sources_render() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.resize(1500, 900);
    s.show();
    s.set_snapshot(g_snapshot);
    for (const QString& v : s.view_ids())
        s.show_view(v);
    QCOMPARE(s.table(QStringLiteral("sector_models"))->model()->rowCount(), 11);
    QCOMPARE(s.table(QStringLiteral("country_models"))->model()->rowCount(), 14);
    QCOMPARE(s.table(QStringLiteral("sources"))->model()->rowCount(), 1);
    const QStringList statuses = column_texts(s.table(QStringLiteral("sources")), 1);
    QCOMPARE(statuses.first(), QStringLiteral("PARTIAL")); // a partial refresh is shown as such
    const QPixmap pm = s.grab();
    QVERIFY(!pm.isNull());
}

void TstEtfResearchUi::refresh_is_the_only_acquisition_path() {
    g_loads = g_refreshes = 0;
    EtfResearchScreen s;
    s.set_autoload(false);
    s.show();
    s.set_snapshot(g_snapshot);
    for (const QString& v : s.view_ids())
        s.show_view(v);
    s.select_symbol(QStringLiteral("SPY"));
    QCOMPARE(g_refreshes, 0);
    QSignalSpy applied(&s, &EtfResearchScreen::snapshot_applied);
    s.findChild<QPushButton*>(QStringLiteral("etfrRefresh"))->click();
    QCOMPARE(g_refreshes, 1);
    QVERIFY(applied.wait(10000)); // the workspace re-reads the store after the refresh
    QCOMPARE(g_loads, 1);
}

void TstEtfResearchUi::narrow_window_stacks_detail() {
    EtfResearchScreen s;
    s.set_autoload(false);
    s.resize(1500, 900);
    s.show();
    s.set_snapshot(g_snapshot);
    s.select_symbol(QStringLiteral("XLK"));
    auto* body = s.findChild<QSplitter*>(QStringLiteral("etfrBody"));
    QCOMPARE(body->orientation(), Qt::Horizontal);
    s.resize(800, 900);
    QApplication::processEvents();
    QVERIFY2(s.minimumSizeHint().width() < 800, qPrintable(QString::number(s.minimumSizeHint().width())));
    QVERIFY(s.width() < 1000);
    QCOMPARE(body->orientation(), Qt::Vertical);
    QVERIFY(s.detail_visible());
}

void TstEtfResearchUi::large_universe_stays_responsive() {
    ResearchSnapshot big = g_snapshot;
    const auto base = g_snapshot.rows;
    for (int k = 1; k < 20; ++k)
        for (auto r : base) {
            r.inst.symbol += QStringLiteral("_%1").arg(k);
            big.rows.append(r);
        }
    QCOMPARE(big.rows.size(), base.size() * 20);
    EtfResearchScreen s;
    s.set_autoload(false);
    s.resize(1500, 900);
    s.show();
    QElapsedTimer timer;
    timer.start();
    s.set_snapshot(big);
    QApplication::processEvents();
    const qint64 ms = timer.elapsed();
    QCOMPARE(s.table(QStringLiteral("universe"))->model()->rowCount(), big.rows.size());
    QVERIFY2(ms < 3000, qPrintable(QStringLiteral("%1 ms").arg(ms)));
    timer.restart();
    s.table(QStringLiteral("universe"))->sortByColumn(6, Qt::DescendingOrder);
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QStringLiteral("sort %1 ms").arg(timer.elapsed())));
}

void TstEtfResearchUi::capture_writes_every_view_without_acquisition() {
    g_loads = g_refreshes = 0;
    QTemporaryDir dir;
    const QJsonArray files = capture_etf_research_views(g_snapshot, dir.path(), 1400, 860);
    QVERIFY(files.size() >= 15);
    QCOMPARE(g_loads, 0);
    QCOMPARE(g_refreshes, 0);
}

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TstEtfResearchUi t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_etf_research_ui.moc"
