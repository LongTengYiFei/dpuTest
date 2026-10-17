# Ceph direct-read / DOCA-decompress read baseline

This benchmark performs the following data path:

1. Recursively read sorted `*.log` files from the input directory and split the
   byte stream into 4 MiB chunks.
2. Write one copy as plain, uncompressed 4 MiB RADOS objects. This is the direct
   read baseline and performs no decompression after `rados_read()` returns.
3. Split the compressed copy of each 4 MiB object into up to four independent
   1 MiB zlib frames, compress them on the CPU at zlib level 6, and write the
   frames as one RADOS object. The internal framing is needed because this DOCA
   device reports a 2 MiB maximum compression buffer size; the Ceph object size
   remains 4 MiB.
4. Warm up both read paths once, then alternate measured direct-read and DOCA
   rounds to reduce ordering bias.
5. For direct reads, measure from immediately before `rados_read()` until it
   returns. For DOCA reads, measure from immediately before `rados_read()` until
   DOCA decompression and its required buffer copies complete.
6. Check every plain object's expected length and verify every DOCA-decompressed
   object using its original length and CRC32.

Run the complete benchmark:

```bash
cd /home/cyf/dpuTest/attach/test
./run_ceph_doca_baseline.sh
```

Run only the uncompressed write/direct-read baseline in its own replicated pool:

```bash
cd /home/cyf/dpuTest/attach/test
./run_ceph_raw_baseline.sh
```

The raw-only script defaults to pool `ceph-raw-baseline`. It does not write
compressed objects, initialize DOCA, or perform any decompression.

Defaults can be overridden with environment variables:

```bash
POOL=doca-zlib-baseline \
DATASET=/home/cyf/raid0/LogHub/Android_v1 \
PREFIX=android-v1-4m \
RAW_PREFIX=android-v1-4m-raw \
ROUNDS=5 \
PCI_ADDR=b1:00.0 \
./run_ceph_doca_baseline.sh
```

The primary metric is `throughput_mib_s`, calculated as original bytes divided
by the sum of client-observed object latency. The `SUMMARY` line reports aggregate
plain direct-read and DOCA throughput. A positive `doca_slower_pct` is how much
slower DOCA is than direct reads; a negative value means DOCA was faster.

