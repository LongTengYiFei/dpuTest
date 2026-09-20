# node3 `/dev/nvme1n2` 创建 Ceph BlueStore 的交接说明

更新日期：2026-09-08 UTC

## 给接手 Codex 的任务背景

用户准备在 node3 的 Intel P5530 上创建 Ceph BlueStore。该盘之前临时用于
SPDK/SNAP 对照测试，目前已经停止 SPDK NVMe-oF target，并恢复给 Linux
内核 `nvme` 驱动。恢复前它曾显示为 `/dev/nvme1n1`，恢复后显示为
`/dev/nvme1n2`。用户担心名称变化是否影响 BlueStore。

结论：**不要为了名字强行改回 `/dev/nvme1n1`。名称变化不影响 BlueStore，
但创建 OSD 时必须按稳定硬件身份选盘，不能硬编码临时内核名。**

本说明不授权创建、zap、格式化或覆盖设备。接手会话应先做只读检查，获得
用户对最终破坏性命令的明确授权后再执行。

## 当前已核验的设备身份

| 属性 | 当前值 |
|---|---|
| Host | `node3` |
| 临时内核块设备名 | `/dev/nvme1n2` |
| 内核 NVMe 控制器名 | `nvme5` |
| PCI BDF | `0000:65:00.0` |
| PCI ID | `8086:0b60` |
| 驱动 | `nvme` |
| 控制器状态 | `live` |
| 实际 NVMe NSID | `1` |
| Model | `INTEL SSDPF2KX019XZ` |
| Serial | `PHAO332302201P9SGN` |
| WWN/NGUID | `eui.01000000000000005cd2e4756c505651` |
| 容量 | 1.7 TiB（`3750748848` 个 512-byte sectors） |
| 只读标志 | `0` |
| 挂载 | 无 |
| holders | `0` |
| 当前可见文件系统签名 | `lsblk` 未显示 FSTYPE/UUID；仍需 root 下用 `wipefs -n` 复核 |
| SPDK `nvmf_tgt` | 无进程 |
| SPDK target PID 文件 | 已移除 |

node3 上其他 NVMe/Ceph 设备不在本任务范围，禁止批量 reset、unbind、zap 或
清盘。

## 为什么会从 `nvme1n1` 变成 `nvme1n2`

node3 启用了 Linux 原生 NVMe multipath：

```text
/sys/module/nvme_core/parameters/multipath = Y
```

重新绑定 PCI 控制器后，内核重新分配 controller、subsystem 和
namespace-head 实例号。本机当前实际关系为：

```text
PCI 0000:65:00.0
  -> controller nvme5
  -> multipath namespace-head /dev/nvme1n2
  -> /sys/class/block/nvme1n2/nsid = 1
```

因此名称末尾的 `n2` 是 Linux namespace-head 实例编号，不表示 SSD 的真实
NSID 已经变为 2，也不表示出现了第二块盘。真正 NSID 仍为 1。

这个名字以后再次解绑、重启或改变设备枚举顺序时仍可能变化。强行追求
`/dev/nvme1n1` 没有持久意义。

## 为什么不应强行改名

可能改变编号的手段包括再次解绑/绑定、重载 NVMe 驱动、修改全局 native
multipath 参数或重启。但这些方法：

- 不能保证最终一定得到 `/dev/nvme1n1`；
- 可能使它继续变为其他实例号；
- 可能影响 node3 上其他正在使用的 NVMe/Ceph 盘；
- 仅改变显示名称，不给 BlueStore 带来任何性能或可靠性收益。

不要创建名为 `/dev/nvme1n1` 的自定义 udev 软链接。该名称属于内核 NVMe
命名空间，未来可能与真实设备节点冲突。

## BlueStore 是否受影响

正常情况下不受影响。BlueStore/`ceph-volume lvm` 接受块设备路径；当使用
原始物理盘创建 OSD 时，`ceph-volume` 会建立 LVM VG/LV，并使用 LVM UUID、
OSD FSID 和 Ceph LVM tags 发现及激活 OSD。创建完成后，OSD 的持久身份不
依赖当时临时出现的 `/dev/nvme1n2` 字符串。

Ceph 官方资料：

- `ceph-volume lvm prepare`：
  <https://docs.ceph.com/en/latest/ceph-volume/lvm/prepare/>
- `ceph-volume` 命令说明：
  <https://docs.ceph.com/en/latest/man/8/ceph-volume/>
- BlueStore 配置参考：
  <https://docs.ceph.com/en/latest/rados/configuration/bluestore-config-ref/>

需要注意：本机的 `multipath=Y` 是 Linux NVMe native multipath；当前设备是
单个 PCIe path，不是 `/dev/mapper/*` 的 dm-multipath 设备。仍应由接手会话
使用实际 Ceph 版本的 `ceph-volume inventory` 确认它被判定为 available，
不能只依据本说明推断。

## 创建 OSD 时应使用的稳定路径

首选 namespace WWN/EUI 链接：

```text
/dev/disk/by-id/nvme-eui.01000000000000005cd2e4756c505651
```

也可以使用 model + serial：

```text
/dev/disk/by-id/nvme-INTEL_SSDPF2KX019XZ_PHAO332302201P9SGN
```

按固定 PCI 槽位识别时可使用：

```text
/dev/disk/by-path/pci-0000:65:00.0-nvme-1
```

以上三个链接当前都解析到 `/dev/nvme1n2`。若希望设备随物理盘移动仍保持
身份，优先使用 by-id/EUI；若业务意图是永远选择这个 PCI 槽位，则使用
by-path。不要在自动化中直接写 `/dev/nvme1n2`。

## 接手会话必须先做的只读检查

以下命令用于确认环境，不会清盘：

```bash
hostname -s
readlink -f /dev/disk/by-id/nvme-eui.01000000000000005cd2e4756c505651
lspci -nnk -s 0000:65:00.0
lsblk -o NAME,PATH,SIZE,MODEL,SERIAL,WWN,FSTYPE,UUID,MOUNTPOINTS
cat /sys/class/block/nvme1n2/nsid
cat /sys/class/nvme/nvme5/state
findmnt -rn -S /dev/nvme1n2
find /sys/class/block/nvme1n2/holders -mindepth 1 -maxdepth 1 -print
sudo wipefs -n /dev/disk/by-id/nvme-eui.01000000000000005cd2e4756c505651
sudo pvs --all
sudo vgs
sudo lvs -a -o+devices
sudo ceph-volume inventory \
  /dev/disk/by-id/nvme-eui.01000000000000005cd2e4756c505651 \
  --format json-pretty
sudo ceph-volume lvm list
sudo ceph-volume raw list
```

交接检查时，node3 的普通 shell 中找不到 `ceph` 和 `ceph-volume`。接手会话
需要先确认该集群通过 RPM/DEB、cephadm container 还是其他方式执行 Ceph
工具，并记录 Ceph 版本；不要未经确认就安装与集群版本不匹配的软件包。

如果 `wipefs -n`、LVM 或 `ceph-volume inventory` 显示已有签名、PV/VG/LV、
BlueStore label 或 OSD 归属，必须停止并向用户报告，不得自动 zap。

## 历史操作及数据状态

该盘曾被绑定到 `uio_pci_generic`，由 node3 SPDK v26.05 作为
NVMe-oF/RDMA target 导出，用于以下 30 秒测试：

- 4 KiB 随机读 QD1/QD64；
- 4 KiB 随机写 QD64；
- 128 KiB 顺序读/写 QD32。

因此盘上部分区域已经被性能测试写过，不应假设保留原有有效业务数据。
恢复脚本本身没有执行格式化、wipe、namespace create/delete 或数据擦除。

## 明确禁止事项

- 不要为了恢复名字而重载全局 `nvme`/`nvme_core`，也不要修改全局
  `nvme_core.multipath`。
- 不要无白名单运行 SPDK `setup.sh reset` 或批量绑定脚本。
- 不要操作 node3 的其他 NVMe/Ceph 盘。
- 不要仅凭 `/dev/nvme1n2` 名称执行 `zap`、`dd`、`mkfs` 或 OSD create。
- 不要重启旧的 `/home/cyf/dpuTest/SNAP/node3_start_target.sh`；这会再次把
  本盘作为远端 target 使用。
- 未获得本次 BlueStore 创建的明确授权前，不执行任何破坏性命令。

## 建议接手动作

1. 确认用户希望采用 cephadm DriveGroup、`ceph-volume lvm` 还是
   `ceph-volume raw`，以及是否需要独立 `block.db`/`block.wal`。
2. 确认集群版本、FSID、目标 OSD 规划和现有 CRUSH/device class 策略。
3. 执行上述只读检查并把完整设备身份展示给用户。
4. 使用 by-id/EUI 路径形成待执行命令或 cephadm spec。
5. 明确说明命令会覆盖该盘，并在用户授权后执行。
6. 创建完成后核对 OSD FSID、LVM tags/BlueStore label、systemd/cephadm
   激活状态和集群健康状态。

