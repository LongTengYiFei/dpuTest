#!/usr/bin/env bash
set -euo pipefail

root=/home/cyf/ceph-dpu
ceph=$root/build/bin/ceph
rados=$root/build/bin/rados
write_runner=$root/ACompressTestTools/run.sh
read_runner=$root/ACompressTestTools/run_read.sh
tools=/home/cyf/dpuTest/ceph_custom_deploy
restart=$tools/restart_custom_osds_current.sh
snapshot=$tools/snapshot_dpu_perf_current.py
report=$tools/build_batch1_pool_report.py
result_root=$root/ACompressTestTools/Exps/expAblation/batch1_pool_8clients_20260921

mkdir -p "$result_root"

datasets=(
  "AstronomyLow:/home/cyf/datasetsSmartCE/AstroLow"
  "AstronomyHigh:/home/cyf/datasetsSmartCE/AstronomyHigh/data"
  "LogHub:/home/cyf/datasetsSmartCE/LogHub"
)

restore_defaults() {
  echo "restoring batch=32, input_zero_copy=false"
  $ceph config set osd bluestore_dpu_decompression_batch_size 32 --force || true
  $ceph config set osd bluestore_dpu_input_zero_copy false --force || true
  $ceph config set osd bluestore_dpu_input_pool_slots 128 --force || true
  "$restart" >"$result_root/final_restore.log" 2>&1 || true
}
trap restore_defaults EXIT

write_manifest() {
  local name=$1
  local data_dir=$2
  local target=$3
  python3 - "$name" "$data_dir" "$target" <<'PY'
import json, math, os, pathlib, sys
name, data_dir, target = sys.argv[1:]
root = pathlib.Path(data_dir)
files = sorted(p for p in root.rglob("*") if p.is_file())
logical = sum(p.stat().st_size for p in files)
objects = sum(math.ceil(p.stat().st_size / (4 * 1024 * 1024)) for p in files)
manifest = {
    "dataset": name,
    "data_dir": str(root),
    "files": len(files),
    "logical_bytes": logical,
    "expected_objects": objects,
    "chunk_bytes": 4 * 1024 * 1024,
}
pathlib.Path(target).write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
PY
}

verify_effective_config() {
  local expected_zero_copy=$1
  local output=$2

  : >"$output"
  for id in $(seq 0 11); do
    for key in bluestore_dpu_decompression_batch_size \
               bluestore_dpu_input_zero_copy \
               bluestore_dpu_input_pool_slots; do
      printf 'osd.%s %s=' "$id" "$key" >>"$output"
      $ceph tell "osd.$id" config get "$key" \
        >>"$output" 2>/dev/null
    done
  done

  python3 - "$output" "$expected_zero_copy" <<'PY'
import json
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
expected_zero_copy = sys.argv[2]
text = path.read_text()
expected = {
    "bluestore_dpu_decompression_batch_size": "1",
    "bluestore_dpu_input_zero_copy": expected_zero_copy,
    "bluestore_dpu_input_pool_slots": "128",
}
seen = {}
for match in re.finditer(r"osd\.(\d+) (\S+)=({.*?\n})", text, re.S):
    osd, key, payload = match.groups()
    seen[(int(osd), key)] = str(json.loads(payload)[key]).lower()

errors = []
for osd in range(12):
    for key, wanted in expected.items():
        got = seen.get((osd, key))
        if got != wanted:
            errors.append(f"osd.{osd} {key}: expected={wanted}, got={got}")
if errors:
    print("effective OSD configuration mismatch:", file=sys.stderr)
    print("\n".join(errors), file=sys.stderr)
    raise SystemExit(1)
print("verified effective configuration on all 12 OSDs")
PY
}

run_case() {
  local dataset_dir=$1
  local mode=$2
  local sample=$3
  local enabled=false
  [[ $mode == on ]] && enabled=true
  local label=${mode}_${sample}
  local case_dir=$dataset_dir/$label
  mkdir -p "$case_dir"

  if [[ -f $case_dir/perf_delta.json ]] && grep -q '测试完成' "$case_dir/read.log" 2>/dev/null; then
    echo "[$label] already complete; skipping"
    return
  fi

  echo "[$label] batch=1 input_zero_copy=$enabled"
  $ceph config set osd bluestore_dpu_decompression_batch_size 1 --force
  $ceph config set osd bluestore_dpu_input_zero_copy "$enabled" --force
  $ceph config set osd bluestore_dpu_input_pool_slots 128 --force
  "$restart" | tee "$case_dir/restart.log"

  $ceph -s --format json-pretty >"$case_dir/cluster_before.json" 2>/dev/null
  {
    $ceph osd pool get po1 compression_algorithm
    $ceph osd pool get po1 compression_mode
    $ceph osd pool get po1 compression_min_blob_size || true
    $ceph osd pool get po1 compression_max_blob_size || true
  } >"$case_dir/pool_config.txt" 2>/dev/null

  verify_effective_config "$enabled" "$case_dir/effective_config.txt"

  "$snapshot" >"$case_dir/perf_before.json"
  /usr/bin/time -v env PYTHONUNBUFFERED=1 "$read_runner" \
    --clients 8 --skip-raw 2>&1 | tee "$case_dir/read.log"
  "$snapshot" >"$case_dir/perf_after.json"
  "$snapshot" --before "$case_dir/perf_before.json" \
    --after "$case_dir/perf_after.json" >"$case_dir/perf_delta.json"

  python3 - "$case_dir/perf_delta.json" "$enabled" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))["totals"]
enabled = sys.argv[2] == "true"
assert d["dpu_submitted_blobs"] > 0, d
assert d["dpu_submit_errors"] == 0, d
assert d["dpu_task_errors"] == 0, d
assert d["dpu_cpu_fallback_blobs"] == 0, d
if enabled:
    assert d["dpu_zero_copy_blobs"] > 0, d
    assert d["dpu_input_copy_bytes"] == 0, d
    assert d["dpu_input_pool_misses"] == 0, d
else:
    assert d["dpu_zero_copy_blobs"] == 0, d
    assert d["dpu_input_copy_bytes"] > 0, d
print(json.dumps(d, indent=2))
PY
  "$report" "$result_root" >"$result_root/checkpoint.json"
  echo "[$label] complete"
}

for entry in "${datasets[@]}"; do
  name=${entry%%:*}
  data_dir=${entry#*:}
  dataset_dir=$result_root/$name
  mkdir -p "$dataset_dir"
  write_manifest "$name" "$data_dir" "$dataset_dir/manifest.json"

  expected=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["expected_objects"])' "$dataset_dir/manifest.json")
  actual=0
  if $ceph osd pool get po1 compression_mode >/dev/null 2>&1; then
    actual=$($rados -p po1 ls | wc -l)
  fi
  if [[ -f $dataset_dir/load.log ]] && \
     grep -q '测试完成' "$dataset_dir/load.log" && \
     [[ $actual -eq $expected ]]; then
    echo "===== reusing complete $name dataset: $actual objects ====="
  else
    echo "===== loading $name from $data_dir ====="
    /usr/bin/time -v env PYTHONUNBUFFERED=1 "$write_runner" \
      --data-dir "$data_dir" --skip-raw 2>&1 | tee "$dataset_dir/load.log"
    actual=$($rados -p po1 ls | wc -l)
  fi

  $ceph osd pool set po1 pg_autoscale_mode off
  $ceph osd pool set po1 compression_algorithm zlib-dpu
  $ceph osd pool application enable po1 rados || true

  if [[ $actual -ne $expected ]]; then
    echo "$name object count mismatch: expected=$expected actual=$actual" >&2
    exit 1
  fi
  echo "$actual" >"$dataset_dir/object_count.txt"

  run_case "$dataset_dir" on 1
  run_case "$dataset_dir" on 2
  "$report" "$result_root" >"$result_root/checkpoint.json"
done

trap - EXIT
restore_defaults
"$report" "$result_root" >"$result_root/final_report.json"
echo "batch=1 pool ablation complete"
