#!/usr/bin/env python3
"""Merge the missing batch=1 + pool measurements into the ablation plot."""

import csv
import json
import pathlib
import statistics

import matplotlib.pyplot as plt
import numpy as np


EXP = pathlib.Path("/home/cyf/ceph-dpu/ACompressTestTools/Exps/expAblation")
RUN = EXP / "batch1_pool_8clients_20260921"
OLD = EXP / "results/ablation_8clients_measured_20260910.csv"
OUT = EXP / "results/ablation_8clients_measured_batch1_pool_20260921.csv"
STEM = EXP / "figures/ablation_measured_batch1_pool_4bars_20260921"
DATASETS = (
    ("AstronomyLow", "Astronomy\nLow"),
    ("AstronomyHigh", "Astronomy\nHigh"),
    ("LogHub", "LogHub"),
)
LABELS = {
    "Batch=1": "Naive",
    "Batch=1+Pool": "Pool",
    "Batch=32": "Batch",
    "Batch=32+Pool": "Batch+Pool",
}


with (RUN / "batch1_pool_8clients_20260921.csv").open(newline="") as stream:
    run_rows = list(csv.DictReader(stream))
with OLD.open(newline="") as stream:
    old_rows = list(csv.DictReader(stream))

old_by_key = {(row["dataset"], row["legend"]): row for row in old_rows}


def relabel_old_row(dataset, source_label):
    return {
        **old_by_key[(dataset, source_label)],
        "legend": LABELS[source_label],
    }


output_rows = []
summary = {}

for dataset, _ in DATASETS:
    current = [row for row in run_rows if row["dataset"] == dataset]
    on = [float(row["throughput_mib_s"]) for row in current
          if row["input_zero_copy"].lower() == "true"]
    if len(on) != 2:
        raise RuntimeError(f"{dataset}: expected two batch=1 + pool samples")
    on_mean = statistics.mean(on)
    batch1_baseline = float(
        old_by_key[(dataset, "Batch=1")]["throughput_mib_s"]
    )
    summary[dataset] = {
        "batch1_pool_samples_mib_s": on,
        "batch1_baseline_mib_s": batch1_baseline,
        "batch1_pool_mean_mib_s": on_mean,
        "pool_change_percent": (on_mean / batch1_baseline - 1) * 100,
    }
    output_rows.extend((
        relabel_old_row(dataset, "Batch=1"),
        {
            "dataset": dataset,
            "legend": LABELS["Batch=1+Pool"],
            "batch_size": 1,
            "input_zero_copy": "on",
            "throughput_mib_s": f"{on_mean:.2f}",
            "sample_count": 2,
            "aggregation": "arithmetic mean",
            "source": "batch1_pool_8clients_20260921",
            "notes": "input-side zero copy; output copy retained",
        },
        relabel_old_row(dataset, "Batch=32"),
        relabel_old_row(dataset, "Batch=32+Pool"),
    ))

fields = (
    "dataset", "legend", "batch_size", "input_zero_copy",
    "throughput_mib_s", "sample_count", "aggregation", "source", "notes",
)
with OUT.open("w", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    writer.writerows(output_rows)

values = {(row["dataset"], row["legend"]): float(row["throughput_mib_s"])
          for row in output_rows}
plt.rcParams.update({
    "font.size": 9,
    "axes.labelsize": 9,
    "xtick.labelsize": 8,
    "ytick.labelsize": 9,
    "legend.fontsize": 6.5,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})
fig, axis = plt.subplots(figsize=(3.35, 2.7))
positions = np.arange(len(DATASETS))
width = 0.19
styles = (
    ("Naive", "#B9B9B9", ""),
    ("Pool", "#A5D6A7", "//"),
    ("Batch", "#4C78A8", ""),
    ("Batch+Pool", "#F2C14E", "//"),
)
offsets = (-1.5, -0.5, 0.5, 1.5)
for offset, (legend, color, hatch) in zip(offsets, styles):
    axis.bar(
        positions + offset * width,
        [values[(dataset, legend)] for dataset, _ in DATASETS],
        width,
        label=legend,
        color=color,
        edgecolor="black",
        linewidth=0.65,
        hatch=hatch,
    )
axis.set_ylabel("Aggregate Object Read\nThroughput (MiB/s)")
axis.set_xticks(positions, [label for _, label in DATASETS])
axis.set_ylim(0, 5000)
axis.grid(axis="y", linestyle="--", linewidth=0.5, alpha=0.45)
axis.set_axisbelow(True)
axis.spines["top"].set_visible(False)
axis.spines["right"].set_visible(False)
fig.legend(
    frameon=False,
    loc="upper center",
    bbox_to_anchor=(0.5, 0.99),
    ncol=2,
    columnspacing=0.8,
    handlelength=1.3,
    handletextpad=0.35,
)
fig.subplots_adjust(left=0.26, right=0.985, bottom=0.22, top=0.79)
STEM.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(STEM.with_suffix(".png"), dpi=300)
fig.savefig(STEM.with_suffix(".pdf"))
plt.close(fig)

report = {
    "experiment": "batch=1 input registered-memory pool ablation",
    "clients": 8,
    "runs": ["on_1", "on_2"],
    "input_pool_slots_per_osd": 128,
    "summary": summary,
    "csv": str(OUT),
    "figure_png": str(STEM.with_suffix(".png")),
    "figure_pdf": str(STEM.with_suffix(".pdf")),
}
(RUN / "final_ablation_summary.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
