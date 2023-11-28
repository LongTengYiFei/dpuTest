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
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_sha.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"

DOCA_LOG_REGISTER(SHA_CREATE);

#define SLEEP_IN_NANOS (10 * 1000) /* Sample the job every 10 microseconds  */

/*
 * Free callback - free doca_buf allocated pointer
 *
 * @addr [in]: Memory range pointer
 * @len [in]: Memory range length
 * @opaque [in]: An opaque pointer passed to iterator
 */
void
free_cb(void *addr, size_t len, void *opaque)
{
	(void)len;
	(void)opaque;

	free(addr);
}

/*
 * Clean all the sample resources
 *
 * @state [in]: program_core_objects struct
 * @sha_ctx [in]: SHA context
 */
static void
sha_cleanup(struct program_core_objects *state, struct doca_sha *sha_ctx)
{
	doca_error_t result;

	destroy_core_objects(state);

	result = doca_sha_destroy(sha_ctx);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy sha: %s", doca_get_error_string(result));
}

/**
 * Check if given device is capable of executing a DOCA_SHA_JOB_SHA512 job with HW.
 *
 * @devinfo [in]: The DOCA device information
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t
job_sha_hardware_is_supported(struct doca_devinfo *devinfo)
{
	doca_error_t result;

	result = doca_sha_job_get_supported(devinfo, DOCA_SHA_JOB_SHA512);
	if (result != DOCA_SUCCESS)
		return result;
	return doca_sha_get_hardware_supported(devinfo);
}

/**
 * Check if given device is capable of executing a DOCA_SHA_JOB_SHA512 job with SW.
 *
 * @devinfo [in]: The DOCA device information
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t
job_sha_software_is_supported(struct doca_devinfo *devinfo)
{
	return doca_sha_job_get_supported(devinfo, DOCA_SHA_JOB_SHA512);
}

/*
 * Run sha_create sample
 *
 * @src_buffer [in]: source data for the SHA job
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t
sha_create_cyf(char *src_buffer, long long src_len, int job_num)
{
	struct program_core_objects state = {0};
	struct doca_sha *sha_ctx;
	doca_error_t result;
	struct timespec ts;
	uint32_t workq_depth = job_num;	
	uint32_t max_bufs = 2*job_num;
	
	int i;

	/* Engine outputs hex format. For char format output, we need double the length */
	char sha_output[DOCA_SHA512_BYTE_COUNT * 2 + 1] = {0};
	uint8_t *resp_head;

	result = doca_sha_create(&sha_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create sha engine: %s", doca_get_error_string(result));
		return result;
	}

	state.ctx = doca_sha_as_ctx(sha_ctx);

	result = open_doca_device_with_capabilities(&job_sha_hardware_is_supported, &state.dev);
	if (result != DOCA_SUCCESS) {
		result = open_doca_device_with_capabilities(&job_sha_software_is_supported, &state.dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to find device for SHA job");
			result = doca_sha_destroy(sha_ctx);
			return result;
		}
		DOCA_LOG_WARN("SHA engine is not enabled, using openssl instead");
	}


	result = init_core_objects(&state, workq_depth, max_bufs);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&state, sha_ctx);
		return result;
	}

	// destination memory (sha result) init
	char* dst_buffer = (char*)malloc(sizeof(char)*job_num*DOCA_SHA512_BYTE_COUNT);
	result = doca_mmap_set_memrange(state.dst_mmap, dst_buffer, DOCA_SHA512_BYTE_COUNT*job_num);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&state, sha_ctx);
		return result;
	}
	
	result = doca_mmap_set_free_cb(state.dst_mmap, &free_cb, NULL);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&state, sha_ctx);
		return result;
	}
	
	result = doca_mmap_start(state.dst_mmap);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&state, sha_ctx);
		return result;
	}
	//doca_mmap_set_permissions(state.dst_mmap, DOCA_ACCESS_LOCAL_READ_WRITE);

	// source memory init
	result = doca_mmap_set_memrange(state.src_mmap, src_buffer, src_len);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&state, sha_ctx);
		return result;
	}
	result = doca_mmap_start(state.src_mmap);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&state, sha_ctx);
		return result;
	}
	//doca_mmap_set_permissions(state.src_mmap, DOCA_ACCESS_LOCAL_READ_WRITE);

	// descriptor init
	struct doca_buf **src_doca_bufs;
	struct doca_buf **dst_doca_bufs;
	src_doca_bufs = (struct doca_buf **)malloc(sizeof(struct doca_buf *)*job_num);
	dst_doca_bufs = (struct doca_buf **)malloc(sizeof(struct doca_buf *)*job_num);

	int segment_len = src_len / job_num;
	for(int i=0; i<=job_num-1; i++){
		/* Source: Construct DOCA buffer for each address range */
		result = doca_buf_inventory_buf_by_addr(state.buf_inv, state.src_mmap, src_buffer+segment_len*i, segment_len,
						&src_doca_bufs[i]);
		if (result == DOCA_ERROR_INVALID_VALUE) {
			DOCA_LOG_ERR("DOCA_ERROR_INVALID_VALUE");
		}else if(result == DOCA_ERROR_NO_MEMORY){
			DOCA_LOG_ERR("DOCA_ERROR_NO_MEMORY");
		}else if(result != DOCA_SUCCESS){
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s", doca_get_error_string(result));
			sha_cleanup(&state, sha_ctx);
			return result;
		}
		/* Source: Set data address and length in the doca_buf */
		result = doca_buf_set_data(src_doca_bufs[i], src_buffer+segment_len*i, segment_len);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("doca_buf_set_data() for request doca_buf failure");
			doca_buf_refcount_rm(src_doca_bufs[i], NULL);
			sha_cleanup(&state, sha_ctx);
			return result;
		}

		/* Destination: Construct DOCA buffer for each address range */
		result = doca_buf_inventory_buf_by_addr(state.buf_inv, state.dst_mmap, dst_buffer+DOCA_SHA512_BYTE_COUNT*i, DOCA_SHA512_BYTE_COUNT,
							&dst_doca_bufs[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s", doca_get_error_string(result));
			doca_buf_refcount_rm(src_doca_bufs[i], NULL);
			sha_cleanup(&state, sha_ctx);
			return result;
		}
	}

	/* Construct sha jobs */
	struct timeval start1,end1;  
	long timeuse_enqueue = 0;
	struct doca_sha_job** sha_jobs = (struct doca_sha_job**)malloc(sizeof(struct doca_sha_job*));
	for(int i=0; i<=job_num-1; i++){
		sha_jobs[i] = (struct doca_sha_job*)malloc(sizeof(struct doca_sha_job));
		sha_jobs[i]->resp_buf = dst_doca_bufs[i];
		sha_jobs[i]->req_buf = src_doca_bufs[i];
		sha_jobs[i]->flags = DOCA_SHA_JOB_FLAGS_NONE;
		sha_jobs[i]->base = (struct doca_job) {
				.type = DOCA_SHA_JOB_SHA512,
				.flags = DOCA_JOB_FLAGS_NONE,
				.ctx = state.ctx,
				.user_data.u64 = DOCA_SHA_JOB_SHA512,
				},

		/* Enqueue sha job */
		gettimeofday(&start1, NULL );
		result = doca_workq_submit(state.workq, &sha_jobs[i]->base);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit sha job: %s", doca_get_error_string(result));
			doca_buf_refcount_rm(dst_doca_bufs[i], NULL);
			doca_buf_refcount_rm(src_doca_bufs[i], NULL);
			sha_cleanup(&state, sha_ctx);
			return result;
		}
		gettimeofday(&end1, NULL );
		timeuse_enqueue+=  1000000 * ( end1.tv_sec - start1.tv_sec ) + end1.tv_usec - start1.tv_usec;  
	}  
	printf("Enqueue jobs time = %ld us\n"); 

	/* Wait for job completion */
	struct doca_event* events = (struct doca_event*)calloc(job_num, sizeof(struct doca_event));
	struct timeval start,end;  
	gettimeofday(&start, NULL );  
	for(int i=0; i<=job_num-1; i++){
		while ((result = doca_workq_progress_retrieve(state.workq, &events[i], DOCA_WORKQ_RETRIEVE_FLAGS_NONE)) ==
	       DOCA_ERROR_AGAIN) {
			/* Wait for the job to complete */
			ts.tv_sec = 0;
			ts.tv_nsec = SLEEP_IN_NANOS;
			nanosleep(&ts, &ts);
		}
		
		if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to retrieve sha job: %s", doca_get_error_string(result));

		else if (events[i].result.u64 != DOCA_SUCCESS)
			DOCA_LOG_ERR("SHA job finished unsuccessfully");

		else if (((int)(events[i].type) != (int)DOCA_SHA_JOB_SHA512) ||
			(events[i].user_data.u64 != DOCA_SHA_JOB_SHA512))
			DOCA_LOG_ERR("Received wrong event");

		else {
			doca_buf_get_data(sha_jobs[i]->resp_buf, (void **)&resp_head);
			for (int j = 0; j < DOCA_SHA512_BYTE_COUNT; j++)
				snprintf(sha_output + (2 * j), 3, "%02x", resp_head[j]);
			DOCA_LOG_INFO("SHA512 output of %s is: %s", src_buffer, sha_output);
		}

		if (doca_buf_refcount_rm(src_doca_bufs[i], NULL) != DOCA_SUCCESS ||
			doca_buf_refcount_rm(dst_doca_bufs[i], NULL) != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to decrease DOCA buffer reference count");
	}
	gettimeofday(&end, NULL );  
	long timeuse = 1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec;  
	printf("retrieve wait time = %ld us\n", timeuse);  

	/* Clean and destroy all relevant objects */
	sha_cleanup(&state, sha_ctx);
	return result;
}
