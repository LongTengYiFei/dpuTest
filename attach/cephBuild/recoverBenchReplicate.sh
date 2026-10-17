#!/bin/bash
# ===========================================================================
# 副本池恢复性能测试脚本
# ---------------------------------------------------------------------------
# 测试流程：
#   1. 清理旧 pool（若存在）
#   2. 创建副本池（默认 size=3）
#   3. 设置恢复调优参数（可选）
#   4. 生成测试数据文件并写入指定数量对象
#   5. 等待集群 active+clean
#   6. 记录恢复前状态
#   7. 逐出若干 OSD（ceph osd out）触发恢复，同时启动恢复监控脚本
#   8. 等待恢复完成（active+clean）
#   9. 将 OSD 重新加入集群（ceph osd in）
#  10. 汇总恢复耗时与数据量
#
# 用法:
#   ./recoverBenchReplicate.sh                       # 使用默认参数
#   ./recoverBenchReplicate.sh --osd-out 3 5 7       # 指定要 out 的 OSD
#   ./recoverBenchReplicate.sh --objects 64 --size 128M
#   ./recoverBenchReplicate.sh --pool replpool --pg-num 32 --size 3
# ===========================================================================

set -u

# ========================== 默认配置 ==========================
ceph_path="/home/cyf/ceph-dpu/build/bin/ceph"
rados_path="/home/cyf/ceph-dpu/build/bin/rados"
scan_script="/home/cyf/ceph-dpu/AAA-myTools/recoverStatusScan.py"
ceph_conf="/etc/ceph/ceph.conf"

pool_name="replpool"
pg_num=32
rep_size=3
object_count=128
object_size="128M"
osd_out_list=()          # 默认为空，运行时若未指定则自动选取

testfile_dir="/home/cyf/test_file"
testfile="${testfile_dir}/genFile${object_size}"

# 恢复调优参数（设为空则不修改）
recovery_chunk_size=""   # 例: "8388608" (8MiB)

# 扫描间隔（秒）
scan_interval=0.5
# =============================================================

# ---- 颜色输出 ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "\n${CYAN}========== $* ==========${NC}"; }

# ---- 参数解析 ----
parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --pool)
                pool_name="$2"; shift 2 ;;
            --pg-num)
                pg_num="$2"; shift 2 ;;
            --size)
                rep_size="$2"; shift 2 ;;
            --objects)
                object_count="$2"; shift 2 ;;
            --obj-size)
                object_size="$2"; shift 2
                testfile="${testfile_dir}/genFile${object_size}" ;;
            --osd-out)
                shift
                while [ $# -gt 0 ] && [[ "$1" != --* ]]; do
                    osd_out_list+=("$1")
                    shift
                done
                ;;
            --chunk-size)
                recovery_chunk_size="$2"; shift 2 ;;
            --help|-h)
                echo "用法: $0 [选项]"
                echo "  --pool NAME        副本池名称 (默认: $pool_name)"
                echo "  --pg-num N         PG 数量 (默认: $pg_num)"
                echo "  --size N           副本数 (默认: $rep_size)"
                echo "  --objects N        写入对象数 (默认: $object_count)"
                echo "  --obj-size SIZE    单个对象大小, 如 128M/256M (默认: $object_size)"
                echo "  --osd-out IDS...   要 out 的 OSD ID 列表 (不指定则自动选取)"
                echo "  --chunk-size BYTES 恢复 chunk 大小 (默认不修改)"
                exit 0 ;;
            *)
                log_error "未知参数: $1"; exit 1 ;;
        esac
    done
}

# ---- 检查是否存在非 clean 状态的 PG ----
# 返回非空字符串 = 有 non-clean PG；空字符串 = 全部 clean
check_non_clean_pgs() {
    local dirty_re='recovering|backfill|degraded|inconsistent|peering|activating|remapped|repair|recovery_wait|backfill_wait|forced_recovery|forced_backfill|incomplete|down|snaptrim|snaptrim_wait|recovery_unfound|backfill_unfound'
    "$ceph_path" pg ls 2>/dev/null | grep -E '^[0-9]+\.[0-9a-f]+' | grep -iE "$dirty_re" || true
}

# ---- 等待恢复真正开始（PG 进入 non-clean 状态）----
# osd out 后 CRUSH 重算需要时间，PG 状态不会立刻变化
wait_recovery_start() {
    local timeout="${1:-30}"
    local elapsed=0
    log_info "等待恢复开始 (PG 进入 non-clean 状态, 超时 ${timeout}s)..."
    while [ "$elapsed" -lt "$timeout" ]; do
        local non_clean
        non_clean=$(check_non_clean_pgs)
        if [ -n "$non_clean" ]; then
            log_info "检测到恢复已开始"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    log_warn "等待恢复开始超时 (${timeout}s)，可能 osd out 未生效或无 PG 受影响"
    return 1
}

# ---- 等待集群完全 active+clean ----
# 检查是否存在任何非 clean 状态关键词（remapped/backfilling/recovering/degraded 等）
# 避免 active+clean+remapped 被误判为 clean
wait_active_clean() {
    local timeout="${1:-300}"
    local elapsed=0
    log_info "等待集群恢复 active+clean (超时 ${timeout}s)..."
    while [ "$elapsed" -lt "$timeout" ]; do
        local non_clean
        non_clean=$(check_non_clean_pgs)
        if [ -n "$non_clean" ]; then
            sleep 2
            elapsed=$((elapsed + 2))
        else
            log_info "集群已 active+clean"
            return 0
        fi
    done
    log_warn "等待 active+clean 超时 (${timeout}s)，当前非 clean 的 PG:"
    check_non_clean_pgs
    return 1
}

# ---- 获取所有 up 的 OSD 列表 ----
get_up_osds() {
    "$ceph_path" osd ls 2>/dev/null
}

# ---- 自动选取要 out 的 OSD ----
auto_select_osd_out() {
    local all_osds
    all_osds=$(get_up_osds)
    local count=${#all_osds[@]}
    if [ "$count" -le "$rep_size" ]; then
        log_error "OSD 数量 ($count) 不足，无法安全 out，请用 --osd-out 手动指定"
        exit 1
    fi
    # 选取 1 个 OSD 来触发恢复（可按需调整）
    osd_out_list=( $(echo "$all_osds" | head -1) )
    log_warn "未指定 --osd-out，自动选取 OSD: ${osd_out_list[*]}"
}

# ---- 生成测试数据文件 ----
ensure_testfile() {
    if [ -f "$testfile" ]; then
        log_info "测试文件已存在: $testfile ($(du -h "$testfile" | awk '{print $1}'))"
        return 0
    fi
    mkdir -p "$testfile_dir"
    log_info "生成测试文件: $testfile ($object_size)..."
    local size_bytes
    size_bytes=$(numfmt --from=auto "$object_size")
    dd if=/dev/urandom of="$testfile" bs=1M count=$((size_bytes / 1048576)) status=progress
    log_info "测试文件生成完成"
}

# ---- 记录 OSD 使用量 ----
dump_osd_usage() {
    local label="$1"
    local out_file="/tmp/replbench_osd_usage_${label}.txt"
    "$ceph_path" osd df 2>/dev/null > "$out_file"
    echo "$out_file"
}

# ---- 记录对象→OSD 映射（抽样若干个） ----
dump_object_map() {
    local n="${1:-5}"
    log_info "抽样 $n 个对象的 OSD 映射:"
    for i in $(seq 1 "$n"); do
        local obj="obj${i}"
        local mapping
        mapping=$("$ceph_path" osd map "$pool_name" "$obj" 2>/dev/null || echo "  (映射失败)")
        echo "  $obj -> $mapping"
    done
}

# ---- 触发恢复并监控 ----
# 结果通过全局变量 RECOVERY_DURATION 返回（秒）
RECOVERY_DURATION=""
run_recovery_benchmark() {
    local osd_ids="${osd_out_list[*]}"

    log_step "第 7 步: 触发恢复"
    log_info "将逐出 OSD: $osd_ids"
    log_info "同时启动恢复监控脚本 (间隔 ${scan_interval}s)..."

    # 后台启动恢复监控
    python3 "$scan_script" &
    local scan_pid=$!
    log_info "恢复监控脚本 PID: $scan_pid"

    # 记录恢复开始时间
    local start_ts
    start_ts=$(date +%s.%N)

    # 逐出 OSD 触发恢复
    "$ceph_path" osd out $osd_ids 2>/dev/null
    log_info "已执行 ceph osd out $osd_ids"

    # 等待恢复真正开始（CRUSH 重算后 PG 进入 non-clean 状态）
    wait_recovery_start 30

    # 等待恢复完成
    log_info "等待恢复完成..."
    wait_active_clean 600

    local end_ts
    end_ts=$(date +%s.%N)
    RECOVERY_DURATION=$(echo "$end_ts - $start_ts" | bc)

    # 停止监控脚本
    if kill -0 "$scan_pid" 2>/dev/null; then
        kill "$scan_pid" 2>/dev/null || true
        wait "$scan_pid" 2>/dev/null || true
    fi

    log_step "恢复结果"
    log_info "恢复耗时: ${RECOVERY_DURATION}s"
}

# ---- 主流程 ----
main() {
    parse_args "$@"

    log_step "副本池恢复性能测试"
    log_info "配置: pool=$pool_name, pg=$pg_num, size=$rep_size"
    log_info "       objects=$object_count, obj_size=$object_size"
    log_info "       osd_out=[${osd_out_list[*]}]"

    # 0. 检查工具
    for tool in "$ceph_path" "$rados_path" "$scan_script"; do
        if [ ! -x "$tool" ] && [ ! -f "$tool" ]; then
            log_error "找不到: $tool"
            exit 1
        fi
    done

    # 1. 确认集群健康
    log_step "第 1 步: 检查集群状态"
    "$ceph_path" -s
    if ! wait_active_clean 120; then
        log_error "集群未处于 active+clean，请先修复后再测试"
        exit 1
    fi

    # 2. 清理旧 pool
    log_step "第 2 步: 清理旧 pool"
    if "$ceph_path" osd pool ls 2>/dev/null | grep -qw "$pool_name"; then
        log_info "删除已存在的 pool: $pool_name"
        "$ceph_path" config set mon mon_allow_pool_delete true 2>/dev/null
        "$ceph_path" osd pool rm "$pool_name" "$pool_name" --yes-i-really-really-mean-it 2>/dev/null
        wait_active_clean 120
    else
        log_info "pool $pool_name 不存在，跳过删除"
    fi

    # 3. 创建副本池
    log_step "第 3 步: 创建副本池"
    "$ceph_path" osd pool create "$pool_name" "$pg_num" 2>/dev/null
    "$ceph_path" osd pool set "$pool_name" size "$rep_size" 2>/dev/null
    "$ceph_path" osd pool set "$pool_name" min_size 2 2>/dev/null
    "$ceph_path" osd pool application enable "$pool_name" rbd 2>/dev/null
    log_info "副本池 $pool_name 创建完成 (size=$rep_size, pg=$pg_num)"
    wait_active_clean 120

    # 4. 设置恢复调优参数
    log_step "第 4 步: 设置恢复调优参数"
    if [ -n "$recovery_chunk_size" ]; then
        "$ceph_path" config set osd osd_recovery_max_chunk "$recovery_chunk_size" 2>/dev/null
        log_info "已设置 osd_recovery_max_chunk=$recovery_chunk_size"
    else
        log_info "未设置恢复 chunk size，使用默认值"
    fi

    # 5. 生成测试文件
    log_step "第 5 步: 准备测试数据"
    ensure_testfile

    # 6. 写入对象
    log_step "第 6 步: 写入 $object_count 个对象 ($object_size each)"
    local write_start
    write_start=$(date +%s)
    for i in $(seq 1 "$object_count"); do
        if ! "$rados_path" -p "$pool_name" put "obj${i}" "$testfile" 2>/dev/null; then
            log_error "写入 obj${i} 失败"
            exit 1
        fi
        if [ $((i % 16)) -eq 0 ]; then
            log_info "  已写入 $i / $object_count"
        fi
    done
    local write_end
    write_end=$(date +%s)
    log_info "写入完成，耗时 $((write_end - write_start))s"

    # 7. 等待 clean 并记录状态
    log_step "等待数据落盘"
    wait_active_clean 300

    local total_size
    total_size=$(numfmt --from=auto "$object_size")
    total_size=$((total_size * object_count))
    log_info "总数据量: $(numfmt --to=iec $total_size) ($object_count x $object_size)"

    # 记录恢复前 OSD 使用量
    local before_file
    before_file=$(dump_osd_usage "before")
    log_info "恢复前 OSD 使用量已记录: $before_file"

    # 对象映射抽样
    dump_object_map 5

    # 8. 自动选取 OSD（若未指定）
    if [ ${#osd_out_list[@]} -eq 0 ]; then
        auto_select_osd_out
    fi

    # 9. 触发恢复并监控
    run_recovery_benchmark
    local duration="$RECOVERY_DURATION"

    # 10. 恢复后状态
    log_step "第 8 步: 恢复后状态"
    "$ceph_path" -s
    local after_file
    after_file=$(dump_osd_usage "after")
    log_info "恢复后 OSD 使用量已记录: $after_file"

    # 11. 将 OSD 重新加入
    log_step "第 9 步: 将 OSD 重新加入集群"
    "$ceph_path" osd in ${osd_out_list[*]} 2>/dev/null
    log_info "已执行 ceph osd in ${osd_out_list[*]}"
    wait_active_clean 600

    # 12. 汇总
    local throughput="N/A"
    if [ -n "$duration" ]; then
        throughput=$(echo "scale=2; $total_size / $duration / 1048576" | bc)
    fi

    log_step "测试汇总"
    echo "------------------------------------------------------------"
    echo "  副本池名称:     $pool_name"
    echo "  副本数:         $rep_size"
    echo "  PG 数:          $pg_num"
    echo "  对象数:         $object_count"
    echo "  单对象大小:     $object_size"
    echo "  总数据量:       $(numfmt --to=iec $total_size)"
    echo "  逐出 OSD:       ${osd_out_list[*]}"
    if [ -n "$recovery_chunk_size" ]; then
        echo "  恢复 chunk:     $recovery_chunk_size bytes"
    fi
    echo "  恢复耗时:       ${duration}s"
    echo "  恢复吞吐:       ${throughput} MiB/s"
    echo "------------------------------------------------------------"
    echo "OSD 使用量对比:"
    echo "  恢复前: $before_file"
    echo "  恢复后: $after_file"
    echo ""

    log_info "副本池恢复性能测试完成"
}

main "$@"
