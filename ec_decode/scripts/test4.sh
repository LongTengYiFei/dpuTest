#!/bin/bash

num_processes=32
run_rounds=10
output_file="./result4.txt"
k=8
m=4
erasure_count=4
batch_size=32

> "$output_file"

echo "开始运行 $run_rounds 轮，每轮同时启动 $num_processes 个CPU测试进程..."

# 存储每轮最小值和最大值的数组
declare -a min_speeds
declare -a max_speeds

for ((round=1; round<=run_rounds; round++)); do
    echo "=== 第 $round 轮开始 ===" >> "$output_file"
    
    temp_file=$(mktemp)
    
    # 启动多个并行CPU测试进程
    for i in $(seq 1 $num_processes); do
        (
            echo "第 $round 轮 - 进程 $i 开始" >> "$output_file"
            ../cpu_ec_test -k $k -m $m -e $erasure_count >> "$temp_file" 2>&1
            echo "第 $round 轮 - 进程 $i 结束" >> "$output_file"
        ) &
    done
    wait
    
    cat "$temp_file" >> "$output_file"
    
    # 提取本轮的恢复速度，找出最小值和最大值
    speeds=$(grep "recover speed" "$temp_file" | awk '{print $5}')
    
    if [ -n "$speeds" ]; then
        round_min=$(echo "$speeds" | awk '
        NR == 1 {min = $1; max = $1}
        NR > 1 {
            if ($1 < min) min = $1
            if ($1 > max) max = $1
        }
        END {print min " " max}')
        
        round_min_val=$(echo $round_min | awk '{print $1}')
        round_max_val=$(echo $round_min | awk '{print $2}')
        
        min_speeds[$round]=$round_min_val
        max_speeds[$round]=$round_max_val
        
        echo "第 $round 轮 - 最小速度: $round_min_val MB/s, 最大速度: $round_max_val MB/s" >> "$output_file"
    else
        echo "第 $round 轮未找到有效速度数据" >> "$output_file"
    fi
    
    echo "=== 第 $round 轮结束 ===" >> "$output_file"
    echo "" >> "$output_file"
    
    rm -f "$temp_file"
done

# 计算最小值平均值和最大值平均值
if [ ${#min_speeds[@]} -gt 0 ]; then
    min_sum=0
    max_sum=0
    count=0
    
    for i in "${!min_speeds[@]}"; do
        if [ -n "${min_speeds[$i]}" ] && [ "${min_speeds[$i]}" != "0" ]; then
            min_sum=$(echo "$min_sum + ${min_speeds[$i]}" | bc)
            max_sum=$(echo "$max_sum + ${max_speeds[$i]}" | bc)
            count=$((count + 1))
        fi
    done
    
    if [ $count -gt 0 ]; then
        min_avg=$(echo "scale=2; $min_sum / $count" | bc)
        max_avg=$(echo "scale=2; $max_sum / $count" | bc)
        
        echo "=== 统计结果 ===" >> "$output_file"
        echo "有效轮数: $count" >> "$output_file"
        echo "每轮最小恢复速度的平均值: $min_avg MB/s" >> "$output_file"
        echo "每轮最大恢复速度的平均值: $max_avg MB/s" >> "$output_file"
        
        echo "=== 统计结果 ==="
        echo "有效轮数: $count"
        echo "每轮最小恢复速度的平均值: $min_avg MB/s"
        echo "每轮最大恢复速度的平均值: $max_avg MB/s"
    fi
fi

echo "执行完成" >> "$output_file"