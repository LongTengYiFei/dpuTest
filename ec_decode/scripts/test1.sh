run_time=10
batch_sizes=(1 2 4 8 16 32)
erasure_count=2
output_file="./result1.txt"
k=4
m=2

# 清空输出文件
> "$output_file"

for batch_size in "${batch_sizes[@]}"; do
    echo "=== Batch Size: $batch_size ===" >> "$output_file"
    
    # 初始化统计变量
    total_src_time=0
    total_dst_time=0
    count=0
    
    for x in $(seq 1 $run_time); do
        # 运行测试并提取时间
        result=$(../doca_ec_test decode_batch $k $m 1MB $batch_size $erasure_count | grep -E "copy src time|copy dst time")
        
        # 提取具体时间值（第4列）
        src_time=$(echo "$result" | grep "copy src time" | awk '{print $4}')
        dst_time=$(echo "$result" | grep "copy dst time" | awk '{print $4}')
        
        # 累加时间
        if [ -n "$src_time" ]; then
            total_src_time=$((total_src_time + src_time))
            total_dst_time=$((total_dst_time + dst_time))
            count=$((count + 1))
            
            # 输出每次运行的结果
            echo "Run $x: src_time=$src_time us, dst_time=$dst_time us" >> "$output_file"
        fi
    done
    
    # 计算平均值
    if [ $count -gt 0 ]; then
        avg_src_time=$((total_src_time / count))
        avg_dst_time=$((total_dst_time / count))
        
        echo "Average: src_time=$avg_src_time us, dst_time=$avg_dst_time us" >> "$output_file"
        echo "Batch Size $batch_size - Avg src: $avg_src_time us, Avg dst: $avg_dst_time us"
    fi
    
    echo "" >> "$output_file"
done

echo "所有测试完成！结果已保存到 $output_file"