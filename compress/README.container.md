# DOCA Decompression Container Test

This is a small Docker wrapper for the DOCA deflate decompression test in this
directory. The image packages the DOCA user-space SDK/runtime from the build
host's `/opt/mellanox/doca`.

The first goal is a yes/no test: can a container see the BlueField/DOCA device
well enough for `doca_compress_create()` and the decompression task path to run?

## Build

From the repository root:

```bash
mkdir -p compress/doca-bundle
tar -C /opt/mellanox -czf compress/doca-bundle/doca.tar.gz doca
```

```bash
docker build --pull=false -f compress/Dockerfile -t doca-compress-test .
```

The build host must have DOCA installed at `/opt/mellanox/doca`, because the
Dockerfile copies `compress/doca-bundle/doca.tar.gz` into the image and extracts
it to `/opt/mellanox/doca`. The runtime host still needs BlueField/DPU kernel
drivers and device nodes; a container image cannot package host kernel drivers.
The `compress/doca-bundle/` directory is intentionally git-ignored because it is
large and machine-specific.

If Docker cannot reach the network to install Ubuntu packages, build this on a
machine with access to the Ubuntu package repositories or change the base image
to an internal image that already has `gcc`, `make`, `zlib1g-dev`, `pciutils`,
and `rdma-core`.

## Probe Device Visibility

Start with the permissive version. This is intentionally broad because the test
is meant to answer whether containerization blocks access to the DPU.

```bash
docker run --rm -it \
  --privileged \
  --network host \
  -v /dev:/dev \
  -v /sys:/sys:ro \
  -v /lib/modules:/lib/modules:ro \
  doca-compress-test probe
```

Expected signs of life:

- `/opt/mellanox/doca` is visible inside the container from the bundled image contents.
- `/dev/infiniband` and/or `/dev/vfio` are visible if the host uses them.
- `lspci` shows a Mellanox/NVIDIA/BlueField device.
- `make doca_test` succeeds.
- `ldd ./doca_compress_test` resolves `libdoca_compress`, `libdoca_common`,
  and `libdoca_argp`.

## Run The Existing Workload Test

Replace the host workload path with your dataset path:

```bash
docker run --rm -it \
  --privileged \
  --network host \
  -v /dev:/dev \
  -v /sys:/sys:ro \
  -v /lib/modules:/lib/modules:ro \
  -v /home/cyf/datasetsSmartCE_sample:/data/workloads:ro \
  doca-compress-test /data/workloads dpuBatch 32 64
```

The program still has the PCI address hardcoded as `b1:00.0` in
`compress_deflate_main.c`. If the container can see the device but DOCA reports
that it cannot open the PCI address, confirm the address with:

```bash
lspci -nn | grep -Ei 'mellanox|nvidia|bluefield'
```

## Tightening Later

If the privileged run works, the next step is to remove broad privileges and
mount only the required devices. For a Ceph deployment, this probably becomes a
Kubernetes device-plugin or SR-IOV style configuration rather than ordinary
Docker flags.
