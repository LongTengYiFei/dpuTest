#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Ceph 压缩 vs 无压缩 副本池读性能对比测试脚本

流程：
  1. 创建压缩副本池 po1      (zlib + aggressive)
     创建无压缩副本池 po1_raw (不启用压缩)
  2. 将 DATA_DIR 下所有 .log 文件分块(4MB)分别写入两个池
  3. 读出全部对象，统计读吞吐(多轮取平均)
  4. 输出两个池的写/读性能 + 压缩比对比报告

用法:
  python3 compress_read_bench.py            # 完整流程(压缩池 + 无压缩池)
  python3 compress_read_bench.py --read-only # 仅读取已写入数据(跳过建池/写入)
  python3 compress_read_bench.py --rounds 3  # 读多轮取平均
  python3 compress_read_bench.py --skip-raw  # 跳过无压缩池
"""

import os
import sys
import time
import argparse
import subprocess

import rados

# ======================== 配置 ========================
CEPH_BIN    = "/home/cyf/ceph-dpu/build/bin/ceph"
# 使用 rados 默认配置文件列表(DEFAULT_CONF_FILES)，会依次读取
#   /etc/ceph/ceph.conf  (含 mon host / RDMA 设置)
#   ./ceph.conf          (build 目录下的 plugin dir = lib)
# 因此脚本需在 build 目录下运行，且 LD_LIBRARY_PATH 指向 build/lib
CEPH_CONF   = None             # None = rados.Rados.DEFAULT_CONF_FILES
DATA_DIR    = "/home/cyf/datasetsSmartCE/LogHub/Android_v2"
POOL_NAME   = "po1"          # 压缩池
RAW_POOL_NAME = "po1_raw"    # 无压缩池
PG_NUM      = 32
CHUNK_SIZE  = 4 * 1024 * 1024   # 4MB 每个对象
# ======================================================


def sh(*args):
    """执行 ceph CLI 命令，返回 stdout。"""
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"[CMD ERROR] {' '.join(args)}\n{r.stderr}\n")
    return r.stdout


def pool_exists(pool):
    out = sh(CEPH_BIN, "osd", "pool", "ls")
    return pool in out.split()


def wait_pool_ready(pool, timeout=180):
    """等待池所有 PG 变为 active+clean。"""
    import json
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = sh(CEPH_BIN, "pg", "ls-by-pool", pool, "-f", "json")
        try:
            data = json.loads(out)
            pgs = data.get("pg_stats", [])
            if pgs and all("active+clean" in p.get("state", "") for p in pgs):
                print(f"[POOL] {pool} 所有 PG active+clean ({len(pgs)} pgs)")
                return True
        except Exception:
            pass
        time.sleep(1)
    print(f"[POOL] 警告: {pool} 等待 PG 就绪超时({timeout}s)，继续尝试")
    return False


def create_pool(pool, compress=True):
    if pool_exists(pool):
        print(f"[POOL] 池 {pool} 已存在，先删除 ...")
        sh(CEPH_BIN, "osd", "pool", "delete", pool, pool,
           "--yes-i-really-really-mean-it")
        # 等待删除完成
        for _ in range(60):
            if not pool_exists(pool):
                break
            time.sleep(1)
        if pool_exists(pool):
            print(f"[POOL] 警告: 池 {pool} 仍在删除中，等待额外时间 ...")
            for _ in range(60):
                if not pool_exists(pool):
                    break
                time.sleep(1)

    print(f"[POOL] 创建池 {pool} (pg_num={PG_NUM}) ...")
    sh(CEPH_BIN, "osd", "pool", "create", pool, str(PG_NUM))
    # 等待池就绪
    for _ in range(30):
        if pool_exists(pool):
            break
        time.sleep(1)

    # 等待所有 PG active+clean，否则写入会报 ObjectNotFound
    wait_pool_ready(pool)

    if compress:
        print(f"[POOL] 设置 compression_algorithm=zlib ...")
        sh(CEPH_BIN, "osd", "pool", "set", pool, "compression_algorithm", "zlib")
        print(f"[POOL] 设置 compression_mode=aggressive ...")
        sh(CEPH_BIN, "osd", "pool", "set", pool, "compression_mode", "aggressive")
    else:
        print(f"[POOL] 设置 compression_mode=none (无压缩) ...")
        sh(CEPH_BIN, "osd", "pool", "set", pool, "compression_mode", "none")

    # 打印确认
    algo = sh(CEPH_BIN, "osd", "pool", "get", pool, "compression_algorithm").strip()
    mode = sh(CEPH_BIN, "osd", "pool", "get", pool, "compression_mode").strip()
    print(f"[POOL] 确认: {algo} / {mode}")


def collect_files(data_dir):
    """收集所有数据文件(跳过 .sh 等脚本)，返回 [(abs_path, oid_prefix), ...]"""
    skip_ext = {".sh", ".py", ".txt", ".backup"}
    files = []
    for root, _, names in os.walk(data_dir):
        for n in sorted(names):
            ext = os.path.splitext(n)[1].lower()
            if ext in skip_ext:
                continue
            ap = os.path.join(root, n)
            # oid 用相对路径+文件名作为前缀，去掉特殊字符
            rel = os.path.relpath(ap, data_dir)
            oid_prefix = rel.replace("/", "_").replace(".", "_")
            files.append((ap, oid_prefix))
    files.sort()
    return files


def write_files(ioctx, files):
    """写入所有文件，返回 (总字节, 总耗时, 对象列表)。"""
    total_bytes = 0
    total_objs  = 0
    t0 = time.time()

    for idx, (fpath, oid_prefix) in enumerate(files, 1):
        fsize = os.path.getsize(fpath)
        n_chunks = (fsize + CHUNK_SIZE - 1) // CHUNK_SIZE
        file_t0 = time.time()
        with open(fpath, "rb") as f:
            chunk_i = 0
            while True:
                buf = f.read(CHUNK_SIZE)
                if not buf:
                    break
                oid = f"{oid_prefix}__chunk{chunk_i:05d}"
                ioctx.write_full(oid, buf)
                chunk_i += 1
        file_dt = time.time() - file_t0
        total_bytes += fsize
        total_objs  += chunk_i
        print(f"  [WRITE {idx:2d}/{len(files)}] {os.path.relpath(fpath, DATA_DIR)} "
              f"size={fsize/1024/1024:8.2f}MB objs={chunk_i} "
              f"rate={fsize/1024/1024/file_dt:8.2f}MB/s")

    total_dt = time.time() - t0
    return total_bytes, total_dt, total_objs


def read_all(ioctx, files, rounds=1):
    """读出所有对象，返回 (总字节, 总耗时, 每轮详情)。"""
    # 先构建 (oid) 列表
    oid_list = []
    for fpath, oid_prefix in files:
        fsize = os.path.getsize(fpath)
        n_chunks = (fsize + CHUNK_SIZE - 1) // CHUNK_SIZE
        for c in range(n_chunks):
            oid_list.append((f"{oid_prefix}__chunk{c:05d}", fpath, c))

    total_bytes = 0
    round_details = []

    for r in range(1, rounds + 1):
        t0 = time.time()
        rd_bytes = 0
        for oid, fpath, c in oid_list:
            data = ioctx.read(oid, length=CHUNK_SIZE)
            rd_bytes += len(data)
        dt = time.time() - t0
        total_bytes = rd_bytes
        rate = rd_bytes / 1024 / 1024 / dt if dt > 0 else 0
        print(f"  [READ  round {r}/{rounds}] objs={len(oid_list)} "
              f"size={rd_bytes/1024/1024:8.2f}MB time={dt:6.2f}s "
              f"rate={rate:8.2f}MB/s")
        round_details.append((r, rd_bytes, dt, rate))

    # 平均吞吐
    if rounds > 1:
        avg_dt = sum(d for _, _, d, _ in round_details) / rounds
        avg_rate = total_bytes / 1024 / 1024 / avg_dt if avg_dt > 0 else 0
        print(f"  [READ  AVG    ] size={total_bytes/1024/1024:8.2f}MB "
              f"avg_time={avg_dt:6.2f}s avg_rate={avg_rate:8.2f}MB/s")

    return total_bytes, sum(d for _, _, d, _ in round_details), round_details


def pool_df(pool):
    """获取池的压缩前后字节数。"""
    out = sh(CEPH_BIN, "df", "detail", "-f", "json")
    import json
    try:
        data = json.loads(out)
        for p in data.get("pools", []):
            if p.get("name") == pool:
                stats = p.get("stats", {})
                stored  = stats.get("stored", 0)
                stored_raw = stats.get("stored_raw", 0)
                compress = stats.get("compress_bytes_used", 0)
                compress_under = stats.get("compress_under_bytes", 0)
                return {
                    "stored": stored,
                    "stored_raw": stored_raw,
                    "compress_bytes_used": compress,
                    "compress_under_bytes": compress_under,
                }
    except Exception as e:
        sys.stderr.write(f"[df parse error] {e}\n")
    return {}


def fmt_bytes(n):
    if n is None:
        return "N/A"
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if n < 1024:
            return f"{n:.2f}{unit}"
        n /= 1024
    return f"{n:.2f}PB"


def bench_pool(files, pool, compress, rounds, read_only):
    """对单个池执行 建池(可选)/写入/读取/统计，返回结果 dict。

    注意: 每个池使用独立的 cluster 连接，在 create_pool 之后才 connect，
    避免 librados 缓存旧的 pool id 映射(删除重建池后 pool id 会变化)。
    """
    tag = "压缩池" if compress else "无压缩池"
    print("\n" + "#" * 70)
    print(f"#  [{tag}] {pool}")
    print("#" * 70)

    if not read_only:
        create_pool(pool, compress=compress)

    # 建池后再连接集群，确保拿到最新 pool id
    cluster = rados.Rados(conffile=rados.Rados.DEFAULT_CONF_FILES)
    cluster.connect()
    print(f"[CLUSTER] 已连接 {pool}, fsid={cluster.get_fsid()}")

    ioctx = cluster.open_ioctx(pool)
    try:
        result = {
            "pool": pool,
            "compress": compress,
            "write_bytes": 0,
            "write_time": 0.0,
            "write_objs": 0,
            "read_bytes": 0,
            "read_time": 0.0,
            "read_avg_rate": 0.0,
            "stats": {},
        }

        if not read_only:
            # 写入
            print("\n----- [{}] 写入阶段 -----".format(tag))
            w_bytes, w_dt, w_objs = write_files(ioctx, files)
            print(f"\n[{tag} WRITE SUMMARY] "
                  f"files={len(files)} objs={w_objs} "
                  f"size={fmt_bytes(w_bytes)} time={w_dt:.2f}s "
                  f"rate={w_bytes/1024/1024/w_dt:.2f}MB/s")
            result["write_bytes"] = w_bytes
            result["write_time"]  = w_dt
            result["write_objs"]  = w_objs

            print("\n[WAIT] 等待 3s 让数据稳定 ...")
            time.sleep(3)

        # 读取
        print("\n----- [{}] 读取阶段 -----".format(tag))
        r_bytes, r_dt, r_details = read_all(ioctx, files, rounds=rounds)
        avg_r_dt = r_dt / rounds if rounds else 0
        avg_rate = r_bytes / 1024 / 1024 / avg_r_dt if avg_r_dt else 0
        print(f"\n[{tag} READ  SUMMARY] "
              f"size={fmt_bytes(r_bytes)} total_time={r_dt:.2f}s "
              f"avg_rate={avg_rate:.2f}MB/s (rounds={rounds})")
        result["read_bytes"]    = r_bytes
        result["read_time"]     = r_dt
        result["read_avg_rate"] = avg_rate

        # 压缩统计
        if not read_only:
            stats = pool_df(pool)
            result["stats"] = stats
            if compress and stats:
                print("\n----- [{}] 压缩统计 -----".format(tag))
                print(f"  stored (逻辑去重后) : {fmt_bytes(stats.get('stored'))}")
                print(f"  stored_raw (实际占用): {fmt_bytes(stats.get('stored_raw'))}")
                print(f"  compress_bytes_used : {fmt_bytes(stats.get('compress_bytes_used'))}")
                print(f"  compress_under_bytes: {fmt_bytes(stats.get('compress_under_bytes'))}")
                under = stats.get("compress_under_bytes", 0)
                used  = stats.get("compress_bytes_used", 0)
                if under and used:
                    ratio = under / used
                    saved = under - used
                    print(f"  压缩比 (under/used) : {ratio:.2f}x")
                    print(f"  节省空间           : {fmt_bytes(saved)}")
    finally:
        ioctx.close()
        cluster.shutdown()

    return result


def print_compare(results):
    """打印多池对比汇总表。"""
    print("\n" + "=" * 70)
    print("对比汇总")
    print("=" * 70)
    hdr = f"{'池名':<12}{'压缩':<8}{'写吞吐(MB/s)':<16}{'读吞吐(MB/s)':<16}{'实际占用':<12}"
    print(hdr)
    print("-" * len(hdr))
    for r in results:
        w_rate = r["write_bytes"] / 1024 / 1024 / r["write_time"] if r["write_time"] else 0
        r_rate = r["read_avg_rate"]
        used = r["stats"].get("compress_bytes_used") or r["stats"].get("stored_raw") or 0
        comp = "zlib" if r["compress"] else "none"
        print(f"{r['pool']:<12}{comp:<8}{w_rate:<16.2f}{r_rate:<16.2f}{fmt_bytes(used):<12}")
    print("=" * 70)


def main():
    ap = argparse.ArgumentParser(description="Ceph 压缩 vs 无压缩 副本池读性能对比")
    ap.add_argument("--data-dir", default=DATA_DIR,
                    help=f"数据目录 (默认: {DATA_DIR})")
    ap.add_argument("--read-only", action="store_true",
                    help="跳过建池/写入，仅读取已有数据")
    ap.add_argument("--rounds", type=int, default=1,
                    help="读取轮数(多轮取平均)")
    ap.add_argument("--skip-raw", action="store_true",
                    help="跳过无压缩池测试")
    args = ap.parse_args()

    data_dir = args.data_dir
    pools = [(POOL_NAME, True)]
    if not args.skip_raw:
        pools.append((RAW_POOL_NAME, False))

    print("=" * 70)
    print("Ceph 压缩 vs 无压缩 副本池读性能对比")
    print(f"  DATA_DIR  = {data_dir}")
    print(f"  POOLS     = {pools}")
    print(f"  CHUNK     = {CHUNK_SIZE/1024/1024}MB")
    print(f"  ROUNDS    = {args.rounds}")
    print("=" * 70)

    files = collect_files(data_dir)
    if not files:
        print(f"[ERROR] {data_dir} 下未找到数据文件")
        sys.exit(1)
    total_logical = sum(os.path.getsize(f) for f, _ in files)
    print(f"[SCAN] 找到 {len(files)} 个数据文件, 总计 {fmt_bytes(total_logical)}")

    results = []
    for pool, compress in pools:
        r = bench_pool(files, pool, compress,
                       rounds=args.rounds, read_only=args.read_only)
        results.append(r)

    if len(results) > 1:
        print_compare(results)

    print("\n" + "=" * 70)
    print("测试完成")
    print("=" * 70)


if __name__ == "__main__":
    main()
