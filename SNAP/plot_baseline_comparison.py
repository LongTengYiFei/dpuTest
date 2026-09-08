#!/usr/bin/env python3
"""Plot Baseline 1 (direct SPDK) versus Baseline 2 (SPDK through SNAP)."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


@dataclass(frozen=True)
class Case:
    workload: str
    block_size: int
    queue_depth: int
    label: str


@dataclass(frozen=True)
class Result:
    iops: float
    mib_s: float
    avg_us: float
    p99_us: float


CASES = (
    Case("randread", 4096, 1, "4K random read\nQD1"),
    Case("randread", 4096, 64, "4K random read\nQD64"),
    Case("randwrite", 4096, 64, "4K random write\nQD64"),
    Case("read", 131072, 32, "128K sequential read\nQD32"),
    Case("write", 131072, 32, "128K sequential write\nQD32"),
)

LATENCY_LABELS = (
    "4K RR\nQD1",
    "4K RR\nQD64",
    "4K RW\nQD64",
    "128K SR\nQD32",
    "128K SW\nQD32",
)

TOTAL_RE = re.compile(
    r"^Total\s*:\s*"
    r"(?P<iops>[0-9.]+)\s+"
    r"(?P<mib>[0-9.]+)\s+"
    r"(?P<avg>[0-9.]+)\s+",
    re.MULTILINE,
)
P99_RE = re.compile(r"^\s*99\.00000%\s*:\s*(?P<p99>[0-9.]+)us", re.MULTILINE)


def find_log(results_dir: Path, baseline: str, case: Case) -> Path:
    pattern = (
        f"{baseline}-{case.workload}-{case.block_size}B-"
        f"qd{case.queue_depth}-*.log"
    )
    matches = sorted(results_dir.glob(pattern))
    if not matches:
        raise FileNotFoundError(f"No result log matches {results_dir / pattern}")
    # A workload may have smoke-test attempts.  The latest timestamp is the
    # result represented in RESULTS.md.
    return matches[-1]


def parse_log(path: Path) -> Result:
    text = path.read_text(encoding="utf-8", errors="replace")
    total = TOTAL_RE.search(text)
    p99 = P99_RE.search(text)
    if total is None or p99 is None:
        raise ValueError(f"Could not parse performance data from {path}")
    return Result(
        iops=float(total.group("iops")),
        mib_s=float(total.group("mib")),
        avg_us=float(total.group("avg")),
        p99_us=float(p99.group("p99")),
    )


def add_grouped_bars(
    ax: plt.Axes,
    labels: list[str],
    baseline1: list[float],
    baseline2: list[float],
    ylabel: str,
    title: str,
    formatter,
    log_scale: bool = False,
) -> None:
    x = np.arange(len(labels))
    width = 0.36
    bars1 = ax.bar(
        x - width / 2,
        baseline1,
        width,
        label="Baseline 1: direct SPDK",
        color="#2878B5",
    )
    bars2 = ax.bar(
        x + width / 2,
        baseline2,
        width,
        label="Baseline 2: SPDK through SNAP/DPA",
        color="#F28E2B",
    )
    if log_scale:
        ax.set_yscale("log")
    ax.set_title(title, fontweight="bold")
    ax.set_ylabel(ylabel)
    ax.set_xticks(x, labels)
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    ax.set_axisbelow(True)
    ax.bar_label(bars1, labels=[formatter(v) for v in baseline1], padding=3, fontsize=8)
    ax.bar_label(bars2, labels=[formatter(v) for v in baseline2], padding=3, fontsize=8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    script_dir = Path(__file__).resolve().parent
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=script_dir / "results",
        help="Directory containing baseline result logs",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=script_dir / "results" / "baseline1-vs-baseline2.png",
        help="Output PNG path",
    )
    args = parser.parse_args()

    data: dict[str, list[Result]] = {}
    sources: dict[str, list[Path]] = {}
    for baseline in ("baseline1", "baseline2"):
        sources[baseline] = [find_log(args.results_dir, baseline, case) for case in CASES]
        data[baseline] = [parse_log(path) for path in sources[baseline]]

    plt.rcParams.update(
        {
            "font.size": 10,
            "axes.titlepad": 10,
            "figure.facecolor": "white",
            "axes.facecolor": "#FAFAFA",
        }
    )
    fig, axes = plt.subplots(2, 2, figsize=(16, 10), constrained_layout=True)
    fig.suptitle(
        "SPDK Storage Path Comparison: Direct vs SNAP/DPA\n"
        "Single 30-second run, node4 to node3 P5530",
        fontsize=17,
        fontweight="bold",
    )

    short_cases = CASES[:3]
    large_cases = CASES[3:]
    add_grouped_bars(
        axes[0, 0],
        [case.label for case in short_cases],
        [r.iops for r in data["baseline1"][:3]],
        [r.iops for r in data["baseline2"][:3]],
        "IOPS",
        "4K random I/O",
        lambda value: f"{value / 1000:.1f}K",
    )
    add_grouped_bars(
        axes[0, 1],
        [case.label for case in large_cases],
        [r.mib_s for r in data["baseline1"][3:]],
        [r.mib_s for r in data["baseline2"][3:]],
        "Throughput (MiB/s)",
        "128K sequential bandwidth",
        lambda value: f"{value:,.0f}",
    )
    add_grouped_bars(
        axes[1, 0],
        list(LATENCY_LABELS),
        [r.avg_us for r in data["baseline1"]],
        [r.avg_us for r in data["baseline2"]],
        "Latency (us, log scale)",
        "Average latency",
        lambda value: f"{value:,.1f}",
        log_scale=True,
    )
    add_grouped_bars(
        axes[1, 1],
        list(LATENCY_LABELS),
        [r.p99_us for r in data["baseline1"]],
        [r.p99_us for r in data["baseline2"]],
        "Latency (us, log scale)",
        "P99 latency",
        lambda value: f"{value:,.1f}",
        log_scale=True,
    )

    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="lower center",
        bbox_to_anchor=(0.5, -0.035),
        ncol=2,
        frameon=False,
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=200, bbox_inches="tight")
    svg_path = args.output.with_suffix(".svg")
    fig.savefig(svg_path, bbox_inches="tight")
    plt.close(fig)

    print(f"PNG: {args.output}")
    print(f"SVG: {svg_path}")
    for baseline in ("baseline1", "baseline2"):
        print(f"{baseline} sources:")
        for path in sources[baseline]:
            print(f"  {path}")


if __name__ == "__main__":
    main()
