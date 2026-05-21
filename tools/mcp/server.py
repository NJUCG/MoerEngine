from __future__ import annotations

from typing import Any

from mcp.server.fastmcp import FastMCP

from engine_client import EngineClientError, get_default_client


MCP_SERVER_NAME = "moerengine"
MCP_SERVER_VERSION = "0.1.0"


mcp = FastMCP(MCP_SERVER_NAME, log_level="ERROR")

# FastMCP 1.27.1 does not expose a public version parameter, and the metadata
# lookup used by its fallback path returns None in our launcher-installed cache
mcp._mcp_server.version = MCP_SERVER_VERSION


# Query the engine remote status endpoint through the MCP bridge
@mcp.tool(description="Query the MoerEngine remote module status, bind address, and HTTP/WebSocket ports.")
def engine_ping() -> dict[str, Any]:
    """Query the currently configured engine remote status."""

    client = get_default_client()
    try:
        status = client.get_remote_status()
    except EngineClientError as exc:
        raise RuntimeError(str(exc)) from exc

    return {
        "ok": bool(status.get("running")),
        "remote_status": status,
    }


# Execute a Python snippet through the existing blocking remote HTTP endpoint
@mcp.tool(
    description=(
        "Execute a Python snippet through MoerEngine remote scripting and return the blocking result, "
        "including stdout, stderr, and exception text."
    )
)
def python_execute(
    code: str,
    session_policy: str = "Stateless",
    session_id: str = "",
    source_name: str = "remote/mcp",
) -> dict[str, Any]:
    """Execute a Python snippet through the engine remote bridge."""

    client = get_default_client()
    try:
        return client.execute_script(
            code=code,
            source_name=source_name,
            session_policy=session_policy,
            session_id=session_id,
            origin="Mcp",
        )
    except EngineClientError as exc:
        raise RuntimeError(str(exc)) from exc


# Reserve the scene summary tool name while the scene-facing query design settles
@mcp.tool(description="Return a placeholder scene summary result until the scene-query phase is implemented.")
def scene_summary() -> dict[str, Any]:
    """Return the current placeholder response for scene summary."""

    return {
        "implemented": False,
        "message": "TODO: scene_summary will be connected in the next MCP phase",
    }


# Reserve the scene query tool name while the first query contract is finalized
@mcp.tool(description="Return a placeholder scene query result until the scene-query phase is implemented.")
def scene_query(query: str = "") -> dict[str, Any]:
    """Return the current placeholder response for scene query."""

    return {
        "implemented": False,
        "query": query,
        "message": "TODO: scene_query will be connected in the next MCP phase",
    }


# Start the MCP stdio server after launcher bootstrap has prepared the environment
def main() -> int:
    mcp.run(transport="stdio")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())