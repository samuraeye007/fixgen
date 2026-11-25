"""Extraction through any OpenAI-compatible chat completions endpoint.

This is the free path: point it at a local Ollama / LM Studio / llama.cpp / vLLM server
(no key, nothing leaves your machine) or at a free hosted tier that speaks the same API
(Gemini, Groq, OpenRouter). Most of these models do not read PDFs, so each chunk is sent
as layout-preserving text extracted with pypdf, with page markers.

Only the standard library is used for HTTP so this adds no dependencies.
"""
from __future__ import annotations

import io
import json
import os
import time
import urllib.error
import urllib.request

import jsonschema

from .prompts import SYSTEM, chunk_prompt
from .schema import CHUNK_SCHEMA

# Hosts people commonly use for free; --base-url accepts any other.
PRESETS = {
    "ollama": "http://localhost:11434/v1",
    "lmstudio": "http://localhost:1234/v1",
    "gemini": "https://generativelanguage.googleapis.com/v1beta/openai",
    "groq": "https://api.groq.com/openai/v1",
    "openrouter": "https://openrouter.ai/api/v1",
}


def chunk_text(pdf: bytes, first_page: int) -> str:
    from pypdf import PdfReader

    reader = PdfReader(io.BytesIO(pdf))
    parts = []
    for i, page in enumerate(reader.pages):
        try:
            text = page.extract_text(extraction_mode="layout")
        except Exception:  # some PDFs break layout mode; plain mode is better than nothing
            text = page.extract_text()
        parts.append(f"===== page {first_page + i} =====\n{text.rstrip()}")
    return "\n\n".join(parts)


class OpenAICompatibleExtractor:
    def __init__(self, model: str, base_url: str = PRESETS["ollama"], api_key: str | None = None,
                 timeout: float = 900, structured: str = "json_schema", post=None,
                 max_retries: int = 6, sleep=time.sleep):
        """structured: "json_schema" (constrained decoding, where the server supports it),
        "json_object" (JSON mode + schema in the prompt) or "none" (prompt only)."""
        self.model = model
        self.base_url = PRESETS.get(base_url, base_url).rstrip("/")
        self.api_key = api_key if api_key is not None else os.environ.get("SPECGEN_API_KEY", "")
        self.timeout = timeout
        self.structured = structured
        self._post = post or self._http_post
        self.max_retries = max_retries
        self._sleep = sleep

    # -- transport ---------------------------------------------------------------------------
    def _http_post(self, url: str, body: dict) -> dict:
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            return json.loads(r.read())

    def _complete(self, messages: list[dict], structured: str) -> str:
        from .extract import ExtractionError

        body: dict = {"model": self.model, "messages": messages, "temperature": 0}
        if structured == "json_schema":
            body["response_format"] = {"type": "json_schema",
                                       "json_schema": {"name": "chunk_extraction", "strict": True,
                                                       "schema": CHUNK_SCHEMA}}
        elif structured == "json_object":
            body["response_format"] = {"type": "json_object"}
        for attempt in range(self.max_retries + 1):
            try:
                resp = self._post(f"{self.base_url}/chat/completions", body)
                break
            except urllib.error.HTTPError as e:
                detail = e.read().decode(errors="replace")[:500]
                # Free tiers rate-limit hard: back off on 429 / overload instead of failing the run.
                if e.code in (429, 500, 502, 503, 504) and attempt < self.max_retries:
                    retry_after = e.headers.get("Retry-After") if e.headers else None
                    delay = float(retry_after) if retry_after and retry_after.isdigit() else min(120, 10 * 2 ** attempt)
                    self._sleep(delay)
                    continue
                raise ExtractionError(f"HTTP {e.code} from {self.base_url}: {detail}") from e
            except urllib.error.URLError as e:
                raise ExtractionError(f"cannot reach {self.base_url}: {e.reason}") from e
        choice = (resp.get("choices") or [{}])[0]
        if choice.get("finish_reason") == "length":
            raise ExtractionError("output truncated (finish_reason=length); use a smaller --pages-per-chunk")
        return (choice.get("message") or {}).get("content") or ""

    # -- extractor interface ---------------------------------------------------------------
    def __call__(self, chunk, venue_name: str) -> dict:
        from .extract import ExtractionError

        system = SYSTEM
        if self.structured != "json_schema":
            system += ("\n\nReply with a single JSON object, no prose, matching this JSON schema:\n"
                       + json.dumps(CHUNK_SCHEMA))
        user = (chunk_prompt(venue_name, chunk.first_page, chunk.last_page, chunk.total_pages)
                + "\n\n" + chunk_text(chunk.pdf, chunk.first_page))
        messages = [{"role": "system", "content": system}, {"role": "user", "content": user}]
        where = f"pages {chunk.first_page}-{chunk.last_page}"

        structured = self.structured
        for attempt in range(2):
            try:
                text = self._complete(messages, structured)
            except ExtractionError as e:
                # Server without json_schema support: fall back to JSON mode once.
                if structured == "json_schema" and ("response_format" in str(e) or "json_schema" in str(e)):
                    structured = "json_object"
                    messages[0]["content"] = SYSTEM + ("\n\nReply with a single JSON object, no prose, matching "
                                                       "this JSON schema:\n" + json.dumps(CHUNK_SCHEMA))
                    continue
                raise ExtractionError(f"{where}: {e}") from e
            try:
                data = json.loads(_strip_fences(text))
                jsonschema.validate(data, CHUNK_SCHEMA)
                return data
            except (json.JSONDecodeError, jsonschema.ValidationError) as e:
                if attempt == 1:
                    raise ExtractionError(f"{where}: model output did not match the schema: {str(e)[:300]}") from e
                # One repair round: show the model its error.
                messages += [{"role": "assistant", "content": text},
                             {"role": "user", "content": f"That output is invalid: {str(e)[:500]}. "
                                                         "Reply again with only the corrected JSON object."}]
        raise ExtractionError(f"{where}: no valid output")


def _strip_fences(text: str) -> str:
    t = text.strip()
    if t.startswith("```"):
        t = t.split("\n", 1)[1] if "\n" in t else ""
        t = t.rsplit("```", 1)[0]
    return t.strip()
