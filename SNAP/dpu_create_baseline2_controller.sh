#!/usr/bin/env bash
set -euo pipefail

RUNTIME=unix:///run/containerd/containerd.sock
STAGE_DIR=/home/cyf/baseline2_stage

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run with sudo" >&2
    exit 1
fi

echo "[1/4] Persist corrected controller configuration"
install -m 0644 "${STAGE_DIR}/baseline2_snap_rpc_init.conf" /etc/nvda_snap/snap_rpc_init.conf

echo "[2/4] Locate stable SNAP container"
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

echo "[3/4] Create PF0 controller using SNAP's supported default queue count"
snap_exec snap_rpc.py emulation_function_list -a || true
if ! snap_exec snap_rpc.py nvme_controller_list | grep -Fq 'SnapCtrl0'; then
    snap_exec snap_rpc.py nvme_controller_create \
        -s nqn.2026-09.io.snap:node4-remote-nvme1 \
        -c SnapCtrl0 --pf_id 0
    snap_exec snap_rpc.py nvme_controller_attach_ns -c SnapCtrl0 -n 1
fi

echo "[4/4] Final controller state"
snap_exec snap_rpc.py nvme_namespace_list -s nqn.2026-09.io.snap:node4-remote-nvme1
snap_exec snap_rpc.py nvme_controller_list -c SnapCtrl0
echo "CONTROLLER_CREATE_COMPLETE"

