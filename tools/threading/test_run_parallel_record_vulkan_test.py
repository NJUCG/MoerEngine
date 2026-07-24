from __future__ import annotations

import unittest

import run_parallel_record_vulkan_test as runner


def pass_line(
    mode: str,
    fault: str,
    production_gate: str = "false",
    production_heavy: str = "false",
) -> str:
    return (
        "[TESTCASE][PASS] name=ParallelRecordOrderedReadback "
        f"mode={mode} worker_fault={fault} production_gate={production_gate} "
        f"production_heavy={production_heavy} iterations=24\n"
        "[TESTCASE][PASS] name=AsyncQueueParallelTranslateSmoke "
        "sources=3 batch=1 source_order=G,G,C first_ready_lanes=G,C "
        "observed_submit_order=G,G,C explicit_state=true "
        "callbacks=exactly_once\n"
        "[TESTCASE][PASS] name=ParallelTranslateRecoverableRejection "
        "distinct_native=true suffix_recorded=true signals=rejected "
        "callbacks=exactly_once runtime=recovered same_scope_reentry=Compute\n"
    )


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

    def test_regular_mode_requires_parallel_translate_contracts(self) -> None:
        text = (
            "[TESTCASE][PASS] name=ParallelRecordOrderedReadback "
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
            "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=true suffix_recorded=true signals=failed "
            "callbacks=exactly_once hard_latch=later_batch_failed\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        runner.validate_log("translate-hard", text)

    def test_translate_hard_rejects_native_submit_after_fault(self) -> None:
        text = (
            "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
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
            "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
            "distinct_native=false suffix_recorded=false signals=failed "
            "callbacks=exactly_once hard_latch=later_batch_failed\n"
            "[VulkanFault][Summary] first_fault_count=1 "
            "native_submit_after_fault=0 native_present_after_fault=0 "
            "device_lost=false\n"
        )
        runner.validate_log("translate-hard", text)

    def test_translate_hard_rejects_multi_digit_fault_count_prefix(self) -> None:
        text = (
            "[TESTCASE][PASS] name=ParallelTranslateFailureRetirement "
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


if __name__ == "__main__":
    unittest.main()
