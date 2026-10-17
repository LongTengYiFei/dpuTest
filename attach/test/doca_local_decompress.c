#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_compress.h>
#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "doca_common.h"
#include "compress_common.h"

#define OBJECT_MAGIC UINT32_C(0x4344424f)
#define FORMAT_VERSION 2U
#define OBJECT_BYTES (4U * 1024U * 1024U)
#define FRAME_BYTES (1U * 1024U * 1024U)
#define MAX_FRAMES (OBJECT_BYTES / FRAME_BYTES)

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

static int decompressor_init(struct doca_decompressor *decompressor, const char *pci_addr)
{
	memset(decompressor, 0, sizeof(*decompressor));
	decompressor->src_capacity = compressBound(FRAME_BYTES);
	decompressor->dst_capacity = FRAME_BYTES;
	if (posix_memalign((void **)&decompressor->src_memory, 4096,
			decompressor->src_capacity) != 0 ||
	    posix_memalign((void **)&decompressor->dst_memory, 4096,
			decompressor->dst_capacity) != 0) {
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
	if (result != DOCA_SUCCESS || decompressor->src_capacity > max_buffer_size ||
	    decompressor->dst_capacity > max_buffer_size) {
		fprintf(stderr, "DOCA max buffer=%" PRIu64 ", src=%zu, dst=%zu\n",
			max_buffer_size, decompressor->src_capacity, decompressor->dst_capacity);
		return -1;
	}

	result = doca_ctx_start(decompressor->state->ctx);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_ctx_start: %s\n", doca_error_get_descr(result));
		return -1;
	}
	result = doca_mmap_set_memrange(decompressor->state->src_mmap,
		decompressor->src_memory, decompressor->src_capacity);
	if (result == DOCA_SUCCESS)
		result = doca_mmap_start(decompressor->state->src_mmap);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Starting source mmap: %s\n", doca_error_get_descr(result));
		return -1;
	}
	result = doca_mmap_set_memrange(decompressor->state->dst_mmap,
		decompressor->dst_memory, decompressor->dst_capacity);
	if (result == DOCA_SUCCESS)
		result = doca_mmap_start(decompressor->state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Starting destination mmap: %s\n", doca_error_get_descr(result));
		return -1;
	}
	result = doca_buf_inventory_buf_get_by_addr(decompressor->state->buf_inv,
		decompressor->state->src_mmap, decompressor->src_memory,
		decompressor->src_capacity, &decompressor->src_buf);
	if (result == DOCA_SUCCESS)
		result = doca_buf_inventory_buf_get_by_addr(decompressor->state->buf_inv,
			decompressor->state->dst_mmap, decompressor->dst_memory,
			decompressor->dst_capacity, &decompressor->dst_buf);
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
		fprintf(stderr, "Allocating decompress task: %s\n", doca_error_get_descr(result));
		return -1;
	}
	decompressor->task =
		doca_compress_task_decompress_deflate_as_task(decompressor->decompress_task);
	return 0;
}

static void decompressor_destroy(struct doca_decompressor *decompressor)
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

static int decompress_frame(struct doca_decompressor *decompressor,
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

static int write_all(int fd, const uint8_t *data, size_t size)
{
	while (size != 0) {
		ssize_t written = write(fd, data, size);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		data += written;
		size -= (size_t)written;
	}
	return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
	struct stat st;
	if (stat(path, &st) != 0 || st.st_size <= 0) {
		fprintf(stderr, "stat(%s): %s\n", path, strerror(errno));
		return -1;
	}
	*size = (size_t)st.st_size;
	*data = malloc(*size);
	if (*data == NULL)
		return -1;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		free(*data);
		return -1;
	}
	size_t offset = 0;
	while (offset < *size) {
		ssize_t count = read(fd, *data + offset, *size - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			fprintf(stderr, "read(%s): %s\n", path,
				count < 0 ? strerror(errno) : "unexpected EOF");
			close(fd);
			free(*data);
			return -1;
		}
		offset += (size_t)count;
	}
	close(fd);
	return 0;
}

static int decompress_object(struct doca_decompressor *decompressor,
			     const char *input_path,
			     const char *output_dir,
			     uint64_t *raw_total)
{
	uint8_t *object_data = NULL;
	size_t object_size = 0;
	if (read_file(input_path, &object_data, &object_size) != 0)
		return -1;
	if (object_size < sizeof(struct object_header)) {
		fprintf(stderr, "Object %s is too small\n", input_path);
		free(object_data);
		return -1;
	}

	struct object_header header;
	memcpy(&header, object_data, sizeof(header));
	if (header.magic != OBJECT_MAGIC || header.version != FORMAT_VERSION ||
	    header.raw_size > OBJECT_BYTES || header.frame_count == 0 ||
	    header.frame_count > MAX_FRAMES) {
		fprintf(stderr, "Invalid object header in %s\n", input_path);
		free(object_data);
		return -1;
	}

	const char *base = strrchr(input_path, '/');
	base = base == NULL ? input_path : base + 1;
	char output_path[4096];
	int length = snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, base);
	if (length < 0 || (size_t)length >= sizeof(output_path)) {
		free(object_data);
		return -1;
	}
	char *suffix = strstr(output_path + strlen(output_dir), ".zobj");
	if (suffix != NULL && suffix[5] == '\0')
		strcpy(suffix, ".raw");

	int output_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (output_fd < 0) {
		fprintf(stderr, "open(%s): %s\n", output_path, strerror(errno));
		free(object_data);
		return -1;
	}

	size_t input_offset = sizeof(header);
	uint64_t output_size = 0;
	uint64_t compressed_sum = 0;
	uLong crc = crc32(0L, Z_NULL, 0);
	int status = 0;
	for (uint32_t index = 0; index < header.frame_count; ++index) {
		if (input_offset + sizeof(struct frame_header) > object_size) {
			status = -1;
			break;
		}
		struct frame_header frame;
		memcpy(&frame, object_data + input_offset, sizeof(frame));
		input_offset += sizeof(frame);
		if (frame.raw_size == 0 || frame.raw_size > FRAME_BYTES ||
		    input_offset + frame.compressed_size > object_size) {
			status = -1;
			break;
		}
		size_t frame_output_size = 0;
		if (decompress_frame(decompressor, object_data + input_offset,
				frame.compressed_size, &frame_output_size) != 0 ||
		    frame_output_size != frame.raw_size ||
		    write_all(output_fd, decompressor->dst_memory, frame_output_size) != 0) {
			status = -1;
			break;
		}
		crc = crc32(crc, decompressor->dst_memory, frame_output_size);
		input_offset += frame.compressed_size;
		compressed_sum += frame.compressed_size;
		output_size += frame_output_size;
	}
	if (close(output_fd) != 0)
		status = -1;
	if (status != 0 || input_offset != object_size || output_size != header.raw_size ||
	    compressed_sum != header.compressed_size || (uint32_t)crc != header.raw_crc32) {
		fprintf(stderr, "Verification failed for %s\n", input_path);
		unlink(output_path);
		free(object_data);
		return -1;
	}

	*raw_total += output_size;
	free(object_data);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "Usage: %s <pci-address> <output-dir> <object.zobj>...\n", argv[0]);
		return EXIT_FAILURE;
	}

	struct doca_decompressor decompressor;
	if (decompressor_init(&decompressor, argv[1]) != 0) {
		decompressor_destroy(&decompressor);
		return EXIT_FAILURE;
	}

	uint64_t raw_total = 0;
	int status = EXIT_SUCCESS;
	for (int index = 3; index < argc; ++index) {
		if (decompress_object(&decompressor, argv[index], argv[2], &raw_total) != 0) {
			status = EXIT_FAILURE;
			break;
		}
	}
	if (status == EXIT_SUCCESS)
		printf("DOCA_LOCAL objects=%d raw_bytes=%" PRIu64 " verification=ok\n",
			argc - 3, raw_total);
	decompressor_destroy(&decompressor);
	return status;
}
