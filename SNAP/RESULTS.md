# SPDK and SNAP baseline results

Date: 2026-09-08 UTC

Path under test:

```text
local x86 SPDK v26.05 NVMe-oF initiator
  -> 200 Gb/s InfiniBand / NVMe-oF RDMA
  -> node3 x86 SPDK v26.05 NVMe-oF target
  -> Intel P5530 1.7 TB (PCI 0000:65:00.0)
```

Both initiator and target used CPU 24 (NUMA node 1), one polling core and one
I/O queue. The node3 BlueField port is on NUMA node 1 while the target NVMe is
on NUMA node 0, so the target path crosses sockets.

## Initial 30-second results

| Workload | QD | IOPS | Throughput | Avg latency | P50 | P99 |
|---|---:|---:|---:|---:|---:|---:|
| 4 KiB random read | 1 | 84,246 | 329 MiB/s | 11.85 us | 10.81 us | 59.38 us |
| 4 KiB random read | 64 | 415,103 | 1,621 MiB/s | 154.16 us | 154.45 us | 202.35 us |
| 4 KiB random write | 64 | 704,012 | 2,750 MiB/s | 90.89 us | 58.41 us | 381.23 us |
| 128 KiB sequential read | 32 | 54,828 | 6,853 MiB/s | 583.62 us | 633.43 us | 1,032.26 us |
| 128 KiB sequential write | 32 | 26,035 | 3,254 MiB/s | 1,229.15 us | 1,227.76 us | 3,128.06 us |

Raw logs are in `results/`.

These are connectivity and initial baseline measurements, not qualified
steady-state SSD results. The SSD was not preconditioned, each workload was run
once, and CPU frequency was not pinned. Write results can therefore include
empty-drive/cache effects. Use identical settings for the SNAP comparisons,
then add repetitions and SSD preconditioning for a publishable comparison.

# Baseline 2 results

Date: 2026-09-08 UTC

Path under test:

```text
node4 x86 SPDK v26.05 PCIe NVMe initiator (core 24)
  -> BlueField-3 emulated PCIe NVMe PF 0000:b1:00.2
  -> DOCA SNAP 4.4.0 NVME_EMU_PROVIDER=dpa
  -> DPU SPDK v24.01 NVMe-oF/RDMA initiator (16 I/O queues)
  -> 200 Gb/s InfiniBand
  -> node3 x86 SPDK v26.05 NVMe-oF target
  -> Intel P5530 1.7 TB (PCI 0000:65:00.0)
```

The Host bound only NVIDIA device `0000:b1:00.2` (`15b3:6001`) to
`uio_pci_generic` and used PA IOVA. The four local Intel NVMe devices were not
rebound or accessed. SNAP reported the backend transport as `rdma_zc`, the
emulated controller exposed 31 I/O queues, and the SNAP startup log confirmed
the DPA provider. The node3 RDMA transport allowed 64 I/O qpairs per controller.

## Initial 30-second results

| Workload | QD | IOPS | Throughput | Avg latency | P50 | P99 |
|---|---:|---:|---:|---:|---:|---:|
| 4 KiB random read | 1 | 35,614 | 139 MiB/s | 28.06 us | 21.87 us | 81.13 us |
| 4 KiB random read | 64 | 390,483 | 1,525 MiB/s | 163.88 us | 154.45 us | 306.94 us |
| 4 KiB random write | 64 | 666,136 | 2,602 MiB/s | 96.06 us | 89.44 us | 244.38 us |
| 128 KiB sequential read | 32 | 53,484 | 6,685 MiB/s | 598.30 us | 594.33 us | 969.70 us |
| 128 KiB sequential write | 32 | 25,799 | 3,225 MiB/s | 1,240.38 us | 1,165.20 us | 3,143.70 us |

## Baseline 2 relative to direct SPDK baseline 1

![Baseline 1 versus Baseline 2](results/baseline1-vs-baseline2.png)

The chart can be regenerated from the raw logs with
`python3 plot_baseline_comparison.py`. An SVG version is generated alongside
the PNG for lossless use in documents and presentations.

Negative IOPS values mean lower throughput. Positive latency values mean
higher latency.

| Workload | IOPS change | Avg latency change | P50 change | P99 change |
|---|---:|---:|---:|---:|
| 4 KiB random read QD1 | -57.73% | +136.79% | +102.26% | +36.63% |
| 4 KiB random read QD64 | -5.93% | +6.31% | 0.00% | +51.69% |
| 4 KiB random write QD64 | -5.38% | +5.69% | +53.14% | -35.90% |
| 128 KiB sequential read QD32 | -2.45% | +2.52% | -6.17% | -6.06% |
| 128 KiB sequential write QD32 | -0.91% | +0.91% | -5.10% | +0.50% |

The main cost of the SNAP path in this run is low-queue-depth latency: 4 KiB
QD1 average latency rose from 11.85 us to 28.06 us (an absolute increase of
16.21 us), reducing IOPS by 57.73%. With enough outstanding I/O, the throughput
gap narrowed to 0.91%-5.93% for the other four workloads. Tail-latency changes
are mixed and should not be treated as conclusive from one 30-second run.

This test also directly disproves the assumption that a SNAP-emulated local
NVMe disk can only be consumed through the Linux kernel/POSIX path: Host SPDK
successfully claimed the emulated PCIe function and issued user-space I/O.

As with baseline 1, these are initial one-shot measurements without SSD
preconditioning, CPU-frequency pinning, repetitions, or confidence intervals.
Raw baseline 2 logs use the timestamp `20260908T085552Z` under `results/`.
