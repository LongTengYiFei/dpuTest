# Simplified RADOS CLI baselines

These scripts use the `rados` command-line tool for all Ceph object I/O. The
test programs do not call the librados API directly. The `rados` executable
itself is a standard Ceph client and internally uses librados.

Both paths recursively concatenate sorted `*.log` files and split the stream
into objects of at most 4 MiB. Pools are created when missing and configured
with `size=3` and `min_size=2`.

## Raw baseline

```bash
cd /home/cyf/dpuTest/attach/test
DATASET=/path/to/logs ./run_rados_cli_raw.sh
```

Data path:

```text
4 MiB raw file -> rados put -> Ceph -> rados get -> local raw file
```

## DOCA baseline

```bash
cd /home/cyf/dpuTest/attach/test
DATASET=/path/to/logs PCI_ADDR=b1:00.0 ./run_rados_cli_doca.sh
```

Data path:

```text
4 MiB raw file -> CPU zlib compression -> rados put -> Ceph
-> rados get -> local compressed file -> DOCA decompression -> local raw file
```

Each compressed 4 MiB object contains up to four independent 1 MiB zlib frames
because the tested DOCA device reports a 2 MiB maximum buffer size. The Ceph
object size remains one object per original 4 MiB chunk.

The default is one measured round. Set `ROUNDS` to repeat reads:

```bash
ROUNDS=5 DATASET=/path/to/logs ./run_rados_cli_raw.sh
ROUNDS=5 DATASET=/path/to/logs PCI_ADDR=b1:00.0 ./run_rados_cli_doca.sh
```

Every run uses a new local directory under `simple-results/`. Downloaded and
decompressed files are retained there. Override `RUN_DIR` to choose another
location. Other optional variables include `POOL`, `PREFIX`, `CEPH_ROOT`,
`CEPH_CLI`, `RADOS_CLI`, `CEPH_CONF`, `CEPH_CLUSTER`, and `CEPH_ID`.

Throughput is original uncompressed bytes divided by client wall-clock time.
Raw time covers all `rados get` commands. DOCA time covers all `rados get`
commands plus local DOCA initialization, decompression, verification, and output
file writes. CPU compression and `rados put` are reported separately and are not
included in read throughput.
