#!/bin/bash
# ===========================================================================
# 组合池（EC + 副本）恢复性能测试脚本
# ---------------------------------------------------------------------------
# 测试流程：
#   1. 创建 EC pool 和副本 pool
#   2. 分别写入指定数量的对象（可配置比例）
#   3. 等待 active+clean
#   4. 记录恢复前数据量
#   5. ceph osd out 触发恢复，两池同时恢复
#   6. 监控恢复总耗时
#   7. ceph osd in 恢复 OSD
#   8. 汇总恢复吞吐
#
# 恢复吞吐 = 两池恢复数据总量 / 恢复总耗时
#
# 用法:
#   # EC写96个, 副本写32个 (128M each)
#   ./recoverBenchMixed.sh --ec-objects 96 --rep-objects 32 --osd-out 3
#
#   # 按比例分配 (总128个对象, EC占75%)
#   ./recoverBenchMixed.sh --total-objects 128 --ec-ratio 0.75 --osd-out 3
#
#   # 自定义EC配置
#   ./recoverBenchMixed.sh --ec-objects 96 --rep-objects 32 --k 6 --m 2 --osd-out 3
#
#   # 自定义对象大小
#   ./recoverBenchMixed.sh --ec-objects 48 --rep-objects 16 --obj-size 256M --osd-out 3
# ===========================================================================

set -u

# ========================== 默认配置 ==========================
ceph_path="/home/cyf/ceph-dpu/build/bin/ceph"
rados_path="/home/cyf/ceph-dpu/build/bin/rados"
scan_script="/home/cyf/ceph-dpu/AAA-myTools/recoverStatusScan.py"
ceph_conf="/etc/ceph/ceph.conf"

ec_pool="ecpool"
rep_pool="replpool"
pg_num=32

ec_k=4
ec_m=2
stripe_unit="1M"

object_size="128M"
ec_objects=96
rep_objects=32
use_ratio=false
total_objects=128
ec_ratio=0.75

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
            --ec-pool)     ec_pool="$2"; shift 2 ;;
            --rep-pool)    rep_pool="$2"; shift 2 ;;
            --pg-num)      pg_num="$2"; shift 2 ;;
            --k)           ec_k="$2"; shift 2 ;;
            --m)           ec_m="$2"; shift 2 ;;
            --stripe-unit) stripe_unit="$2"; shift 2 ;;
            --ec-objects)  ec_objects="$2"; shift 2 ;;
            --rep-objects) rep_objects="$2"; shift 2 ;;
            --total-objects)
                total_objects="$2"; use_ratio=true; shift 2 ;;
            --ec-ratio)
                ec_ratio="$2"; use_ratio=true; shift 2 ;;
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
                echo "方式1 - 直接指定各池对象数:"
                echo "  --ec-objects N     EC 池写入对象数 (默认: $ec_objects)"
                echo "  --rep-objects N    副本池写入对象数 (默认: $rep_objects)"
                echo ""
                echo "方式2 - 按比例分配:"
                echo "  --total-objects N  总对象数 (默认: $total_objects)"
                echo "  --ec-ratio R       EC 占比 0~1 (默认: $ec_ratio)"
                echo ""
                echo "EC 配置:"
                echo "  --k N              数据块数 (默认: $ec_k)"
                echo "  --m N              冗余块数 (默认: $ec_m)"
                echo "  --stripe-unit SIZE 条带单元 (默认: $stripe_unit)"
                echo ""
                echo "通用:"
                echo "  --ec-pool NAME     EC 池名称 (默认: $ec_pool)"
                echo "  --rep-pool NAME    副本池名称 (默认: $rep_pool)"
                echo "  --pg-num N         PG 数量 (默认: $pg_num)"
                echo "  --obj-size SIZE    单对象大小 (默认: $object_size)"
                echo "  --osd-out IDS...   要 out 的 OSD ID 列表"
                echo "  --chunk-size BYTES 恢复 chunk 大小"
                echo ""
                echo "示例:"
                echo "  $0 --ec-objects 96 --rep-objects 32 --osd-out 3"
                echo "  $0 --total-objects 128 --ec-ratio 0.75 --osd-out 3"
                echo "  $0 --ec-objects 48 --rep-objects 16 --k 6 --m 2 --obj-size 256M --osd-out 3"
                exit 0 ;;
            *)
                log_error "未知参数: $1"; exit 1 ;;
        esac
    done

    # 按比例计算
    if [ "$use_ratio" = true ]; then
        ec_objects=$(python3 -c "print(int($total_objects * $ec_ratio))")
        rep_objects=$((total_objects - ec_objects))
        log_info "按比例分配: 总 $total_objects 对象, EC $ec_objects + 副本 $rep_objects"
    fi
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

# ---- 创建 EC profile ----
create_ec_profile() {
    local profile_name="$1"
    log_info "创建 EC profile: $profile_name (k=$ec_k, m=$ec_m, stripe=$stripe_unit)"
    "$ceph_path" osd erasure-code-profile rm "$profile_name" 2>/dev/null || true
    "$ceph_path" osd erasure-code-profile set "$profile_name" \
        k=$ec_k m=$ec_m crush-failure-domain=osd stripe_unit=$stripe_unit 2>/dev/null
    if [ $? -ne 0 ]; then
        log_error "创建 EC profile 失败: $profile_name"
        return 1
    fi
    log_info "EC profile 创建成功: $profile_name"
}

# ---- 写入对象到指定 pool ----
write_objects() {
    local pool="$1"
    local count="$2"
    local prefix="$3"

    log_info "向 $pool 写入 $count 个对象 ($object_size each, prefix=$prefix)..."
    local write_start
    write_start=$(date +%s)
    for i in $(seq 1 "$count"); do
        if ! "$rados_path" -p "$pool" put "${prefix}${i}" "$testfile" 2>/dev/null; then
            log_error "写入 ${prefix}${i} 到 $pool 失败"
            return 1
        fi
        if [ $((i % 16)) -eq 0 ]; then
            log_info "  $pool: 已写入 $i / $count"
        fi
    done
    local write_end
    write_end=$(date +%s)
    log_info "$pool 写入完成，耗时 $((write_end - write_start))s"
}

# ---- 触发恢复并监控 ----
RECOVERY_DURATION=""
run_recovery_benchmark() {
    local osd_ids="${osd_out_list[*]}"

    log_step "触发恢复"
    log_info "将逐出 OSD: $osd_ids"
    log_info "同时启动恢复监控脚本 (间隔 ${scan_interval}s)..."

    python3 "$scan_script" &
    local scan_pid=$!
    log_info "恢复监控脚本 PID: $scan_pid"

    local start_ts
    start_ts=$(date +%s.%N)

    "$ceph_path" osd out $osd_ids 2>/dev/null
    log_info "已执行 ceph osd out $osd_ids"

    wait_recovery_start 30

    log_info "等待恢复完成..."
    wait_active_clean 600

    local end_ts
    end_ts=$(date +%s.%N)
    RECOVERY_DURATION=$(echo "$end_ts - $start_ts" | bc)

    if kill -0 "$scan_pid" 2>/dev/null; then
        kill "$scan_pid" 2>/dev/null || true
        wait "$scan_pid" 2>/dev/null || true
    fi

    log_step "恢复结果"
    log_info "恢复耗时: ${RECOVERY_DURATION}s"
}

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

    local km=$((ec_k + ec_m))
    local obj_size_bytes
    obj_size_bytes=$(numfmt --from=auto "$object_size")

    local ec_user_bytes=$((obj_size_bytes * ec_objects))
    local rep_user_bytes=$((obj_size_bytes * rep_objects))
    local total_user_bytes=$((ec_user_bytes + rep_user_bytes))

    local num_osds
    num_osds=$(get_up_osds | wc -l)

    # 理论恢复数据量
    # EC: 用户数据 × (k+m)/k / OSD数
    # 副本: 用户数据 × 3 / OSD数
    local ec_recovery_bytes=$(( ec_user_bytes * km / ec_k / num_osds ))
    local rep_recovery_bytes=$(( rep_user_bytes * 3 / num_osds ))
    local total_recovery_bytes=$((ec_recovery_bytes + rep_recovery_bytes))

    log_step "组合池恢复性能测试"
    log_config "EC 池:   $ec_pool (k=$ec_k, m=$ec_m), $ec_objects 对象 = $(numfmt --to=iec $ec_user_bytes)"
    log_config "副本池:  $rep_pool (size=3), $rep_objects 对象 = $(numfmt --to=iec $rep_user_bytes)"
    log_config "总用户数据: $(numfmt --to=iec $total_user_bytes)"
    log_config "集群 OSD 数: $num_osds"
    log_config "逐出 OSD: ${osd_out_list[*]}"
    log_config "EC 占比: $(python3 -c "print(f'{$ec_user_bytes/$total_user_bytes*100:.1f}%')")"
    echo ""
    log_config "理论恢复数据量:"
    log_config "  EC 池:   $(numfmt --to=iec $ec_recovery_bytes) ($ec_recovery_bytes bytes)"
    log_config "  副本池:  $(numfmt --to=iec $rep_recovery_bytes) ($rep_recovery_bytes bytes)"
    log_config "  合计:    $(numfmt --to=iec $total_recovery_bytes) ($total_recovery_bytes bytes)"
    echo ""

    # 1. 确认集群健康
    log_step "第 1 步: 检查集群状态"
    "$ceph_path" -s
    if ! wait_active_clean 120; then
        log_error "集群未处于 active+clean，请先修复后再测试"
        exit 1
    fi

    # 2. 清理旧 pool
    log_step "第 2 步: 清理旧 pool"
    delete_pool "$ec_pool"
    delete_pool "$rep_pool"

    # 3. 创建 EC pool
    log_step "第 3 步: 创建 EC pool"
    local ec_profile="mixedbench_${ec_k}_${ec_m}"
    create_ec_profile "$ec_profile" || exit 1
    "$ceph_path" osd pool create "$ec_pool" "$pg_num" erasure "$ec_profile" 2>/dev/null
    if [ $? -ne 0 ]; then
        log_error "创建 EC pool 失败"
        exit 1
    fi
    log_info "EC pool $ec_pool 创建完成"
    wait_active_clean 120

    # 4. 创建副本池
    log_step "第 4 步: 创建副本池"
    "$ceph_path" osd pool create "$rep_pool" "$pg_num" 2>/dev/null
    "$ceph_path" osd pool set "$rep_pool" size 3 2>/dev/null
    "$ceph_path" osd pool set "$rep_pool" min_size 2 2>/dev/null
    "$ceph_path" osd pool application enable "$rep_pool" rbd 2>/dev/null
    log_info "副本池 $rep_pool 创建完成"
    wait_active_clean 120

    # 5. 设置恢复调优参数
    log_step "第 5 步: 设置恢复调优参数"
    if [ -n "$recovery_chunk_size" ]; then
        "$ceph_path" config set osd osd_recovery_max_chunk "$recovery_chunk_size" 2>/dev/null
        log_info "已设置 osd_recovery_max_chunk=$recovery_chunk_size"
    else
        log_info "未设置恢复 chunk size，使用默认值"
    fi

    # 6. 生成测试文件
    log_step "第 6 步: 准备测试数据"
    ensure_testfile

    # 7. 写入对象（EC 池和副本池）
    log_step "第 7 步: 写入对象"
    write_objects "$ec_pool" "$ec_objects" "ecobj" || exit 1
    write_objects "$rep_pool" "$rep_objects" "repobj" || exit 1

    # 8. 等待 clean
    log_step "等待数据落盘"
    wait_active_clean 300

    log_info "数据落盘完成"
    log_info "EC 池用户数据:   $(numfmt --to=iec $ec_user_bytes) ($ec_objects 对象)"
    log_info "副本池用户数据:   $(numfmt --to=iec $rep_user_bytes) ($rep_objects 对象)"
    log_info "总用户数据:       $(numfmt --to=iec $total_user_bytes)"

    # 9. 触发恢复并监控
    run_recovery_benchmark
    local duration="$RECOVERY_DURATION"

    # 10. 恢复后状态
    log_step "第 8 步: 恢复后状态"
    "$ceph_path" -s

    # 11. 将 OSD 重新加入
    log_step "第 9 步: 将 OSD 重新加入集群"
    "$ceph_path" osd in ${osd_out_list[*]} 2>/dev/null
    log_info "已执行 ceph osd in ${osd_out_list[*]}"
    wait_active_clean 600

    # 12. 计算吞吐
    local throughput="N/A"
    local ec_throughput="N/A"
    local rep_throughput="N/A"
    if [ -n "$duration" ] && [ "$duration" != "0" ]; then
        throughput=$(echo "scale=2; $total_recovery_bytes / $duration / 1048576" | bc)
        ec_throughput=$(echo "scale=2; $ec_recovery_bytes / $duration / 1048576" | bc)
        rep_throughput=$(echo "scale=2; $rep_recovery_bytes / $duration / 1048576" | bc)
    fi

    # 13. 汇总
    log_step "测试汇总"
    echo "============================================================"
    echo "  组合池恢复性能测试结果"
    echo "============================================================"
    echo ""
    echo "  [数据分布]"
    echo "    EC 池 ($ec_k+$ec_m):   $ec_objects 对象 = $(numfmt --to=iec $ec_user_bytes) ($(python3 -c "print(f'{$ec_user_bytes/$total_user_bytes*100:.1f}%')"))"
    echo "    副本池 (size=3):       $rep_objects 对象 = $(numfmt --to=iec $rep_user_bytes) ($(python3 -c "print(f'{$rep_user_bytes/$total_user_bytes*100:.1f}%')"))"
    echo "    总用户数据:            $(numfmt --to=iec $total_user_bytes)"
    echo ""
    echo "  [恢复数据量(理论)]"
    echo "    EC 池:   $(numfmt --to=iec $ec_recovery_bytes)"
    echo "    副本池:  $(numfmt --to=iec $rep_recovery_bytes)"
    echo "    合计:    $(numfmt --to=iec $total_recovery_bytes)"
    echo ""
    echo "  [恢复性能]"
    echo "    逐出 OSD:        ${osd_out_list[*]}"
    echo "    恢复耗时:        ${duration}s"
    echo "    总重构吞吐:      ${throughput} MiB/s"
    echo "      - EC 贡献:     ${ec_throughput} MiB/s"
    echo "      - 副本贡献:    ${rep_throughput} MiB/s"
    echo ""
    echo "  [对比参考]"
    echo "    纯3副本 (16G):   ~236 MiB/s (17.36s)"
    echo "    纯EC 4+2 (16G):  ~52 MiB/s (39.15s)"
    echo "    纯EC 6+2 (16G):  ~31 MiB/s (55.75s)"
    echo "============================================================"
    echo ""

    # 清理
    log_info "清理: 删除 pool 和 EC profile"
    delete_pool "$ec_pool"
    delete_pool "$rep_pool"
    "$ceph_path" osd erasure-code-profile rm "$ec_profile" 2>/dev/null || true

    log_info "组合池恢复性能测试完成"
}

main "$@"
