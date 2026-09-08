#!/usr/bin/env bash
set -euo pipefail

# Run this script on the local BlueField-3 Arm OS as root.
DPU_IB_DEV="${DPU_IB_DEV:-ib1}"
DPU_IB_ADDR="${DPU_IB_ADDR:-192.168.200.4/24}"
TARGET_ADDR="${TARGET_ADDR:-192.168.200.3}"
MST_DEV="${MST_DEV:-/dev/mst/mt41692_pciconf0}"

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run as root (sudo $0)" >&2
    exit 1
fi

echo "[1/6] BlueField port mapping and state"
ibdev2netdev || true
ibstat mlx5_1 || true

echo "[2/6] Configure DPU IPoIB address ${DPU_IB_ADDR}"
ip link set dev "${DPU_IB_DEV}" up
addr_ip="${DPU_IB_ADDR%/*}"
if ! ip -4 -o addr show dev "${DPU_IB_DEV}" | grep -Fq " ${addr_ip}/"; then
    if command -v arping >/dev/null 2>&1; then
        if arping -D -c 3 -I "${DPU_IB_DEV}" "${addr_ip}" >/dev/null 2>&1; then
            :
        else
            echo "ERROR: ${addr_ip} appears to be in use; address was not assigned." >&2
            exit 2
        fi
    fi
    ip addr replace "${DPU_IB_ADDR}" dev "${DPU_IB_DEV}"
fi
ip -br addr show dev "${DPU_IB_DEV}"

echo "[3/6] DPU -> node3 IPoIB connectivity"
ping -I "${DPU_IB_DEV}" -c 3 -W 2 "${TARGET_ADDR}"

echo "[4/6] SNAP-related firmware settings (query only)"
mst start >/dev/null
mlxconfig -d "${MST_DEV}" -e query 2>&1 \
    | grep -E 'INTERNAL_CPU_MODEL|NVME_EMULATION_ENABLE|NVME_EMULATION_NUM_PF|NVME_EMULATION_NUM_VF|VIRTIO_BLK_EMULATION_ENABLE|PCI_SWITCH_EMULATION_ENABLE' \
    || true

echo "[5/6] Installed SNAP files and container images"
dpkg-query -W -f='${binary:Package}\t${Version}\n' mlnx-libsnap spdk spdk-rpc 2>/dev/null || true
find /etc/kubelet.d /etc/kubernetes/manifests -maxdepth 1 -type f -printf '%p\n' 2>/dev/null | sort
if command -v crictl >/dev/null 2>&1; then
    crictl --runtime-endpoint unix:///run/containerd/containerd.sock images 2>&1 || true
fi
if command -v ctr >/dev/null 2>&1; then
    for ns in k8s.io default; do
        echo "containerd namespace: ${ns}"
        ctr -n "${ns}" images list 2>&1 || true
    done
fi

echo "[6/6] Resource status"
systemctl is-active containerd kubelet || true
free -h
lscpu | grep -E '^(CPU\(s\)|NUMA node\(s\))' || true

echo "PRECHECK_COMPLETE"
