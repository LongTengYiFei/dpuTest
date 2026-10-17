#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CEPH_ROOT="${CEPH_ROOT:-/home/cyf/githubDownload/ceph}"
DOCA_ROOT="${DOCA_ROOT:-/opt/mellanox/doca}"
CEPH_CLI="${CEPH_CLI:-$CEPH_ROOT/build/bin/ceph}"
RADOS_CLI="${RADOS_CLI:-$CEPH_ROOT/build/bin/rados}"
CEPH_CONF="${CEPH_CONF:-/etc/ceph/ceph.conf}"
CEPH_CLUSTER="${CEPH_CLUSTER:-ceph}"
CEPH_ID="${CEPH_ID:-admin}"
CEPH_LIB_DIR="${CEPH_LIB_DIR:-$CEPH_ROOT/build/lib}"
DOCA_LIB_DIR="${DOCA_LIB_DIR:-$DOCA_ROOT/lib/x86_64-linux-gnu}"

POOL="${POOL:-rados-cli-doca}"
DATASET="${DATASET:-/home/cyf/raid0/LogHub/Android_v1}"
PREFIX="${PREFIX:-zlib-4m}"
ROUNDS="${ROUNDS:-1}"
PCI_ADDR="${PCI_ADDR:-b1:00.0}"
RUN_DIR="${RUN_DIR:-$SCRIPT_DIR/simple-results/doca-$(date +%Y%m%dT%H%M%S)-$$}"
RESULT_FILE="${RESULT_FILE:-$RUN_DIR/result.txt}"

export LD_LIBRARY_PATH="$CEPH_LIB_DIR:$DOCA_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
CEPH_CMD=("$CEPH_CLI" --cluster "$CEPH_CLUSTER" --id "$CEPH_ID" -c "$CEPH_CONF")
RADOS_CMD=("$RADOS_CLI" --cluster "$CEPH_CLUSTER" --id "$CEPH_ID" -c "$CEPH_CONF" -p "$POOL")

make -C "$SCRIPT_DIR" doca_local_decompress CEPH_ROOT="$CEPH_ROOT" DOCA_ROOT="$DOCA_ROOT"

mkdir -p "$RUN_DIR/chunks" "$RUN_DIR/compressed"
mapfile -d '' LOG_FILES < <(find "$DATASET" -type f -name '*.log' -print0 | sort -z)
if ((${#LOG_FILES[@]} == 0)); then
	echo "No .log files found under $DATASET" >&2
	exit 1
fi

for file in "${LOG_FILES[@]}"; do
	cat "$file"
done | split -b 4M -d -a 6 - "$RUN_DIR/chunks/chunk."

shopt -s nullglob
CHUNKS=("$RUN_DIR"/chunks/chunk.*)
if ((${#CHUNKS[@]} == 0)); then
	echo "Dataset is empty" >&2
	exit 1
fi

RAW_BYTES=0
for file in "${CHUNKS[@]}"; do
	((RAW_BYTES += $(stat -c %s "$file")))
	index="${file##*.}"
	python3 "$SCRIPT_DIR/pack_zlib_frames.py" "$file" "$RUN_DIR/compressed/$PREFIX.$index.zobj"
done
COMPRESSED=("$RUN_DIR"/compressed/*.zobj)
COMPRESSED_BYTES=0
for file in "${COMPRESSED[@]}"; do
	((COMPRESSED_BYTES += $(stat -c %s "$file")))
done

if ! "${CEPH_CMD[@]}" osd pool ls | grep -Fxq "$POOL"; then
	"${CEPH_CMD[@]}" osd pool create "$POOL" 8
fi
"${CEPH_CMD[@]}" osd pool set "$POOL" size 3
"${CEPH_CMD[@]}" osd pool set "$POOL" min_size 2
"${CEPH_CMD[@]}" osd pool application enable "$POOL" benchmark --yes-i-really-mean-it >/dev/null

{
	echo "CONFIG mode=doca-cli pool=$POOL dataset=$DATASET prefix=$PREFIX rounds=$ROUNDS pci=$PCI_ADDR run_dir=$RUN_DIR"
	echo "DATASET objects=${#CHUNKS[@]} raw_bytes=$RAW_BYTES compressed_bytes=$COMPRESSED_BYTES object_bytes=4194304"

	PUT_START=$(date +%s%N)
	for file in "${COMPRESSED[@]}"; do
		name="$(basename "$file" .zobj)"
		"${RADOS_CMD[@]}" put "$name" "$file"
	done
	PUT_NS=$(($(date +%s%N) - PUT_START))
	echo "PUT objects=${#COMPRESSED[@]} compressed_bytes=$COMPRESSED_BYTES elapsed_ms=$(awk -v ns="$PUT_NS" 'BEGIN {printf "%.3f", ns / 1000000}')"

	TOTAL_NS=0
	for ((round = 1; round <= ROUNDS; ++round)); do
		GET_DIR="$RUN_DIR/get-round-$round"
		OUTPUT_DIR="$RUN_DIR/decompressed-round-$round"
		mkdir -p "$GET_DIR" "$OUTPUT_DIR"
		START=$(date +%s%N)
		DOWNLOADED=()
		for file in "${COMPRESSED[@]}"; do
			name="$(basename "$file" .zobj)"
			local_file="$GET_DIR/$name.zobj"
			"${RADOS_CMD[@]}" get "$name" "$local_file"
			DOWNLOADED+=("$local_file")
		done
		AFTER_GET=$(date +%s%N)
		"$SCRIPT_DIR/doca_local_decompress" "$PCI_ADDR" "$OUTPUT_DIR" "${DOWNLOADED[@]}"
		END=$(date +%s%N)
		GET_NS=$((AFTER_GET - START))
		DECOMPRESS_NS=$((END - AFTER_GET))
		ELAPSED_NS=$((END - START))
		((TOTAL_NS += ELAPSED_NS))
		MIB_S=$(awk -v bytes="$RAW_BYTES" -v ns="$ELAPSED_NS" 'BEGIN {printf "%.2f", bytes / 1048576 / (ns / 1000000000)}')
		echo "RESULT method=doca-cli round=$round raw_bytes=$RAW_BYTES compressed_bytes=$COMPRESSED_BYTES total_ms=$(awk -v ns="$ELAPSED_NS" 'BEGIN {printf "%.3f", ns / 1000000}') get_ms=$(awk -v ns="$GET_NS" 'BEGIN {printf "%.3f", ns / 1000000}') decompress_ms=$(awk -v ns="$DECOMPRESS_NS" 'BEGIN {printf "%.3f", ns / 1000000}') throughput_mib_s=$MIB_S get_dir=$GET_DIR output_dir=$OUTPUT_DIR"
	done
	TOTAL_BYTES=$((RAW_BYTES * ROUNDS))
	SUMMARY_MIB_S=$(awk -v bytes="$TOTAL_BYTES" -v ns="$TOTAL_NS" 'BEGIN {printf "%.2f", bytes / 1048576 / (ns / 1000000000)}')
	echo "SUMMARY method=doca-cli doca_mib_s=$SUMMARY_MIB_S rounds=$ROUNDS total_ms=$(awk -v ns="$TOTAL_NS" 'BEGIN {printf "%.3f", ns / 1000000}')"
} 2>&1 | tee "$RESULT_FILE"

echo "Result saved to $RESULT_FILE"
