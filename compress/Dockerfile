FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV DOCA_HOME=/opt/mellanox/doca
ENV LD_LIBRARY_PATH=/opt/mellanox/doca/lib/x86_64-linux-gnu:/opt/mellanox/dpdk/lib/x86_64-linux-gnu

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        ibverbs-providers \
        kmod \
        libibverbs-dev \
        make \
        pciutils \
        rdma-core \
        zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/compress
COPY compress/ /workspace/compress/
COPY compress/docker-entrypoint.sh /usr/local/bin/doca-compress-entrypoint

RUN chmod +x /usr/local/bin/doca-compress-entrypoint

ENTRYPOINT ["/usr/local/bin/doca-compress-entrypoint"]
CMD ["probe"]
