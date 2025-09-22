#!/bin/bash

run_time=10
output_file="./result3.txt"
num_processes=16

# 清空输出文件
> "$output_file"

echo "开始同时启动 $num_processes 个进程，每个进程运行 $run_time 次..."

# 启动多个并行进程，每个进程运行指定次数
for i in $(seq 1 $num_processes); do
    (
        echo "=== 进程 $i 开始，运行 $run_time 次 ===" >> "$output_file"
        for ((j=1; j<=run_time; j++)); do
            echo "进程 $i - 第 $j 次运行" >> "$output_file"
            ../doca_ec_test decode_batch 8 4 1MB 32 4 >> "$output_file" 2>&1
            echo "进程 $i - 第 $j 次运行完成" >> "$output_file"
        done
        echo "=== 进程 $i 结束 ===" >> "$output_file"
    ) &
done

# 等待所有后台进程完成
wait

echo "所有 $num_processes 个进程执行完成，总共运行了 $((num_processes * run_time)) 次" >> "$output_file"
echo "结果已保存到: $output_file"