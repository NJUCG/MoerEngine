from __future__ import annotations

import os

from typing import Any

import httpx


DEFAULT_REMOTE_BASE_URL = "http://127.0.0.1:18080"
DEFAULT_REMOTE_TIMEOUT_SECONDS = 30.0


def _build_remote_startup_hint(base_url: str) -> str:
    return (
        f"MoerEngine remote at {base_url} is not reachable. "
        "If the engine is not running on this development machine, start it with "
        "./target/bin/Debug/MoerEditor.exe or just r."
    )


def _build_remote_unreachable_message(base_url: str, detail: str) -> str:
    return f"MoerEngine remote request failed: {detail}. {_build_remote_startup_hint(base_url)}"


# Wrap transport and response failures from the engine remote HTTP bridge
class EngineClientError(RuntimeError):
    pass


# Talk to the existing runtime/remote HTTP endpoints used by MCP tools
class EngineClient:
    def __init__(self, base_url: str, timeout_seconds: float = DEFAULT_REMOTE_TIMEOUT_SECONDS) -> None:
        self._base_url = base_url.rstrip("/")
        self._client = httpx.Client(base_url=self._base_url, timeout=timeout_seconds)

    # Query the remote service health and bind information
    def get_remote_status(self) -> dict[str, Any]:
        return self._request("GET", "/api/remote/status")

    # Execute a Python snippet through the engine's existing blocking HTTP path
    def execute_script(
        self,
        code: str,
        source_name: str = "remote/mcp",
        execution_kind: str = "Snippet",
        session_policy: str = "Stateless",
        session_id: str = "",
        origin: str = "Mcp",
    ) -> dict[str, Any]:
        payload = {
            "code": code,
            "source_name": source_name,
            "execution_kind": execution_kind,
            "session_policy": session_policy,
            "session_id": session_id,
            "origin": origin,
        }
        return self._request("POST", "/api/script/execute", json_body=payload)

    # Close the underlying HTTP client when the process shuts down
    def close(self) -> None:
        self._client.close()

    # Normalize remote transport failures into concise tool-facing exceptions
    def _request(
        self,
        method: str,
        path: str,
        json_body: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        try:
            response = self._client.request(method, path, json=json_body)
            response.raise_for_status()
        except httpx.HTTPStatusError as exc:
            detail = exc.response.text.strip()
            raise EngineClientError(
                f"MoerEngine remote returned HTTP {exc.response.status_code}: {detail}"
            ) from exc
        except httpx.TimeoutException as exc:
            raise EngineClientError(
                _build_remote_unreachable_message(self._base_url, str(exc) or "timed out")
            ) from exc
        except httpx.ConnectError as exc:
            raise EngineClientError(
                _build_remote_unreachable_message(self._base_url, str(exc))
            ) from exc
        except httpx.HTTPError as exc:
            raise EngineClientError(f"MoerEngine remote request failed: {exc}") from exc

        try:
            payload = response.json()
        except ValueError as exc:
            raise EngineClientError("MoerEngine remote response was not valid JSON") from exc

        if not isinstance(payload, dict):
            raise EngineClientError("MoerEngine remote response JSON must be an object")

        return payload


_DEFAULT_CLIENT: EngineClient | None = None


# Reuse one HTTP client instance across all tool calls in the MCP process
def get_default_client() -> EngineClient:
    global _DEFAULT_CLIENT

    if _DEFAULT_CLIENT is None:
        base_url = os.environ.get("MOERENGINE_REMOTE_BASE_URL", DEFAULT_REMOTE_BASE_URL)
        _DEFAULT_CLIENT = EngineClient(base_url=base_url)
    return _DEFAULT_CLIENT