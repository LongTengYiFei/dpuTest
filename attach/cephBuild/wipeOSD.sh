#!/bin/bash

# 遍历所有 NVMe 设备（nvme0n1 到 nvme3n1）的分区 1-4
for device in /dev/nvme[0-3]n1; do
    for partition in 1 2 3 4; do
        partition_path="${device}p${partition}"
        
        # 检查分区是否存在
        if [ -b "$partition_path" ]; then
            echo "正在清除分区: $partition_path"
            
            # 强制清除文件系统签名
            sudo wipefs -a -f "$partition_path"
            
            if [ $? -eq 0 ]; then
                echo "✅ 成功清除 $partition_path"
            else
                echo "❌ 清除 $partition_path 失败！"
            fi
        else
            echo "⚠️ 分区 $partition_path 不存在，跳过"
        fi
    done
done

echo "所有 NVMe 分区处理完成"