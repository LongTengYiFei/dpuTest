#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=baseline1.env
source "$SCRIPT_DIR/baseline1.env"

workload=${1:-randread}
block_size=${2:-4096}
queue_depth=${3:-64}
runtime=${4:-30}

case "$workload" in
    write|randwrite|rw|randrw)
        if [[ ${ALLOW_DESTRUCTIVE_WRITE:-NO} != YES ]]; then
            echo "Workload $workload overwrites node3 nvme1n1." >&2
            echo "Re-run with ALLOW_DESTRUCTIVE_WRITE=YES after confirming." >&2
            exit 1
        fi
        ;;
    read|randread) ;;
    *) echo "Unsupported workload: $workload" >&2; exit 1 ;;
esac

ping -c 2 -W 2 "$TARGET_IP" >/dev/null
mkdir -p "$SCRIPT_DIR/results"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
result="$SCRIPT_DIR/results/baseline1-${workload}-${block_size}B-qd${queue_depth}-${stamp}.log"
transport="trtype:RDMA adrfam:IPv4 traddr:$TARGET_IP trsvcid:$TRSVCID subnqn:$NQN hostaddr:$CLIENT_IP"
runner=()
if [[ $EUID -ne 0 ]]; then
    runner=(sudo)
fi

{
    echo "SPDK baseline 1: direct userspace NVMe-oF/RDMA initiator"
    echo "UTC: $stamp"
    echo "workload=$workload block_size=$block_size queue_depth=$queue_depth runtime=$runtime"
    echo "transport=$transport"
    echo
} | tee "$result"

"${runner[@]}" "$SPDK_DIR/build/bin/spdk_nvme_perf" \
    -q "$queue_depth" -o "$block_size" -w "$workload" -t "$runtime" \
    -c "$SPDK_CORE_MASK" --iova-mode=va -L --transport-stats -r "$transport" \
    2>&1 | tee -a "$result"

echo "Result: $result"
