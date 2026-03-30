#!/bin/bash

batch_sizes=(1 2 4 8 16 32 64)
run_time=10
# offload_type=("Naive" "Batch_Copy" "Batch_NoCopy" "Batch_Copy_Pipeline")
offload_type=("Batch_Copy")

# 定义 EC 配置二维数组
# 格式: "描述 k m" 其中 k=数据块数, m=校验块数
# 这里使用关联数组模拟二维数组
declare -A ec_configs
# ec_configs[0,0]="4+2"
# ec_configs[0,1]="4"
# ec_configs[0,2]="2"
# ec_configs[0,3]="2"

# ec_configs[1,0]="6+2"
# ec_configs[1,1]="6"
# ec_configs[1,2]="2"
# ec_configs[1,3]="2"

# 可以继续添加更多配置
ec_configs[0,0]="8+4"
ec_configs[0,1]="8"
ec_configs[0,2]="4"
ec_configs[0,3]="4"

ec_configs[1,0]="12+4"
ec_configs[1,1]="12"
ec_configs[1,2]="4"
ec_configs[1,3]="4"

# 获取 EC 配置数量
ec_config_count=2  # 如果有更多配置，修改这个数字

echo "=========================================="
echo "开始批量测试"
echo "EC 配置数: $ec_config_count"
echo "Offload 类型数: ${#offload_type[@]}"
echo "Batch Size 数: ${#batch_sizes[@]}"
echo "每个测试运行 $run_time 次"
echo "=========================================="
echo ""

# 外层循环：遍历 EC 配置
for ((ec_idx=0; ec_idx<ec_config_count; ec_idx++)); do
    ec_desc="${ec_configs[$ec_idx,0]}"
    k="${ec_configs[$ec_idx,1]}"
    m="${ec_configs[$ec_idx,2]}"
    e="${ec_configs[$ec_idx,3]}"
    
    echo "##########################################"
    echo "开始测试 EC 配置: $ec_desc (k=$k, m=$m, e=$e)"
    echo "##########################################"
    
    # 为每个 EC 配置创建目录（可选）
    ec_dir="results_${ec_desc}"
    mkdir -p "$ec_dir"
    
    # 中层循环：遍历不同的 offload type
    for offload in "${offload_type[@]}"; do
        # 根据 EC 配置和 offload type 命名结果文件
        resultFile="${ec_dir}/${offload}_${ec_desc}_result.txt"
        
        echo "=========================================="
        echo "测试 EC: $ec_desc, Offload: $offload"
        echo "结果将保存到: $resultFile"
        echo "=========================================="
        
        # 清空或创建结果文件
        > "$resultFile"
        
        # 写入表头
        echo "======================================" >> "$resultFile"
        echo "EC 配置: $ec_desc (k=$k, m=$m, e=$e)" >> "$resultFile"
        echo "Offload Type: $offload" >> "$resultFile"
        echo "测试时间: $(date)" >> "$resultFile"
        echo "每次运行 $run_time 次" >> "$resultFile"
        echo "======================================" >> "$resultFile"
        echo "" >> "$resultFile"
        
        # 内层循环：遍历不同的 batch size
        for batch_size in "${batch_sizes[@]}"; do
            echo "  正在测试 batch size: $batch_size (共 $run_time 次)"
            echo "Batch Size: $batch_size" >> "$resultFile"
            
            for ((i=1; i<=$run_time; i++)); do
                echo "    第 $i 次运行..." 
                echo -n "    Run $i: " >> "$resultFile"
                
                # 运行命令，使用当前的 EC 配置和 offload type
                # 参数: offload_type k m e 1MB batch_size
                ../doca_ec_test "$offload" "$k" "$m" "$e" 1MB "$batch_size" >> "$resultFile" 2>&1
                
                # 添加分隔符便于阅读
                if [ $i -lt $run_time ]; then
                    echo "" >> "$resultFile"
                fi
            done
            
            echo "" >> "$resultFile"
            echo "--------------------------------------" >> "$resultFile"
            echo "" >> "$resultFile"
            
            # 每个 batch size 之间稍作停顿
            sleep 1
        done
        
        echo "  完成 $offload 测试！"
        echo ""
        
        # 不同 offload type 之间稍作停顿
        sleep 1
    done
    
    echo "完成 EC 配置 $ec_desc 的所有测试！"
    echo "结果保存在目录: $ec_dir"
    echo ""
    
    # 不同 EC 配置之间稍作停顿
    sleep 2
done

echo "##########################################"
echo "所有测试完成！"
echo "##########################################"
echo ""
echo "生成的结果文件："
for ((ec_idx=0; ec_idx<ec_config_count; ec_idx++)); do
    ec_desc="${ec_configs[$ec_idx,0]}"
    ec_dir="results_${ec_desc}"
    echo ""
    echo "EC 配置: $ec_desc (目录: $ec_dir)"
    for offload in "${offload_type[@]}"; do
        echo "  - ${offload}_${ec_desc}_result.txt"
    done
done
echo ""
echo "结果按 EC 配置分别保存在不同的目录中"