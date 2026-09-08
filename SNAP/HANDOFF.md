# BlueField-3 SNAP 存储测试交接手册

更新日期：2026-09-08 UTC

本文是 node4、node4 BlueField-3 DPU 与 node3 之间 SPDK/SNAP
测试的交接入口。目标是让接手者能够恢复已经验证过的数据路径、复现
Baseline 1/2，并按统一口径继续完成 Linux 内核路径的 Baseline 3/4。

## 1. 当前结论

已经完成并验证两条用户态路径：

```text
Baseline 1：node4 Host SPDK
            -> NVMe-oF/RDMA
            -> node3 Host SPDK target
            -> Intel P5530

Baseline 2：node4 Host SPDK
            -> BlueField SNAP 模拟 PCIe NVMe
            -> DPA
            -> DPU SPDK NVMe-oF/RDMA initiator
            -> node3 Host SPDK target
            -> Intel P5530
```

关键观察如下：

- Host SPDK 能直接接管 SNAP 模拟的 PCIe NVMe，SNAP 设备并非只能通过
  Linux 内核/POSIX 接口使用。
- Baseline 2 的高并发吞吐与 Baseline 1 接近，差距为 0.91%～5.93%。
- Baseline 2 的主要代价出现在低队列深度：4 KiB 随机读 QD1 的平均延迟
  从 11.85 us 增加到 28.06 us，IOPS 下降 57.73%。
- 当前测试只衡量了插入 SNAP 后的数据面代价。两边 Host 均运行 SPDK
  轮询程序，因此尚不能证明 SNAP 是否降低 Host CPU 消耗，也没有验证
  普通 Linux 应用的透明使用体验。
- 不应根据 Baseline 1/2 直接得出“SNAP 有效”或“SNAP 无效”的产品结论。
  下一阶段必须完成匹配的 Linux 内核直连与 Linux 经 SNAP 对照。

完整原始结果见 [RESULTS.md](RESULTS.md)，对比图如下：

![Baseline 1 versus Baseline 2](results/baseline1-vs-baseline2.png)

## 2. 组件角色

SNAP 不是远端存储 target。它运行在消费端服务器的 BlueField DPU 上，
向本机 Host 模拟一块 PCIe NVMe 设备，再通过 SPDK bdev 连接本地或远端
backend。

```text
Host/VM
  │ 标准 PCIe NVMe 命令
  ▼
SNAP 模拟控制器
  │ backend I/O
  ▼
SPDK NVMe-oF initiator
  │ RDMA/TCP
  ▼
SPDK target / Linux nvmet / 存储阵列
```

在协议角色上，SNAP 朝 Host 一侧表现为 NVMe 控制器，朝远端存储一侧
表现为 initiator。服务端 target 的 DPA 卸载属于 DOCA STA/SPDK target
方向，不属于 SNAP。本轮为了只测消费端 SNAP 开销，node3 target 在
Baseline 1/2 中保持完全一致。

## 3. 测试环境清单

| 角色 | 地址/设备 | 说明 |
|---|---|---|
| node4 Host | IPoIB `192.168.200.2/24`，接口 `ibs21f1` | Baseline 1/3 initiator；SPDK v26.05 |
| node4 DPU 管理口 | `ssh cyf@192.168.100.2` | 已配置密钥登录；DPU hostname 显示为 `localhost` |
| node4 DPU 数据口 | IPoIB `192.168.200.4/24`，接口 `ib1` | SNAP 到 node3 的数据路径 |
| node4 SNAP PF | `0000:b1:00.2`，ID `15b3:6001` | Mellanox NVMe SNAP Controller |
| node3 Host | `ssh -p 23579 cyf@node3` | SPDK NVMe-oF target；SPDK v26.05 |
| node3 数据口 | IPoIB `192.168.200.3/24`，接口 `ibs21f1` | NVMe-oF/RDMA listener |
| node3 测试盘 | PCI `0000:65:00.0`，`/dev/nvme1n1` | Intel P5530 1.7 TB，无文件系统 |
| 测试盘序列号 | `PHAO332302201P9SGN` | 所有破坏性操作必须同时核对 BDF 和序列号 |
| NVMe-oF 服务 | `192.168.200.3:4420` | NQN `nqn.2026-09.io.spdk:node3-nvme1` |

SNAP 已验证的软件配置：

- BlueField-3，32 GiB 内存。
- DOCA 2.7 环境。
- SNAP 容器 `nvcr.io/nvidia/doca/doca_snap:4.4.0-doca2.7.0`。
- SNAP 4.4.0，环境变量 `NVME_EMU_PROVIDER=dpa`。
- 容器内 SPDK v24.01。
- DPU backend 名称 `RemoteNvme0n1`，配置 16 个 NVMe-oF I/O queues。
- SNAP subsystem NQN `nqn.2026-09.io.snap:node4-remote-nvme1`。
- SNAP controller `SnapCtrl0`，PF 0，Host BDF `0000:b1:00.2`。
- 模拟控制器曾报告 31 个 I/O queues、64 个 MSI-X。
- node3 RDMA transport 使用 `-m 64`，为 DPU 的 16 个 backend 队列留出余量。

## 4. 交接时点的运行状态

以下状态由 2026-09-08 10:45 UTC 的只读检查得到。它们不是持久保证，
每次测试前仍须重新检查。

| 项目 | 当前状态 |
|---|---|
| node4 SNAP PF | 存在，绑定 `uio_pci_generic` |
| node4 Host `ibs21f1` | `DOWN`，当前无 `192.168.200.2` |
| node4 Host `opensmd` | `inactive`；不代表整个 fabric 一定没有其他 SM |
| node4 DPU `ib1` | `UP`，地址 `192.168.200.4/24` |
| node3 `ibs21f1` | `UP`，地址 `192.168.200.3/24` |
| node3 SPDK target | PID 文件指向的进程已停止 |
| node3 测试盘 BDF | 仍绑定 `uio_pci_generic` |
| SNAP 容器 | 此次未以 root 权限复查；最后一次成功测试时运行正常 |

因此，此刻不能直接重跑任何 baseline。至少需要先恢复 node3 target，
并根据所测路径恢复 node4 Host 或 DPU 的网络与 SNAP 状态。

## 5. 安全红线

1. 所有写测试都会覆盖 node3 的测试盘内容。测试盘虽无文件系统，但仍要
   在每次写入前确认授权。
2. node3 上还有其他正在使用的 NVMe/Ceph 设备。严禁无参数运行可能批量
   绑定 NVMe 的 SPDK `setup.sh`。
3. node3 只允许操作同时满足以下三个身份条件的设备：
   `0000:65:00.0`、`/dev/nvme1n1`、序列号 `PHAO332302201P9SGN`。
4. node4 只允许为 Baseline 2/4 切换 SNAP PF `0000:b1:00.2`
   (`15b3:6001`)；不得改绑四块本地 Intel NVMe。
5. `/dev/nvmeXn1` 编号会随连接与驱动切换变化。后续内核测试不得仅凭
   设备名选择写入目标，必须核对 NQN、serial、model、UUID 和 PCI BDF。
6. 不要在 SPDK 仍占用 SNAP PF 时把它切换给内核，反之亦然。
7. `mkfs`、分区和挂载不是原始块设备性能测试的必要步骤。Baseline 3/4
   应首先使用 raw block + `direct=1`，避免误格式化和 page cache 干扰。

现有 `node3_prepare.sh` 已实现 hostname、BDF、序列号、挂载和 holder 检查；
不要绕开这些保护逻辑。

## 6. Baseline 1：Host SPDK 直接连接远端 target

### 6.1 路径与目的

```text
node4 SPDK v26.05（CPU 24）
  -> Host IPoIB 192.168.200.2
  -> NVMe-oF/RDMA
  -> node3 SPDK target 192.168.200.3:4420
  -> P5530
```

这是当前最短的软件路径，用作性能上限参考。Host 应用必须使用 SPDK；
它不提供普通 `/dev/nvmeXn1`。

### 6.2 恢复与运行

先确认 InfiniBand fabric 存在活动 Subnet Manager，端口状态应为 `Active`
且 `SM lid` 非 0。仅有 `Physical state: LinkUp` 不代表 IB 已可用。

node3：

```bash
cd /home/cyf/dpuTest/SNAP
sudo ./node3_prepare.sh
sudo ./node3_start_target.sh
```

node4：

```bash
cd /home/cyf/dpuTest/SNAP
sudo ./client_prepare.sh
./run_baseline1.sh randread 4096 1 30
./run_baseline1.sh randread 4096 64 30
ALLOW_DESTRUCTIVE_WRITE=YES ./run_baseline1.sh randwrite 4096 64 30
./run_baseline1.sh read 131072 32 30
ALLOW_DESTRUCTIVE_WRITE=YES ./run_baseline1.sh write 131072 32 30
```

### 6.3 已完成结果

| 负载 | QD | IOPS | 带宽 | 平均延迟 | P50 | P99 |
|---|---:|---:|---:|---:|---:|---:|
| 4 KiB 随机读 | 1 | 84,246 | 329 MiB/s | 11.85 us | 10.81 us | 59.38 us |
| 4 KiB 随机读 | 64 | 415,103 | 1,621 MiB/s | 154.16 us | 154.45 us | 202.35 us |
| 4 KiB 随机写 | 64 | 704,012 | 2,750 MiB/s | 90.89 us | 58.41 us | 381.23 us |
| 128 KiB 顺序读 | 32 | 54,828 | 6,853 MiB/s | 583.62 us | 633.43 us | 1,032.26 us |
| 128 KiB 顺序写 | 32 | 26,035 | 3,254 MiB/s | 1,229.15 us | 1,227.76 us | 3,128.06 us |

本表使用的五个原始日志分别是：

```text
baseline1-randread-4096B-qd1-20260908T055203Z.log
baseline1-randread-4096B-qd64-20260908T055054Z.log
baseline1-randwrite-4096B-qd64-20260908T055343Z.log
baseline1-read-131072B-qd32-20260908T055246Z.log
baseline1-write-131072B-qd32-20260908T055423Z.log
```

`results/` 中另有早期 smoke/重试日志；生成正式对比时不要误选。

## 7. Baseline 2：Host SPDK 消费 SNAP 模拟盘

### 7.1 路径与目的

```text
node4 Host SPDK v26.05（CPU 24，IOVA=PA）
  -> PCIe 0000:b1:00.2
  -> SNAP 4.4.0 / DPA
  -> DPU SPDK v24.01 / RDMA zero copy backend
  -> DPU IPoIB 192.168.200.4
  -> node3 SPDK target 192.168.200.3:4420
  -> P5530
```

Baseline 2 用于隔离 SNAP 插入后的数据面开销。Host SNAP PF 绑定
`uio_pci_generic`，所以不会生成 `/dev/nvmeXn1`，`lsblk` 看不到它是
预期行为。SPDK 通过 `trtype:PCIe traddr:0000:b1:00.2` 直接探测设备。

### 7.2 恢复与运行

1. 按 Baseline 1 的 node3 步骤启动 target。
2. 在 DPU 上确认 `ib1`、hugepages、SNAP pod、backend、namespace 和
   controller 状态；如有队列问题，执行已经修正的重启脚本：

   ```bash
   ssh -t cyf@192.168.100.2 \
     'sudo /home/cyf/baseline2_stage/dpu_restart_baseline2_with_queues.sh'
   ```

3. DPU 侧至少确认：

   - backend `RemoteNvme0` 为 `enabled`；
   - namespace 不再是 `In_progress`；
   - controller `SnapCtrl0` 为 `STARTED`；
   - 启动日志没有 `No free I/O queue IDs`。

4. node4 Host 运行：

   ```bash
   cd /home/cyf/dpuTest
   sudo ./SNAP/run_baseline2_suite.sh
   ```

该 suite 包含写测试，会覆盖测试盘。

### 7.3 已完成结果

| 负载 | QD | IOPS | 带宽 | 平均延迟 | P50 | P99 |
|---|---:|---:|---:|---:|---:|---:|
| 4 KiB 随机读 | 1 | 35,614 | 139 MiB/s | 28.06 us | 21.87 us | 81.13 us |
| 4 KiB 随机读 | 64 | 390,483 | 1,525 MiB/s | 163.88 us | 154.45 us | 306.94 us |
| 4 KiB 随机写 | 64 | 666,136 | 2,602 MiB/s | 96.06 us | 89.44 us | 244.38 us |
| 128 KiB 顺序读 | 32 | 53,484 | 6,685 MiB/s | 598.30 us | 594.33 us | 969.70 us |
| 128 KiB 顺序写 | 32 | 25,799 | 3,225 MiB/s | 1,240.38 us | 1,165.20 us | 3,143.70 us |

本轮五项测试使用统一时间戳 `20260908T085552Z`，汇总文件为
`results/baseline2-suite-20260908T085552Z.summary`。

### 7.4 Baseline 2 相对 Baseline 1

| 负载 | IOPS/带宽变化 | 平均延迟变化 | P50 变化 | P99 变化 |
|---|---:|---:|---:|---:|
| 4 KiB 随机读 QD1 | -57.73% | +136.79% | +102.26% | +36.63% |
| 4 KiB 随机读 QD64 | -5.93% | +6.31% | 0.00% | +51.69% |
| 4 KiB 随机写 QD64 | -5.38% | +5.69% | +53.14% | -35.90% |
| 128 KiB 顺序读 QD32 | -2.45% | +2.52% | -6.17% | -6.06% |
| 128 KiB 顺序写 QD32 | -0.91% | +0.91% | -5.10% | +0.50% |

P99 的改善与恶化混杂，且每项只运行一次，不能据此得出稳定的尾延迟结论。

## 8. 后续实验矩阵（编号以本节为准）

Baseline 表示存储数据路径；`psync`、`libaio` 和 `io_uring` 是同一路径下
的 I/O engine 子项，不再各自占用一个顶层 baseline 编号。

| Baseline | Host 消费栈 | 到 node3 的路径 | I/O engine | 状态 | 回答的问题 |
|---|---|---|---|---|---|
| 1 | SPDK | Host 直接 NVMe-oF/RDMA | SPDK | 已完成 | 用户态最短路径性能 |
| 2 | SPDK | SNAP/DPA 转发 NVMe-oF/RDMA | SPDK | 已完成 | SPDK 下插入 SNAP 的纯数据面代价 |
| 3 | Linux `nvme-rdma` | Host 直接 NVMe-oF/RDMA | fio `psync`、`libaio`、`io_uring` | 待完成 | 常规 Linux 直连路径 |
| 4 | Linux `nvme` | SNAP 模拟本地 PCIe NVMe | fio `psync`、`libaio`、`io_uring` | 待完成 | SNAP 的常规产品用法 |

比较关系必须保持如下：

```text
Baseline 2 - Baseline 1
  = 相同 SPDK 消费方式下的 SNAP 额外开销

Baseline 4 - Baseline 3
  = 相同 Linux engine 下的 SNAP 额外开销/收益

Baseline 3 内部、Baseline 4 内部
  = psync、libaio、io_uring 的提交模型差异

Baseline 3/4 的 Host CPU 指标
  = SNAP 是否真正降低 Host 侧存储/网络处理成本
```

不能仅用 Baseline 4 与 Baseline 1 比较并归因于 SNAP，因为其中同时改变了
SNAP、内核驱动和 I/O API。

## 9. Baseline 3/4 的统一 fio 设计

### 9.1 测试介质与语义

- 使用 raw block device，不创建文件系统。
- 强制 `direct=1`，避免 page cache 把内存性能误当成存储性能。
- 使用 `time_based=1`、相同 runtime 和 ramp time。
- 输出 fio JSON，保留完整 percentile、CPU 和错误信息。
- 每条路径必须运行完全相同的 workload、块大小、读写方向和并发度。

### 9.2 工作负载

| 工作负载 | 块大小 | 并发深度 | psync | libaio | io_uring |
|---|---:|---:|---|---|---|
| 随机读 | 4 KiB | QD1 | 测 | 测 | 测 |
| 随机读 | 4 KiB | QD64 | 不作为单 job 对等项 | 测 | 测 |
| 随机写 | 4 KiB | QD64 | 不作为单 job 对等项 | 测 | 测 |
| 顺序读 | 128 KiB | QD32 | 不作为单 job 对等项 | 测 | 测 |
| 顺序写 | 128 KiB | QD32 | 不作为单 job 对等项 | 测 | 测 |

`psync` 单 job 即使设置 `iodepth=64`，也不会形成与异步 engine 等价的
64 个在途请求。若需要模拟同步多线程，应作为单独的 `psync + numjobs=N`
场景报告，不能混入异步 QD64 对比。

第一轮 `io_uring` 使用默认模式，不启用 `SQPOLL`、`IOPOLL/hipri` 等会
额外占用轮询 CPU 或改变驱动行为的选项。这些优化可作为后续扩展实验。

### 9.3 必须采集的指标

- IOPS、MiB/s。
- 平均、P50、P95、P99、P99.9 延迟。
- fio `usr_cpu`、`sys_cpu`。
- Host 总 CPU、测试进程 cycles、instructions、context switches。
- 在相同吞吐或相同 offered load 下的 cycles/IO。
- Baseline 2/4 的 DPU ARM CPU、内存、hugepages 和 DPA/SNAP telemetry。
- NVMe-oF 重连、I/O error、timeout 和 controller reset。

SPDK poll-mode 在饱和测试中通常会占满指定核心，因此只比较 `%CPU` 不够，
需要结合完成 I/O 数计算 cycles/IO。要验证 offload，最好再增加固定 IOPS
而非只跑满的测试。

### 9.4 正式结果的质量要求

目前 Baseline 1/2 都是每项一次、30 秒的初测，SSD 未预处理，CPU 频率未
固定，也没有置信区间。正式结论至少应做到：

1. SSD 按统一方案预处理并记录盘容量使用状态。
2. 固定 CPU governor、测试核、NUMA 节点和 IRQ/RPS/XPS 配置。
3. 每项预热后重复 3～5 次，报告中位数及离散程度。
4. 随机化或交替 Baseline 3/4 的运行次序，降低温度、缓存和盘状态漂移。
5. 每轮前后记录 firmware、kernel、SPDK、DOCA/SNAP、fio 版本。
6. 明确 GB/s 与 GiB/s、MB/s 与 MiB/s，不混用单位。
7. 写测试结束后留出一致的恢复时间，并检查介质温度和后台 GC 影响。

## 10. Baseline 3/4 实施注意事项

### 10.1 Baseline 3：Linux 原生 NVMe-oF

Host 使用内核 `nvme-rdma` 和 `nvme-cli` 连接 node3 的同一个 NQN。连接后
应通过 `nvme list-subsys` 确认 transport、traddr、NQN，再解析对应的
`/dev/nvmeXn1`。测试结束后断开该 NQN，避免与 Baseline 4 的 SNAP 盘混淆。

执行 Baseline 3 前需要恢复 node4 Host `ibs21f1` 的
`192.168.200.2/24`，并验证它能直接访问 `192.168.200.3:4420`。

### 10.2 Baseline 4：Linux 消费 SNAP 本地盘

Baseline 4 要把 `0000:b1:00.2` 从 `uio_pci_generic` 精确切换到 Linux
`nvme` 驱动。成功后应满足：

```text
lspci -nnk -s 0000:b1:00.2
  -> Kernel driver in use: nvme

nvme list / lsblk
  -> 出现一个 model/serial/容量与 SNAP namespace 对应的 /dev/nvmeXn1
```

只有在确认该设备对应 `SnapCtrl0 -> RemoteNvme0n1 -> node3 P5530` 后才能
运行写测试。Baseline 4 完成后，如需恢复 Baseline 2，再把同一个 BDF
精确切回 `uio_pci_generic`。

目前尚未编写 Baseline 3/4 的连接、驱动切换和 fio suite。实现时必须沿用
现有脚本的设备身份保护风格，禁止使用会影响所有 NVMe 的批量 reset/bind。

## 11. 已解决问题与排障经验

### 11.1 IB 显示 LinkUp 但 RDMA 不通

InfiniBand 的 `Physical state: LinkUp` 只表示物理链路正常。如果端口是
`Initializing`、Base LID 为 65535、SM LID 为 0，则通常缺少可用的
Subnet Manager。需要确认集群已有 OpenSM/UFM master；同一 fabric 不要
无规划地启动多个 master。曾在本机启动 OpenSM 后，DPU 和 node3 获得 LID
并恢复 IPoIB 通信。

### 11.2 SNAP 固件设置需要冷启动

`NVME_EMULATION_ENABLE=True` 写入 Next Boot 后，DPU-only reboot 不足以让
Host PCIe 枚举新 PF。此次使用整机 cold power cycle 后，Current/Next Boot
均为 True，Host 才看到 `0000:b1:00.2`。

### 11.3 `ctr: archive/tar: invalid tar header`

第一次导入 SNAP 镜像失败。镜像重新整理后校验和通过并成功导入
containerd。再次遇到时应先确认文件格式、校验和、架构 `linux/arm64` 和
containerd namespace，不要反复导入未知或损坏的归档。

### 11.4 RPC 返回 exit code 137

早期 backend/controller RPC 执行时，SNAP container 被终止或重启，命令
表现为 exit 137。排查时先查看 `crictl ps -a`、容器退出原因、hugepages、
内存和 container logs，再判断 RPC 本身是否有语法错误。

### 11.5 `No free I/O queue IDs`

根因是 node3 target 最初只允许 8 个 I/O qpairs，而 SNAP/DPU 上多个 reactor
需要更多 backend channels。最终配置为：

```text
node3 nvmf_create_transport: -q 128 -m 64
DPU bdev_nvme_attach_controller: --num-io-queues 16
```

修改顺序是先重启 node3 target，再重启 SNAP container 让 backend 重连。
仅重建 SNAP controller 不足以清理已经失败的 backend channels。

### 11.6 Host SPDK 的 IOVA 模式不匹配

初次探测出现：

```text
Expecting 'PA' IOVA mode but current mode is 'VA'
```

Baseline 2 已固定使用 `--iova-mode=pa`。Baseline 1 的网络 RDMA initiator
当前使用 `--iova-mode=va`；不要把两个路径的设置机械互换。

### 11.7 `lsblk` 看不到 SNAP 盘

Baseline 2 中这是预期现象：`0000:b1:00.2` 绑定 `uio_pci_generic` 后由
SPDK 直接消费，不会注册 Linux block device。只有 Baseline 4 将它绑定
`nvme` 内核驱动后，才应在 `lsblk` 中出现。

## 12. 文件索引

| 文件 | 用途 |
|---|---|
| `HANDOFF.md` | 本交接手册，后续实验应同步更新 |
| `README.md` | 快速启动入口 |
| `RESULTS.md` | 已完成结果与结论 |
| `baseline1.env` | 地址、BDF、NQN、序列号和 CPU mask |
| `node3_prepare.sh` | 安全检查并只绑定 node3 测试盘 |
| `node3_start_target.sh` | 启动 node3 SPDK NVMe-oF/RDMA target |
| `node3_stop_target.sh` | 停止 node3 target |
| `client_prepare.sh` | 准备 node4 Host 的 Baseline 1 环境 |
| `run_baseline1.sh` | 单项运行 Baseline 1 |
| `run_baseline2_suite.sh` | 运行 Baseline 2 五项矩阵 |
| `doca_snap_baseline2.yaml` | SNAP static pod 配置，明确 DPA provider |
| `baseline2_spdk_rpc_init.conf` | DPU 远端 SPDK backend 配置 |
| `baseline2_snap_rpc_init.conf` | SNAP subsystem/namespace/controller 配置 |
| `dpu_restart_baseline2_with_queues.sh` | 使用修正队列数重启并验证 SNAP |
| `dpu_collect_baseline2_diagnostics.sh` | 收集 DPU/SNAP 诊断信息 |
| `plot_baseline_comparison.py` | 从日志生成 Baseline 1/2 PNG 和 SVG |
| `results/` | 原始日志、summary 与对比图 |

## 13. 下一位接手者的推荐顺序

1. 阅读本手册的安全红线，不运行任何无 BDF 白名单的 SPDK setup 命令。
2. 恢复并验证 IB Subnet Manager、node3 target 和 DPU SNAP runtime。
3. 先重跑只读 smoke test，确认 Baseline 1/2 仍能访问同一 namespace。
4. 编写带身份保护的 Baseline 3 内核 NVMe-oF connect/disconnect 脚本。
5. 编写只切换 `0000:b1:00.2` 的 Baseline 4 驱动切换脚本。
6. 编写统一 fio suite，使 Baseline 3/4 只改变存储路径，不改变测试参数。
7. 首先完成所有只读 workload；写测试前再次确认盘身份和授权。
8. 补采 Host CPU、cycles/IO 与 DPU telemetry，而不是只比较 IOPS。
9. 完成重复测试和预处理后，再形成 SNAP 是否适合浪潮场景的最终结论。

## 14. 官方资料入口

- NVIDIA DOCA SNAP-4 Service Guide：
  <https://docs.nvidia.com/doca/sdk/doca-snap-4-service-guide/>
- NVIDIA DOCA STA：
  <https://docs.nvidia.com/doca/sdk/doca%2Bsta/index.html>
- NVIDIA DPF SNAP Block Storage 示例：
  <https://docs.nvidia.com/networking/display/dpf25100/ovn-kubernetes-with-host-based-networking-and-snap-block-storage>
