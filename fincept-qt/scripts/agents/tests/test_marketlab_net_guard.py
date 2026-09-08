"""
test_marketlab_net_guard.py — the Python-side hosted-destination guard.

Unit under test: scripts/agents/marketlab_net_guard.py, the mirror of the C++
network::HostedPathGuard / GuardedNetworkAccessManager for the Python child
processes (FINCEPT_FORK_PLAN.md §5.3).

Two layers, matching the C++ test split (tst_marketlab_boundary +
tst_redirect_guard):

  * the pure predicates — classification only, no sockets;
  * the guarded transports — a real 302 over a loopback socket, with the
    forbidden target expressed as the reserved RFC 2606 name "denied.invalid"
    so that a broken guard fails with a DNS error against a name that cannot
    contact anything, never with traffic to a real Fincept host. The real
    Fincept URL appears only as pure predicate input below, never as a
    dialable target.

Run from the application-managed runtime:
    <venv>/Scripts/python.exe -m pytest scripts/agents/tests/test_marketlab_net_guard.py -q
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

from marketlab_net_guard import (  # noqa: E402
    HostedServiceUnavailable,
    guarded_async_http_client,
    guarded_http_client,
    is_fincept_host,
    is_fincept_url,
    reject_if_fincept,
)

DENIED_TARGET = "https://denied.invalid/telemetry"


def _deny_invalid(url: str) -> bool:
    """The hermetic stand-in for the real deny predicate: the Fincept rules
    plus the reserved .invalid name used as the dialable-but-unreachable
    forbidden target of the transport cases."""
    return is_fincept_url(url) or "denied.invalid" in url


# ── Pure predicates ──────────────────────────────────────────────────────────

class TestPredicates:
    def test_fincept_hosts_are_denied(self):
        assert is_fincept_host("fincept.in")
        assert is_fincept_host("api.fincept.in")
        assert is_fincept_host("api.fincept.com")
        assert is_fincept_host("x.fincept.app")
        assert is_fincept_host("www.fincept.ai")

    def test_normalisation_matches_the_cpp_guard(self):
        # Trailing root-label dot: the same destination, and QUrl preserves it.
        assert is_fincept_host("api.fincept.in.")
        assert is_fincept_host("  API.FINCEPT.IN. ")
        assert is_fincept_url("https://api.fincept.in./v1/x")

    def test_non_fincept_hosts_pass(self):
        assert not is_fincept_host("api.anthropic.com")
        assert not is_fincept_host("notfincept.in")
        assert not is_fincept_host("finceptine.com")
        assert not is_fincept_host("")
        assert not is_fincept_host("127.0.0.1")

    def test_url_judges_host_whatever_the_scheme(self):
        # A Fincept-owned host is Fincept-owned over any protocol, exactly like
        # the C++ guard (connector config can name ftp:// or redis:// URLs).
        assert is_fincept_url("ftp://api.fincept.in/x")
        assert is_fincept_url("//api.fincept.in/svc")

    def test_github_org_rules_are_path_scoped(self):
        assert is_fincept_url("https://github.com/Fincept-Corporation/x")
        assert is_fincept_url("https://github.com/Fincept-Corporation")
        assert is_fincept_url("https://raw.githubusercontent.com/Fincept-Corporation/foo")
        assert not is_fincept_url("https://github.com/other/org")
        # No network scheme, no path-scoped rule (mirrors the C++ guard).
        assert not is_fincept_url("file:///github.com/Fincept-Corporation/x")

    def test_userinfo_is_not_the_host(self):
        # The host half is what is judged; userinfo text is not.
        assert is_fincept_url("http://x@api.fincept.in/")
        assert not is_fincept_url("http://api.fincept.in@api.anthropic.com/")

    def test_hostless_values_are_false(self):
        # A bare "host:port" reads as a scheme to urlsplit, exactly as it does
        # to QUrl — there is no host to judge.
        assert not is_fincept_url("api.fincept.in:443")
        assert not is_fincept_url("file:///etc/passwd")

    def test_reject_raises_the_typed_error(self):
        with pytest.raises(HostedServiceUnavailable) as err:
            reject_if_fincept("https://api.fincept.in/v1")
        assert str(err.value).startswith("HOSTED_SERVICE_UNAVAILABLE")
        reject_if_fincept("https://api.anthropic.com")  # no raise


# ── Guarded transports ───────────────────────────────────────────────────────

class _RedirectServer:
    """Loopback HTTP server that answers one canned response and counts hits."""

    def __init__(self, location: str):
        self._location = location
        self._server = http.server.HTTPServer(("127.0.0.1", 0), self._handler)
        self.hits = 0
        self.port = self._server.server_address[1]
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    class _handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.server.owner.hits += 1
            body = b"SECOND-SERVER-BODY"
            self.send_response(302 if self.server.owner._location else 200)
            if self.server.owner._location:
                self.send_header("Location", self.server.owner._location)
            else:
                self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    def start(self):
        self._server.owner = self
        self._thread.start()
        return self

    def stop(self):
        self._server.shutdown()
        self._server.server_close()


class TestGuardedTransports:
    def test_redirect_to_denied_destination_is_refused_before_dial(self):
        redirector = _RedirectServer(DENIED_TARGET).start()
        try:
            client = guarded_http_client(timeout=httpx.Timeout(5.0, connect=5.0),
                                         deny=_deny_invalid)
            with pytest.raises(HostedServiceUnavailable):
                client.get(f"http://127.0.0.1:{redirector.port}/")
        finally:
            redirector.stop()

    def test_initial_denied_url_is_refused_before_dial(self):
        client = guarded_http_client(timeout=httpx.Timeout(5.0, connect=5.0),
                                     deny=_deny_invalid)
        with pytest.raises(HostedServiceUnavailable):
            client.get(DENIED_TARGET)

    def test_redirect_to_an_allowed_host_still_works(self):
        target = _RedirectServer("").start()
        redirector = _RedirectServer(f"http://127.0.0.1:{target.port}/").start()
        try:
            client = guarded_http_client(timeout=httpx.Timeout(5.0, connect=5.0))
            response = client.get(f"http://127.0.0.1:{redirector.port}/")
            assert response.status_code == 200
            assert response.content == b"SECOND-SERVER-BODY"
            # The counter is live — the refusal cases above mean something only
            # because a permitted redirect really is followed.
            assert target.hits == 1
        finally:
            redirector.stop()
            target.stop()

    @pytest.mark.anyio
    async def test_async_redirect_to_denied_destination_is_refused(self):
        redirector = _RedirectServer(DENIED_TARGET).start()
        client = guarded_async_http_client(timeout=httpx.Timeout(5.0, connect=5.0),
                                           deny=_deny_invalid)
        try:
            with pytest.raises(HostedServiceUnavailable):
                await client.get(f"http://127.0.0.1:{redirector.port}/")
        finally:
            redirector.stop()
            await client.aclose()


# ── Configuration-layer consumers of the guard ──────────────────────────────

class TestConfigLayerConsumers:
    def test_knowledge_url_source_is_guarded_before_the_reader_fetches(self, monkeypatch):
        from finagent_core.modules.knowledge_module import KnowledgeModule

        module = KnowledgeModule()
        fetched = []

        class FakeReader:
            def read(self, src, **kwargs):
                fetched.append(src)
                return []

        monkeypatch.setattr(module, "_get_reader", lambda _doc_type: FakeReader())

        # A Fincept-owned source is refused, the reader never sees it, and the
        # typed error reaches the caller instead of being logged away.
        with pytest.raises(HostedServiceUnavailable):
            module.add_documents("https://api.fincept.in/telemetry", doc_type="url")
        assert fetched == []

        # An allowed source still reaches the reader (and only network-capable
        # doc types are vetted — a local file path is not a route).
        module.add_documents("https://example.com/docs", doc_type="url")
        assert fetched == ["https://example.com/docs"]
        module.add_documents("/tmp/report.pdf", doc_type="pdf")
        assert fetched == ["https://example.com/docs", "/tmp/report.pdf"]

        # A bare github slug names a github.com destination the path-scoped
        # Fincept-Corporation rule must judge; a Fincept org slug is refused,
        # an unrelated org slug passes.
        with pytest.raises(HostedServiceUnavailable):
            module.add_documents("Fincept-Corporation/marketlab-terminal", doc_type="github")
        module.add_documents("torvalds/linux", doc_type="github")
        assert fetched[-1] == "torvalds/linux"
