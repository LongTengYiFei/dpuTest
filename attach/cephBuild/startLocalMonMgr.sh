#!/usr/bin/env bash
set -Eeuo pipefail

# Rebuild and start a single-node Ceph monitor and manager from the local
# development build. This script intentionally does not use systemd or an
# installed Ceph package.

BUILD_DIR="/home/cyf/githubDownload/ceph/build"
SOURCE_DIR="/home/cyf/githubDownload/ceph"
BIN_DIR="$BUILD_DIR/bin"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_DIR="$SCRIPT_DIR/local-cluster"
RUN_DIR="$STATE_DIR/run"
LOG_DIR="$STATE_DIR/log"
CRASH_DIR="$STATE_DIR/crash"
PYDEPS_DIR="$STATE_DIR/pydeps"
CONF_DIR="/etc/ceph"
CONF_FILE="$CONF_DIR/ceph.conf"
CLUSTER="ceph"
FSID="886cee71-e800-4d78-95f4-43b791bfeb50"
MON_NAME="node1"
MON_IP="10.26.57.2"
MGR_NAME="m1"
CEPH_DATA="/var/lib/ceph"

for program in ceph ceph-authtool ceph-mon ceph-mgr monmaptool; do
    if [[ ! -x "$BIN_DIR/$program" ]]; then
        echo "ERROR: missing executable: $BIN_DIR/$program" >&2
        exit 1
    fi
done

if [[ $(id -u) -eq 0 ]]; then
    echo "ERROR: run this script as user cyf, not root." >&2
    exit 1
fi

if [[ $(id -un) != "cyf" ]]; then
    echo "ERROR: this development cluster is configured for user cyf." >&2
    exit 1
fi

if ! ip -4 addr show | grep -q "inet ${MON_IP}/"; then
    echo "ERROR: $MON_IP is not configured on this host." >&2
    exit 1
fi

if pgrep -x ceph-mon >/dev/null || pgrep -x ceph-mgr >/dev/null || pgrep -x ceph-osd >/dev/null; then
    echo "ERROR: Ceph daemon processes are already running; refusing to replace their data." >&2
    pgrep -a 'ceph-(mon|mgr|osd)' || true
    exit 1
fi

if [[ ! -d "$CONF_DIR" || ! -w "$CONF_DIR" ]]; then
    echo "ERROR: $CONF_DIR must exist and be writable by user cyf." >&2
    exit 1
fi
if [[ ! -d "$CEPH_DATA" || ! -w "$CEPH_DATA" ]]; then
    echo "ERROR: $CEPH_DATA must exist and be writable by user cyf." >&2
    exit 1
fi

mkdir -p "$RUN_DIR" "$LOG_DIR" "$CRASH_DIR" "$PYDEPS_DIR"

# The build-time ceph CLI imports these packages, and the required wheels are
# already shipped in the Ceph source tree. Extract locally without networking.
if [[ ! -d "$PYDEPS_DIR/prettytable" ]]; then
    unzip -oq "$SOURCE_DIR/src/pybind/mgr/wheelhouse/prettytable-3.3.0-py3-none-any.whl" -d "$PYDEPS_DIR"
fi
if [[ ! -d "$PYDEPS_DIR/wcwidth" ]]; then
    unzip -oq "$SOURCE_DIR/src/pybind/mgr/wheelhouse/wcwidth-0.2.13-py2.py3-none-any.whl" -d "$PYDEPS_DIR"
fi
for wheel in \
    pecan-1.5.1-py3-none-any.whl \
    WebOb-1.8.7-py2.py3-none-any.whl \
    Mako-1.3.5-py3-none-any.whl \
    MarkupSafe-2.1.5-cp310-cp310-manylinux_2_17_x86_64.manylinux2014_x86_64.whl \
    logutils-0.3.5-py3-none-any.whl; do
    unzip -oq "$SOURCE_DIR/src/pybind/mgr/wheelhouse/$wheel" -d "$PYDEPS_DIR"
done

export CEPH_DEV=1
export LD_LIBRARY_PATH="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$PYDEPS_DIR:$SOURCE_DIR/src/pybind:$SOURCE_DIR/src/python-common${PYTHONPATH:+:$PYTHONPATH}"

timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
if [[ -f "$CONF_FILE" ]]; then
    cp -a "$CONF_FILE" "$STATE_DIR/ceph.conf.before-$timestamp"
fi

cat >"$STATE_DIR/ceph.conf.new" <<EOF
[global]
fsid = $FSID
mon initial members = $MON_NAME
mon host = [v2:$MON_IP:3300,v1:$MON_IP:6789]
public network = 10.26.57.0/24
cluster network = 10.26.57.0/24
auth cluster required = cephx
auth service required = cephx
auth client required = cephx
ms type = async+posix
ms cluster type = async+posix
osd pool default size = 3
osd pool default min size = 2
osd crush chooseleaf type = 0
mon allow pool delete = true
run dir = $RUN_DIR
log file = $LOG_DIR/ceph-\$name.log
crash dir = $CRASH_DIR
setuser match path = $CEPH_DATA
setgroup match path = $CEPH_DATA
erasure code dir = $BUILD_DIR/lib
plugin dir = $BUILD_DIR/lib
osd class dir = $BUILD_DIR/lib

[client.admin]
keyring = $CONF_DIR/ceph.client.admin.keyring

[client.bootstrap-osd]
keyring = $CEPH_DATA/bootstrap-osd/ceph.keyring
EOF
mv "$STATE_DIR/ceph.conf.new" "$CONF_FILE"

# This is a deliberate fresh-cluster rebuild. The exact target is fixed above.
find "$CEPH_DATA" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
mkdir -p \
    "$CEPH_DATA/bootstrap-osd" \
    "$CEPH_DATA/mgr/$CLUSTER-$MGR_NAME" \
    "$CEPH_DATA/mon/$CLUSTER-$MON_NAME" \
    "$CEPH_DATA/osd"

rm -f \
    "$CONF_DIR/monmap" \
    "$CONF_DIR/ceph.mon.keyring" \
    "$CONF_DIR/ceph.client.admin.keyring"

"$BIN_DIR/ceph-authtool" --create-keyring "$CONF_DIR/ceph.mon.keyring" \
    --gen-key -n mon. --cap mon 'allow *'
"$BIN_DIR/ceph-authtool" --create-keyring "$CONF_DIR/ceph.client.admin.keyring" \
    --gen-key -n client.admin --cap mon 'allow *' --cap osd 'allow *' \
    --cap mds 'allow *' --cap mgr 'allow *'
"$BIN_DIR/ceph-authtool" --create-keyring "$CEPH_DATA/bootstrap-osd/ceph.keyring" \
    --gen-key -n client.bootstrap-osd --cap mon 'profile bootstrap-osd' --cap mgr 'allow r'
"$BIN_DIR/ceph-authtool" "$CONF_DIR/ceph.mon.keyring" \
    --import-keyring "$CONF_DIR/ceph.client.admin.keyring"
"$BIN_DIR/ceph-authtool" "$CONF_DIR/ceph.mon.keyring" \
    --import-keyring "$CEPH_DATA/bootstrap-osd/ceph.keyring"

"$BIN_DIR/monmaptool" --create --addv "$MON_NAME" "[v2:$MON_IP:3300,v1:$MON_IP:6789]" \
    --fsid "$FSID" "$CONF_DIR/monmap"
"$BIN_DIR/ceph-mon" --cluster "$CLUSTER" --mkfs -i "$MON_NAME" \
    --monmap "$CONF_DIR/monmap" --keyring "$CONF_DIR/ceph.mon.keyring"

echo "Starting ceph-mon.$MON_NAME ..."
"$BIN_DIR/ceph-mon" --cluster "$CLUSTER" -i "$MON_NAME"

for attempt in $(seq 1 30); do
    if "$BIN_DIR/ceph" --cluster "$CLUSTER" --connect-timeout 5 -s >/dev/null 2>&1; then
        break
    fi
    if [[ $attempt -eq 30 ]]; then
        echo "ERROR: monitor did not become ready; see $LOG_DIR/ceph-mon.$MON_NAME.log" >&2
        exit 1
    fi
    sleep 1
done

"$BIN_DIR/ceph" --cluster "$CLUSTER" auth get-or-create "mgr.$MGR_NAME" \
    mon 'allow profile mgr' osd 'allow *' mds 'allow *' \
    -o "$CEPH_DATA/mgr/$CLUSTER-$MGR_NAME/keyring"

echo "Starting ceph-mgr.$MGR_NAME ..."
"$BIN_DIR/ceph-mgr" --cluster "$CLUSTER" -i "$MGR_NAME" \
    --mgr-module-path "$SOURCE_DIR/src/pybind/mgr"

for attempt in $(seq 1 30); do
    if "$BIN_DIR/ceph" --cluster "$CLUSTER" --connect-timeout 5 -s 2>/dev/null | grep -q "mgr: $MGR_NAME(active"; then
        break
    fi
    if [[ $attempt -eq 30 ]]; then
        echo "ERROR: manager did not become active; see $LOG_DIR/ceph-mgr.$MGR_NAME.log" >&2
        exit 1
    fi
    sleep 1
done

# This local test cluster does not use the REST API. Disabling it avoids a
# health warning when optional dashboard Python dependencies are not installed.
"$BIN_DIR/ceph" --cluster "$CLUSTER" mgr module disable restful >/dev/null
"$BIN_DIR/ceph" --cluster "$CLUSTER" config set mon \
    auth_allow_insecure_global_id_reclaim false

echo
"$BIN_DIR/ceph" --cluster "$CLUSTER" -s
echo "MON and MGR are ready."

