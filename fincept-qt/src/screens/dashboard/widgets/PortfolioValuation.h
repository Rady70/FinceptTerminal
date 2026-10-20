#pragma once
// PortfolioValuation — values a holding in the portfolio's currency.
//
// The Portfolio Summary tile used to multiply every quote by its share count
// and add the results up under the portfolio's currency sign, so a EUR
// portfolio holding AAPL showed dollars as euros, and a USD portfolio holding
// SAP.DE added euros to dollars. A holding is now valued in its own quote
// currency (the unit its average cost is recorded in), then converted with the
// FX quote `<FROM><TO>=X` (TO per one FROM). A holding whose currency or rate
// is not known yet is left out of the totals, which are then marked partial —
// never added in the wrong currency. Header-only, Qt Core only, testable.
#include <QString>

#include <optional>

namespace fincept::screens::widgets::valuation {

/// A quote currency in its major unit plus the factor from the quoted unit:
/// London quotes in pence ("GBp"), Johannesburg in cents ("ZAc"), Tel Aviv in
/// agorot ("ILA").
struct Currency {
    QString code;
    double factor = 1.0;
};

inline Currency normalize_currency(const QString& raw) {
    const QString t = raw.trimmed();
    if (t == QLatin1String("GBp") || t == QLatin1String("GBX"))
        return {QStringLiteral("GBP"), 0.01};
    if (t == QLatin1String("ZAc") || t == QLatin1String("ZAC"))
        return {QStringLiteral("ZAR"), 0.01};
    if (t == QLatin1String("ILA"))
        return {QStringLiteral("ILS"), 0.01};
    return {t.toUpper(), 1.0};
}

/// The Yahoo FX symbol whose price is `to` per one `from` ("EURUSD=X").
inline QString fx_symbol(const QString& from, const QString& to) {
    return from + to + QStringLiteral("=X");
}

struct HoldingInput {
    double shares = 0;
    double avg_cost = 0; ///< in the quoted unit, like the price
    bool has_price = false;
    double price = 0;
    bool has_change = false;
    double change = 0;
    QString quote_currency; ///< raw quote currency; empty = not known yet
};

struct HoldingValue {
    bool priced = false;       ///< valued at a current price (else at cost)
    bool converted = false;    ///< a portfolio-currency value exists
    double value = 0;          ///< portfolio currency
    double cost = 0;           ///< portfolio currency
    std::optional<double> day; ///< day P&L, portfolio currency
    QString why_not;           ///< why no converted value: currency / rate
};

/// `rate` is the converted price of one major unit of the holding's currency in
/// the portfolio currency (1 when they are the same; nullopt when unknown).
inline HoldingValue value_holding(const HoldingInput& h, const QString& portfolio_currency,
                                  std::optional<double> rate) {
    HoldingValue out;
    out.priced = h.has_price;
    if (h.quote_currency.isEmpty()) {
        out.why_not = QStringLiteral("currency not known yet");
        return out;
    }
    const Currency c = normalize_currency(h.quote_currency);
    double r = 1.0;
    if (c.code != portfolio_currency.trimmed().toUpper()) {
        if (!rate || *rate <= 0) {
            out.why_not = QStringLiteral("no %1→%2 rate").arg(c.code, portfolio_currency);
            return out;
        }
        r = *rate;
    }
    const double unit = c.factor * r;
    const double px = h.has_price ? h.price : h.avg_cost;
    out.converted = true;
    out.value = px * h.shares * unit;
    out.cost = h.avg_cost * h.shares * unit;
    if (h.has_price && h.has_change)
        out.day = h.change * h.shares * unit;
    return out;
}

/// A share count as entered: whole numbers without decimals, fractions with as
/// many digits as they need (0.0012 BTC is not "0.00").
inline QString shares_text(double shares) {
    if (shares == static_cast<double>(static_cast<long long>(shares)))
        return QString::number(static_cast<long long>(shares));
    QString t = QString::number(shares, 'f', 8);
    while (t.endsWith(QLatin1Char('0')))
        t.chop(1);
    if (t.endsWith(QLatin1Char('.')))
        t.chop(1);
    return t;
}

} // namespace fincept::screens::widgets::valuation
