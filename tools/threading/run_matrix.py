from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

@dataclass(frozen=True)
class Scenario:
    name: str
    renderer: str
    render_thread: bool
    max_frame_lag: int
    render_graph_debug_dump: bool = False
    render_graph_parallel_recording: bool = False
    window_stress: bool = False
    editor_args: tuple[str, ...] = ()
    fault_validation: bool = False
    renderer_switch_validation: bool = False
    raster_lifecycle_validation: bool = False
    framebuffer_validation: str | None = None
    expect_exit_after_ready: bool = False
    ready_log_pattern: str | None = None
    ready_settle_seconds: float | None = None
    require_profile_logs: bool = True

FUNCTIONAL_SCENARIOS = (
    Scenario("raster_gt", "Raster", False, 0, window_stress=True),
    Scenario("raster_rt0", "Raster", True, 0),
    Scenario("raster_rt1", "Raster", True, 1, window_stress=True),
    Scenario("ray_gt", "Raytracing", False, 0),
    Scenario("ray_rt0", "Raytracing", True, 0),
    Scenario("ray_rt1", "Raytracing", True, 1, window_stress=True),
)

RENDERGRAPH_SCENARIOS = (
    Scenario(
        "raster_graph_gt", "Raster", False, 0,
        render_graph_debug_dump=True,
        window_stress=True,
    ),
    Scenario(
        "raster_graph_rt0", "Raster", True, 0,
        render_graph_debug_dump=True,
    ),
    Scenario(
        "raster_graph_rt1", "Raster", True, 1,
        render_graph_debug_dump=True,
        window_stress=True,
    ),
    Scenario(
        "raster_graph_rt1_reload_switch", "Raster", True, 1,
        render_graph_debug_dump=True,
        editor_args=("--threading-renderer-switch-validation",),
        renderer_switch_validation=True,
        expect_exit_after_ready=True,
        ready_log_pattern="[ThreadingValidation][RendererSwitch] Complete:",
        require_profile_logs=False,
    ),
)

RENDERGRAPH_RESOURCE_SCENARIOS = tuple(
    Scenario(
        f"raster_key_{name}_{mode}", "Raster", render_thread, max_frame_lag,
        render_graph_debug_dump=True,
        framebuffer_validation=framebuffer,
    )
    for name, framebuffer in (
        ("base", "base_color"),
        ("normal", "normal"),
        ("depth", "depth_linear_sampler"),
    )
    for mode, render_thread, max_frame_lag in (("gt", False, 0), ("rt1", True, 1))
)

FAULT_SCENARIOS = (
    Scenario(
        "ray_rt1_fault_present_submit", "Raytracing", True, 1,
        editor_args=("--vulkan-fault-inject=present-submit@3",),
        fault_validation=True,
        ready_settle_seconds=1.0,
    ),
)

FEATURE_VALIDATION_SCENARIOS = (
    Scenario(
        "feature_renderer_switch", "Raster", True, 1,
        editor_args=("--threading-renderer-switch-validation", "--no-splash"),
        renderer_switch_validation=True,
        expect_exit_after_ready=True,
        ready_log_pattern="[ThreadingValidation][RendererSwitch] Complete:",
        require_profile_logs=False,
    ),
    Scenario(
        "feature_raster_graph_serial_recording", "Raster", True, 1,
        render_graph_debug_dump=True,
        editor_args=("--threading-raster-lifecycle-validation", "--no-splash"),
        raster_lifecycle_validation=True,
        expect_exit_after_ready=True,
        ready_log_pattern="[ThreadingValidation][RasterLifecycle] Complete:",
        require_profile_logs=False,
    ),
    Scenario(
        "feature_raster_graph_parallel_recording", "Raster", True, 1,
        render_graph_debug_dump=True,
        render_graph_parallel_recording=True,
        editor_args=("--threading-raster-lifecycle-validation", "--no-splash"),
        raster_lifecycle_validation=True,
        expect_exit_after_ready=True,
        ready_log_pattern="[ThreadingValidation][RasterLifecycle] Complete:",
        require_profile_logs=False,
    ),
    Scenario(
        "feature_raster_graph_parallel_recording_gt", "Raster", False, 0,
        render_graph_debug_dump=True,
        render_graph_parallel_recording=True,
        editor_args=("--threading-raster-lifecycle-validation", "--no-splash"),
        raster_lifecycle_validation=True,
        expect_exit_after_ready=True,
        ready_log_pattern="[ThreadingValidation][RasterLifecycle] Complete:",
        require_profile_logs=False,
    ),
    Scenario(
        "feature_raster_graph_parallel_recording_reload_switch", "Raster", True, 1,
        render_graph_debug_dump=True,
        render_graph_parallel_recording=True,
        editor_args=("--threading-renderer-switch-validation", "--no-splash"),
        renderer_switch_validation=True,
        expect_exit_after_ready=True,
        ready_log_pattern="[ThreadingValidation][RendererSwitch] Complete:",
        require_profile_logs=False,
    ),
)

UPPER_PARALLEL_RECORD_GATES = (
    r"\[RenderGraph\]\[CompiledMerge\] graph=RasterFrame dispatch=task-graph worker_jobs=(?:[2-9]|[1-9][0-9]+) completed=true",
)

SCENARIOS = {
    scenario.name: scenario
    for scenario in (
        *FUNCTIONAL_SCENARIOS,
        *RENDERGRAPH_SCENARIOS,
        *RENDERGRAPH_RESOURCE_SCENARIOS,
        *FAULT_SCENARIOS,
        *FEATURE_VALIDATION_SCENARIOS,
    )
}

SCENARIO_SETS = {
    "smoke": ("raster_gt", "raster_rt1", "ray_rt1"),
    "full": tuple(scenario.name for scenario in FUNCTIONAL_SCENARIOS),
    "rendergraph": tuple(scenario.name for scenario in RENDERGRAPH_SCENARIOS),
    "rendergraph-resources": tuple(
        scenario.name for scenario in RENDERGRAPH_RESOURCE_SCENARIOS
    ),
    "soak": ("raster_rt1", "ray_rt1"),
    "fault": tuple(scenario.name for scenario in FAULT_SCENARIOS),
    "feature": tuple(scenario.name for scenario in FEATURE_VALIDATION_SCENARIOS),
}

SECTION_PATTERN = re.compile(r"^\s*\[([^]]+)]\s*(?:#.*)?$")
ASSIGNMENT_PATTERN = re.compile(r"^(?P<indent>\s*)(?P<key>[A-Za-z0-9_-]+)\s*=")

def toml_literal(value: object) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return json.dumps(value)
    raise TypeError(f"Unsupported TOML scalar: {value!r}")

def generate_config(base_text: str, scenario: Scenario) -> str:
    replacements = {
        ("engine.threading", "render_thread"): scenario.render_thread,
        ("engine.threading", "max_frame_lag"): scenario.max_frame_lag,
        ("engine.threading", "profile_logging"): True,
        ("engine.render", "default_render_method"): scenario.renderer,
        (
            "engine.render.raster",
            "render_graph_debug_dump",
        ): scenario.render_graph_debug_dump,
        (
            "engine.render.raster",
            "render_graph_parallel_recording",
        ): scenario.render_graph_parallel_recording,
    }
    replaced = {target: 0 for target in replacements}
    current_section = ""
    output: list[str] = []

    for line in base_text.splitlines(keepends=True):
        line_without_newline = line.rstrip("\r\n")
        newline = line[len(line_without_newline) :]
        section_match = SECTION_PATTERN.match(line_without_newline)
        if section_match:
            current_section = section_match.group(1).strip()
            output.append(line)
            continue

        assignment_match = ASSIGNMENT_PATTERN.match(line_without_newline)
        if assignment_match:
            target = (current_section, assignment_match.group("key"))
            if target in replacements:
                output.append(
                    f"{assignment_match.group('indent')}{target[1]} = "
                    f"{toml_literal(replacements[target])}{newline}"
                )
                replaced[target] += 1
                continue
        output.append(line)

    invalid = [target for target, count in replaced.items() if count != 1]
    if invalid:
        details = ", ".join(f"{section}.{key}={replaced[(section, key)]}" for section, key in invalid)
        raise ValueError(f"Base config does not contain each required key exactly once: {details}")

    generated = "".join(output)
    parsed = tomllib.loads(generated)
    threading = parsed["engine"]["threading"]
    render = parsed["engine"]["render"]
    raster = render["raster"]
    actual = {
        ("engine.threading", "render_thread"): threading["render_thread"],
        ("engine.threading", "max_frame_lag"): threading["max_frame_lag"],
        ("engine.threading", "profile_logging"): threading["profile_logging"],
        ("engine.render", "default_render_method"): render["default_render_method"],
        ("engine.render.raster", "render_graph_debug_dump"): raster[
            "render_graph_debug_dump"
        ],
        ("engine.render.raster", "render_graph_parallel_recording"): raster[
            "render_graph_parallel_recording"
        ],
    }
    if actual != replacements:
        raise ValueError(f"Generated config validation failed: expected={replacements}, actual={actual}")
    return generated

def bool_text(value: bool) -> str:
    return "true" if value else "false"

def required_patterns(scenario: Scenario) -> list[str]:
    execution_thread = "Render" if scenario.render_thread else "Game"
    patterns = [
        "[Config] Using command-line config override:",
        (
            f"[Threading] render_thread={bool_text(scenario.render_thread)}, "
            f"max_frame_lag={scenario.max_frame_lag}"
        ),
        "[RHIExecutor][Vulkan] ownership runtime started",
        (
            f"[Threading] {scenario.renderer} frames execute on "
            f"{execution_thread} thread id ="
        ),
    ]
    patterns.append(f"[RenderGraph] {scenario.renderer} execution mode: compiled")
    if scenario.require_profile_logs and not scenario.fault_validation:
        patterns.append("[ThreadingProfile][Prepare]")
    if scenario.render_thread and scenario.max_frame_lag == 1:
        patterns.append("[Threading] GT/RT overlap active")
    if scenario.render_thread and scenario.require_profile_logs and not scenario.fault_validation:
        patterns.append("[ThreadingProfile][RT]")
    if scenario.renderer == "Raster" and scenario.render_graph_debug_dump:
        patterns.append("[RenderGraph][DebugDump]")
        patterns.append(
            "upper recording: "
            + (
                "parallel-frontend/merged-submit"
                if scenario.render_graph_parallel_recording
                else "serial-frontend/merged-submit"
            )
        )
    if scenario.renderer_switch_validation:
        patterns.extend(
            [
                "[ThreadingValidation][RendererSwitch] Enabled",
                "[ThreadingValidation][RendererSwitch] Request Raster reload",
                "[ThreadingValidation][RendererSwitch] Request Raster->Raytracing reload",
                "[ThreadingValidation][RendererSwitch] Request Raytracing->Raster reload",
                "[ThreadingValidation][RendererSwitch] Complete:",
                (
                    "[Threading] Raytracing frames execute on "
                    f"{execution_thread} thread id ="
                ),
            ]
        )
    if scenario.raster_lifecycle_validation:
        patterns.extend(
            [
                "[ThreadingValidation][RasterLifecycle] Enabled",
                "[ThreadingValidation][RasterLifecycle] Complete:",
            ]
        )
    if scenario.framebuffer_validation:
        patterns.append(
            "[ThreadingValidation][RasterFramebuffer] selection="
            + scenario.framebuffer_validation
        )
    return patterns

def build_verifier_command(
    verifier: Path,
    exe: Path,
    workdir: Path,
    run_dir: Path,
    config_path: Path,
    scenario: Scenario,
    args: argparse.Namespace,
) -> list[str]:
    ready_settle_seconds = (
        scenario.ready_settle_seconds
        if scenario.ready_settle_seconds is not None
        else args.ready_settle_seconds
    )
    command = [
        sys.executable,
        str(verifier),
        "--exe",
        str(exe),
        "--workdir",
        str(workdir),
        "--outdir",
        str(run_dir),
        "--config",
        str(config_path),
        "--startup-timeout",
        str(args.startup_timeout),
        "--ready-log-pattern",
        scenario.ready_log_pattern or args.ready_log_pattern,
        "--ready-timeout",
        str(args.ready_timeout),
        "--ready-settle-seconds",
        str(ready_settle_seconds),
        "--close-timeout",
        str(args.close_timeout),
        "--min-nonblack-ratio",
        str(args.min_nonblack_ratio),
    ]
    for editor_arg in scenario.editor_args:
        command.append(f"--editor-arg={editor_arg}")
    if scenario.framebuffer_validation:
        command.append(
            "--editor-arg=--threading-raster-framebuffer-validation="
            + scenario.framebuffer_validation
        )
    if scenario.expect_exit_after_ready:
        command.append("--expect-exit-after-ready")
    command.extend(["--forbid-log-pattern", "did not provide a compiled resource plan"])
    for pattern in required_patterns(scenario):
        command.extend(["--require-log-pattern", pattern])
    if scenario.render_graph_parallel_recording:
        for pattern in UPPER_PARALLEL_RECORD_GATES:
            command.extend(["--require-log-min-count", pattern, "1"])
    if scenario.renderer == "Raster":
        raster_mode_marker = (
            "[RenderGraph] Raster execution mode: compiled"
        )
        expected_raster_instances = 3 if scenario.renderer_switch_validation else 1
        command.extend(
            [
                "--require-log-count",
                re.escape(raster_mode_marker),
                str(expected_raster_instances),
            ]
        )
        if scenario.render_graph_debug_dump:
            command.extend(
                ["--forbid-log-pattern", r"\[RenderGraph\]\[Fallback\]"]
            )
            if scenario.renderer_switch_validation:
                command.extend(
                    [
                        "--require-log-min-count",
                        r"\[RenderGraph\]\[DebugDump\]",
                        str(expected_raster_instances),
                    ]
                )
    if scenario.renderer_switch_validation:
        lifecycle_counts = (
            ("[Threading] Render frame queue drained before renderer shutdown/reload.", 4),
            ("[Threading] Destroying renderer on Render Thread.", 4),
            ("[Threading] Renderer destroyed on Render Thread.", 4),
            ("[Threading] RasterRenderer destruction finished on Render Thread.", 3),
            ("[Threading] RaytracingRenderer destruction finished on Render Thread.", 1),
        )
        for marker, expected_count in lifecycle_counts:
            command.extend(
                ["--require-log-count", re.escape(marker), str(expected_count)]
            )
    if scenario.fault_validation:
        exact_injection_line = (
            r"^\[[^\]\r\n]+\] \[info\] \[[^\]\r\n]+\] "
            r"\[VulkanFault\]\[Injection\] point=present-submit "
            r"trigger=3 mode=synthetic-device-lost$"
        )
        exact_first_fault_line = (
            r"^\[[^\]\r\n]+\] \[error\] \[[^\]\r\n]+\] "
            r"\[VulkanFault\]\[First\] operation=PresentSubmit "
            r"result=VK_ERROR_DEVICE_LOST result_code=-4 queue=Graphics "
            r"queue_handle=0x[1-9a-f][0-9a-f]* timeline=[1-9][0-9]* "
            r"work_serial=[1-9][0-9]* thread=[1-9][0-9]* "
            r"injected=true predrained=true$"
        )
        exact_summary_line = (
            r"^\[[^\]\r\n]+\] \[info\] \[[^\]\r\n]+\] "
            r"\[VulkanFault\]\[Summary\] first_fault_count=1 "
            r"native_submit_after_fault=0 native_present_after_fault=0 "
            r"(?:rejected_submit=[1-9][0-9]* rejected_present=[0-9]+|"
            r"rejected_submit=0 rejected_present=[1-9][0-9]*) "
            r"device_lost=true "
            r"skipped_command_pool_reset=true allocator_quarantined=true "
            r"sync_completed=true quarantine_count=[1-9][0-9]* "
            r"queue_sync_count=3$"
        )
        fault_log_patterns = (
            r"\[VulkanFault\]\[Injection\]",
            r"\[VulkanFault\]\[First\]",
            r"\[VulkanFault\]\[Summary\]",
            exact_injection_line,
            exact_first_fault_line,
            exact_summary_line,
        )
        command.extend(
            [
                "--allow-forbidden-log-pattern",
                exact_first_fault_line,
            ]
        )
        for pattern in fault_log_patterns:
            command.extend(["--require-log-count", pattern, "1"])
    if args.skip_window_stress or not scenario.window_stress:
        command.append("--skip-window-stress")
    if args.detach_configs:
        command.append("--detach-configs")
    if args.soak_seconds > 0.0:
        command.extend(["--soak-seconds", str(args.soak_seconds)])
    return command

def parse_profile_value(value: str) -> object:
    try:
        return int(value)
    except ValueError:
        try:
            return float(value)
        except ValueError:
            return value

def parse_profile_windows(path: Path) -> dict[str, list[dict[str, object]]]:
    profiles: dict[str, list[dict[str, object]]] = {
        "RT": [],
        "Prepare": [],
    }
    if not path.is_file():
        return profiles

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        for profile_type in profiles:
            marker = f"[ThreadingProfile][{profile_type}]"
            marker_index = line.find(marker)
            if marker_index < 0:
                continue
            fields = {}
            for key, value in re.findall(
                r"([A-Za-z_]+)=([^\s]+)", line[marker_index + len(marker) :]
            ):
                fields[key] = parse_profile_value(value)
            profiles[profile_type].append(fields)
            break
    return profiles

def aggregate_profile_windows(
    windows: list[dict[str, object]], profile_type: str, tail_count: int
) -> dict[str, object]:
    selected = windows[-tail_count:] if tail_count > 0 else windows
    if not selected:
        return {}

    sample_key = "samples" if profile_type == "Prepare" else "frames"
    sample_count = sum(int(window.get(sample_key, 0)) for window in selected)
    window_ms = sum(float(window.get("window_ms", 0.0)) for window in selected)

    def weighted_average(field: str) -> float:
        if sample_count == 0:
            return 0.0
        return sum(
            float(window.get(field, 0.0)) * int(window.get(sample_key, 0))
            for window in selected
        ) / sample_count

    def weighted_average_by(
        field: str, count_field: str, fallback_count_field: str | None = None
    ) -> float:
        def field_count(window: dict[str, object]) -> int:
            fallback = window.get(fallback_count_field, 0) if fallback_count_field else 0
            return int(window.get(count_field, fallback))

        count = sum(field_count(window) for window in selected)
        if count == 0:
            return 0.0
        return sum(
            float(window.get(field, 0.0)) * field_count(window)
            for window in selected
        ) / count

    aggregate: dict[str, object] = {
        "window_count": len(selected),
        "sample_count": sample_count,
        "window_ms": window_ms,
        "samples_per_second": sample_count * 1000.0 / window_ms if window_ms > 0.0 else 0.0,
        "windows": selected,
    }
    if profile_type == "RT":
        aggregate.update(
            {
                "prepare_sample_count": sum(
                    int(window.get("prepare_samples", window.get("frames", 0)))
                    for window in selected
                ),
                "gt_wait_sample_count": sum(
                    int(window.get("gt_wait_samples", window.get("frames", 0)))
                    for window in selected
                ),
                "prepare_avg_ms": weighted_average_by(
                    "prepare_avg_ms", "prepare_samples", "frames"
                ),
                "prepare_max_ms": max(float(window.get("prepare_max_ms", 0.0)) for window in selected),
                "queue_wait_avg_ms": weighted_average("queue_wait_avg_ms"),
                "queue_wait_max_ms": max(
                    float(window.get("queue_wait_max_ms", 0.0)) for window in selected
                ),
                "render_avg_ms": weighted_average("render_avg_ms"),
                "render_max_ms": max(float(window.get("render_max_ms", 0.0)) for window in selected),
                "gt_wait_avg_ms": weighted_average_by(
                    "gt_wait_avg_ms", "gt_wait_samples", "frames"
                ),
                "gt_wait_max_ms": max(float(window.get("gt_wait_max_ms", 0.0)) for window in selected),
                "max_pending": max(int(window.get("max_pending", 0)) for window in selected),
            }
        )
    elif profile_type == "Prepare":
        aggregate.update(
            {
                "renderer": selected[-1].get("renderer"),
                "total_avg_ms": weighted_average("total_avg_ms"),
                "total_max_ms": max(
                    float(window.get("total_max_ms", 0.0)) for window in selected
                ),
                "window_avg_ms": weighted_average("window_avg_ms"),
                "scripting_avg_ms": weighted_average("scripting_avg_ms"),
                "test_avg_ms": weighted_average("test_avg_ms"),
                "ui_tick_avg_ms": weighted_average("ui_tick_avg_ms"),
                "camera_and_test_avg_ms": weighted_average("camera_and_test_avg_ms"),
                "config_snapshot_avg_ms": weighted_average("config_snapshot_avg_ms"),
                "scene_update_avg_ms": weighted_average("scene_update_avg_ms"),
                "scene_update_max_ms": max(
                    float(window.get("scene_update_max_ms", 0.0)) for window in selected
                ),
                "scene_snapshot_avg_ms": weighted_average("scene_snapshot_avg_ms"),
                "scene_snapshot_max_ms": max(
                    float(window.get("scene_snapshot_max_ms", 0.0)) for window in selected
                ),
                "ui_composition_avg_ms": weighted_average("ui_composition_avg_ms"),
                "ui_draw_packet_avg_ms": weighted_average("ui_draw_packet_avg_ms"),
                "ui_draw_packet_max_ms": max(
                    float(window.get("ui_draw_packet_max_ms", 0.0)) for window in selected
                ),
                "other_avg_ms": weighted_average("other_avg_ms"),
                "scene_ready_frames": sum(
                    int(window.get("scene_ready_frames", 0)) for window in selected
                ),
                "scene_dirty_frames": sum(
                    int(window.get("scene_dirty_frames", 0)) for window in selected
                ),
                "initial_gpu_update_frames": sum(
                    int(window.get("initial_gpu_update_frames", 0)) for window in selected
                ),
                "update_gpu_update_frames": sum(
                    int(window.get("update_gpu_update_frames", 0)) for window in selected
                ),
                "geometry_snapshot_frames": sum(
                    int(window.get("geometry_snapshot_frames", 0)) for window in selected
                ),
                "scene_snapshot_build_frames": sum(
                    int(window.get("scene_snapshot_build_frames", 0)) for window in selected
                ),
                "ui_vertices_avg": weighted_average("ui_vertices_avg"),
                "ui_indices_avg": weighted_average("ui_indices_avg"),
                "ui_commands_avg": weighted_average("ui_commands_avg"),
                "ui_vertices_max": max(
                    int(window.get("ui_vertices_max", 0)) for window in selected
                ),
            }
        )
    return aggregate

def profile_number(run: dict[str, object], profile: str, field: str) -> float:
    data = run.get(profile, {})
    return float(data.get(field, 0.0)) if isinstance(data, dict) else 0.0

def write_markdown_summary(path: Path, summary: dict[str, object]) -> None:
    lines = [
        "# Renderer Threading Validation",
        "",
        f"- Started: `{summary['started_at']}`",
        f"- Duration: `{summary['duration_seconds']:.1f}s`",
        f"- Result: `{'PASS' if summary['success'] else 'FAIL'}`",
        "",
        "| Scenario | Iteration | Result | RT queue | RT render | GT wait |",
        "|---|---:|---|---:|---:|---:|",
    ]
    for run in summary["runs"]:
        lines.append(
            f"| {run['scenario']} | {run['iteration']} | "
            f"{'PASS' if run['success'] else 'FAIL'} | "
            f"{profile_number(run, 'rt_profile', 'queue_wait_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'rt_profile', 'render_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'rt_profile', 'gt_wait_avg_ms'):.3f} ms |"
        )
    lines.extend(
        [
            "",
            "## PrepareFrame Breakdown",
            "",
            "All averages are weighted by the `samples` count in the selected tail windows.",
            "",
            "| Scenario | Iteration | Renderer | Samples/s | Total avg | Window | Scripting | Test | UI tick | Camera/test | Config snapshot | Scene update | Scene snapshot | UI composition | UI draw packet | Other |",
            "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for run in summary["runs"]:
        prepare_profile = run.get("prepare_profile", {})
        renderer = (
            prepare_profile.get("renderer", "-")
            if isinstance(prepare_profile, dict)
            else "-"
        )
        lines.append(
            f"| {run['scenario']} | {run['iteration']} | {renderer} | "
            f"{profile_number(run, 'prepare_profile', 'samples_per_second'):.1f} | "
            f"{profile_number(run, 'prepare_profile', 'total_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'window_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'scripting_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'test_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'ui_tick_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'camera_and_test_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'config_snapshot_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'scene_update_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'scene_snapshot_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'ui_composition_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'ui_draw_packet_avg_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'other_avg_ms'):.3f} ms |"
        )
    lines.extend(
        [
            "",
            "| Scenario | Iteration | Total max | Scene update max | Scene snapshot max | UI draw max | Scene ready | Scene dirty | Initial GPU | Update GPU | Geometry snapshot | Snapshot build | UI vertices avg/max | UI indices avg | UI commands avg |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for run in summary["runs"]:
        lines.append(
            f"| {run['scenario']} | {run['iteration']} | "
            f"{profile_number(run, 'prepare_profile', 'total_max_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'scene_update_max_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'scene_snapshot_max_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'ui_draw_packet_max_ms'):.3f} ms | "
            f"{profile_number(run, 'prepare_profile', 'scene_ready_frames'):.0f} | "
            f"{profile_number(run, 'prepare_profile', 'scene_dirty_frames'):.0f} | "
            f"{profile_number(run, 'prepare_profile', 'initial_gpu_update_frames'):.0f} | "
            f"{profile_number(run, 'prepare_profile', 'update_gpu_update_frames'):.0f} | "
            f"{profile_number(run, 'prepare_profile', 'geometry_snapshot_frames'):.0f} | "
            f"{profile_number(run, 'prepare_profile', 'scene_snapshot_build_frames'):.0f} | "
            f"{profile_number(run, 'prepare_profile', 'ui_vertices_avg'):.1f}/"
            f"{profile_number(run, 'prepare_profile', 'ui_vertices_max'):.0f} | "
            f"{profile_number(run, 'prepare_profile', 'ui_indices_avg'):.1f} | "
            f"{profile_number(run, 'prepare_profile', 'ui_commands_avg'):.1f} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")

def parse_args(repo_root: Path) -> argparse.Namespace:
    default_exe = repo_root / "target" / "bin" / "Debug" / "MoerEditor.exe"
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    parser = argparse.ArgumentParser(
        description="Run repeatable MoerEngine renderer threading validation matrices."
    )
    parser.add_argument("--exe", type=Path, default=default_exe)
    parser.add_argument("--workdir", type=Path)
    parser.add_argument(
        "--base-config", type=Path, default=repo_root / "template.MoerEngine.toml"
    )
    parser.add_argument(
        "--outdir",
        type=Path,
        default=repo_root / "target" / "validation" / "rt_rhi" / timestamp,
    )
    parser.add_argument("--set", choices=tuple(SCENARIO_SETS), default="smoke")
    parser.add_argument("--scenario", action="append", choices=tuple(SCENARIOS))
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--soak-seconds", type=float)
    parser.add_argument("--startup-timeout", type=float, default=90.0)
    parser.add_argument("--ready-timeout", type=float, default=180.0)
    parser.add_argument("--ready-settle-seconds", type=float, default=12.0)
    parser.add_argument("--close-timeout", type=float, default=45.0)
    parser.add_argument("--ready-log-pattern", default="Copied ImGui frame includes")
    parser.add_argument("--min-nonblack-ratio", type=float, default=0.01)
    parser.add_argument("--profile-tail-windows", type=int, default=5)
    parser.add_argument("--skip-window-stress", action="store_true")
    parser.add_argument("--detach-configs", action="store_true")
    parser.add_argument("--continue-on-failure", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be at least 1")
    if args.soak_seconds is None:
        args.soak_seconds = 300.0 if args.set == "soak" else 0.0
    if args.soak_seconds < 0.0:
        parser.error("--soak-seconds cannot be negative")
    if args.profile_tail_windows < 1:
        parser.error("--profile-tail-windows must be at least 1")
    return args

def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    verifier = Path(__file__).resolve().with_name("runtime_verify.py")
    args = parse_args(repo_root)
    if args.list:
        for name, scenario in SCENARIOS.items():
            print(
                f"{name}: renderer={scenario.renderer}, rt={scenario.render_thread}, "
                f"lag={scenario.max_frame_lag}, execution=compiled, "
                f"graph_dump={scenario.render_graph_debug_dump}, "
                f"graph_parallel_record={scenario.render_graph_parallel_recording}, "
                f"stress={scenario.window_stress}"
            )
        return 0

    exe = args.exe.resolve()
    workdir = args.workdir.resolve() if args.workdir else exe.parent
    base_config = args.base_config.resolve()
    outdir = args.outdir.resolve()
    if not verifier.is_file():
        print(f"Verifier does not exist: {verifier}", file=sys.stderr)
        return 2
    if not args.dry_run and not exe.is_file():
        print(f"MoerEditor executable does not exist: {exe}", file=sys.stderr)
        return 2
    if not base_config.is_file():
        print(f"Base config does not exist: {base_config}", file=sys.stderr)
        return 2

    selected_names = args.scenario or list(SCENARIO_SETS[args.set])
    try:
        base_text = base_config.read_text(encoding="utf-8")
        generated_configs = {
            scenario_name: generate_config(base_text, SCENARIOS[scenario_name])
            for scenario_name in dict.fromkeys(selected_names)
        }
    except (OSError, KeyError, TypeError, ValueError) as error:
        print(f"Failed to generate matrix configs: {error}", file=sys.stderr)
        return 2

    planned_run_dirs = []
    for iteration in range(1, args.repeat + 1):
        for scenario_name in selected_names:
            run_name = (
                scenario_name if args.repeat == 1 else f"{scenario_name}_{iteration:03d}"
            )
            planned_run_dirs.append(outdir / run_name)
    nonempty_run_dirs = [
        path for path in planned_run_dirs if path.is_dir() and any(path.iterdir())
    ]
    if nonempty_run_dirs:
        print(
            "Refusing to reuse non-empty run directories:\n  "
            + "\n  ".join(str(path) for path in nonempty_run_dirs),
            file=sys.stderr,
        )
        return 2

    outdir.mkdir(parents=True, exist_ok=True)

    started_at = datetime.now().astimezone().isoformat()
    started = time.monotonic()
    runs: list[dict[str, object]] = []
    stop = False
    for iteration in range(1, args.repeat + 1):
        for scenario_name in selected_names:
            scenario = SCENARIOS[scenario_name]
            run_name = scenario.name if args.repeat == 1 else f"{scenario.name}_{iteration:03d}"
            run_dir = outdir / run_name
            run_dir.mkdir(parents=True, exist_ok=True)
            config_path = run_dir / "MoerEngine.toml"
            config_path.write_text(generated_configs[scenario_name], encoding="utf-8")
            command = build_verifier_command(
                verifier,
                exe,
                workdir,
                run_dir,
                config_path,
                scenario,
                args,
            )
            print(f"[{iteration}/{args.repeat}] {scenario.name}")
            print("  " + subprocess.list2cmdline(command))

            if args.dry_run:
                runs.append(
                    {
                        "scenario": scenario.name,
                        "iteration": iteration,
                        "success": True,
                        "dry_run": True,
                        "command": command,
                    }
                )
                continue

            completed = subprocess.run(
                command, capture_output=True, text=True,
                env=os.environ.copy(),
            )
            report_path = run_dir / "report.json"
            report = (
                json.loads(report_path.read_text(encoding="utf-8"))
                if report_path.is_file()
                else {}
            )
            captures = report.get("captures", [])
            profile_windows = parse_profile_windows(run_dir / "stdout.log")
            rt_profile = aggregate_profile_windows(
                profile_windows["RT"], "RT", args.profile_tail_windows
            )
            prepare_profile = aggregate_profile_windows(
                profile_windows["Prepare"], "Prepare", args.profile_tail_windows
            )
            min_nonblack = (
                min(capture.get("nonblack_ratio", 0.0) for capture in captures)
                if captures
                else None
            )
            success = completed.returncode == 0 and bool(report.get("success"))
            failure_reasons = list(report.get("failure_reasons", []))
            run = {
                "scenario": scenario.name,
                "iteration": iteration,
                "success": success,
                "verifier_exit_code": completed.returncode,
                "exit_code": report.get("exit_code"),
                "closed_normally": report.get("closed_normally", False),
                "capture_count": len(captures),
                "captures_expected": not scenario.expect_exit_after_ready,
                "min_nonblack_ratio": min_nonblack,
                "duration_seconds": report.get("duration_seconds", 0.0),
                "failure_reasons": failure_reasons,
                "rt_profile": rt_profile,
                "prepare_profile": prepare_profile,
                "report": str(report_path),
            }
            runs.append(run)
            visual_result = (
                "captures=skipped (lifecycle-only)"
                if scenario.expect_exit_after_ready
                else f"captures={run['capture_count']}, min_nonblack={min_nonblack or 0.0:.4f}"
            )
            print(
                f"  {'PASS' if success else 'FAIL'}: exit={run['exit_code']}, "
                f"{visual_result}"
            )
            if not success:
                if completed.stdout:
                    print(completed.stdout, file=sys.stderr)
                if completed.stderr:
                    print(completed.stderr, file=sys.stderr)
                if not args.continue_on_failure:
                    stop = True
                    break
        if stop:
            break

    summary = {
        "started_at": started_at,
        "duration_seconds": time.monotonic() - started,
        "matrix_set": args.set,
        "selected_scenarios": selected_names,
        "repeat": args.repeat,
        "soak_seconds": args.soak_seconds,
        "profile_tail_windows": args.profile_tail_windows,
        "dry_run": args.dry_run,
        "success": all(run["success"] for run in runs)
        and len(runs) == len(selected_names) * args.repeat,
        "runs": runs,
    }
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    write_markdown_summary(outdir / "summary.md", summary)
    print(f"Summary: {outdir / 'summary.md'}")
    return 0 if summary["success"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
