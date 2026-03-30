#!/bin/bash

batch_sizes=(1 2 4 8 16 32 64)
run_time=10
# offload_type=("Naive" "Batch_Copy" "Batch_Copy_Pipeline")
offload_type=("Batch_Copy")

# 外层循环：遍历不同的 offload type
for offload in "${offload_type[@]}"; do
    # 根据 offload type 命名结果文件
    resultFile="${offload}_result.txt"
    
    echo "=========================================="
    echo "开始测试 Offload Type: $offload"
    echo "结果将保存到: $resultFile"
    echo "=========================================="
    
    # 清空或创建结果文件
    > "$resultFile"
    
    # 写入表头
    echo "======================================" >> "$resultFile"
    echo "Offload Type: $offload 测试结果" >> "$resultFile"
    echo "测试时间: $(date)" >> "$resultFile"
    echo "每次运行 $run_time 次" >> "$resultFile"
    echo "======================================" >> "$resultFile"
    echo "" >> "$resultFile"
    
    # 内层循环：遍历不同的 batch size
    for batch_size in "${batch_sizes[@]}"; do
        echo "正在测试 $offload - batch size: $batch_size (共 $run_time 次)"
        echo "Batch Size: $batch_size" >> "$resultFile"
        
        for ((i=1; i<=$run_time; i++)); do
            echo "  第 $i 次运行..." 
            echo -n "  Run $i: " >> "$resultFile"
            
            # 运行命令，使用当前的 offload type
            ../doca_ec_test "$offload" 8 4 4 1MB "$batch_size" >> "$resultFile" 2>&1
            
            # 添加分隔符便于阅读
            if [ $i -lt $run_time ]; then
                echo "" >> "$resultFile"
            fi
        done
        
        echo "" >> "$resultFile"
        echo "--------------------------------------" >> "$resultFile"
        echo "" >> "$resultFile"
        
        # 每个 batch size 之间稍作停顿，避免资源竞争
        sleep 1
    done
    
    echo "完成 $offload 测试！结果已保存到 $resultFile"
    echo ""
    
    # 不同 offload type 之间稍作停顿
    sleep 2
done

echo "=========================================="
echo "所有测试完成！"
echo "生成的结果文件："
for offload in "${offload_type[@]}"; do
    echo "  - ${offload}_result.txt"
done
echo "=========================================="