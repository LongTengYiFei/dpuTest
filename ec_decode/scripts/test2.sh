#!/bin/bash

batch_sizes=(1 2 4 8 16 32)
run_num=20
output_file="./result21.txt"
k=4
m=2
erasure_count=2

# 清空或创建输出文件
> "$output_file"

for batch_size in "${batch_sizes[@]}"; do
    echo "Testing batch_size: $batch_size" | tee -a "$output_file"
    echo "==========================================" | tee -a "$output_file"
    
    for ((i=1; i<=$run_num; i++)); do
        echo "Running iteration $i/$run_num with batch_size=$batch_size"
        # 使用timeout防止命令卡住，并捕获输出
        result=$(timeout 30s ../doca_ec_test decode_batch $k $m 1MB $batch_size $erasure_count 2>/dev/null | grep "doca Raw decode time")
        if [ -n "$result" ]; then
            echo "Batch_size $batch_size - Run $i: $result" | tee -a "$output_file"
        else
            echo "Batch_size $batch_size - Run $i: ERROR - No output or timeout" | tee -a "$output_file"
        fi
    done
    
    echo "" | tee -a "$output_file"
done

echo "All tests completed. Results saved to $output_file"

input_file="./result21.txt"
output_file2="./result22.txt"

# 使用awk处理结果文件
awk -v run_num="$run_num" '
/doca Raw decode time/ {
    # 提取时间值（假设时间在第10个字段）
    time_value = $10
    # 移除可能的后缀（如"us"）
    gsub(/[^0-9.]/, "", time_value)
    times[++count] = time_value
}
END {
    # 按batch_size分组输出
    batch_count = count / run_num
    for (batch = 0; batch < batch_count; batch++) {
        for (i = 1; i <= run_num; i++) {
            idx = batch * run_num + i
            if (idx <= count) {
                printf "%s", times[idx]
                if (i < run_num) printf " "
            }
        }
        printf "\n"
    }
}' "$input_file" > "$output_file2"

echo "数据已重新格式化到 $output_file2，每${run_num}个时间数据为一行"