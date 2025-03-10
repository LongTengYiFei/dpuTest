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

#include <stdlib.h>
#include <string.h>
#include <doca_argp.h>
#include <doca_compress.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
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
#include <sys/time.h>
#include <utils.h>
#include <fcntl.h>

#include "common.h"
#include "compress_common.h"

DOCA_LOG_REGISTER(COMPRESS_DEFLATE::MAIN);

doca_error_t compress_deflate(char* file_name);
#define GB (1024ul * 1024ul * 1024ul)
#define MB (1024ul * 1024ul)

struct compress_deflate_result {
	doca_error_t status; /**< The completion status */
	uint32_t crc_cs;     /**< The CRC checksum */
	uint32_t adler_cs;   /**< The Adler Checksum */
};

doca_error_t compress_deflate(char* file_name)
{
	/*
		切片读文件，每次读一点到src_buffer中，覆盖原来的数据；
	*/	

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

	int fd = open(file_name, O_RDONLY);


	/* Allocate resources */
	resources.mode = COMPRESS_MODE_COMPRESS_DEFLATE;
	result = allocate_compress_resources("b1:00.0", max_bufs, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate compress resources: %s", doca_error_get_descr(result));
		return result;
	}

	state = resources.state;
	result = doca_compress_cap_task_decompress_deflate_get_max_buf_size(doca_dev_as_devinfo(state->dev), &max_buf_size);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to query compress max buf size: %s", doca_error_get_descr(result));

	char *src_buffer = (char*)malloc(max_buf_size);

	// 默认最大2M
	max_output_size = max_buf_size;

	/* Start compress context */
	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));

	dst_buffer = calloc(1, max_output_size);
	if (dst_buffer == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate memory: %s", doca_error_get_descr(result));
	}
		
	// 目标就准备2M就行，反复覆盖
	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, max_output_size);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));

	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));

	// 源数据
	result = doca_mmap_set_memrange(state->src_mmap, src_buffer, max_buf_size);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));

	/* Construct DOCA buffer for each address range */
	result =
		doca_buf_inventory_buf_get_by_addr(state->buf_inv, state->src_mmap, src_buffer, max_buf_size, &src_doca_buf);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));

	/* Construct DOCA buffer for each address range */
	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    state->dst_mmap,
						    dst_buffer,
						    max_buf_size,
						    &dst_doca_buf);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
			     doca_error_get_descr(result));

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
	
	int block_size = max_buf_size;

	// set 一次就行了，这个地址已经注册过，之后反复read覆盖；
	result = doca_buf_set_data(src_doca_buf, src_buffer, block_size);
	if (result != DOCA_SUCCESS){
		DOCA_LOG_ERR("Unable to set data in the DOCA buffer representing source buffer: %s", doca_error_get_descr(result));
		return result;
	}


	int compress_time_us = 0;
	int size_after_compression;
	unsigned long total_size_before_compression = 0;
	unsigned long total_size_after_compression = 0;
	// 没搞懂为啥超过2GB就任务失败
	for(;;){
		// read from file
		int n = read(fd, src_buffer, block_size);
		total_size_before_compression += n;
		if(n == 0) break;

		// reset
		doca_buf_reset_data_len(dst_doca_buf);

		struct timeval start_time, end_time;
		gettimeofday(&start_time, NULL);
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
		gettimeofday(&end_time, NULL);
		compress_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);

		/* Check result of task according to the result we update in the callbacks */
		if (task_result.status != DOCA_SUCCESS)
			return task_result.status;

		result = doca_buf_get_data_len(dst_doca_buf, &size_after_compression);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to get data length in the DOCA buffer representing destination buffer: %s", doca_error_get_descr(result));
			return result;
		}
		total_size_after_compression += size_after_compression;
		
	}

	printf("used time ms: %d\n", compress_time_us / 1000);
	printf("compression speed %.2f MB/s \n", total_size_before_compression / 1024.0 / 1024.0 / (compress_time_us / 1000000.0));
	printf("total size before compression: %.2f MB\n", total_size_before_compression / 1024.0 / 1024.0);
	printf("total size after compression: %.2f MB\n", total_size_after_compression / 1024.0 / 1024.0);
	printf("compress ratio: %.2f%%\n", (1 - (float)total_size_after_compression / (float)total_size_before_compression)*100);

	return result;
}

int main(int argc, char **argv)
{
	doca_error_t result;
	struct compress_cfg compress_cfg;
	uint8_t *file_data = (uint8_t*)malloc(GB);;
	size_t file_size;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS){
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}

	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS){
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}
		
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS){
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}
		
	result = doca_argp_init("doca_compress_deflate", &compress_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}

	result = register_compress_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}

	result = register_deflate_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		return 0;
	}

	compress_deflate("/home/cyf/dpuTest/compress/applogcat.log");
}
