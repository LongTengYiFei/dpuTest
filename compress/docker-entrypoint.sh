#!/usr/bin/env bash
set -euo pipefail

cd /workspace/compress

print_probe() {
    echo "== container =="
    uname -a
    echo

    echo "== doca installation =="
    if [ -d "${DOCA_HOME:-/opt/mellanox/doca}" ]; then
        echo "found ${DOCA_HOME:-/opt/mellanox/doca}"
        find "${DOCA_HOME:-/opt/mellanox/doca}/lib" -maxdepth 2 -name 'libdoca_compress.so*' -print 2>/dev/null || true
    else
        echo "missing ${DOCA_HOME:-/opt/mellanox/doca}"
    fi
    echo

    echo "== visible device nodes =="
    ls -l /dev/infiniband 2>/dev/null || echo "missing /dev/infiniband"
    ls -l /dev/vfio 2>/dev/null || echo "missing /dev/vfio"
    echo

    echo "== pci devices containing Mellanox/NVIDIA =="
    lspci -nn 2>/dev/null | grep -Ei 'mellanox|nvidia|bluefield' || true
    echo
}

build_binary() {
    if [ ! -d "${DOCA_HOME:-/opt/mellanox/doca}" ]; then
        echo "ERROR: ${DOCA_HOME:-/opt/mellanox/doca} was not found in the container." >&2
        echo "Build the image on a host with /opt/mellanox/doca available, or mount it at runtime." >&2
        exit 2
    fi

    make doca_test
    echo
    echo "== linked libraries =="
    ldd ./doca_compress_test || true
}

case "${1:-probe}" in
    probe)
        print_probe
        build_binary
        echo
        echo "Probe complete. To run the benchmark, pass: <workloads_path> dpuBatch <batch_size> <blob_size_KB>"
        ;;
    bash|sh)
        exec "$@"
        ;;
    *)
        build_binary
        exec ./doca_compress_test "$@"
        ;;
esac
