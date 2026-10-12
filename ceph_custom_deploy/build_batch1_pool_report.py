#!/usr/bin/env python3
"""Build checkpointed summaries for the missing batch=1 + pool runs."""

import csv
import json
import pathlib
import re
import statistics
import sys


root = pathlib.Path(sys.argv[1])
datasets = ("AstronomyLow", "AstronomyHigh", "LogHub")
labels = ("on_1", "on_2")
rows = []

for dataset in datasets:
    dataset_dir = root / dataset
    if not (dataset_dir / "manifest.json").exists():
        continue
    manifest = json.loads((dataset_dir / "manifest.json").read_text())
    dataset_rows = []
    for label in labels:
        case_dir = dataset_dir / label
        read_log = case_dir / "read.log"
        delta_path = case_dir / "perf_delta.json"
        if not read_log.exists() or not delta_path.exists():
            continue
        text = read_log.read_text(errors="replace")
        rates = re.findall(r"聚合吞吐\s+([0-9.]+)MB/s", text)
        times = re.findall(r"=> 最晚结束\s+([0-9.]+)s", text)
        if not rates or not times or "测试完成" not in text:
            continue
        counters = json.loads(delta_path.read_text())["totals"]
        row = {
            "dataset": dataset,
            "case": label,
            "input_zero_copy": label.startswith("on_"),
            "batch_size": 1,
            "clients": 8,
            "throughput_mib_s": float(rates[-1]),
            "read_seconds": float(times[-1]),
            "logical_bytes": manifest["logical_bytes"],
            **counters,
        }
        rows.append(row)
        dataset_rows.append(row)

    if dataset_rows:
        on = [r["throughput_mib_s"] for r in dataset_rows if r["input_zero_copy"]]
        summary = {
            "dataset": dataset,
            "runs": dataset_rows,
            "on_mean_mib_s": statistics.mean(on) if on else None,
        }
        (dataset_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

if rows:
    fields = list(rows[0])
    with (root / "batch1_pool_8clients_20260921.csv").open("w", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

print(json.dumps({"complete_runs": len(rows), "rows": rows}, indent=2))
