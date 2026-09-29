"""Provider-specific error types for the MarketLab FedWatch backend.

Batch A of the FedWatch migration (FEDWATCH_INTEGRATION_PLAN.md section 17)
requires provider failures to stay distinguishable: a failure to obtain the
Fed-side probability source must never be reported as a Polymarket failure and
vice versa. Every failure raised by this package therefore carries the provider
that failed, a stable machine-readable code, and a human-readable message.

The codes are provider-namespaced so a caller (the Qt layer or the snapshot
builder) can branch on them without parsing prose:

    investing       INVESTING_SOURCE_UNAVAILABLE
                    INVESTING_PARSE_EMPTY
                    INVESTING_DISTRIBUTION_INVALID
    fred            FRED_SOURCE_UNAVAILABLE
                    FRED_TARGET_RANGE_INVALID
    fomc_calendar   FOMC_CALENDAR_UNAVAILABLE
    polymarket      POLYMARKET_DISCOVERY_UNAVAILABLE
                    POLYMARKET_DISCOVERY_EMPTY
                    POLYMARKET_MARKET_DATA_UNAVAILABLE
    fedwatch_history
                    FEDWATCH_HISTORY_LOCATION_UNAVAILABLE
                    FEDWATCH_HISTORY_UNAVAILABLE
                    FEDWATCH_HISTORY_WRITE_FAILED
                    FEDWATCH_HISTORY_READ_FAILED
                    FEDWATCH_HISTORY_SCHEMA_NEWER
                    FEDWATCH_HISTORY_SCHEMA_INCOMPATIBLE
    zq              FEDWATCH_ZQ_DATA_UNAVAILABLE
                    FEDWATCH_ZQ_DATA_INVALID
                    FEDWATCH_ZQ_RECONSTRUCTION_INCOMPLETE
"""

from __future__ import annotations

PROVIDER_INVESTING = "investing"
PROVIDER_FRED = "fred"
PROVIDER_FOMC_CALENDAR = "fomc_calendar"
PROVIDER_POLYMARKET = "polymarket"
# Batch B local components. These are not external data providers: they name
# the MarketLab-owned durable history store and the user-supplied historical
# ZQ dataset so their failures stay distinguishable from provider failures.
PROVIDER_HISTORY = "fedwatch_history"
PROVIDER_ZQ = "zq"

PROVIDERS = (
    PROVIDER_INVESTING,
    PROVIDER_FRED,
    PROVIDER_FOMC_CALENDAR,
    PROVIDER_POLYMARKET,
    PROVIDER_HISTORY,
    PROVIDER_ZQ,
)


class FedwatchError(Exception):
    """A provider failure that must remain attributable to one provider."""

    def __init__(self, provider: str, code: str, message: str, detail: dict | None = None):
        if provider not in PROVIDERS:
            raise ValueError(f"unknown FedWatch provider {provider!r}")
        super().__init__(message)
        self.provider = provider
        self.code = code
        self.message = message
        self.detail = dict(detail) if detail else {}

    def to_dict(self) -> dict:
        """CFTC-compatible error envelope shape: {"error": "..."} plus context.

        ``EconomicsEnvelopeParse`` reads the nested ``error`` string as the
        human-readable message and passes any ``error_code`` through as a
        ``[CODE] `` prefix. The CFTC provider attaches its message under
        ``error`` as well, so this shape stays inside the existing contract.
        """
        out = {
            "error": self.message,
            "provider": self.provider,
            "code": self.code,
        }
        if self.detail:
            out["detail"] = self.detail
        return out


class InvestingDistributionError(FedwatchError, ValueError):
    """A malformed Investing distribution.

    Subclasses ``ValueError`` on purpose: the previously qualified reference
    boundary raised ``ValueError`` for exactly these cases and the ported
    regression asserts that contract. A malformed distribution hard-rejects the
    current Fed-side conversion instead of being "repaired".
    """

    def __init__(self, code: str, message: str, detail: dict | None = None):
        FedwatchError.__init__(self, PROVIDER_INVESTING, code, message, detail)
        ValueError.__init__(self, message)


class HistoryStoreError(FedwatchError):
    """A failure of the MarketLab-owned FedWatch history store.

    Batch B history failures are provider-attributed like every other
    FedWatch failure so a persistence problem can never be presented as a
    provider outage (or vice versa).
    """

    def __init__(self, code: str, message: str, detail: dict | None = None):
        FedwatchError.__init__(self, PROVIDER_HISTORY, code, message, detail)


class ZqDataError(FedwatchError):
    """A failure of the optional user-supplied historical ZQ dataset path."""

    def __init__(self, code: str, message: str, detail: dict | None = None):
        FedwatchError.__init__(self, PROVIDER_ZQ, code, message, detail)
