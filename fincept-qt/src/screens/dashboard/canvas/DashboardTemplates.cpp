#include "screens/dashboard/canvas/DashboardTemplates.h"

namespace fincept::screens {

// Helper: create a GridItem with type + 12-col layout (no instance_id)
static GridItem gi(const char* id, int x, int y, int w, int h, int mw = 2, int mh = 3) {
    GridItem item;
    item.id = id;
    item.cell = {x, y, w, h, mw, mh};
    return item;
}

QVector<DashboardTemplate> all_dashboard_templates() {
    return {

        // ── Portfolio Manager ────────────────────────────────────────────────
        {"portfolio_manager",
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Portfolio Manager"),
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Holdings, performance, risk and watchlist"),
         {
             gi("indices", 0, 0, 4, 5, 3, 4),
             gi("performance", 4, 0, 4, 5, 3, 4),
             gi("risk_metrics", 8, 0, 4, 5, 3, 4),
             gi("portfolio_summary", 0, 5, 6, 4),
             gi("watchlist", 6, 5, 6, 4),
             gi("news", 0, 9, 8, 4),
             gi("sentiment", 8, 9, 4, 4),
         }},

        // ── Hedge Fund ────────────────────────────────────────────────────────
        {"hedge_fund",
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Hedge Fund"),
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Sector heatmap, screener, risk and macro"),
         {
             gi("sector_heatmap", 0, 0, 6, 5, 3, 4),
             gi("screener", 6, 0, 6, 5, 3, 4),
             gi("risk_metrics", 0, 5, 4, 4),
             gi("performance", 4, 5, 4, 4),
             gi("top_movers", 8, 5, 4, 4),
             gi("news", 0, 9, 12, 4),
         }},

        // ── Crypto Trader ─────────────────────────────────────────────────────
        // MarketLab: quick_trade removed (no order-entry surface).
        {"crypto_trader",
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Crypto Trader"),
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Crypto prices, watchlist, sentiment and movers"),
         {
             gi("crypto", 0, 0, 6, 4),
             gi("top_movers", 6, 0, 6, 5, 3, 4),
             gi("watchlist", 0, 4, 12, 5),
             gi("sentiment", 0, 9, 6, 4),
             gi("news", 6, 9, 6, 4),
         }},

        // ── Equity Trader ─────────────────────────────────────────────────────
        {"equity_trader",
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Equity Trader"),
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Indices, forex, commodities, screener and movers"),
         {
             gi("indices", 0, 0, 4, 5, 3, 4),
             gi("forex", 4, 0, 4, 4),
             gi("commodities", 8, 0, 4, 4),
             gi("stock_quote", 0, 5, 4, 5),
             gi("screener", 4, 4, 8, 5, 3, 4),
             gi("top_movers", 0, 10, 6, 4),
             gi("news", 6, 9, 6, 4),
         }},

        // ── Macro Economist ───────────────────────────────────────────────────
        // MarketLab: econ_calendar removed (hosted macro calendar).
        {"macro_economist",
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Macro Economist"),
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Indices, commodities, performance and news"),
         {
             gi("indices", 0, 0, 6, 5, 3, 4),
             gi("commodities", 6, 0, 6, 4),
             gi("performance", 0, 5, 6, 4),
             gi("news", 0, 9, 8, 5),
             gi("risk_metrics", 8, 8, 4, 5),
         }},

        // ── Geopolitics Analyst ───────────────────────────────────────────────
        // MarketLab: econ_calendar removed (hosted macro calendar).
        {"geopolitics",
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "Geopolitics Analyst"),
         QT_TRANSLATE_NOOP("fincept::screens::DashboardTemplates", "News, sentiment and screener"),
         {
             gi("news", 0, 0, 8, 5),
             gi("sentiment", 8, 0, 4, 4),
             gi("screener", 0, 5, 6, 5, 3, 4),
             gi("indices", 6, 5, 6, 4),
         }},
    };
}

QString template_display_name_tr(const DashboardTemplate& t) {
    return QCoreApplication::translate("fincept::screens::DashboardTemplates", t.display_name.toUtf8().constData());
}

QString template_description_tr(const DashboardTemplate& t) {
    return QCoreApplication::translate("fincept::screens::DashboardTemplates", t.description.toUtf8().constData());
}

} // namespace fincept::screens
