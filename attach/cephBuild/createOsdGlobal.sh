#!/bin/bash
# 脚本说明：
# 在每块 NVMe 盘上直接创建 OSD（不使用分区）
# 通过 SSH 远程在各节点上执行，每个节点使用各自的 SSD 列表
# 使用条件：
# 必须是全新集群，且盘已清理（wipefs等）

# 各节点的 SSD 设备列表（设备名，不带 /dev/ 前缀）
# 每次重启服务器编号可能变化，需手动核对
osd_list_node1=(nvme0n1 nvme1n1 nvme2n1 nvme3n1)
osd_list_node2=(nvme1n1 nvme2n1 nvme3n1 nvme4n1)
osd_list_node3=(nvme1n1 nvme2n1 nvme3n1 nvme4n1)

# 节点信息
dest_ips=("10.26.57.54" "10.26.57.55" "10.26.57.56")
ssh_port=23579
user="cyf"

# 环境变量
export PYTHONPATH=/home/cyf/ceph-dpu/src/pybind
export LD_LIBRARY_PATH=/usr/local/lib/ceph/dpu

# ---- SSH 连接复用设置 ----
MUX_DIR="/tmp/ceph_createosd_mux_$$"
mkdir -p "$MUX_DIR"
MUX_OPTS="-o ControlMaster=auto -o ControlPath=${MUX_DIR}/mux_%h -o ControlPersist=60"

cleanup_mux() {
    for node in "${dest_ips[@]}"; do
        ssh -p "${ssh_port}" $MUX_OPTS -O exit "${user}@${node}" 2>/dev/null
    done
    rm -rf "$MUX_DIR"
}
trap cleanup_mux EXIT

# 检测某 IP 是否是本机
is_local() {
    hostname -I 2>/dev/null | tr ' ' '\n' | grep -qw "$1"
}

# 检查参数（可选：指定列表索引范围，如 0-3 表示每个节点列表的第0到第3个设备）
if [ $# -gt 1 ]; then
    echo "用法: $0 [索引范围]"
    echo "示例: $0          # 在每个节点的所有SSD上创建OSD"
    echo "示例: $0 0-2      # 在每个节点列表的前3个SSD上创建OSD"
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

echo "==== 将在各节点的指定 SSD 上创建 OSD ===="

# 遍历每个节点
for idx in "${!dest_ips[@]}"; do
    node="${dest_ips[$idx]}"
    node_num=$((idx + 1))
    # 获取该节点的 SSD 列表
    list_name="osd_list_node${node_num}[@]"
    ssd_list=("${!list_name}")

    echo "==== 处理节点 $node (node${node_num}) ===="

    # 确定实际的结束索引
    list_len=${#ssd_list[@]}
    if [ "$end_idx" -eq -1 ] || [ "$end_idx" -ge "$list_len" ]; then
        actual_end=$((list_len - 1))
    else
        actual_end="$end_idx"
    fi

    if [ "$start_idx" -ge "$list_len" ]; then
        echo "⚠️ 起始索引 $start_idx 超出节点 $node 的列表长度 $list_len，跳过"
        continue
    fi

    echo "节点 $node 将使用设备索引 $start_idx-$actual_end: ${ssd_list[@]:$start_idx:$((actual_end - start_idx + 1))}"

    # 判断是否是本机
    if is_local "$node"; then
        echo "（检测到本机，直接本地执行）"

        # 确保本地 /var/lib/ceph/osd 目录存在（ceph-volume 要在此挂载 tmpfs）
        if [ "$(id -u)" -eq 0 ]; then
            mkdir -p /var/lib/ceph/osd
        else
            sudo mkdir -p /var/lib/ceph/osd
        fi

        # 遍历该节点的 SSD 列表创建 OSD
        for ((i=start_idx; i<=actual_end; i++)); do
            dev_name="${ssd_list[$i]}"
            device="/dev/${dev_name}"

            echo "---- 本地为 $device 创建 OSD ----"
            if [ ! -b "$device" ]; then
                echo "⚠️ 设备 $device 不存在，跳过"
                continue
            fi

            # 本地执行：root 直接跑，非 root 用 sudo
            if [ "$(id -u)" -eq 0 ]; then
                ceph-volume raw prepare --bluestore --data "$device"
            else
                sudo env PYTHONPATH="$PYTHONPATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" ceph-volume raw prepare --bluestore --data "$device"
            fi

            if [ $? -eq 0 ]; then
                echo "✅ 成功在本机为 $device 创建 OSD"
            else
                echo "❌ 在本机为 $device 创建 OSD 失败"
            fi
        done
    else
        # 远程节点：通过 SSH 执行
        # 建立该节点的 SSH 主连接
        echo "建立到 $node 的 SSH 主连接 ..."
        if ! ssh -p "${ssh_port}" -fN -o ControlMaster=yes -o ControlPath="${MUX_DIR}/mux_%h" -o ControlPersist=60 "${user}@${node}"; then
            echo "❌ 无法建立到 $node 的连接，跳过该节点"
            continue
        fi

        # 确保远端 /var/lib/ceph/osd 目录存在（ceph-volume 要在此挂载 tmpfs）
        ssh -t -p "${ssh_port}" $MUX_OPTS "${user}@${node}" "sudo mkdir -p /var/lib/ceph/osd"

        # 遍历该节点的 SSD 列表创建 OSD
        for ((i=start_idx; i<=actual_end; i++)); do
            dev_name="${ssd_list[$i]}"
            device="/dev/${dev_name}"

            echo "---- 在 $node 上为 $device 创建 OSD ----"
            if ssh -t -p "${ssh_port}" $MUX_OPTS "${user}@${node}" "export PYTHONPATH=/home/cyf/ceph-dpu/src/pybind LD_LIBRARY_PATH=/usr/local/lib/ceph/dpu; if [ -b '$device' ]; then sudo env PYTHONPATH=\$PYTHONPATH LD_LIBRARY_PATH=\$LD_LIBRARY_PATH ceph-volume raw prepare --bluestore --data '$device'; else echo '设备 $device 不存在'; exit 1; fi"; then
                echo "✅ 成功在 $node 为 $device 创建 OSD"
            else
                echo "❌ 在 $node 为 $device 创建 OSD 失败"
            fi
        done
    fi
    echo ""
done

echo "==== 所有节点 OSD 创建完成 ===="
