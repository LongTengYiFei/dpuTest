#!/usr/bin/env bash
set -euo pipefail

MST_DEV=/dev/mst/mt41692_pciconf0

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run with sudo" >&2
    exit 1
fi

echo "[1/5] Verify firmware setting after the cold power cycle"
mst start >/dev/null
fw_line=$(mlxconfig -d "${MST_DEV}" -e query 2>&1 | grep 'NVME_EMULATION_ENABLE' || true)
echo "${fw_line}"
if [[ "${fw_line}" != *'True(1)'* ]]; then
    echo "ERROR: NVMe emulation is not active. Confirm that node4 received a cold power cycle." >&2
    exit 2
fi

echo "[2/5] Verify network, image, and hugepages"
systemctl start snap-ib1.service
ping -I ib1 -c 2 -W 2 192.168.200.3
ctr -n k8s.io images list | grep -F 'nvcr.io/nvidia/doca/doca_snap:4.4.0-doca2.7.0'
grep -E 'HugePages_Total|HugePages_Free|Hugepagesize' /proc/meminfo

echo "[3/5] Activate SNAP static pod"
install -m 0644 /etc/nvda_snap/doca_snap.yaml.pending /etc/kubelet.d/doca_snap.yaml
systemctl restart kubelet

echo "[4/5] Wait for the SNAP container"
snap_cid=""
for _ in $(seq 1 40); do
    snap_cid=$(crictl --runtime-endpoint unix:///run/containerd/containerd.sock ps --name snap -q | head -n 1)
    [[ -n "${snap_cid}" ]] && break
    sleep 3
done
if [[ -z "${snap_cid}" ]]; then
    echo "ERROR: SNAP container did not enter Running state." >&2
    crictl --runtime-endpoint unix:///run/containerd/containerd.sock ps -a --name snap
    pod_id=$(crictl --runtime-endpoint unix:///run/containerd/containerd.sock pods --name snap -q | head -n 1)
    [[ -n "${pod_id}" ]] && crictl --runtime-endpoint unix:///run/containerd/containerd.sock logs "$(crictl --runtime-endpoint unix:///run/containerd/containerd.sock ps -a --pod "${pod_id}" -q | head -n 1)" || true
    exit 3
fi

echo "[5/5] SNAP backend/controller state"
crictl --runtime-endpoint unix:///run/containerd/containerd.sock exec "${snap_cid}" spdk_rpc.py bdev_get_bdevs
crictl --runtime-endpoint unix:///run/containerd/containerd.sock exec "${snap_cid}" snap_rpc.py nvme_controller_list
echo "ACTIVATION_COMPLETE"
