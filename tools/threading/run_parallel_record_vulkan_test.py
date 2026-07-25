from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Sequence


SUMMARY_RE = re.compile(
    r"\[ParallelRecord\] batch=(?P<batch>\d+).*?effective=(?P<effective>true|false) "
    r"outcome=(?P<outcome>\S+).*?work_units=(?P<work_units>\d+) "
    r"ordered_cb=(?P<ordered_cb>\d+) "
    r"waves=(?P<waves>\d+) islands=(?P<islands>\d+).*?"
    r"distinct_workers=(?P<workers>\d+) max_active=(?P<max_active>\d+)"
)
INJECTION_RE = re.compile(
    r"\[ParallelRecord\]\[Injection\].*?point=worker-throw "
    r"phase=after-first-command.*?batch=(?P<batch>\d+)"
)


class VulkanTestError(RuntimeError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise VulkanTestError(message)


def _testcase_marker(name: str, text: str) -> tuple[str, dict[str, str]] | None:
    matches = list(
        re.finditer(
            rf"\[TESTCASE\]\[(?P<status>PASS|SKIP)\]"
            rf"[^\r\n]*\bname={re.escape(name)}(?:\s|$)[^\r\n]*",
            text,
        )
    )
    _require(
        len(matches) <= 1,
        f"{name}: expected at most one terminal marker, got {len(matches)}",
    )
    if not matches:
        return None
    marker = matches[0]
    field_pairs = re.findall(
        r"(?:^|\s)([A-Za-z0-9_]+)=([^\s]+)", marker.group(0)
    )
    seen_fields: set[str] = set()
    duplicate_fields: set[str] = set()
    for key, _ in field_pairs:
        if key in seen_fields:
            duplicate_fields.add(key)
        seen_fields.add(key)
    _require(
        not duplicate_fields,
        f"{name}: duplicate terminal marker fields: "
        + ", ".join(sorted(duplicate_fields)),
    )
    fields = dict(field_pairs)
    return marker.group("status"), fields


def _vulkan_fault_summary(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"\[VulkanFault\]\[Summary\][^\r\n]*", text))
    _require(
        len(matches) == 1,
        f"VulkanFault Summary: expected exactly one marker, got {len(matches)}",
    )
    field_pairs = re.findall(
        r"(?:^|\s)([A-Za-z0-9_]+)=([^\s]+)", matches[0].group(0)
    )
    seen_fields: set[str] = set()
    duplicate_fields: set[str] = set()
    for key, _ in field_pairs:
        if key in seen_fields:
            duplicate_fields.add(key)
        seen_fields.add(key)
    _require(
        not duplicate_fields,
        "VulkanFault Summary: duplicate fields: "
        + ", ".join(sorted(duplicate_fields)),
    )
    return dict(field_pairs)


def _require_clean_hard_fault_summary(mode: str, text: str) -> None:
    fields = _vulkan_fault_summary(text)
    _require(
        fields.get("first_fault_count") == "1"
        and fields.get("native_submit_after_fault") == "0"
        and fields.get("native_present_after_fault") == "0"
        and fields.get("device_lost") == "false",
        f"{mode}: incomplete clean hard-fault ownership summary",
    )


def _require_phase15b_copy_contracts(mode: str, text: str) -> None:
    round_trip = _testcase_marker("ActiveRdgGraphicsCopyRoundTrip", text)
    _require(
        round_trip is not None,
        f"{mode}: missing active RDG Graphics-Copy round-trip marker",
    )
    round_trip_status, round_trip_fields = round_trip
    if round_trip_status == "PASS":
        native_alias = round_trip_fields.get("native_alias")
        expected_gpu_syncs = "0" if native_alias == "true" else "2"
        expected_mode = "serial" if mode == "serial" else "parallel"
        graphics_native = round_trip_fields.get("graphics_native")
        copy_native = round_trip_fields.get("copy_native")
        _require(
            native_alias in {"true", "false"}
            and round_trip_fields.get("mode") == expected_mode
            and round_trip_fields.get("ownership")
            in {"local-acquire", "release-acquire"}
            and round_trip_fields.get("transfers") == "2"
            and round_trip_fields.get("gpu_syncs") == expected_gpu_syncs
            and graphics_native is not None
            and copy_native is not None
            and ((graphics_native == copy_native) == (native_alias == "true"))
            and round_trip_fields.get("readback") == "verified",
            f"{mode}: incomplete active RDG Graphics-Copy round-trip contract",
        )
    else:
        _require(
            round_trip_fields.get("reason") == "copy_queue_unavailable",
            f"{mode}: invalid active RDG Graphics-Copy round-trip SKIP contract",
        )

    recoverable_copy = _testcase_marker(
        "RecoverableCopyDependencyRejection", text
    )
    _require(
        recoverable_copy is not None and recoverable_copy[0] == "PASS",
        f"{mode}: missing recoverable Copy rejection marker",
    )
    _, recoverable_copy_fields = recoverable_copy
    _require(
        recoverable_copy_fields.get("fence") == "reused"
        and recoverable_copy_fields.get("rejected") == "N"
        and recoverable_copy_fields.get("recovered") == "N+1"
        and recoverable_copy_fields.get("stale_wait") == "rejected"
        and recoverable_copy_fields.get("publication") == "Submission"
        and recoverable_copy_fields.get("native_rejected") == "0"
        and recoverable_copy_fields.get("native_accepted") == "3"
        and recoverable_copy_fields.get("native_owner") == "verified"
        and recoverable_copy_fields.get("runtime") == "recovered"
        and recoverable_copy_fields.get("replay") == "0",
        f"{mode}: incomplete recoverable Copy rejection contract",
    )

    cross_queue = _testcase_marker("CrossQueueSubmissionTopology", text)
    _require(
        cross_queue is not None and cross_queue[0] == "PASS",
        f"{mode}: missing cross-queue Submission ownership marker",
    )
    _, cross_queue_fields = cross_queue
    _require(
        cross_queue_fields.get("batches") == "4"
        and cross_queue_fields.get("queues")
        == "Graphics,Compute,Copy,Graphics"
        and cross_queue_fields.get("ownership") == "explicit"
        and cross_queue_fields.get("native_owner") == "verified"
        and cross_queue_fields.get("native_alias") in {"true", "false"}
        and cross_queue_fields.get("callbacks") == "exactly_once"
        and cross_queue_fields.get("replay") == "0",
        f"{mode}: incomplete cross-queue Submission ownership contract",
    )

    direct_copy = _testcase_marker("DirectCopyExecuteRejected", text)
    _require(
        direct_copy is not None and direct_copy[0] == "PASS",
        f"{mode}: missing direct Copy fail-closed marker",
    )
    _, direct_copy_fields = direct_copy
    _require(
        direct_copy_fields.get("runtime_claim") == "exclusive"
        and direct_copy_fields.get("native_submit") == "0"
        and direct_copy_fields.get("callbacks") == "exactly_once"
        and direct_copy_fields.get("signal") == "rejected"
        and direct_copy_fields.get("replay") == "0",
        f"{mode}: incomplete direct Copy fail-closed contract",
    )

    io_copy = _testcase_marker("VulkanStorageUnifiedCopySubmission", text)
    _require(
        io_copy is not None and io_copy[0] == "PASS",
        f"{mode}: missing VulkanStorage unified Copy submission marker",
    )
    _, io_copy_fields = io_copy
    _require(
        io_copy_fields.get("commits") == "2"
        and io_copy_fields.get("signals") == "2"
        and io_copy_fields.get("native_submit") == "2"
        and io_copy_fields.get("native_owner") == "Submission"
        and io_copy_fields.get("session_recycle") == "verified"
        and io_copy_fields.get("readback") == "verified",
        f"{mode}: incomplete VulkanStorage unified Copy submission contract",
    )

    shutdown = _testcase_marker("ShutdownDependencyCancellation", text)
    _require(
        shutdown is not None and shutdown[0] == "PASS",
        f"{mode}: missing shutdown dependency cancellation marker",
    )
    _, shutdown_fields = shutdown
    _require(
        shutdown_fields.get("queues") == "Copy,Graphics"
        and shutdown_fields.get("copy_wait") == "entered"
        and shutdown_fields.get("dependency") == "unpublished"
        and shutdown_fields.get("native_submit") == "0"
        and shutdown_fields.get("sync_wait") == "registered"
        and shutdown_fields.get("concurrent_sync") == "drained",
        f"{mode}: incomplete shutdown dependency cancellation contract",
    )


def _require_phase15c_multi_segment_contracts(mode: str, text: str) -> None:
    execution = _testcase_marker("MultiSegmentSourceExecution", text)
    _require(
        execution is not None,
        f"{mode}: missing Phase15C multi-segment source execution marker",
    )
    recovery = _testcase_marker("MultiSegmentRecoverableRejection", text)
    _require(
        recovery is not None,
        f"{mode}: missing Phase15C multi-segment recoverable rejection marker",
    )
    copy_round_trip = _testcase_marker("MultiSegmentCopyRoundTrip", text)
    _require(
        copy_round_trip is not None,
        f"{mode}: missing Phase15C multi-segment Copy round-trip marker",
    )

    copy_status, copy_fields = copy_round_trip
    if copy_status == "SKIP":
        _require(
            copy_fields.get("reason") == "copy_queue_unavailable"
            and copy_fields.get("graphics_native") is not None
            and copy_fields.get("copy_native") is not None,
            f"{mode}: invalid Phase15C multi-segment Copy SKIP contract",
        )
    else:
        copy_native_alias = copy_fields.get("native_alias")
        _require(
            copy_native_alias in {"true", "false"},
            f"{mode}: Phase15C multi-segment Copy source omitted native alias state",
        )
        expected_copy_predecessor_waits = (
            "0" if copy_native_alias == "true" else "2"
        )
        _require(
            copy_fields.get("source") == "1"
            and copy_fields.get("segments") == "3"
            and copy_fields.get("queues") == "Graphics,Copy,Graphics"
            and copy_fields.get("observed_submit_order") == "G,Copy,G"
            and copy_fields.get("source_segments") == "0:0/3,0:1/3,0:2/3"
            and copy_fields.get("predecessor_waits")
            == expected_copy_predecessor_waits
            and copy_fields.get("native_owner") == "Submission"
            and copy_fields.get("callbacks") == "exactly_once"
            and copy_fields.get("signal") == "success"
            and copy_fields.get("readback") == "verified"
            and copy_fields.get("replay") == "0",
            f"{mode}: incomplete Phase15C multi-segment Copy round-trip contract",
        )

    execution_status, execution_fields = execution
    recovery_status, recovery_fields = recovery
    _require(
        execution_status == recovery_status,
        f"{mode}: Phase15C multi-segment execution/recovery status mismatch",
    )
    if execution_status == "SKIP":
        _require(
            execution_fields.get("reason") == "queue_unavailable"
            and execution_fields.get("graphics_native") is not None
            and execution_fields.get("compute_native") is not None
            and recovery_fields.get("reason") == "queue_unavailable"
            and recovery_fields.get("graphics_native") is not None
            and recovery_fields.get("compute_native") is not None,
            f"{mode}: invalid Phase15C multi-segment SKIP contract",
        )
        return

    native_alias = execution_fields.get("native_alias")
    _require(
        native_alias in {"true", "false"},
        f"{mode}: Phase15C multi-segment source omitted native alias state",
    )
    expected_first_ready = "G" if native_alias == "true" else "G,C"
    expected_predecessor_waits = "0" if native_alias == "true" else "1"
    expected_compute_saw_graphics_finished = (
        "true" if native_alias == "true" else "false"
    )
    _require(
        execution_fields.get("source") == "1"
        and execution_fields.get("segments") == "2"
        and execution_fields.get("queues") == "Graphics,Compute"
        and execution_fields.get("first_ready_lanes") == expected_first_ready
        and execution_fields.get("observed_submit_order") == "G,C"
        and execution_fields.get("source_segments") == "0:0/2,0:1/2"
        and execution_fields.get("predecessor_waits")
        == expected_predecessor_waits
        and execution_fields.get("compute_saw_graphics_finished")
        == expected_compute_saw_graphics_finished
        and execution_fields.get("native_owner") == "Submission"
        and execution_fields.get("callbacks") == "exactly_once"
        and execution_fields.get("signal") == "success"
        and execution_fields.get("replay") == "0",
        f"{mode}: incomplete Phase15C multi-segment source execution contract",
    )

    distinct_native = recovery_fields.get("distinct_native")
    _require(
        recovery_fields.get("source") == "1"
        and recovery_fields.get("segments") == "2"
        and recovery_fields.get("dependency") == "rejected"
        and distinct_native in {"true", "false"}
        and distinct_native == recovery_fields.get("suffix_recorded")
        and (distinct_native == "true") == (native_alias == "false")
        and recovery_fields.get("native_rejected") == "0"
        and recovery_fields.get("callbacks") == "ordinary1_success0"
        and recovery_fields.get("signal") == "rejected"
        and recovery_fields.get("native_accepted") == "1"
        and recovery_fields.get("recovered_callbacks")
        == "ordinary1_success1"
        and recovery_fields.get("runtime") == "recovered"
        and recovery_fields.get("replay") == "0",
        f"{mode}: incomplete Phase15C multi-segment recoverable rejection contract",
    )


def _require_phase15c_completion_aggregate_cpu_probe(
    mode: str, text: str
) -> None:
    probe = _testcase_marker("MultiSegmentCompletionAggregateCpuProbe", text)
    _require(
        probe is not None and probe[0] == "PASS",
        f"{mode}: missing multi-segment completion aggregate CPU probe marker",
    )
    _, fields = probe
    _require(
        fields.get("suffix_first") == "deferred"
        and fields.get("prefix_second") == "ordinary1_success0"
        and fields.get("replay") == "0",
        f"{mode}: incomplete multi-segment completion aggregate CPU probe contract",
    )


def validate_log(mode: str, text: str) -> None:
    _require("[TESTCASE][FAIL]" not in text, f"{mode}: test emitted a FAIL marker")
    _require("VUID-" not in text, f"{mode}: Vulkan validation VUID was emitted")
    _require("Validation Error" not in text, f"{mode}: Vulkan validation error was emitted")
    _require_phase15c_completion_aggregate_cpu_probe(mode, text)
    if mode == "translate-hard":
        retirement = _testcase_marker(
            "ParallelTranslateFailureRetirement", text
        )
        _require(
            retirement is not None and retirement[0] == "PASS",
            "translate-hard: missing failure-retirement PASS marker",
        )
        _, retirement_fields = retirement
        _require(
            retirement_fields.get("distinct_native")
            == retirement_fields.get("suffix_recorded")
            and retirement_fields.get("distinct_native") in {"true", "false"}
            and retirement_fields.get("signals") == "failed"
            and retirement_fields.get("callbacks") == "exactly_once"
            and retirement_fields.get("hard_latch") == "later_batch_failed",
            "translate-hard: incomplete failure-retirement contract",
        )
        _require_clean_hard_fault_summary(mode, text)
        return

    if mode == "multi-segment-hard":
        retirement = _testcase_marker(
            "MultiSegmentPrefixSubmitSuffixTranslateFailure", text
        )
        _require(
            retirement is not None and retirement[0] == "PASS",
            "multi-segment-hard: missing prefix-submit/suffix-failure PASS marker",
        )
        _, retirement_fields = retirement
        _require(
            retirement_fields.get("source") == "1"
            and retirement_fields.get("segments") == "2"
            and retirement_fields.get("queues") == "Graphics,Graphics"
            and retirement_fields.get("prefix_translated") == "true"
            and retirement_fields.get("source_submitted") == "0:0/2"
            and retirement_fields.get("native_accepted_prefix") == "1"
            and retirement_fields.get("native_owner") == "Submission"
            and retirement_fields.get("callbacks") == "ordinary1_success0"
            and retirement_fields.get("signal") == "failed"
            and retirement_fields.get("later_callbacks")
            == "ordinary1_success0"
            and retirement_fields.get("hard_latch") == "later_batch_failed"
            and retirement_fields.get("replay") == "0",
            "multi-segment-hard: incomplete aggregate retirement contract",
        )
        _require_clean_hard_fault_summary(mode, text)
        return

    ready_marker = _testcase_marker("AsyncQueueParallelTranslateSmoke", text)
    _require(
        ready_marker is not None,
        f"{mode}: missing ready-native-lane Translate smoke marker",
    )
    ready_status, ready_fields = ready_marker
    if ready_status == "PASS":
        _require(
            ready_fields.get("sources") == "3"
            and ready_fields.get("batch") == "1"
            and ready_fields.get("source_order") == "G,G,C"
            and ready_fields.get("first_ready_lanes") == "G,C"
            and ready_fields.get("observed_submit_order") == "G,G,C"
            and ready_fields.get("explicit_state") == "true"
            and ready_fields.get("callbacks") == "exactly_once",
            f"{mode}: incomplete ready-native-lane Translate PASS contract",
        )
    else:
        graphics_native = ready_fields.get("graphics_native")
        compute_native = ready_fields.get("compute_native")
        _require(
            ready_fields.get("reason")
            in {"queue_unavailable", "native_queue_alias"}
            and ready_fields.get("cpu_seam")
            == "VulkanTranslateWaveScheduler"
            and graphics_native is not None
            and compute_native is not None
            and (
                ready_fields.get("reason") != "native_queue_alias"
                or graphics_native == compute_native
            ),
            f"{mode}: invalid ready-native-lane Translate SKIP contract",
        )

    recoverable_marker = _testcase_marker(
        "ParallelTranslateRecoverableRejection", text
    )
    _require(
        recoverable_marker is not None
        and recoverable_marker[0] == "PASS",
        f"{mode}: missing recoverable Translate retirement marker",
    )
    _, recoverable_fields = recoverable_marker
    _require(
        recoverable_fields.get("distinct_native")
        == recoverable_fields.get("suffix_recorded")
        and recoverable_fields.get("distinct_native") in {"true", "false"}
        and recoverable_fields.get("signals") == "rejected"
        and recoverable_fields.get("callbacks") == "exactly_once"
        and recoverable_fields.get("runtime") == "recovered"
        and recoverable_fields.get("same_scope_reentry") == "Compute",
        f"{mode}: incomplete recoverable Translate retirement contract",
    )

    _require_phase15c_multi_segment_contracts(mode, text)
    _require_phase15b_copy_contracts(mode, text)

    expected_fault = "true" if mode == "fallback" else "false"
    expected_mode = "serial" if mode == "serial" else "parallel"
    expected_production_gate = "true" if mode in ("gated", "heavy") else "false"
    expected_production_heavy = "true" if mode == "heavy" else "false"
    _require(
        re.search(
            rf"\[TESTCASE\]\[PASS\].*mode={expected_mode}.*worker_fault={expected_fault}"
            rf".*production_gate={expected_production_gate}"
            rf".*production_heavy={expected_production_heavy}",
            text,
        )
        is not None,
        f"{mode}: missing matching PASS marker",
    )

    summaries = [match.groupdict() for match in SUMMARY_RE.finditer(text)]
    if mode == "serial":
        _require(not summaries, "serial: parallel backend summary was unexpectedly emitted")
        _require("[ParallelRecord][Injection]" not in text, "serial: fault injection ran")
        return
    if mode == "gated":
        _require(not summaries, "gated: cheap workload unexpectedly recorded in parallel")
        _require(
            "reason=insufficient-work" in text,
            "gated: production threshold did not report insufficient work",
        )
        _require("[ParallelRecord][Wave]" not in text, "gated: worker wave was dispatched")
        _require("[ParallelRecord][Injection]" not in text, "gated: fault injection ran")
        return

    _require(summaries, f"{mode}: no parallel backend summary was emitted")
    _require(
        any(
            item["outcome"] == "parallel"
            and int(item["work_units"]) > 0
            and int(item["ordered_cb"]) > 1
            and int(item["waves"]) >= 2
            and int(item["islands"]) >= 1
            and int(item["workers"]) >= 2
            and int(item["max_active"]) >= 2
            for item in summaries
        ),
        f"{mode}: no effective concurrent multi-CB batch with two waves and a serial island",
    )
    if mode == "heavy":
        _require(
            any(int(item["work_units"]) >= 128 for item in summaries),
            "heavy: production floor did not admit two threshold-sized jobs",
        )

    lines = text.splitlines()
    first_parallel_batch = next(
        item["batch"] for item in summaries if item["outcome"] == "parallel"
    )
    wave_positions = [
        index
        for index, line in enumerate(lines)
        if f"[ParallelRecord][Wave] batch={first_parallel_batch} " in line
    ]
    island_positions = [
        index
        for index, line in enumerate(lines)
        if f"[ParallelRecord][Island] batch={first_parallel_batch} " in line
    ]
    _require(
        any(
            any(wave < island for wave in wave_positions)
            and any(wave > island for wave in wave_positions)
            for island in island_positions
        ),
        f"{mode}: logs do not prove joined wave -> serial island -> later wave order",
    )

    injection_batches = [match.group("batch") for match in INJECTION_RE.finditer(text)]
    fallback_batches = [
        item["batch"]
        for item in summaries
        if item["outcome"] == "serial-fallback-worker-failure"
    ]
    if mode in ("parallel", "heavy"):
        _require(not injection_batches, "parallel: unexpected worker fault injection")
        _require(not fallback_batches, "parallel: unexpected worker fallback")
    else:
        _require(
            len(injection_batches) == 1,
            f"fallback: expected one post-command injection, got {len(injection_batches)}",
        )
        _require(
            len(fallback_batches) == 1,
            f"fallback: expected one fallback batch, got {len(fallback_batches)}",
        )
        fault_batch = fallback_batches[0]
        _require(
            injection_batches[0] == fault_batch,
            "fallback: injection and coordinator fallback belong to different batches",
        )
        _require(
            re.search(
                rf"\[ParallelRecord\]\[Wave\] batch={fault_batch} .*?worker_recorded=false",
                text,
            )
            is not None,
            "fallback: injected batch has no failed worker wave",
        )


def run_case(executable: Path, outdir: Path, mode: str, timeout: float) -> Path:
    arguments: list[str] = []
    if mode in ("parallel", "fallback", "gated", "heavy"):
        arguments.append("--parallel")
    if mode == "fallback":
        arguments.append("--inject-worker-failure")
    if mode == "gated":
        arguments.append("--production-gate")
    if mode == "heavy":
        arguments.append("--production-heavy")
    if mode == "translate-hard":
        arguments.append("--inject-translate-failure")
    if mode == "multi-segment-hard":
        arguments.append("--inject-multi-segment-translate-failure")

    completed = subprocess.run(
        [str(executable), *arguments],
        cwd=executable.parent,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )
    text = completed.stdout + completed.stderr
    outdir.mkdir(parents=True, exist_ok=True)
    log_path = outdir / f"{mode}.log"
    log_path.write_text(text, encoding="utf-8")
    _require(completed.returncode == 0, f"{mode}: executable returned {completed.returncode}")
    validate_log(mode, text)
    return log_path


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run and gate the Vulkan parallel-record serial/parallel/fallback matrix."
    )
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--outdir", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args(argv)
    if args.timeout <= 0.0:
        parser.error("--timeout must be positive")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    executable = args.executable.resolve()
    if not executable.is_file():
        print(f"parallel Vulkan test: executable not found: {executable}", file=sys.stderr)
        return 2
    try:
        logs = [
            run_case(executable, args.outdir, mode, args.timeout)
            for mode in (
                "serial",
                "parallel",
                "fallback",
                "gated",
                "heavy",
                "translate-hard",
                "multi-segment-hard",
            )
        ]
    except (OSError, subprocess.TimeoutExpired, VulkanTestError) as error:
        print(f"parallel Vulkan test: FAIL: {error}", file=sys.stderr)
        return 1
    print("parallel Vulkan test: PASS: " + ", ".join(str(path) for path in logs))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
