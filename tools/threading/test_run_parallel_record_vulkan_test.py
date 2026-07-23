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


if __name__ == "__main__":
    unittest.main()
