#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=baseline1.env
source "$SCRIPT_DIR/baseline1.env"

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

ip link set dev "$IB_IF" up
ip addr replace "$CLIENT_CIDR" dev "$IB_IF"

# Allocate hugepages only. Never bind any client PCI device here.
env HUGEMEM="$HUGEMEM_MB" PCI_ALLOWED=none TARGET_USER=cyf \
    "$SPDK_DIR/scripts/setup.sh"

ip -br addr show dev "$IB_IF"
grep -E 'HugePages_(Total|Free)|Hugepagesize' /proc/meminfo
