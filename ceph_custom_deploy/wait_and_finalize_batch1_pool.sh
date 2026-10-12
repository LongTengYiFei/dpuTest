#!/usr/bin/env bash
set -euo pipefail

main_unit=ceph-batch1-pool-ablation-20260921.service
while systemctl is-active --quiet "$main_unit"; do
  sleep 30
done

result=$(systemctl show "$main_unit" -p Result --value)
if [[ $result != success ]]; then
  echo "$main_unit ended with Result=$result; refusing to publish final figure" >&2
  exit 1
fi

exec /home/cyf/dpuTest/ceph_custom_deploy/finalize_batch1_pool_ablation.py
