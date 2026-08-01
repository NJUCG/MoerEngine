from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

try:
    from . import run_parallel_record_vulkan_test as runner
except ImportError:
    import run_parallel_record_vulkan_test as runner


def completion_probe_line() -> str:
    return (
        "[TESTCASE][PASS] name=MultiSegmentCompletionAggregateCpuProbe "
        "suffix_first=deferred prefix_second=ordinary1_success0 replay=0\n"
    )


def pipeline_line(
    window: int,
    native_alias: str,
    overlap_assertion: str,
    include_overlap_marker: bool | None = None,
    include_recoverable_rejection: bool = True,
    include_recoverable_overlap_marker: bool | None = None,
    include_shutdown_cancellation: bool = True,
    include_shutdown_overlap_marker: bool | None = None,
) -> str:
    alias_window2 = window == 2 and native_alias == "true"
    emit_overlap_marker = (
        alias_window2
        if include_overlap_marker is None
        else include_overlap_marker
    )
    overlap = ""
    if emit_overlap_marker:
        overlap = (
            "[TESTCASE][PASS] "
            "name=BoundedCrossBatchSubmissionPipelineOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=7 "
            "compute_native=11 copy_native=7 admission=blocked\n"
        )
    recoverable_rejection = ""
    if window == 2 and include_recoverable_rejection:
        recoverable_rejection = (
            "[TESTCASE][PASS] "
            "name=BoundedCrossBatchRecoverableRejection "
            f"window=2 native_alias={native_alias} "
            f"overlap_assertion={overlap_assertion} batches=2 "
            "queues=Graphics,Copy "
            "batch0_native_submit=0 "
            "batch0_callbacks=ordinary1_success0 "
            "batch0_signal=rejected batch1_native_submit=1 "
            "batch1_callbacks=ordinary1_success1 "
            "batch1_signal=success copy_translate_owner=Translate "
            "native_owner=Submission readback=verified replay=0\n"
        )
    emit_recoverable_overlap_marker = (
        alias_window2
        if include_recoverable_overlap_marker is None
        else include_recoverable_overlap_marker
    )
    recoverable_overlap = ""
    if emit_recoverable_overlap_marker:
        recoverable_overlap = (
            "[TESTCASE][PASS] "
            "name=BoundedCrossBatchRecoverableRejectionOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=7 "
            "compute_native=11 copy_native=7 admission=blocked "
            "cpu_seam=RHISubmissionPipelinePolicy\n"
        )
    emit_shutdown_overlap_marker = (
        alias_window2
        if include_shutdown_overlap_marker is None
        else include_shutdown_overlap_marker
    )
    shutdown_overlap = ""
    if emit_shutdown_overlap_marker:
        shutdown_overlap = (
            "[TESTCASE][PASS] "
            "name=BoundedPipelineShutdownCancellationOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=7 "
            "compute_native=11 copy_native=7 admission=blocked "
            "cpu_seam=RHISubmissionPipelinePolicy\n"
        )
    malformed_ordering = ""
    if window == 2:
        malformed_ordering = (
            "[TESTCASE][PASS] "
            "name=BoundedCrossBatchMalformedPreflightOrdering "
            "window=2 batches=2 queue=Graphics prefix=committed "
            "malformed=rejected callback_order=prefix,malformed "
            "signals=success,rejected native_owner=Submission replay=0 "
            "malformed_translate=0 runtime=restarted\n"
        )
    shutdown_cancellation = ""
    if window == 2 and include_shutdown_cancellation:
        shutdown_cancellation = (
            "[TESTCASE][PASS] "
            "name=BoundedPipelineShutdownCancellation "
            f"window=2 native_alias={native_alias} "
            f"overlap_assertion={overlap_assertion} "
            "queues=Graphics,Copy "
            "batch0_native_submit=0 batch1_native_submit=1 "
            "batch0_signal=rejected batch1_signal=success "
            "copy_translate_owner=Translate native_owner=Submission "
            "readback=verified callbacks=exactly_once "
            "concurrent_sync=drained owners=stopped\n"
        )
    return (
        overlap
        + "[TESTCASE][PASS] name=BoundedCrossBatchSubmissionPipeline "
        f"window={window} native_alias={native_alias} "
        f"overlap_assertion={overlap_assertion} batches=2 "
        "queues=Graphics,Compute source_order=G,C "
        "native_owner=Submission signals=success callbacks=exactly_once "
        "replay=0\n"
        + malformed_ordering
        + recoverable_overlap
        + recoverable_rejection
        + shutdown_overlap
        + shutdown_cancellation
    )


def serial_control_boundary_line() -> str:
    return (
        "[TESTCASE][PASS] name=SerialControlPipelineBoundary "
        "order=Prefix,SerialControl,Later owner=Submission "
        "later_translate=blocked readback=verified signals=success "
        "boundary_events=2 native_submits=3 replay=0\n"
    )


def queued_present_shutdown_line() -> str:
    return (
        "[TESTCASE][PASS] name=QueuedPresentShutdownBoundary "
        "outcome=Retry prefix=rejected receipt_attempts=1 "
        "submitted=false recreate=false owner=Submission "
        "native_submit=0 owners=stopped replay=0\n"
    )


def present_source_contract_line() -> str:
    return (
        "[TESTCASE][PASS] name=PresentSourceContractRejection "
        "rejected=17 null=true usage=true transfer_src=true samples=true "
        "format=true compressed=true mip=true layer=true offset=true extent=true "
        "fresh=true accepted_export=true stale_clear=true "
        "backend_tracked_same_batch=true backend_rejected=true "
        "marker_then_clear=true accepted_mutation_clear=true "
        "rejected_mutation_preserves=true accepted_copy_mutation=true "
        "marker_then_segmented_state_change=true "
        "accepted_prefix_rejected_suffix=true copy_commit=verified "
        "wrong_queue_clear=true rejected_export_clear=true "
        "bindless_source_order=true bindless_refcount=true "
        "bindless_rejected_update=true bindless_segments=true "
        "bindless_cross_queue=verified bindless_parallel_record=true "
        "valid_override=4 owner=Submission\n"
    )


def presentation_completion_integration_line() -> str:
    return (
        "[TESTCASE][PASS] name=PresentationCompletionIntegrationBoundary "
        "present_fence=nonblocking_targeted fence_owner=Completion "
        "completion_threads=1 queue_idle_fallback=targeted "
        "queue_idle_owner=Submission outstanding=0\n"
    )


def present_boundary_line(bridge: str = "required") -> str:
    prefix_native = "2" if bridge == "required" else "0"
    return (
        present_source_contract_line()
        + serial_control_boundary_line()
        + "[TESTCASE][PASS] name=PresentPipelineBoundary "
        "outcome=Recreate order=Prefix,Bridge?,Present,Later "
        "owner=Submission receipt_attempts=1 submitted=false "
        "recreate=true completion=drained later_batch=success "
        f"present_only=verified bridge={bridge} graphics_native=0 "
        f"prefix_native={prefix_native} readback=verified replay=0\n"
        + presentation_completion_integration_line()
        + queued_present_shutdown_line()
    )


def present_hard_line() -> str:
    return (
        "[TESTCASE][PASS] name=PresentHardFailureBoundary "
        "outcome=Rejected owner=Submission receipt_attempts=1 "
        "submitted=false recreate=false later_batch=rejected "
        "native_after_present=0 device_fault_priority=true "
        "invalid_source=true concurrent_hard_latch=verified replay=0\n"
    )


def rt_export_rejection_line() -> str:
    return (
        "[TESTCASE][PASS] name=RaytracingExportAcceptanceRejection "
        "attempt1=prefix_rejected attempt1_tail=dependency_rejected "
        "attempt1_latch=false request_pending=true "
        "readback_retry=frame_accepted recovery_consumed=true encoder=once "
        "decision_table=verified native_rejected=0 callbacks=exactly_once "
        "keepalive=terminal replay=0\n"
        "[TESTCASE][PASS] name=RaytracingAcceptedReadbackMaterializationFailure "
        "native=accepted payload=error encoder=0 latch=true\n"
    )


def readback_future_line() -> str:
    return (
        "[TESTCASE][PASS] name=OwningReadbackFuture "
        "queue=Copy buffer=subrange texture=mip1,layer1 "
        "layer_copy=isolated_nonzero_layer "
        "sibling=next_submit_mip0_layer0 state_restore=whole_image "
        "bytes=32,64,64,256 callbacks=exactly_once siblings=terminal "
        "native_staging=completion-owned host_snapshot=future-owned "
        "unsupported=compressed,msaa,partial\n"
    )


def occlusion_query_line() -> str:
    return (
        "[TESTCASE][PASS] name=OcclusionQueryCompletionOwnership "
        "queue=Graphics status=Ready samples=0 visible=false "
        "count_precise=true "
        "availability=explicit pool=allocator_local bulk_pairs=48 "
        "growth=verified reuse=verified "
        "recording=serial_island "
        "order=signal->completion->query->ordinary "
        "callbacks=exactly_once readback=verified replay=0\n"
    )


def timestamp_query_success_batch_line() -> str:
    return (
        "[TESTCASE][PASS] "
        "name=TimestampQuerySuccessfulBatchPublication "
        "sources=2 queues=Graphics,Graphics batch_sequence=7 "
        "signals=terminal_before_query_callbacks "
        "queries=batch_published_before_notify "
        "cross_future_wait=ready owner=Completion "
        "query_callbacks=exactly_once "
        "ordinary_callbacks=exactly_once "
        "success_callbacks=exactly_once "
        "native_submit=2 native_owner=Submission replay=0\n"
        "[TESTCASE][PASS] "
        "name=TimestampQueryCrossQueueBatchPublication "
        "sources=2 queues=Graphics,Compute native_alias=false "
        "queries=batch_published_before_notify "
        "cross_future_wait=ready owner=Completion "
        "queue_local_sync=blocked_until_group_settled "
        "callbacks=exactly_once replay=0\n"
        "[TESTCASE][PASS] "
        "name=TimestampQueryMixedMultiSegmentCallbackTiers "
        "original_sources=2 executable_sources=3 "
        "source0_segments=2 source1_query=true "
        "order=Query,AllOrdinary,AllSuccess "
        "owner=Completion callbacks=exactly_once replay=0\n"
        "[TESTCASE][PASS] "
        "name=TimestampQueryCopyOnlyPayloadArrival "
        "sources=2 queues=Graphics,Copy "
        "copy_payload=query_only copy_query=Error "
        "group_arrival=verified owner=Completion "
        "callbacks=exactly_once native_submit=2 replay=0\n"
        "[TESTCASE][PASS] "
        "name=TimestampQueryCopySupported "
        "queue=Copy status=Ready valid_bits=64 reset_mode=host "
        "copy=verified "
        "gpu_completion=Ready "
        "order=signal->completion->query->ordinary->success "
        "owner=Completion callbacks=exactly_once "
        "allocator_reuse=verified native_owner=Submission replay=0\n"
        "[TESTCASE][PASS] "
        "name=TimestampQueryCopyUnsupportedFallback "
        "queue=Copy injected_valid_bits=0 query=Error copy=verified "
        "signal=success gpu_completion=Ready native_submit=accepted "
        "native_owner=Submission owner=Completion "
        "callbacks=exactly_once runtime=recovered replay=0\n"
    )


def present_legacy_owner_line() -> str:
    return (
        "[TESTCASE][PASS] name=LegacyDirectPresentOwnerBoundary "
        "rhi_thread=false owner=Submission thread=caller "
        "receipt=exactly_once native_present=0\n"
    )


def present_completion_shutdown_line() -> str:
    return (
        "[TESTCASE][PASS] name=PresentationCompletionShutdownDrainBoundary "
        "pending=fence+fallback fence_owner=Completion "
        "queue_idle_owner=Submission outstanding=0 dispose=returned\n"
    )


def timestamp_query_line() -> str:
    return (
        "[TESTCASE][PASS] name=TimestampQueryCompletionOwnership "
        "status=Ready gpu_completion=Ready owner=Completion "
        "order=signal->completion->query->ordinary "
        "allocator_slot_reuse=verified large_query_pairs=501 "
        "post_growth_submit=accepted valid_bits=64 duration_ns=12.5 "
        "reused_duration_ns=3.25 readback=verified replay=0\n"
        + timestamp_query_success_batch_line()
        + "[TESTCASE][PASS] name=TimestampQueryPreparationRejection "
        "async_scope=5784928276370600517 prepare_calls=2 status=Error "
        "suffix_query=Error publish_before_completion=true "
        "signal=rejected-not-failed native_rejected_batch=0 "
        "recovery_submit=accepted owner=Completion callbacks=exactly_once "
        "replay=0\n"
        "[TESTCASE][PASS] name=TimestampQueryPreflightRejection "
        "reason=multi-segment-query sources=2 status=Error owner=Completion "
        "batch_terminal_before_notify=true bounded_cross_future_wait=true "
        "ordinary_callback=exactly_once query_callback=exactly_once "
        "success_callback=0 native_submit=0 replay=0\n"
    )


def timestamp_query_mid_failure_line() -> str:
    return (
        "[TESTCASE][PASS] name=TimestampQueryMidBatchTranslateFailure "
        "queues=Graphics,Graphics native_prefix_submit=1 "
        "suffix_translate=main-thread-released suffix_status=Error "
        "prefix_completion=Ready suffix_completion=Error "
        "pre_terminal_callback_entry=0 batch_terminal_before_notify=true "
        "bounded_cross_future_wait=true owner=Completion "
        "callbacks=exactly_once replay=0\n"
    )


def timestamp_query_record_failure_line() -> str:
    return (
        "[TESTCASE][PASS] name=TimestampQuerySerialRecordFailure "
        "sources=2 failing_source=0 serial_query_island=true "
        "sibling_query=Error sibling_signal=failed "
        "batch_terminal_before_notify=true recorded_phase_gate_count=1 "
        "failed_phase_gate_count=0 pre_release_callback=0 "
        "owner=Completion callbacks=exactly_once native_submit=0 replay=0\n"
    )


def gpu_scope_stream_line() -> str:
    return (
        "[TESTCASE][PASS] name=GpuScopeStreamCompletionAndParallelIsolation "
        "frame_id=7 queue=Graphics scopes=2 hierarchy=nested "
        "query_source=query-serial-island "
        "query_free_sibling=parallel-effective raw_ticks=verified "
        "duration_ns=12.5,3.25 exclusive_ns=9.25,3.25 "
        "owner=Completion readback=verified sibling_readbacks=8/8 replay=0\n"
    )


def pass_line(
    mode: str,
    fault: str,
    production_gate: str = "false",
    production_heavy: str = "false",
) -> str:
    return (
        completion_probe_line()
        + "[TESTCASE][PASS] name=ParallelRecordOrderedReadback "
        f"mode={mode} worker_fault={fault} production_gate={production_gate} "
        f"production_heavy={production_heavy} iterations=24\n"
        "[TESTCASE][PASS] name=AsyncQueueParallelTranslateSmoke "
        "graphics_native=0 compute_native=1 copy_native=2 "
        "sources=4 batch=1 source_order=G,G,C,Copy "
        "first_ready_lanes=G,C,Copy observed_submit_order=G,G,C,Copy "
        "copy_translate_owner=Translate native_owner=Submission "
        "explicit_state=true readback=verified "
        "callbacks=exactly_once replay=0\n"
        "[TESTCASE][PASS] name=ParallelTranslateRecoverableRejection "
        "distinct_native=true suffix_recorded=true signals=rejected "
        "callbacks=exactly_once runtime=recovered same_scope_reentry=Compute\n"
        "[TESTCASE][PASS] name=MultiSegmentSourceExecution "
        "source=1 segments=2 queues=Graphics,Compute native_alias=false "
        "first_ready_lanes=G,C observed_submit_order=G,C "
        "source_segments=0:0/2,0:1/2 predecessor_waits=1 "
        "compute_saw_graphics_finished=false native_owner=Submission "
        "callbacks=exactly_once signal=success replay=0\n"
        "[TESTCASE][PASS] name=MultiSegmentCopyRoundTrip "
        "source=1 segments=3 queues=Graphics,Copy,Graphics native_alias=false "
        "observed_submit_order=G,Copy,G source_segments=0:0/3,0:1/3,0:2/3 "
        "predecessor_waits=2 native_owner=Submission callbacks=exactly_once "
        "signal=success readback=verified replay=0\n"
        "[TESTCASE][PASS] name=MultiSegmentRecoverableRejection "
        "source=1 segments=2 dependency=rejected distinct_native=true "
        "suffix_recorded=true native_rejected=0 "
        "callbacks=ordinary1_success0 signal=rejected native_accepted=1 "
        "recovered_callbacks=ordinary1_success1 runtime=recovered replay=0\n"
        "[TESTCASE][PASS] name=ActiveRdgGraphicsCopyRoundTrip "
        f"mode={mode} ownership=release-acquire transfers=2 gpu_syncs=2 "
        "graphics_native=0 copy_native=2 native_alias=false readback=verified\n"
        "[TESTCASE][PASS] name=RecoverableCopyDependencyRejection "
        "fence=reused rejected=N recovered=N+1 stale_wait=rejected "
        "publication=Submission native_rejected=0 native_accepted=3 "
        "native_owner=verified runtime=recovered replay=0\n"
        "[TESTCASE][PASS] name=CrossQueueSubmissionTopology "
        "batches=4 queues=Graphics,Compute,Copy,Graphics ownership=explicit "
        "native_owner=verified native_alias=false callbacks=exactly_once "
        "replay=0\n"
        "[TESTCASE][PASS] name=DirectCopyExecuteRejected "
        "runtime_claim=exclusive native_submit=0 callbacks=exactly_once "
        "signal=rejected replay=0\n"
        "[TESTCASE][PASS] name=VulkanStorageUnifiedCopySubmission "
        "commits=2 signals=2 native_submit=2 native_owner=Submission "
        "session_recycle=verified readback=verified\n"
        "[TESTCASE][PASS] name=ShutdownDependencyCancellation "
        "queues=Copy,Graphics copy_wait=entered dependency=unpublished "
        "native_submit=0 sync_wait=registered concurrent_sync=drained\n"
    )


def replace_testcase_marker(text: str, name: str, replacement: str) -> str:
    marker = f"name={name} "
    lines = [
        replacement if marker in line else line
        for line in text.splitlines()
    ]
    return "\n".join(lines) + "\n"


def wave(batch: int, index: int) -> str:
    return (
        f"[ParallelRecord][Wave] batch={batch} wave={index} layers=0..2 jobs=2 "
        "join_completed=true worker_recorded=true\n"
    )


def island(batch: int) -> str:
    return (
        f"[ParallelRecord][Island] batch={batch} island=0 first_layer=2 joined_waves=1\n"
    )


def summary(batch: int, outcome: str = "parallel") -> str:
    effective = "true" if outcome == "parallel" else "false"
    return (
        f"[ParallelRecord] batch={batch} requested=true effective={effective} "
        f"outcome={outcome} layers=4 jobs=4 work_units=256 ordered_cb=10 waves=2 islands=1 "
        "workers=4 distinct_workers=3 max_active=3 coordinator=9 submit_status=0\n"
    )


def gpu_env_line(
    *,
    validation_requested: str = "true",
    validation_layer_available: str = "true",
    validation_enabled: str = "true",
    device_type: str = "discrete_gpu",
    vendor_id: str = "0x000010de",
    device_id: str = "0x00002c02",
    device_uuid: str = "3b172c4f63b522546e783cce9b27bd48",
    requested_api: str = "1.3.0",
    device_api: str = "1.3.280",
    device_api_raw: str = "0x00403118",
) -> str:
    return (
        "[Vulkan][GPU_ENV] schema=1 backend=vulkan "
        f"validation_requested={validation_requested} "
        f"validation_layer_available={validation_layer_available} "
        f"validation_enabled={validation_enabled} "
        f"device_type={device_type} vendor_id={vendor_id} device_id={device_id} "
        f"device_uuid={device_uuid} requested_api={requested_api} "
        f"device_api={device_api} api_variant=0 device_api_raw={device_api_raw} "
        "driver_id=4 driver_version_raw=0x12345678\n"
    )


class VulkanRunnerTests(unittest.TestCase):
    def test_gpu_environment_schema_is_accepted(self) -> None:
        environment = runner._gpu_environment(gpu_env_line(), required=True)
        self.assertIsNotNone(environment)
        assert environment is not None
        self.assertEqual(environment.vendor_id, 0x10DE)
        self.assertEqual(environment.device_id, 0x2C02)
        self.assertEqual(
            environment.device_uuid, "3b172c4f63b522546e783cce9b27bd48"
        )
        self.assertEqual(environment.device_type, "discrete_gpu")
        self.assertEqual(environment.device_api, (1, 3, 280))

    def test_gpu_environment_is_required_in_strict_mode(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError, "expected exactly one marker"
        ):
            runner._gpu_environment("", required=True)

    def test_gpu_environment_rejects_duplicate_markers(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError, "expected exactly one marker"
        ):
            runner._gpu_environment(gpu_env_line() * 2, required=True)

    def test_gpu_environment_rejects_unknown_fields(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError, "malformed or unsupported schema"
        ):
            runner._gpu_environment(
                gpu_env_line().replace(
                    " driver_id=4", " unexpected=true driver_id=4"
                ),
                required=True,
            )

    def test_gpu_environment_rejects_inconsistent_raw_api(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError, "do not match device_api_raw"
        ):
            runner._gpu_environment(
                gpu_env_line(device_api_raw="0x00403119"),
                required=True,
            )

    def test_gpu_gate_requires_full_validation_proof(self) -> None:
        environment = runner._gpu_environment(
            gpu_env_line(validation_enabled="false"), required=True
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "validation requested/available/enabled"
        ):
            runner._validate_gpu_environment(
                environment,
                runner.GpuGateRequirements(require_validation=True),
            )

    def test_gpu_gate_rejects_wrong_vendor(self) -> None:
        environment = runner._gpu_environment(gpu_env_line(), required=True)
        with self.assertRaisesRegex(runner.VulkanTestError, "vendor mismatch"):
            runner._validate_gpu_environment(
                environment,
                runner.GpuGateRequirements(vendor_id=0x1002),
            )

    def test_gpu_gate_rejects_device_api_below_minimum(self) -> None:
        environment = runner._gpu_environment(
            gpu_env_line(device_api="1.2.0", device_api_raw="0x00402000"),
            required=True,
        )
        with self.assertRaisesRegex(runner.VulkanTestError, "below required"):
            runner._validate_gpu_environment(
                environment,
                runner.GpuGateRequirements(minimum_device_api=(1, 3, 0)),
            )

    def test_gpu_gate_rejects_requested_api_below_minimum(self) -> None:
        environment = runner._gpu_environment(
            gpu_env_line(requested_api="1.2.0"), required=True
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "requested API is below required"
        ):
            runner._validate_gpu_environment(
                environment,
                runner.GpuGateRequirements(minimum_device_api=(1, 3, 0)),
            )

    def test_gpu_gate_rejects_environment_changes_across_modes(self) -> None:
        first = runner._gpu_environment(gpu_env_line(), required=True)
        second = runner._gpu_environment(
            gpu_env_line(device_id="0x00002704"), required=True
        )
        with self.assertRaisesRegex(runner.VulkanTestError, "changed across"):
            runner._validate_consistent_gpu_environments(
                (
                    runner.CaseResult(Path("first.log"), first),
                    runner.CaseResult(Path("second.log"), second),
                )
            )

    def test_gpu_gate_distinguishes_same_model_devices_by_uuid(self) -> None:
        first = runner._gpu_environment(gpu_env_line(), required=True)
        second = runner._gpu_environment(
            gpu_env_line(device_uuid="00112233445566778899aabbccddeeff"),
            required=True,
        )
        with self.assertRaisesRegex(runner.VulkanTestError, "changed across"):
            runner._validate_consistent_gpu_environments(
                (
                    runner.CaseResult(Path("first.log"), first),
                    runner.CaseResult(Path("second.log"), second),
                )
            )

    def test_parse_args_accepts_strict_gpu_policy(self) -> None:
        args = runner.parse_args(
            (
                "--executable",
                "test.exe",
                "--outdir",
                "logs",
                "--strict-gpu-gate",
                "--require-vendor-id",
                "0x10de",
                "--require-device-type",
                "discrete_gpu",
                "--minimum-device-api",
                "1.3",
            )
        )
        self.assertTrue(args.strict_gpu_gate)
        self.assertEqual(args.require_vendor_id, 0x10DE)
        self.assertEqual(args.minimum_device_api, (1, 3, 0))

    def test_strict_gpu_policy_defaults_to_vulkan_1_3(self) -> None:
        args = runner.parse_args(
            (
                "--executable",
                "test.exe",
                "--outdir",
                "logs",
                "--strict-gpu-gate",
            )
        )
        self.assertEqual(args.minimum_device_api, (1, 3, 0))

    def test_strict_gpu_policy_rejects_lower_explicit_api(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ):
            runner.parse_args(
                (
                    "--executable",
                    "test.exe",
                    "--outdir",
                    "logs",
                    "--strict-gpu-gate",
                    "--minimum-device-api",
                    "1.2",
                )
            )

    def test_timeout_must_be_finite_and_positive(self) -> None:
        for value in ("nan", "inf", "-inf", "0", "-1"):
            with self.subTest(value=value), contextlib.redirect_stderr(
                io.StringIO()
            ), self.assertRaises(SystemExit):
                runner.parse_args(
                    (
                        "--executable",
                        "test.exe",
                        "--outdir",
                        "logs",
                        "--timeout",
                        value,
                    )
                )

    def test_timeout_persists_partial_stdout_and_stderr(self) -> None:
        timeout = subprocess.TimeoutExpired(
            cmd=["test.exe"],
            timeout=1.0,
            output=b"partial stdout\n",
            stderr=b"partial stderr\n",
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            outdir = Path(temporary_directory)
            with mock.patch.object(runner.subprocess, "run", side_effect=timeout):
                with self.assertRaisesRegex(runner.VulkanTestError, "timed out"):
                    runner.run_case(Path("test.exe"), outdir, "serial", 1.0)
            self.assertEqual(
                (outdir / "serial.stdout.log").read_text(encoding="utf-8"),
                "partial stdout\n",
            )
            self.assertEqual(
                (outdir / "serial.stderr.log").read_text(encoding="utf-8"),
                "partial stderr\n",
            )

    def test_strict_gpu_gate_rejects_any_testcase_skip(self) -> None:
        completed = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout=(
                gpu_env_line()
                + pipeline_line(1, "false", "blocked")
                + "[TESTCASE][SKIP] name=Unexpected reason=unsupported\n"
            ),
            stderr="",
        )
        requirements = runner.GpuGateRequirements(
            require_marker=True,
            require_validation=True,
            reject_testcase_skips=True,
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            with mock.patch.object(
                runner.subprocess, "run", return_value=completed
            ):
                with self.assertRaisesRegex(
                    runner.VulkanTestError, "does not permit TESTCASE skips"
                ):
                    runner.run_case(
                        Path("test.exe"),
                        Path(temporary_directory),
                        "pipeline-window1",
                        30.0,
                        requirements,
                    )

    def test_summary_records_stable_gpu_identity(self) -> None:
        environment = runner._gpu_environment(gpu_env_line(), required=True)
        with tempfile.TemporaryDirectory() as temporary_directory:
            outdir = Path(temporary_directory)
            summary_path = runner._write_summary(
                outdir,
                (runner.CaseResult(outdir / "serial.log", environment),),
            )
            summary_text = summary_path.read_text(encoding="utf-8")
            self.assertIn('"mode_count": 1', summary_text)
            self.assertIn('"vendor_id": "0x000010de"', summary_text)
            self.assertIn(
                '"device_uuid": "3b172c4f63b522546e783cce9b27bd48"',
                summary_text,
            )
            self.assertIn('"validation_enabled": true', summary_text)

    def test_summary_records_explicit_tested_sha_over_event_sha(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory, mock.patch.dict(
            os.environ,
            {
                "MOER_GATE_SHA": "tested-merge-sha",
                "GITHUB_SHA": "controller-base-sha",
                "MOER_GATE_PR_NUMBER": "232",
                "MOER_GATE_RUN_URL": "https://example.invalid/run/1",
            },
            clear=False,
        ):
            outdir = Path(temporary_directory)
            summary_path = runner._write_summary(outdir, ())
            payload = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertEqual(payload["commit"], "tested-merge-sha")
            self.assertEqual(payload["pull_request"], "232")
            self.assertEqual(
                payload["workflow_run_url"], "https://example.invalid/run/1"
            )

    def test_runner_refuses_nonempty_output_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            outdir = Path(temporary_directory)
            (outdir / "stale.log").write_text("old", encoding="utf-8")
            with self.assertRaisesRegex(
                runner.VulkanTestError, "refusing to reuse non-empty"
            ):
                runner._prepare_outdir(outdir)

    def test_pipeline_window1_contract_is_accepted(self) -> None:
        runner.validate_log(
            "pipeline-window1",
            pipeline_line(1, "false", "blocked"),
        )

    def test_pipeline_window2_distinct_native_contract_is_accepted(
        self,
    ) -> None:
        runner.validate_log(
            "pipeline-window2",
            pipeline_line(2, "false", "verified"),
        )

    def test_pipeline_window2_alias_fallback_contract_is_accepted(
        self,
    ) -> None:
        runner.validate_log(
            "pipeline-window2",
            pipeline_line(
                2,
                "true",
                "blocked",
                include_overlap_marker=True,
                include_shutdown_overlap_marker=True,
            ),
        )

    def test_pipeline_window1_queue_unavailable_skip_is_accepted(
        self,
    ) -> None:
        text = replace_testcase_marker(
            pipeline_line(1, "false", "blocked"),
            "BoundedCrossBatchSubmissionPipeline",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchSubmissionPipeline "
            "window=1 reason=queue_unavailable "
            "graphics_available=false compute_available=true "
            "graphics_native=0 compute_native=1",
        )
        runner.validate_log("pipeline-window1", text)

    def test_pipeline_window2_compute_unavailable_skip_is_accepted(
        self,
    ) -> None:
        text = replace_testcase_marker(
            pipeline_line(2, "false", "verified"),
            "BoundedCrossBatchSubmissionPipeline",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchSubmissionPipeline "
            "window=2 reason=queue_unavailable "
            "graphics_available=true compute_available=false "
            "graphics_native=0 compute_native=1",
        )
        runner.validate_log("pipeline-window2", text)

    def test_pipeline_window2_copy_unavailable_skips_are_accepted(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified")
        text = replace_testcase_marker(
            text,
            "BoundedCrossBatchRecoverableRejection",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchRecoverableRejection "
            "window=2 reason=queue_unavailable "
            "graphics_available=true copy_available=false "
            "graphics_native=0 copy_native=2",
        )
        text = replace_testcase_marker(
            text,
            "BoundedPipelineShutdownCancellation",
            "[TESTCASE][SKIP] "
            "name=BoundedPipelineShutdownCancellation "
            "window=2 reason=queue_unavailable "
            "graphics_available=true copy_available=false "
            "graphics_native=0 copy_native=2",
        )
        runner.validate_log("pipeline-window2", text)

    def test_pipeline_window2_graphics_unavailable_skips_are_accepted(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified")
        text = replace_testcase_marker(
            text,
            "BoundedCrossBatchSubmissionPipeline",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchSubmissionPipeline "
            "window=2 reason=queue_unavailable "
            "graphics_available=false compute_available=true "
            "graphics_native=0 compute_native=1",
        )
        text = replace_testcase_marker(
            text,
            "BoundedCrossBatchMalformedPreflightOrdering",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchMalformedPreflightOrdering "
            "reason=graphics_queue_unavailable",
        )
        text = replace_testcase_marker(
            text,
            "BoundedCrossBatchRecoverableRejection",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchRecoverableRejection "
            "window=2 reason=queue_unavailable "
            "graphics_available=false copy_available=true "
            "graphics_native=0 copy_native=2",
        )
        text = replace_testcase_marker(
            text,
            "BoundedPipelineShutdownCancellation",
            "[TESTCASE][SKIP] "
            "name=BoundedPipelineShutdownCancellation "
            "window=2 reason=queue_unavailable "
            "graphics_available=false copy_available=true "
            "graphics_native=0 copy_native=2",
        )
        runner.validate_log("pipeline-window2", text)

    def test_pipeline_recoverable_skip_requires_queue_ids(self) -> None:
        text = replace_testcase_marker(
            pipeline_line(2, "false", "verified"),
            "BoundedCrossBatchRecoverableRejection",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchRecoverableRejection "
            "window=2 reason=queue_unavailable "
            "graphics_available=true copy_available=false "
            "graphics_native=0",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid bounded recoverable rejection SKIP contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_pass_rejects_graphics_unavailable_malformed_skip(
        self,
    ) -> None:
        text = replace_testcase_marker(
            pipeline_line(2, "false", "verified"),
            "BoundedCrossBatchMalformedPreflightOrdering",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchMalformedPreflightOrdering "
            "reason=graphics_queue_unavailable",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid malformed-preflight ordering SKIP contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_rejects_inconsistent_graphics_copy_availability(
        self,
    ) -> None:
        text = replace_testcase_marker(
            pipeline_line(2, "false", "verified"),
            "BoundedCrossBatchRecoverableRejection",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchRecoverableRejection "
            "window=2 reason=queue_unavailable "
            "graphics_available=true copy_available=false "
            "graphics_native=0 copy_native=2",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "inconsistent bounded Graphics/Copy availability contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_pass_rejects_graphics_unavailable_copy_skips(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified")
        text = replace_testcase_marker(
            text,
            "BoundedCrossBatchRecoverableRejection",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchRecoverableRejection "
            "window=2 reason=queue_unavailable "
            "graphics_available=false copy_available=true "
            "graphics_native=0 copy_native=2",
        )
        text = replace_testcase_marker(
            text,
            "BoundedPipelineShutdownCancellation",
            "[TESTCASE][SKIP] "
            "name=BoundedPipelineShutdownCancellation "
            "window=2 reason=queue_unavailable "
            "graphics_available=false copy_available=true "
            "graphics_native=0 copy_native=2",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "inconsistent bounded pipeline Graphics availability contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_graphics_unavailable_rejects_copy_passes(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified")
        text = replace_testcase_marker(
            text,
            "BoundedCrossBatchSubmissionPipeline",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchSubmissionPipeline "
            "window=2 reason=queue_unavailable "
            "graphics_available=false compute_available=true "
            "graphics_native=0 compute_native=1",
        )
        text = replace_testcase_marker(
            text,
            "BoundedCrossBatchMalformedPreflightOrdering",
            "[TESTCASE][SKIP] "
            "name=BoundedCrossBatchMalformedPreflightOrdering "
            "reason=graphics_queue_unavailable",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "inconsistent bounded pipeline Graphics availability contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_window2_alias_requires_pipeline_overlap_marker(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "true",
            "blocked",
            include_overlap_marker=False,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "alias overlap marker presence contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_window_requires_common_contract_fields(self) -> None:
        text = pipeline_line(1, "false", "blocked").replace(
            "source_order=G,C",
            "source_order=C,G",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded cross-batch pipeline contract",
        ):
            runner.validate_log("pipeline-window1", text)

    def test_pipeline_window1_requires_blocked_overlap(self) -> None:
        text = pipeline_line(1, "false", "verified")
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded cross-batch pipeline contract",
        ):
            runner.validate_log("pipeline-window1", text)

    def test_pipeline_window2_distinct_native_requires_verified_overlap(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "skipped")
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded cross-batch pipeline contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_window2_alias_requires_blocked_overlap(self) -> None:
        text = pipeline_line(2, "true", "verified")
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded cross-batch pipeline contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_window2_requires_malformed_ordering_marker(
        self,
    ) -> None:
        marker = (
            "[TESTCASE][PASS] "
            "name=BoundedCrossBatchMalformedPreflightOrdering "
            "window=2 batches=2 queue=Graphics prefix=committed "
            "malformed=rejected callback_order=prefix,malformed "
            "signals=success,rejected native_owner=Submission replay=0 "
            "malformed_translate=0 runtime=restarted\n"
        )
        text = pipeline_line(2, "false", "verified").replace(marker, "")
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "malformed-preflight ordering terminal marker",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_malformed_ordering_requires_prefix_callback_order(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified").replace(
            "callback_order=prefix,malformed",
            "callback_order=malformed,prefix",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete malformed-preflight ordering contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_malformed_ordering_requires_no_translate(self) -> None:
        text = pipeline_line(2, "false", "verified").replace(
            "malformed_translate=0",
            "malformed_translate=1",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete malformed-preflight ordering contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_window2_requires_recoverable_rejection_marker(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "false",
            "verified",
            include_recoverable_rejection=False,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "recoverable rejection terminal marker",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_recoverable_rejection_requires_no_rejected_submit(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified").replace(
            "batch0_native_submit=0",
            "batch0_native_submit=1",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded cross-batch recoverable rejection contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_recoverable_rejection_requires_matching_alias(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified").replace(
            "name=BoundedCrossBatchRecoverableRejection "
            "window=2 native_alias=false",
            "name=BoundedCrossBatchRecoverableRejection "
            "window=2 native_alias=true",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded cross-batch recoverable rejection contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_recoverable_rejection_marker_must_be_unique(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified")
        duplicate = (
            "[TESTCASE][PASS] "
            "name=BoundedCrossBatchRecoverableRejection "
            "window=2 native_alias=false overlap_assertion=verified "
            "batches=2 batch0_native_submit=0 "
            "batch0_callbacks=ordinary1_success0 "
            "batch0_signal=rejected batch1_native_submit=1 "
            "batch1_callbacks=ordinary1_success1 "
            "batch1_signal=success native_owner=Submission replay=0\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "expected at most one terminal marker",
        ):
            runner.validate_log("pipeline-window2", text + duplicate)

    def test_pipeline_alias_requires_recoverable_overlap_marker(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "true",
            "blocked",
            include_recoverable_overlap_marker=False,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "recoverable alias overlap marker presence contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_recoverable_overlap_marker_rejected_for_distinct_queues(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "false",
            "verified",
            include_recoverable_overlap_marker=True,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "recoverable alias overlap marker presence contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_recoverable_overlap_requires_consistent_queue_ids(
        self,
    ) -> None:
        text = pipeline_line(2, "true", "blocked").replace(
            "name=BoundedCrossBatchRecoverableRejectionOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=7 "
            "compute_native=11 copy_native=7 admission=blocked",
            "name=BoundedCrossBatchRecoverableRejectionOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=8 "
            "compute_native=11 copy_native=8 admission=blocked",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid bounded recoverable alias fallback contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_recoverable_rejection_requires_copy_contract(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified").replace(
            "copy_translate_owner=Translate",
            "copy_translate_owner=Submission",
            1,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded cross-batch recoverable rejection contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_recoverable_overlap_requires_policy_seam(
        self,
    ) -> None:
        text = pipeline_line(2, "true", "blocked").replace(
            "cpu_seam=RHISubmissionPipelinePolicy",
            "cpu_seam=VulkanTranslateWaveScheduler",
            1,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid bounded recoverable alias fallback contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_window2_requires_shutdown_cancellation_marker(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "false",
            "verified",
            include_shutdown_cancellation=False,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "shutdown cancellation terminal marker",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_shutdown_cancellation_requires_stopped_owners(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified").replace(
            "owners=stopped",
            "owners=running",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded pipeline shutdown cancellation contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_shutdown_cancellation_requires_matching_alias(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified").replace(
            "name=BoundedPipelineShutdownCancellation "
            "window=2 native_alias=false",
            "name=BoundedPipelineShutdownCancellation "
            "window=2 native_alias=true",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded pipeline shutdown cancellation contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_shutdown_cancellation_marker_must_be_unique(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified")
        duplicate = (
            "[TESTCASE][PASS] name=BoundedPipelineShutdownCancellation "
            "window=2 native_alias=false overlap_assertion=verified "
            "batch0_native_submit=0 batch1_native_submit=1 "
            "batch0_signal=rejected batch1_signal=success "
            "callbacks=exactly_once concurrent_sync=drained owners=stopped\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "expected at most one terminal marker",
        ):
            runner.validate_log("pipeline-window2", text + duplicate)

    def test_pipeline_shutdown_overlap_requires_consistent_queue_ids(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "true",
            "blocked",
            include_shutdown_overlap_marker=True,
        ).replace(
            "name=BoundedPipelineShutdownCancellationOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=7 "
            "compute_native=11 copy_native=7 admission=blocked",
            "name=BoundedPipelineShutdownCancellationOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=8 "
            "compute_native=11 copy_native=8 admission=blocked",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid bounded pipeline shutdown alias fallback contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_shutdown_cancellation_requires_copy_readback(
        self,
    ) -> None:
        text = pipeline_line(2, "false", "verified").replace(
            "readback=verified callbacks=exactly_once",
            "readback=missing callbacks=exactly_once",
            1,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete bounded pipeline shutdown cancellation contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_shutdown_overlap_requires_policy_seam(
        self,
    ) -> None:
        text = pipeline_line(2, "true", "blocked").replace(
            "name=BoundedPipelineShutdownCancellationOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=7 "
            "compute_native=11 copy_native=7 admission=blocked "
            "cpu_seam=RHISubmissionPipelinePolicy",
            "name=BoundedPipelineShutdownCancellationOverlap "
            "requested_window=2 effective_window=1 "
            "reason=native_queue_alias graphics_native=7 "
            "compute_native=11 copy_native=7 admission=blocked "
            "cpu_seam=VulkanTranslateWaveScheduler",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid bounded pipeline shutdown alias fallback contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_alias_requires_shutdown_overlap_marker(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "true",
            "blocked",
            include_shutdown_overlap_marker=False,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "shutdown alias marker presence contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_shutdown_overlap_marker_rejected_for_distinct_queues(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "false",
            "verified",
            include_shutdown_overlap_marker=True,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "shutdown alias marker presence contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_alias_overlap_marker_is_rejected_for_distinct_native_queues(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "false",
            "verified",
            include_overlap_marker=True,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "alias overlap marker presence contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_alias_overlap_requires_matching_native_queue_ids(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "true",
            "blocked",
            include_overlap_marker=True,
        ).replace("copy_native=7", "copy_native=8", 1)
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid bounded cross-batch alias fallback contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_alias_overlap_requires_an_actual_native_alias(
        self,
    ) -> None:
        text = pipeline_line(
            2,
            "true",
            "blocked",
            include_overlap_marker=True,
        ).replace(
            "graphics_native=7 compute_native=11 copy_native=7",
            "graphics_native=7 compute_native=11 copy_native=13",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid bounded cross-batch alias fallback contract",
        ):
            runner.validate_log("pipeline-window2", text)

    def test_pipeline_modes_reject_validation_failures(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "Vulkan validation VUID",
        ):
            runner.validate_log(
                "pipeline-window1",
                "VUID-vkQueueSubmit-test\n"
                + pipeline_line(1, "false", "blocked"),
            )

    def test_pipeline_modes_map_to_focused_executable_arguments(self) -> None:
        cases = (
            (
                "pipeline-window1",
                "--pipeline-window1",
                pipeline_line(1, "false", "blocked"),
            ),
            (
                "pipeline-window2",
                "--pipeline-window2",
                pipeline_line(2, "false", "verified"),
            ),
            (
                "present-boundary",
                "--present-boundary",
                present_boundary_line(),
            ),
            (
                "present-hard",
                "--present-hard",
                present_hard_line(),
            ),
            (
                "present-legacy-owner",
                "--present-legacy-owner",
                present_legacy_owner_line(),
            ),
            (
                "present-completion-shutdown",
                "--present-completion-shutdown",
                present_completion_shutdown_line(),
            ),
            (
                "rt-export-rejection",
                "--rt-export-rejection",
                rt_export_rejection_line(),
            ),
            (
                "readback-future",
                "--readback-future",
                readback_future_line(),
            ),
            (
                "occlusion-query",
                "--occlusion-query",
                occlusion_query_line(),
            ),
            (
                "timestamp-query-success-batch",
                "--timestamp-query-success-batch",
                timestamp_query_success_batch_line(),
            ),
            (
                "timestamp-query",
                "--timestamp-query",
                timestamp_query_line(),
            ),
            (
                "timestamp-query-mid-failure",
                "--timestamp-query-mid-failure",
                timestamp_query_mid_failure_line(),
            ),
            (
                "timestamp-query-record-failure",
                "--timestamp-query-record-failure",
                timestamp_query_record_failure_line(),
            ),
            (
                "gpu-scope-stream",
                "--gpu-scope-stream",
                gpu_scope_stream_line(),
            ),
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            for mode, expected_argument, output in cases:
                with self.subTest(mode=mode):
                    completed = subprocess.CompletedProcess(
                        args=[],
                        returncode=0,
                        stdout=output,
                        stderr="",
                    )
                    with mock.patch.object(
                        runner.subprocess,
                        "run",
                        return_value=completed,
                    ) as run:
                        runner.run_case(
                            Path("TestRHIParallelRecordVulkan.exe"),
                            Path(temporary_directory),
                            mode,
                            30.0,
                        )
                    self.assertEqual(
                        run.call_args.args[0],
                        [
                            "TestRHIParallelRecordVulkan.exe",
                            expected_argument,
                        ],
                    )

    def test_declared_gpu_matrix_contains_all_21_unique_modes(self) -> None:
        modes = [test_case.mode for test_case in runner.VULKAN_TEST_CASES]
        self.assertEqual(
            set(modes),
            {
                "serial",
                "parallel",
                "fallback",
                "gated",
                "heavy",
                "translate-hard",
                "multi-segment-hard",
                "pipeline-window1",
                "pipeline-window2",
                "present-boundary",
                "present-hard",
                "present-legacy-owner",
                "present-completion-shutdown",
                "rt-export-rejection",
                "readback-future",
                "occlusion-query",
                "timestamp-query",
                "timestamp-query-success-batch",
                "timestamp-query-mid-failure",
                "timestamp-query-record-failure",
                "gpu-scope-stream",
            },
        )
        self.assertEqual(len(modes), 21)
        self.assertEqual(len(set(modes)), 21)
        self.assertEqual(set(modes), set(runner.VULKAN_TEST_CASE_BY_MODE))

    def test_new_focused_modes_reject_weakened_terminal_contracts(self) -> None:
        cases = (
            (
                "present-legacy-owner",
                present_legacy_owner_line().replace(
                    "owner=Submission", "owner=Completion"
                ),
            ),
            (
                "present-completion-shutdown",
                present_completion_shutdown_line().replace(
                    "outstanding=0", "outstanding=1"
                ),
            ),
            (
                "timestamp-query",
                timestamp_query_line().replace(
                    "allocator_slot_reuse=verified",
                    "allocator_slot_reuse=missing",
                ),
            ),
            (
                "timestamp-query-mid-failure",
                timestamp_query_mid_failure_line().replace(
                    "pre_terminal_callback_entry=0",
                    "pre_terminal_callback_entry=1",
                ),
            ),
            (
                "timestamp-query-record-failure",
                timestamp_query_record_failure_line().replace(
                    "failed_phase_gate_count=0",
                    "failed_phase_gate_count=1",
                ),
            ),
            (
                "gpu-scope-stream",
                gpu_scope_stream_line().replace(
                    "query_free_sibling=parallel-effective",
                    "query_free_sibling=serial",
                ),
            ),
        )
        for mode, text in cases:
            with self.subTest(mode=mode), self.assertRaises(
                runner.VulkanTestError
            ):
                runner.validate_log(mode, text)

    def test_gpu_scope_stream_accepts_only_documented_nonstrict_skip(self) -> None:
        documented = (
            "[TESTCASE][SKIP] "
            "name=GpuScopeStreamCompletionAndParallelIsolation "
            "reason=graphics_queue_unavailable\n"
        )
        runner.validate_log("gpu-scope-stream", documented)
        with self.assertRaisesRegex(runner.VulkanTestError, "invalid SKIP"):
            runner.validate_log(
                "gpu-scope-stream",
                documented.replace(
                    "graphics_queue_unavailable", "test_disabled"
                ),
            )

    def test_gpu_scope_stream_rejects_invalid_timing_evidence(self) -> None:
        for duration, exclusive in (
            ("0,3.25", "0,3.25"),
            ("nan,3.25", "0,3.25"),
            ("12.5,3.25", "8.0,3.25"),
        ):
            with self.subTest(
                duration=duration, exclusive=exclusive
            ), self.assertRaisesRegex(
                runner.VulkanTestError,
                "query-island/parallel-sibling contract",
            ):
                runner.validate_log(
                    "gpu-scope-stream",
                    gpu_scope_stream_line()
                    .replace("duration_ns=12.5,3.25", f"duration_ns={duration}")
                    .replace("exclusive_ns=9.25,3.25", f"exclusive_ns={exclusive}"),
                )

    def test_full_timestamp_mode_requires_all_three_unique_boundaries(self) -> None:
        for marker_name in (
            "TimestampQueryCompletionOwnership",
            "TimestampQueryPreparationRejection",
            "TimestampQueryPreflightRejection",
        ):
            with self.subTest(marker=marker_name), self.assertRaises(
                runner.VulkanTestError
            ):
                runner.validate_log(
                    "timestamp-query",
                    replace_testcase_marker(
                        timestamp_query_line(), marker_name, ""
                    ),
                )

    def test_timestamp_dynamic_success_variants_are_accepted(self) -> None:
        runner.validate_log(
            "timestamp-query",
            timestamp_query_line().replace("prepare_calls=2", "prepare_calls=1"),
        )
        runner.validate_log(
            "timestamp-query-record-failure",
            timestamp_query_record_failure_line()
            .replace("recorded_phase_gate_count=1", "recorded_phase_gate_count=0")
            .replace("failed_phase_gate_count=0", "failed_phase_gate_count=1"),
        )

    def test_strict_gate_rejects_documented_gpu_scope_skip(self) -> None:
        completed = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout=(
                gpu_env_line()
                + "[TESTCASE][SKIP] "
                "name=GpuScopeStreamCompletionAndParallelIsolation "
                "reason=graphics_queue_unavailable\n"
            ),
            stderr="",
        )
        requirements = runner.GpuGateRequirements(
            require_marker=True,
            require_validation=True,
            reject_testcase_skips=True,
        )
        with tempfile.TemporaryDirectory() as temporary_directory, mock.patch.object(
            runner.subprocess, "run", return_value=completed
        ):
            with self.assertRaisesRegex(
                runner.VulkanTestError, "does not permit TESTCASE skips"
            ):
                runner.run_case(
                    Path("TestRHIParallelRecordVulkan.exe"),
                    Path(temporary_directory),
                    "gpu-scope-stream",
                    30.0,
                    requirements,
                )

    def test_readback_future_contract_is_accepted(self) -> None:
        runner.validate_log(
            "readback-future",
            readback_future_line(),
        )

    def test_readback_future_requires_whole_image_restore_marker(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete owning readback contract",
        ):
            runner.validate_log(
                "readback-future",
                readback_future_line().replace(
                    " state_restore=whole_image", ""
                ),
            )

    def test_readback_future_rejects_validation_vuid(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "Vulkan validation VUID",
        ):
            runner.validate_log(
                "readback-future",
                "VUID-VkImageMemoryBarrier2-oldLayout-01197\n"
                + readback_future_line(),
            )

    def test_occlusion_query_contract_is_accepted(self) -> None:
        runner.validate_log(
            "occlusion-query",
            occlusion_query_line(),
        )

    def test_occlusion_query_requires_serial_island(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete native query contract",
        ):
            runner.validate_log(
                "occlusion-query",
                occlusion_query_line().replace(
                    "recording=serial_island",
                    "recording=parallel_wave",
                ),
            )

    def test_occlusion_query_requires_bulk_growth(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete native query contract",
        ):
            runner.validate_log(
                "occlusion-query",
                occlusion_query_line().replace(
                    "bulk_pairs=48 growth=verified",
                    "bulk_pairs=1 growth=unverified",
                ),
            )

    def test_occlusion_query_requires_precision_capability(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete native query contract",
        ):
            runner.validate_log(
                "occlusion-query",
                occlusion_query_line().replace(
                    "count_precise=true ",
                    "",
                ),
            )

        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete native query contract",
        ):
            runner.validate_log(
                "occlusion-query",
                occlusion_query_line().replace(
                    "count_precise=true",
                    "count_precise=unknown",
                ),
            )

    def test_occlusion_query_rejects_validation_vuid(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "Vulkan validation VUID",
        ):
            runner.validate_log(
                "occlusion-query",
                "VUID-vkCmdBeginQuery-None-00807\n"
                + occlusion_query_line(),
            )

    def test_timestamp_query_success_batch_contract_is_accepted(
        self,
    ) -> None:
        runner.validate_log(
            "timestamp-query-success-batch",
            timestamp_query_success_batch_line(),
        )

    def test_timestamp_query_success_batch_rejects_partial_publication(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete batch publication contract",
        ):
            runner.validate_log(
                "timestamp-query-success-batch",
                timestamp_query_success_batch_line().replace(
                    "queries=batch_published_before_notify",
                    "queries=per_packet_notify",
                ),
            )

    def test_timestamp_query_success_batch_rejects_early_queue_sync(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete cross-queue settlement contract",
        ):
            runner.validate_log(
                "timestamp-query-success-batch",
                timestamp_query_success_batch_line().replace(
                    "queue_local_sync=blocked_until_group_settled",
                    "queue_local_sync=returned_before_group_settled",
                ),
            )

    def test_timestamp_query_success_batch_rejects_mixed_tier_inversion(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete mixed-topology callback-tier contract",
        ):
            runner.validate_log(
                "timestamp-query-success-batch",
                timestamp_query_success_batch_line().replace(
                    "order=Query,AllOrdinary,AllSuccess",
                    "order=Query,SuccessInsideOrdinary",
                ),
            )

    def test_timestamp_query_success_batch_rejects_copy_isolation_failure(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete Copy unsupported fallback contract",
        ):
            runner.validate_log(
                "timestamp-query-success-batch",
                timestamp_query_success_batch_line().replace(
                    "query=Error copy=verified signal=success",
                    "query=Error copy=rejected signal=success",
                ),
            )

    def test_timestamp_query_success_batch_rejects_copy_callback_owner(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete Copy unsupported fallback contract",
        ):
            runner.validate_log(
                "timestamp-query-success-batch",
                timestamp_query_success_batch_line().replace(
                    "native_owner=Submission owner=Completion "
                    "callbacks=exactly_once runtime=recovered",
                    "native_owner=Submission owner=Translate "
                    "callbacks=exactly_once runtime=recovered",
                ),
            )

    def test_timestamp_query_success_batch_rejects_copy_allocator_reuse(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete Copy supported contract",
        ):
            runner.validate_log(
                "timestamp-query-success-batch",
                timestamp_query_success_batch_line().replace(
                    "allocator_reuse=verified",
                    "allocator_reuse=unverified",
                ),
            )

    def test_timestamp_query_success_batch_rejects_copy_supported_reset_mode(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete Copy supported contract",
        ):
            runner.validate_log(
                "timestamp-query-success-batch",
                timestamp_query_success_batch_line().replace(
                    "valid_bits=64 reset_mode=host",
                    "valid_bits=64 reset_mode=unsupported",
                ),
            )

    def test_timestamp_query_success_batch_accepts_copy_reset_unsupported_skip(
        self,
    ) -> None:
        supported_marker = (
            "[TESTCASE][PASS] "
            "name=TimestampQueryCopySupported "
            "queue=Copy status=Ready valid_bits=64 reset_mode=host "
            "copy=verified gpu_completion=Ready "
            "order=signal->completion->query->ordinary->success "
            "owner=Completion callbacks=exactly_once "
            "allocator_reuse=verified native_owner=Submission replay=0"
        )
        unsupported_marker = (
            "[TESTCASE][SKIP] "
            "name=TimestampQueryCopySupported "
            "reason=timestamp_unsupported valid_bits=64 "
            "reset_mode=unsupported"
        )
        runner.validate_log(
            "timestamp-query-success-batch",
            timestamp_query_success_batch_line().replace(
                supported_marker,
                unsupported_marker,
            ),
        )

    def test_rt_export_rejection_contract_is_accepted(self) -> None:
        runner.validate_log(
            "rt-export-rejection",
            rt_export_rejection_line(),
        )

    def test_rt_export_rejection_rejects_retry_latch(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete export transaction contract",
        ):
            runner.validate_log(
                "rt-export-rejection",
                rt_export_rejection_line().replace(
                    "attempt1_latch=false", "attempt1_latch=true"
                ),
            )

    def test_rt_export_rejection_requires_readback_recovery(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "incomplete export transaction contract",
        ):
            runner.validate_log(
                "rt-export-rejection",
                rt_export_rejection_line().replace(
                    "readback_retry=frame_accepted",
                    "readback_retry=frame_rejected",
                ),
            )

    def test_rt_export_rejection_requires_materialization_failure(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError, "accepted-readback failure"
        ):
            runner.validate_log(
                "rt-export-rejection",
                replace_testcase_marker(
                    rt_export_rejection_line(),
                    "RaytracingAcceptedReadbackMaterializationFailure",
                    "",
                ),
            )

    def test_present_boundary_contract_is_accepted(self) -> None:
        runner.validate_log(
            "present-boundary", present_boundary_line()
        )
        runner.validate_log(
            "present-boundary", present_boundary_line("elided")
        )

    def test_present_boundary_requires_serial_control_gate(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "SerialControlPipelineBoundary",
        ):
            runner.validate_log(
                "present-boundary",
                present_boundary_line().replace(
                    serial_control_boundary_line(), ""
                ),
            )

    def test_present_boundary_requires_every_invoked_subtest_marker(self) -> None:
        for marker_name in (
            "PresentSourceContractRejection",
            "PresentationCompletionIntegrationBoundary",
        ):
            with self.subTest(marker=marker_name), self.assertRaises(
                runner.VulkanTestError
            ):
                runner.validate_log(
                    "present-boundary",
                    replace_testcase_marker(
                        present_boundary_line(), marker_name, ""
                    ),
                )

    def test_present_source_contract_accepts_qualified_queue_skips(self) -> None:
        runner.validate_log(
            "present-boundary",
            present_boundary_line()
            .replace("rejected=17", "rejected=15")
            .replace("copy_commit=verified", "copy_commit=skipped")
            .replace("bindless_cross_queue=verified", "bindless_cross_queue=skipped"),
        )

    def test_serial_control_boundary_requires_blocked_later_translate(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "SerialControl pipeline-boundary contract",
        ):
            runner.validate_log(
                "present-boundary",
                present_boundary_line().replace(
                    "later_translate=blocked",
                    "later_translate=overtook",
                ),
            )

    def test_present_boundary_requires_shutdown_gate(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "QueuedPresentShutdownBoundary",
        ):
            runner.validate_log(
                "present-boundary",
                present_boundary_line().replace(
                    queued_present_shutdown_line(), ""
                ),
            )

    def test_queued_present_shutdown_requires_retry(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "queued Present shutdown contract",
        ):
            runner.validate_log(
                "present-boundary",
                present_boundary_line().replace(
                    "outcome=Retry prefix=rejected",
                    "outcome=Recreate prefix=rejected",
                ),
            )

    def test_present_boundary_requires_exactly_one_receipt_attempt(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "Present pipeline-boundary contract",
        ):
            runner.validate_log(
                "present-boundary",
                present_boundary_line().replace(
                    "owner=Submission receipt_attempts=1 submitted=false",
                    "owner=Submission receipt_attempts=2 submitted=false",
                ),
            )

    def test_present_boundary_bridge_matches_native_identity(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "Present pipeline-boundary contract",
        ):
            runner.validate_log(
                "present-boundary",
                present_boundary_line("elided").replace(
                    "prefix_native=0", "prefix_native=2"
                ),
            )

    def test_present_boundary_accepts_alias_completion_marker(
        self,
    ) -> None:
        runner.validate_log(
            "present-boundary",
            present_boundary_line("required").replace(
                "prefix_native=2", "prefix_native=0"
            ),
        )

    def test_present_hard_contract_is_accepted(self) -> None:
        runner.validate_log("present-hard", present_hard_line())

    def test_present_hard_rejects_native_work_after_boundary(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "hard Present boundary contract",
        ):
            runner.validate_log(
                "present-hard",
                present_hard_line().replace(
                    "native_after_present=0",
                    "native_after_present=1",
                ),
            )

    def test_present_hard_requires_fault_priority_and_concurrent_latch(
        self,
    ) -> None:
        for original, replacement in (
            ("device_fault_priority=true", "device_fault_priority=false"),
            ("invalid_source=true", "invalid_source=false"),
            (
                "concurrent_hard_latch=verified",
                "concurrent_hard_latch=missing",
            ),
        ):
            with self.subTest(field=original), self.assertRaisesRegex(
                runner.VulkanTestError,
                "hard Present boundary contract",
            ):
                runner.validate_log(
                    "present-hard",
                    present_hard_line().replace(original, replacement),
                )

    def test_serial_accepts_only_pass_marker(self) -> None:
        runner.validate_log("serial", pass_line("serial", "false"))

    def test_completion_aggregate_cpu_probe_contract_is_accepted(self) -> None:
        runner.validate_log("serial", pass_line("serial", "false"))

    def test_completion_aggregate_cpu_probe_marker_is_required(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "MultiSegmentCompletionAggregateCpuProbe",
            "",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "completion aggregate CPU probe marker",
        ):
            runner.validate_log("serial", text)

    def test_completion_aggregate_cpu_probe_fields_are_required(self) -> None:
        text = pass_line("serial", "false").replace(
            "prefix_second=ordinary1_success0",
            "prefix_second=ordinary1_success1",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "completion aggregate CPU probe contract",
        ):
            runner.validate_log("serial", text)

    def test_marker_parser_accepts_unique_fields(self) -> None:
        marker = runner._testcase_marker(
            "UniqueFields",
            "[TESTCASE][PASS] name=UniqueFields first=1 second=2\n",
        )
        self.assertEqual(
            marker,
            (
                "PASS",
                {"name": "UniqueFields", "first": "1", "second": "2"},
            ),
        )

    def test_marker_parser_rejects_duplicate_fields(self) -> None:
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "duplicate terminal marker fields: callbacks",
        ):
            runner._testcase_marker(
                "DuplicateFields",
                "[TESTCASE][PASS] name=DuplicateFields "
                "callbacks=wrong callbacks=exactly_once\n",
            )

    def test_regular_mode_requires_parallel_translate_contracts(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelRecordOrderedReadback "
            "mode=serial worker_fault=false production_gate=false "
            "production_heavy=false iterations=24\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "ready-native-lane Translate smoke"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_rejects_unapproved_skip_reason(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "AsyncQueueParallelTranslateSmoke",
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "reason=test_disabled cpu_seam=VulkanTranslateWaveScheduler "
            "graphics_available=true compute_available=true "
            "copy_available=true "
            "graphics_native=0 compute_native=1 copy_native=2",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "invalid ready-native-lane"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_requires_observed_submit_order(self) -> None:
        text = pass_line("serial", "false").replace(
            "observed_submit_order=G,G,C,Copy",
            "observed_submit_order=G,C,G,Copy",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "incomplete ready-native-lane"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_requires_copy_translate_owner(self) -> None:
        text = pass_line("serial", "false").replace(
            "copy_translate_owner=Translate",
            "copy_translate_owner=Submission",
            1,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "incomplete ready-native-lane"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_requires_verified_readback_and_no_replay(
        self,
    ) -> None:
        text = pass_line("serial", "false").replace(
            "readback=verified callbacks=exactly_once replay=0",
            "readback=verified callbacks=exactly_once replay=1",
            1,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "incomplete ready-native-lane"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_copy_alias_uses_alias_fallback(self) -> None:
        text = pass_line("serial", "false").replace(
            "graphics_native=0 compute_native=1 copy_native=2",
            "graphics_native=0 compute_native=1 copy_native=0",
            1,
        ).replace(
            "first_ready_lanes=G,C,Copy",
            "first_ready_lanes=alias_fallback",
            1,
        )
        runner.validate_log("serial", text)

    def test_ready_translate_copy_alias_rejects_distinct_lane_claim(
        self,
    ) -> None:
        text = pass_line("serial", "false").replace(
            "graphics_native=0 compute_native=1 copy_native=2",
            "graphics_native=0 compute_native=1 copy_native=0",
            1,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "incomplete ready-native-lane"
        ):
            runner.validate_log("serial", text)

    def test_recoverable_translate_requires_matching_suffix_observation(
        self,
    ) -> None:
        text = pass_line("serial", "false").replace(
            "distinct_native=true suffix_recorded=true",
            "distinct_native=true suffix_recorded=false",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "incomplete recoverable"
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_requires_stable_source_segment_identity(self) -> None:
        text = pass_line("serial", "false").replace(
            "source_segments=0:0/2,0:1/2",
            "source_segments=0:0/2,1:1/2",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "multi-segment source execution",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_requires_deterministic_alias_observation(self) -> None:
        text = pass_line("serial", "false").replace(
            "compute_saw_graphics_finished=false",
            "compute_saw_graphics_finished=true",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "multi-segment source execution",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_queue_skip_contract_is_accepted(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "MultiSegmentSourceExecution",
            "[TESTCASE][SKIP] name=MultiSegmentSourceExecution "
            "reason=queue_unavailable graphics_native=0 compute_native=1",
        )
        text = replace_testcase_marker(
            text,
            "MultiSegmentRecoverableRejection",
            "[TESTCASE][SKIP] name=MultiSegmentRecoverableRejection "
            "reason=queue_unavailable graphics_native=0 compute_native=1",
        )
        runner.validate_log("serial", text)

    def test_multi_segment_queue_skip_requires_topology_fields(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "MultiSegmentSourceExecution",
            "[TESTCASE][SKIP] name=MultiSegmentSourceExecution "
            "reason=queue_unavailable graphics_native=0",
        )
        text = replace_testcase_marker(
            text,
            "MultiSegmentRecoverableRejection",
            "[TESTCASE][SKIP] name=MultiSegmentRecoverableRejection "
            "reason=queue_unavailable graphics_native=0 compute_native=1",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid Phase15C multi-segment SKIP contract",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_recovery_skip_requires_topology_fields(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "MultiSegmentSourceExecution",
            "[TESTCASE][SKIP] name=MultiSegmentSourceExecution "
            "reason=queue_unavailable graphics_native=0 compute_native=1",
        )
        text = replace_testcase_marker(
            text,
            "MultiSegmentRecoverableRejection",
            "[TESTCASE][SKIP] name=MultiSegmentRecoverableRejection "
            "reason=queue_unavailable compute_native=1",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid Phase15C multi-segment SKIP contract",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_alias_requires_serial_translate_and_no_wait(
        self,
    ) -> None:
        text = pass_line("serial", "false").replace(
            "native_alias=false first_ready_lanes=G,C",
            "native_alias=true first_ready_lanes=G,C",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "multi-segment source execution",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_copy_requires_real_native_submit_order(self) -> None:
        text = pass_line("serial", "false").replace(
            "observed_submit_order=G,Copy,G",
            "observed_submit_order=G,G,Copy",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "multi-segment Copy round-trip",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_copy_alias_requires_no_predecessor_waits(self) -> None:
        text = pass_line("serial", "false").replace(
            "queues=Graphics,Copy,Graphics native_alias=false",
            "queues=Graphics,Copy,Graphics native_alias=true",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "multi-segment Copy round-trip",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_copy_native_alias_contract_is_accepted(self) -> None:
        text = pass_line("serial", "false")
        text = text.replace(
            "queues=Graphics,Copy,Graphics native_alias=false",
            "queues=Graphics,Copy,Graphics native_alias=true",
        ).replace(
            "source_segments=0:0/3,0:1/3,0:2/3 predecessor_waits=2",
            "source_segments=0:0/3,0:1/3,0:2/3 predecessor_waits=0",
        )
        runner.validate_log("serial", text)

    def test_multi_segment_copy_skip_contract_is_accepted(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "MultiSegmentCopyRoundTrip",
            "[TESTCASE][SKIP] name=MultiSegmentCopyRoundTrip "
            "reason=copy_queue_unavailable graphics_native=0 copy_native=2",
        )
        runner.validate_log("serial", text)

    def test_multi_segment_copy_skip_requires_topology_fields(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "MultiSegmentCopyRoundTrip",
            "[TESTCASE][SKIP] name=MultiSegmentCopyRoundTrip "
            "reason=copy_queue_unavailable graphics_native=0",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "invalid Phase15C multi-segment Copy SKIP contract",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_native_alias_contract_is_accepted(self) -> None:
        text = pass_line("serial", "false")
        text = text.replace(
            "native_alias=false first_ready_lanes=G,C",
            "native_alias=true first_ready_lanes=G",
        ).replace(
            "source_segments=0:0/2,0:1/2 predecessor_waits=1",
            "source_segments=0:0/2,0:1/2 predecessor_waits=0",
        ).replace(
            "compute_saw_graphics_finished=false",
            "compute_saw_graphics_finished=true",
        ).replace(
            "dependency=rejected distinct_native=true suffix_recorded=true",
            "dependency=rejected distinct_native=false suffix_recorded=false",
        )
        runner.validate_log("serial", text)

    def test_multi_segment_rejection_requires_aggregate_success_suppression(
        self,
    ) -> None:
        text = pass_line("serial", "false").replace(
            "callbacks=ordinary1_success0 signal=rejected",
            "callbacks=ordinary1_success1 signal=rejected",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "multi-segment recoverable rejection",
        ):
            runner.validate_log("serial", text)

    def test_multi_segment_rejection_must_skip_native_submit(self) -> None:
        text = pass_line("serial", "false").replace(
            "suffix_recorded=true native_rejected=0",
            "suffix_recorded=true native_rejected=1",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError,
            "multi-segment recoverable rejection",
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_real_native_submission_owner(self) -> None:
        text = pass_line("serial", "false").replace(
            "native_owner=verified runtime=recovered",
            "native_owner=legacy runtime=recovered",
            1,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "recoverable Copy rejection"
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_rejected_packets_skip_native_submit(
        self,
    ) -> None:
        text = pass_line("serial", "false").replace(
            "native_rejected=0 native_accepted=3",
            "native_rejected=1 native_accepted=3",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "recoverable Copy rejection"
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_alias_aware_gpu_sync_count(self) -> None:
        text = pass_line("serial", "false").replace(
            "gpu_syncs=2 graphics_native=0 copy_native=2 native_alias=false",
            "gpu_syncs=2 graphics_native=0 copy_native=0 native_alias=true",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "Graphics-Copy round-trip"
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_deterministic_shutdown_wait(self) -> None:
        text = pass_line("serial", "false").replace(
            "copy_wait=entered dependency=unpublished",
            "copy_wait=unobserved dependency=unpublished",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "shutdown dependency cancellation"
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_cross_queue_native_owner(self) -> None:
        text = pass_line("serial", "false").replace(
            "ownership=explicit native_owner=verified native_alias=false",
            "ownership=explicit native_owner=legacy native_alias=false",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "cross-queue Submission ownership"
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_direct_execute_skip_native_submit(
        self,
    ) -> None:
        text = pass_line("serial", "false").replace(
            "runtime_claim=exclusive native_submit=0 callbacks=exactly_once",
            "runtime_claim=exclusive native_submit=1 callbacks=exactly_once",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "direct Copy fail-closed"
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_io_session_recycle_and_native_submit(
        self,
    ) -> None:
        text = pass_line("serial", "false").replace(
            "commits=2 signals=2 native_submit=2 native_owner=Submission",
            "commits=2 signals=2 native_submit=1 native_owner=Submission",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "VulkanStorage unified Copy submission"
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_shutdown_skip_native_submit(self) -> None:
        text = pass_line("serial", "false").replace(
            "dependency=unpublished native_submit=0 sync_wait=registered",
            "dependency=unpublished native_submit=1 sync_wait=registered",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "shutdown dependency cancellation"
        ):
            runner.validate_log("serial", text)

    def test_copy_contracts_require_round_trip_mode(self) -> None:
        text = pass_line("serial", "false").replace(
            "mode=serial ownership=release-acquire transfers=2",
            "mode=parallel ownership=release-acquire transfers=2",
            1,
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "Graphics-Copy round-trip"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_rejects_duplicate_terminal_markers(self) -> None:
        text = pass_line("serial", "false")
        duplicate = (
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "reason=graphics_compute_native_alias "
            "cpu_seam=VulkanTranslateWaveScheduler "
            "graphics_available=true compute_available=true "
            "copy_available=true "
            "graphics_native=0 compute_native=0 copy_native=2\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "at most one terminal marker"
        ):
            runner.validate_log("serial", text + duplicate)

    def test_ready_translate_alias_skip_requires_matching_native_queues(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "AsyncQueueParallelTranslateSmoke",
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "reason=graphics_compute_native_alias "
            "cpu_seam=VulkanTranslateWaveScheduler "
            "graphics_available=true compute_available=true "
            "copy_available=true "
            "graphics_native=0 compute_native=1 copy_native=2",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "invalid ready-native-lane Translate SKIP"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_alias_skip_accepts_matching_native_queues(self) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "AsyncQueueParallelTranslateSmoke",
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "reason=graphics_compute_native_alias "
            "cpu_seam=VulkanTranslateWaveScheduler "
            "graphics_available=true compute_available=true "
            "copy_available=true "
            "graphics_native=0 compute_native=0 copy_native=2",
        )
        runner.validate_log("serial", text)

    def test_ready_translate_queue_unavailable_skip_requires_copy_id(
        self,
    ) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "AsyncQueueParallelTranslateSmoke",
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "reason=queue_unavailable "
            "cpu_seam=VulkanTranslateWaveScheduler "
            "graphics_available=true compute_available=true "
            "copy_available=false "
            "graphics_native=0 compute_native=1",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "invalid ready-native-lane Translate SKIP"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_queue_unavailable_skip_is_accepted(
        self,
    ) -> None:
        text = replace_testcase_marker(
            pass_line("serial", "false"),
            "AsyncQueueParallelTranslateSmoke",
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "reason=queue_unavailable "
            "cpu_seam=VulkanTranslateWaveScheduler "
            "graphics_available=true compute_available=true "
            "copy_available=false "
            "graphics_native=0 compute_native=1 copy_native=2",
        )
        runner.validate_log("serial", text)

    def test_parallel_requires_wave_island_wave_and_concurrency(self) -> None:
        text = wave(1, 0) + island(1) + wave(1, 1) + summary(1) + pass_line("parallel", "false")
        runner.validate_log("parallel", text)

    def test_parallel_rejects_missing_second_wave(self) -> None:
        text = wave(1, 0) + island(1) + summary(1) + pass_line("parallel", "false")
        with self.assertRaisesRegex(runner.VulkanTestError, "do not prove"):
            runner.validate_log("parallel", text)

    def test_production_gate_rejects_cheap_copy_workload(self) -> None:
        text = (
            "[ParallelRecord] batch=1 requested=true effective=false "
            "reason=insufficient-work coordinator=9\n"
            + pass_line("parallel", "false", "true")
        )
        runner.validate_log("gated", text)

    def test_production_heavy_requires_real_weighted_parallel_work(self) -> None:
        text = (
            wave(1, 0)
            + island(1)
            + wave(1, 1)
            + summary(1)
            + pass_line("parallel", "false", "true", "true")
        )
        runner.validate_log("heavy", text)

    def test_fallback_requires_exactly_one_post_command_injection(self) -> None:
        text = (
            "[ParallelRecord][Injection] point=worker-throw phase=after-first-command "
            "trigger=1 batch=1 layer=0 command=0\n"
            + wave(1, 0).replace("worker_recorded=true", "worker_recorded=false")
            + island(1)
            + wave(1, 1)
            + summary(1, "serial-fallback-worker-failure")
            + wave(2, 0)
            + island(2)
            + wave(2, 1)
            + summary(2)
            + pass_line("parallel", "true")
        )
        runner.validate_log("fallback", text)

    def test_validation_vuid_is_fatal(self) -> None:
        with self.assertRaisesRegex(runner.VulkanTestError, "Vulkan validation VUID"):
            runner.validate_log("serial", "VUID-vkQueueSubmit-test\n" + pass_line("serial", "false"))

    def test_translate_hard_requires_retirement_and_fault_summary(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        runner.validate_log("translate-hard", text)

    def test_translate_hard_rejects_native_submit_after_fault(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=1 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "hard-fault ownership summary"
        ):
            runner.validate_log("translate-hard", text)

    def test_translate_hard_accepts_native_alias_without_suffix_recording(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=false suffix_recorded=false suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        runner.validate_log("translate-hard", text)

    def test_translate_hard_requires_copy_no_submit_no_replay(self) -> None:
        base = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        mutations = (
            ("suffix_queue=Copy", "suffix_queue=Compute"),
            ("native_submit=0", "native_submit=1"),
            (
                "hard_latch=later_batch_failed replay=0",
                "hard_latch=later_batch_failed replay=1",
            ),
        )
        for original, replacement in mutations:
            with self.subTest(field=original):
                with self.assertRaisesRegex(
                    runner.VulkanTestError,
                    "incomplete failure-retirement contract",
                ):
                    runner.validate_log(
                        "translate-hard",
                        base.replace(original, replacement, 1),
                    )

    def test_translate_hard_rejects_duplicate_retirement_markers(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=false suffix_recorded=true suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "at most one terminal marker"
        ):
            runner.validate_log("translate-hard", text)

    def test_translate_hard_rejects_conflicting_fault_summaries(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
            "[VulkanFault][Summary] first_fault_count=2 "
            "native_submit_after_fault=7 native_present_after_fault=0 "
            "device_lost=true\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "expected exactly one marker"
        ):
            runner.validate_log("translate-hard", text)

    def test_translate_hard_rejects_duplicate_fault_summary_fields(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 first_fault_count=2 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "duplicate fields"
        ):
            runner.validate_log("translate-hard", text)

    def test_translate_hard_rejects_multi_digit_fault_count_prefix(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true suffix_queue=Copy "
            "native_submit=0 signals=failed callbacks=exactly_once "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=10 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "hard-fault ownership summary"
        ):
            runner.validate_log("translate-hard", text)

    def test_multi_segment_hard_requires_aggregate_retirement(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] "
            "name=MultiSegmentPrefixSubmitSuffixTranslateFailure "
            "source=1 segments=2 queues=Graphics,Graphics "
            "prefix_translated=true source_submitted=0:0/2 "
            "native_accepted_prefix=1 native_owner=Submission "
            "callbacks=ordinary1_success0 signal=failed "
            "later_callbacks=ordinary1_success0 "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        runner.validate_log("multi-segment-hard", text)

    def test_multi_segment_hard_rejects_early_source_success(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] "
            "name=MultiSegmentPrefixSubmitSuffixTranslateFailure "
            "source=1 segments=2 queues=Graphics,Graphics "
            "prefix_translated=true source_submitted=0:0/2 "
            "native_accepted_prefix=1 native_owner=Submission "
            "callbacks=ordinary1_success1 signal=failed "
            "later_callbacks=ordinary1_success0 "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "aggregate retirement contract"
        ):
            runner.validate_log("multi-segment-hard", text)

    def test_multi_segment_hard_requires_exactly_one_native_prefix(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] "
            "name=MultiSegmentPrefixSubmitSuffixTranslateFailure "
            "source=1 segments=2 queues=Graphics,Graphics "
            "prefix_translated=true source_submitted=0:0/2 "
            "native_accepted_prefix=2 native_owner=Submission "
            "callbacks=ordinary1_success0 signal=failed "
            "later_callbacks=ordinary1_success0 "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "aggregate retirement contract"
        ):
            runner.validate_log("multi-segment-hard", text)

    def test_multi_segment_hard_rejects_duplicate_fault_summaries(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] "
            "name=MultiSegmentPrefixSubmitSuffixTranslateFailure "
            "source=1 segments=2 queues=Graphics,Graphics "
            "prefix_translated=true source_submitted=0:0/2 "
            "native_accepted_prefix=1 native_owner=Submission "
            "callbacks=ordinary1_success0 signal=failed "
            "later_callbacks=ordinary1_success0 "
            "hard_latch=later_batch_failed replay=0\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=3 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "expected exactly one marker"
        ):
            runner.validate_log("multi-segment-hard", text)


if __name__ == "__main__":
    unittest.main()
