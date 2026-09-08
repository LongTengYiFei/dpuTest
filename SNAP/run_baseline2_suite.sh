#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SPDK_DIR="${SCRIPT_DIR}/spdk"
SNAP_BDF="0000:b1:00.2"
CORE_MASK="0x1000000"
RUNTIME="${RUNTIME:-30}"

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run this suite with sudo" >&2
    exit 1
fi
if [[ ! -x "${SPDK_DIR}/build/bin/spdk_nvme_perf" ]]; then
    echo "ERROR: SPDK nvme_perf is missing under ${SPDK_DIR}" >&2
    exit 2
fi
if [[ ! -e "/sys/bus/pci/devices/${SNAP_BDF}" ]]; then
    echo "ERROR: SNAP PCI function ${SNAP_BDF} is absent" >&2
    exit 3
fi

vendor=$(<"/sys/bus/pci/devices/${SNAP_BDF}/vendor")
device=$(<"/sys/bus/pci/devices/${SNAP_BDF}/device")
if [[ "${vendor}:${device}" != "0x15b3:0x6001" ]]; then
    echo "ERROR: ${SNAP_BDF} is ${vendor}:${device}, not NVIDIA NVMe SNAP 15b3:6001" >&2
    exit 4
fi

echo "[1/3] Bind only ${SNAP_BDF} to uio_pci_generic"
modprobe uio_pci_generic
PCI_ALLOWED="${SNAP_BDF}" DRIVER_OVERRIDE=uio_pci_generic HUGEMEM=2048 \
    "${SPDK_DIR}/scripts/setup.sh"
driver=$(basename "$(readlink -f "/sys/bus/pci/devices/${SNAP_BDF}/driver")")
if [[ "${driver}" != "uio_pci_generic" ]]; then
    echo "ERROR: ${SNAP_BDF} driver is ${driver}, expected uio_pci_generic" >&2
    exit 5
fi
echo "${SNAP_BDF} driver=${driver}"

echo "[2/3] One-second SPDK attach smoke test"
transport="trtype:PCIe traddr:${SNAP_BDF}"
"${SPDK_DIR}/build/bin/spdk_nvme_perf" \
    -q 1 -o 4096 -w randread -t 1 -c "${CORE_MASK}" \
    --iova-mode=pa -r "${transport}"

echo "[3/3] Run baseline 2 workload matrix"
mkdir -p "${SCRIPT_DIR}/results"
suite_stamp=$(date -u +%Y%m%dT%H%M%SZ)
summary="${SCRIPT_DIR}/results/baseline2-suite-${suite_stamp}.summary"
printf 'Baseline 2: Host SPDK -> SNAP PCIe NVMe -> DPU NVMe-oF/RDMA -> node3 SPDK target\n' | tee "${summary}"
printf 'UTC=%s BDF=%s core_mask=%s runtime=%ss\n' "${suite_stamp}" "${SNAP_BDF}" "${CORE_MASK}" "${RUNTIME}" | tee -a "${summary}"

run_case() {
    local workload=$1 block_size=$2 queue_depth=$3
    local result="${SCRIPT_DIR}/results/baseline2-${workload}-${block_size}B-qd${queue_depth}-${suite_stamp}.log"
    echo
    echo "CASE workload=${workload} block_size=${block_size} queue_depth=${queue_depth}" | tee -a "${summary}"
    {
        echo "SPDK baseline 2: userspace PCIe initiator consuming a SNAP-emulated NVMe disk"
        echo "UTC=${suite_stamp}"
        echo "workload=${workload} block_size=${block_size} queue_depth=${queue_depth} runtime=${RUNTIME}"
        echo "transport=${transport}"
        echo
        "${SPDK_DIR}/build/bin/spdk_nvme_perf" \
            -q "${queue_depth}" -o "${block_size}" -w "${workload}" -t "${RUNTIME}" \
            -c "${CORE_MASK}" --iova-mode=pa -L -r "${transport}"
    } 2>&1 | tee "${result}"
    grep -E 'Total|IOPS|MiB/s|Average|99\.00000' "${result}" | tail -n 12 | tee -a "${summary}" || true
    echo "log=${result}" | tee -a "${summary}"
}

run_case randread 4096 1
run_case randread 4096 64
run_case randwrite 4096 64
run_case read 131072 32
run_case write 131072 32

echo
echo "BASELINE2_SUITE_COMPLETE"
echo "Summary: ${summary}"
