#!/bin/bash
# CPU
# 固定参数
TEST_FILE="/home/cyf/test_files/genFile768MB"
MODE="cpu"
ARG1="12"
ARG2="4"
ARG4="32"

# 测试块大小（64KB → 1MB）
BLOCK_SIZES=("64KB" "128KB" "256KB" "512KB" "1MB")

echo "Running proxy_test for each block size (3 trials)..."
echo "Block Size | Average Throughput (MB/s)"
echo "-------------------------------------"

for BLOCK in "${BLOCK_SIZES[@]}"; do
    SUM=0
    COUNT=0
    
    for (( i=1; i<=3; i++ )); do
        OUTPUT=$(./proxy_test "$TEST_FILE" "$MODE" "$ARG1" "$ARG2" "$BLOCK" "$ARG4" 2>/dev/null)
        THROUGHPUT=$(echo "$OUTPUT" | grep "CPU encoding throughput" | awk '{print $4}')
        
        if [ -n "$THROUGHPUT" ]; then
            SUM=$(echo "$SUM + $THROUGHPUT" | bc)
            COUNT=$((COUNT + 1))
        fi
    done

    # 计算平均吞吐量（仅当有有效数据时）
    if [ "$COUNT" -gt 0 ]; then
        AVG=$(echo "scale=2; $SUM / $COUNT" | bc)
        printf "%-8s  | %10.2f\n" "$BLOCK" "$AVG"
    else
        printf "%-8s  | %s\n" "$BLOCK" "Failed"
    fi
done

echo "-------------------------------------"
echo "Done!"