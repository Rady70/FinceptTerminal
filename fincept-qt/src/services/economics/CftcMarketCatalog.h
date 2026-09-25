// src/services/economics/CftcMarketCatalog.h
// The supported MarketLab CFTC/COT market universe with its logical
// asset-class grouping.
//
// The market key is the exact analytical identifier the provider's `cot_codes`
// mapping uses (scripts/cftc_data.py); the CFTC contract-market code lives
// there too, so there is exactly one source of identity and the display
// catalog here cannot invent a second mapping. A Python fixture test parses
// this header and asserts the key set matches `cot_codes`, so the two cannot
// drift silently.
//
// Labels are the established CFTC-panel display names (proper nouns, not
// translated). Asset-class grouping is a presentation aid only: it never takes
// part in acquisition, interpretation or alert selection.
#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>

namespace fincept::services {

struct CftcMarketDefinition {
    QString key;         // analytical market key used by the provider
    QString label;       // established panel display name
    QString asset_class; // stable grouping code
};

/// Asset-class codes in display order.
inline QStringList cftc_asset_class_order() {
    return {
        QStringLiteral("metals"),     QStringLiteral("energy"),         QStringLiteral("agriculture"),
        QStringLiteral("currencies"), QStringLiteral("equity_indices"), QStringLiteral("interest_rates"),
        QStringLiteral("crypto"),     QStringLiteral("dollar_index"),
    };
}

inline QString cftc_asset_class_label(const QString& asset_class) {
    if (asset_class == QLatin1String("metals"))
        return QCoreApplication::translate("CftcMarketCatalog", "Metals");
    if (asset_class == QLatin1String("energy"))
        return QCoreApplication::translate("CftcMarketCatalog", "Energy");
    if (asset_class == QLatin1String("agriculture"))
        return QCoreApplication::translate("CftcMarketCatalog", "Agriculture");
    if (asset_class == QLatin1String("currencies"))
        return QCoreApplication::translate("CftcMarketCatalog", "Currencies");
    if (asset_class == QLatin1String("equity_indices"))
        return QCoreApplication::translate("CftcMarketCatalog", "Equity Indices");
    if (asset_class == QLatin1String("interest_rates"))
        return QCoreApplication::translate("CftcMarketCatalog", "Interest Rates");
    if (asset_class == QLatin1String("crypto"))
        return QCoreApplication::translate("CftcMarketCatalog", "Crypto");
    if (asset_class == QLatin1String("dollar_index"))
        return QCoreApplication::translate("CftcMarketCatalog", "US Dollar Index");
    return asset_class;
}

/// The supported market universe in its established display order.
inline QVector<CftcMarketDefinition> cftc_market_catalog() {
    return {
        {QStringLiteral("gold"), QStringLiteral("Gold"), QStringLiteral("metals")},
        {QStringLiteral("silver"), QStringLiteral("Silver"), QStringLiteral("metals")},
        {QStringLiteral("copper"), QStringLiteral("Copper"), QStringLiteral("metals")},
        {QStringLiteral("platinum"), QStringLiteral("Platinum"), QStringLiteral("metals")},
        {QStringLiteral("palladium"), QStringLiteral("Palladium"), QStringLiteral("metals")},
        {QStringLiteral("crude_oil"), QStringLiteral("Crude Oil (WTI)"), QStringLiteral("energy")},
        {QStringLiteral("natural_gas"), QStringLiteral("Natural Gas"), QStringLiteral("energy")},
        {QStringLiteral("gasoline"), QStringLiteral("Gasoline"), QStringLiteral("energy")},
        {QStringLiteral("heating_oil"), QStringLiteral("Heating Oil"), QStringLiteral("energy")},
        {QStringLiteral("corn"), QStringLiteral("Corn"), QStringLiteral("agriculture")},
        {QStringLiteral("wheat"), QStringLiteral("Wheat"), QStringLiteral("agriculture")},
        {QStringLiteral("soybeans"), QStringLiteral("Soybeans"), QStringLiteral("agriculture")},
        {QStringLiteral("cotton"), QStringLiteral("Cotton"), QStringLiteral("agriculture")},
        {QStringLiteral("coffee"), QStringLiteral("Coffee"), QStringLiteral("agriculture")},
        {QStringLiteral("sugar"), QStringLiteral("Sugar"), QStringLiteral("agriculture")},
        {QStringLiteral("cocoa"), QStringLiteral("Cocoa"), QStringLiteral("agriculture")},
        {QStringLiteral("live_cattle"), QStringLiteral("Live Cattle"), QStringLiteral("agriculture")},
        {QStringLiteral("lean_hogs"), QStringLiteral("Lean Hogs"), QStringLiteral("agriculture")},
        {QStringLiteral("euro"), QStringLiteral("Euro (EUR/USD)"), QStringLiteral("currencies")},
        {QStringLiteral("jpy"), QStringLiteral("Japanese Yen"), QStringLiteral("currencies")},
        {QStringLiteral("british_pound"), QStringLiteral("British Pound"), QStringLiteral("currencies")},
        {QStringLiteral("swiss_franc"), QStringLiteral("Swiss Franc"), QStringLiteral("currencies")},
        {QStringLiteral("canadian_dollar"), QStringLiteral("Canadian Dollar"), QStringLiteral("currencies")},
        {QStringLiteral("australian_dollar"), QStringLiteral("Australian Dollar"), QStringLiteral("currencies")},
        {QStringLiteral("s&p_500"), QStringLiteral("S&P 500"), QStringLiteral("equity_indices")},
        {QStringLiteral("nasdaq_100"), QStringLiteral("Nasdaq 100"), QStringLiteral("equity_indices")},
        {QStringLiteral("dow_jones"), QStringLiteral("Dow Jones"), QStringLiteral("equity_indices")},
        {QStringLiteral("nikkei"), QStringLiteral("Nikkei 225 (Yen)"), QStringLiteral("equity_indices")},
        {QStringLiteral("vix"), QStringLiteral("VIX"), QStringLiteral("equity_indices")},
        {QStringLiteral("treasury_bonds"), QStringLiteral("T-Bonds (30Y)"), QStringLiteral("interest_rates")},
        {QStringLiteral("treasury_notes_10y"), QStringLiteral("T-Notes (10Y)"), QStringLiteral("interest_rates")},
        {QStringLiteral("treasury_notes_5y"), QStringLiteral("T-Notes (5Y)"), QStringLiteral("interest_rates")},
        {QStringLiteral("treasury_notes_2y"), QStringLiteral("T-Notes (2Y)"), QStringLiteral("interest_rates")},
        {QStringLiteral("fed_funds"), QStringLiteral("Fed Funds"), QStringLiteral("interest_rates")},
        {QStringLiteral("bitcoin"), QStringLiteral("Bitcoin"), QStringLiteral("crypto")},
        {QStringLiteral("ether"), QStringLiteral("Ethereum"), QStringLiteral("crypto")},
        {QStringLiteral("us_dollar_index"), QStringLiteral("US Dollar Index"), QStringLiteral("dollar_index")},
    };
}

/// The catalog entry for a market key, or a placeholder whose label is the key
/// itself. An unknown key is never silently dropped: the monitor shows it with
/// an explicit unknown-market state.
inline CftcMarketDefinition cftc_market_definition(const QString& key) {
    for (const auto& market : cftc_market_catalog()) {
        if (market.key == key)
            return market;
    }
    CftcMarketDefinition unknown;
    unknown.key = key;
    unknown.label = key;
    return unknown;
}

inline bool cftc_market_is_known(const QString& key) {
    for (const auto& market : cftc_market_catalog()) {
        if (market.key == key)
            return true;
    }
    return false;
}

} // namespace fincept::services
