"""
config.py — LiteLLM / rdagent LLM config wiring for Fincept rdagents.

rdagent 0.8.0's LiteLLM backend (rdagent.oai.backend.litellm) reads its
settings from a pydantic BaseSettings object with env prefix `LITELLM_`
(LITELLM_CHAT_MODEL, LITELLM_EMBEDDING_MODEL, LITELLM_CHAT_OPENAI_API_KEY,
LITELLM_CHAT_OPENAI_BASE_URL, …), instantiated at MODULE IMPORT TIME —
callers must run apply_llm_config() BEFORE the first rdagent import, which is
exactly what the cli.py handlers do.

The endpoint is then read by litellm itself, which does not consult the
rdagent settings object for it: litellm's OpenAI-protocol transformation
resolves the API base from its own environment (OPENAI_BASE_URL /
OPENAI_API_BASE), so the guarded base URL is exported under BOTH contract
names — the rdagent settings name for parity and the names litellm actually
dials. Without this, a configured custom endpoint was silently ignored and
every request went to api.openai.com with the default chat_model.

Key env vars read:
  LITELLM_CHAT_MODEL            — rdagent settings: the model litellm calls
  LITELLM_EMBEDDING_MODEL       — rdagent settings: the embedding model
  LITELLM_CHAT_OPENAI_API_KEY   — rdagent settings: chat API key
  LITELLM_CHAT_OPENAI_BASE_URL  — rdagent settings: base URL (parity)
  OPENAI_API_KEY                — read by litellm directly for its clients
  OPENAI_API_BASE / OPENAI_BASE_URL — read by litellm's get_api_base()
  ANTHROPIC_API_KEY             — read by litellm for anthropic-prefixed models
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Any

# The MarketLab hosted-destination guard lives one directory up
# (scripts/agents/marketlab_net_guard.py); scripts/agents/ is normally already
# on sys.path via cli.py, but this module must not depend on the caller having
# arranged it.
_GUARD_DIR = str(Path(__file__).resolve().parent.parent)
if _GUARD_DIR not in sys.path:
    sys.path.insert(0, _GUARD_DIR)
from marketlab_net_guard import reject_if_fincept


# Providers that need anthropic-style API key env var
_ANTHROPIC_PROVIDERS = {"anthropic"}

# Providers that use a custom base_url (OpenAI-compatible)
_CUSTOM_BASE_PROVIDERS = {
    "minimax", "deepseek", "openrouter", "together", "fireworks",
    "groq", "mistral", "cohere",
}

# litellm model prefix map — litellm needs provider prefix for non-OpenAI models
_LITELLM_PREFIXES: dict[str, str] = {
    "anthropic":  "anthropic/",
    "groq":       "groq/",
    "deepseek":   "deepseek/",
    "mistral":    "mistral/",
    "cohere":     "cohere/",
    "together":   "together_ai/",
    "fireworks":  "fireworks_ai/",
    "openrouter": "openrouter/",
}

# Default embedding model
_DEFAULT_EMBEDDING = "text-embedding-3-small"


def apply_llm_config(config: dict[str, Any]) -> dict[str, str]:
    """
    Translate Fincept LLM config dict into rdagent env vars.

    Sets os.environ in place so rdagent's LiteLLMAPIBackend picks them up.

    Args:
        config: Dict with keys: llm_provider, llm_api_key, llm_model,
                llm_base_url (optional), llm_embedding_model (optional)

    Returns:
        Dict of env vars that were set (for logging/debugging).
    """
    provider   = config.get("llm_provider", "openai").lower()
    api_key    = config.get("llm_api_key", "")
    model      = config.get("llm_model", "gpt-4o")
    base_url   = config.get("llm_base_url", "")

    # MarketLab: refuse a configured base_url that NAMES a Fincept-owned
    # destination before it is exported to the environment
    # (FINCEPT_FORK_PLAN.md §5.3). litellm builds its own HTTP clients
    # internally, so the configuration layer is the guard this path has;
    # the C++ AIQuantLabService forwarder performs no host check of its own.
    if base_url:
        reject_if_fincept(base_url)

    env_vars: dict[str, str] = {}

    # --- Model name ---
    # litellm needs a provider prefix for non-OpenAI providers
    prefix = _LITELLM_PREFIXES.get(provider, "")
    if prefix and not model.startswith(prefix):
        litellm_model = prefix + model
    else:
        litellm_model = model

    # For custom base_url providers (MiniMax, DeepSeek hosted, etc.)
    # litellm treats them as openai-compatible — no prefix needed
    if provider in _CUSTOM_BASE_PROVIDERS and base_url:
        litellm_model = model  # use raw model name

    # rdagent 0.8.0 settings names (env_prefix LITELLM_, read at import time)
    env_vars["LITELLM_CHAT_MODEL"] = litellm_model
    os.environ["LITELLM_CHAT_MODEL"] = litellm_model

    # --- API key ---
    # litellm reads OPENAI_API_KEY directly for its OpenAI-protocol clients,
    # and rdagent's settings carry LITELLM_CHAT_OPENAI_API_KEY as the parallel
    # contract name. Both are set so neither layer can miss it.
    env_vars["OPENAI_API_KEY"] = api_key
    os.environ["OPENAI_API_KEY"] = api_key
    env_vars["LITELLM_CHAT_OPENAI_API_KEY"] = api_key
    os.environ["LITELLM_CHAT_OPENAI_API_KEY"] = api_key

    # Anthropic also reads its own env var (litellm reads it for
    # anthropic-prefixed models).
    if provider == "anthropic":
        env_vars["ANTHROPIC_API_KEY"] = api_key
        os.environ["ANTHROPIC_API_KEY"] = api_key

    # --- Base URL (OpenAI-compatible custom endpoints) ---
    # Written under every name a layer actually reads: the rdagent settings
    # name (LITELLM_CHAT_OPENAI_BASE_URL, parity), and the names litellm's
    # OpenAI-protocol transformation resolves the endpoint from
    # (OPENAI_BASE_URL / OPENAI_API_BASE). Without the litellm names the
    # configured endpoint is silently ignored and every completion goes to
    # api.openai.com.
    if base_url:
        env_vars["LITELLM_CHAT_OPENAI_BASE_URL"] = base_url
        os.environ["LITELLM_CHAT_OPENAI_BASE_URL"] = base_url
        env_vars["OPENAI_API_BASE"] = base_url
        os.environ["OPENAI_API_BASE"] = base_url
        env_vars["OPENAI_BASE_URL"] = base_url
        os.environ["OPENAI_BASE_URL"] = base_url
    else:
        for key in ("LITELLM_CHAT_OPENAI_BASE_URL", "OPENAI_API_BASE", "OPENAI_BASE_URL"):
            env_vars.pop(key, None)
            os.environ.pop(key, None)

    # --- Embedding model ---
    embedding_model = config.get("llm_embedding_model", _DEFAULT_EMBEDDING)
    env_vars["LITELLM_EMBEDDING_MODEL"] = embedding_model
    os.environ["LITELLM_EMBEDDING_MODEL"] = embedding_model

    return env_vars


def clear_llm_config() -> None:
    """Remove rdagent LLM env vars (call between tasks if needed)."""
    for key in ("LITELLM_CHAT_MODEL", "LITELLM_EMBEDDING_MODEL",
                "LITELLM_CHAT_OPENAI_API_KEY", "LITELLM_CHAT_OPENAI_BASE_URL",
                "OPENAI_API_KEY", "OPENAI_API_BASE", "OPENAI_BASE_URL",
                "ANTHROPIC_API_KEY"):
        os.environ.pop(key, None)
