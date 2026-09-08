#!/usr/bin/env bash
set -euo pipefail

RUNTIME=unix:///run/containerd/containerd.sock
STAGE_DIR=/home/cyf/baseline2_stage

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run with sudo" >&2
    exit 1
fi

echo "[1/6] Install corrected backend config"
install -m 0644 "${STAGE_DIR}/baseline2_spdk_rpc_init.conf" /etc/nvda_snap/spdk_rpc_init.conf
# kubelet treated the old .pending file as a manifest; the active .yaml is kept.
rm -f /etc/kubelet.d/doca_snap.yaml.pending

echo "[2/6] Locate running SNAP container"
snap_cid=$(crictl --runtime-endpoint "${RUNTIME}" ps --name snap -q | head -n 1)
if [[ -z "${snap_cid}" ]]; then
    echo "ERROR: SNAP container is not running" >&2
    crictl --runtime-endpoint "${RUNTIME}" ps -a --name snap
    exit 2
fi
echo "SNAP container: ${snap_cid}"

snap_exec() {
    crictl --runtime-endpoint "${RUNTIME}" exec "${snap_cid}" "$@"
}

echo "[3/6] Connect SNAP's SPDK initiator to node3 (route selects ib1)"
if ! snap_exec spdk_rpc.py bdev_get_bdevs -b RemoteNvme0n1 >/dev/null 2>&1; then
    snap_exec spdk_rpc.py bdev_nvme_attach_controller \
        -b RemoteNvme0 -t rdma -f ipv4 \
        -a 192.168.200.3 -s 4420 \
        -n nqn.2026-09.io.spdk:node3-nvme1
fi
snap_exec spdk_rpc.py bdev_get_bdevs -b RemoteNvme0n1

echo "[4/6] Register the SPDK bdev with SNAP"
if ! snap_exec snap_rpc.py bdev_list | grep -Fq 'RemoteNvme0n1'; then
    snap_exec snap_rpc.py spdk_bdev_create RemoteNvme0n1
fi

echo "[5/6] Create emulated NVMe subsystem, namespace, and DPA controller"
if ! snap_exec snap_rpc.py nvme_subsystem_list | grep -Fq 'nqn.2026-09.io.snap:node4-remote-nvme1'; then
    snap_exec snap_rpc.py nvme_subsystem_create \
        -s nqn.2026-09.io.snap:node4-remote-nvme1 \
        -sn SNAPNODE3NVME1 -mn BlueField3-SNAP
    snap_exec snap_rpc.py nvme_namespace_create \
        -s nqn.2026-09.io.snap:node4-remote-nvme1 \
        -b RemoteNvme0n1 -n 1
fi
if ! snap_exec snap_rpc.py nvme_controller_list | grep -Fq 'SnapCtrl0'; then
    snap_exec snap_rpc.py nvme_controller_create \
        -s nqn.2026-09.io.snap:node4-remote-nvme1 \
        -c SnapCtrl0 --pf_id 0 -n 64
    snap_exec snap_rpc.py nvme_controller_attach_ns -c SnapCtrl0 -n 1
fi

echo "[6/6] Final SNAP state"
snap_exec snap_rpc.py bdev_list
snap_exec snap_rpc.py nvme_subsystem_list
snap_exec snap_rpc.py nvme_controller_list
echo "BACKEND_REPAIR_COMPLETE"

