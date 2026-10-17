#!/bin/bash

# 检查是否传入参数
if [ $# -eq 0 ]; then
    echo "用法:"
    echo "  1. 启动单个或多个 OSD: $0 <osd_id1> <osd_id2> ..."
    echo "  2. 启动连续范围的 OSD: $0 <start_id>-<end_id>"
    echo "示例:"
    echo "  $0 1 2 5       # 启动 OSD 1, 2, 5"
    echo "  $0 0-15        # 启动 OSD 0 到 15"
    exit 1
fi

# 解析参数，支持范围（如 0-15）和单个数字（如 1 2 3）
parse_args() {
    for arg in "$@"; do
        if [[ "$arg" =~ ^[0-9]+-[0-9]+$ ]]; then  # 匹配 "X-Y" 格式
            start=${arg%-*}
            end=${arg#*-}
            for ((i=start; i<=end; i++)); do
                osd_list+=("$i")
            done
        else  # 单个数字
            osd_list+=("$arg")
        fi
    done
}

# 存储所有要启动的 OSD ID
osd_list=()

# 解析命令行参数
parse_args "$@"

# 去重并排序（可选）
osd_list=($(printf "%s\n" "${osd_list[@]}" | sort -nu))

# 启动 OSD
for osd_id in "${osd_list[@]}"; do
    echo "启动 OSD: $osd_id"
    sudo LD_LIBRARY_PATH=/usr/local/lib/ceph/dpu:/usr/local/lib ceph-osd -i "$osd_id" --cluster ceph
done

echo "所有指定的 OSD 启动完成"