#!/usr/bin/env python3
"""Capture or diff custom DPU BlueStore counters for OSDs 0 through 11."""

import argparse
import datetime
import json
import pathlib
import subprocess


ROOT = pathlib.Path("/home/cyf/ceph-dpu")
CEPH = ROOT / "build/bin/ceph"
OSDS = tuple(range(12))
COUNTERS = (
    "dpu_batches",
    "dpu_batch_blobs",
    "dpu_submitted_blobs",
    "dpu_input_pool_in_use",
    "dpu_input_pool_hits",
    "dpu_input_pool_misses",
    "dpu_read_into_bytes",
    "dpu_zero_copy_blobs",
    "dpu_input_copy_bytes",
    "dpu_input_copy_calls",
    "dpu_output_copy_bytes",
    "dpu_output_copy_calls",
    "dpu_submit_errors",
    "dpu_task_errors",
    "dpu_cpu_fallback_blobs",
)


def capture():
    osds = {}
    for osd_id in OSDS:
        raw = subprocess.check_output(
            [str(CEPH), "tell", f"osd.{osd_id}", "perf", "dump"],
            cwd=ROOT,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        blue = json.loads(raw)["bluestore"]
        osds[str(osd_id)] = {key: blue.get(key, 0) for key in COUNTERS}
    return {
        "captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "osds": osds,
        "totals": {
            key: sum(osds[str(osd_id)][key] for osd_id in OSDS)
            for key in COUNTERS
        },
    }


def delta(before, after):
    osds = {}
    for osd_id in OSDS:
        oid = str(osd_id)
        osds[oid] = {
            key: after["osds"][oid][key] - before["osds"][oid][key]
            for key in COUNTERS
        }
    return {
        "before": before["captured_at"],
        "after": after["captured_at"],
        "osds": osds,
        "totals": {
            key: sum(osds[str(osd_id)][key] for osd_id in OSDS)
            for key in COUNTERS
        },
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=pathlib.Path)
    parser.add_argument("--after", type=pathlib.Path)
    args = parser.parse_args()
    current = json.loads(args.after.read_text()) if args.after else capture()
    if args.before:
        current = delta(json.loads(args.before.read_text()), current)
    print(json.dumps(current, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
