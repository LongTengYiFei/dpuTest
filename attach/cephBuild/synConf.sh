#!/bin/bash
# 多文件同步到多节点，SSH端口23579
# 同步时会根据目标节点的 mlx5_1 GID 自动更新 ceph.conf 中的 ms_async_rdma_local_gid
# 使用 SSH ControlMaster 连接复用：每个节点只需输入一次密码

dest_ips=("10.26.57.55" "10.26.57.56" "10.26.57.57")
syn_files=("/var/lib/ceph/bootstrap-osd/ceph.keyring"
           "/etc/ceph/ceph.client.admin.keyring"
           "/etc/ceph/ceph.conf")

ssh_port=23579
user="cyf"

# ---- SSH 连接复用设置 ----
# 每个节点建立一条主连接，后续所有 ssh/scp 复用它，避免重复输入密码
MUX_DIR="/tmp/ceph_synconf_mux_$$"
mkdir -p "$MUX_DIR"
MUX_OPTS="-o ControlMaster=auto -o ControlPath=${MUX_DIR}/mux_%h -o ControlPersist=60"

cleanup_mux() {
    for node in "${dest_ips[@]}"; do
        ssh -p "${ssh_port}" $MUX_OPTS -O exit "${user}@${node}" 2>/dev/null
    done
    rm -rf "$MUX_DIR"
}
trap cleanup_mux EXIT

# 遍历节点
for node in "${dest_ips[@]}"; do
    echo "==== 处理节点 $node ===="

    # 0. 建立该节点的 SSH 主连接（只需输入一次密码）
    echo "建立到 $node 的 SSH 主连接 ..."
    ssh -p "${ssh_port}" -fN -o ControlMaster=yes -o ControlPath="${MUX_DIR}/mux_%h" -o ControlPersist=60 "${user}@${node}"
    if [ $? -ne 0 ]; then
        echo "❌ 无法建立到 $node 的连接，跳过该节点"
        continue
    fi

    # 1. 确保远端所有目标文件的父目录存在，并将其属主属组改为 cyf:cyf
    #    额外把 /var/lib/ceph 顶层目录也 chown 给 cyf，保证可写入 bootstrap-osd 子目录
    remote_dirs=""
    for file in "${syn_files[@]}"; do
        dir=$(dirname "$file")
        remote_dirs="$remote_dirs $dir"
    done
    # 加入 /var/lib/ceph 顶层目录
    remote_dirs="$remote_dirs /var/lib/ceph"
    # 去重
    remote_dirs=$(echo "$remote_dirs" | tr ' ' '\n' | sort -u | xargs)

    echo "检查并设置 $node 上的目标目录: $remote_dirs ..."
    ssh -p "${ssh_port}" $MUX_OPTS "${user}@${node}" "for d in $remote_dirs; do sudo mkdir -p \$d && sudo chown ${user}:${user} \$d && sudo chmod 755 \$d; done"
    if [ $? -ne 0 ]; then
        echo "❌ 在 $node 创建或设置目标目录失败（可能权限不足），跳过该节点"
        continue
    fi

    # 2. 遍历文件进行同步
    for file in "${syn_files[@]}"; do
        if [ ! -f "$file" ]; then
            echo "⚠️ 文件 $file 不存在，跳过"
            continue
        fi

        echo "推送 $file 到 $node:$file"

        if [ "$file" = "/etc/ceph/ceph.conf" ]; then
            remote_gid=$(ssh -p "${ssh_port}" $MUX_OPTS "${user}@${node}" "show_gids 2>/dev/null | awk '/mlx5_1/ {print \$4; exit}'")
            if [ -n "$remote_gid" ]; then
                echo "检测到 $node 的 mlx5_1 GID: $remote_gid"

                tmp_conf=$(mktemp)
                cp "$file" "$tmp_conf"
                if grep -q '^ms_async_rdma_local_gid=' "$tmp_conf"; then
                    sed -i -E "s#^ms_async_rdma_local_gid=.*#ms_async_rdma_local_gid=${remote_gid}#" "$tmp_conf"
                else
                    echo "ms_async_rdma_local_gid=${remote_gid}" >> "$tmp_conf"
                fi

                scp -P "${ssh_port}" $MUX_OPTS "$tmp_conf" "${user}@${node}":"$file"
                scp_status=$?
                rm -f "$tmp_conf"
            else
                echo "⚠️ 未能从 $node 获取 mlx5_1 GID，使用原始配置文件继续同步"
                scp -P "${ssh_port}" $MUX_OPTS "$file" "${user}@${node}":"$file"
                scp_status=$?
            fi
        else
            scp -P "${ssh_port}" $MUX_OPTS "$file" "${user}@${node}":"$file"
            scp_status=$?
        fi

        if [ "$scp_status" -eq 0 ]; then
            echo "✅ 已成功同步 $file 到 $node"
        else
            echo "❌ 同步 $file 到 $node 失败"
        fi
    done
    echo ""
done

echo "==== 所有文件同步完成 ===="
