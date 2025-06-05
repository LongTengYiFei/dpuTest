#!/bin/bash
# DPU
# 固定参数
TEST_FILE="/home/cyf/test_files/genFile3072MB"
MODE="dpuBatch"
ARG1="6"
ARG2="2"
BLOCK="1MB"  # 固定块大小

# ARG4按2的幂次变化：1, 2, 4, 8, 16, 32, 64, 128, 256
ARG4_VALUES=(1 2 4 8 16 32 64 128 256)

echo "Running proxy_test with ARG4 as powers of 2..."
echo "ARG4 Value | Average Throughput (MB/s)"
echo "------------------------------------"

for ARG4 in "${ARG4_VALUES[@]}"; do
    SUM=0
    COUNT=0
    
    # 每个ARG4值运行3次
    for (( i=1; i<=3; i++ )); do
        OUTPUT=$(./proxy_test "$TEST_FILE" "$MODE" "$ARG1" "$ARG2" "$BLOCK" "$ARG4" 2>/dev/null)
        THROUGHPUT=$(echo "$OUTPUT" | grep "DPU accelerator encoding throughput REAL" | awk '{print $6}')
        
        if [ -n "$THROUGHPUT" ]; then
            SUM=$(awk "BEGIN {print $SUM + $THROUGHPUT}")
            COUNT=$((COUNT + 1))
        fi
    done

    # 计算并输出平均值
    if [ "$COUNT" -gt 0 ]; then
        AVG=$(awk "BEGIN {printf \"%.2f\", $SUM / $COUNT}")
        printf "%-9s | %10.2f\n" "$ARG4" "$AVG"
    else
        printf "%-9s | %s\n" "$ARG4" "Failed"
    fi
done

echo "------------------------------------"
echo "Done!"