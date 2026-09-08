#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=baseline1.env
source "$SCRIPT_DIR/baseline1.env"

if [[ $EUID -ne 0 ]]; then
    echo "Run this on node3 as root: sudo $0" >&2
    exit 1
fi
if [[ $(hostname -s) != node3 ]]; then
    echo "Refusing to run: expected hostname node3, got $(hostname -s)" >&2
    exit 1
fi
if [[ ! -e /sys/bus/pci/devices/$TARGET_BDF ]]; then
    echo "Target PCI device $TARGET_BDF does not exist" >&2
    exit 1
fi

current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$TARGET_BDF/driver")")
if [[ $current_driver == nvme ]]; then
    actual_bdf=$(basename "$(readlink -f "/sys/block/${TARGET_NVME_DEV##*/}/device/device")")
    actual_serial=$(tr -d ' ' < "/sys/block/${TARGET_NVME_DEV##*/}/device/serial")
    [[ $actual_bdf == "$TARGET_BDF" ]] || {
        echo "Refusing: $TARGET_NVME_DEV maps to $actual_bdf, expected $TARGET_BDF" >&2
        exit 1
    }
    [[ $actual_serial == "$TARGET_SERIAL" ]] || {
        echo "Refusing: serial is $actual_serial, expected $TARGET_SERIAL" >&2
        exit 1
    }
    if findmnt -rn -S "$TARGET_NVME_DEV" | grep -q .; then
        echo "Refusing: $TARGET_NVME_DEV is mounted" >&2
        exit 1
    fi
    if compgen -G "/sys/block/${TARGET_NVME_DEV##*/}/holders/*" >/dev/null; then
        echo "Refusing: $TARGET_NVME_DEV has active holders" >&2
        exit 1
    fi
elif [[ $current_driver != vfio-pci && $current_driver != uio_pci_generic ]]; then
    echo "Refusing: unexpected driver $current_driver for $TARGET_BDF" >&2
    exit 1
fi

ip link set dev "$IB_IF" up
ip addr replace "$TARGET_CIDR" dev "$IB_IF"

# Bind only the explicitly verified NVMe device. Other NVMe/Ceph devices are excluded.
env HUGEMEM="$HUGEMEM_MB" PCI_ALLOWED="$TARGET_BDF" DEV_TYPE=NVME TARGET_USER=cyf \
    "$SPDK_DIR/scripts/setup.sh"

ip -br addr show dev "$IB_IF"
"$SPDK_DIR/scripts/setup.sh" status
