#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=baseline1.env
source "$SCRIPT_DIR/baseline1.env"

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

if [[ ! -f $TARGET_PID_FILE ]]; then
    echo "No target PID file"
    exit 0
fi
pid=$(<"$TARGET_PID_FILE")
if kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid"
    for _ in {1..100}; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
fi
rm -f "$TARGET_PID_FILE"
echo "Stopped target PID $pid"
