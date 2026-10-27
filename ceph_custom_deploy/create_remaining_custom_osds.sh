#!/usr/bin/env bash
set -euo pipefail

ssh_port=23579
ceph_host=node1
ceph_bin=/home/cyf/ceph-dpu/build/bin/ceph
build_dir=/home/cyf/ceph-dpu/build
library_path=/usr/local/lib/ceph/dpu:/home/cyf/ceph-dpu/build/lib:/opt/mellanox/doca/lib/x86_64-linux-gnu

entries=(
  "1:node2:/dev/nvme1n1"
  "2:node3:/dev/nvme1n1"
  "3:node1:/dev/nvme1n1"
  "4:node2:/dev/nvme2n1"
  "5:node3:/dev/nvme2n1"
  "6:node1:/dev/nvme2n1"
  "7:node2:/dev/nvme3n1"
  "8:node3:/dev/nvme3n1"
  "9:node1:/dev/nvme3n1"
  "10:node2:/dev/nvme4n1"
  "11:node3:/dev/nvme4n1"
)

key_tmp_dir=$(mktemp -d /tmp/ceph-custom-osd-keys.XXXXXX)

for entry in "${entries[@]}"; do
  IFS=: read -r id host device <<<"$entry"
  uuid=$(uuidgen)
  node1_key="/tmp/ceph-custom-osd-${id}.keyring"
  target_key="/tmp/ceph-custom-osd-${id}.install.keyring"
  local_key="$key_tmp_dir/osd.${id}.keyring"

  echo "===== creating osd.$id on $host:$device uuid=$uuid ====="
  ssh -p "$ssh_port" "$ceph_host" \
    "cd $build_dir/bin && ./ceph osd create '$uuid' '$id'"
  ssh -p "$ssh_port" "$ceph_host" \
    "umask 077; cd $build_dir/bin && ./ceph auth get-or-create osd.$id osd 'allow *' mon 'allow profile osd' -o '$node1_key'"
  scp -q -P "$ssh_port" "$ceph_host:$node1_key" "$local_key"
  scp -q -P "$ssh_port" "$local_key" "$host:$target_key"

  ssh -p "$ssh_port" "$host" "
    set -e
    sudo -n install -d -o root -g root -m 0750 /var/lib/ceph/osd/ceph-$id
    sudo -n ln -s '$device' /var/lib/ceph/osd/ceph-$id/block
    sudo -n install -o root -g root -m 0600 '$target_key' /var/lib/ceph/osd/ceph-$id/keyring
    set +e
    sudo -n prlimit --memlock=unlimited:unlimited -- \
      env LD_LIBRARY_PATH='$library_path' \
      $build_dir/bin/ceph-osd -i '$id' --mkfs --osd-uuid '$uuid' \
      --osd-data /var/lib/ceph/osd/ceph-$id \
      --osd-class-dir $build_dir/lib \
      >/tmp/ceph-custom-osd-$id.mkfs.log 2>&1
    mkfs_rc=\$?
    set -e
    if ! sudo -n test -f /var/lib/ceph/osd/ceph-$id/ready; then
      echo 'osd.$id mkfs failed before ready marker; rc='\$mkfs_rc >&2
      sudo -n tail -n 120 /tmp/ceph-custom-osd-$id.mkfs.log >&2
      exit 1
    fi
    echo 'osd.$id mkfs ready; rc='\$mkfs_rc
    sudo -n systemctl enable --now ceph-custom-osd@$id
    systemctl is-active --quiet ceph-custom-osd@$id
  "

  unlink "$local_key"
  ssh -p "$ssh_port" "$ceph_host" "unlink '$node1_key'"
  ssh -p "$ssh_port" "$host" "unlink '$target_key'"
  echo "osd.$id custom service active"
done

rmdir "$key_tmp_dir"
echo "all remaining custom OSD services started"
