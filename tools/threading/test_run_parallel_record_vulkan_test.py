from __future__ import annotations

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


def present_boundary_line(bridge: str = "required") -> str:
    prefix_native = "2" if bridge == "required" else "0"
    return (
        serial_control_boundary_line()
        + "[TESTCASE][PASS] name=PresentPipelineBoundary "
        "outcome=Recreate order=Prefix,Bridge?,Present,Later "
        "owner=Submission receipt_attempts=1 submitted=false "
        "recreate=true completion=drained later_batch=success "
        f"present_only=verified bridge={bridge} graphics_native=0 "
        f"prefix_native={prefix_native} readback=verified replay=0\n"
        + queued_present_shutdown_line()
    )


def present_hard_line() -> str:
    return (
        "[TESTCASE][PASS] name=PresentHardFailureBoundary "
        "outcome=Rejected owner=Submission receipt_attempts=1 "
        "submitted=false recreate=false later_batch=rejected "
        "native_after_present=0 hard_latch=verified replay=0\n"
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


class VulkanRunnerTests(unittest.TestCase):
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
