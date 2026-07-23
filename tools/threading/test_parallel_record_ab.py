from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import parallel_record_ab


def profile_line(**overrides: float | int | str) -> str:
    values: dict[str, float | int | str] = {
        "mode": "serial",
        "samples": 100,
        "requested": 0,
        "planned": 0,
        "effective": 0,
        "worker_fallbacks": 0,
        "record_p50_ms": 2.0,
        "record_p95_ms": 3.0,
        "record_p99_ms": 4.0,
        "record_max_ms": 5.0,
        "execute_cpu_p50_ms": 2.1,
        "execute_cpu_p95_ms": 3.1,
        "execute_cpu_p99_ms": 4.1,
        "execute_cpu_max_ms": 5.1,
        "reorder_avg_ms": 0.1,
        "preprocess_avg_ms": 0.2,
        "worker_join_avg_ms": 1.0,
        "submit_cpu_avg_ms": 0.1,
        "jobs_avg": 4.0,
        "work_units_avg": 256.0,
        "ordered_cb_avg": 4.0,
        "max_active": 4,
    }
    values.update(overrides)
    if "planned" not in overrides and "effective" in overrides:
        values["planned"] = values["effective"]
    if "worker_fallbacks" not in overrides:
        values["worker_fallbacks"] = int(values["planned"]) - int(values["effective"])
    fields = " ".join(f"{key}={value}" for key, value in values.items())
    return f"[2026-07-22 12:00:00] [ParallelRecordProfile] {fields}\n"


class ParallelRecordAbTests(unittest.TestCase):
    def write_log(self, root: Path, name: str, *lines: str) -> Path:
        path = root / name
        path.write_text("unrelated log line\n" + "".join(lines), encoding="utf-8")
        return path

    def test_aggregate_skips_each_log_and_weights_by_samples(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = self.write_log(
                root,
                "first.log",
                profile_line(record_p50_ms=9.0, record_p95_ms=9.0, record_p99_ms=9.0, record_max_ms=9.0),
                profile_line(
                    mode="mixed", samples=100, requested=100, effective=96, record_p50_ms=2.0
                ),
            )
            second = self.write_log(
                root,
                "second.log",
                profile_line(record_p50_ms=8.0, record_p95_ms=8.0, record_p99_ms=8.0, record_max_ms=8.0),
                profile_line(
                    mode="mixed", samples=300, requested=300, effective=294, record_p50_ms=1.0
                ),
            )
            aggregate = parallel_record_ab.aggregate_group([first, second], 1)

            self.assertEqual(aggregate["windows"], 2)
            self.assertEqual(aggregate["skipped_windows"], 2)
            self.assertEqual(aggregate["samples"], 400)
            self.assertEqual(aggregate["requested"], 400)
            self.assertEqual(aggregate["planned"], 390)
            self.assertEqual(aggregate["effective"], 390)
            self.assertAlmostEqual(float(aggregate["plan_rate"]), 0.975)
            self.assertAlmostEqual(float(aggregate["worker_success_rate"]), 1.0)
            self.assertAlmostEqual(float(aggregate["effective_rate"]), 0.975)
            self.assertAlmostEqual(float(aggregate["record_p50_ms"]), 1.25)
            self.assertEqual(aggregate["record_max_ms"], 5.0)

    def test_go_requires_all_four_gates(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            serial_log = self.write_log(root, "serial.log", profile_line())
            parallel_log = self.write_log(
                root,
                "parallel.log",
                profile_line(
                    mode="mixed",
                    requested=100,
                    effective=97,
                    record_p50_ms=1.5,
                    record_p95_ms=3.1,
                    record_p99_ms=4.1,
                    execute_cpu_p50_ms=1.5,
                    execute_cpu_p95_ms=3.1,
                    execute_cpu_p99_ms=4.1,
                ),
            )
            serial = parallel_record_ab.aggregate_group([serial_log], 0)
            parallel = parallel_record_ab.aggregate_group([parallel_log], 0)
            report = parallel_record_ab.make_report(
                serial,
                parallel,
                serial_logs=[serial_log],
                parallel_logs=[parallel_log],
                skip_windows=0,
            )

            self.assertEqual(report["verdict"], "GO")
            self.assertTrue(all(check["pass"] for check in report["checks"]))

    def test_experimental_when_absolute_p50_gain_or_tail_gate_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            serial_log = self.write_log(
                root,
                "serial.log",
                profile_line(record_p50_ms=1.0, record_p95_ms=3.0, record_p99_ms=4.0),
            )
            parallel_log = self.write_log(
                root,
                "parallel.log",
                profile_line(
                    mode="parallel",
                    requested=100,
                    effective=100,
                    record_p50_ms=0.84,
                    record_p95_ms=3.21,
                    record_p99_ms=4.21,
                    execute_cpu_p50_ms=1.94,
                    execute_cpu_p95_ms=3.31,
                    execute_cpu_p99_ms=4.31,
                ),
            )
            report = parallel_record_ab.make_report(
                parallel_record_ab.aggregate_group([serial_log], 0),
                parallel_record_ab.aggregate_group([parallel_log], 0),
                serial_logs=[serial_log],
                parallel_logs=[parallel_log],
                skip_windows=0,
            )

            self.assertEqual(report["verdict"], "EXPERIMENTAL")
            failures = {check["name"] for check in report["checks"] if not check["pass"]}
            self.assertEqual(
                failures,
                {
                    "execute_cpu_p50_improvement",
                    "execute_cpu_p95_ms_regression",
                    "execute_cpu_p99_ms_regression",
                },
            )

    def test_no_qualifying_work_is_gated_without_worker_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            serial_log = self.write_log(root, "serial.log", profile_line())
            parallel_log = self.write_log(
                root,
                "parallel.log",
                profile_line(
                    mode="mixed",
                    requested=100,
                    planned=0,
                    effective=0,
                    execute_cpu_p50_ms=2.15,
                    execute_cpu_p95_ms=3.15,
                    execute_cpu_p99_ms=4.15,
                ),
            )
            report = parallel_record_ab.make_report(
                parallel_record_ab.aggregate_group([serial_log], 0),
                parallel_record_ab.aggregate_group([parallel_log], 0),
                serial_logs=[serial_log],
                parallel_logs=[parallel_log],
                skip_windows=0,
            )

            self.assertEqual(report["verdict"], "GATED")
            self.assertEqual(report["parallel"]["plan_rate"], 0.0)
            self.assertIsNone(report["parallel"]["worker_success_rate"])
            worker_check = report["checks"][0]
            self.assertFalse(worker_check["applicable"])
            self.assertTrue(worker_check["pass"])

    def test_execute_cpu_gate_catches_submit_overhead(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            serial_log = self.write_log(root, "serial.log", profile_line())
            parallel_log = self.write_log(
                root,
                "parallel.log",
                profile_line(
                    mode="parallel",
                    requested=100,
                    effective=100,
                    record_p50_ms=1.4,
                    execute_cpu_p50_ms=2.4,
                    submit_cpu_avg_ms=0.8,
                ),
            )
            report = parallel_record_ab.make_report(
                parallel_record_ab.aggregate_group([serial_log], 0),
                parallel_record_ab.aggregate_group([parallel_log], 0),
                serial_logs=[serial_log],
                parallel_logs=[parallel_log],
                skip_windows=0,
            )

            self.assertEqual(report["verdict"], "EXPERIMENTAL")
            failures = {check["name"] for check in report["checks"] if not check["pass"]}
            self.assertIn("execute_cpu_p50_improvement", failures)

    def test_worker_success_is_measured_only_over_planned_submissions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log = self.write_log(
                root,
                "parallel.log",
                profile_line(
                    mode="mixed",
                    requested=100,
                    planned=20,
                    effective=19,
                    worker_fallbacks=1,
                ),
            )
            aggregate = parallel_record_ab.aggregate_group([log], 0)

            self.assertAlmostEqual(float(aggregate["plan_rate"]), 0.2)
            self.assertAlmostEqual(float(aggregate["worker_success_rate"]), 0.95)

    def test_malformed_profile_line_is_not_silently_ignored(self) -> None:
        with self.assertRaisesRegex(parallel_record_ab.ProfileParseError, "missing fields"):
            parallel_record_ab.parse_profile_line(
                "[ParallelRecordProfile] samples=10 requested=10 effective=10",
                "broken.log",
                7,
            )

    def test_mode_must_match_window_counters(self) -> None:
        inconsistent_lines = (
            profile_line(mode="serial", requested=100, planned=0, effective=0),
            profile_line(
                mode="parallel",
                requested=100,
                planned=99,
                effective=99,
            ),
            profile_line(
                mode="mixed",
                requested=100,
                planned=100,
                effective=100,
            ),
        )

        for line_number, line in enumerate(inconsistent_lines, start=1):
            with self.subTest(line_number=line_number):
                with self.assertRaisesRegex(
                    parallel_record_ab.ProfileParseError,
                    "mode|counters",
                ):
                    parallel_record_ab.parse_profile_line(
                        line,
                        "inconsistent.log",
                        line_number,
                    )

    def test_config_sibling_is_ignored(self) -> None:
        self.assertIsNone(
            parallel_record_ab.parse_profile_line(
                "[ParallelRecordProfile][Config] queue=Graphics enabled=true",
                "profile.log",
                1,
            )
        )

    def test_group_purity_rejects_swapped_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            feature_on = self.write_log(
                root,
                "feature_on.log",
                profile_line(mode="parallel", requested=100, effective=100),
            )
            aggregate = parallel_record_ab.aggregate_group([feature_on], 0)
            with self.assertRaisesRegex(
                parallel_record_ab.ProfileParseError, "pure serial baseline"
            ):
                parallel_record_ab.validate_group_purity(aggregate, "serial")

    def test_main_writes_json_and_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            serial_log = self.write_log(root, "serial.log", profile_line())
            serial_log_2 = self.write_log(root, "serial_2.log", profile_line())
            parallel_log = self.write_log(
                root,
                "parallel.log",
                profile_line(
                    mode="parallel",
                    requested=100,
                    effective=100,
                    record_p50_ms=1.5,
                    execute_cpu_p50_ms=1.5,
                ),
            )
            parallel_log_2 = self.write_log(
                root,
                "parallel_2.log",
                profile_line(
                    mode="parallel",
                    requested=100,
                    effective=100,
                    record_p50_ms=1.5,
                    execute_cpu_p50_ms=1.5,
                ),
            )
            outdir = root / "report"

            exit_code = parallel_record_ab.main(
                [
                    "--serial-log",
                    str(serial_log),
                    "--serial-log",
                    str(serial_log_2),
                    "--parallel-log",
                    str(parallel_log),
                    "--parallel-log",
                    str(parallel_log_2),
                    "--outdir",
                    str(outdir),
                ]
            )

            self.assertEqual(exit_code, 0)
            report = json.loads((outdir / "report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["verdict"], "GO")
            self.assertIn("**Verdict: GO**", (outdir / "report.md").read_text(encoding="utf-8"))

    def test_main_requires_distinct_runs_on_both_sides(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            serial_log = self.write_log(root, "serial.log", profile_line())
            parallel_log = self.write_log(
                root,
                "parallel.log",
                profile_line(mode="parallel", requested=100, effective=100),
            )

            exit_code = parallel_record_ab.main(
                [
                    "--serial-log",
                    str(serial_log),
                    "--parallel-log",
                    str(parallel_log),
                    "--outdir",
                    str(root / "report"),
                ]
            )

            self.assertEqual(exit_code, 2)


if __name__ == "__main__":
    unittest.main()
