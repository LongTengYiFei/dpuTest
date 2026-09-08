# SPDK and SNAP storage baselines

完整环境、当前状态、已知问题及 Baseline 1～4 实验矩阵见
[`HANDOFF.md`](HANDOFF.md)。

This directory contains the direct SPDK NVMe-oF/RDMA baseline:

```text
node3 /dev/nvme1n1 -> SPDK NVMe-oF/RDMA target -> local SPDK initiator
```

SPDK is pinned to v26.05. The target disk is guarded by both PCI address
`0000:65:00.0` and serial `PHAO332302201P9SGN`. The preparation script binds
only that device; it must never bind the other Ceph NVMe devices on node3.

The first successful measurement set is summarized in `RESULTS.md`; complete
tool output is retained under `results/`.

Baseline 2 adds BlueField-3 SNAP in DPA mode and consumes its emulated PCIe
NVMe function from Host SPDK:

```text
node3 SPDK target -> NVMe-oF/RDMA -> DPU SNAP/DPA -> emulated PCIe NVMe -> Host SPDK
```

## One-time preparation

On the local client:

```bash
cd /home/cyf/dpuTest/SNAP
sudo ./client_prepare.sh
```

On node3:

```bash
cd /home/cyf/dpuTest/SNAP
sudo ./node3_prepare.sh
sudo ./node3_start_target.sh
```

`node3_prepare.sh` removes `/dev/nvme1n1` from the Linux NVMe driver and binds
PCI device `0000:65:00.0` to a userspace driver. No filesystem is created.

## Run a read-only smoke benchmark

```bash
./run_baseline1.sh randread 4096 64 30
```

Write workloads overwrite the exported disk and require an explicit guard:

```bash
ALLOW_DESTRUCTIVE_WRITE=YES ./run_baseline1.sh randwrite 4096 64 60
```

Results are saved under `results/`.

## Run baseline 2

After the DPU SNAP service and its backend/controller configuration are active:

```bash
sudo ./run_baseline2_suite.sh
```

The suite binds only SNAP PF `0000:b1:00.2` to `uio_pci_generic` and runs the
same five 30-second workloads used in the initial comparison. It includes write
tests and therefore overwrites portions of the exported test disk.

## Stop

On node3:

```bash
sudo ./node3_stop_target.sh
```

To return the NVMe device to the Linux kernel later, use SPDK's targeted reset
procedure and verify the PCI address before proceeding.
