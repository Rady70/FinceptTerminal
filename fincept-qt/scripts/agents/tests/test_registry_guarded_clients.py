"""
test_registry_guarded_clients.py — the guarded-client attachment in
finagent_core's model registry (FINCEPT_FORK_PLAN.md §5.3).

The registry resolves provider classes dynamically and used to decide
"OpenAI-protocol" by exact class-name equality. That skipped every agno
OpenAILike subclass — most importantly DeepSeek, which accepts a
user-configurable base_url and whose inherited OpenAI SDK client follows
redirects by default. These tests pin the capability-based behaviour:

  * a native subclass (DeepSeek) built with a custom base_url receives the
    guarded sync AND async clients (deny transport, redirects vetted hop by
    hop);
  * the guarded client attached to the model refuses a denied destination
    before any dial, and refuses a 302 that points at one;
  * a non-OpenAI-protocol provider with a custom base_url is routed to the
    guarded OpenAIChat path instead of receiving the URL on an unguardable
    native transport;
  * a Fincept-named base_url is refused outright, whatever the provider.

Run from the application-managed runtime:
    <venv>/Scripts/python.exe -m pytest scripts/agents/tests/test_registry_guarded_clients.py -q
"""

from __future__ import annotations

import http.server
import pathlib
import sys
import threading

import httpx
import pytest

SCRIPTS_AGENTS = str(pathlib.Path(__file__).resolve().parent.parent)
if SCRIPTS_AGENTS not in sys.path:
    sys.path.insert(0, SCRIPTS_AGENTS)

from marketlab_net_guard import HostedServiceUnavailable, is_fincept_url  # noqa: E402

DENIED_TARGET = "https://denied.invalid/telemetry"


def _deny_invalid(url: str) -> bool:
    return is_fincept_url(url) or "denied.invalid" in url


class _RedirectServer:
    """Loopback HTTP server answering one canned 302, counting hits."""

    def __init__(self, location: str):
        self._location = location
        self._server = http.server.HTTPServer(("127.0.0.1", 0), self._handler)
        self.hits = 0
        self.port = self._server.server_address[1]
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    class _handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            self.server.owner.hits += 1
            self.send_response(302)
            self.send_header("Location", self.server.owner._location)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *args):
            pass

    def start(self):
        self._server.owner = self
        self._thread.start()
        return self

    def stop(self):
        self._server.shutdown()
        self._server.server_close()


def _is_guarded_client(sdk_client) -> bool:
    from marketlab_net_guard import _AsyncFinceptDenyTransport, _FinceptDenyTransport

    inner = getattr(sdk_client, "_client", None) or sdk_client
    transport = getattr(inner, "_transport", None)
    return isinstance(transport, (_FinceptDenyTransport, _AsyncFinceptDenyTransport))


def _cause_chain(exc: BaseException):
    """The chain of wrapped causes the SDKs produce around transport errors."""
    cur = exc
    seen = []
    while cur is not None and len(seen) < 8:
        seen.append(cur)
        cur = cur.__cause__ or cur.__context__
    return seen


def _contains_hosted_unavailable(exc: BaseException) -> bool:
    return any(isinstance(e, HostedServiceUnavailable) for e in _cause_chain(exc))


class TestRegistryGuardedClients:
    def test_deepseek_subclass_receives_guarded_sync_and_async_clients(self):
        from finagent_core.registries.models_registry import ModelsRegistry

        model = ModelsRegistry.create_model(
            "deepseek", model_id="deepseek-chat", api_key="test-key",
            base_url="http://localhost:9999/v1")

        assert model.client is not None
        assert model.async_client is not None
        assert _is_guarded_client(model.client), "sync client must be the guarded transport"
        assert _is_guarded_client(model.async_client), "async client must be the guarded transport"

        # The attached client refuses a Fincept destination before any dial —
        # the production deny-list, called directly on the httpx client the
        # model will use. The raise happens in the transport before a socket
        # exists, so this assertion makes no network attempt.
        with pytest.raises(HostedServiceUnavailable):
            model.client._client.get("https://api.fincept.in/telemetry")

    def test_guarded_client_refuses_a_redirect_to_a_denied_destination(self):
        from marketlab_net_guard import guarded_openai_clients

        redirector = _RedirectServer(DENIED_TARGET).start()
        try:
            client, _ = guarded_openai_clients(
                "test-key", f"http://127.0.0.1:{redirector.port}/v1",
                timeout=httpx.Timeout(5.0, connect=5.0), deny=_deny_invalid)
            # The SDK posts to the allowed local endpoint; the 302 hop to the
            # denied target must be refused by the transport before any dial.
            # The SDK wraps transport errors, so the typed refusal is found in
            # the cause chain — a DNS/connect error would mean the target was
            # actually dialed.
            with pytest.raises(Exception) as err:
                client.chat.completions.create(
                    model="test", messages=[{"role": "user", "content": "hi"}])
            assert _contains_hosted_unavailable(err.value), _cause_chain(err.value)
        finally:
            redirector.stop()

    @pytest.mark.anyio
    async def test_guarded_async_client_refuses_a_redirect_to_a_denied_destination(self):
        from marketlab_net_guard import guarded_openai_clients

        redirector = _RedirectServer(DENIED_TARGET).start()
        try:
            _, async_client = guarded_openai_clients(
                "test-key", f"http://127.0.0.1:{redirector.port}/v1",
                timeout=httpx.Timeout(5.0, connect=5.0), deny=_deny_invalid)
            with pytest.raises(Exception) as err:
                await async_client.chat.completions.create(
                    model="test", messages=[{"role": "user", "content": "hi"}])
            assert _contains_hosted_unavailable(err.value), _cause_chain(err.value)
        finally:
            redirector.stop()
            await async_client.close()

    def test_non_openai_protocol_provider_with_custom_base_url_is_rerouted_guarded(self):
        # anthropic's native class is NOT OpenAI-protocol; a custom base_url
        # must land on the guarded OpenAIChat path, not on the unguardable
        # native constructor.
        from finagent_core.registries.models_registry import ModelsRegistry

        model = ModelsRegistry.create_model(
            "anthropic", model_id="claude-x", api_key="test-key",
            base_url="http://localhost:9999/anthropic")

        assert type(model).__name__ == "OpenAIChat"
        assert model.client is not None and model.async_client is not None
        assert _is_guarded_client(model.client)
        assert _is_guarded_client(model.async_client)

    def test_fincept_named_base_url_is_refused_whatever_the_provider(self):
        from finagent_core.registries.models_registry import ModelsRegistry

        for provider in ("deepseek", "anthropic", "openai"):
            with pytest.raises(Exception) as err:
                ModelsRegistry.create_model(
                    provider, api_key="k", base_url="https://api.fincept.in/v1")
            assert "HOSTED_SERVICE_UNAVAILABLE" in str(err.value)
