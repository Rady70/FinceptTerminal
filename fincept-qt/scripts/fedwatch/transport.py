"""HTTP transport for the MarketLab FedWatch backend.

Every provider function takes an explicit transport. The production transport
is a thin ``requests`` wrapper; the deterministic fixture tests inject a fake
transport, so the tests exercise the real parsing and validation paths without
network access and without requiring ``requests`` to be installed.

``requests`` is imported lazily inside :class:`HttpTransport` on purpose: the
migration must run inside MarketLab's existing app-managed Python environment
(which already declares ``requests``), and the fixture suite must run on a
plain Python interpreter with no third-party packages at all.
"""

from __future__ import annotations


class TransportError(Exception):
    """A transport-level failure (connection, timeout, HTTP status, bad JSON)."""

    def __init__(self, message: str, status_code: int | None = None, url: str | None = None):
        super().__init__(message)
        self.status_code = status_code
        self.url = url

    def detail(self) -> dict:
        out = {}
        if self.status_code is not None:
            out["status_code"] = self.status_code
        if self.url:
            out["url"] = self.url
        return out


class Transport:
    """The two operations the FedWatch providers need."""

    def get_text(self, url: str, headers: dict | None = None, timeout: int = 20) -> str:
        raise NotImplementedError

    def get_json(self, url: str, params: dict | None = None, timeout: int = 20):
        raise NotImplementedError


class HttpTransport(Transport):
    """Production transport. No retries, no caching — the same single-GET
    behavior the previously qualified implementation used."""

    def _request(self, url: str, headers=None, params=None, timeout=20):
        try:
            import requests
        except ImportError as exc:  # pragma: no cover - environment failure
            raise TransportError(
                "the python 'requests' package is not available in this runtime",
                url=url,
            ) from exc

        try:
            response = requests.get(url, headers=headers, params=params, timeout=timeout)
        except requests.exceptions.RequestException as exc:
            raise TransportError(f"request failed: {exc}", url=url) from exc

        if response.status_code >= 400:
            raise TransportError(
                f"HTTP {response.status_code}", status_code=response.status_code, url=url
            )
        return response

    def get_text(self, url: str, headers: dict | None = None, timeout: int = 20) -> str:
        return self._request(url, headers=headers, timeout=timeout).text

    def get_json(self, url: str, params: dict | None = None, timeout: int = 20):
        response = self._request(url, params=params, timeout=timeout)
        try:
            return response.json()
        except ValueError as exc:
            raise TransportError("response body is not valid JSON", url=url) from exc
