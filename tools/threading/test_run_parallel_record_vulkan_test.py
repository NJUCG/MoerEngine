from __future__ import annotations

import unittest

import run_parallel_record_vulkan_test as runner


def completion_probe_line() -> str:
    return (
        "[TESTCASE][PASS] name=MultiSegmentCompletionAggregateCpuProbe "
        "suffix_first=deferred prefix_second=ordinary1_success0 replay=0\n"
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
        "sources=3 batch=1 source_order=G,G,C first_ready_lanes=G,C "
        "observed_submit_order=G,G,C explicit_state=true "
        "callbacks=exactly_once\n"
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
        text = pass_line("serial", "false").replace(
            "[TESTCASE][PASS] name=AsyncQueueParallelTranslateSmoke "
            "sources=3 batch=1 source_order=G,G,C first_ready_lanes=G,C "
            "observed_submit_order=G,G,C explicit_state=true "
            "callbacks=exactly_once",
            "[TESTCASE][SKIP] name=AsyncQueueParallelTranslateSmoke "
            "reason=test_disabled cpu_seam=VulkanTranslateWaveScheduler",
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "invalid ready-native-lane"
        ):
            runner.validate_log("serial", text)

    def test_ready_translate_requires_observed_submit_order(self) -> None:
        text = pass_line("serial", "false").replace(
            "observed_submit_order=G,G,C", "observed_submit_order=G,C,G"
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
            "reason=native_queue_alias "
            "cpu_seam=VulkanTranslateWaveScheduler\n"
        )
        with self.assertRaisesRegex(
            runner.VulkanTestError, "at most one terminal marker"
        ):
            runner.validate_log("serial", text + duplicate)

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
            "distinct_native=true suffix_recorded=true signals=failed "
            "callbacks=exactly_once hard_latch=later_batch_failed\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        runner.validate_log("translate-hard", text)

    def test_translate_hard_rejects_native_submit_after_fault(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true signals=failed "
            "callbacks=exactly_once hard_latch=later_batch_failed\n"
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
            "distinct_native=false suffix_recorded=false signals=failed "
            "callbacks=exactly_once hard_latch=later_batch_failed\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        runner.validate_log("translate-hard", text)

    def test_translate_hard_rejects_multi_digit_fault_count_prefix(self) -> None:
        text = (
            completion_probe_line()
            + "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true signals=failed "
            "callbacks=exactly_once hard_latch=later_batch_failed\n"
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


if __name__ == "__main__":
    unittest.main()
