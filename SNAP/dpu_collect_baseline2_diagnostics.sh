#!/usr/bin/env bash
set -uo pipefail

RUNTIME=unix:///run/containerd/containerd.sock
OUT=/home/cyf/baseline2_diagnostics.txt

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run with sudo" >&2
    exit 1
fi

exec > >(tee "${OUT}") 2>&1

echo "=== TIME ==="
date -Ins

echo "=== CRI PODS AND CONTAINERS ==="
crictl --runtime-endpoint "${RUNTIME}" pods --name snap
crictl --runtime-endpoint "${RUNTIME}" ps -a --name snap
snap_cid=$(crictl --runtime-endpoint "${RUNTIME}" ps --name snap -q | head -n 1)
echo "snap_cid=${snap_cid}"

echo "=== SNAP CONTAINER LOG ==="
if [[ -n "${snap_cid}" ]]; then
    crictl --runtime-endpoint "${RUNTIME}" logs "${snap_cid}" | tail -n 500
fi

snap_exec() {
    crictl --runtime-endpoint "${RUNTIME}" exec "${snap_cid}" "$@"
}

echo "=== SPDK VERSION AND BDEV ==="
snap_exec spdk_rpc.py spdk_get_version || true
snap_exec spdk_rpc.py bdev_get_bdevs -b RemoteNvme0n1 || true
snap_exec spdk_rpc.py bdev_nvme_get_controllers -n RemoteNvme0 || true
snap_exec spdk_rpc.py bdev_get_iostat -b RemoteNvme0n1 || true

echo "=== SNAP STATE ==="
snap_exec snap_rpc.py bdev_list || true
snap_exec snap_rpc.py nvme_namespace_list -s nqn.2026-09.io.snap:node4-remote-nvme1 || true
snap_exec snap_rpc.py nvme_controller_list -c SnapCtrl0 || true
snap_exec snap_rpc.py nvme_controller_dbg_io_stats_get -c SnapCtrl0 || true

echo "=== MEMORY CGROUP ==="
snap_pid=$(pgrep -xo reactor_0 || true)
echo "snap_pid=${snap_pid}"
if [[ -n "${snap_pid}" ]]; then
    cgroup_rel=$(awk -F: '$1 == "0" {print $3}' "/proc/${snap_pid}/cgroup")
    for f in memory.current memory.max memory.events memory.stat; do
        echo "--- ${f} ---"
        cat "/sys/fs/cgroup${cgroup_rel}/${f}" 2>/dev/null || true
    done
fi

echo "=== KERNEL MESSAGES ==="
dmesg -T | grep -Ei 'snap|dpa|nvme|mlx5|iommu|vfio|oom|killed process' | tail -n 500 || true

echo "=== RPC AUDIT LOG ==="
tail -n 300 /var/log/snap-log/rpc-log 2>/dev/null || true

chmod 0644 "${OUT}"
chown cyf:cyf "${OUT}"
echo "DIAGNOSTICS_COMPLETE ${OUT}"

