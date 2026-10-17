#!/bin/bash
# 脚本说明：
# 不使用 ceph-volume，直接用 build/bin 下的 ceph/ceph-osd 在本机裸盘上
# 创建并启动 bluestore OSD（整盘，不分区）
# 使用条件：
# 1. 集群 mon/mgr 已用 rebuildCeph.sh 启动
#    （/etc/ceph/ceph.conf 和 ceph.client.admin.keyring 就绪）
# 2. 盘已清理（wipefs）
# 3. ceph-osd 需要写裸盘，脚本内部对 ceph-osd 自动使用 sudo；
#    ceph-osd 启动后会按 ceph.conf 的 setuser match path 降权到 cyf
# 停止 OSD： killall ceph-osd

# ---------- 配置 ----------
# 本机的 SSD 设备列表（设备名，不带 /dev/ 前缀）
# 每次重启服务器编号可能变化，需手动核对
osd_list_node1=(nvme0n1 nvme1n1 nvme2n1 nvme3n1)

BUILD_DIR="/home/cyf/ceph-dpu/build"
BIN="$BUILD_DIR/bin"
OSD_BASE="/var/lib/ceph/osd"

export PYTHONPATH=/home/cyf/ceph-dpu/src/pybind
export LD_LIBRARY_PATH="/usr/local/lib/ceph/dpu:$BUILD_DIR/lib:${LD_LIBRARY_PATH}"

if [ ! -x "$BIN/ceph-osd" ] || [ ! -x "$BIN/ceph" ]; then
    echo "❌ $BIN 下找不到 ceph-osd/ceph，请先编译"
    exit 1
fi

# ceph-osd 需要写裸盘：root 直接跑，非 root 用 sudo 并带上环境变量
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo env PYTHONPATH=$PYTHONPATH LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
fi

# ---------- 检查参数（可选：指定列表索引范围，如 0-3） ----------
if [ $# -gt 1 ]; then
    echo "用法: $0 [索引范围]"
    echo "示例: $0          # 在列表中所有SSD上创建OSD"
    echo "示例: $0 0-2      # 在列表的前3个SSD上创建OSD"
    exit 1
fi

start_idx=0
end_idx=-1  # -1 表示到最后

if [ $# -eq 1 ]; then
    range="$1"
    if [[ ! "$range" =~ ^[0-9]+-[0-9]+$ ]]; then
        echo "错误: 参数格式不正确，请使用 '起始索引-结束索引' 格式"
        echo "示例: 0-3"
        exit 1
    fi
    start_idx=$(echo "$range" | cut -d'-' -f1)
    end_idx=$(echo "$range" | cut -d'-' -f2)
    if [ "$start_idx" -gt "$end_idx" ]; then
        echo "错误: 起始索引不能大于结束索引"
        exit 1
    fi
fi

list_len=${#osd_list_node1[@]}
if [ "$end_idx" -eq -1 ] || [ "$end_idx" -ge "$list_len" ]; then
    actual_end=$((list_len - 1))
else
    actual_end="$end_idx"
fi

if [ "$start_idx" -ge "$list_len" ]; then
    echo "错误: 起始索引 $start_idx 超出列表长度 $list_len"
    exit 1
fi

# ---------- 前置检查 ----------
# ceph CLI 走默认路径 /etc/ceph/ceph.conf 和 admin keyring（由 rebuildCeph.sh 准备）
if ! "$BIN/ceph" -s >/dev/null 2>&1; then
    echo "❌ 无法连接集群，请先用 rebuildCeph.sh 启动 mon/mgr"
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    mkdir -p "$OSD_BASE"
else
    sudo mkdir -p "$OSD_BASE"
    # mkfs 由 root 执行后目录属主是 root，启动时 ceph-osd 降权到 cyf 需要可写
    sudo chown "$(id -u):$(id -g)" "$OSD_BASE"
fi

echo "==== 将在本机的设备索引 $start_idx-$actual_end 上创建 OSD ===="

# ---------- 逐盘创建 OSD ----------
for ((i=start_idx; i<=actual_end; i++)); do
    dev_name="${osd_list_node1[$i]}"
    device="/dev/${dev_name}"

    echo "---- 为 $device 创建 OSD ----"
    if [ ! -b "$device" ]; then
        echo "⚠️ 设备 $device 不存在，跳过"
        continue
    fi

    # 1. 生成 osd uuid，向 mon 申请 osd id
    uuid=$(uuidgen 2>/dev/null || cat /proc/sys/kernel/random/uuid)
    osd_id=$("$BIN/ceph" osd create "$uuid" 2>/dev/null | tr -d '[:space:]')
    if [ -z "$osd_id" ] || ! [[ "$osd_id" =~ ^[0-9]+$ ]]; then
        echo "❌ 为 $device 申请 osd id 失败（返回: '$osd_id'），跳过"
        continue
    fi
    echo "osd id = $osd_id, uuid = $uuid"

    # 2. 准备 osd 数据目录，block 软链指向裸盘（bluestore 元数据全部写在盘上，
    #    目录里只有 keyring、block 链接等少量文件，不需要 tmpfs/挂载）
    osd_dir="$OSD_BASE/ceph-$osd_id"
    rm -rf "$osd_dir"
    mkdir -p "$osd_dir"
    ln -sf "$device" "$osd_dir/block"

    # 3. 从 mon 获取该 osd 的 keyring（需要 admin keyring）
    if ! "$BIN/ceph" auth get-or-create "osd.$osd_id" \
            osd 'allow *' mon 'allow profile osd' \
            -o "$osd_dir/keyring" >/dev/null 2>&1; then
        echo "❌ 为 osd.$osd_id 生成 keyring 失败，跳过"
        continue
    fi

    # 4. mkfs：把 bluestore 格式写到裸盘上
    if ! $SUDO "$BIN/ceph-osd" -i "$osd_id" --mkfs \
            --osd-uuid "$uuid" --osd-data "$osd_dir"; then
        echo "❌ osd.$osd_id ($device) mkfs 失败，跳过"
        continue
    fi
    echo "✅ osd.$osd_id ($device) mkfs 完成"

    # 5. 启动 OSD（默认 daemonize 到后台；没有 systemd，直接起进程）
    if $SUDO "$BIN/ceph-osd" -i "$osd_id" --osd-data "$osd_dir"; then
        echo "✅ osd.$osd_id ($device) 已启动"
    else
        echo "❌ osd.$osd_id ($device) 启动失败，日志: /var/log/ceph/ceph-osd.$osd_id.log"
    fi
done

echo ""
echo "==== 创建完成，当前集群状态 ===="
"$BIN/ceph" osd tree
