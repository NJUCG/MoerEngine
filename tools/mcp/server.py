from __future__ import annotations

import json

from textwrap import dedent, indent
from typing import Any

from mcp.server.fastmcp import FastMCP

from engine_client import EngineClientError, get_default_client


MCP_SERVER_NAME = "moerengine"
MCP_SERVER_VERSION = "0.1.0"
MCP_RUNTIME_BEHAVIOR_RESOURCE_URI = "moerengine://guide/runtime-behavior"
MCP_SERVER_INSTRUCTIONS = dedent(
    """
    MoerEngine MCP exposes four tools.
    Use engine_ping first to confirm the engine remote module is enabled and reachable.
    Use scene_summary for a bounded read-only scene overview: readiness, source path, main camera or lights, root subtree stats, and a shallow root tree preview.
    Use scene_query for targeted lookup by exactly one selector: entity, name_exact, or name_contains. It returns structured node details, type tags, transforms, child counts, subtree stats, and a bounded child preview.
    Use python_execute for scene mutation or custom scripting. It executes through the current blocking remote HTTP path, so long sleeps, long loops, and infinite loops will block the tool call.
    Session policy is forwarded to runtime scripting. Stateless is best for one-shot scripts. NamedSession can preserve Python globals across calls.
    scene_summary and scene_query are first-phase query tools, not a full scene DSL.
    Read the runtime behavior guide resource at moerengine://guide/runtime-behavior for blocking semantics, long-running script caveats, and session guidance.
    """
).strip()
MCP_RUNTIME_BEHAVIOR_GUIDE = dedent(
        f"""
        # MoerEngine MCP Runtime Behavior

        This guide captures operational semantics that affect how an agent should plan tool usage.

        ## 1. python_execute is blocking

        `python_execute` goes through the current HTTP remote execute path:

        ```text
        MCP tools/call
            -> POST /api/script/execute
                -> RemoteScriptService::ExecuteAndWait(...)
                    -> ScriptHost::Submit(...)
                    -> future.get()
        ```

        Consequences:

        1. The MCP tool call does not complete until the script returns.
        2. A script that sleeps for 60 seconds causes the tool call to wait for about 60 seconds.
        3. Long loops and infinite loops hold the current HTTP request open.
        4. This is currently an engine-side behavior; the agent must plan around it.

        ## 2. What this path is good for

        Prefer `python_execute` for:

        1. One-shot scene inspection or mutation.
        2. Short synchronous workflows: query -> modify -> print result.
        3. Creating or moving a bounded number of objects and then returning.

        Avoid using it as the primary vehicle for:

        1. Continuous 60 Hz animation loops.
        2. Long-running jobs that need progress updates.
        3. Tasks that need cancellation mid-execution.
        4. Streaming stdout/stderr back incrementally.

        ## 3. How agents should handle long-running intent

        Current recommendation:

        1. Keep each `python_execute` call short and bounded whenever possible.
        2. If a task conceptually lasts a long time, split it into smaller calls and let the agent orchestrate them.
        3. Do not assume background work is visible or cancellable just because Python threads can be started.
        4. If the desired workflow truly needs non-blocking execution, streaming, or cancel, that is a remote/runtime capability gap rather than a prompt-engineering problem.

        ## 4. Session policy semantics

        `session_policy` is forwarded directly to runtime scripting:

        1. `Stateless`: best default for one-shot calls.
        2. `NamedSession`: preserves Python globals across calls.
        3. `SharedGlobal`: shared interpreter state.

        `NamedSession` can preserve variables and experimental background state, but it should not be treated as a guaranteed animation or job-system primitive.

        ## 5. Known caveat: background animation

        A previous experiment used `NamedSession` plus a background Python thread to update cube transforms at about 60 Hz.

        Observed result:

        1. Queried transform values changed over time.
        2. Continuous visible viewport motion was not reliably observed.

        Current conclusion:

        1. Treat background-thread animation as unresolved behavior.
        2. Do not present it as a stable supported pattern.

        ## 6. Query tool expectations

        `scene_summary` and `scene_query` are first-phase query tools.

        1. `scene_summary` is for bounded overview, not exhaustive dump.
        2. `scene_query` is for targeted lookup by one selector, not a full scene DSL.
        3. For richer filtering or enumeration, the underlying Scene query API still needs expansion.

        ## 7. Resource location

        This guide is exposed at:

        `{MCP_RUNTIME_BEHAVIOR_RESOURCE_URI}`
        """
).strip()
JSON_PAYLOAD_BEGIN = "__MOER_MCP_JSON_BEGIN__"
JSON_PAYLOAD_END = "__MOER_MCP_JSON_END__"


mcp = FastMCP(MCP_SERVER_NAME, instructions=MCP_SERVER_INSTRUCTIONS, log_level="ERROR")

# FastMCP 1.27.1 does not expose a public version parameter, and the metadata
# lookup used by its fallback path returns None in our launcher-installed cache
mcp._mcp_server.version = MCP_SERVER_VERSION


def _execute_remote_script(
    code: str,
    source_name: str,
    session_policy: str = "Stateless",
    session_id: str = "",
) -> dict[str, Any]:
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


def _extract_json_payload(stdout_text: str) -> Any:
    begin_index = stdout_text.find(JSON_PAYLOAD_BEGIN)
    end_index = stdout_text.find(JSON_PAYLOAD_END)
    if begin_index < 0 or end_index < 0 or end_index < begin_index:
        preview = stdout_text.strip()
        if len(preview) > 600:
            preview = preview[:600] + "..."
        raise RuntimeError(
            "MoerEngine scene query script did not emit a JSON payload. "
            f"stdout={preview!r}"
        )

    payload_text = stdout_text[begin_index + len(JSON_PAYLOAD_BEGIN) : end_index].strip()
    try:
        return json.loads(payload_text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Failed to decode MoerEngine scene query JSON payload: {exc}") from exc


def _execute_json_script(code: str, source_name: str) -> dict[str, Any]:
    remote_result = _execute_remote_script(code=code, source_name=source_name)
    exception_text = str(remote_result.get("exception_text") or "").strip()
    stderr_text = str(remote_result.get("stderr_text") or "").strip()
    if exception_text:
        details = exception_text
        if stderr_text:
            details += f"\n[stderr]\n{stderr_text}"
        raise RuntimeError(details)

    if not bool(remote_result.get("success", False)):
        details = "MoerEngine scene query script failed without exception text."
        if stderr_text:
            details += f"\n[stderr]\n{stderr_text}"
        raise RuntimeError(details)

    payload = _extract_json_payload(str(remote_result.get("stdout_text") or ""))
    if not isinstance(payload, dict):
        raise RuntimeError("MoerEngine scene query JSON payload must be an object.")

    payload["request_id"] = remote_result.get("request_id")
    return payload


def _build_scene_script(body: str, payload: dict[str, Any]) -> str:
    script_body = indent(body.strip(), "        ")
    return dedent(
        f"""
        import json
        import moer

        INPUT = json.loads({json.dumps(payload, ensure_ascii=False)!r})
        scene = moer.scene()

        def _transform_to_dict(transform):
            if transform is None:
                return None
            return {{
                "translation": {{
                    "x": transform.translation.x,
                    "y": transform.translation.y,
                    "z": transform.translation.z,
                }},
                "rotation": {{
                    "w": transform.rotation.w,
                    "x": transform.rotation.x,
                    "y": transform.rotation.y,
                    "z": transform.rotation.z,
                }},
                "scale": {{
                    "x": transform.scale.x,
                    "y": transform.scale.y,
                    "z": transform.scale.z,
                }},
            }}

        def _stats_to_dict(stats):
            return {{
                "node_count": stats.node_count,
                "renderable_count": stats.renderable_count,
                "camera_count": stats.camera_count,
                "light_count": stats.light_count,
                "contains_main_camera": stats.contains_main_camera,
                "contains_main_light_tag": stats.contains_main_light_tag,
            }}

        def _flags_to_dict(flags):
            return {{
                "is_valid_entity": flags.is_valid_entity,
                "is_node": flags.is_node,
                "is_root_node": flags.is_root_node,
                "is_renderable": flags.is_renderable,
                "is_camera": flags.is_camera,
                "is_light": flags.is_light,
                "is_directional_light": flags.is_directional_light,
                "is_point_light": flags.is_point_light,
                "is_main_camera": flags.is_main_camera,
                "is_main_light": flags.is_main_light,
            }}

        def _make_type_tags(flags):
            tags = []
            if flags.is_node:
                tags.append("Node")
            if flags.is_root_node:
                tags.append("RootNode")
            if flags.is_renderable:
                tags.append("Renderable")
            if flags.is_camera:
                tags.append("Camera")
            if flags.is_light:
                tags.append("Light")
            if flags.is_directional_light:
                tags.append("DirectionalLight")
            if flags.is_point_light:
                tags.append("PointLight")
            if flags.is_main_camera:
                tags.append("MainCamera")
            if flags.is_main_light:
                tags.append("MainLight")
            if not tags and flags.is_valid_entity:
                tags.append("Entity")
            return tags

        def _summarize_entity(entity, include_children_preview=False, child_preview_limit=8):
            flags = scene.get_entity_component_flags(entity)
            summary = {{
                "entity": entity,
                "component_flags": _flags_to_dict(flags),
                "type_tags": _make_type_tags(flags),
            }}
            if not flags.is_valid_entity:
                summary["is_valid_entity"] = False
                return summary

            summary["is_valid_entity"] = True
            if not flags.is_node:
                return summary

            child_count = scene.get_node_child_count(entity)
            summary.update(
                {{
                    "display_name": scene.get_node_display_name(entity),
                    "authored_name": scene.try_get_node_name(entity),
                    "child_count": child_count,
                    "is_root_node": flags.is_root_node,
                    "local_transform": _transform_to_dict(scene.try_get_node_local_transform(entity)),
                    "subtree_stats": _stats_to_dict(scene.get_node_subtree_stats(entity)),
                }}
            )

            if include_children_preview:
                preview = []
                for child_entity in scene.list_node_children(entity)[:child_preview_limit]:
                    preview.append(
                        {{
                            "entity": child_entity,
                            "display_name": scene.get_node_display_name(child_entity),
                            "authored_name": scene.try_get_node_name(child_entity),
                        }}
                    )
                summary["children_preview"] = preview
                summary["children_truncated"] = child_count > len(preview)

            return summary

        def _summarize_node_tree(entity, depth_remaining, max_children_per_node):
            if not scene.is_valid_node_entity(entity):
                return None

            summary = _summarize_entity(entity)
            if depth_remaining <= 0:
                return summary

            children = []
            for child_entity in scene.list_node_children(entity)[:max_children_per_node]:
                child_summary = _summarize_node_tree(child_entity, depth_remaining - 1, max_children_per_node)
                if child_summary is not None:
                    children.append(child_summary)

            summary["children"] = children
            summary["children_truncated"] = summary.get("child_count", 0) > len(children)
            return summary

        def _iter_node_entities(root_entity):
            if not scene.is_valid_node_entity(root_entity):
                return

            stack = [root_entity]
            while stack:
                entity = stack.pop()
                yield entity
                children = scene.list_node_children(entity)
                for child_entity in reversed(children):
                    stack.append(child_entity)

{script_body}

        print({JSON_PAYLOAD_BEGIN!r})
        print(json.dumps(result, ensure_ascii=False))
        print({JSON_PAYLOAD_END!r})
        """
    )


def _build_scene_summary_script(max_depth: int, max_children_per_node: int) -> str:
    return _build_scene_script(
        body=dedent(
            """
            root_entity = scene.get_root_node_entity()
            root_is_valid = scene.is_valid_node_entity(root_entity)

            main_camera_entity = scene.get_main_camera_entity() if root_is_valid else None
            main_directional_light_entity = scene.get_main_directional_light_entity() if root_is_valid else None
            main_point_light_entity = scene.get_main_point_light_entity() if root_is_valid else None

            result = {
                "scene_ready": scene.is_ready(),
                "is_start_loading": scene.is_start_loading(),
                "source_file_path": scene.get_source_file_path(),
                "root_node_entity": root_entity if root_is_valid else None,
                "main_camera_entity": main_camera_entity if main_camera_entity is not None and scene.is_valid_entity(main_camera_entity) else None,
                "main_directional_light_entity": (
                    main_directional_light_entity
                    if main_directional_light_entity is not None and scene.is_valid_entity(main_directional_light_entity)
                    else None
                ),
                "main_point_light_entity": (
                    main_point_light_entity
                    if main_point_light_entity is not None and scene.is_valid_entity(main_point_light_entity)
                    else None
                ),
                "root_subtree_stats": _stats_to_dict(scene.get_node_subtree_stats(root_entity)) if root_is_valid else None,
                "root_tree_preview": _summarize_node_tree(root_entity, INPUT["max_depth"], INPUT["max_children_per_node"]) if root_is_valid else None,
                "limits": {
                    "max_depth": INPUT["max_depth"],
                    "max_children_per_node": INPUT["max_children_per_node"],
                },
            }
            """
        ),
        payload={
            "max_depth": max_depth,
            "max_children_per_node": max_children_per_node,
        },
    )


def _build_scene_query_script(
    entity: int | None,
    name_exact: str,
    name_contains: str,
    max_results: int,
    child_preview_limit: int,
) -> str:
    return _build_scene_script(
        body=dedent(
            """
            selector_count = 0
            selector_count += 1 if INPUT["entity"] is not None else 0
            selector_count += 1 if INPUT["name_exact"] else 0
            selector_count += 1 if INPUT["name_contains"] else 0
            if selector_count != 1:
                raise ValueError("Provide exactly one of entity, name_exact, or name_contains.")

            root_entity = scene.get_root_node_entity()
            matches = []
            query_mode = ""
            truncated = False

            if INPUT["entity"] is not None:
                query_mode = "entity"
                requested_entity = int(INPUT["entity"])
                if scene.is_valid_entity(requested_entity):
                    matches.append(
                        _summarize_entity(
                            requested_entity,
                            include_children_preview=True,
                            child_preview_limit=INPUT["child_preview_limit"],
                        )
                    )
            else:
                query_mode = "name_exact" if INPUT["name_exact"] else "name_contains"
                needle_exact = INPUT["name_exact"]
                needle_contains = INPUT["name_contains"].lower()

                for entity in _iter_node_entities(root_entity):
                    authored_name = scene.try_get_node_name(entity) or ""
                    display_name = scene.get_node_display_name(entity)

                    matched = False
                    if needle_exact:
                        matched = authored_name == needle_exact or display_name == needle_exact
                    else:
                        matched = needle_contains in authored_name.lower() or needle_contains in display_name.lower()

                    if not matched:
                        continue

                    matches.append(
                        _summarize_entity(
                            entity,
                            include_children_preview=True,
                            child_preview_limit=INPUT["child_preview_limit"],
                        )
                    )
                    if len(matches) >= INPUT["max_results"]:
                        truncated = True
                        break

            result = {
                "query_mode": query_mode,
                "query": {
                    "entity": INPUT["entity"],
                    "name_exact": INPUT["name_exact"],
                    "name_contains": INPUT["name_contains"],
                    "max_results": INPUT["max_results"],
                    "child_preview_limit": INPUT["child_preview_limit"],
                },
                "match_count": len(matches),
                "truncated": truncated,
                "matches": matches,
            }
            """
        ),
        payload={
            "entity": entity,
            "name_exact": name_exact,
            "name_contains": name_contains,
            "max_results": max_results,
            "child_preview_limit": child_preview_limit,
        },
    )


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
        "including stdout, stderr, and exception text. The call waits until the script returns, so long sleeps "
        "or long loops keep the current MCP tool call open."
    )
)
def python_execute(
    code: str,
    session_policy: str = "Stateless",
    session_id: str = "",
    source_name: str = "remote/mcp",
) -> dict[str, Any]:
    """Execute a Python snippet through the engine remote bridge."""

    return _execute_remote_script(
        code=code,
        source_name=source_name,
        session_policy=session_policy,
        session_id=session_id,
    )


@mcp.resource(
    MCP_RUNTIME_BEHAVIOR_RESOURCE_URI,
    name="runtime-behavior",
    title="MoerEngine MCP Runtime Behavior",
    description="Operational caveats for blocking script execution, long-running tasks, sessions, and first-phase query tools.",
    mime_type="text/markdown",
)
def runtime_behavior_guide() -> str:
    """Return the protocol-visible runtime behavior guide for MoerEngine MCP."""

    return MCP_RUNTIME_BEHAVIOR_GUIDE


# Summarize the current scene with a bounded root subtree preview for agent consumption
@mcp.tool(
    description=(
        "Return a stable scene summary including readiness, source path, root subtree stats, "
        "main camera or light entities, and a bounded root tree preview."
    )
)
def scene_summary(max_depth: int = 2, max_children_per_node: int = 12) -> dict[str, Any]:
    """Return a bounded summary of the current runtime scene."""

    clamped_depth = max(0, min(max_depth, 4))
    clamped_children = max(1, min(max_children_per_node, 32))
    return _execute_json_script(
        _build_scene_summary_script(clamped_depth, clamped_children),
        source_name="mcp/scene_summary",
    )


# Query scene nodes by entity id, exact name, or case-insensitive name substring
@mcp.tool(
    description=(
        "Query scene entities by exact entity id, exact node name, or case-insensitive name substring, "
        "and return structured node details and type tags."
    )
)
def scene_query(
    entity: int | None = None,
    name_exact: str = "",
    name_contains: str = "",
    max_results: int = 20,
    child_preview_limit: int = 8,
) -> dict[str, Any]:
    """Query scene nodes by one explicit selector."""

    clamped_results = max(1, min(max_results, 64))
    clamped_child_preview = max(0, min(child_preview_limit, 16))
    return _execute_json_script(
        _build_scene_query_script(
            entity=entity,
            name_exact=name_exact,
            name_contains=name_contains,
            max_results=clamped_results,
            child_preview_limit=clamped_child_preview,
        ),
        source_name="mcp/scene_query",
    )


# Start the MCP stdio server after launcher bootstrap has prepared the environment
def main() -> int:
    mcp.run(transport="stdio")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())