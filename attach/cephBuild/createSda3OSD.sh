#!/usr/bin/env bash
set -Eeuo pipefail

# Destructively repartition /dev/sda into three equal GPT partitions, then
# create one BlueStore OSD on each partition using the local Ceph build.

DEVICE="/dev/sda"
EXPECTED_MODEL="ST1000DM014-2UB10D"
BUILD_DIR="/home/cyf/githubDownload/ceph/build"
SOURCE_DIR="/home/cyf/githubDownload/ceph"
BIN_DIR="$BUILD_DIR/bin"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_DIR="$SCRIPT_DIR/local-cluster"
PYDEPS_DIR="$STATE_DIR/pydeps"
OSD_BASE="/var/lib/ceph/osd"
RUN_USER="${SUDO_USER:-cyf}"
RUN_GROUP="$(id -gn "$RUN_USER")"
HOST_BUCKET="$(hostname -s)"
CEPH_OSD_GUID="4FBD7E29-9D25-41B8-AFD0-062C0CEFF05D"

if [[ ${1:-} != "--yes-i-really-mean-it" || $# -ne 1 ]]; then
    echo "Usage: sudo $0 --yes-i-really-mean-it" >&2
    echo "WARNING: this erases all partitioning and data on $DEVICE." >&2
    exit 2
fi
if [[ $(id -u) -ne 0 ]]; then
    echo "ERROR: this phase must run through sudo." >&2
    exit 1
fi
if [[ "$RUN_USER" != "cyf" ]]; then
    echo "ERROR: expected invoking user cyf, got $RUN_USER." >&2
    exit 1
fi
if [[ ! -b "$DEVICE" ]]; then
    echo "ERROR: $DEVICE is not a block device." >&2
    exit 1
fi

actual_model="$(lsblk -dn -o MODEL "$DEVICE" | sed 's/[[:space:]]*$//')"
if [[ "$actual_model" != "$EXPECTED_MODEL" ]]; then
    echo "ERROR: $DEVICE model is '$actual_model', expected '$EXPECTED_MODEL'." >&2
    exit 1
fi
if [[ "$(lsblk -dn -o TYPE "$DEVICE")" != "disk" ]]; then
    echo "ERROR: $DEVICE is not a whole disk." >&2
    exit 1
fi
if lsblk -nr -o MOUNTPOINTS "$DEVICE" | grep -q '[^[:space:]]'; then
    echo "ERROR: $DEVICE or one of its partitions is mounted." >&2
    lsblk "$DEVICE" >&2
    exit 1
fi
if swapon --noheadings --raw --show=NAME | grep -qE "^${DEVICE}([0-9]+)?$"; then
    echo "ERROR: $DEVICE is in use as swap." >&2
    exit 1
fi
root_parent="$(lsblk -no PKNAME "$(findmnt -no SOURCE /)" 2>/dev/null || true)"
if [[ -n "$root_parent" && "/dev/$root_parent" == "$DEVICE" ]]; then
    echo "ERROR: refusing to partition the root filesystem disk $DEVICE." >&2
    exit 1
fi

for program in ceph ceph-osd; do
    if [[ ! -x "$BIN_DIR/$program" ]]; then
        echo "ERROR: missing executable: $BIN_DIR/$program" >&2
        exit 1
    fi
done
for program in sgdisk partprobe udevadm wipefs blockdev; do
    command -v "$program" >/dev/null || {
        echo "ERROR: missing required command: $program" >&2
        exit 1
    }
done
if ! pgrep -x ceph-mon >/dev/null || ! pgrep -x ceph-mgr >/dev/null; then
    echo "ERROR: MON and MGR must already be running." >&2
    exit 1
fi

export CEPH_DEV=1
export LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$PYDEPS_DIR:$SOURCE_DIR/src/pybind:$SOURCE_DIR/src/python-common${PYTHONPATH:+:$PYTHONPATH}"

run_as_ceph_user() {
    sudo -u "$RUN_USER" env \
        CEPH_DEV="$CEPH_DEV" \
        LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
        PYTHONPATH="$PYTHONPATH" \
        "$@"
}

if ! run_as_ceph_user "$BIN_DIR/ceph" --connect-timeout 5 -s >/dev/null 2>&1; then
    echo "ERROR: cannot contact the local cluster as $RUN_USER." >&2
    exit 1
fi

echo "Erasing and partitioning $DEVICE into three equal GPT partitions ..."
wipefs --all --force "$DEVICE"
sgdisk --zap-all "$DEVICE"

sector_size="$(blockdev --getss "$DEVICE")"
total_sectors="$(blockdev --getsz "$DEVICE")"
alignment=$((1024 * 1024 / sector_size))
first_sector="$alignment"
last_usable=$((total_sectors - 34))
usable=$((last_usable - first_sector + 1))
chunk=$((usable / 3 / alignment * alignment))

p1_start="$first_sector"
p1_end=$((p1_start + chunk - 1))
p2_start=$((p1_end + 1))
p2_end=$((p2_start + chunk - 1))
p3_start=$((p2_end + 1))
p3_end="$last_usable"

sgdisk \
    --new="1:${p1_start}:${p1_end}" --typecode="1:$CEPH_OSD_GUID" --change-name='1:ceph-osd-0' \
    --new="2:${p2_start}:${p2_end}" --typecode="2:$CEPH_OSD_GUID" --change-name='2:ceph-osd-1' \
    --new="3:${p3_start}:${p3_end}" --typecode="3:$CEPH_OSD_GUID" --change-name='3:ceph-osd-2' \
    "$DEVICE"
partprobe "$DEVICE"
udevadm settle

for partition in "${DEVICE}1" "${DEVICE}2" "${DEVICE}3"; do
    if [[ ! -b "$partition" ]]; then
        echo "ERROR: kernel did not expose $partition." >&2
        exit 1
    fi
    wipefs --all --force "$partition"
    chown "$RUN_USER:$RUN_GROUP" "$partition"
    chmod 660 "$partition"
done

mkdir -p "$OSD_BASE"
chown "$RUN_USER:$RUN_GROUP" "$OSD_BASE"

for partition in "${DEVICE}1" "${DEVICE}2" "${DEVICE}3"; do
    osd_uuid="$(cat /proc/sys/kernel/random/uuid)"
    osd_id="$(run_as_ceph_user "$BIN_DIR/ceph" osd create "$osd_uuid" | tr -d '[:space:]')"
    if [[ ! "$osd_id" =~ ^[0-9]+$ ]]; then
        echo "ERROR: invalid OSD id '$osd_id' for $partition." >&2
        exit 1
    fi

    osd_dir="$OSD_BASE/ceph-$osd_id"
    mkdir -p "$osd_dir"
    chown "$RUN_USER:$RUN_GROUP" "$osd_dir"
    ln -s "$partition" "$osd_dir/block"
    chown -h "$RUN_USER:$RUN_GROUP" "$osd_dir/block"

    run_as_ceph_user "$BIN_DIR/ceph" auth get-or-create "osd.$osd_id" \
        mon 'allow profile osd' mgr 'allow profile osd' osd 'allow *' \
        -o "$osd_dir/keyring"
    chown "$RUN_USER:$RUN_GROUP" "$osd_dir/keyring"

    run_as_ceph_user "$BIN_DIR/ceph-osd" -i "$osd_id" --mkfs \
        --osd-uuid "$osd_uuid" --osd-data "$osd_dir"
    run_as_ceph_user "$BIN_DIR/ceph" osd crush add "osd.$osd_id" 1.0 \
        "host=$HOST_BUCKET" root=default
    run_as_ceph_user "$BIN_DIR/ceph-osd" -i "$osd_id" --osd-data "$osd_dir"

    for attempt in $(seq 1 120); do
        if run_as_ceph_user "$BIN_DIR/ceph" osd dump -f json 2>/dev/null \
                | /usr/bin/python3.10 -c \
                    'import json,sys; oid=int(sys.argv[1]); data=json.load(sys.stdin); sys.exit(0 if any(o["osd"] == oid and o["up"] == 1 and o["in"] == 1 for o in data["osds"]) else 1)' \
                    "$osd_id"; then
            echo "Started osd.$osd_id on $partition (up/in)"
            break
        fi
        if [[ $attempt -eq 120 ]]; then
            echo "ERROR: osd.$osd_id did not become up/in within 120 seconds." >&2
            exit 1
        fi
        sleep 1
    done
done

echo
lsblk -o NAME,SIZE,TYPE,FSTYPE,PARTLABEL,PARTTYPE,MOUNTPOINTS "$DEVICE"
echo
run_as_ceph_user "$BIN_DIR/ceph" osd tree
run_as_ceph_user "$BIN_DIR/ceph" -s

