CC := gcc
CXX := g++

CEPH_ROOT := /home/cyf/githubDownload/ceph
DOCA_ROOT := /opt/mellanox/doca

CPPFLAGS := \
	-I$(CEPH_ROOT)/src/include \
	-I$(DOCA_ROOT)/include \
	-I$(DOCA_ROOT)/samples \
	-I$(DOCA_ROOT)/applications/common \
	-I../../compress

CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Wno-deprecated-declarations
CXXFLAGS := -O2 -g -std=gnu++17 -Wall -Wextra -Wno-deprecated-declarations
LDFLAGS := \
	-L$(CEPH_ROOT)/build/lib \
	-L$(DOCA_ROOT)/lib/x86_64-linux-gnu \
	-Wl,-rpath,$(CEPH_ROOT)/build/lib \
	-Wl,-rpath,$(DOCA_ROOT)/lib/x86_64-linux-gnu

LDLIBS := -lrados -ldoca_compress -ldoca_common -ldoca_argp -lz -lm -lstdc++

OBJECTS := ceph_doca_read_bench.o compress_common.o doca_common.o doca_utils.o

.PHONY: all clean

all: ceph_doca_read_bench

doca_local_decompress: doca_local_decompress.o compress_common.o doca_common.o doca_utils.o
	$(CXX) $^ $(LDFLAGS) -ldoca_compress -ldoca_common -ldoca_argp -lz -lm -o $@

ceph_doca_read_bench: $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

ceph_doca_read_bench.o: ceph_doca_read_bench.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

doca_local_decompress.o: doca_local_decompress.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

compress_common.o: ../../compress/compress_common.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

doca_common.o: ../../compress/doca_common.cc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

doca_utils.o: $(DOCA_ROOT)/applications/common/utils.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f ceph_doca_read_bench doca_local_decompress doca_local_decompress.o $(OBJECTS)

