"""
marketlab_net_guard.py — Python-side hosted-destination guard for
user-configured LLM endpoints (FINCEPT_FORK_PLAN.md §5.3).

Why this module exists
----------------------
The C++ GuardedNetworkAccessManager vets every request and every redirect hop
for the application's own Qt networking. It cannot see anything a Python child
process dials, and the Python children do their own networking: agno and
langchain build HTTP clients from the openai / anthropic SDKs, and those SDKs
follow HTTP redirects by default (their httpx clients are built with
follow_redirects=True). A user-configured `base_url` that answers
"302 Location: https://api.fincept.in/..." is therefore followed *inside the
Python process* unless the client used for the model calls is guarded.

Two layers, mirroring the C++ design:

1. reject_if_fincept() — a pure predicate over the configured URL, called at
   every model-construction site before a client exists. This refuses a
   base_url that NAMES a Fincept-owned destination outright (the case the C++
   AgentService pre-filter already covers on its side, re-checked here so the
   Python child does not depend on the C++ caller having remembered).
2. guarded_http_client() / guarded_async_http_client() — httpx clients whose
   transport denies Fincept-owned request URLs on *every* dial, including each
   302/307 redirect hop. Redirects to allowed hosts still work; only
   Fincept-owned targets are refused, before a connection is made. These are
   handed to the model SDKs (openai OpenAI/AsyncOpenAI via agno's
   client=/async_client=, langchain's http_client=/http_async_client=), which
   is the supported injection point for both.

Accepted limits, stated rather than left silent (the same limits the C++
HostedPathGuard documents for itself):

 * This is a NAME deny-list. A user who deliberately types the raw IP address
   of a Fincept host is not caught by it, here or in C++.
 * Provider SDKs whose HTTP stacks have no supported client-injection seam
   (agno's native provider wrappers other than OpenAIChat, langchain's native
   ChatAnthropic / ChatGroq / ChatCohere / ChatGoogleGenerativeAI paths,
   litellm's internal client construction) are protected at the configuration
   layer by reject_if_fincept() rather than inside their own transport. A
   base_url that names Fincept is refused there; redirects from a *non*-Fincept
   configured endpoint are governed by that SDK, exactly as the C++ guard
   accepts redirects to hosts it does not know.
"""

from __future__ import annotations

import urllib.parse

import httpx

_FINCEPT_DOMAINS = ("fincept.in", "fincept.com", "fincept.app", "fincept.ai")

# Same shape as the openai SDK's default (openai/_constants.py,
# DEFAULT_TIMEOUT): the guarded client replaces the SDK's default client, so it
# must not silently change the caller's time budget. Long generations are
# normal for an LLM call; httpx's own 5-second default would cut them off.
_GUARDED_TIMEOUT = httpx.Timeout(timeout=600, connect=5.0)

_HOSTED_PREFIX = "HOSTED_SERVICE_UNAVAILABLE"


class HostedServiceUnavailable(RuntimeError):
    """Typed error for a rejected Fincept-owned destination.

    The prefix mirrors the C++ kHostedUnavailablePrefix so the two sides of the
    boundary report the same failure class; callers match the prefix, not the
    human wording.
    """

    def __init__(self, target: str) -> None:
        self.target = target
        super().__init__(f"{_HOSTED_PREFIX}: {target}")


def normalized_host(host: str) -> str:
    return host.strip().lower().rstrip(".")


def is_fincept_host(host: str) -> bool:
    """True when `host` is a Fincept-owned hostname, independent of scheme.

    Mirrors HostedPathGuard::is_fincept_host: fincept.in/.com/.app/.ai and
    every subdomain. The github.com rules are deliberately absent here for the
    same reason as in C++ — they are path-scoped and a bare host carries no
    path to judge; callers that hold a URL must use is_fincept_url().
    """
    h = normalized_host(host)
    if not h:
        return False
    return any(h == d or h.endswith("." + d) for d in _FINCEPT_DOMAINS)


def is_fincept_url(url: str) -> bool:
    """True when `url` names a Fincept-owned destination.

    Mirrors HostedPathGuard::is_fincept_destination: the host half is judged
    whatever the scheme is; the Fincept-Corporation GitHub rules are
    path-scoped and therefore require an http/https/ws/wss scheme; a value with
    no host (a bare "api.fincept.in:443" reads as a scheme to urlsplit, exactly
    as it does to QUrl) is false.
    """
    parts = urllib.parse.urlsplit(url.strip())
    host = parts.hostname
    if host is None:
        return False
    if is_fincept_host(host):
        return True
    scheme = (parts.scheme or "").lower()
    if scheme not in ("http", "https", "ws", "wss"):
        return False
    if host in ("raw.githubusercontent.com", "github.com"):
        path = parts.path.lower()
        return path.startswith("/fincept-corporation/") or path == "/fincept-corporation"
    return False


def reject_if_fincept(url: str) -> None:
    """Raise HostedServiceUnavailable when `url` names a Fincept-owned destination.

    Call this on every user-configurable endpoint before a client exists.
    """
    if is_fincept_url(url):
        raise HostedServiceUnavailable(url)


class _FinceptDenyTransport(httpx.HTTPTransport):
    """Transport that refuses denied request URLs before dialing them.

    httpx sends every request — the original one and each 3xx redirect hop —
    through handle_request(), so overriding it here is the "inspect every
    redirect target before following it" half of the §5.3 requirement while
    leaving redirects to allowed hosts untouched. `deny` is the predicate, a
    constructor seam so the hermetic transport tests can exercise the refusal
    path against the reserved .invalid name instead of a real Fincept host.
    """

    def __init__(self, deny) -> None:
        super().__init__()
        self._deny = deny

    def handle_request(self, request: httpx.Request) -> httpx.Response:
        if self._deny(str(request.url)):
            raise HostedServiceUnavailable(str(request.url))
        return super().handle_request(request)


class _AsyncFinceptDenyTransport(httpx.AsyncHTTPTransport):
    def __init__(self, deny) -> None:
        super().__init__()
        self._deny = deny

    async def handle_async_request(self, request: httpx.Request) -> httpx.Response:
        if self._deny(str(request.url)):
            raise HostedServiceUnavailable(str(request.url))
        return await super().handle_async_request(request)


def guarded_http_client(*, timeout: httpx.Timeout | None = None,
                        deny=None) -> httpx.Client:
    """A sync httpx.Client that refuses denied destinations on every dial.

    `deny` defaults to is_fincept_url; it is a parameter only so the hermetic
    transport tests can exercise the refusal path against a reserved .invalid
    name instead of a real Fincept host (the same seam the C++ manager exposes
    as setDeniedDestination).

    Redirects stay enabled deliberately: the deny transport inspects every
    request — each 3xx hop included — before it is dialed, so a redirect to an
    allowed host works while a redirect to a denied target is refused.
    """
    return httpx.Client(
        transport=_FinceptDenyTransport(deny or is_fincept_url),
        timeout=timeout if timeout is not None else _GUARDED_TIMEOUT,
        follow_redirects=True,
    )


def guarded_async_http_client(*, timeout: httpx.Timeout | None = None,
                              deny=None) -> httpx.AsyncClient:
    """An async httpx.AsyncClient that refuses denied destinations on every dial."""
    return httpx.AsyncClient(
        transport=_AsyncFinceptDenyTransport(deny or is_fincept_url),
        timeout=timeout if timeout is not None else _GUARDED_TIMEOUT,
        follow_redirects=True,
    )


def guarded_openai_clients(
    api_key: str | None, base_url: str | None, *, timeout: httpx.Timeout | None = None,
    deny=None,
):
    """Guarded sync + async OpenAI SDK clients for one model-construction site.

    The openai SDK delegates redirect following entirely to the httpx client it
    is given, so handing it these two clients is what keeps both the sync and
    the async call paths from following a redirect into a Fincept-owned
    destination. Returned as (sync, async) for agno's client=/async_client=
    parameters and for any other construction site that accepts prebuilt
    clients.

    `deny` defaults to is_fincept_url and is a parameter only so the hermetic
    integration tests can exercise the refusal path against a reserved
    .invalid name instead of a real Fincept host (the same seam the C++ manager
    exposes as setDeniedDestination).

    A missing credential aborts construction here (the SDK raises the same
    failure a first request would have, but with no endpoint named) — the
    error is re-raised with the base_url attached so the caller can report
    WHICH endpoint could not be built.
    """
    from openai import AsyncOpenAI, OpenAI

    common: dict = {"base_url": base_url or None}
    sync_kwargs = {"http_client": guarded_http_client(timeout=timeout, deny=deny), **common}
    async_kwargs = {"http_client": guarded_async_http_client(timeout=timeout, deny=deny), **common}
    if api_key:
        sync_kwargs["api_key"] = api_key
        async_kwargs["api_key"] = api_key
    try:
        return OpenAI(**sync_kwargs), AsyncOpenAI(**async_kwargs)
    except Exception as exc:
        raise RuntimeError(
            f"Unable to build guarded OpenAI clients for base_url {base_url!r}: {exc}"
        ) from exc
