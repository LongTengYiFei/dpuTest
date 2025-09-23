#!/bin/bash

num_processes=8
run_rounds=10      # 运行轮数
output_file="./result3.txt"
k=8
m=4
erasure_count=4
batch_size=32

# 清空输出文件
> "$output_file"

echo "开始运行 $run_rounds 轮，每轮同时启动 $num_processes 个进程..."
echo "每轮取最大值，然后计算最大值的平均值"

# 存储每轮最大值的临时文件
max_times_file=$(mktemp)

# 运行多轮
for ((round=1; round<=run_rounds; round++)); do
    echo "=== 第 $round 轮开始 ===" >> "$output_file"
    
    # 存储本轮各进程时间的临时文件
    round_times_file=$(mktemp)
    
    # 启动多个并行进程（一轮）
    for i in $(seq 1 $num_processes); do
        (
            echo "第 $round 轮 - 进程 $i 开始" >> "$output_file"
            ../doca_ec_test decode_batch $k $m 1MB $batch_size $erasure_count >> "$output_file" 2>&1
            echo "第 $round 轮 - 进程 $i 结束" >> "$output_file"
        ) &
    done
    
    # 等待本轮所有进程完成
    wait
    
    # 提取本轮所有进程的时间数据，并找出最大值
    round_max=$(grep "doca Raw decode time" "$output_file" | tail -n $num_processes | awk '{print $5}' | awk '
    BEGIN {max = 0}
    {
        if ($1 > max) max = $1
    }
    END {print max}')
    
    echo "第 $round 轮最大时间: $round_max 微秒" >> "$output_file"
    echo "$round_max" >> "$max_times_file"
    
    echo "=== 第 $round 轮结束，本轮最大值: $round_max 微秒 ===" >> "$output_file"
    echo "" >> "$output_file"
    
    # 清理临时文件
    rm -f "$round_times_file"
done

# 计算最大值的平均值
if [ -s "$max_times_file" ]; then
    max_avg=$(awk '{sum += $1} END {print sum/NR}' "$max_times_file")
    max_count=$(wc -l < "$max_times_file")
    
    # 计算per stripe的时间
    max_avg_per_stripe=$(echo "$max_avg / $batch_size" | bc -l)
    
    echo "=== 最大值统计结果 ===" >> "$output_file"
    echo "总轮数: $max_count" >> "$output_file"
    echo "每轮最大值的平均值: $max_avg 微秒" >> "$output_file"
    echo "max decode time per stripe: $max_avg_per_stripe 微秒" >> "$output_file"
    
    # 显示所有轮的最大值
    echo "各轮最大值:" >> "$output_file"
    cat "$max_times_file" >> "$output_file"
    
    # 终端显示
    echo "=== 最大值统计结果 ==="
    echo "总轮数: $max_count"
    echo "每轮最大值的平均值: $max_avg 微秒"
    echo "max decode time per stripe: $max_avg_per_stripe 微秒"
else
    echo "未找到时间数据" >> "$output_file"
    echo "未找到时间数据"
fi

# 清理临时文件
rm -f "$max_times_file"

echo "所有 $run_rounds 轮执行完成，总共运行了 $((num_processes * run_rounds)) 次" >> "$output_file"
echo "结果已保存到: $output_file"