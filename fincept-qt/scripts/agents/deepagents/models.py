"""
models.py — LangChain model creation from Fincept LLM config.

Single responsibility:
  - Take a config dict with llm_provider/llm_api_key/llm_model/llm_base_url
  - Return a BaseChatModel ready for deepagents / direct use
  - extract_text() handles both plain str and block-list responses (extended thinking)
"""

from __future__ import annotations

import logging
import sys
from pathlib import Path
from typing import Any

# The MarketLab hosted-destination guard lives one directory up
# (scripts/agents/marketlab_net_guard.py), and this module is also imported
# from paths that do not add scripts/agents to sys.path.
_GUARD_DIR = str(Path(__file__).resolve().parent.parent)
if _GUARD_DIR not in sys.path:
    sys.path.insert(0, _GUARD_DIR)
from marketlab_net_guard import guarded_async_http_client, guarded_http_client, reject_if_fincept

logger = logging.getLogger(__name__)

# Providers that support LangChain tool calling (bind_tools / tool_calls in AIMessage).
# Only these can use the deepagents library path.
TOOL_CALLING_PROVIDERS = {
    "anthropic",
    "openai",
    "google",
    "groq",
    "deepseek",
    "openrouter",
    "azure",
    "mistral",
    "cohere",
    "fireworks",
    "together",
}

# OpenAI-compatible providers (use ChatOpenAI with custom base_url)
_OPENAI_COMPAT = {
    "deepseek":   "https://api.deepseek.com/v1",
    "openrouter": "https://openrouter.ai/api/v1",
    "fireworks":  "https://api.fireworks.ai/inference/v1",
    "together":   "https://api.together.xyz/v1",
    "mistral":    "https://api.mistral.ai/v1",
    "azure":      None,  # base_url must be supplied by caller
}


def create_model(config: dict[str, Any]):
    """
    Create a LangChain BaseChatModel from a Fincept LLM config dict.

    Config keys:
        llm_provider  : str   — provider name (anthropic, openai, google, ...)
        llm_api_key   : str   — API key
        llm_model     : str   — model name/id
        llm_base_url  : str   — optional custom base URL

    Returns:
        BaseChatModel instance.

    Raises:
        ValueError  if provider is unknown or required packages are missing.
        ImportError if the required langchain-* package is not installed.
    """
    provider = config.get("llm_provider", "").lower().strip()
    api_key  = config.get("llm_api_key", "")
    model    = config.get("llm_model", "")
    base_url = config.get("llm_base_url") or None

    if not provider:
        raise ValueError("llm_provider is required in config")

    # MarketLab: a configured base_url that NAMES a Fincept-owned destination
    # is refused before any SDK below constructs a client around it
    # (FINCEPT_FORK_PLAN.md §5.3). The C++ AIQuantLabService forwarder performs
    # no host check of its own, so this is the boundary for this child.
    if base_url:
        reject_if_fincept(base_url)

    if provider == "anthropic":
        from langchain_anthropic import ChatAnthropic
        # MarketLab containment (§5.3): langchain_anthropic builds its own
        # httpx clients internally and exposes no http_client seam, and the
        # Anthropic SDK defaults to follow_redirects=True — so a custom
        # anthropic_api_url cannot be put on a guardable transport. The
        # configuration-derived route is therefore refused outright rather
        # than left to a transport we cannot vet; the default Anthropic
        # endpoint (a fixed vendor destination) still works. Configure an
        # OpenAI-compatible provider for custom endpoints.
        if base_url:
            raise ValueError(
                "Custom anthropic base URLs are unavailable in MarketLab "
                "Terminal: langchain_anthropic provides no guardable HTTP "
                "client seam (FINCEPT_FORK_PLAN.md §5.3). Configure an "
                "OpenAI-compatible provider for custom endpoints, or use the "
                "default Anthropic endpoint without llm_base_url."
            )
        kwargs: dict[str, Any] = {
            "api_key":    api_key,
            "model_name": model or "claude-sonnet-4-5-20250514",
        }
        return ChatAnthropic(**kwargs)

    if provider == "openai":
        from langchain_openai import ChatOpenAI
        kwargs = {"api_key": api_key, "model": model or "gpt-4o-mini"}
        if base_url:
            kwargs["base_url"] = base_url
            # The OpenAI SDK langchain builds follows redirects by default.
            # Guarded sync+async clients refuse Fincept-owned targets on every
            # dial, redirect hops included.
            kwargs["http_client"] = guarded_http_client()
            kwargs["http_async_client"] = guarded_async_http_client()
        return ChatOpenAI(**kwargs)

    if provider == "google":
        from langchain_google_genai import ChatGoogleGenerativeAI
        return ChatGoogleGenerativeAI(
            google_api_key=api_key,
            model=model or "gemini-2.0-flash",
        )

    if provider == "groq":
        from langchain_groq import ChatGroq
        return ChatGroq(
            api_key=api_key,
            model_name=model or "llama-3.3-70b-versatile",
        )

    if provider == "cohere":
        from langchain_cohere import ChatCohere
        return ChatCohere(
            cohere_api_key=api_key,
            model=model or "command-r-plus",
        )

    if provider in _OPENAI_COMPAT:
        from langchain_openai import ChatOpenAI
        url = base_url or _OPENAI_COMPAT[provider]
        if url is None:
            raise ValueError(f"llm_base_url is required for provider '{provider}'")
        return ChatOpenAI(
            api_key=api_key,
            model=model or "default",
            base_url=url,
            # Guarded sync+async clients, as in the openai branch above.
            http_client=guarded_http_client(),
            http_async_client=guarded_async_http_client(),
        )

    raise ValueError(
        f"Unknown llm_provider '{provider}'. "
        f"Supported: {sorted(TOOL_CALLING_PROVIDERS)}"
    )


def supports_tool_calling(config: dict[str, Any]) -> bool:
    """Return True if the configured provider supports LangChain tool calling."""
    provider = config.get("llm_provider", "").lower().strip()
    return provider in TOOL_CALLING_PROVIDERS


def extract_text(content: Any) -> str:
    """
    Extract plain text from a model response content value.

    Handles:
      - str                  — returned as-is
      - list of content blocks — extracts text from {"type": "text", "text": "..."} blocks,
                                 ignores thinking/redacted_thinking/tool_use blocks
      - anything else        — str() fallback
    """
    if isinstance(content, str):
        return content

    if isinstance(content, list):
        parts = [
            block["text"]
            for block in content
            if isinstance(block, dict) and block.get("type") == "text"
        ]
        return " ".join(parts)

    return str(content)
