#!/bin/bash
# 使用前提：ceph.conf.backup 已配置好 fsid 和基本参数

# ---------- 构建本地 ceph.conf ----------
ceph_conf_backup="./ceph.conf.backup"
ceph_dir="/etc/ceph"

echo "检查本地 $ceph_dir 目录..."
if [ ! -d "$ceph_dir" ]; then
    echo "目录不存在，创建 $ceph_dir ..."
    sudo mkdir -p "$ceph_dir"
    if [ $? -ne 0 ]; then
        echo "❌ 创建本地 $ceph_dir 失败，请检查权限"
        exit 1
    fi
fi

# 拷贝备份文件覆盖
if [ -f "$ceph_conf_backup" ]; then
    echo "拷贝 $ceph_conf_backup 到 $ceph_dir/ceph.conf ..."
    sudo cp -f "$ceph_conf_backup" "$ceph_dir/ceph.conf"
    if [ $? -eq 0 ]; then
        echo "✅ 本地 ceph.conf 已更新"
    else
        echo "❌ 拷贝失败，请检查权限"
        exit 1
    fi
else
    echo "⚠️ 警告：$ceph_conf_backup 不存在，跳过本地构建"
fi

# ---------- 更新本机 mlx5_1 的 GID ----------
CONF_FILE="/etc/ceph/ceph.conf"
if [ ! -f "$CONF_FILE" ]; then
    echo "错误: $CONF_FILE 不存在"
    exit 1
fi

GID_OUT=$(show_gids 2>/dev/null)
if [ $? -ne 0 ] || [ -z "$GID_OUT" ]; then
    echo "错误: 无法执行 show_gids 或输出为空"
    exit 1
fi

NEW_GID=$(echo "$GID_OUT" | awk '$1 == "mlx5_1" {print $4}')
if [ -z "$NEW_GID" ]; then
    echo "错误: 未找到 mlx5_1 的 GID"
    exit 1
fi
echo "找到 mlx5_1 的 GID: $NEW_GID"

sudo cp "$CONF_FILE" "${CONF_FILE}.bak"
sudo sed -i "s/^ms_async_rdma_local_gid *=.*/ms_async_rdma_local_gid = $NEW_GID/" "$CONF_FILE"
if [ $? -eq 0 ]; then
    echo "✅ 已更新 $CONF_FILE 中的 ms_async_rdma_local_gid 为 $NEW_GID"
else
    echo "❌ 更新失败"
    exit 1
fi

# ---------- 🔧 自动修复配置文件格式（在所有节之间插入空行） ----------
# 避免 "expected '<empty_line>'" 解析错误
sudo cp "$CONF_FILE" "${CONF_FILE}.bak2"   # 额外备份
sudo awk '
BEGIN { first=1 }
/^\[/ {
    if (!first) print ""   # 如果不是第一个节，先打印空行
    first=0
}
{ print }
' "$CONF_FILE" > /tmp/ceph.conf.fixed
sudo mv /tmp/ceph.conf.fixed "$CONF_FILE"
echo "✅ 已自动格式化配置文件，确保每个节之间都有空行"

# ---------- 获取 fsid ----------
get_fsid() {
    local config_file="$1"
    if [[ ! -f "$config_file" ]]; then
        echo "错误：文件 '$config_file' 不存在。" >&2
        return 1
    fi
    local fsid
    fsid=$(awk -F' *= *' '/^\s*fsid\s*=/ {print $2; exit}' "$config_file")
    if [[ -z "$fsid" ]]; then
        echo "错误：在文件 '$config_file' 中未找到 'fsid'。" >&2
        return 1
    fi
    echo "$fsid"
}

mon_ip="10.26.57.54"
mon_name="node1"
fsid=$(get_fsid "/etc/ceph/ceph.conf")

# ---------- 部署 mon ----------
rm -fr /var/lib/ceph
mkdir -p /var/lib/ceph/bootstrap-osd /var/lib/ceph/mgr /var/lib/ceph/osd
mkdir -p /var/lib/ceph/mon/ceph-$mon_name

rm -f /etc/ceph/monmap /etc/ceph/ceph.mon.keyring /etc/ceph/ceph.client.admin.keyring

ceph-authtool --create-keyring /etc/ceph/ceph.mon.keyring --gen-key -n mon. --cap mon 'allow *'
ceph-authtool --create-keyring /etc/ceph/ceph.client.admin.keyring --gen-key -n client.admin --cap mon 'allow *' --cap osd 'allow *' --cap mds 'allow *' --cap mgr 'allow *'
sudo ceph-authtool --create-keyring /var/lib/ceph/bootstrap-osd/ceph.keyring --gen-key -n client.bootstrap-osd --cap mon 'profile bootstrap-osd' --cap mgr 'allow r'

sudo ceph-authtool /etc/ceph/ceph.mon.keyring --import-keyring /etc/ceph/ceph.client.admin.keyring
sudo ceph-authtool /etc/ceph/ceph.mon.keyring --import-keyring /var/lib/ceph/bootstrap-osd/ceph.keyring

monmaptool --create --add $mon_name $mon_ip --fsid $fsid /etc/ceph/monmap

ceph-mon --mkfs -i $mon_name --monmap /etc/ceph/monmap --keyring /etc/ceph/ceph.mon.keyring

chown -R cyf:cyf /var/lib/ceph
chown -R cyf:cyf /etc/ceph

sudo systemctl restart ceph-mon@$mon_name

# ---------- 部署 mgr ----------
mkdir -p /var/lib/ceph/mgr/ceph-m1
touch /var/lib/ceph/mgr/ceph-m1/keyring
AUTH_OUTPUT=$(/home/cyf/ceph-dpu/build/bin/ceph auth get-or-create mgr.m1 mon 'allow profile mgr' osd 'allow *' mds 'allow *' | tail -n +1)
echo "$AUTH_OUTPUT" >> /var/lib/ceph/mgr/ceph-m1/keyring
/home/cyf/ceph-dpu/build/bin/ceph-mgr -i m1