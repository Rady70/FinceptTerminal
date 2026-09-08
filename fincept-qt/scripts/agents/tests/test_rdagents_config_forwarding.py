"""
test_rdagents_config_forwarding.py — the rdagents/LiteLLM configuration forwarder
(FINCEPT_FORK_PLAN.md §5.3 / review finding: env-name and import-order mismatch).

rdagent 0.8.0's LiteLLM backend builds a LITELLM_SETTINGS pydantic object at
MODULE IMPORT TIME with env prefix `LITELLM_`, and litellm itself resolves the
endpoint from OPENAI_BASE_URL / OPENAI_API_BASE. A previous revision of
config.py wrote CHAT_MODEL / CHAT_OPENAI_BASE_URL / EMBEDDING_MODEL — names
neither layer reads — and the cli handlers imported the rdagent loops before
applying the config. The result: the configured model and endpoint were
silently ignored and completions fell back to the default chat_model against
api.openai.com.

Each test runs in a SUBPROCESS with a scrubbed environment, because the
settings singleton can only be built once per process and reads env at import.
The tests prove, against the exact installed rdagent version:

  * apply_llm_config() sets the names rdagent's settings object actually reads
    (chat model, embedding model, chat api key, chat base URL);
  * the settings singleton picks them up when built AFTER the config (the
    order the cli handlers now use);
  * litellm's OpenAI-protocol get_api_base() resolves the configured endpoint
    from the environment, so the custom base URL reaches the layer that dials;
  * a Fincept-named base_url is refused before any env export.

Run from the application-managed runtime:
    <venv>/Scripts/python.exe -m pytest scripts/agents/tests/test_rdagents_config_forwarding.py -q
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys

import pytest

SCRIPTS_AGENTS = str(pathlib.Path(__file__).resolve().parent.parent)
RDAGENTS_DIR = str(pathlib.Path(__file__).resolve().parent.parent / "rdagents")

PROBE = r"""
import os
import sys
sys.path.insert(0, sys.argv[1])

from config import apply_llm_config  # noqa: E402

apply_llm_config({
    "llm_provider": "deepseek",
    "llm_api_key": "test-key",
    "llm_model": "deepseek-chat",
    "llm_base_url": "http://127.0.0.1:9/v1",
    "llm_embedding_model": "text-embedding-3-small",
})

from rdagent.oai.backend.litellm import LITELLM_SETTINGS  # noqa: E402
from litellm.llms.openai.chat.gpt_transformation import OpenAIGPTConfig  # noqa: E402

print("chat_model=" + LITELLM_SETTINGS.chat_model)
print("embedding_model=" + LITELLM_SETTINGS.embedding_model)
print("chat_openai_base_url=" + str(LITELLM_SETTINGS.chat_openai_base_url))
print("chat_openai_api_key=" + str(LITELLM_SETTINGS.chat_openai_api_key))
print("litellm_api_base=" + str(OpenAIGPTConfig.get_api_base(None)))
"""

FINCEPT_PROBE = r"""
import sys
sys.path.insert(0, sys.argv[1])

from config import apply_llm_config  # noqa: E402

apply_llm_config({
    "llm_provider": "openai",
    "llm_api_key": "k",
    "llm_model": "gpt-4o",
    "llm_base_url": "https://api.fincept.in/v1",
})
print("accepted")
"""


def _run_probe(probe: str) -> str:
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("LITELLM_")
           and k not in ("OPENAI_API_KEY", "OPENAI_API_BASE", "OPENAI_BASE_URL",
                         "ANTHROPIC_API_KEY", "CHAT_MODEL", "EMBEDDING_MODEL")}
    proc = subprocess.run(
        [sys.executable, "-c", probe, RDAGENTS_DIR],
        capture_output=True, text=True, env=env, timeout=600, cwd=RDAGENTS_DIR)
    return proc


def _require_rdagent() -> None:
    try:
        import rdagent  # noqa: F401
    except ImportError:
        pytest.skip("rdagent not installed in this runtime")


class TestRdagentsConfigForwarding:
    def test_settings_pick_up_the_forwarded_config(self):
        _require_rdagent()
        proc = _run_probe(PROBE)
        assert proc.returncode == 0, proc.stderr[-2000:]
        out = dict(line.split("=", 1) for line in proc.stdout.strip().splitlines())
        # The model, embedding, key and endpoint the user configured must reach
        # the backend settings object — not the gpt-4-turbo default.
        assert out["chat_model"] == "deepseek-chat"
        assert out["embedding_model"] == "text-embedding-3-small"
        assert out["chat_openai_base_url"] == "http://127.0.0.1:9/v1"
        assert out["chat_openai_api_key"] == "test-key"
        # litellm itself (the layer that dials) resolves the configured endpoint.
        assert out["litellm_api_base"] == "http://127.0.0.1:9/v1"

    def test_fincept_base_url_is_refused_before_any_export(self):
        proc = _run_probe(FINCEPT_PROBE)
        assert proc.returncode != 0
        assert "HOSTED_SERVICE_UNAVAILABLE" in proc.stderr
