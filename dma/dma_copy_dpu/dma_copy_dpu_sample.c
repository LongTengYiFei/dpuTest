/*
 * Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "dma_common.h"

#include <sys/time.h>

DOCA_LOG_REGISTER(DMA_COPY_DPU);

#define SLEEP_IN_NANOS (10 * 1000) /* Sample the task every 10 microseconds  */
#define RECV_BUF_SIZE (512)	   /* Buffer which contains config information */

#define KB (1024ul)
#define MB (1024ul*KB)
#define GB (1024ul*MB)

/*
 * Saves export descriptor and buffer information content into memory buffers
 *
 * @export_desc_file_path [in]: Export descriptor file path
 * @buffer_info_file_path [in]: Buffer information file path
 * @export_desc [in]: Export descriptor buffer
 * @export_desc_len [in]: Export descriptor buffer length
 * @remote_addr [in]: Remote buffer address
 * @remote_addr_len [in]: Remote buffer total length
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t save_config_info_to_buffers(const char *export_desc_file_path,
						const char *buffer_info_file_path,
						char *export_desc,
						size_t *export_desc_len,
						char **remote_addr,
						size_t *remote_addr_len)
{
	FILE *fp;
	long file_size;
	char buffer[RECV_BUF_SIZE];
	size_t convert_value;

	fp = fopen(export_desc_file_path, "r");
	if (fp == NULL) {
		DOCA_LOG_ERR("Failed to open %s", export_desc_file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		DOCA_LOG_ERR("Failed to calculate file size");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	file_size = ftell(fp);
	if (file_size == -1) {
		DOCA_LOG_ERR("Failed to calculate file size");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	if (file_size > RECV_BUF_SIZE)
		file_size = RECV_BUF_SIZE;

	*export_desc_len = file_size;

	if (fseek(fp, 0L, SEEK_SET) != 0) {
		DOCA_LOG_ERR("Failed to calculate file size");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	if (fread(export_desc, 1, file_size, fp) != (size_t)file_size) {
		DOCA_LOG_ERR("Failed to allocate memory for source buffer");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}

	fclose(fp);

	/* Read source buffer information from file */
	fp = fopen(buffer_info_file_path, "r");
	if (fp == NULL) {
		DOCA_LOG_ERR("Failed to open %s", buffer_info_file_path);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Get source buffer address */
	if (fgets(buffer, RECV_BUF_SIZE, fp) == NULL) {
		DOCA_LOG_ERR("Failed to read the source (host) buffer address");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}
	convert_value = strtoull(buffer, NULL, 0);
	if (convert_value == ULLONG_MAX) {
		DOCA_LOG_ERR("Failed to read the source (host) buffer address. Data is corrupted");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}
	*remote_addr = (char *)convert_value;

	memset(buffer, 0, RECV_BUF_SIZE);

	/* Get source buffer length */
	if (fgets(buffer, RECV_BUF_SIZE, fp) == NULL) {
		DOCA_LOG_ERR("Failed to read the source (host) buffer length");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}
	convert_value = strtoull(buffer, NULL, 0);
	if (convert_value == ULLONG_MAX) {
		DOCA_LOG_ERR("Failed to read the source (host) buffer length. Data is corrupted");
		fclose(fp);
		return DOCA_ERROR_IO_FAILED;
	}
	*remote_addr_len = convert_value;

	fclose(fp);

	return DOCA_SUCCESS;
}

/*
 * Run DOCA DMA DPU copy sample
 *
 * @export_desc_file_path [in]: Export descriptor file path
 * @buffer_info_file_path [in]: Buffer info file path
 * @pcie_addr [in]: Device PCI address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dma_copy_dpu(char *export_desc_file_path, char *buffer_info_file_path, const char *pcie_addr)
{
	struct dma_resources resources;
	struct program_core_objects *state = &resources.state;
	union doca_data task_user_data = {0};

	struct doca_mmap *remote_mmap = NULL;
	char export_desc[1024] = {0};
	char *remote_addr = NULL;
	char *dpu_buffer;
	size_t dst_buffer_size, remote_addr_len = 0, export_desc_len = 0;
	size_t max_buffer_size;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result, tmp_result, task_result;

	int batch_size = 32;
	int payload_size = 256 * KB;
	int full_queue_size = payload_size * batch_size;
	

	struct doca_dma_task_memcpy **dma_task = (struct doca_dma_task_memcpy **)malloc(batch_size * sizeof(struct doca_dma_task_memcpy*));
	struct doca_task **task = (struct doca_task **)malloc(batch_size * sizeof(struct doca_task*));
	struct doca_buf **src_doca_buf = (struct doca_buf **)malloc(batch_size * sizeof(struct doca_buf*));
	struct doca_buf **dst_doca_buf = (struct doca_buf **)malloc(batch_size * sizeof(struct doca_buf*));

	/* Allocate resources */
	result = allocate_dma_resources(pcie_addr, &resources, 2*batch_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DMA resources: %s", doca_error_get_descr(result));
		return result;
	}

	/* Connect context to progress engine */
	result = doca_pe_connect_ctx(state->pe, state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect progress engine to context: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
		goto destroy_resources;
	}

	/* Copy all relevant information into local buffers */
	result = save_config_info_to_buffers(export_desc_file_path,
					     buffer_info_file_path,
					     export_desc,
					     &export_desc_len,
					     &remote_addr,
					     &remote_addr_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read memory configuration from file: %s", doca_error_get_descr(result));
		goto stop_dma;
	}

	dpu_buffer = (char *)malloc(full_queue_size);
	if (dpu_buffer == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate buffer memory: %s", doca_error_get_descr(result));
		goto stop_dma;
	}

	result = doca_mmap_set_memrange(state->dst_mmap, dpu_buffer, full_queue_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memory range for destination mmap: %s", doca_error_get_descr(result));
		goto free_dpu_buffer;
	}

	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start destination mmap: %s", doca_error_get_descr(result));
		goto free_dpu_buffer;
	}

	/* Create a local DOCA mmap from exported data */
	result =
		doca_mmap_create_from_export(NULL, (const void *)export_desc, export_desc_len, state->dev, &remote_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap from export: %s", doca_error_get_descr(result));
		goto free_dpu_buffer;
	}

	for(int i=0; i<=batch_size-1; i++){
		/* Construct DOCA buffer for each address range */
		// 这个src地址映射没办法了，就是这么奇怪
		result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
								remote_mmap,
								remote_addr,
								payload_size,
								&src_doca_buf[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing remote buffer: %s",
					doca_error_get_descr(result));
			goto destroy_remote_mmap;
		}
		
		/* Construct DOCA buffer for each address range */
		result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
								state->dst_mmap,
								dpu_buffer + payload_size * i,
								payload_size,
								&dst_doca_buf[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
					doca_error_get_descr(result));
			goto destroy_src_buf;
		}
	}

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;

	/* Allocate and construct DMA task */
	for(int i=0; i<=batch_size-1; i++){
		result = doca_dma_task_memcpy_alloc_init(resources.dma_ctx,
				src_doca_buf[i],
				dst_doca_buf[i],
				task_user_data,
				&dma_task[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate DMA memcpy task: %s", doca_error_get_descr(result));
			goto destroy_dst_buf;
		}

		task[i] = doca_dma_task_memcpy_as_task(dma_task[i]);
		if (task[i] == NULL) {
			result = DOCA_ERROR_UNEXPECTED;
			DOCA_LOG_ERR("Failed to get DOCA DMA hash task as DOCA task: %s", doca_error_get_descr(result));
			return result;
		}
	}

	struct timeval start_time, end_time;
	gettimeofday(&start_time, NULL);
	long long cost_time_us = 0;

	int batch_num = remote_addr_len / full_queue_size;
	for(int i=0; i<=batch_num - 1; i++){
		resources.num_remaining_tasks = 0;
		for(int j=0; j<=batch_size-1; j++){
			// 调整源地址 remote
			result = doca_buf_set_data(src_doca_buf[j], remote_addr + full_queue_size*i + payload_size*j, payload_size);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to set data in the DOCA buffer representing source buffer: %s",
						doca_error_get_descr(result));
				goto destroy_dst_buf;
			}

			// 重置目标长度为0
			doca_buf_reset_data_len(dst_doca_buf[j]);

			/* Submit DMA task */
			resources.num_remaining_tasks++;
			result = doca_task_submit(task[j]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to submit DMA task: %s", doca_error_get_descr(result));
				doca_task_free(task[j]);
				goto destroy_dst_buf;
			}
		}

		resources.run_pe_progress = true;

		/* Wait for all tasks to be completed and context stopped */
		while (resources.run_pe_progress) {
			if (doca_pe_progress(state->pe) == 0)
				;
			result = task_result;
		}

		DOCA_LOG_INFO("batch %d over", i);
	}

	gettimeofday(&end_time, NULL);
	cost_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);
	DOCA_LOG_INFO("DMA memcpy task cost %lld us", cost_time_us);

	/* Check result of task according to the result we update in the callbacks */
	// if (task_result == DOCA_SUCCESS) {
	// 	DOCA_LOG_INFO("Remote DMA copy was done Successfully");
	// 	dpu_buffer[dst_buffer_size - 1] = '\0';
	// 	DOCA_LOG_INFO("Memory content: %s", dpu_buffer);
	// } else
	// 	DOCA_LOG_ERR("DMA memcpy task failed: %s", doca_error_get_descr(task_result));

	result = task_result;
	DOCA_LOG_INFO("all batch over");
	return result;

	/* Inform host that DMA operation is done */
	DOCA_LOG_INFO("Host sample can be closed, DMA copy ended");

destroy_dst_buf:
	tmp_result = doca_buf_dec_refcount(dst_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to decrease DOCA destination buffer reference count: %s",
			     doca_error_get_descr(tmp_result));
	}
destroy_src_buf:
	tmp_result = doca_buf_dec_refcount(src_doca_buf, NULL);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to decrease DOCA source buffer reference count: %s",
			     doca_error_get_descr(tmp_result));
	}
destroy_remote_mmap:
	tmp_result = doca_mmap_destroy(remote_mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to decrease remote mmap: %s", doca_error_get_descr(tmp_result));
	}
free_dpu_buffer:
	free(dpu_buffer);
stop_dma:
	tmp_result = doca_ctx_stop(state->ctx);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Unable to stop context: %s", doca_error_get_descr(tmp_result));
	}
	state->ctx = NULL;
destroy_resources:
	tmp_result = destroy_dma_resources(&resources);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy DMA resources: %s", doca_error_get_descr(tmp_result));
	}

	return result;
}
