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

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_mmap.h>
#include <doca_sha.h>
#include <doca_pe.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"
#include <math.h>
#include <sys/time.h>

DOCA_LOG_REGISTER(SHA_CREATE);

#define SLEEP_IN_NANOS (10 * 1000)			 /* Sample the task every 10 microseconds  */
#define LOG_NUM_SHA_TASKS (0)				 /* Log of SHA tasks number */
#define SHA_SAMPLE_ALGORITHM (DOCA_SHA_ALGORITHM_SHA1) /* doca_sha_algorithm for the sample */
#define SHA256_LEN 32
struct sha_resources {
	struct program_core_objects state; /* Core objects that manage our "state" */
	struct doca_sha *sha_ctx;	   /* DOCA SHA context */
	size_t num_remaining_tasks;	   /* Number of remaining tasks to process */
	bool run_pe_progress;		   /* Should we keep on progressing the PE? */
};

/*
 * Free callback - free doca_buf allocated pointer
 *
 * @addr [in]: Memory range pointer
 * @len [in]: Memory range length
 * @opaque [in]: An opaque pointer passed to iterator
 */
void free_cb(void *addr, size_t len, void *opaque)
{
	(void)len;
	(void)opaque;

	free(addr);
}

/*
 * Clean all the sample resources
 *
 * @resources [in]: sha_resources struct
 * @return: DOCA_SUCCESS if the device supports SHA hash task and DOCA_ERROR otherwise.
 */
static doca_error_t sha_cleanup(struct sha_resources *resources)
{
	struct program_core_objects *state = &resources->state;
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	if (state->pe != NULL && state->ctx != NULL) {
		tmp_result = doca_ctx_stop(state->ctx);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to destroy DOCA SHA: %s", doca_error_get_descr(tmp_result));
		}

		state->ctx = NULL;
	}

	if (resources->sha_ctx != NULL) {
		tmp_result = doca_sha_destroy(resources->sha_ctx);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to destroy DOCA SHA: %s", doca_error_get_descr(tmp_result));
		}
	}

	tmp_result = destroy_core_objects(state);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to destroy DOCA SHA: %s", doca_error_get_descr(tmp_result));
	}

	return result;
}

/*
 * SHA hash task completed callback
 *
 * @sha_hash_task [in]: Completed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void sha_hash_completed_callback(struct doca_sha_task_hash *sha_hash_task,
					union doca_data task_user_data,
					union doca_data ctx_user_data)
{
	struct sha_resources *resources = (struct sha_resources *)ctx_user_data.ptr;
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	*result = DOCA_SUCCESS;

	--resources->num_remaining_tasks;

	if (resources->num_remaining_tasks == 0)
		resources->run_pe_progress = false;

}

/*
 * SHA hash task error callback
 *
 * @sha_hash_task [in]: Failed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void sha_hash_error_callback(struct doca_sha_task_hash *sha_hash_task,
				    union doca_data task_user_data,
				    union doca_data ctx_user_data)
{
	struct sha_resources *resources = (struct sha_resources *)ctx_user_data.ptr;
	struct doca_task *task = doca_sha_task_hash_as_task(sha_hash_task);
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	/* Get the result of the task */
	*result = doca_task_get_status(task);
	DOCA_LOG_ERR("SHA hash task failed: %s", doca_error_get_descr(*result));

	/* Free task */
	doca_task_free(task);
	/* Decrement number of remaining tasks */
	--resources->num_remaining_tasks;
	/* Stop context once all tasks are completed */
	if (resources->num_remaining_tasks == 0)
		(void)doca_ctx_stop(resources->state.ctx);
}

/*
 * Check if given device is capable of executing a SHA hash task.
 *
 * @devinfo [in]: The DOCA device information
 * @return: DOCA_SUCCESS if the device supports SHA hash task and DOCA_ERROR otherwise.
 */
static doca_error_t sha_hash_is_supported(struct doca_devinfo *devinfo)
{
	return doca_sha_cap_task_hash_get_supported(devinfo, SHA_SAMPLE_ALGORITHM);
}

/**
 * Callback triggered whenever SHA context state changes
 *
 * @user_data [in]: User data associated with the SHA context. Will hold struct sha_resources *
 * @ctx [in]: The SHA context that had a state change
 * @prev_state [in]: Previous context state
 * @next_state [in]: Next context state (context is already in this state when the callback is called)
 */
static void sha_state_changed_callback(const union doca_data user_data,
				       struct doca_ctx *ctx,
				       enum doca_ctx_states prev_state,
				       enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct sha_resources *resources = (struct sha_resources *)user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		DOCA_LOG_INFO("SHA context has been stopped");
		/* We can stop progressing the PE */
		resources->run_pe_progress = false;
		break;
	case DOCA_CTX_STATE_STARTING:
		/**
		 * The context is in starting state, this is unexpected for SHA.
		 */
		DOCA_LOG_ERR("SHA context entered into starting state. Unexpected transition");
		break;
	case DOCA_CTX_STATE_RUNNING:
		DOCA_LOG_INFO("SHA context is running");
		break;
	case DOCA_CTX_STATE_STOPPING:
		/**
		 * doca_ctx_stop() has been called.
		 * In this sample, this happens either due to a failure encountered, in which case doca_pe_progress()
		 * will cause any inflight task to be flushed, or due to the successful compilation of the sample flow.
		 * In both cases, in this sample, doca_pe_progress() will eventually transition the context to idle
		 * state.
		 */
		DOCA_LOG_INFO("SHA context entered into stopping state. Any inflight tasks will be flushed");
		break;
	default:
		break;
	}
}

/*
 * Run sha_create sample
 *
 * @src_buffer [in]: source data for the SHA task
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t sha_create(char *src_buffer, int unit_size, int batch_size, int batch_num)
{
	struct sha_resources resources;
	struct program_core_objects *state = &resources.state;
	union doca_data ctx_user_data = {0};
	union doca_data task_user_data = {0};
	
	struct doca_buf **src_doca_buf = NULL;
	struct doca_buf **dst_doca_buf = NULL;
	src_doca_buf = (struct doca_buf **)malloc(sizeof(struct doca_buf *) * batch_size);
	dst_doca_buf = (struct doca_buf **)malloc(sizeof(struct doca_buf *) * batch_size);

	struct doca_sha_task_hash **sha_hash_task;
	sha_hash_task = (struct doca_sha_task_hash**)malloc(sizeof(struct doca_sha_task_hash*) * batch_size);
	struct doca_task **task;
	task = (struct doca_task**)malloc(sizeof(struct doca_task*) * batch_size);

	uint8_t *dst_buffer = NULL;
	uint8_t *dst_buffer_data = NULL;
	char *sha_output = NULL;
	uint32_t max_bufs = 2*batch_size; 
	uint32_t min_dst_sha_buffer_size;
	uint64_t max_source_buffer_size;
	size_t src_buffer_len = unit_size * batch_size;
	size_t hash_length;
	size_t i;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t result, task_result;

	memset(&resources, 0, sizeof(resources));

	/* Open DOCA device that supports SHA */
	result = open_doca_device_with_capabilities(&sha_hash_is_supported, &state->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to open DOCA device for SHA hash task: %s", doca_error_get_descr(result));
		return result;
	}

	/* Make sure that the source buffer size is less than the maximum size */
	// 这里说的可能是单个任务的最大buffer，先注释掉
	result = doca_sha_cap_get_max_src_buf_size(doca_dev_as_devinfo(state->dev), &max_source_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get maximum source buffer size for DOCA SHA: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}
	if (src_buffer_len > max_source_buffer_size) {
		DOCA_LOG_ERR("User data length %lu exceeds the maximum length %lu for DOCA SHA: %s",
			     src_buffer_len,
			     max_source_buffer_size,
			     doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	result = doca_sha_cap_get_min_dst_buf_size(doca_dev_as_devinfo(state->dev),
						   SHA_SAMPLE_ALGORITHM,
						   &min_dst_sha_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get minimum destination buffer size for DOCA SHA: %s",
			     doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	result = doca_sha_create(state->dev, &resources.sha_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create sha engine: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	state->ctx = doca_sha_as_ctx(resources.sha_ctx);
	result = create_core_objects(state, max_bufs);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&resources);
		return result;
	}

	/* Connect context to progress engine */
	result = doca_pe_connect_ctx(state->pe, state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect progress engine to context: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	int LOG_NUM = log(batch_size)/log(2);
	result = doca_sha_task_hash_set_conf(resources.sha_ctx,
					    sha_hash_completed_callback,
					    sha_hash_error_callback,
					    LOG_NUM);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set hash complete callback: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	result = doca_ctx_set_state_changed_cb(state->ctx, sha_state_changed_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set SHA state change callback: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	// SHA结果内存准备
	// int dest_buf_size = batch_size * SHA256_LEN;
	int dest_buf_size = min_dst_sha_buffer_size * batch_size;
	dst_buffer = calloc(1, dest_buf_size);
	if (dst_buffer == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		sha_cleanup(&resources);
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, dest_buf_size);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&resources);
		return result;
	}

	result = doca_mmap_set_free_cb(state->dst_mmap, &free_cb, NULL);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&resources);
		return result;
	}

	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&resources);
		return result;
	}

	// 源数据内存准备
	result = doca_mmap_set_memrange(state->src_mmap, src_buffer, src_buffer_len);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&resources);
		return result;
	}

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&resources);
		return result;
	}

	for(int i=0; i<=batch_size-1; i++){
		/* Construct DOCA source buffer */
		// src_buffer是一个大的区间，我们计算其中一段一段数据的SHA值，而不是整个buffer。
		result = doca_buf_inventory_buf_get_by_data(state->buf_inv,
								state->src_mmap,
								src_buffer + unit_size*i,
								unit_size,
								&src_doca_buf[i]);
		doca_buf_set_data(src_doca_buf[i], src_buffer + unit_size*i, unit_size);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
					doca_error_get_descr(result));
			return result;
		}

		/* Construct DOCA destination buffer */
		result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
								state->dst_mmap,
								dst_buffer + min_dst_sha_buffer_size*i,
								min_dst_sha_buffer_size,
								&dst_doca_buf[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
					doca_error_get_descr(result));
			return result;
		}
	}

	/* Include tasks counter in user data of context to be decremented in callbacks */
	resources.num_remaining_tasks = batch_size;
	resources.run_pe_progress = true;
	ctx_user_data.ptr = &resources;
	doca_ctx_set_user_data(state->ctx, ctx_user_data);

	/* Start the context */
	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start context: %s",
					doca_error_get_descr(result));
		return result;
	}
	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;

	for(int i=0; i<=batch_size-1; i++){
		/* Allocate and construct SHA hash task */
		result = doca_sha_task_hash_alloc_init(resources.sha_ctx,
					SHA_SAMPLE_ALGORITHM,
					src_doca_buf[i],
					dst_doca_buf[i],
					task_user_data,
					&sha_hash_task[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate SHA hash task: %s", doca_error_get_descr(result));
			return result;
		}

		/* Number of tasks submitted to progress engine */
		task[i] = doca_sha_task_hash_as_task(sha_hash_task[i]);
		if (task[i] == NULL) {
			result = DOCA_ERROR_UNEXPECTED;
			DOCA_LOG_ERR("Failed to get DOCA SHA hash task as DOCA task: %s", doca_error_get_descr(result));
			return result;
		}
	}

	struct timeval start_time, end_time;
	gettimeofday(&start_time, NULL);
	for(int k=0; k<=batch_num-1; k++){
		for(int i=0; i<=batch_size-1; i++){
			/* Submit SHA hash task */
			result = doca_task_submit(task[i]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to submit SHA hash task: %s", doca_error_get_descr(result));
				return result;
			}
		}

		while (resources.run_pe_progress) {
			if (doca_pe_progress(state->pe) == 0)
					//nanosleep(&ts, &ts);
				;
		}

		for(int i=0; i<=batch_size-1; i++){
			doca_buf_reset_data_len(dst_doca_buf[i]);
		}

		resources.num_remaining_tasks = batch_size;
		resources.run_pe_progress = true;
		
		DOCA_LOG_INFO("batch %d finished", k);
	}

	
	gettimeofday(&end_time, NULL);
	long long used_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);
	DOCA_LOG_INFO("sha_create() used time: %lld ms", used_time_us / 1000);
	DOCA_LOG_INFO("speed %.2f GB/s", 1.0 / ((float)(used_time_us) / 1000000));


	// result = task_result;

	// /* Check result of task according to the result we update in the callbacks */
	// if (task_result != DOCA_SUCCESS) {
	// 	return result;
	// }

	// /* Engine outputs hex format. For char format output, we need double the length */
	// sha_output = calloc(1, (min_dst_sha_buffer_size * 2) + 1);
	// if (sha_output == NULL) {
	// 	DOCA_LOG_ERR("Failed to allocate memory");
	// 	return DOCA_ERROR_NO_MEMORY;
	// }

	// for(int i=0; i<=batch_size-1; i++){
	// 	result = doca_buf_get_data_len(dst_doca_buf[i], &hash_length);
	// 	if (result != DOCA_SUCCESS) {
	// 		DOCA_LOG_ERR("Failed to get the data length of DOCA buffer: %s", doca_error_get_descr(result));
	// 		return result;
	// 	}

	// 	result = doca_buf_get_data(dst_doca_buf[i], (void **)&dst_buffer_data);
	// 	if (result != DOCA_SUCCESS) {
	// 		DOCA_LOG_ERR("Failed to get the data of DOCA buffer: %s", doca_error_get_descr(result));
	// 		return result;
	// 	}

	// 	/* Convert the hex format to char format */
	// 	for (int j = 0; j < hash_length; j++)
	// 		snprintf(sha_output + (2 * j), 3, "%02x", dst_buffer_data[j]);
	// 	DOCA_LOG_INFO("SHA256 output is: %s", sha_output);
	// }


	// /* Clean and destroy all relevant objects */
	// free(sha_output);
	// if (doca_buf_dec_refcount(src_doca_buf, NULL) != DOCA_SUCCESS ||
	//     doca_buf_dec_refcount(dst_doca_buf, NULL) != DOCA_SUCCESS)
	// 	DOCA_LOG_ERR("Failed to decrease DOCA buffer reference count");
	// sha_cleanup(&resources);

	return DOCA_SUCCESS;
	//return result;
}
