#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=baseline1.env
source "$SCRIPT_DIR/baseline1.env"
RPC="$SPDK_DIR/scripts/rpc.py"

if [[ $(hostname -s) != node3 ]]; then
    echo "Run this script on node3" >&2
    exit 1
fi
if [[ $EUID -ne 0 ]]; then
    echo "UIO access requires root: sudo $0" >&2
    exit 1
fi
if [[ $(<"/sys/class/net/$IB_IF/carrier") != 1 ]]; then
    echo "$IB_IF has no carrier. The InfiniBand subnet manager must provide the IPoIB broadcast group." >&2
    exit 1
fi

driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$TARGET_BDF/driver")")
if [[ $driver != vfio-pci && $driver != uio_pci_generic ]]; then
    echo "$TARGET_BDF is bound to $driver; run node3_prepare.sh first" >&2
    exit 1
fi
if [[ -f $TARGET_PID_FILE ]]; then
    old_pid=$(<"$TARGET_PID_FILE")
    if kill -0 "$old_pid" 2>/dev/null; then
        echo "Target already running as PID $old_pid" >&2
        exit 1
    fi
fi

mkdir -p "$(dirname "$TARGET_PID_FILE")"
rm -f "$RPC_SOCK"
nohup "$SPDK_DIR/build/bin/nvmf_tgt" -m "$SPDK_CORE_MASK" -r "$RPC_SOCK" \
    >"$TARGET_LOG" 2>&1 &
target_pid=$!
echo "$target_pid" >"$TARGET_PID_FILE"

cleanup_on_error() {
    kill "$target_pid" 2>/dev/null || true
}
trap cleanup_on_error ERR

for _ in {1..100}; do
    [[ -S $RPC_SOCK ]] && break
    kill -0 "$target_pid" 2>/dev/null || {
        tail -100 "$TARGET_LOG" >&2
        exit 1
    }
    sleep 0.1
done
[[ -S $RPC_SOCK ]] || {
    echo "RPC socket did not appear" >&2
    exit 1
}

"$RPC" -s "$RPC_SOCK" bdev_nvme_attach_controller \
    -b Nvme1 -t PCIe -a "$TARGET_BDF"
# SNAP uses one backend I/O channel per Arm reactor. Keep enough controller
# qpairs for all 16 DPU cores plus headroom; -m is max I/O qpairs/controller.
"$RPC" -s "$RPC_SOCK" nvmf_create_transport -t RDMA -q 128 -m 64
"$RPC" -s "$RPC_SOCK" nvmf_create_subsystem "$NQN" -a \
    -s SPDKNODE3NVME1 -d "node3 nvme1n1"
"$RPC" -s "$RPC_SOCK" nvmf_subsystem_add_ns "$NQN" Nvme1n1
"$RPC" -s "$RPC_SOCK" nvmf_subsystem_add_listener "$NQN" \
    -t rdma -f ipv4 -a "$TARGET_IP" -s "$TRSVCID"

trap - ERR
echo "SPDK NVMe-oF target PID: $target_pid"
"$RPC" -s "$RPC_SOCK" nvmf_get_subsystems
