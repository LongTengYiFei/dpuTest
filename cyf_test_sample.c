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

#include <stdio.h>
#include <pthread.h>

DOCA_LOG_REGISTER(SHA_CREATE);

#define SLEEP_IN_NANOS (10 * 1000) /* Sample the job every 10 microseconds  */

int queue_working_number = 0;

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

typedef struct
{
	struct program_core_objects* state; 
	struct doca_sha *sha_ctx;
	struct doca_sha_job** sha_jobs; 
	struct doca_buf **src_doca_bufs;
	struct doca_buf **dst_doca_bufs;
	char* src_buffer;
	char* dst_buffer;
	int block_num;
	int block_size;
	int queue_depth;
}SubmitArg;

void submitThread(void* args){

	// 一次往队列里塞请求，中间可能会塞不进去，只能while等retreive线程取出；
	doca_error_t result;
	long timeuse_enqueue = 0;
	struct timeval start,end;  
	struct timespec ts;
	SubmitArg *sa = (SubmitArg*)args;

	for(int i=0; i<=sa->block_num-1; i++){
		/* queue number check*/
		while(queue_working_number>=sa->queue_depth){
			/* Wait for a job to be retrieved */
			ts.tv_sec = 0;
			ts.tv_nsec = SLEEP_IN_NANOS;
			nanosleep(&ts, &ts);
		}

		if(i<=sa->queue_depth-1){
			/* Source: Construct DOCA buffer for each address range */
			result = doca_buf_inventory_buf_by_addr(sa->state->buf_inv, sa->state->src_mmap, sa->src_buffer+sa->block_size*i, sa->block_size,
							&sa->src_doca_bufs[i%sa->queue_depth]);
			if (result == DOCA_ERROR_INVALID_VALUE) {
				DOCA_LOG_ERR("DOCA_ERROR_INVALID_VALUE");
			}else if(result == DOCA_ERROR_NO_MEMORY){
				DOCA_LOG_ERR("DOCA_ERROR_NO_MEMORY");
			}else if(result != DOCA_SUCCESS){
				DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s", doca_get_error_string(result));
				sha_cleanup(&sa->state, sa->sha_ctx);
				return result;
			}
		}
		
		result = doca_buf_set_data(sa->src_doca_bufs[i%sa->queue_depth], sa->src_buffer+sa->block_size*i, sa->block_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("doca_buf_set_data() for request doca_buf failure");
			doca_buf_refcount_rm(sa->src_doca_bufs[i%sa->queue_depth], NULL);
			sha_cleanup(&sa->state, sa->sha_ctx);
			return result;
		}

		if(i<=sa->queue_depth-1){
			/* Destination: Construct DOCA buffer for each address range */
			result = doca_buf_inventory_buf_by_addr(sa->state->buf_inv, sa->state->dst_mmap, sa->dst_buffer+DOCA_SHA512_BYTE_COUNT*(i%sa->queue_depth), DOCA_SHA512_BYTE_COUNT,
								&sa->dst_doca_bufs[i%sa->queue_depth]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s", doca_get_error_string(result));
				doca_buf_refcount_rm(sa->src_doca_bufs[i%sa->queue_depth], NULL);
				sha_cleanup(&sa->state, sa->sha_ctx);
				return result;
			}
		}

		sa->sha_jobs[i%sa->queue_depth]->resp_buf = sa->dst_doca_bufs[i%sa->queue_depth];
		sa->sha_jobs[i%sa->queue_depth]->req_buf = sa->src_doca_bufs[i%sa->queue_depth];

		/* Enqueue sha job */
		gettimeofday(&start, NULL );
		result = doca_workq_submit(sa->state->workq, &sa->sha_jobs[i%sa->queue_depth]->base);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit sha job: %s", doca_get_error_string(result));
			doca_buf_refcount_rm(sa->dst_doca_bufs[i%sa->queue_depth], NULL);
			doca_buf_refcount_rm(sa->src_doca_bufs[i%sa->queue_depth], NULL);
			sha_cleanup(sa->state, sa->sha_ctx);
			printf("submit %d job error\n");
			return ;
		}
		gettimeofday(&end, NULL );
		timeuse_enqueue+=  1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec; 

		queue_working_number++;
	}
	printf("Enqueue jobs time = %ld us\n", timeuse_enqueue); 
}

typedef struct
{
	struct program_core_objects* state; 
	struct doca_sha *sha_ctx;
	struct doca_sha_job** sha_jobs;
	int queue_depth;
	int block_num;
}RetrieveArg;

void retrieveThread(void* args){
	/* Wait for job completion */
	struct doca_event event = {0};
	struct timeval start,end;  
	doca_error_t result;
	long timeuse_retrieve = 0;  
	uint8_t *resp_head;
	struct timespec ts;
	RetrieveArg *ra = (RetrieveArg*)args;

	/* Engine outputs hex format. For char format output, we need double the length */
	char sha_output[DOCA_SHA512_BYTE_COUNT * 2 + 1] = {0};

	//这个函数最好稍微晚点比submitThread启动，否则工作数量刚开始等于0，初始submit无法完成；
	for(int i=0; i<=ra->block_num-1; i++){
		gettimeofday(&start, NULL );
		while ((result = doca_workq_progress_retrieve(ra->state->workq, &event, DOCA_WORKQ_RETRIEVE_FLAGS_NONE)) ==
	       DOCA_ERROR_AGAIN) {
			/* Wait for the job to complete */
			ts.tv_sec = 0;
			ts.tv_nsec = SLEEP_IN_NANOS;
			nanosleep(&ts, &ts);
		}
		gettimeofday(&end, NULL );  
		timeuse_retrieve += 1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec;
		
		queue_working_number--;

		if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to retrieve sha job: %s", doca_get_error_string(result));

		else if (event.result.u64 != DOCA_SUCCESS)
			DOCA_LOG_ERR("SHA job finished unsuccessfully");

		else if (((int)(event.type) != (int)DOCA_SHA_JOB_SHA512) ||
			(event.user_data.u64 != DOCA_SHA_JOB_SHA512))
			DOCA_LOG_ERR("Received wrong event");

		else {
			doca_buf_get_data(ra->sha_jobs[i%ra->queue_depth]->resp_buf, (void **)&resp_head);
			for (int j = 0; j < DOCA_SHA512_BYTE_COUNT; j++)
				snprintf(sha_output + (2 * j), 3, "%02x", resp_head[j]);
			DOCA_LOG_INFO("SHA512 output is: %s", sha_output);
		}

	}
	printf("Retrieve wait time = %ld us\n", timeuse_retrieve);  
}

/*
 * Run sha_create sample
 *
 * @src_buffer [in]: source data for the SHA job
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
void
sha_create_cyf(char *src_buffer, long long src_len, int block_size, int queue_depth)
{
	struct program_core_objects state = {0};
	struct doca_sha *sha_ctx;
	doca_error_t result;
	
	uint32_t workq_depth = queue_depth;	
	uint32_t max_bufs = 2*queue_depth;

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
	char* dst_buffer = (char*)malloc(sizeof(char)*queue_depth*DOCA_SHA512_BYTE_COUNT);
	result = doca_mmap_set_memrange(state.dst_mmap, dst_buffer, DOCA_SHA512_BYTE_COUNT*queue_depth);
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
	doca_mmap_set_permissions(state.dst_mmap, DOCA_ACCESS_LOCAL_READ_WRITE);

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
	doca_mmap_set_permissions(state.src_mmap, DOCA_ACCESS_LOCAL_READ_WRITE);

	// descriptor init
	struct doca_buf **src_doca_bufs;
	struct doca_buf **dst_doca_bufs;
	src_doca_bufs = (struct doca_buf **)malloc(sizeof(struct doca_buf *)*queue_depth);
	dst_doca_bufs = (struct doca_buf **)malloc(sizeof(struct doca_buf *)*queue_depth);

	int block_num = src_len / block_size;

	/* Construct sha jobs */
	struct doca_sha_job** sha_jobs = (struct doca_sha_job**)malloc(sizeof(struct doca_sha_job*)*queue_depth);
	for(int i=0; i<=queue_depth-1; i++){
		sha_jobs[i] = (struct doca_sha_job*)malloc(sizeof(struct doca_sha_job));
		sha_jobs[i]->flags = DOCA_SHA_JOB_FLAGS_NONE;
		sha_jobs[i]->base = (struct doca_job) {
				.type = DOCA_SHA_JOB_SHA512,
				.flags = DOCA_JOB_FLAGS_NONE,
				.ctx = state.ctx,
				.user_data.u64 = DOCA_SHA_JOB_SHA512,
				};
	}

	int err;
	pthread_t thread_submit, thread_retrieve;
	SubmitArg sa = (SubmitArg)
	{
		.state = &state,
		.sha_ctx = sha_ctx,
		.sha_jobs = sha_jobs, 
		.src_doca_bufs = src_doca_bufs,
		.dst_doca_bufs = dst_doca_bufs,
		.src_buffer = src_buffer,
		.dst_buffer = dst_buffer,
		.block_num = block_num,
		.block_size = block_size,
		.queue_depth = queue_depth,
	};

	RetrieveArg ra = (RetrieveArg)
	{
		.state = &state, 
		.sha_ctx = sha_ctx,
		.sha_jobs = sha_jobs, 
		.queue_depth = queue_depth,
		.block_num = block_num,
	};
	
	if((err = pthread_create(&thread_submit, NULL, submitThread, (void*)&sa))!=0)
	{
		perror("pthread_create error");
	}
 
	if((err = pthread_create(&thread_retrieve, NULL, retrieveThread, (void*)&ra))!=0)
	{
		perror("pthread_create error");
	}

	printf("Start two thread\n");
 
	pthread_join(thread_submit, NULL);
	pthread_join(thread_retrieve, NULL);

	return ;
}


