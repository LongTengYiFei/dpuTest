/home/cyf/ceph-dpu/build/bin/ceph osd pool delete po1 po1 --yes-i-really-really-mean-it
/home/cyf/ceph-dpu/build/bin/ceph osd pool delete po2 po2 --yes-i-really-really-mean-it

/home/cyf/ceph-dpu/build/bin/ceph osd pool create po1 32
/home/cyf/ceph-dpu/build/bin/ceph osd pool set po1 compression_algorithm zlib
/home/cyf/ceph-dpu/build/bin/ceph osd pool set po1 compression_mode aggressive

/home/cyf/ceph-dpu/build/bin/ceph osd pool create po2 32