"""FedWatch route semantics, independent of an observation's quality state."""
from fedwatch import history, published_history


def describe(store, meeting_date=None):
    routes = [
        {"source": "cme_published", "method": published_history.METHOD,
         "availability": "AVAILABLE_MANUAL_OR_USER_TRIGGERED", "integration": "LOCAL_CSV_IMPORT",
         "automated_collection": "NOT_QUALIFIED_FOR_AUTOMATED_COLLECTION",
         "financial_object": published_history.OBJECT,
         "action": "Import history", "command": "history_cme_import",
         "limitation": "Public Excel download exists; automated web harvesting is restricted. Local use rights must be established; native workbook format is not qualified."},
        {"source": "investing", "method": history.FED_METHOD_LIVE,
         "availability": "AVAILABLE_AND_INTEGRATED", "integration": "MANUAL_REFRESH",
         "automated_collection": "EXISTING_CURRENT_ROUTE_ONLY",
         "financial_object": "LOCAL_MEETING_CHANGE_BP", "action": "Refresh", "command": "collect",
         "limitation": "Qualified current parser retained. Website storage/reuse terms are restrictive; technical qualification is not data-use permission."},
        {"source": "investing_monthly", "method": history.FED_METHOD_ZQ,
         "availability": "DERIVED_OR_RECONSTRUCTED", "integration": "LOCAL_MONTHLY_CSV_IMPORT",
         "automated_collection": "NOT_QUALIFIED_FOR_AUTOMATED_COLLECTION",
         "financial_object": "LOCAL_MEETING_CHANGE_BP", "action": "Load history", "command": "history_zq_import",
         "limitation": "Individual monthly histories exist publicly. Use permitted exports with explicit monthly identity; never substitute continuous FFc1. Indicative close, missing OI, date-only timing."},
        {"source": "yahoo", "availability": "PARTIAL", "integration": "AVAILABLE_NOT_IMPLEMENTED",
         "automated_collection": "REFERENCE_ONLY",
         "limitation": "One monthly ZQ contract was observed; a complete monthly ladder and historical depth are unqualified."},
        {"source": "polymarket", "method": history.POLY_METHOD,
         "availability": "AVAILABLE_AND_INTEGRATED", "integration": "EXPLICIT_BOUNDED_CLOB_BACKFILL",
         "financial_object": "LOCAL_MEETING_CHANGE_BP", "action": "Load history", "command": "history_backfill",
         "limitation": "Per-token coverage varies. Validated mappings and existing stale/failure gates remain authoritative."},
        {"source": "fred_fomc", "availability": "AVAILABLE_AND_INTEGRATED", "integration": "MANUAL_CURRENT_OR_ZQ_IMPORT",
         "financial_object": "OFFICIAL_RATE_CONTEXT_AND_MEETING_CALENDAR",
         "limitation": "Official target/EFFR observations and calendars are inputs, not probability histories; target revisions are not vintage proof."},
    ]
    coverage = store.method_summary(meeting_date)
    return {"routes": routes, "coverage": coverage,
            "definitions": {"UNAVAILABLE": "The specific requested value/coverage cannot be supplied after the applicable route checks; never shorthand for not integrated.",
                            "PROVIDER_TEMPORARILY_FAILED": "A retrieval failed; public availability and retained observations are unchanged.",
                            "PARTIAL": "Only some requested dates/contracts/outcomes are covered; missing values remain missing."},
            "network_requests": 0, "errors": []}
