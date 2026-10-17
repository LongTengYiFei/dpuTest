#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CEPH_BIN="/home/cyf/githubDownload/ceph/build/bin"
POOL="${POOL:-ceph-raw-baseline}"
DATASET="${DATASET:-/home/cyf/raid0/LogHub/Android_v1}"
RAW_PREFIX="${RAW_PREFIX:-android-v1-4m-raw}"
ROUNDS="${ROUNDS:-5}"
RESULT_FILE="${RESULT_FILE:-$SCRIPT_DIR/raw-result-$(date -u +%Y%m%dT%H%M%SZ).txt}"

export LD_LIBRARY_PATH="/home/cyf/githubDownload/ceph/build/lib:/opt/mellanox/doca/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

make -C "$SCRIPT_DIR"

if ! "$CEPH_BIN/ceph" osd pool ls | grep -Fxq "$POOL"; then
	"$CEPH_BIN/ceph" osd pool create "$POOL" 8
fi
"$CEPH_BIN/ceph" osd pool set "$POOL" size 3
"$CEPH_BIN/ceph" osd pool set "$POOL" min_size 2
"$CEPH_BIN/ceph" osd pool application enable "$POOL" benchmark --yes-i-really-mean-it >/dev/null

{
	echo "RAW_CONFIG pool=$POOL dataset=$DATASET raw_prefix=$RAW_PREFIX rounds=$ROUNDS"
	"$SCRIPT_DIR/ceph_doca_read_bench" write-raw "$POOL" "$DATASET" "$RAW_PREFIX"
	"$SCRIPT_DIR/ceph_doca_read_bench" bench-raw "$POOL" "$RAW_PREFIX" "$ROUNDS"
} 2>&1 | tee "$RESULT_FILE"

echo "Raw baseline result saved to $RESULT_FILE"
