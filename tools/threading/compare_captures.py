from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

PREFERRED_CAPTURE = "01_maximized.bmp"


def load_pillow() -> None:
    global Image, ImageChops, UnidentifiedImageError
    try:
        from PIL import Image as pillow_image
        from PIL import ImageChops as pillow_image_chops
        from PIL import UnidentifiedImageError as pillow_unidentified_image_error
    except ImportError as error:
        raise ValueError(
            "Pillow is required to compare captures; install the 'Pillow' package"
        ) from error
    Image = pillow_image
    ImageChops = pillow_image_chops
    UnidentifiedImageError = pillow_unidentified_image_error


@dataclass(frozen=True)
class CapturePair:
    left: Path
    right: Path
    selection: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare two RGB captures without NumPy. Inputs may be BMP files or "
            "scenario output directories; directory pairs use the same relative "
            "capture name and prefer 01_maximized.bmp. Exit 0 means all enabled "
            "thresholds passed, 1 means a threshold failed, and 2 means a tool error."
        )
    )
    parser.add_argument("left", type=Path, help="Left scenario directory or BMP file")
    parser.add_argument("right", type=Path, help="Right scenario directory or BMP file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        required=True,
        help="JSON report path",
    )
    parser.add_argument(
        "--max-mean-absolute-error",
        type=float,
        help="Maximum allowed mean absolute RGB channel error (0..255)",
    )
    parser.add_argument(
        "--max-rmse",
        type=float,
        help="Maximum allowed RGB channel RMSE (0..255)",
    )
    parser.add_argument(
        "--max-p99",
        type=float,
        help="Maximum allowed 99th-percentile absolute RGB channel error (0..255)",
    )
    parser.add_argument(
        "--max-error",
        type=float,
        help="Maximum allowed absolute RGB channel error (0..255)",
    )
    parser.add_argument(
        "--min-exact-ratio",
        type=float,
        help="Minimum fraction of pixels whose RGB values match exactly (0..1)",
    )
    parser.add_argument(
        "--min-all-channels-le-2-ratio",
        type=float,
        help="Minimum fraction of pixels with every RGB channel error <= 2 (0..1)",
    )
    args = parser.parse_args()
    validate_threshold_args(parser, args)
    return args


def validate_threshold_args(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    error_limits = (
        ("--max-mean-absolute-error", args.max_mean_absolute_error),
        ("--max-rmse", args.max_rmse),
        ("--max-p99", args.max_p99),
        ("--max-error", args.max_error),
    )
    ratio_limits = (
        ("--min-exact-ratio", args.min_exact_ratio),
        ("--min-all-channels-le-2-ratio", args.min_all_channels_le_2_ratio),
    )
    for option, value in error_limits:
        if value is not None and (not math.isfinite(value) or not 0.0 <= value <= 255.0):
            parser.error(f"{option} must be a finite value in [0, 255]")
    for option, value in ratio_limits:
        if value is not None and (not math.isfinite(value) or not 0.0 <= value <= 1.0):
            parser.error(f"{option} must be a finite value in [0, 1]")


def require_input(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.exists():
        raise ValueError(f"Input does not exist: {resolved}")
    if not (resolved.is_file() or resolved.is_dir()):
        raise ValueError(f"Input is neither a file nor a directory: {resolved}")
    if resolved.is_file() and resolved.suffix.casefold() != ".bmp":
        raise ValueError(f"Capture file must use the .bmp extension: {resolved}")
    return resolved


def iter_bmps(root: Path) -> list[Path]:
    return sorted(
        (
            path
            for path in root.rglob("*")
            if path.is_file() and path.suffix.casefold() == ".bmp"
        ),
        key=lambda path: path.relative_to(root).as_posix().casefold(),
    )


def collect_bmps(root: Path) -> dict[str, tuple[str, Path]]:
    captures: dict[str, tuple[str, Path]] = {}
    for path in iter_bmps(root):
        relative = path.relative_to(root).as_posix()
        key = relative.casefold()
        if key in captures:
            raise ValueError(
                f"Scenario directory contains case-colliding capture paths: "
                f"{captures[key][0]!r} and {relative!r}"
            )
        captures[key] = (relative, path)
    if not captures:
        raise ValueError(f"Scenario directory contains no BMP captures: {root}")
    return captures


def capture_priority(relative: str) -> tuple[int, str]:
    normalized = relative.casefold()
    basename = Path(relative).name.casefold()
    if normalized == PREFERRED_CAPTURE.casefold():
        priority = 0
    elif basename == PREFERRED_CAPTURE.casefold():
        priority = 1
    else:
        priority = 2
    return priority, normalized


def find_named_capture(root: Path, filename: str) -> Path:
    direct = root / filename
    if direct.is_file():
        return direct
    matches = [path for path in iter_bmps(root) if path.name.casefold() == filename.casefold()]
    if not matches:
        raise ValueError(f"No capture named {filename!r} exists under {root}")
    if len(matches) > 1:
        relative_matches = ", ".join(
            repr(path.relative_to(root).as_posix()) for path in matches
        )
        raise ValueError(
            f"Capture name {filename!r} is ambiguous under {root}: {relative_matches}"
        )
    return matches[0]


def select_capture_pair(left: Path, right: Path) -> CapturePair:
    if left.is_file() and right.is_file():
        return CapturePair(left, right, "explicit BMP files")
    if left.is_file():
        return CapturePair(
            left,
            find_named_capture(right, left.name),
            f"matched explicit capture name {left.name!r}",
        )
    if right.is_file():
        return CapturePair(
            find_named_capture(left, right.name),
            right,
            f"matched explicit capture name {right.name!r}",
        )

    left_captures = collect_bmps(left)
    right_captures = collect_bmps(right)
    common_keys = set(left_captures).intersection(right_captures)
    if not common_keys:
        raise ValueError(
            "Scenario directories have no BMP capture with the same relative path: "
            f"{left} and {right}"
        )
    selected_key = min(common_keys, key=lambda key: capture_priority(left_captures[key][0]))
    relative, left_capture = left_captures[selected_key]
    _, right_capture = right_captures[selected_key]
    return CapturePair(left_capture, right_capture, f"common relative capture {relative!r}")


def percentile_from_histogram(histogram: list[int], percentile: float) -> int:
    sample_count = sum(histogram)
    if sample_count <= 0:
        raise ValueError("Cannot compute a percentile from an empty image")
    rank = max(1, math.ceil(percentile * sample_count))
    cumulative = 0
    for value, count in enumerate(histogram):
        cumulative += count
        if cumulative >= rank:
            return value
    return len(histogram) - 1


def compare_images(left_path: Path, right_path: Path) -> dict[str, object]:
    try:
        with Image.open(left_path) as left_source, Image.open(right_path) as right_source:
            left = left_source.convert("RGB")
            right = right_source.convert("RGB")
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError) as error:
        raise ValueError(f"Failed to read a capture: {error}") from error

    if left.size != right.size:
        raise ValueError(
            f"Capture dimensions differ: {left_path} is {left.width}x{left.height}, "
            f"{right_path} is {right.width}x{right.height}"
        )

    pixel_count = left.width * left.height
    if pixel_count == 0:
        raise ValueError("Captures have zero pixels")

    difference = ImageChops.difference(left, right)
    histogram = difference.histogram()
    channel_sample_count = pixel_count * 3
    absolute_sum = sum((index % 256) * count for index, count in enumerate(histogram))
    squared_sum = sum((index % 256) ** 2 * count for index, count in enumerate(histogram))
    combined_histogram = [
        histogram[value] + histogram[256 + value] + histogram[512 + value]
        for value in range(256)
    ]
    max_error = max(value for value, count in enumerate(combined_histogram) if count)

    exact_pixels = 0
    all_channels_le_2_pixels = 0
    for red, green, blue in difference.getdata():
        exact_pixels += int(red == 0 and green == 0 and blue == 0)
        all_channels_le_2_pixels += int(red <= 2 and green <= 2 and blue <= 2)

    return {
        "width": left.width,
        "height": left.height,
        "pixel_count": pixel_count,
        "rgb_channel_sample_count": channel_sample_count,
        "mean_absolute_error": absolute_sum / channel_sample_count,
        "rmse": math.sqrt(squared_sum / channel_sample_count),
        "p99_absolute_error": percentile_from_histogram(combined_histogram, 0.99),
        "max_absolute_error": max_error,
        "exact_pixel_ratio": exact_pixels / pixel_count,
        "all_channels_le_2_pixel_ratio": all_channels_le_2_pixels / pixel_count,
    }


def evaluate_thresholds(
    metrics: dict[str, object], args: argparse.Namespace
) -> list[dict[str, object]]:
    definitions: tuple[tuple[str, str, float | None, Callable[[float, float], bool]], ...] = (
        (
            "mean_absolute_error",
            "<=",
            args.max_mean_absolute_error,
            lambda actual, limit: actual <= limit,
        ),
        ("rmse", "<=", args.max_rmse, lambda actual, limit: actual <= limit),
        (
            "p99_absolute_error",
            "<=",
            args.max_p99,
            lambda actual, limit: actual <= limit,
        ),
        (
            "max_absolute_error",
            "<=",
            args.max_error,
            lambda actual, limit: actual <= limit,
        ),
        (
            "exact_pixel_ratio",
            ">=",
            args.min_exact_ratio,
            lambda actual, limit: actual >= limit,
        ),
        (
            "all_channels_le_2_pixel_ratio",
            ">=",
            args.min_all_channels_le_2_ratio,
            lambda actual, limit: actual >= limit,
        ),
    )
    checks: list[dict[str, object]] = []
    for metric, operator, threshold, predicate in definitions:
        if threshold is None:
            continue
        actual = float(metrics[metric])
        checks.append(
            {
                "metric": metric,
                "operator": operator,
                "threshold": threshold,
                "actual": actual,
                "passed": predicate(actual, threshold),
            }
        )
    return checks


def write_report(path: Path, report: dict[str, object]) -> None:
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    report: dict[str, object] = {
        "schema_version": 1,
        "inputs": {"left": str(args.left), "right": str(args.right)},
    }
    try:
        load_pillow()
        left_input = require_input(args.left)
        right_input = require_input(args.right)
        pair = select_capture_pair(left_input, right_input)
        metrics = compare_images(pair.left, pair.right)
        checks = evaluate_thresholds(metrics, args)
        success = all(bool(check["passed"]) for check in checks)
        report.update(
            {
                "captures": {
                    "left": str(pair.left),
                    "right": str(pair.right),
                    "selection": pair.selection,
                },
                "metrics": metrics,
                "checks": checks,
                "success": success,
            }
        )
        write_report(args.output, report)
        print(json.dumps(report, indent=2))
        return 0 if success else 1
    except (OSError, ValueError) as error:
        report.update({"success": False, "error": str(error)})
        try:
            write_report(args.output, report)
        except OSError as report_error:
            report["report_write_error"] = str(report_error)
        print(json.dumps(report, indent=2), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
