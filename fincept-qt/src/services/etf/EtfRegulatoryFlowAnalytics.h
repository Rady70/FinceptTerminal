// src/services/etf/EtfRegulatoryFlowAnalytics.h
//
// ETF Capital Flows, Batch C: research analytics of SEC N-PORT regulatory flow
// (docs/ETF_FLOW_BATCH_C_IMPLEMENTATION.md in the control repository).
//
// The input is what Batch B stores for one SEC reporting entity (a series, or
// a registrant that reports without one): per calendar month, the sales,
// redemptions and reinvested distributions a filing reports, and per report
// date the filing's net assets, each as source vintages. The output is monthly
// and stays monthly: nothing here spreads a month into days, and the only
// frequency is the calendar month the filing reports.
//
// Method `regulatory_flow_analytics_v1` (every measure of month m, USD unless
// stated; a window of n months ends at m):
//
//   net_flow                    sales - redemption + reinvestment, all three
//                               from ONE filing (the newest available at as_of
//                               that reports m); components of different
//                               filings are never mixed
//   flow_pct_prior_net_assets   net_flow / the regulatory net assets of the
//                               latest report date before the month (at most
//                               two months earlier, so the first month of a
//                               quarter uses the quarter-start value exactly
//                               and the other two use it with a lag of one or
//                               two months, which is recorded)
//   net_flow_3m, net_flow_12m   sums over 3 and 12 consecutive months
//   flow_pct_3m, flow_pct_12m   those sums over the net assets of the latest
//                               report date before the window (same rule);
//                               a 3-month window that is a filing quarter is
//                               normalized by its exact quarter-start value
//   normalized_flow_acceleration_3m
//                               flow_pct_3m(m) - flow_pct_3m(m-3): the change
//                               between consecutive non-overlapping quarters
//   flow_pct_percentile_36m     mid-rank percentile of flow_pct_prior_net_assets
//                               (m) among the usable values of the 36 months
//                               before m; needs at least 12 of them
//   net_flow_sign_balance_12m   (inflow months - outflow months) / 12 over 12
//                               consecutive months; a zero month is neither
//
// No z-score: monthly regulatory flow is heavy-tailed (A2 records single
// months of several billion dollars against a monthly norm far below that)
// and the stored history is short, so a mean/variance scale would be set by a
// handful of months; the percentile is bounded and rank-based.
//
// Rules that keep the values honest:
//   * a missing or unparseable component, a month no available filing
//     reports, a missing or non-positive denominator: the value is MISSING
//     with its reason, never zero and never estimated;
//   * a measure of a month whose own net flow is unusable carries that
//     month's reason; otherwise a window that reaches before the first
//     available month is insufficient_history, and a gap inside the history
//     is window_incomplete;
//   * every value is available from the latest `available_from` of all its
//     inputs (every month of its window and its denominator), with the weakest
//     point-in-time status among them (EtfDerivedModel.h);
//   * an amendment (NPORT-P/A) supersedes its original once it is available
//     at as_of; every filing that reports the month stays listed with its own
//     net flow, its form and, for an amendment, the accession it states it
//     amends (etf_sec_filings.amends_accession, as Batch B stored it). When
//     the selected filing is an amendment, its net flow is compared with THAT
//     accession's, never with whichever filing precedes it in acceptance
//     order, and the comparison says why when it cannot be made. A value
//     computed from a filing whose numbers differ from an earlier filing of
//     the same month is REVISED; an amendment that repeats the original's
//     numbers is not.
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfDerivedModel.h"

#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace fincept::services::etf {

inline constexpr const char* kRegulatoryFlowAnalyticsVersion = "regulatory_flow_analytics_v1";

/// The parameters of regulatory_flow_analytics_v1. Changing any of them is a
/// new method version (tst_etf_derived_calc pins the pair).
struct RegulatoryFlowParameters {
    static constexpr int kDenominatorMaxLagMonths = 2;
    static constexpr int kShortWindowMonths = 3;
    static constexpr int kLongWindowMonths = 12;
    static constexpr int kAccelerationWindowMonths = 3;
    static constexpr int kPercentileWindowMonths = 36;
    static constexpr int kPercentileMinPrior = 12;
    static constexpr int kSignBalanceMonths = 12;
};

inline QString regulatory_flow_parameters_text() {
    using P = RegulatoryFlowParameters;
    return QStringLiteral("net_flow=sales-redemption+reinvestment(one_filing);"
                          "denominator=latest_regulatory_net_assets_before_window,max_lag_months=%1;"
                          "windows=%2,%3;acceleration=%4;percentile=%5m,min_prior=%6,mid_rank;sign_balance=%7;"
                          "amendment_comparison=stated_amended_accession")
        .arg(P::kDenominatorMaxLagMonths)
        .arg(P::kShortWindowMonths)
        .arg(P::kLongWindowMonths)
        .arg(P::kAccelerationWindowMonths)
        .arg(P::kPercentileWindowMonths)
        .arg(P::kPercentileMinPrior)
        .arg(P::kSignBalanceMonths);
}

inline constexpr const char* kNportSales = "nport_sales";
inline constexpr const char* kNportRedemption = "nport_redemption";
inline constexpr const char* kNportReinvestment = "nport_reinvestment";
inline constexpr const char* kNportNetAssets = "nport_net_assets";

/// The lineage Batch B stores for one N-PORT filing (etf_sec_filings). A
/// filed document does not change, so none of it changes after it is stored.
struct SecFilingLineage {
    QString accession;
    QString form;             ///< NPORT-P or NPORT-P/A
    QString amends_accession; ///< the accession an NPORT-P/A states it amends; empty when none is stated
};

/// Every stored filing of one reporting entity, by accession.
using SecFilingLineageMap = QHash<QString, SecFilingLineage>;

inline constexpr const char* kNportAmendmentForm = "NPORT-P/A";

/// A filing that reports a month, with the net flow it reports (when all
/// three components are numbers).
struct RegulatoryMonthVintage {
    QString accession;
    QString form;             ///< empty when the filing has no stored lineage row
    QString amends_accession; ///< as stored for an NPORT-P/A
    QDate report_period;
    bool filing_recorded = false; ///< the filing's lineage row is stored
    QDateTime accepted_at;
    QDateTime available_from;
    QString revision_state;
    std::optional<double> net_flow;
};

/// How the selected filing's net flow compares with the filing it amends.
enum class AmendmentComparisonState {
    NotAnAmendment,            ///< the selected filing is an NPORT-P
    FilingRecordMissing,       ///< the selected filing has no stored lineage row
    AmendedAccessionNotStated, ///< an NPORT-P/A that names no amended accession
    AmendedFilingNotAvailable, ///< the amended accession is not a filing of the month known and available here
    NetFlowNotComparable,      ///< either filing reports no net flow for the month
    Compared,
};

inline const char* amendment_comparison_state_id(AmendmentComparisonState s) {
    switch (s) {
        case AmendmentComparisonState::NotAnAmendment:
            return "not_an_amendment";
        case AmendmentComparisonState::FilingRecordMissing:
            return "filing_record_missing";
        case AmendmentComparisonState::AmendedAccessionNotStated:
            return "amended_accession_not_stated";
        case AmendmentComparisonState::AmendedFilingNotAvailable:
            return "amended_filing_not_available";
        case AmendmentComparisonState::NetFlowNotComparable:
            return "net_flow_not_comparable";
        case AmendmentComparisonState::Compared:
            return "compared";
    }
    return "";
}

struct AmendmentComparison {
    AmendmentComparisonState state = AmendmentComparisonState::NotAnAmendment;
    QString selected_form;
    QString amends_accession;              ///< the stated amended accession
    std::optional<double> net_flow_change; ///< selected net flow - the amended filing's (Compared only)
};

struct RegulatoryMonthInputs {
    QDate month; ///< first day of the calendar month
    bool available = false;
    QString selected_accession;
    QDateTime accepted_at;
    SelectedInput sales;
    SelectedInput redemption;
    SelectedInput reinvestment;
    QVector<RegulatoryMonthVintage> vintages; ///< every filing available at as_of that reports the month, oldest first
    AmendmentComparison amendment;            ///< the selected filing against the accession it states it amends

    QDate month_end() const { return month.addMonths(1).addDays(-1); }
};

/// The regulatory net assets a normalized value divides by.
struct NetAssetsDenominator {
    bool found = false;
    QDate report_date;
    int lag_months = 0; ///< months between the report date and the month before the window starts
    SelectedInput input;
};

struct RegulatoryMonthResult {
    RegulatoryMonthInputs inputs;
    DerivedValue net_flow;
    DerivedValue flow_pct_prior_net_assets;
    DerivedValue net_flow_3m;
    DerivedValue net_flow_12m;
    DerivedValue flow_pct_3m;
    DerivedValue flow_pct_12m;
    DerivedValue normalized_flow_acceleration_3m;
    DerivedValue flow_pct_percentile_36m;
    DerivedValue net_flow_sign_balance_12m;
    NetAssetsDenominator denominator_1m;
    NetAssetsDenominator denominator_3m;
    NetAssetsDenominator denominator_12m;
};

struct RegulatoryNetAssetsInput {
    QDate report_date;
    SelectedInput input;
};

struct RegulatoryFlowAnalytics {
    DerivedTimeFrame frame;
    QVector<RegulatoryMonthResult> months;        ///< first to last available month, gaps included
    QVector<RegulatoryNetAssetsInput> net_assets; ///< every report date with an available vintage
};

namespace regulatory_detail {

inline QDate month_key(const QDate& d) {
    return QDate(d.year(), d.month(), 1);
}

inline int months_between(const QDate& from_key, const QDate& to_key) {
    return (to_key.year() - from_key.year()) * 12 + (to_key.month() - from_key.month());
}

/// Vintages of one measure, by effective date.
using KeyMap = QMap<QDate, QVector<StoredObservation>>;

inline const StoredObservation* find_in(const QVector<StoredObservation>& known, const QString& accession) {
    for (const StoredObservation& v : known) {
        if (v.source_document == accession)
            return &v;
    }
    return nullptr;
}

inline SelectedInput selected_from(const QVector<StoredObservation>& known, const StoredObservation& v) {
    SelectedInput s;
    s.present = true;
    s.observation_id = v.observation_id;
    s.value = v.value;
    qsizetype idx = -1;
    for (qsizetype i = 0; i < known.size(); ++i) {
        if (known[i].observation_id == v.observation_id)
            idx = i;
    }
    s.quality = derived_input_quality(known, idx);
    s.available_from = v.available_from;
    s.point_in_time_status =
        point_in_time_status_from_id(v.point_in_time_status).value_or(PointInTimeStatus::NotPointInTime);
    s.revision_state = v.revision_state;
    s.units = v.units;
    s.basis = v.basis;
    s.source_document = v.source_document;
    s.accepted_at = v.accepted_at;
    s.vintages_known = static_cast<int>(known.size());
    return s;
}

inline bool available_at(const StoredObservation& v, const QDateTime& as_of) {
    return v.available_from.isValid() && as_of.isValid() && v.available_from <= as_of;
}

inline std::optional<double> net_of(const SelectedInput& s, const SelectedInput& r, const SelectedInput& d) {
    if (!s.usable_number() || !r.usable_number() || !d.usable_number())
        return std::nullopt;
    return s.value.value - r.value.value + d.value.value;
}

} // namespace regulatory_detail

/// The selected filing (the last of `vintages`) against the accession it
/// states it amends, among the month's filings known and available here.
inline AmendmentComparison compare_with_amended(const QVector<RegulatoryMonthVintage>& vintages) {
    AmendmentComparison c;
    if (vintages.isEmpty())
        return c;
    const RegulatoryMonthVintage& selected = vintages.last();
    c.selected_form = selected.form;
    c.amends_accession = selected.amends_accession;
    if (!selected.filing_recorded) {
        c.state = AmendmentComparisonState::FilingRecordMissing;
        return c;
    }
    if (selected.form != QLatin1String(kNportAmendmentForm)) {
        c.state = AmendmentComparisonState::NotAnAmendment;
        return c;
    }
    if (selected.amends_accession.isEmpty()) {
        c.state = AmendmentComparisonState::AmendedAccessionNotStated;
        return c;
    }
    const RegulatoryMonthVintage* amended = nullptr;
    for (const RegulatoryMonthVintage& v : vintages) {
        if (v.accession == selected.amends_accession)
            amended = &v;
    }
    if (!amended) {
        c.state = AmendmentComparisonState::AmendedFilingNotAvailable;
        return c;
    }
    if (!selected.net_flow || !amended->net_flow) {
        c.state = AmendmentComparisonState::NetFlowNotComparable;
        return c;
    }
    c.state = AmendmentComparisonState::Compared;
    c.net_flow_change = *selected.net_flow - *amended->net_flow;
    return c;
}

/// Compute every monthly measure of one reporting entity. `observations` are
/// all stored vintages of the entity (any measure; other sources are ignored);
/// `filings` the entity's stored filing lineage, by accession.
inline RegulatoryFlowAnalytics compute_regulatory_flow_analytics(const QVector<StoredObservation>& observations,
                                                                 const DerivedTimeFrame& tf,
                                                                 const SecFilingLineageMap& filings) {
    using namespace regulatory_detail;
    using P = RegulatoryFlowParameters;
    RegulatoryFlowAnalytics out;
    out.frame = tf;
    if (!tf.valid())
        return out;

    QHash<QString, KeyMap> by_measure;
    for (const StoredObservation& o : observations) {
        if (o.source_type != QLatin1String(source_type_id(SourceType::SecNport)))
            continue;
        by_measure[o.measure][o.effective_date].append(o);
    }
    // Copies (implicitly shared): a reference from QHash::operator[] would not
    // survive the insertion the next operator[] may make.
    const KeyMap sales = by_measure.value(QLatin1String(kNportSales));
    const KeyMap redemption = by_measure.value(QLatin1String(kNportRedemption));
    const KeyMap reinvestment = by_measure.value(QLatin1String(kNportReinvestment));
    const KeyMap net_assets = by_measure.value(QLatin1String(kNportNetAssets));

    // ── Inputs of each month: one filing, the newest available at as_of ──────
    QMap<QDate, RegulatoryMonthInputs> month_inputs;
    QSet<QDate> month_ends;
    for (const KeyMap* m : {&sales, &redemption, &reinvestment}) {
        for (auto it = m->cbegin(); it != m->cend(); ++it)
            month_ends.insert(it.key());
    }
    for (const QDate& end : month_ends) {
        const QVector<StoredObservation> ks = known_vintages(sales.value(end), tf.known_at);
        const QVector<StoredObservation> kr = known_vintages(redemption.value(end), tf.known_at);
        const QVector<StoredObservation> kd = known_vintages(reinvestment.value(end), tf.known_at);
        // Every filing available at as_of that reports this month, oldest first.
        QMap<QString, const StoredObservation*> representative;
        for (const QVector<StoredObservation>* known : {&ks, &kr, &kd}) {
            for (const StoredObservation& v : *known) {
                if (available_at(v, tf.as_of) && !representative.contains(v.source_document))
                    representative.insert(v.source_document, &v);
            }
        }
        RegulatoryMonthInputs mi;
        mi.month = month_key(end);
        if (representative.isEmpty()) {
            month_inputs.insert(mi.month, mi);
            continue;
        }
        QVector<const StoredObservation*> reporting(representative.cbegin(), representative.cend());
        std::stable_sort(reporting.begin(), reporting.end(),
                         [](const StoredObservation* a, const StoredObservation* b) { return vintage_before(*a, *b); });
        for (const StoredObservation* f : reporting) {
            RegulatoryMonthVintage mv;
            mv.accession = f->source_document;
            if (const auto lineage = filings.constFind(f->source_document); lineage != filings.cend()) {
                mv.filing_recorded = true;
                mv.form = lineage->form;
                mv.amends_accession = lineage->amends_accession;
            }
            mv.report_period = f->report_period;
            mv.accepted_at = f->accepted_at;
            mv.available_from = f->available_from;
            mv.revision_state = f->revision_state;
            const StoredObservation* s = find_in(ks, f->source_document);
            const StoredObservation* r = find_in(kr, f->source_document);
            const StoredObservation* d = find_in(kd, f->source_document);
            if (s && r && d && s->value.reported() && r->value.reported() && d->value.reported())
                mv.net_flow = s->value.value - r->value.value + d->value.value;
            mi.vintages.append(mv);
        }
        const StoredObservation* chosen = reporting.last();
        mi.available = true;
        mi.selected_accession = chosen->source_document;
        mi.accepted_at = chosen->accepted_at;
        // A component the chosen filing does not carry is not taken from
        // another filing: it stays absent (present == false).
        if (const StoredObservation* s = find_in(ks, chosen->source_document); s && available_at(*s, tf.as_of))
            mi.sales = selected_from(ks, *s);
        if (const StoredObservation* r = find_in(kr, chosen->source_document); r && available_at(*r, tf.as_of))
            mi.redemption = selected_from(kr, *r);
        if (const StoredObservation* d = find_in(kd, chosen->source_document); d && available_at(*d, tf.as_of))
            mi.reinvestment = selected_from(kd, *d);
        mi.amendment = compare_with_amended(mi.vintages);
        month_inputs.insert(mi.month, mi);
    }

    // ── Net assets per report date ───────────────────────────────────────────
    QMap<QDate, SelectedInput> na_selected;
    for (auto it = net_assets.cbegin(); it != net_assets.cend(); ++it) {
        const SelectedInput s = select_input(it.value(), tf);
        if (s.present) {
            na_selected.insert(it.key(), s);
            out.net_assets.append({it.key(), s});
        }
    }

    // ── The month range: first to last month with an available filing ───────
    QDate first_month;
    QDate last_month;
    for (auto it = month_inputs.cbegin(); it != month_inputs.cend(); ++it) {
        if (!it.value().available)
            continue;
        if (!first_month.isValid() || it.key() < first_month)
            first_month = it.key();
        if (!last_month.isValid() || it.key() > last_month)
            last_month = it.key();
    }
    if (!first_month.isValid())
        return out;

    // The denominator of a window that starts in month `start`: the regulatory
    // net assets of the latest report date among the month ends before it,
    // at most kDenominatorMaxLagMonths months earlier than the month before it.
    auto denominator_for = [&](const QDate& start) {
        NetAssetsDenominator d;
        for (int lag = 0; lag <= P::kDenominatorMaxLagMonths; ++lag) {
            const QDate date = start.addMonths(-lag).addDays(-1);
            const auto it = na_selected.constFind(date);
            if (it == na_selected.cend())
                continue;
            d.found = true;
            d.report_date = date;
            d.lag_months = lag;
            d.input = it.value();
            return d;
        }
        return d;
    };

    const int n_months = months_between(first_month, last_month) + 1;
    QVector<InputAccumulator> net_inputs(n_months);
    out.months.reserve(n_months);
    for (int i = 0; i < n_months; ++i) {
        const QDate month = first_month.addMonths(i);
        RegulatoryMonthResult r;
        r.inputs = month_inputs.value(month);
        r.inputs.month = month;

        // net_flow
        if (!r.inputs.available) {
            r.net_flow = DerivedValue::unavailable(DerivedReason::MonthNotAvailable);
        } else {
            const auto net = net_of(r.inputs.sales, r.inputs.redemption, r.inputs.reinvestment);
            if (!net) {
                r.net_flow = DerivedValue::unavailable(DerivedReason::ComponentMissing);
            } else {
                InputAccumulator acc;
                const QDate end = month.addMonths(1).addDays(-1);
                acc.add(r.inputs.sales, end);
                acc.add(r.inputs.redemption, end);
                acc.add(r.inputs.reinvestment, end);
                net_inputs[i] = acc;
                r.net_flow = DerivedValue::computed(*net, acc, QualityState::Confirmed);
            }
        }
        out.months.append(r);
    }

    // Sum of net_flow over the n months ending at month index i. A month whose
    // own net flow is unusable passes its own reason on; otherwise a window
    // reaching before the first month is insufficient_history, and one with an
    // unusable earlier month is window_incomplete.
    struct WindowSum {
        std::optional<double> sum;
        DerivedReason reason = DerivedReason::None;
        InputAccumulator inputs;
    };
    auto window_sum = [&](int i, int n) {
        WindowSum w;
        if (!out.months[i].net_flow.usable()) {
            w.reason = out.months[i].net_flow.reason;
            return w;
        }
        if (i - n + 1 < 0) {
            w.reason = DerivedReason::InsufficientHistory;
            return w;
        }
        double sum = 0.0;
        for (int k = i - n + 1; k <= i; ++k) {
            const DerivedValue& v = out.months[k].net_flow;
            if (!v.usable()) {
                w.reason = DerivedReason::WindowIncomplete;
                return w;
            }
            sum += *v.value;
            w.inputs.merge(net_inputs[k]);
        }
        w.sum = sum;
        return w;
    };
    // A window sum over its denominator.
    auto normalized = [&](const WindowSum& w, const NetAssetsDenominator& d) {
        if (!w.sum)
            return DerivedValue::unavailable(w.reason);
        if (!d.found)
            return DerivedValue::unavailable(DerivedReason::DenominatorUnavailable);
        if (!d.input.value.reported())
            return DerivedValue::unavailable(DerivedReason::DenominatorMissingValue);
        if (!(d.input.value.value > 0.0) || !std::isfinite(d.input.value.value))
            return DerivedValue::unavailable(DerivedReason::InvalidDenominator);
        InputAccumulator acc = w.inputs;
        acc.add(d.input, d.report_date);
        return DerivedValue::computed(*w.sum / d.input.value.value, acc, QualityState::Confirmed);
    };

    for (int i = 0; i < n_months; ++i) {
        RegulatoryMonthResult& r = out.months[i];
        const QDate month = first_month.addMonths(i);

        const WindowSum one = window_sum(i, 1);
        r.denominator_1m = denominator_for(month);
        r.flow_pct_prior_net_assets = normalized(one, r.denominator_1m);

        const WindowSum three = window_sum(i, P::kShortWindowMonths);
        r.net_flow_3m = three.sum ? DerivedValue::computed(*three.sum, three.inputs, QualityState::Confirmed)
                                  : DerivedValue::unavailable(three.reason);
        r.denominator_3m = denominator_for(month.addMonths(-(P::kShortWindowMonths - 1)));
        r.flow_pct_3m = normalized(three, r.denominator_3m);

        const WindowSum twelve = window_sum(i, P::kLongWindowMonths);
        r.net_flow_12m = twelve.sum ? DerivedValue::computed(*twelve.sum, twelve.inputs, QualityState::Confirmed)
                                    : DerivedValue::unavailable(twelve.reason);
        r.denominator_12m = denominator_for(month.addMonths(-(P::kLongWindowMonths - 1)));
        r.flow_pct_12m = normalized(twelve, r.denominator_12m);

        // Acceleration: this quarter's normalized flow minus the previous,
        // non-overlapping one.
        const int prev = i - P::kAccelerationWindowMonths;
        if (!r.flow_pct_3m.usable()) {
            r.normalized_flow_acceleration_3m = DerivedValue::unavailable(r.flow_pct_3m.reason);
        } else if (prev < 0) {
            r.normalized_flow_acceleration_3m = DerivedValue::unavailable(DerivedReason::InsufficientHistory);
        } else if (!out.months[prev].flow_pct_3m.usable()) {
            // The earlier quarter's own reason (its window or its denominator).
            r.normalized_flow_acceleration_3m = DerivedValue::unavailable(out.months[prev].flow_pct_3m.reason);
        } else {
            const WindowSum older = window_sum(prev, P::kShortWindowMonths);
            InputAccumulator acc = three.inputs;
            acc.merge(older.inputs);
            acc.add(r.denominator_3m.input, r.denominator_3m.report_date);
            acc.add(out.months[prev].denominator_3m.input, out.months[prev].denominator_3m.report_date);
            r.normalized_flow_acceleration_3m = DerivedValue::computed(
                *r.flow_pct_3m.value - *out.months[prev].flow_pct_3m.value, acc, QualityState::Confirmed);
        }

        // Sign balance over 12 consecutive months.
        if (!twelve.sum) {
            r.net_flow_sign_balance_12m = DerivedValue::unavailable(twelve.reason);
        } else {
            int pos = 0;
            int neg = 0;
            for (int k = i - P::kSignBalanceMonths + 1; k <= i; ++k) {
                const double v = *out.months[k].net_flow.value;
                pos += v > 0.0 ? 1 : 0;
                neg += v < 0.0 ? 1 : 0;
            }
            r.net_flow_sign_balance_12m = DerivedValue::computed(static_cast<double>(pos - neg) / P::kSignBalanceMonths,
                                                                 twelve.inputs, QualityState::Confirmed);
        }
    }

    // Percentile of this month's normalized flow among the usable values of
    // the 36 months before it (only earlier months; the current one is the
    // value ranked, never part of its own baseline).
    for (int i = 0; i < n_months; ++i) {
        RegulatoryMonthResult& r = out.months[i];
        if (!r.flow_pct_prior_net_assets.usable()) {
            r.flow_pct_percentile_36m = DerivedValue::unavailable(r.flow_pct_prior_net_assets.reason);
            continue;
        }
        const double x = *r.flow_pct_prior_net_assets.value;
        int n = 0;
        int less = 0;
        int equal = 0;
        InputAccumulator acc;
        acc.merge(net_inputs[i]);
        acc.add(r.denominator_1m.input, r.denominator_1m.report_date);
        for (int k = std::max(0, i - P::kPercentileWindowMonths); k < i; ++k) {
            const DerivedValue& prior = out.months[k].flow_pct_prior_net_assets;
            if (!prior.usable())
                continue;
            ++n;
            less += *prior.value < x ? 1 : 0;
            equal += *prior.value == x ? 1 : 0;
            acc.merge(net_inputs[k]);
            acc.add(out.months[k].denominator_1m.input, out.months[k].denominator_1m.report_date);
        }
        if (n < P::kPercentileMinPrior) {
            r.flow_pct_percentile_36m = DerivedValue::unavailable(DerivedReason::InsufficientHistory);
            continue;
        }
        r.flow_pct_percentile_36m =
            DerivedValue::computed((less + 0.5 * equal) / static_cast<double>(n), acc, QualityState::Confirmed);
    }
    return out;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

inline QJsonObject regulatory_selected_input_json(const SelectedInput& s) {
    QJsonObject o;
    o.insert(QStringLiteral("present"), s.present);
    if (!s.present)
        return o;
    o.insert(QStringLiteral("observation_id"), s.observation_id);
    o.insert(QStringLiteral("value"), s.value.reported() ? QJsonValue(s.value.value) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("value_state"), QLatin1String(value_state_id(s.value.state)));
    o.insert(QStringLiteral("quality"), QLatin1String(quality_state_id(s.quality)));
    o.insert(QStringLiteral("available_from"), derived_time_text(s.available_from));
    o.insert(QStringLiteral("point_in_time_status"), QLatin1String(point_in_time_status_id(s.point_in_time_status)));
    o.insert(QStringLiteral("revision_state"), s.revision_state);
    o.insert(QStringLiteral("accession"), s.source_document);
    o.insert(QStringLiteral("basis"), s.basis);
    o.insert(QStringLiteral("vintages_known"), s.vintages_known);
    return o;
}

inline QJsonObject net_assets_denominator_json(const NetAssetsDenominator& d) {
    QJsonObject o;
    o.insert(QStringLiteral("found"), d.found);
    if (!d.found)
        return o;
    o.insert(QStringLiteral("report_date"), d.report_date.toString(Qt::ISODate));
    o.insert(QStringLiteral("lag_months"), d.lag_months);
    o.insert(QStringLiteral("input"), regulatory_selected_input_json(d.input));
    return o;
}

/// JSON of the analytics. Months outside [from, to] (either may be invalid:
/// unbounded) are left out; the calculation itself always used the full stored
/// history, so the windows of the first listed month are complete.
inline QJsonObject regulatory_flow_analytics_json(const RegulatoryFlowAnalytics& a, const QDate& from = QDate(),
                                                  const QDate& to = QDate()) {
    const QString kind = QLatin1String(measurement_kind_id(MeasurementKind::RegulatoryReportedFlow));
    QJsonArray months;
    for (const RegulatoryMonthResult& r : a.months) {
        if ((from.isValid() && r.inputs.month_end() < from) || (to.isValid() && r.inputs.month > to))
            continue;
        QJsonObject inputs;
        inputs.insert(QStringLiteral("available"), r.inputs.available);
        if (r.inputs.available) {
            inputs.insert(QStringLiteral("selected_accession"), r.inputs.selected_accession);
            inputs.insert(QStringLiteral("accepted_at"), derived_time_text(r.inputs.accepted_at));
            inputs.insert(QLatin1String(kNportSales), regulatory_selected_input_json(r.inputs.sales));
            inputs.insert(QLatin1String(kNportRedemption), regulatory_selected_input_json(r.inputs.redemption));
            inputs.insert(QLatin1String(kNportReinvestment), regulatory_selected_input_json(r.inputs.reinvestment));
            QJsonArray vintages;
            for (const RegulatoryMonthVintage& v : r.inputs.vintages) {
                vintages.append(QJsonObject{
                    {QStringLiteral("accession"), v.accession},
                    {QStringLiteral("form"), v.filing_recorded ? QJsonValue(v.form) : QJsonValue(QJsonValue::Null)},
                    {QStringLiteral("amends_accession"), v.amends_accession},
                    {QStringLiteral("report_period"), v.report_period.toString(Qt::ISODate)},
                    {QStringLiteral("accepted_at"), derived_time_text(v.accepted_at)},
                    {QStringLiteral("available_from"), derived_time_text(v.available_from)},
                    {QStringLiteral("revision_state"), v.revision_state},
                    {QStringLiteral("net_flow"), v.net_flow ? QJsonValue(*v.net_flow) : QJsonValue(QJsonValue::Null)}});
            }
            inputs.insert(QStringLiteral("vintages"), vintages);
            const AmendmentComparison& c = r.inputs.amendment;
            inputs.insert(
                QStringLiteral("amendment"),
                QJsonObject{{QStringLiteral("selected_form"), c.selected_form},
                            {QStringLiteral("amends_accession"), c.amends_accession},
                            {QStringLiteral("comparison"), QLatin1String(amendment_comparison_state_id(c.state))},
                            {QStringLiteral("net_flow_change"),
                             c.net_flow_change ? QJsonValue(*c.net_flow_change) : QJsonValue(QJsonValue::Null)}});
        }
        auto with_denominator = [&](const DerivedValue& v, const NetAssetsDenominator& d) {
            QJsonObject o = derived_value_json(v, kind, QStringLiteral("fraction_of_regulatory_net_assets"));
            o.insert(QStringLiteral("denominator"), net_assets_denominator_json(d));
            return o;
        };
        QJsonObject values;
        values.insert(QStringLiteral("net_flow"), derived_value_json(r.net_flow, kind, QStringLiteral("USD")));
        values.insert(QStringLiteral("flow_pct_prior_net_assets"),
                      with_denominator(r.flow_pct_prior_net_assets, r.denominator_1m));
        values.insert(QStringLiteral("net_flow_3m"), derived_value_json(r.net_flow_3m, kind, QStringLiteral("USD")));
        values.insert(QStringLiteral("net_flow_12m"), derived_value_json(r.net_flow_12m, kind, QStringLiteral("USD")));
        values.insert(QStringLiteral("flow_pct_3m"), with_denominator(r.flow_pct_3m, r.denominator_3m));
        values.insert(QStringLiteral("flow_pct_12m"), with_denominator(r.flow_pct_12m, r.denominator_12m));
        values.insert(QStringLiteral("normalized_flow_acceleration_3m"),
                      derived_value_json(r.normalized_flow_acceleration_3m, kind,
                                         QStringLiteral("difference_of_fractions_of_regulatory_net_assets")));
        values.insert(QStringLiteral("flow_pct_percentile_36m"),
                      derived_value_json(r.flow_pct_percentile_36m, kind, QStringLiteral("percentile_rank_0_1")));
        values.insert(QStringLiteral("net_flow_sign_balance_12m"),
                      derived_value_json(r.net_flow_sign_balance_12m, kind, QStringLiteral("sign_balance_minus1_1")));
        months.append(QJsonObject{{QStringLiteral("month"), r.inputs.month.toString(QStringLiteral("yyyy-MM"))},
                                  {QStringLiteral("period_start"), r.inputs.month.toString(Qt::ISODate)},
                                  {QStringLiteral("period_end"), r.inputs.month_end().toString(Qt::ISODate)},
                                  {QStringLiteral("inputs"), inputs},
                                  {QStringLiteral("values"), values}});
    }
    QJsonArray net_assets;
    for (const RegulatoryNetAssetsInput& n : a.net_assets) {
        net_assets.append(QJsonObject{{QStringLiteral("report_date"), n.report_date.toString(Qt::ISODate)},
                                      {QStringLiteral("input"), regulatory_selected_input_json(n.input)}});
    }
    QJsonObject o;
    o.insert(QStringLiteral("method"), QLatin1String(kRegulatoryFlowAnalyticsVersion));
    o.insert(QStringLiteral("months"), months);
    o.insert(QStringLiteral("net_assets"), net_assets);
    if (!a.months.isEmpty()) {
        const QDate latest = a.months.last().inputs.month_end();
        o.insert(QStringLiteral("latest_month"), a.months.last().inputs.month.toString(QStringLiteral("yyyy-MM")));
        o.insert(QStringLiteral("latest_month_age_days"),
                 static_cast<qint64>(latest.daysTo(a.frame.as_of.toUTC().date())));
    } else {
        o.insert(QStringLiteral("latest_month"), QJsonValue(QJsonValue::Null));
    }
    return o;
}

} // namespace fincept::services::etf
