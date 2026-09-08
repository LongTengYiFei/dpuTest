#!/usr/bin/env bash
set -euo pipefail

RUNTIME=unix:///run/containerd/containerd.sock
STAGE_DIR=/home/cyf/baseline2_stage

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run with sudo" >&2
    exit 1
fi

echo "[1/5] Persist backend queue and controller fixes"
install -m 0644 "${STAGE_DIR}/baseline2_spdk_rpc_init.conf" /etc/nvda_snap/spdk_rpc_init.conf
install -m 0644 "${STAGE_DIR}/baseline2_snap_rpc_init.conf" /etc/nvda_snap/snap_rpc_init.conf
echo "SPDK init: $(cat /etc/nvda_snap/spdk_rpc_init.conf)"

echo "[2/5] Restart only the SNAP container"
old_cid=$(crictl --runtime-endpoint "${RUNTIME}" ps --name snap -q | head -n 1)
if [[ -z "${old_cid}" ]]; then
    echo "ERROR: no running SNAP container" >&2
    exit 2
fi
echo "old SNAP container: ${old_cid}"
crictl --runtime-endpoint "${RUNTIME}" stop "${old_cid}"

echo "[3/5] Wait for a new SNAP instance and initialization"
new_cid=""
for _ in $(seq 1 40); do
    new_cid=$(crictl --runtime-endpoint "${RUNTIME}" ps --name snap -q | head -n 1)
    if [[ -n "${new_cid}" && "${new_cid}" != "${old_cid}" ]]; then
        break
    fi
    sleep 2
done
if [[ -z "${new_cid}" || "${new_cid}" == "${old_cid}" ]]; then
    echo "ERROR: replacement SNAP container did not start" >&2
    crictl --runtime-endpoint "${RUNTIME}" ps -a --name snap
    exit 3
fi
echo "new SNAP container: ${new_cid}"

snap_exec() {
    crictl --runtime-endpoint "${RUNTIME}" exec "${new_cid}" "$@"
}
for _ in $(seq 1 30); do
    if snap_exec snap_rpc.py nvme_controller_list -c SnapCtrl0 >/dev/null 2>&1; then
        break
    fi
    sleep 2
done

echo "[4/5] Verify backend and namespace readiness"
snap_exec spdk_rpc.py bdev_nvme_get_controllers -n RemoteNvme0
snap_exec spdk_rpc.py bdev_get_iostat -b RemoteNvme0n1
ns_state=$(snap_exec snap_rpc.py nvme_namespace_list -s nqn.2026-09.io.snap:node4-remote-nvme1)
echo "${ns_state}"
snap_exec snap_rpc.py nvme_controller_list -c SnapCtrl0

echo "[5/5] Check startup log for queue exhaustion"
startup_log=$(crictl --runtime-endpoint "${RUNTIME}" logs "${new_cid}" 2>&1)
echo "${startup_log}" | tail -n 120
if echo "${startup_log}" | grep -Fq 'No free I/O queue IDs'; then
    echo "ERROR: backend I/O queue exhaustion remains" >&2
    exit 4
fi
if [[ "${ns_state}" == *'In_progress'* ]]; then
    echo "ERROR: namespace is still In_progress" >&2
    exit 5
fi
echo "QUEUE_REPAIR_COMPLETE"

