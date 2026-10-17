#!/bin/bash
# ===========================================================================
# EC池恢复性能测试脚本
# ---------------------------------------------------------------------------
# 测试流程（每个配置）：
#   1. 创建 EC profile (k, m, stripe_unit)
#   2. 删除旧 pool，创建 EC pool
#   3. 写入指定数量对象
#   4. 等待 active+clean
#   5. 记录 out OSD 上的实际数据量（恢复数据量）
#   6. ceph osd out 触发恢复，监控恢复耗时
#   7. ceph osd in 恢复 OSD
#   8. 计算恢复吞吐 = 恢复数据量 / 恢复耗时
#
# 恢复数据量定义：
#   out 的 OSD 上承载的分片数据总量 = 需要重构的数据量
#   实际测量：osd out 前后该 OSD 的使用量差值
#   理论估算：total_user_data × (k+m) / k / num_osds
#
# 用法:
#   # 单配置测试
#   ./recoverBenchEC.sh --k 4 --m 2 --osd-out 3
#   ./recoverBenchEC.sh --k 6 --m 2 --osd-out 3
#
#   # 批量对比多个配置
#   ./recoverBenchEC.sh --configs "4+2,6+2" --osd-out 3
#   ./recoverBenchEC.sh --configs "4+2,6+2,8+4" --osd-out 3
#
#   # 自定义参数
#   ./recoverBenchEC.sh --k 4 --m 2 --osd-out 3 --objects 64 --obj-size 256M
# ===========================================================================

set -u

# ========================== 默认配置 ==========================
ceph_path="/home/cyf/ceph-dpu/build/bin/ceph"
rados_path="/home/cyf/ceph-dpu/build/bin/rados"
scan_script="/home/cyf/ceph-dpu/AAA-myTools/recoverStatusScan.py"
ceph_conf="/etc/ceph/ceph.conf"

pool_name="ecpool"
pg_num=32
ec_k=4
ec_m=2
stripe_unit="1M"
configs=""              # 批量配置，如 "4+2,6+2"

object_count=64
object_size="128M"
osd_out_list=()

testfile_dir="/home/cyf/test_file"
testfile="${testfile_dir}/genFile${object_size}"

recovery_chunk_size=""
scan_interval=0.5
# =============================================================

# ---- 颜色输出 ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "\n${CYAN}========== $* ==========${NC}"; }
log_config() { echo -e "${MAGENTA}[CONFIG]${NC} $*"; }

# ---- 参数解析 ----
parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --pool)        pool_name="$2"; shift 2 ;;
            --pg-num)      pg_num="$2"; shift 2 ;;
            --k)           ec_k="$2"; shift 2 ;;
            --m)           ec_m="$2"; shift 2 ;;
            --stripe-unit) stripe_unit="$2"; shift 2 ;;
            --configs)     configs="$2"; shift 2 ;;
            --objects)     object_count="$2"; shift 2 ;;
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
            --chunk-size)  recovery_chunk_size="$2"; shift 2 ;;
            --help|-h)
                echo "用法: $0 [选项]"
                echo ""
                echo "单配置:"
                echo "  --k N              数据块数 (默认: $ec_k)"
                echo "  --m N              冗余块数 (默认: $ec_m)"
                echo "  --stripe-unit SIZE 条带单元 (默认: $stripe_unit)"
                echo ""
                echo "批量对比:"
                echo "  --configs LIST     配置列表, 如 \"4+2,6+2,8+4\""
                echo ""
                echo "通用:"
                echo "  --pool NAME        池名称 (默认: $pool_name)"
                echo "  --pg-num N         PG 数量 (默认: $pg_num)"
                echo "  --objects N        写入对象数 (默认: $object_count)"
                echo "  --obj-size SIZE    单对象大小 (默认: $object_size)"
                echo "  --osd-out IDS...   要 out 的 OSD ID 列表"
                echo "  --chunk-size BYTES 恢复 chunk 大小"
                exit 0 ;;
            *)
                log_error "未知参数: $1"; exit 1 ;;
        esac
    done
}

# ---- 检查是否存在非 clean 状态的 PG ----
check_non_clean_pgs() {
    local dirty_re='recovering|backfill|degraded|inconsistent|peering|activating|remapped|repair|recovery_wait|backfill_wait|forced_recovery|forced_backfill|incomplete|down|snaptrim|snaptrim_wait|recovery_unfound|backfill_unfound'
    "$ceph_path" pg ls 2>/dev/null | grep -E '^[0-9]+\.[0-9a-f]+' | grep -iE "$dirty_re" || true
}

# ---- 等待恢复真正开始 ----
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
    log_warn "等待恢复开始超时 (${timeout}s)"
    return 1
}

# ---- 等待集群完全 active+clean ----
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

# ---- 获取指定 OSD 的已用空间 (bytes) ----
get_osd_used_bytes() {
    local osd_id="$1"
    "$ceph_path" osd df --format=json 2>/dev/null | python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    for node in data.get('nodes', []):
        if node['id'] == ${osd_id}:
            print(int(node.get('kb_used', 0)) * 1024)
            sys.exit(0)
    print(0)
except:
    print(0)
" 2>/dev/null
}

# ---- 获取所有 up 的 OSD 列表 ----
get_up_osds() {
    "$ceph_path" osd ls 2>/dev/null
}

# ---- 自动选取要 out 的 OSD ----
auto_select_osd_out() {
    local km=$((ec_k + ec_m))
    local all_osds
    all_osds=$(get_up_osds)
    local count
    count=$(echo "$all_osds" | wc -l)
    if [ "$count" -le "$km" ]; then
        log_error "OSD 数量 ($count) 不足 (需要 > $km)，请用 --osd-out 手动指定"
        exit 1
    fi
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

# ---- 记录 OSD 使用量到文件 ----
dump_osd_usage() {
    local label="$1"
    local out_file="/tmp/ecbench_osd_usage_${label}.txt"
    "$ceph_path" osd df 2>/dev/null > "$out_file"
    echo "$out_file"
}

# ---- 创建 EC profile ----
create_ec_profile() {
    local k="$1"
    local m="$2"
    local profile_name="$3"

    log_info "创建 EC profile: $profile_name (k=$k, m=$m, stripe=$stripe_unit)"

    # 先尝试删除旧 profile（忽略错误）
    "$ceph_path" osd erasure-code-profile rm "$profile_name" 2>/dev/null || true

    local cmd_args="k=$k m=$m crush-failure-domain=osd stripe_unit=$stripe_unit"

    "$ceph_path" osd erasure-code-profile set "$profile_name" $cmd_args 2>/dev/null
    if [ $? -ne 0 ]; then
        log_error "创建 EC profile 失败: $profile_name"
        return 1
    fi
    log_info "EC profile 创建成功: $profile_name"
}

# ---- 删除 pool 并等待 clean ----
delete_pool() {
    local pool="$1"
    if "$ceph_path" osd pool ls 2>/dev/null | grep -qw "$pool"; then
        log_info "删除已存在的 pool: $pool"
        "$ceph_path" config set mon mon_allow_pool_delete true 2>/dev/null
        "$ceph_path" osd pool rm "$pool" "$pool" --yes-i-really-really-mean-it 2>/dev/null
        wait_active_clean 120
    else
        log_info "pool $pool 不存在，跳过删除"
    fi
}

# ---- 触发恢复并监控，结果存入全局变量 RECOVERY_DURATION ----
RECOVERY_DURATION=""
run_recovery_benchmark() {
    local osd_ids="${osd_out_list[*]}"

    log_step "触发恢复"
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

    # 等待恢复真正开始
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

# ===========================================================================
# 运行单个 EC 配置的恢复测试
# 参数: k m
# 全局变量输出: RESULT_DURATION, RESULT_RECOVERY_BYTES, RESULT_THEORY_BYTES
# ===========================================================================
RESULT_DURATION=""
RESULT_RECOVERY_BYTES=""
RESULT_THEORY_BYTES=""

run_single_config() {
    local k="$1"
    local m="$2"
    local km=$((k + m))
    local profile_name="ecbench_${k}_${m}"

    log_step "EC 恢复测试: k=$k m=$m"
    log_config "配置: k=$k, m=$m, stripe=$stripe_unit"
    log_config "       pool=$pool_name, pg=$pg_num, objects=$object_count, obj_size=$object_size"
    log_config "       osd_out=[${osd_out_list[*]}]"

    # 1. 确认集群健康
    log_step "第 1 步: 检查集群状态"
    "$ceph_path" -s
    if ! wait_active_clean 120; then
        log_error "集群未处于 active+clean，跳过此配置"
        return 1
    fi

    # 2. 创建 EC profile
    log_step "第 2 步: 创建 EC profile"
    create_ec_profile "$k" "$m" "$profile_name" || return 1

    # 3. 删除旧 pool 并创建 EC pool
    log_step "第 3 步: 创建 EC pool"
    delete_pool "$pool_name"
    "$ceph_path" osd pool create "$pool_name" "$pg_num" erasure "$profile_name" 2>/dev/null
    if [ $? -ne 0 ]; then
        log_error "创建 EC pool 失败"
        return 1
    fi
    log_info "EC pool $pool_name 创建完成 (k=$k, m=$m, pg=$pg_num)"
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
            return 1
        fi
        if [ $((i % 16)) -eq 0 ]; then
            log_info "  已写入 $i / $object_count"
        fi
    done
    local write_end
    write_end=$(date +%s)
    log_info "写入完成，耗时 $((write_end - write_start))s"

    # 7. 等待 clean
    log_step "等待数据落盘"
    wait_active_clean 300

    local total_user_bytes
    total_user_bytes=$(numfmt --from=auto "$object_size")
    total_user_bytes=$((total_user_bytes * object_count))
    log_info "用户数据总量: $(numfmt --to=iec $total_user_bytes)"

    local num_osds
    num_osds=$(get_up_osds | wc -l)
    log_info "集群 OSD 数: $num_osds"

    # 8. 测量 out OSD 上的实际数据量
    log_step "第 7 步: 测量恢复数据量"

    local out_osd="${osd_out_list[0]}"
    local osd_used_before_write
    # 此处 OSD 已有数据，直接取当前值作为"该 OSD 上承载的数据量"
    local osd_used_bytes
    osd_used_bytes=$(get_osd_used_bytes "$out_osd")
    log_info "OSD $out_osd 当前使用量: $(numfmt --to=iec $osd_used_bytes)"

    # 理论恢复数据量 = total_user_data × (k+m) / k / num_osds
    local theory_bytes
    theory_bytes=$(( total_user_bytes * km / k / num_osds ))
    log_info "理论恢复数据量 (重构写): $(numfmt --to=iec $theory_bytes)"
    log_info "理论读取数据量 (k 分片): $(numfmt --to=iec $((theory_bytes * k)))"

    # 记录 OSD 使用量快照
    local before_file
    before_file=$(dump_osd_usage "before_${k}_${m}")
    log_info "恢复前 OSD 使用量已记录: $before_file"

    # 9. 触发恢复并监控
    run_recovery_benchmark
    local duration="$RECOVERY_DURATION"

    # 10. 恢复后状态
    log_step "第 8 步: 恢复后状态"
    "$ceph_path" -s
    local after_file
    after_file=$(dump_osd_usage "after_${k}_${m}")
    log_info "恢复后 OSD 使用量已记录: $after_file"

    # 11. 将 OSD 重新加入
    log_step "第 9 步: 将 OSD 重新加入集群"
    "$ceph_path" osd in ${osd_out_list[*]} 2>/dev/null
    log_info "已执行 ceph osd in ${osd_out_list[*]}"
    wait_active_clean 600

    # 12. 计算吞吐
    local recovery_bytes="$theory_bytes"
    local reconstruct_throughput="N/A"
    local read_throughput="N/A"
    if [ -n "$duration" ] && [ "$duration" != "0" ]; then
        reconstruct_throughput=$(echo "scale=2; $recovery_bytes / $duration / 1048576" | bc)
        read_throughput=$(echo "scale=2; $recovery_bytes * $k / $duration / 1048576" | bc)
    fi

    log_step "测试汇总: k=$k m=$m"
    echo "------------------------------------------------------------"
    echo "  EC 配置:         k=$k, m=$m"
    echo "  PG 数:           $pg_num"
    echo "  对象数:          $object_count"
    echo "  单对象大小:      $object_size"
    echo "  用户数据总量:    $(numfmt --to=iec $total_user_bytes)"
    echo "  集群 OSD 数:     $num_osds"
    echo "  逐出 OSD:        ${osd_out_list[*]}"
    echo "  恢复数据量(理论): $(numfmt --to=iec $theory_bytes) ($theory_bytes bytes)"
    echo "  恢复读取量(理论): $(numfmt --to=iec $((theory_bytes * k))) (读 $k 个分片)"
    echo "  恢复耗时:        ${duration}s"
    echo "  重构吞吐(写):    ${reconstruct_throughput} MiB/s"
    echo "  读取吞吐(读):    ${read_throughput} MiB/s"
    echo "------------------------------------------------------------"
    echo ""

    # 保存结果供对比
    RESULT_DURATION="$duration"
    RESULT_RECOVERY_BYTES="$theory_bytes"
    RESULT_THEORY_BYTES="$theory_bytes"

    # 清理: 删除 pool 和 profile
    log_info "清理: 删除 pool 和 EC profile"
    delete_pool "$pool_name"
    "$ceph_path" osd erasure-code-profile rm "$profile_name" 2>/dev/null || true
}

# ---- 结果数组（用于多配置对比） ----
declare -a RESULT_LABELS=()
declare -a RESULT_DURATIONS=()
declare -a RESULT_RECOVERY=()
declare -a RESULT_THROUGHPUTS=()

# ===========================================================================
# 主流程
# ===========================================================================
main() {
    parse_args "$@"

    # 检查工具
    for tool in "$ceph_path" "$rados_path" "$scan_script"; do
        if [ ! -x "$tool" ] && [ ! -f "$tool" ]; then
            log_error "找不到: $tool"
            exit 1
        fi
    done

    # 自动选取 OSD
    if [ ${#osd_out_list[@]} -eq 0 ]; then
        auto_select_osd_out
    fi

    # 确定要测试的配置列表
    declare -a config_k=()
    declare -a config_m=()

    if [ -n "$configs" ]; then
        # 解析 "4+2,6+2" 格式
        IFS=',' read -ra cfg_arr <<< "$configs"
        for cfg in "${cfg_arr[@]}"; do
            local k_val m_val
            k_val=$(echo "$cfg" | cut -d'+' -f1 | tr -d ' ')
            m_val=$(echo "$cfg" | cut -d'+' -f2 | tr -d ' ')
            if [ -z "$k_val" ] || [ -z "$m_val" ]; then
                log_error "配置格式错误: $cfg (应为 k+m, 如 4+2)"
                exit 1
            fi
            config_k+=("$k_val")
            config_m+=("$m_val")
        done
    else
        config_k+=("$ec_k")
        config_m+=("$ec_m")
    fi

    log_step "EC 恢复性能测试"
    log_info "将测试 ${#config_k[@]} 个配置: ${configs:-"${ec_k}+${ec_m}"}"
    echo ""

    # 逐个运行配置
    for idx in "${!config_k[@]}"; do
        local k_val="${config_k[$idx]}"
        local m_val="${config_m[$idx]}"
        local label="${k_val}+${m_val}"

        log_step ">>> 开始测试配置: $label <<<"

        run_single_config "$k_val" "$m_val"

        # 记录结果
        local throughput="N/A"
        if [ -n "$RESULT_DURATION" ] && [ "$RESULT_DURATION" != "0" ]; then
            throughput=$(echo "scale=2; $RESULT_RECOVERY_BYTES / $RESULT_DURATION / 1048576" | bc)
        fi

        RESULT_LABELS+=("$label")
        RESULT_DURATIONS+=("$RESULT_DURATION")
        RESULT_RECOVERY+=("$RESULT_RECOVERY_BYTES")
        RESULT_THROUGHPUTS+=("$throughput")

        echo ""
    done

    # 多配置对比表
    if [ "${#RESULT_LABELS[@]}" -gt 0 ]; then
        log_step "EC 恢复性能对比汇总"
        printf "%-16s %-14s %-14s %-14s %-14s\n" "配置" "恢复数据量" "恢复耗时" "重构吞吐" "读取吞吐"
        printf "%-16s %-14s %-14s %-14s %-14s\n" "----" "----------" "--------" "--------" "--------"

        for idx in "${!RESULT_LABELS[@]}"; do
            local label="${RESULT_LABELS[$idx]}"
            local dur="${RESULT_DURATIONS[$idx]}"
            local rec_bytes="${RESULT_RECOVERY[$idx]}"
            local rec_human
            rec_human=$(numfmt --to=iec "$rec_bytes" 2>/dev/null || echo "$rec_bytes")
            local tp="${RESULT_THROUGHPUTS[$idx]}"

            # 读取吞吐 = 重构吞吐 × k
            local k_val
            k_val=$(echo "$label" | grep -oE '^[0-9]+' | head -1)
            local read_tp="N/A"
            if [ "$tp" != "N/A" ] && [ -n "$k_val" ]; then
                read_tp=$(echo "scale=2; $tp * $k_val" | bc)
            fi

            printf "%-16s %-14s %-14ss %-12s %-12s\n" "$label" "$rec_human" "$dur" "${tp} MiB/s" "${read_tp} MiB/s"
        done
        echo ""

        log_info "说明:"
        echo "  - 恢复数据量 = out OSD 上承载的分片数据 (理论值: 用户数据 ×(k+m)/k / OSD数)"
        echo "  - 重构吞吐 = 恢复数据量 / 恢复耗时 (写入新 OSD 的数据量)"
        echo "  - 读取吞吐 = 重构吞吐 × k (从 k 个 OSD 读取分片)"
        echo "  - out 1 个 OSD 时, EC 恢复只需重构该 OSD 上的分片"
        echo ""
    fi

    log_info "EC 恢复性能测试完成"
}

main "$@"
