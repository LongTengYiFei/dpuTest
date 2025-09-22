batch_sizes=(1 2 4 8 16 32)
run_num=100
output_file="./result21.txt"

# 清空或创建输出文件
> "$output_file"

for batch_size in "${batch_sizes[@]}"; do
    echo "Testing batch_size: $batch_size" | tee -a "$output_file"
    echo "==========================================" | tee -a "$output_file"
    
    for ((i=1; i<=$run_num; i++)); do
        echo "Running iteration $i/$run_num with batch_size=$batch_size"
        result=$(../doca_ec_test decode_batch 8 4 1MB $batch_size 4 | grep "doca Raw decode time")
        echo "Batch_size $batch_size - Run $i: $result" | tee -a "$output_file"
    done
    
    echo "" | tee -a "$output_file"
done

echo "All tests completed. Results saved to $output_file"

input_file="./result21.txt"
output_file2="./result22.txt"

# 提取时间数据并每100行一列
awk '/doca Raw decode time/ {
    times[NR] = $10
}
END {
    count = 0
    for (i in times) {
        printf "%s", times[i]
        count++
        if (count % 100 == 0) {
            printf "\n"
        } else {
            printf " "
        }
    }
    if (count % 100 != 0) {
        printf "\n"
    }
}' "$input_file" > "$output_file2"

echo "数据已重新格式化到 $output_file2，每100个时间数据为一列"