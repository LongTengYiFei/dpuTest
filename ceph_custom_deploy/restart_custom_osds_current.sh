#!/usr/bin/env bash
set -euo pipefail

ceph=/home/cyf/ceph-dpu/build/bin/ceph
ssh_port=23579
entries=(
  node1:0 node2:1 node3:2 node1:3 node2:4 node3:5
  node1:6 node2:7 node3:8 node1:9 node2:10 node3:11
)

wait_clean() {
  local deadline=$((SECONDS + 240))
  while (( SECONDS < deadline )); do
    if json=$($ceph -s --format json 2>/dev/null) && \
       python3 -c '
import json, sys
s = json.load(sys.stdin)
o = s["osdmap"]
p = s["pgmap"]
states = p.get("pgs_by_state", [])
ok = (o["num_up_osds"] == 12 and o["num_in_osds"] == 12 and
      p.get("num_pgs", 0) > 0 and
      all(x["state_name"] == "active+clean" for x in states))
raise SystemExit(0 if ok else 1)
' <<<"$json"; then
      return 0
    fi
    sleep 2
  done
  $ceph -s
  return 1
}

unset_noout() {
  $ceph osd unset noout >/dev/null 2>&1 || true
}

$ceph osd set noout
trap unset_noout EXIT

for entry in "${entries[@]}"; do
  host=${entry%%:*}
  id=${entry##*:}
  echo "restarting custom osd.$id on $host"
  ssh -p "$ssh_port" "$host" "sudo -n systemctl restart ceph-custom-osd@$id"
  ssh -p "$ssh_port" "$host" "systemctl is-active --quiet ceph-custom-osd@$id"
  wait_clean
done

$ceph osd unset noout
trap - EXIT
$ceph -s
