# Ceph 自研集群环境交接说明

> 更新时间：2026-10-27（UTC）  
> 用途：将当前集群环境、部署方式和实验前置条件交接给后续实验人员或其他对话。

## 1. 集群概况

这是一个使用自研 Ceph 源码构建的 Ceph Quincy 集群，不使用系统安装的 Ceph daemon。

| 项目 | 当前值 |
|---|---|
| Ceph 版本 | Quincy 17.2.7，自研修改版 |
| 源码目录 | `/home/cyf/ceph-dpu` |
| 自研二进制目录 | `/home/cyf/ceph-dpu/build/bin` |
| FSID | `886cee71-e800-4d78-95f4-43b791bfeb50` |
| 认证 | cephx |
| 网络传输 | `async+posix`（TCP/IPoIB datagram） |
| OSD 数据接口 | `ibs21f1`，200G HDR，MTU 2044 |
| MON/管理接口 | `ens25f3`，`1000Mb/s` |
| MON | node1，单 MON quorum |
| MGR | node1，daemon 名称为 `m1` |
| OSD | 12 个，node1、node2、node3 各 4 个 |
| 客户端 | node4，不运行 MON/MGR/OSD |
| SSH 端口 | `23579` |
| sudo | 已配置免密 sudo |

节点地址：

| 节点 | 管理地址 | IPoIB 数据地址 | 用途 |
|---|---|---|---|
| node1 | `10.26.57.54` | `10.200.0.1` | MON、MGR、4 OSD |
| node2 | `10.26.57.55` | `10.200.0.2` | 4 OSD |
| node3 | `10.26.57.56` | `10.200.0.3` | 4 OSD |
| node4 | `10.26.57.57` | `10.200.0.4` | 实验客户端 |

2026-10-26 重建完成后的状态：

```text
mon: 1 daemons, quorum node1
mgr: m1(active)
osd: 12 osds: 12 up, 12 in
pgs: active+clean
raw capacity: 约 21 TiB
```

目前只有 Ceph 自动创建的 `.mgr` 池。以前的实验池和实验数据已经不存在，后续实验需要重新创建池并装载数据。

## 2. OSD 与磁盘映射

所有 OSD 都直接使用整块裸 NVMe，不使用分区。

| 节点 | OSD | 设备 |
|---|---:|---|
| node1 | osd.0 | `/dev/nvme0n1` |
| node1 | osd.3 | `/dev/nvme1n1` |
| node1 | osd.6 | `/dev/nvme2n1` |
| node1 | osd.9 | `/dev/nvme3n1` |
| node2 | osd.1 | `/dev/nvme1n1` |
| node2 | osd.4 | `/dev/nvme2n1` |
| node2 | osd.7 | `/dev/nvme3n1` |
| node2 | osd.10 | `/dev/nvme4n1` |
| node3 | osd.2 | `/dev/nvme1n1` |
| node3 | osd.5 | `/dev/nvme2n1` |
| node3 | osd.8 | `/dev/nvme3n1` |
| node3 | osd.11 | `/dev/nvme4n1` |

这些设备均为约 1.7 TiB 的 Intel NVMe。

## 3. 网络传输配置

2026-10-27 已将 OSD public/cluster 数据路径切换到 TCP/IPoIB。Ceph transport
保持 `async+posix`，因此不使用 Ceph native RDMA；对象写入和 OSD 间恢复流量
通过 `mlx5_1` 对应的 `ibs21f1` 和 `10.200.0.0/24` 传输。底层链路为
200G HDR InfiniBand，IPoIB 使用 datagram 模式，实际 MTU 为 2044。

MON 仍绑定 `10.26.57.54:6789`，管理和 MON 控制流量继续走 `ens25f3`
的 1GbE；这不承载主要对象数据路径。

当前核心配置：

```ini
ms_type = async+posix
ms_cluster_type = async+posix
```

已通过运行时配置确认 MON、MGR 和全部 12 个 OSD 的 `ms_type` 均为
`async+posix`。全部 OSD 的 public/cluster 地址已确认是 `10.200.0.1-3`。
OSD 专用配置为：

```ini
[osd]
public network = 10.200.0.0/24
cluster network = 10.200.0.0/24
```

从 1GbE TCP 切换到 IPoIB 前的节点实际配置保存在：

```text
/etc/ceph/ceph.conf.pre_ipoib_20261027
```

从 native RDMA 切换到 TCP 前的节点实际配置保存在：

```text
/etc/ceph/ceph.conf.pre_tcp_20261026T1450Z
```

IPoIB 已持久化：

```text
/etc/modules-load.d/ib_ipoib.conf
/etc/netplan/60-ipoib.yaml
```

以下 GID 和 RDMA 参数是切换前的历史记录；当前 TCP transport 不使用它们。

各节点 `mlx5_1` 的 GID：

```text
node1: fe80:0000:0000:0000:a088:c203:0031:f04d
node2: fe80:0000:0000:0000:a088:c203:00aa:b94d
node3: fe80:0000:0000:0000:a088:c203:00ab:1f6d
node4: fe80:0000:0000:0000:a088:c203:0031:ed09
```

以上 GID 已在 2026-10-26 重建前通过各节点 `show_gids` 重新读取；与
2026-10-12 的旧记录相比发生了轮换，后续不要继续使用旧值。

原 RDMA 核心配置：

```ini
ms_type = async+rdma
ms_cluster_type = async+rdma
ms_async_rdma_device_name = mlx5_1
ms_async_rdma_polling_us = 0
```

每台机器的 `ms_async_rdma_local_gid` 使用该机器对应的 GID。这些配置行仍可保留在
`ceph.conf` 中，但在 `async+posix` 下不会生效。

节点配置模板：

```text
/home/cyf/dpuTest/ceph_custom_deploy/node1-ceph.conf
/home/cyf/dpuTest/ceph_custom_deploy/node2-ceph.conf
/home/cyf/dpuTest/ceph_custom_deploy/node3-ceph.conf
```

实际配置安装在：

```text
/etc/ceph/ceph.conf
```

node4 已安装 admin keyring，因此可以直接执行：

```bash
cd /home/cyf/ceph-dpu/build/bin
./ceph -s
./ceph osd tree
```

### 3.1 2026-10-27 RDMA 与 TCP/IPoIB 恢复矩阵

使用完全相同的 6+2 EC、128 PG、约 128 GiB 数据、故障 OSD 0/3 和目标
OSD 2/5，分别在 native RDMA 与 TCP/IPoIB 下测试
`osd_max_backfills=1/2/4/8`，每个组合独立测试三次。

平均恢复时间与 Ceph 逻辑恢复速率如下：

| backfills | IPoIB 时间 | RDMA 时间 | RDMA 时间优势 | IPoIB 速率 | RDMA 速率 |
|---:|---:|---:|---:|---:|---:|
| 1 | 482.07s | 456.01s | 5.41% | 237.44 MiB/s | 255.34 MiB/s |
| 2 | 259.81s | 236.06s | 9.14% | 417.79 MiB/s | 474.12 MiB/s |
| 4 | 151.78s | 133.89s | 11.79% | 735.82 MiB/s | 878.04 MiB/s |
| 8 | 92.11s | 81.82s | 11.18% | 1261.31 MiB/s | 1477.80 MiB/s |

完整对比报告：

```text
/home/cyf/ceph-dpu/zhongdian/ec62_rdma_vs_tcp_ipoib_backfill_comparison_20261027.md
/home/cyf/ceph-dpu/zhongdian/ec62_rdma_vs_tcp_ipoib_backfill_comparison_20261027.json
```

原始矩阵：

```text
/home/cyf/ceph-dpu/zhongdian/ec62_tcp_ipoib_backfill_matrix_20261027T081907Z
/home/cyf/ceph-dpu/zhongdian/ec62_rdma_backfill_matrix_20261027T101200Z
```

测试结束后已恢复为 TCP/IPoIB。切换 RDMA 前的 node1-node4 当前配置备份为：

```text
/etc/ceph/ceph.conf.pre_rdma_matrix_20261027T100947Z
```

## 4. 自研代码版本

集群存在历史遗留的源码版本差异。这是此前实验已经使用过的状态，不要直接覆盖远端源码树。

```text
node1/node2:
  ceph version fdb0f1061
  git commit 8fdb0f106133d896028732ad4871745158f13432

node3:
  ceph version a8ee6b4ae
  git commit 5a8ee6b4ae4106e8b7c1719a4ad16ca12eb6e9fa
```

MON、MGR 以及 node1/node2 的 8 个 OSD 使用 `fdb0f1061`；node3 的 4 个 OSD 使用 `a8ee6b4ae`。

所有运行进程都已检查，实际 executable 是：

```text
/home/cyf/ceph-dpu/build/bin/ceph-mon
/home/cyf/ceph-dpu/build/bin/ceph-mgr
/home/cyf/ceph-dpu/build/bin/ceph-osd
```

## 5. 本次集群如何创建

### 5.1 MON/MGR

node1 使用以下脚本从空集群重建：

```text
/home/cyf/ceph-dpu/AAA-GlobalTools/rebuildCeph.sh
```

配置备份：

```text
/home/cyf/ceph-dpu/AAA-GlobalTools/ceph.conf.backup
```

脚本完成以下操作：

1. 生成 MON、admin 和 bootstrap-osd keyring。
2. 创建 monmap。
3. 初始化 node1 MON。
4. 从 `build/bin` 直接启动 `ceph-mon`。
5. 创建并启动 `mgr.m1`。

MON/MGR 不是由 systemd 管理，而是自研二进制直接 daemonize 到后台。

> **危险：** `rebuildCeph.sh` 会删除 `/var/lib/ceph`。它只能用于整个集群从零重建，正常重启现有集群时绝对不能再次运行。

在全新的 `/etc/ceph` 上，该脚本存在权限顺序问题，需要先执行：

```bash
sudo mkdir -p /etc/ceph
sudo chown -R cyf:cyf /etc/ceph
```

### 5.2 OSD

本次没有使用 `createOsdGlobal.sh`，因为该脚本调用系统安装的 `ceph-volume`，不能保证运行的是自研代码。

本次采用直接裸盘初始化方式：

1. 手工在 node1 的 `/dev/nvme0n1` 上创建 `osd.0`。
2. 使用以下脚本创建 `osd.1` 至 `osd.11`：

```text
/home/cyf/dpuTest/ceph_custom_deploy/create_remaining_custom_osds.sh
```

OSD systemd 模板：

```text
/home/cyf/dpuTest/ceph_custom_deploy/ceph-custom-osd@.service
/etc/systemd/system/ceph-custom-osd@.service
```

其核心启动程序是：

```text
/home/cyf/ceph-dpu/build/bin/ceph-osd
```

全部 12 个 OSD 服务都已经 enabled，服务名称为：

```text
ceph-custom-osd@0.service
ceph-custom-osd@1.service
...
ceph-custom-osd@11.service
```

## 6. 正常重启现有集群

> 正常重启时不要运行 `rebuildCeph.sh`。

先在 node1 启动 MON/MGR：

```bash
ssh node1 -p 23579

sudo mkdir -p /var/run/ceph /var/log/ceph
sudo chown cyf:cyf /var/run/ceph /var/log/ceph

export LD_LIBRARY_PATH=/usr/local/lib/ceph/dpu:/home/cyf/ceph-dpu/build/lib:/opt/mellanox/doca/lib/x86_64-linux-gnu

/home/cyf/ceph-dpu/build/bin/ceph-mon -i node1

/home/cyf/ceph-dpu/build/bin/ceph-mgr \
  -i m1 \
  --mgr-module-path /home/cyf/ceph-dpu/src/pybind/mgr
```

然后启动各节点 OSD：

```bash
ssh node1 -p 23579 \
  "sudo systemctl start ceph-custom-osd@{0,3,6,9}.service"

ssh node2 -p 23579 \
  "sudo systemctl start ceph-custom-osd@{1,4,7,10}.service"

ssh node3 -p 23579 \
  "sudo systemctl start ceph-custom-osd@{2,5,8,11}.service"
```

检查集群：

```bash
/home/cyf/ceph-dpu/build/bin/ceph -s
/home/cyf/ceph-dpu/build/bin/ceph osd tree
```

滚动重启全部 OSD 可以使用：

```text
/home/cyf/dpuTest/ceph_custom_deploy/restart_custom_osds_current.sh
```

该脚本会设置 `noout`，逐个重启 OSD 并等待 PG clean，最后解除 `noout`。

## 7. 当前自研实验参数

全部 12 个 OSD 当前运行值：

```text
bluestore_dpu_decompression_batch_size = 32
bluestore_dpu_input_zero_copy = false
bluestore_dpu_input_pool_slots = 128
```

也就是当前处于 `Batch` 状态，未启用输入内存池。

如果要运行 `batch=1 + memory pool`：

```bash
ceph=/home/cyf/ceph-dpu/build/bin/ceph

$ceph config set osd bluestore_dpu_decompression_batch_size 1 --force
$ceph config set osd bluestore_dpu_input_zero_copy true --force
$ceph config set osd bluestore_dpu_input_pool_slots 128 --force

bash /home/cyf/dpuTest/ceph_custom_deploy/restart_custom_osds_current.sh
```

修改后必须检查全部 12 个 OSD 的实际运行值，不能只检查 MON 中保存的目标值：

```bash
$ceph tell 'osd.*' config get bluestore_dpu_decompression_batch_size
$ceph tell 'osd.*' config get bluestore_dpu_input_zero_copy
$ceph tell 'osd.*' config get bluestore_dpu_input_pool_slots
```

## 8. 健康告警和已知问题

当前 `HEALTH_WARN` 来自：

- `auth_allow_insecure_global_id_reclaim`。
- mgr 的 `nfs`、`orchestrator`、`volumes` 模块缺少 `nvmeof` 依赖。
- MON 只绑定 msgr1：`10.26.57.54:6789`，没有启用 msgr2。

这些告警不涉及 OSD、PG 或对象数据路径。

其他已知现象：

- 使用原 `async+rdma` 配置时，CLI 偶尔打印 `Infiniband send_cm_meta ... Connection refused`；切换到 TCP 后不再走该 RDMA 数据路径。
- 该自研版本执行 `ceph-osd --mkfs` 时，会在写出 `ready` 标记后以 SIGSEGV、`rc=139` 退出。12 个 OSD 均出现过这个现象，但随后正式 daemon 都正常启动并保持 `up/in`。不要因为这个 mkfs 退出码直接重复格式化磁盘。
- node4 已完成临时 RADOS 池的写入、读回比对和删除测试，测试池已清理。
- 当前 OSD map 没有设置 `noout`。

## 9. 常用检查命令

```bash
ceph=/home/cyf/ceph-dpu/build/bin/ceph

$ceph -s
$ceph health detail
$ceph osd tree
$ceph versions
$ceph osd pool ls detail

$ceph tell 'osd.*' config get ms_type
$ceph tell 'osd.*' config get ms_async_rdma_local_gid
$ceph tell 'osd.*' config get bluestore_dpu_decompression_batch_size
$ceph tell 'osd.*' config get bluestore_dpu_input_zero_copy
$ceph tell 'osd.*' config get bluestore_dpu_input_pool_slots
```

