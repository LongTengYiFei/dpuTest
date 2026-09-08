#!/usr/bin/env bash
set -euo pipefail

IMAGE_ARCHIVE=/home/cyf/doca_snap_4.4.0-doca2.7.0_arm64.tar.gz
EXPECTED_SHA256=27af3d803d639c431b7eaa80811d267ec06c4f3d157eb541ffb9ea467e7fee6e
MST_DEV=/dev/mst/mt41692_pciconf0
STAGE_DIR=/home/cyf/baseline2_stage

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run with sudo" >&2
    exit 1
fi

echo "[1/6] Verify and import the DOCA 2.7-compatible SNAP image"
echo "${EXPECTED_SHA256}  ${IMAGE_ARCHIVE}" | sha256sum --check
gzip -t "${IMAGE_ARCHIVE}"
if ! ctr -n k8s.io images list | grep -Fq 'nvcr.io/nvidia/doca/doca_snap:4.4.0-doca2.7.0'; then
    # BlueField's ctr does not auto-detect the outer gzip stream.
    gzip -dc "${IMAGE_ARCHIVE}" | ctr -n k8s.io images import -
fi
ctr -n k8s.io images list | grep -F 'nvcr.io/nvidia/doca/doca_snap:4.4.0-doca2.7.0'

echo "[2/6] Install persistent IPoIB and hugepage configuration"
install -m 0644 "${STAGE_DIR}/snap-ib1.service" /etc/systemd/system/snap-ib1.service
install -m 0644 "${STAGE_DIR}/90-snap-hugepages.conf" /etc/sysctl.d/90-snap-hugepages.conf
systemctl daemon-reload
systemctl enable --now snap-ib1.service
sysctl --system >/dev/null
ip -br addr show ib1
grep -E 'HugePages_Total|HugePages_Free|Hugepagesize' /proc/meminfo

echo "[3/6] Verify node3 NVMe-oF network path"
ping -I ib1 -c 3 -W 2 192.168.200.3

echo "[4/6] Stage SNAP runtime configuration (pod remains disabled)"
install -d -m 0755 /etc/nvda_snap /var/log/snap-log
install -m 0644 "${STAGE_DIR}/baseline2_spdk_rpc_init.conf" /etc/nvda_snap/spdk_rpc_init.conf
install -m 0644 "${STAGE_DIR}/baseline2_snap_rpc_init.conf" /etc/nvda_snap/snap_rpc_init.conf
install -m 0644 "${STAGE_DIR}/doca_snap_baseline2.yaml" /etc/nvda_snap/doca_snap.yaml.pending

echo "[5/6] Enable one NVMe emulation PF in BlueField firmware"
mst start >/dev/null
mlxconfig -y -d "${MST_DEV}" set  \
    INTERNAL_CPU_MODEL=1 \
    NVME_EMULATION_ENABLE=1 \
    NVME_EMULATION_NUM_PF=1

echo "[6/6] Pending settings"
mlxconfig -d "${MST_DEV}" -e query 2>&1 \
    | grep -E 'INTERNAL_CPU_MODEL|NVME_EMULATION_ENABLE|NVME_EMULATION_NUM_PF'
echo
echo "STAGE_COMPLETE"
echo "A cold power cycle of node4 is now required. A DPU-only reboot is insufficient."
echo "The SNAP pod is staged but deliberately not activated until after that power cycle."
