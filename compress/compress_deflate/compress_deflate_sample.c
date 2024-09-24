/*
 * Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_compress.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"
#include "compress_common.h"

#include <sys/time.h>

DOCA_LOG_REGISTER(COMPRESS_DEFLATE);

struct compress_deflate_result {
	doca_error_t status; /**< The completion status */
	uint32_t crc_cs;     /**< The CRC checksum */
	uint32_t adler_cs;   /**< The Adler Checksum */
};

/*
 * Run compress_deflate sample
 *
 * @cfg [in]: Configuration parameters
 * @file_data [in]: file data for the compress task
 * @file_size [in]: file size
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
doca_error_t compress_deflate(struct compress_cfg *cfg, char *file_data, size_t file_size)
{
	struct compress_resources resources = {0};
	struct program_core_objects *state;
	struct doca_buf *src_doca_buf;
	struct doca_buf *dst_doca_buf;
	/* The sample will use 2 doca buffers */
	uint32_t max_bufs = 2;
	uint64_t output_checksum = 0;
	uint32_t adler_checksum = 0;
	doca_be32_t be_adler_checksum;
	char *dst_buffer;
	void *dst_buf_data, *dst_buf_tail;
	size_t data_len, write_len, written_len;
	FILE *out_file;
	doca_error_t result, tmp_result;
	uint64_t max_buf_size, max_output_size;

	out_file = fopen(cfg->output_path, "wr");
	if (out_file == NULL) {
		DOCA_LOG_ERR("Unable to open output file: %s", cfg->output_path);
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Allocate resources */
	resources.mode = COMPRESS_MODE_COMPRESS_DEFLATE;
	result = allocate_compress_resources(cfg->pci_address, max_bufs, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate compress resources: %s", doca_error_get_descr(result));
		goto close_file;
	}
	state = resources.state;

	result = doca_compress_cap_task_decompress_deflate_get_max_buf_size(doca_dev_as_devinfo(state->dev),
									    &max_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query compress max buf size: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	// 切片计算，解除限制，先注释掉
	// if (file_size > max_buf_size) {
	// 	DOCA_LOG_ERR("Invalid file size. Should be smaller than %lu", max_buf_size);
	// 	goto destroy_resources;
	// }

	// 默认最大2M
	max_output_size = max_buf_size;

	/* Consider the Zlib header and the added checksum at the end */
	if (cfg->zlib_compatible)
		max_output_size += ZLIB_COMPATIBILITY_ADDITIONAL_MEMORY;

	/* Start compress context */
	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	dst_buffer = calloc(1, max_output_size);
	if (dst_buffer == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate memory: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	// 目标就准备2M就行，反复覆盖
	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, max_output_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));
		goto free_dst_buf;
	}
	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		goto free_dst_buf;
	}

	// 源数据
	result = doca_mmap_set_memrange(state->src_mmap, file_data, file_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));
		goto free_dst_buf;
	}

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		goto free_dst_buf;
	}

	/* Construct DOCA buffer for each address range */
	result =
		doca_buf_inventory_buf_get_by_addr(state->buf_inv, state->src_mmap, file_data, file_size, &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));
		goto free_dst_buf;
	}

	/* Construct DOCA buffer for each address range */
	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    state->dst_mmap,
						    dst_buffer,
						    max_buf_size,
						    &dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
			     doca_error_get_descr(result));
		goto destroy_src_buf;
	}

	if (cfg->zlib_compatible) {
		/* Set data pointer to reserve space for the header */
		result = doca_buf_set_data(dst_doca_buf, dst_buffer + ZLIB_HEADER_SIZE, 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set data in the DOCA buffer representing destination buffer: %s",
				     doca_error_get_descr(result));
			goto destroy_dst_buf;
		}
	}

	// alloc task
	struct doca_compress_task_compress_deflate *compress_task;
	struct doca_task *task;
	union doca_data task_user_data = {0};
	struct compress_deflate_result task_result = {0};
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	task_user_data.ptr = &task_result;
	result = doca_compress_task_compress_deflate_alloc_init(resources.compress,
								src_doca_buf,
								dst_doca_buf,
								task_user_data,
								&compress_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate compress task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_compress_task_compress_deflate_as_task(compress_task);


	// 每次循环都要设置src doca buf，然后reset dst doca buf的长度
	unsigned long total_compressed_len = 0;
	unsigned long cost_time_us = 0;
	int block_size = (1024*1024);

	struct timeval start_time, end_time;
	gettimeofday(&start_time, NULL);

	// 没搞懂为啥超过2GB就任务失败
	for(int i=0; i<=1024-1; i++){
		// set src data
		result = doca_buf_set_data(src_doca_buf, file_data + block_size*i, block_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set data in the DOCA buffer representing source buffer: %s",
					doca_error_get_descr(result));
			goto destroy_dst_buf;
		}

		// reset dst len
		doca_buf_reset_data_len(dst_doca_buf);

		/* Submit compress task */
		resources.num_remaining_tasks++;
		result = doca_task_submit(task);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit compress task: %s", doca_error_get_descr(result));
			doca_task_free(task);
			return result;
		}

		resources.run_pe_progress = true;

		/* Wait for all tasks to be completed */
		while (resources.run_pe_progress) {
			if (doca_pe_progress(state->pe) == 0)
				nanosleep(&ts, &ts);
		}

		/* Check result of task according to the result we update in the callbacks */
		if (task_result.status != DOCA_SUCCESS)
			return task_result.status;

		result = doca_buf_get_data_len(dst_doca_buf, &data_len);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to get data length in the DOCA buffer representing destination buffer: %s",
					doca_error_get_descr(result));
			goto destroy_dst_buf;
		}
		total_compressed_len += data_len;
	}

	gettimeofday(&end_time, NULL);
	int used_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);
	DOCA_LOG_INFO("used time ms: %d", used_time_us / 1000);
	DOCA_LOG_INFO("total compressed len: %d", total_compressed_len);

	// 后面的不用管了，直接返回
	return result;

	result = doca_buf_get_data_len(dst_doca_buf, &data_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get data length in the DOCA buffer representing destination buffer: %s",
			     doca_error_get_descr(result));
		goto destroy_dst_buf;
	}
	write_len = data_len;

	if (cfg->zlib_compatible) {
		/* Write the Zlib header in the reserved header space */
		init_compress_zlib_header((struct compress_zlib_header *)dst_buffer);

		/* Retrieve the data address and compute the end of the data section */
		result = doca_buf_get_data(dst_doca_buf, &dst_buf_data);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to get data length in the DOCA buffer representing destination buffer: %s",
				     doca_error_get_descr(result));
			goto destroy_dst_buf;
		}
		dst_buf_tail = ((uint8_t *)dst_buf_data + data_len);

		/* Set data pointer to consider the added checksum */
		data_len += ZLIB_TRAILER_SIZE;
		result = doca_buf_set_data(dst_doca_buf, dst_buf_data, data_len);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set data in the DOCA buffer representing destination buffer: %s",
				     doca_error_get_descr(result));
			goto destroy_dst_buf;
		}

		/* Extract the Adler32 checksum from the output_checksum and write it after the compressed data */
		adler_checksum = (uint32_t)(output_checksum >> ADLER_CHECKSUM_SHIFT);
		be_adler_checksum = htobe32(adler_checksum);

		memcpy(dst_buf_tail, &be_adler_checksum, ZLIB_TRAILER_SIZE);

		/* Consider the Zlib header and the added checksum at the end */
		write_len += ZLIB_COMPATIBILITY_ADDITIONAL_MEMORY;
	}

	/* Write the result to output file */
	written_len = fwrite(dst_buffer, sizeof(uint8_t), write_len, out_file);
	if (written_len != write_len) {
		DOCA_LOG_ERR("Failed to write the DOCA buffer representing destination buffer into a file");
		goto destroy_dst_buf;
	}

	DOCA_LOG_INFO("File was compressed successfully and saved in: %s", cfg->output_path);
	if (cfg->output_checksum)
		DOCA_LOG_INFO("Checksum is %lu", output_checksum);

destroy_dst_buf:
	tmp_result = doca_buf_dec_refcount(dst_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decrease DOCA destination buffer reference count: %s",
			     doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_src_buf:
	tmp_result = doca_buf_dec_refcount(src_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to decrease DOCA source buffer reference count: %s",
			     doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
free_dst_buf:
	free(dst_buffer);
destroy_resources:
	tmp_result = destroy_compress_resources(&resources);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy compress resources: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
close_file:
	fclose(out_file);

	return result;
}
