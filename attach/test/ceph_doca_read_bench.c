#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <rados/librados.h>
#include <zlib.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_compress.h>
#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "doca_common.h"
#include "compress_common.h"

#define OBJECT_BYTES (4U * 1024U * 1024U)
#define FRAME_BYTES (1U * 1024U * 1024U)
#define MAX_FRAMES (OBJECT_BYTES / FRAME_BYTES)
#define OBJECT_MAGIC UINT32_C(0x4344424f) /* CDBO */
#define MANIFEST_MAGIC UINT32_C(0x4344424d) /* CDBM */
#define FORMAT_VERSION 2U
#define DEFAULT_PREFIX "android-v1-4m"
#define DEFAULT_RAW_PREFIX "android-v1-4m-raw"
#define DEFAULT_PCI_ADDR "b1:00.0"
#define MAX_PREFIX 64

struct object_header {
	uint32_t magic;
	uint32_t version;
	uint64_t raw_size;
	uint64_t compressed_size;
	uint32_t raw_crc32;
	uint32_t frame_count;
} __attribute__((packed));

struct frame_header {
	uint32_t raw_size;
	uint32_t compressed_size;
} __attribute__((packed));

struct manifest {
	uint32_t magic;
	uint32_t version;
	uint64_t object_size;
	uint64_t object_count;
	uint64_t total_raw_size;
	uint64_t total_compressed_size;
	char prefix[MAX_PREFIX];
} __attribute__((packed));

struct file_list {
	char **paths;
	size_t count;
	size_t capacity;
};

struct method_result {
	uint64_t raw_bytes;
	uint64_t compressed_bytes;
	uint64_t objects;
	uint64_t total_ns;
	uint64_t read_ns;
	uint64_t decompress_ns;
};

struct compress_deflate_result {
	doca_error_t status;
	uint32_t crc_cs;
	uint32_t adler_cs;
};

struct doca_decompressor {
	struct compress_resources resources;
	struct program_core_objects *state;
	struct doca_buf *src_buf;
	struct doca_buf *dst_buf;
	struct doca_compress_task_decompress_deflate *decompress_task;
	struct doca_task *task;
	struct compress_deflate_result task_result;
	uint8_t *src_memory;
	uint8_t *dst_memory;
	size_t src_capacity;
	size_t dst_capacity;
};

static struct file_list *active_file_list;

static uint64_t now_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static bool has_log_suffix(const char *path)
{
	size_t len = strlen(path);
	return len >= 4 && strcmp(path + len - 4, ".log") == 0;
}

static int collect_log_file(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	(void)ftw;
	if (type != FTW_F || !has_log_suffix(path))
		return 0;

	if (active_file_list->count == active_file_list->capacity) {
		size_t new_capacity = active_file_list->capacity == 0 ? 16 : active_file_list->capacity * 2;
		char **new_paths = realloc(active_file_list->paths, new_capacity * sizeof(*new_paths));
		if (new_paths == NULL)
			return -1;
		active_file_list->paths = new_paths;
		active_file_list->capacity = new_capacity;
	}
	active_file_list->paths[active_file_list->count] = strdup(path);
	if (active_file_list->paths[active_file_list->count] == NULL)
		return -1;
	active_file_list->count++;
	return 0;
}

static int compare_paths(const void *left, const void *right)
{
	const char *const *a = left;
	const char *const *b = right;
	return strcmp(*a, *b);
}

static int build_file_list(const char *root, struct file_list *files)
{
	memset(files, 0, sizeof(*files));
	active_file_list = files;
	if (nftw(root, collect_log_file, 32, FTW_PHYS) != 0) {
		fprintf(stderr, "Failed to scan %s: %s\n", root, strerror(errno));
		return -1;
	}
	if (files->count == 0) {
		fprintf(stderr, "No .log files found under %s\n", root);
		return -1;
	}
	qsort(files->paths, files->count, sizeof(*files->paths), compare_paths);
	return 0;
}

static void free_file_list(struct file_list *files)
{
	for (size_t i = 0; i < files->count; ++i)
		free(files->paths[i]);
	free(files->paths);
}

static int connect_rados(const char *pool_name, rados_t *cluster, rados_ioctx_t *ioctx)
{
	int rc = rados_create2(cluster, "ceph", "client.admin", 0);
	if (rc < 0) {
		fprintf(stderr, "rados_create2: %s\n", strerror(-rc));
		return -1;
	}
	rc = rados_conf_read_file(*cluster, "/etc/ceph/ceph.conf");
	if (rc < 0) {
		fprintf(stderr, "rados_conf_read_file: %s\n", strerror(-rc));
		goto fail;
	}
	rc = rados_connect(*cluster);
	if (rc < 0) {
		fprintf(stderr, "rados_connect: %s\n", strerror(-rc));
		goto fail;
	}
	rc = rados_ioctx_create(*cluster, pool_name, ioctx);
	if (rc < 0) {
		fprintf(stderr, "rados_ioctx_create(%s): %s\n", pool_name, strerror(-rc));
		goto fail;
	}
	return 0;

fail:
	rados_shutdown(*cluster);
	return -1;
}

static void close_rados(rados_t cluster, rados_ioctx_t ioctx)
{
	rados_ioctx_destroy(ioctx);
	rados_shutdown(cluster);
}

static void object_name(char *buffer, size_t size, const char *prefix, uint64_t index)
{
	snprintf(buffer, size, "%s.%06" PRIu64, prefix, index);
}

static void manifest_name(char *buffer, size_t size, const char *prefix)
{
	snprintf(buffer, size, "%s.manifest", prefix);
}

static int write_chunk(rados_ioctx_t ioctx,
		       const char *prefix,
		       uint64_t index,
		       const uint8_t *raw,
		       size_t raw_size,
		       uint8_t *object_buffer,
		       size_t object_capacity,
		       uint64_t *compressed_total)
{
	struct object_header header = {
		.magic = OBJECT_MAGIC,
		.version = FORMAT_VERSION,
		.raw_size = raw_size,
		.compressed_size = 0,
		.raw_crc32 = (uint32_t)crc32(0L, raw, raw_size),
		.frame_count = 0,
	};
	size_t raw_offset = 0;
	size_t object_offset = sizeof(header);
	while (raw_offset < raw_size) {
		size_t frame_raw_size = raw_size - raw_offset;
		if (frame_raw_size > FRAME_BYTES)
			frame_raw_size = FRAME_BYTES;
		if (header.frame_count >= MAX_FRAMES || object_offset + sizeof(struct frame_header) >= object_capacity)
			return -1;

		struct frame_header frame = {
			.raw_size = (uint32_t)frame_raw_size,
			.compressed_size = 0,
		};
		size_t frame_header_offset = object_offset;
		object_offset += sizeof(frame);
		uLongf frame_compressed_size = object_capacity - object_offset;
		int zrc = compress2(object_buffer + object_offset, &frame_compressed_size,
				    raw + raw_offset, frame_raw_size, Z_DEFAULT_COMPRESSION);
		if (zrc != Z_OK) {
			fprintf(stderr, "zlib compress2 failed for object %" PRIu64 " frame %u: %d\n",
				index, header.frame_count, zrc);
			return -1;
		}
		frame.compressed_size = (uint32_t)frame_compressed_size;
		memcpy(object_buffer + frame_header_offset, &frame, sizeof(frame));
		object_offset += frame_compressed_size;
		raw_offset += frame_raw_size;
		header.compressed_size += frame_compressed_size;
		header.frame_count++;
	}
	memcpy(object_buffer, &header, sizeof(header));

	char oid[128];
	object_name(oid, sizeof(oid), prefix, index);
	int rc = rados_write_full(ioctx, oid, (const char *)object_buffer, object_offset);
	if (rc < 0) {
		fprintf(stderr, "rados_write_full(%s): %s\n", oid, strerror(-rc));
		return -1;
	}
	*compressed_total += header.compressed_size;
	return 0;
}

static int write_compressed_dataset(const char *pool_name, const char *dataset_root, const char *prefix)
{
	struct file_list files;
	if (build_file_list(dataset_root, &files) != 0)
		return EXIT_FAILURE;

	rados_t cluster;
	rados_ioctx_t ioctx;
	if (connect_rados(pool_name, &cluster, &ioctx) != 0) {
		free_file_list(&files);
		return EXIT_FAILURE;
	}

	uint8_t *raw = malloc(OBJECT_BYTES);
	size_t object_capacity = sizeof(struct object_header) +
		MAX_FRAMES * (sizeof(struct frame_header) + compressBound(FRAME_BYTES));
	uint8_t *object_buffer = malloc(object_capacity);
	if (raw == NULL || object_buffer == NULL) {
		fprintf(stderr, "Memory allocation failed\n");
		close_rados(cluster, ioctx);
		free_file_list(&files);
		free(raw);
		free(object_buffer);
		return EXIT_FAILURE;
	}

	uint64_t start = now_ns();
	uint64_t object_count = 0;
	uint64_t total_raw = 0;
	uint64_t total_compressed = 0;
	size_t fill = 0;
	int status = EXIT_SUCCESS;

	for (size_t file_index = 0; file_index < files.count && status == EXIT_SUCCESS; ++file_index) {
		int fd = open(files.paths[file_index], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "open(%s): %s\n", files.paths[file_index], strerror(errno));
			status = EXIT_FAILURE;
			break;
		}
		for (;;) {
			ssize_t count = read(fd, raw + fill, OBJECT_BYTES - fill);
			if (count < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "read(%s): %s\n", files.paths[file_index], strerror(errno));
				status = EXIT_FAILURE;
				break;
			}
			if (count == 0)
				break;
			fill += (size_t)count;
			total_raw += (uint64_t)count;
			if (fill == OBJECT_BYTES) {
				if (write_chunk(ioctx, prefix, object_count, raw, fill, object_buffer,
						object_capacity, &total_compressed) != 0) {
					status = EXIT_FAILURE;
					break;
				}
				object_count++;
				fill = 0;
			}
		}
		close(fd);
	}

	if (status == EXIT_SUCCESS && fill != 0) {
		if (write_chunk(ioctx, prefix, object_count, raw, fill, object_buffer,
				object_capacity, &total_compressed) != 0) {
			status = EXIT_FAILURE;
		} else {
			object_count++;
		}
	}

	if (status == EXIT_SUCCESS) {
		struct manifest manifest = {
			.magic = MANIFEST_MAGIC,
			.version = FORMAT_VERSION,
			.object_size = OBJECT_BYTES,
			.object_count = object_count,
			.total_raw_size = total_raw,
			.total_compressed_size = total_compressed,
		};
		snprintf(manifest.prefix, sizeof(manifest.prefix), "%s", prefix);
		char oid[128];
		manifest_name(oid, sizeof(oid), prefix);
		int rc = rados_write_full(ioctx, oid, (const char *)&manifest, sizeof(manifest));
		if (rc < 0) {
			fprintf(stderr, "rados_write_full(%s): %s\n", oid, strerror(-rc));
			status = EXIT_FAILURE;
		} else {
			uint64_t elapsed = now_ns() - start;
			printf("WRITE objects=%" PRIu64 " raw_bytes=%" PRIu64
			       " compressed_bytes=%" PRIu64 " ratio=%.4f elapsed_ms=%.3f raw_mib_s=%.2f\n",
			       object_count, total_raw, total_compressed,
			       (double)total_compressed / (double)total_raw,
			       elapsed / 1e6,
			       (double)total_raw / 1048576.0 / ((double)elapsed / 1e9));
		}
	}

	free(raw);
	free(object_buffer);
	close_rados(cluster, ioctx);
	free_file_list(&files);
	return status;
}

static int write_raw_dataset(const char *pool_name, const char *dataset_root, const char *prefix)
{
	struct file_list files;
	if (build_file_list(dataset_root, &files) != 0)
		return EXIT_FAILURE;

	rados_t cluster;
	rados_ioctx_t ioctx;
	if (connect_rados(pool_name, &cluster, &ioctx) != 0) {
		free_file_list(&files);
		return EXIT_FAILURE;
	}

	uint8_t *raw = malloc(OBJECT_BYTES);
	if (raw == NULL) {
		fprintf(stderr, "Memory allocation failed\n");
		close_rados(cluster, ioctx);
		free_file_list(&files);
		return EXIT_FAILURE;
	}

	uint64_t start = now_ns();
	uint64_t object_count = 0;
	uint64_t total_raw = 0;
	size_t fill = 0;
	int status = EXIT_SUCCESS;

	for (size_t file_index = 0; file_index < files.count && status == EXIT_SUCCESS; ++file_index) {
		int fd = open(files.paths[file_index], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "open(%s): %s\n", files.paths[file_index], strerror(errno));
			status = EXIT_FAILURE;
			break;
		}
		for (;;) {
			ssize_t count = read(fd, raw + fill, OBJECT_BYTES - fill);
			if (count < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "read(%s): %s\n", files.paths[file_index], strerror(errno));
				status = EXIT_FAILURE;
				break;
			}
			if (count == 0)
				break;
			fill += (size_t)count;
			total_raw += (uint64_t)count;
			if (fill == OBJECT_BYTES) {
				char oid[128];
				object_name(oid, sizeof(oid), prefix, object_count);
				int rc = rados_write_full(ioctx, oid, (const char *)raw, fill);
				if (rc < 0) {
					fprintf(stderr, "rados_write_full(%s): %s\n", oid, strerror(-rc));
					status = EXIT_FAILURE;
					break;
				}
				object_count++;
				fill = 0;
			}
		}
		close(fd);
	}

	if (status == EXIT_SUCCESS && fill != 0) {
		char oid[128];
		object_name(oid, sizeof(oid), prefix, object_count);
		int rc = rados_write_full(ioctx, oid, (const char *)raw, fill);
		if (rc < 0) {
			fprintf(stderr, "rados_write_full(%s): %s\n", oid, strerror(-rc));
			status = EXIT_FAILURE;
		} else {
			object_count++;
		}
	}

	if (status == EXIT_SUCCESS) {
		struct manifest manifest = {
			.magic = MANIFEST_MAGIC,
			.version = FORMAT_VERSION,
			.object_size = OBJECT_BYTES,
			.object_count = object_count,
			.total_raw_size = total_raw,
			.total_compressed_size = total_raw,
		};
		snprintf(manifest.prefix, sizeof(manifest.prefix), "%s", prefix);
		char oid[128];
		manifest_name(oid, sizeof(oid), prefix);
		int rc = rados_write_full(ioctx, oid, (const char *)&manifest, sizeof(manifest));
		if (rc < 0) {
			fprintf(stderr, "rados_write_full(%s): %s\n", oid, strerror(-rc));
			status = EXIT_FAILURE;
		} else {
			uint64_t elapsed = now_ns() - start;
			printf("WRITE_RAW objects=%" PRIu64 " raw_bytes=%" PRIu64
			       " elapsed_ms=%.3f raw_mib_s=%.2f\n",
			       object_count, total_raw, elapsed / 1e6,
			       (double)total_raw / 1048576.0 / ((double)elapsed / 1e9));
		}
	}

	free(raw);
	close_rados(cluster, ioctx);
	free_file_list(&files);
	return status;
}

static int read_manifest(rados_ioctx_t ioctx, const char *prefix, struct manifest *manifest)
{
	char oid[128];
	manifest_name(oid, sizeof(oid), prefix);
	int rc = rados_read(ioctx, oid, (char *)manifest, sizeof(*manifest), 0);
	if (rc < 0) {
		fprintf(stderr, "rados_read(%s): %s\n", oid, strerror(-rc));
		return -1;
	}
	if ((size_t)rc != sizeof(*manifest) || manifest->magic != MANIFEST_MAGIC ||
	    manifest->version != FORMAT_VERSION || manifest->object_size != OBJECT_BYTES) {
		fprintf(stderr, "Invalid manifest object %s\n", oid);
		return -1;
	}
	return 0;
}

static int doca_decompressor_init(struct doca_decompressor *decompressor,
				  const char *pci_addr,
				  size_t src_capacity,
				  size_t dst_capacity)
{
	memset(decompressor, 0, sizeof(*decompressor));
	decompressor->src_capacity = src_capacity;
	decompressor->dst_capacity = dst_capacity;
	if (posix_memalign((void **)&decompressor->src_memory, 4096, src_capacity) != 0 ||
	    posix_memalign((void **)&decompressor->dst_memory, 4096, dst_capacity) != 0) {
		fprintf(stderr, "Unable to allocate aligned DOCA buffers\n");
		return -1;
	}

	decompressor->resources.mode = COMPRESS_MODE_DECOMPRESS_DEFLATE;
	doca_error_t result = allocate_compress_resources(pci_addr, 2, &decompressor->resources);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "allocate_compress_resources: %s\n", doca_error_get_descr(result));
		return -1;
	}
	decompressor->state = decompressor->resources.state;

	uint64_t max_buffer_size = 0;
	result = doca_compress_cap_task_decompress_deflate_get_max_buf_size(
		doca_dev_as_devinfo(decompressor->state->dev), &max_buffer_size);
	if (result != DOCA_SUCCESS || src_capacity > max_buffer_size || dst_capacity > max_buffer_size) {
		fprintf(stderr, "DOCA maximum buffer size is insufficient: max=%" PRIu64
			" src_required=%zu dst_required=%zu\n",
			max_buffer_size, src_capacity, dst_capacity);
		return -1;
	}
	result = doca_ctx_start(decompressor->state->ctx);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_ctx_start: %s\n", doca_error_get_descr(result));
		return -1;
	}
	result = doca_mmap_set_memrange(decompressor->state->src_mmap,
		decompressor->src_memory, src_capacity);
	if (result == DOCA_SUCCESS)
		result = doca_mmap_start(decompressor->state->src_mmap);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Starting DOCA source mmap: %s\n", doca_error_get_descr(result));
		return -1;
	}
	result = doca_mmap_set_memrange(decompressor->state->dst_mmap,
		decompressor->dst_memory, dst_capacity);
	if (result == DOCA_SUCCESS)
		result = doca_mmap_start(decompressor->state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Starting DOCA destination mmap: %s\n", doca_error_get_descr(result));
		return -1;
	}
	result = doca_buf_inventory_buf_get_by_addr(decompressor->state->buf_inv,
		decompressor->state->src_mmap, decompressor->src_memory, src_capacity,
		&decompressor->src_buf);
	if (result == DOCA_SUCCESS)
		result = doca_buf_inventory_buf_get_by_addr(decompressor->state->buf_inv,
			decompressor->state->dst_mmap, decompressor->dst_memory, dst_capacity,
			&decompressor->dst_buf);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Creating DOCA buffers: %s\n", doca_error_get_descr(result));
		return -1;
	}

	union doca_data task_user_data = {0};
	task_user_data.ptr = &decompressor->task_result;
	result = doca_compress_task_decompress_deflate_alloc_init(
		decompressor->resources.compress, decompressor->src_buf, decompressor->dst_buf,
		task_user_data, &decompressor->decompress_task);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Allocating DOCA decompress task: %s\n", doca_error_get_descr(result));
		return -1;
	}
	decompressor->task = doca_compress_task_decompress_deflate_as_task(decompressor->decompress_task);
	return 0;
}

static int doca_decompress_one(struct doca_decompressor *decompressor,
			       const uint8_t *zlib_data,
			       size_t compressed_size,
			       size_t *output_size)
{
	if (compressed_size < ZLIB_COMPATIBILITY_ADDITIONAL_MEMORY ||
	    compressed_size > decompressor->src_capacity)
		return -1;

	memcpy(decompressor->src_memory, zlib_data, compressed_size);
	void *deflate_data = decompressor->src_memory + ZLIB_HEADER_SIZE;
	size_t deflate_size = compressed_size - ZLIB_COMPATIBILITY_ADDITIONAL_MEMORY;
	doca_error_t result = doca_buf_set_data(decompressor->src_buf, deflate_data, deflate_size);
	if (result == DOCA_SUCCESS)
		result = doca_buf_reset_data_len(decompressor->dst_buf);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Preparing DOCA buffers: %s\n", doca_error_get_descr(result));
		return -1;
	}

	memset(&decompressor->task_result, 0, sizeof(decompressor->task_result));
	decompressor->resources.num_remaining_tasks = 1;
	decompressor->resources.run_pe_progress = true;
	result = doca_task_submit(decompressor->task);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_task_submit: %s\n", doca_error_get_descr(result));
		return -1;
	}
	while (decompressor->resources.run_pe_progress)
		(void)doca_pe_progress(decompressor->state->pe);
	if (decompressor->task_result.status != DOCA_SUCCESS) {
		fprintf(stderr, "DOCA decompression failed: %s\n",
			doca_error_get_descr(decompressor->task_result.status));
		return -1;
	}
	result = doca_buf_get_data_len(decompressor->dst_buf, output_size);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_buf_get_data_len: %s\n", doca_error_get_descr(result));
		return -1;
	}
	return 0;
}

static void doca_decompressor_destroy(struct doca_decompressor *decompressor)
{
	if (decompressor->task != NULL)
		doca_task_free(decompressor->task);
	if (decompressor->dst_buf != NULL)
		(void)doca_buf_dec_refcount(decompressor->dst_buf, NULL);
	if (decompressor->src_buf != NULL)
		(void)doca_buf_dec_refcount(decompressor->src_buf, NULL);
	if (decompressor->resources.state != NULL)
		(void)destroy_compress_resources(&decompressor->resources);
	free(decompressor->src_memory);
	free(decompressor->dst_memory);
}

static int run_read_method(rados_ioctx_t ioctx,
			   const struct manifest *manifest,
			   const char *method,
			   struct doca_decompressor *doca,
			   uint8_t *read_buffer,
			   size_t read_capacity,
			   uint8_t *output_buffer,
			   bool measured,
			   unsigned round,
			   struct method_result *result)
{
	memset(result, 0, sizeof(*result));

	for (uint64_t index = 0; index < manifest->object_count; ++index) {
		char oid[128];
		object_name(oid, sizeof(oid), manifest->prefix, index);
		uint64_t start = now_ns();
		int bytes_read = rados_read(ioctx, oid, (char *)read_buffer, read_capacity, 0);
		uint64_t after_read = now_ns();
		if (bytes_read < 0) {
			fprintf(stderr, "rados_read(%s): %s\n", oid, strerror(-bytes_read));
			return -1;
		}
		if ((size_t)bytes_read < sizeof(struct object_header)) {
			fprintf(stderr, "Object %s is too small\n", oid);
			return -1;
		}
		struct object_header header;
		memcpy(&header, read_buffer, sizeof(header));
		if (header.magic != OBJECT_MAGIC || header.version != FORMAT_VERSION ||
		    header.raw_size > OBJECT_BYTES || header.frame_count == 0 ||
		    header.frame_count > MAX_FRAMES) {
			fprintf(stderr, "Invalid object header in %s\n", oid);
			return -1;
		}

		if (strcmp(method, "cpu") != 0 && strcmp(method, "doca") != 0) {
			fprintf(stderr, "Unknown read method: %s\n", method);
			return -1;
		}

		size_t input_offset = sizeof(header);
		size_t output_size = 0;
		uint64_t compressed_sum = 0;
		for (uint32_t frame_index = 0; frame_index < header.frame_count; ++frame_index) {
			if (input_offset + sizeof(struct frame_header) > (size_t)bytes_read) {
				fprintf(stderr, "Truncated frame header in %s\n", oid);
				return -1;
			}
			struct frame_header frame;
			memcpy(&frame, read_buffer + input_offset, sizeof(frame));
			input_offset += sizeof(frame);
			if (frame.raw_size == 0 || frame.raw_size > FRAME_BYTES ||
			    frame.compressed_size < ZLIB_COMPATIBILITY_ADDITIONAL_MEMORY ||
			    input_offset + frame.compressed_size > (size_t)bytes_read ||
			    output_size + frame.raw_size > OBJECT_BYTES) {
				fprintf(stderr, "Invalid frame %u in %s\n", frame_index, oid);
				return -1;
			}

			size_t frame_output_size = 0;
			if (strcmp(method, "cpu") == 0) {
				uLongf zlib_output_size = frame.raw_size;
				int zrc = uncompress(output_buffer + output_size, &zlib_output_size,
					read_buffer + input_offset, frame.compressed_size);
				if (zrc != Z_OK) {
					fprintf(stderr, "zlib uncompress(%s frame %u): %d\n", oid, frame_index, zrc);
					return -1;
				}
				frame_output_size = zlib_output_size;
			} else {
				if (doca_decompress_one(doca, read_buffer + input_offset,
						frame.compressed_size, &frame_output_size) != 0) {
					fprintf(stderr, "DOCA decompression failed for %s frame %u\n", oid, frame_index);
					return -1;
				}
				memcpy(output_buffer + output_size, doca->dst_memory, frame_output_size);
			}
			if (frame_output_size != frame.raw_size) {
				fprintf(stderr, "Unexpected decompressed frame size in %s frame %u\n", oid, frame_index);
				return -1;
			}
			input_offset += frame.compressed_size;
			compressed_sum += frame.compressed_size;
			output_size += frame_output_size;
		}
		if (input_offset != (size_t)bytes_read || compressed_sum != header.compressed_size) {
			fprintf(stderr, "Object frame accounting failed for %s\n", oid);
			return -1;
		}
		uint64_t after_decompress = now_ns();
		if (output_size != header.raw_size ||
		    (uint32_t)crc32(0L, output_buffer, output_size) != header.raw_crc32) {
			fprintf(stderr, "Data verification failed for %s using %s\n", oid, method);
			return -1;
		}

		result->objects++;
		result->raw_bytes += header.raw_size;
		result->compressed_bytes += header.compressed_size;
		result->total_ns += after_decompress - start;
		result->read_ns += after_read - start;
		result->decompress_ns += after_decompress - after_read;
	}

	if (measured) {
		double seconds = (double)result->total_ns / 1e9;
		printf("RESULT method=%s round=%u objects=%" PRIu64 " raw_bytes=%" PRIu64
		       " compressed_bytes=%" PRIu64 " total_ms=%.3f read_ms=%.3f"
		       " decompress_ms=%.3f throughput_mib_s=%.2f verification=ok\n",
		       method, round, result->objects, result->raw_bytes, result->compressed_bytes,
		       result->total_ns / 1e6, result->read_ns / 1e6,
		       result->decompress_ns / 1e6,
		       (double)result->raw_bytes / 1048576.0 / seconds);
	}
	return 0;
}

static int run_raw_read_method(rados_ioctx_t ioctx,
			       const struct manifest *manifest,
			       uint8_t *read_buffer,
			       bool measured,
			       unsigned round,
			       struct method_result *result)
{
	memset(result, 0, sizeof(*result));

	for (uint64_t index = 0; index < manifest->object_count; ++index) {
		char oid[128];
		object_name(oid, sizeof(oid), manifest->prefix, index);
		uint64_t expected_size = OBJECT_BYTES;
		if (index + 1 == manifest->object_count)
			expected_size = manifest->total_raw_size - index * OBJECT_BYTES;

		uint64_t start = now_ns();
		int bytes_read = rados_read(ioctx, oid, (char *)read_buffer, OBJECT_BYTES, 0);
		uint64_t completed = now_ns();
		if (bytes_read < 0) {
			fprintf(stderr, "rados_read(%s): %s\n", oid, strerror(-bytes_read));
			return -1;
		}
		if ((uint64_t)bytes_read != expected_size) {
			fprintf(stderr, "Unexpected raw object size for %s: got=%d expected=%" PRIu64 "\n",
				oid, bytes_read, expected_size);
			return -1;
		}

		result->objects++;
		result->raw_bytes += (uint64_t)bytes_read;
		result->compressed_bytes += (uint64_t)bytes_read;
		result->total_ns += completed - start;
		result->read_ns += completed - start;
	}

	if (measured) {
		double seconds = (double)result->total_ns / 1e9;
		printf("RESULT method=raw round=%u objects=%" PRIu64 " raw_bytes=%" PRIu64
		       " total_ms=%.3f read_ms=%.3f decompress_ms=0.000"
		       " throughput_mib_s=%.2f verification=size-ok\n",
		       round, result->objects, result->raw_bytes,
		       result->total_ns / 1e6, result->read_ns / 1e6,
		       (double)result->raw_bytes / 1048576.0 / seconds);
	}
	return 0;
}

static void add_result(struct method_result *total, const struct method_result *round)
{
	total->objects += round->objects;
	total->raw_bytes += round->raw_bytes;
	total->compressed_bytes += round->compressed_bytes;
	total->total_ns += round->total_ns;
	total->read_ns += round->read_ns;
	total->decompress_ns += round->decompress_ns;
}

static int benchmark_raw_reads(const char *pool_name, const char *raw_prefix, unsigned rounds)
{
	rados_t cluster;
	rados_ioctx_t ioctx;
	if (connect_rados(pool_name, &cluster, &ioctx) != 0)
		return EXIT_FAILURE;

	struct manifest manifest;
	if (read_manifest(ioctx, raw_prefix, &manifest) != 0) {
		close_rados(cluster, ioctx);
		return EXIT_FAILURE;
	}
	if (manifest.total_compressed_size != manifest.total_raw_size) {
		fprintf(stderr, "Manifest %s is not for an uncompressed dataset\n", raw_prefix);
		close_rados(cluster, ioctx);
		return EXIT_FAILURE;
	}

	uint8_t *read_buffer = malloc(OBJECT_BYTES);
	if (read_buffer == NULL) {
		fprintf(stderr, "Memory allocation failed\n");
		close_rados(cluster, ioctx);
		return EXIT_FAILURE;
	}

	printf("RAW_DATASET objects=%" PRIu64 " raw_bytes=%" PRIu64
	       " object_bytes=%" PRIu64 " rounds=%u prefix=%s\n",
	       manifest.object_count, manifest.total_raw_size, manifest.object_size,
	       rounds, manifest.prefix);
	printf("WARMUP method=raw\n");
	struct method_result scratch;
	if (run_raw_read_method(ioctx, &manifest, read_buffer, false, 0, &scratch) != 0)
		goto fail;

	struct method_result raw_total = {0};
	for (unsigned round = 1; round <= rounds; ++round) {
		struct method_result one_round;
		if (run_raw_read_method(ioctx, &manifest, read_buffer,
				true, round, &one_round) != 0)
			goto fail;
		add_result(&raw_total, &one_round);
	}

	double raw_mib_s = (double)raw_total.raw_bytes / 1048576.0 /
		((double)raw_total.total_ns / 1e9);
	printf("RAW_SUMMARY raw_mib_s=%.2f total_ms=%.3f read_ms=%.3f"
	       " rounds=%u verification=size-ok\n",
	       raw_mib_s, raw_total.total_ns / 1e6, raw_total.read_ns / 1e6, rounds);

	free(read_buffer);
	close_rados(cluster, ioctx);
	return EXIT_SUCCESS;

fail:
	free(read_buffer);
	close_rados(cluster, ioctx);
	return EXIT_FAILURE;
}

static int benchmark_reads(const char *pool_name,
			   const char *compressed_prefix,
			   const char *raw_prefix,
			   unsigned rounds,
			   const char *pci_addr)
{
	rados_t cluster;
	rados_ioctx_t ioctx;
	if (connect_rados(pool_name, &cluster, &ioctx) != 0)
		return EXIT_FAILURE;

	struct manifest compressed_manifest;
	struct manifest raw_manifest;
	if (read_manifest(ioctx, compressed_prefix, &compressed_manifest) != 0 ||
	    read_manifest(ioctx, raw_prefix, &raw_manifest) != 0) {
		close_rados(cluster, ioctx);
		return EXIT_FAILURE;
	}
	if (compressed_manifest.object_count != raw_manifest.object_count ||
	    compressed_manifest.total_raw_size != raw_manifest.total_raw_size) {
		fprintf(stderr, "Compressed and raw datasets do not match\n");
		close_rados(cluster, ioctx);
		return EXIT_FAILURE;
	}
	size_t read_capacity = sizeof(struct object_header) +
		MAX_FRAMES * (sizeof(struct frame_header) + compressBound(FRAME_BYTES));
	uint8_t *read_buffer = malloc(read_capacity);
	uint8_t *output_buffer = malloc(OBJECT_BYTES);
	if (read_buffer == NULL || output_buffer == NULL) {
		fprintf(stderr, "Memory allocation failed\n");
		free(read_buffer);
		free(output_buffer);
		close_rados(cluster, ioctx);
		return EXIT_FAILURE;
	}

	size_t source_capacity = compressBound(FRAME_BYTES);
	struct doca_decompressor doca;
	if (doca_decompressor_init(&doca, pci_addr, source_capacity, FRAME_BYTES) != 0) {
		free(read_buffer);
		free(output_buffer);
		close_rados(cluster, ioctx);
		return EXIT_FAILURE;
	}

	printf("DATASET objects=%" PRIu64 " raw_bytes=%" PRIu64 " compressed_bytes=%" PRIu64
	       " object_bytes=%" PRIu64 " rounds=%u pci=%s raw_prefix=%s compressed_prefix=%s\n",
	       compressed_manifest.object_count, compressed_manifest.total_raw_size,
	       compressed_manifest.total_compressed_size, compressed_manifest.object_size,
	       rounds, pci_addr, raw_manifest.prefix, compressed_manifest.prefix);
	printf("WARMUP method=raw\n");
	struct method_result scratch;
	if (run_raw_read_method(ioctx, &raw_manifest, read_buffer, false, 0, &scratch) != 0)
		goto fail;
	printf("WARMUP method=doca\n");
	if (run_read_method(ioctx, &compressed_manifest, "doca", &doca, read_buffer, read_capacity,
			output_buffer, false, 0, &scratch) != 0)
		goto fail;

	struct method_result raw_total = {0};
	struct method_result doca_total = {0};
	for (unsigned round = 1; round <= rounds; ++round) {
		const char *first = (round % 2 == 1) ? "raw" : "doca";
		const char *second = (round % 2 == 1) ? "doca" : "raw";
		const char *methods[2] = {first, second};
		for (size_t method_index = 0; method_index < 2; ++method_index) {
			struct method_result one_round;
			if (strcmp(methods[method_index], "raw") == 0) {
				if (run_raw_read_method(ioctx, &raw_manifest, read_buffer,
						true, round, &one_round) != 0)
					goto fail;
				add_result(&raw_total, &one_round);
			} else {
				if (run_read_method(ioctx, &compressed_manifest, "doca", &doca,
						read_buffer, read_capacity, output_buffer,
						true, round, &one_round) != 0)
					goto fail;
				add_result(&doca_total, &one_round);
			}
		}
	}

	double raw_mib_s = (double)raw_total.raw_bytes / 1048576.0 / ((double)raw_total.total_ns / 1e9);
	double doca_mib_s = (double)doca_total.raw_bytes / 1048576.0 / ((double)doca_total.total_ns / 1e9);
	double ratio = doca_mib_s / raw_mib_s;
	double doca_slower_pct = (raw_mib_s - doca_mib_s) / raw_mib_s * 100.0;
	printf("SUMMARY raw_mib_s=%.2f doca_mib_s=%.2f doca_over_raw=%.4fx"
	       " doca_slower_pct=%.2f raw_total_ms=%.3f doca_total_ms=%.3f"
	       " raw_read_ms=%.3f doca_read_ms=%.3f doca_decompress_ms=%.3f"
	       " verification=ok\n",
	       raw_mib_s, doca_mib_s, ratio, doca_slower_pct,
	       raw_total.total_ns / 1e6, doca_total.total_ns / 1e6,
	       raw_total.read_ns / 1e6, doca_total.read_ns / 1e6,
	       doca_total.decompress_ns / 1e6);

	doca_decompressor_destroy(&doca);
	free(read_buffer);
	free(output_buffer);
	close_rados(cluster, ioctx);
	return EXIT_SUCCESS;

fail:
	doca_decompressor_destroy(&doca);
	free(read_buffer);
	free(output_buffer);
	close_rados(cluster, ioctx);
	return EXIT_FAILURE;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s write <pool> <dataset-dir> [compressed-prefix]\n"
		"  %s write-raw <pool> <dataset-dir> [raw-prefix]\n"
		"  %s bench-raw <pool> [raw-prefix] [rounds]\n"
		"  %s bench <pool> [compressed-prefix] [raw-prefix] [rounds] [pci-address]\n",
		program, program, program, program);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (strcmp(argv[1], "write") == 0) {
		if (argc < 4 || argc > 5) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		const char *prefix = argc == 5 ? argv[4] : DEFAULT_PREFIX;
		if (strlen(prefix) >= MAX_PREFIX) {
			fprintf(stderr, "Object prefix is too long\n");
			return EXIT_FAILURE;
		}
		return write_compressed_dataset(argv[2], argv[3], prefix);
	}
	if (strcmp(argv[1], "write-raw") == 0) {
		if (argc < 4 || argc > 5) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		const char *prefix = argc == 5 ? argv[4] : DEFAULT_RAW_PREFIX;
		if (strlen(prefix) >= MAX_PREFIX) {
			fprintf(stderr, "Object prefix is too long\n");
			return EXIT_FAILURE;
		}
		return write_raw_dataset(argv[2], argv[3], prefix);
	}
	if (strcmp(argv[1], "bench-raw") == 0) {
		if (argc < 3 || argc > 5) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		const char *raw_prefix = argc >= 4 ? argv[3] : DEFAULT_RAW_PREFIX;
		unsigned rounds = argc >= 5 ? (unsigned)strtoul(argv[4], NULL, 10) : 5;
		if (rounds == 0) {
			fprintf(stderr, "Rounds must be greater than zero\n");
			return EXIT_FAILURE;
		}
		return benchmark_raw_reads(argv[2], raw_prefix, rounds);
	}
	if (strcmp(argv[1], "bench") == 0) {
		if (argc < 3 || argc > 7) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		const char *compressed_prefix = argc >= 4 ? argv[3] : DEFAULT_PREFIX;
		const char *raw_prefix = argc >= 5 ? argv[4] : DEFAULT_RAW_PREFIX;
		unsigned rounds = argc >= 6 ? (unsigned)strtoul(argv[5], NULL, 10) : 5;
		const char *pci_addr = argc >= 7 ? argv[6] : DEFAULT_PCI_ADDR;
		if (rounds == 0) {
			fprintf(stderr, "Rounds must be greater than zero\n");
			return EXIT_FAILURE;
		}
		return benchmark_reads(argv[2], compressed_prefix, raw_prefix, rounds, pci_addr);
	}
	usage(argv[0]);
	return EXIT_FAILURE;
}

