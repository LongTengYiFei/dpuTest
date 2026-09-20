#!/usr/bin/env bash
set -euo pipefail

EXPECTED_HOST=node3
TARGET_BDF=0000:65:00.0
TARGET_PCI_ID=0x8086:0x0b60
TARGET_SERIAL=PHAO332302201P9SGN
TARGET_PID_FILE=/home/cyf/dpuTest/SNAP/run/node3-nvmf.pid

if [[ ${EUID} -ne 0 ]]; then
    echo "ERROR: run this script with sudo on node3" >&2
    exit 1
fi
if [[ $(hostname -s) != "${EXPECTED_HOST}" ]]; then
    echo "ERROR: expected host ${EXPECTED_HOST}, got $(hostname -s)" >&2
    exit 2
fi
device_path="/sys/bus/pci/devices/${TARGET_BDF}"
if [[ ! -d ${device_path} ]]; then
    echo "ERROR: PCI device ${TARGET_BDF} is absent" >&2
    exit 3
fi
vendor=$(<"${device_path}/vendor")
device=$(<"${device_path}/device")
if [[ "${vendor}:${device}" != "${TARGET_PCI_ID}" ]]; then
    echo "ERROR: ${TARGET_BDF} is ${vendor}:${device}, expected ${TARGET_PCI_ID}" >&2
    exit 4
fi

echo "[1/4] Stop only the recorded node3 SPDK target"
if [[ -f ${TARGET_PID_FILE} ]]; then
    target_pid=$(<"${TARGET_PID_FILE}")
    if kill -0 "${target_pid}" 2>/dev/null; then
        target_exe=$(readlink -f "/proc/${target_pid}/exe" 2>/dev/null || true)
        if [[ ${target_exe##*/} != nvmf_tgt ]]; then
            echo "ERROR: PID ${target_pid} is ${target_exe}, not nvmf_tgt" >&2
            exit 5
        fi
        kill -INT "${target_pid}"
        for _ in {1..100}; do
            kill -0 "${target_pid}" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "${target_pid}" 2>/dev/null; then
            echo "ERROR: nvmf_tgt PID ${target_pid} did not stop" >&2
            exit 6
        fi
    fi
    rm -f "${TARGET_PID_FILE}"
fi

echo "[2/4] Verify no process is using this device's UIO node"
shopt -s nullglob
for uio_path in "${device_path}"/uio/uio*; do
    uio_dev="/dev/${uio_path##*/}"
    if command -v fuser >/dev/null 2>&1 && fuser -s "${uio_dev}"; then
        echo "ERROR: ${uio_dev} is still in use; refusing to unbind" >&2
        fuser -v "${uio_dev}" >&2 || true
        exit 7
    fi
done
shopt -u nullglob

echo "[3/4] Bind only ${TARGET_BDF} to the Linux nvme driver"
modprobe nvme
current_driver=$(basename "$(readlink -f "${device_path}/driver")")
case "${current_driver}" in
    nvme)
        echo "${TARGET_BDF} is already bound to nvme"
        ;;
    uio_pci_generic|vfio-pci)
        echo "${TARGET_BDF}" >"${device_path}/driver/unbind"
        echo nvme >"${device_path}/driver_override"
        echo "${TARGET_BDF}" >/sys/bus/pci/drivers_probe
        echo "" >"${device_path}/driver_override"
        ;;
    *)
        echo "ERROR: unexpected current driver ${current_driver}" >&2
        exit 8
        ;;
esac

echo "[4/4] Verify the restored namespace identity"
udevadm settle
block_sysfs=""
block_dev=""
for _ in {1..100}; do
    for candidate in /sys/class/block/nvme*n*; do
        [[ -e ${candidate} ]] || continue
        [[ -e ${candidate}/partition ]] && continue
        candidate_bdf=$(basename "$(readlink -f "${candidate}/device/device")")
        if [[ ${candidate_bdf} == "${TARGET_BDF}" ]]; then
            block_sysfs=${candidate}
            block_dev="/dev/${candidate##*/}"
            break 2
        fi
    done
    sleep 0.1
done
if [[ -z ${block_dev} ]]; then
    echo "ERROR: no NVMe namespace appeared for ${TARGET_BDF}" >&2
    exit 9
fi

restored_driver=$(basename "$(readlink -f "${device_path}/driver")")
actual_serial=$(tr -d '[:space:]' <"${block_sysfs}/device/serial")
if [[ ${restored_driver} != nvme ]]; then
    echo "ERROR: ${TARGET_BDF} is bound to ${restored_driver}, expected nvme" >&2
    exit 10
fi
if [[ ${actual_serial} != "${TARGET_SERIAL}" ]]; then
    echo "ERROR: ${block_dev} serial is ${actual_serial}, expected ${TARGET_SERIAL}" >&2
    exit 11
fi

lsblk -d -o NAME,PATH,SIZE,MODEL,SERIAL,TRAN "${block_dev}"
echo "RESTORE_COMPLETE: ${TARGET_BDF} -> ${block_dev}, driver=nvme, serial=${actual_serial}"
echo "No disk data was erased or formatted."
