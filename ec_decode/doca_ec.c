/*
 * Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
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
#define _POSIX_C_SOURCE 199309L
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <utils.h>

#include "common.h"
// #include <jerasure.h>
// #include <jerasure/reed_sol.h>
// #include <jerasure/cauchy.h>
// #include "doca_common.h"

#include <sys/time.h>
 #include <fcntl.h>

DOCA_LOG_REGISTER(EC_RECOVER);

#define SLEEP_IN_NANOS (10 * 1000) /* sample the task every 10 microseconds  */
/* assert function - if fails print error, clean up(state - ec_sample_objects) and exit  */
#define SAMPLE_ASSERT(condition, result, state, error...) \
	do { \
		if (!(condition)) { \
			DOCA_LOG_ERR(error); \
			ec_cleanup(state); \
			return result; \
		} \
	} while (0)
/* callback assert function - if fails print error, update the callback_result parameter and exit  */
#define CB_ASSERT(condition, result, cb_result, error...) \
	do { \
		if (!(condition)) { \
			DOCA_LOG_ERR(error); \
			*(cb_result) = (result); \
			goto free_task; \
		} \
	} while (0)
/* assert function - same as before just for doca error  */
#define ASSERT_DOCA_ERR(result, state, error) \
	SAMPLE_ASSERT((result) == DOCA_SUCCESS, (result), state, (error ": %s"), doca_error_get_descr(result))

#define NUM_EC_TASKS (8)		       /* EC tasks number */
#define USER_MAX_PATH_NAME 255		       /* Max file name length */
#define MAX_PATH_NAME (USER_MAX_PATH_NAME + 1) /* Max file name string length */
#define MAX_DATA_SIZE (MAX_PATH_NAME + 100)    /* Max data file length - path + max int string size */
#define RECOVERED_FILE_NAME "_recovered"       /* Recovered file extension (if file name not given) */
#define DATA_INFO_FILE_NAME "data_info"	       /* Data information file name - i.e. size & name of original file */
#define DATA_BLOCK_FILE_NAME "data_block_"     /* Data blocks file name (attached index at the end) */
#define RDNC_BLOCK_FILE_NAME "rdnc_block_"     /* Redundancy blocks file name (attached index at the end) */

struct ec_sample_objects {
	struct doca_buf *src_doca_buf;		/* Source doca buffer as input for the task */
	struct doca_buf *dst_doca_buf;		/* Destination doca buffer as input for the task */
	struct doca_ec *ec;			/* DOCA Erasure coding context */
	char *src_buffer;			/* Source memory region to be used as input for the task */
	char *dst_buffer;			/* Destination memory region to be used as output for task results */
	char* app_dst_buffer;			/* 任务结束时，需要从doca dst buffer拷贝至app指定内存 */
	char *file_data;			/* Block data pointer from reading block file */
	char *block_file_data;			/* Block data pointer from reading block file */
	uint32_t *missing_indices;		/* Data indices to that are missing and need recover */
	FILE *out_file;				/* Recovered file pointer to write to */
	FILE *block_file;			/* Block file pointer to write to */
	struct doca_ec_matrix *encoding_matrix; /* Encoding matrix that will be use to create the redundancy */
	struct doca_ec_matrix *decoding_matrix; /* Decoding matrix that will be use to recover the data */
	struct program_core_objects core_state; /* DOCA core objects - please refer to struct program_core_objects */
	bool run_pe_progress;			/* Controls whether progress loop should run */
	int num_remaining_tasks;
};

/*
 * Clean all the sample resources
 *
 * @state [in]: ec_sample_objects struct
 * @ec [in]: ec context
 */
static void ec_cleanup(struct ec_sample_objects *state)
{
	doca_error_t result = DOCA_SUCCESS;

	if (state->src_doca_buf != NULL) {
		result = doca_buf_dec_refcount(state->src_doca_buf, NULL);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to decrease DOCA buffer reference count: %s",
				     doca_error_get_descr(result));
	}
	if (state->dst_doca_buf != NULL) {
		result = doca_buf_dec_refcount(state->dst_doca_buf, NULL);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to decrease DOCA buffer reference count: %s",
				     doca_error_get_descr(result));
	}

	if (state->missing_indices != NULL)
		free(state->missing_indices);
	if (state->block_file_data != NULL)
		free(state->block_file_data);
	if (state->file_data != NULL)
		free(state->file_data);
	if (state->src_buffer != NULL)
		free(state->src_buffer);
	if (state->dst_buffer != NULL)
		free(state->dst_buffer);
	if (state->out_file != NULL)
		fclose(state->out_file);
	if (state->block_file != NULL)
		fclose(state->block_file);
	if (state->encoding_matrix != NULL) {
		result = doca_ec_matrix_destroy(state->encoding_matrix);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy ec encoding matrix: %s", doca_error_get_descr(result));
	}
	if (state->decoding_matrix != NULL) {
		result = doca_ec_matrix_destroy(state->decoding_matrix);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy ec decoding matrix: %s", doca_error_get_descr(result));
	}

	if (state->core_state.ctx != NULL) {
		result = doca_ctx_stop(state->core_state.ctx);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Unable to stop context: %s", doca_error_get_descr(result));
		state->core_state.ctx = NULL;
	}
	if (state->ec != NULL) {
		result = doca_ec_destroy(state->ec);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy ec: %s", doca_error_get_descr(result));
	}

	destroy_core_objects(&state->core_state);
}

/**
 * Callback triggered whenever Erasure Coding context state changes
 *
 * @user_data [in]: User data associated with the Erasure Coding context. Will hold struct ec_sample_objects *
 * @ctx [in]: The Erasure Coding context that had a state change
 * @prev_state [in]: Previous context state
 * @next_state [in]: Next context state (context is already in this state when the callback is called)
 */
static void ec_state_changed_callback(const union doca_data user_data,
				      struct doca_ctx *ctx,
				      enum doca_ctx_states prev_state,
				      enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct ec_sample_objects *state = (struct ec_sample_objects *)user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		DOCA_LOG_INFO("Erasure Coding context has been stopped");
		/* We can stop progressing the PE */
		state->run_pe_progress = false;
		break;
	case DOCA_CTX_STATE_STARTING:
		/**
		 * The context is in starting state, this is unexpected for Erasure Coding.
		 */
		DOCA_LOG_ERR("Erasure Coding context entered into starting state. Unexpected transition");
		break;
	case DOCA_CTX_STATE_RUNNING:
		DOCA_LOG_INFO("Erasure Coding context is running");
		break;
	case DOCA_CTX_STATE_STOPPING:
		/**
		 * doca_ctx_stop() has been called.
		 * In this sample, this happens either due to a failure encountered, in which case doca_pe_progress()
		 * will cause any inflight task to be flushed, or due to the successful compilation of the sample flow.
		 * In both cases, in this sample, doca_pe_progress() will eventually transition the context to idle
		 * state.
		 */
		DOCA_LOG_INFO("Erasure Coding context entered into stopping state. Any inflight tasks will be flushed");
		break;
	default:
		break;
	}
}

/**
 * Init ec core objects.
 *
 * @state [in]: The DOCA EC sample state
 * @pci_addr [in]: The PCI address of a doca device
 * @is_support_func [in]: Function that pci device should support
 * @max_bufs [in]: The buffer count to create
 * @src_size [in]: The source data size (to create the buffer)
 * @dst_size [in]: The destination data size (to create the buffer)
 * @max_block_size [out]: The maximum block size supported for ec operations
 * @return: DOCA_SUCCESS if the core init successfully and DOCA_ERROR otherwise.
 */
static doca_error_t ec_core_init(struct ec_sample_objects *state,
				 const char *pci_addr,
				 tasks_check is_support_func,
				 uint32_t max_bufs,
				 uint32_t src_size,
				 uint32_t dst_size,
				 uint64_t *max_block_size)
{
	doca_error_t result;
	union doca_data ctx_user_data;

	result = open_doca_device_with_pci(pci_addr, is_support_func, &state->core_state.dev);
	ASSERT_DOCA_ERR(result, state, "Unable to open the pci device");

	result = create_core_objects(&state->core_state, max_bufs);
	ASSERT_DOCA_ERR(result, state, "Failed to init core");

	result = doca_ec_create(state->core_state.dev, &state->ec);
	ASSERT_DOCA_ERR(result, state, "Unable to create ec engine");

	result = doca_ec_cap_get_max_block_size(doca_dev_as_devinfo(state->core_state.dev), max_block_size);
	ASSERT_DOCA_ERR(result, state, "Unable to query maximum block size supported");

	state->core_state.ctx = doca_ec_as_ctx(state->ec);
	SAMPLE_ASSERT(state->core_state.ctx != NULL, DOCA_ERROR_UNEXPECTED, state, "Unable to retrieve ctx");

	result = doca_pe_connect_ctx(state->core_state.pe, state->core_state.ctx);
	ASSERT_DOCA_ERR(result, state, "Unable to connect context to progress engine");

	result = doca_mmap_set_memrange(state->core_state.dst_mmap, state->dst_buffer, dst_size);
	ASSERT_DOCA_ERR(result, state, "Failed to set mmap mem range dst");

	result = doca_mmap_start(state->core_state.dst_mmap);
	ASSERT_DOCA_ERR(result, state, "Failed to start mmap dst");

	result = doca_mmap_set_memrange(state->core_state.src_mmap, state->src_buffer, src_size);
	ASSERT_DOCA_ERR(result, state, "Failed to set mmap mem range src");

	result = doca_mmap_start(state->core_state.src_mmap);
	ASSERT_DOCA_ERR(result, state, "Failed to start mmap src");

	/* Construct DOCA buffer for each address range */
	result = doca_buf_inventory_buf_get_by_addr(state->core_state.buf_inv,
						    state->core_state.src_mmap,
						    state->src_buffer,
						    src_size,
						    &state->src_doca_buf);
	ASSERT_DOCA_ERR(result, state, "Unable to acquire DOCA buffer representing source buffer");

	/* Construct DOCA buffer for each address range */
	result = doca_buf_inventory_buf_get_by_addr(state->core_state.buf_inv,
						    state->core_state.dst_mmap,
						    state->dst_buffer,
						    dst_size,
						    &state->dst_doca_buf);
	ASSERT_DOCA_ERR(result, state, "Unable to acquire DOCA buffer representing destination buffer");

	/* Setting data length in doca buffer */
	result = doca_buf_set_data(state->src_doca_buf, state->src_buffer, src_size);
	ASSERT_DOCA_ERR(result, state, "Unable to set DOCA buffer data");

	/* Include state in user data of context to be used in callbacks */
	ctx_user_data.ptr = state;
	result = doca_ctx_set_user_data(state->core_state.ctx, ctx_user_data);
	ASSERT_DOCA_ERR(result, state, "Unable to set user data to context");

	/* Set state change callback to be called whenever the context state changes */
	result = doca_ctx_set_state_changed_cb(state->core_state.ctx, ec_state_changed_callback);
	ASSERT_DOCA_ERR(result, state, "Unable to set state change callback");

	return DOCA_SUCCESS;
}

/*
 * EC tasks mutual error callback
 *
 * @task [in]: the failed doca task
 * @task_status [out]: the status of the task
 * @cb_result [out]: the result of the callback
 */
static void ec_task_error(struct doca_task *task, doca_error_t *task_status, doca_error_t *cb_result)
{
	*task_status = DOCA_ERROR_UNEXPECTED;

	DOCA_LOG_ERR("EC Task finished unsuccessfully");

	/* Free task */
	doca_task_free(task);

	*cb_result = DOCA_SUCCESS;

	/* Stop context once task is completed */
	(void)doca_ctx_stop(doca_task_get_ctx(task));
}

/*
 * All the necessary variables for EC create task callback functions defined in this sample
 */
struct create_task_data {
	const char *output_dir_path;  /* The path in which the output file should be saved */
	uint32_t block_size;	      /* The block size used for EC */
	size_t rdnc_block_count;      /* The number of redundancy blocks created for the data */
	struct doca_buf *rdnc_blocks; /* The redundancy blocks created for the data */
	doca_error_t *task_status;    /* The status of the task (output parameter) */
	doca_error_t *cb_result;      /* The result of the callback (output parameter) */
};

/*
 * EC create task error callback
 *
 * @create_task [in]: the failed create task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void ec_create_error_callback(struct doca_ec_task_create *create_task,
				     union doca_data task_user_data,
				     union doca_data ctx_user_data)
{
	struct create_task_data *task_data = task_user_data.ptr;
	(void)ctx_user_data;

	ec_task_error(doca_ec_task_create_as_task(create_task), task_data->task_status, task_data->cb_result);
}

/*
 * EC create task completed callback
 *
 * @create_task [in]: the completed create task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void ec_create_completed_callback(struct doca_ec_task_create *create_task,
					 union doca_data task_user_data,
					 union doca_data ctx_user_data)
{
	struct ec_sample_objects *state = (struct ec_sample_objects *)ctx_user_data.ptr;
    --state->num_remaining_tasks;
    if(state->num_remaining_tasks == 0)
	    state->run_pe_progress = false;
	
	
}

void writeRandomData(char* buf, size_t len) {
	int i;
	for (i = 0; i < len; i++) {
		buf[i] = rand();
	}
}

/*
 * Run ec encode
 *
 * @pci_addr [in]: PCI address of a doca device
 * @file_path [in]: file data for the ec task
 * @matrix_type [in]: matrix type
 * @output_dir_path [in]: path to the task output file
 * @data_block_count [in]: data block count
 * @rdnc_block_count [in]: redundancy block count
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
doca_error_t encode(int k, int m, int block_size)
{
	// 统计信息
	struct timeval start_time, end_time;
	uint64_t matrix_create_time_us = 0;
	uint64_t doca_pure_encode_time = 0;
	uint64_t memcpy_time = 0;
	uint64_t copy_src_time_us = 0;
	uint64_t copy_dst_time_us = 0;

	uint32_t max_bufs = 2;
	doca_error_t result;
	uint64_t max_block_size;
	uint64_t src_size;
	uint64_t dst_size;
	struct ec_sample_objects state_object = {0};
	struct ec_sample_objects *state = &state_object;

	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

	doca_error_t task_result = DOCA_SUCCESS;
	doca_error_t callback_result = DOCA_SUCCESS;
	struct doca_task *doca_task;
	struct doca_ec_task_create *task;
	struct create_task_data task_data;

	int data_block_count = k;
	int rdnc_block_count = m;
	struct doca_buf **src_doca_bufs;		/* Source doca buffer as input for the task */
	// 即使是一个stripe也离散开，不放在一起
	src_doca_bufs = (struct doca_buf **)malloc(data_block_count*sizeof(struct doca_buf*));

	int align_size = 64*1024;
	src_size = (uint64_t)block_size * data_block_count;
	dst_size = (uint64_t)block_size * rdnc_block_count;
	char* pci_addr = "b1:00.0";

	posix_memalign(&state->src_buffer, align_size, src_size);

	// 根据条带大小注入适量的随机数据
	// 比如4+2，我只需要读4M就行了，如果我模拟编码128MB数据，那么我循环32次encode即可。
	writeRandomData(state->src_buffer, src_size);

	posix_memalign(&state->dst_buffer, align_size, dst_size);
	SAMPLE_ASSERT(state->dst_buffer != NULL, DOCA_ERROR_NO_MEMORY, state, "Unable to allocate dst_buffer string");

	union doca_data task_user_data;
	union doca_data ctx_user_data;

	result = open_doca_device_with_pci(pci_addr, (tasks_check)&doca_ec_cap_task_create_is_supported, &state->core_state.dev);
	ASSERT_DOCA_ERR(result, state, "Unable to open the pci device");

	result = create_core_objects(&state->core_state, data_block_count+1);
	ASSERT_DOCA_ERR(result, state, "Failed to init core");

	result = doca_ec_create(state->core_state.dev, &state->ec);
	ASSERT_DOCA_ERR(result, state, "Unable to create ec engine");

	result = doca_ec_cap_get_max_block_size(doca_dev_as_devinfo(state->core_state.dev), &max_block_size);
	ASSERT_DOCA_ERR(result, state, "Unable to query maximum block size supported");

	state->core_state.ctx = doca_ec_as_ctx(state->ec);
	SAMPLE_ASSERT(state->core_state.ctx != NULL, DOCA_ERROR_UNEXPECTED, state, "Unable to retrieve ctx");

	result = doca_pe_connect_ctx(state->core_state.pe, state->core_state.ctx);
	ASSERT_DOCA_ERR(result, state, "Unable to connect context to progress engine");

	result = doca_mmap_set_memrange(state->core_state.dst_mmap, state->dst_buffer, dst_size);
	ASSERT_DOCA_ERR(result, state, "Failed to set mmap mem range dst");

	result = doca_mmap_start(state->core_state.dst_mmap);
	ASSERT_DOCA_ERR(result, state, "Failed to start mmap dst");

	result = doca_mmap_set_memrange(state->core_state.src_mmap, state->src_buffer, src_size);
	ASSERT_DOCA_ERR(result, state, "Failed to set mmap mem range src");

	result = doca_mmap_start(state->core_state.src_mmap);
	ASSERT_DOCA_ERR(result, state, "Failed to start mmap src");

	for(int i=0; i<=data_block_count-1; i++){
		result = doca_buf_inventory_buf_get_by_addr(state->core_state.buf_inv,
								state->core_state.src_mmap,
								state->src_buffer+i*block_size,
								block_size,
								&src_doca_bufs[i]);

		// 每个block分别set
		result = doca_buf_set_data(src_doca_bufs[i], state->src_buffer+i*block_size, block_size);
	}

	result = doca_buf_inventory_buf_get_by_addr(state->core_state.buf_inv,
							state->core_state.dst_mmap,
							state->dst_buffer,
							dst_size,
							&state->dst_doca_buf);

	for (int i = 1; i<=data_block_count-1; i++) {
		result = doca_buf_chain_list(src_doca_bufs[0], src_doca_bufs[i]);
	}

	ctx_user_data.ptr = state;
	result = doca_ctx_set_user_data(state->core_state.ctx, ctx_user_data);

	result = doca_ctx_set_state_changed_cb(state->core_state.ctx, ec_state_changed_callback);

	result = doca_ec_task_create_set_conf(state->ec,
					      ec_create_completed_callback,
					      ec_create_error_callback,
					      1);

	result = doca_ctx_start(state->core_state.ctx);

	gettimeofday(&start_time, 0);
	result = doca_ec_matrix_create(state->ec,
				       DOCA_EC_MATRIX_TYPE_VANDERMONDE,
				       data_block_count,
				       rdnc_block_count,
				       &state->encoding_matrix);
	gettimeofday(&end_time, 0);
	matrix_create_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        end_time.tv_usec - start_time.tv_usec;
	
	ASSERT_DOCA_ERR(result, state, "Unable to create ec matrix");

	SAMPLE_ASSERT(
		block_size <= max_block_size,
		DOCA_ERROR_INVALID_VALUE,
		state,
		"Block size (%lu) exceeds the maximum size supported (%lu). Try to increase the number of blocks or use a smaller file as input",
		block_size,
		max_block_size);

	task_user_data.ptr = &task_result;

	result = doca_ec_task_create_allocate_init(state->ec,
						   state->encoding_matrix,
						   src_doca_bufs[0],
						   state->dst_doca_buf,
						   task_user_data,
						   &task);
	ASSERT_DOCA_ERR(result, state, "Unable to allocate and initiate task");

	doca_task = doca_ec_task_create_as_task(task);
	SAMPLE_ASSERT(doca_task != NULL, DOCA_ERROR_UNEXPECTED, state, "Unable to retrieve task as doca_task");

	gettimeofday(&start_time, 0);
	result = doca_task_submit(doca_task);
	ASSERT_DOCA_ERR(result, state, "Unable to submit task");

	state->run_pe_progress = true;
	state->num_remaining_tasks = 1;
	while (state->run_pe_progress){
		if (doca_pe_progress(state->core_state.pe) == 0)
			nanosleep(&ts, &ts);
	}
	gettimeofday(&end_time, 0);
	doca_pure_encode_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        end_time.tv_usec - start_time.tv_usec;

	int len;
	doca_buf_get_data_len(state->dst_doca_buf, &len);
	printf("dst len %d\n", len);
	result = doca_buf_get_data(state->dst_doca_buf, (void **)&state->dst_buffer);
		
	// 打印统计信息
	printf("matrix create time %d us\n", matrix_create_time_us);
	printf("raw encode time %d us\n", doca_pure_encode_time);
	printf("raw encode time %d us\n", doca_pure_encode_time);

	return callback_result;
}

/*
 * All the necessary variables for EC recover task callback functions defined in this sample
 */
struct recover_task_data {
	const char *dir_path;			/* The path to the tasks output file */
	char *output_file_path;			/* The path of the recovered file */
	int64_t file_size;			/* The size of the input file */
	int32_t block_size;			/* The block size used for EC */
	uint32_t data_block_count;		/* The number of data blocks created */
	size_t n_missing;			/* The number of missing data blocks that are to be recovered on success
						 */
	struct doca_buf *recovered_data_blocks; /* The buffer to which the blocks of recovered data will be written on
						 * success
						 */
	doca_error_t *task_status;		/* The status of the task (output parameter) */
	doca_error_t *cb_result;		/* The result of the callback (output parameter) */
};

struct recover_task_user_data {
	struct ec_sample_objects *state;
	int batch_index;
	int erasures_count;
	int block_size;
	int dst_size_seg;
	char *dst_buffer;
	char* app_dst_buffer;
};

/*
 * EC recover task error callback
 *
 * @recover_task [in]: the failed recover task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void ec_recover_error_callback(struct doca_ec_task_recover *recover_task,
				      union doca_data task_user_data,
				      union doca_data ctx_user_data)
{
	struct recover_task_data *task_data = task_user_data.ptr;
	(void)ctx_user_data;

	ec_task_error(doca_ec_task_recover_as_task(recover_task), task_data->task_status, task_data->cb_result);

	printf("ec recover error callback, rest task num %d\n", ((struct ec_sample_objects *)ctx_user_data.ptr)->num_remaining_tasks);
}

/*
 * EC recover task completed callback
 *
 * @recover_task [in]: the completed recover task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
static void ec_recover_completed_callback(struct doca_ec_task_recover *recover_task,
					  union doca_data task_user_data,
					  union doca_data ctx_user_data)
{
	struct ec_sample_objects *state = (struct ec_sample_objects *)ctx_user_data.ptr;
	--state->num_remaining_tasks;
	if (state->num_remaining_tasks == 0)
		state->run_pe_progress = false;

	printf("ec recover complete callback, rest task num %d\n", state->num_remaining_tasks);
}

static void ec_recover_completed_callback_memcpy(struct doca_ec_task_recover *recover_task,
					  union doca_data task_user_data,
					  union doca_data ctx_user_data)
{
	struct ec_sample_objects *state = (struct ec_sample_objects *)ctx_user_data.ptr;
	struct recover_task_user_data *task_data = (struct recover_task_user_data *)task_user_data.ptr;

	// copy result data to app dst buffer
	if (task_data != NULL && task_data->state != NULL) {
		char* app_dst = task_data->app_dst_buffer + (size_t)task_data->batch_index * (size_t)task_data->dst_size_seg;
		char *doca_dst = task_data->dst_buffer + (size_t)task_data->batch_index * (size_t)task_data->dst_size_seg;
		memcpy(app_dst, doca_dst, (size_t)task_data->dst_size_seg);
		//free(task_data);
	}

	--state->num_remaining_tasks;
	if (state->num_remaining_tasks == 0)
		state->run_pe_progress = false;

	//printf("ec recover complete callback memcpy, rest task num %d\n", state->num_remaining_tasks);
}

/*
 * Run ec decode
 *
 * @pci_addr [in]: PCI address of a doca device
 * @matrix_type [in]: matrix type
 * @user_output_file_path [in]: path to the task output file
 * @dir_path [in]: path to the tasks output file
 * @data_block_count [in]: data block count
 * @rdnc_block_count [in]: redundancy block count
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
doca_error_t ec_decode()
{
	uint32_t max_bufs = 2;
	doca_error_t result;
	int ret;
	size_t i;
	uint64_t max_block_size;
	size_t block_file_size;

	uint32_t str_len;

	struct ec_sample_objects state_object = {0};
	struct ec_sample_objects *state = &state_object;
	
	int64_t file_size;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	doca_error_t task_status = DOCA_SUCCESS;
	doca_error_t callback_result = DOCA_SUCCESS;
	struct doca_task *doca_task;
	struct doca_ec_task_recover *task;
	struct recover_task_data task_data;
	union doca_data user_data;

	char* pci_addr = "b1:00.0";

	#define MB (1024*1024)
	uint64_t block_size = MB;

	size_t n_missing = 4;
	int data_block_count = 8;
	int rdnc_block_count = 4;
	uint64_t src_size = block_size * data_block_count;
	uint64_t dst_size = block_size * rdnc_block_count;
	state->missing_indices = (uint32_t*)malloc((4)*sizeof(uint32_t));

	int align_size = 64*1024;

	//数据块 0 1 2 3 4 5 6 7
	//校验块 8 9 10 11
	state->missing_indices[0] = 0;
	state->missing_indices[1] = 1;
	state->missing_indices[2] = 8;
	state->missing_indices[3] = 9;

	char* test_src = malloc(12*MB);
	char* test_dest = malloc(12*MB);
	struct timeval start_time, end_time;
	for(int i=0;i<12;i++){
		memcpy(test_src+i*MB, test_dest+i*MB, MB);
	}
	gettimeofday(&start_time, 0);
	for(int i=0;i<12;i++){
		memcpy(test_src+i*MB, test_dest+i*MB, MB);
	}
	gettimeofday(&end_time, 0);
	int time_copy = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        end_time.tv_usec - start_time.tv_usec;
	printf("copy time %d us\n", time_copy);

	// 准备源数据
	posix_memalign(&state->src_buffer, align_size, src_size);

	int fd_data = open("./testInput", O_RDONLY);
	lseek(fd_data, 2*block_size, SEEK_SET);
	int n = read(fd_data, state->src_buffer, 6*block_size);

	int fd_parity0 = open("./parity0", O_RDONLY);
	n = read(fd_parity0, state->src_buffer + 6*block_size, 1*block_size);

	int fd_parity1 = open("./parity1", O_RDONLY);
	n = read(fd_parity1, state->src_buffer + 7*block_size, 1*block_size);

	// int fd_parity2 = open("./parity2", O_RDONLY);
	// n = read(fd_parity2, state->src_buffer + 6*block_size, 1*block_size);

	// int fd_parity3 = open("./parity3", O_RDONLY);
	// n = read(fd_parity3, state->src_buffer + 7*block_size, 1*block_size);

	// 分配目标数据空间
	posix_memalign(&state->dst_buffer, align_size, dst_size);
	memset(state->dst_buffer, 0, dst_size);

	result = ec_core_init(state,
			      pci_addr,
			      (tasks_check)&doca_ec_cap_task_recover_is_supported,
			      max_bufs,
			      src_size,
			      dst_size,
			      &max_block_size);
	if (result != DOCA_SUCCESS)
		return result;

	/* Set task configuration */
	result = doca_ec_task_recover_set_conf(state->ec,
					       ec_recover_completed_callback,
					       ec_recover_error_callback,
					       1);
	ASSERT_DOCA_ERR(result, state, "Unable to set configuration for recover tasks");

	/* Start the task */
	result = doca_ctx_start(state->core_state.ctx);
	ASSERT_DOCA_ERR(result, state, "Unable to start context");

	result = doca_ec_matrix_create(state->ec,
				       DOCA_EC_MATRIX_TYPE_VANDERMONDE,
				       data_block_count,
				       rdnc_block_count,
				       &state->encoding_matrix);
	ASSERT_DOCA_ERR(result, state, "Unable to create ec matrix");

	result = doca_ec_matrix_create_recover(state->ec,
					       state->encoding_matrix,
					       state->missing_indices,
					       n_missing,
					       &state->decoding_matrix);
	ASSERT_DOCA_ERR(result, state, "Unable to create recovery matrix");

	/* Include all necessary parameters for completion callback in user data of task */
	task_data = (struct recover_task_data){.dir_path = "dir_path",
					       .output_file_path = "output_file_path",
					       .file_size = file_size,
					       .block_size = block_size,
					       .data_block_count = data_block_count,
					       .n_missing = n_missing,
					       .recovered_data_blocks = state->dst_doca_buf,
					       .task_status = &task_status,
					       .cb_result = &callback_result};
	user_data.ptr = &task_data;

	/* Construct EC recover task */
	result = doca_ec_task_recover_allocate_init(state->ec,
						    state->decoding_matrix,
						    state->src_doca_buf,
						    state->dst_doca_buf,
						    user_data,
						    &task);
	ASSERT_DOCA_ERR(result, state, "Unable to allocate and initiate task");

	doca_task = doca_ec_task_recover_as_task(task);
	SAMPLE_ASSERT(doca_task != NULL, DOCA_ERROR_UNEXPECTED, state, "Unable to retrieve task as doca_task");

	/* Enqueue ec recover task */

	gettimeofday(&start_time, 0);

	result = doca_task_submit(doca_task);
	// ASSERT_DOCA_ERR(result, state, "Unable to submit task");

	state->run_pe_progress = true;
	state->num_remaining_tasks = 1;

	/* Wait for recover task completion and for context to return to idle */
	while (state->run_pe_progress) {
		if (doca_pe_progress(state->core_state.pe) == 0)
		;
			// nanosleep(&ts, &ts);
	}
	gettimeofday(&end_time, 0);
	int time_cost_decoding = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        end_time.tv_usec - start_time.tv_usec;
	printf("decoding time %d us\n", time_cost_decoding);

	// 检查恢复的数据
	result = doca_buf_get_data(state->dst_doca_buf, (void **)&state->dst_buffer);
	int len;
	doca_buf_get_data_len(state->dst_doca_buf, &len);
	printf("recover dst len %d\n", len);

	//write parity block
	for(int i=0; i<n_missing; i++){
		char* file_name = (char*)malloc(20);
		sprintf(file_name, "./recoverBlock%d", i);
		int fd_recover = open(file_name, O_RDWR | O_CREAT, 0666);
		write(fd_recover, state->dst_buffer+i*block_size, block_size);
		close(fd_recover);
	}

	printf("recover over\n");
	return callback_result;
}


double get_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct decode_batch_submit_data {
	struct ec_sample_objects *state;
	struct doca_task **ec_task_batch;
	int batch_size;
	int task_start_idx;
	char *src_buffer;
	char *src_seg;
	int src_size_seg;
	doca_error_t submit_result;
};

static void *ec_decode_submit_thread(void *arg)
{
	struct decode_batch_submit_data *data = (struct decode_batch_submit_data *)arg;
	for (int i = 0; i < data->batch_size; i++) {
		memcpy(data->src_buffer + i * data->src_size_seg, data->src_seg + i * data->src_size_seg, data->src_size_seg);

		int task_idx = data->task_start_idx + i;
		doca_error_t result = doca_task_submit(data->ec_task_batch[task_idx]);
		if (result != DOCA_SUCCESS) {
			printf("submit error at task %d: %s\n", i, doca_error_get_descr(result));
			data->submit_result = result;
			return NULL;
		}

		/* 
			因为主线程在等待任务完成时会调用doca_pe_progress来驱动任务的完成回调，
		  	所以这里提交完任务后就直接增加剩余任务数，等主线程调用doca_pe_progress时就能正确地知道还有多少任务在等待完成。
		*/
		__sync_fetch_and_add(&data->state->num_remaining_tasks, 1);
		data->state->run_pe_progress = true;
		
	}

	data->submit_result = DOCA_SUCCESS;
	return NULL;
}

struct timespec ts = {
	.tv_sec = 0,
	.tv_nsec = SLEEP_IN_NANOS,
};

enum offload_type{
	Naive, // 最笨的办法，每个任务提交前都要memcpy src数据到doca buf，任务完成后再memcpy结果数据到dst buffer；
	Batch_Copy, // 批量提交，提交线程负责memcpy src数据到doca buf和提交任务，主线程负责poll任务完成和memcpy结果数据到dst buffer；
	Batch_NoCopy, // 此为ideal 方法，很难集成到分布式系统；
	Batch_Copy_Pipeline, //主线程轮询，任务提交线程负责memcpy src数据到doca buf和提交任务，主线程负责memcpy结果数据到dst buffer，提交线程和主线程并行执行；
};

doca_error_t decode_Bench
(int batch_size, int k, int m, int erasures_count, int block_size, enum offload_type ot)
{
	// 统计信息
	struct timeval start_time, end_time;
	uint64_t matrix_create_time_us = 0;
	uint64_t doca_decode_time = 0;
	uint64_t set_mmap_time_us = 0;
	uint64_t task_init_time_us = 0;
	uint64_t reset_time_us = 0;

	doca_error_t result;
	struct ec_sample_objects state_object = {0};
	struct ec_sample_objects *state = &state_object;
	doca_error_t task_status = DOCA_SUCCESS;
	doca_error_t callback_result = DOCA_SUCCESS;
	struct doca_ec_task_recover *task;
	struct recover_task_data task_data;

	size_t n_missing = erasures_count;
	int data_block_count = k;
	int rdnc_block_count = m;
	int src_size_seg = (data_block_count + rdnc_block_count - erasures_count) * block_size;
	int dst_size_seg = erasures_count * block_size;
	uint64_t src_size = src_size_seg * batch_size;
	uint64_t dst_size = dst_size_seg * batch_size;

	state->missing_indices = (uint32_t*)malloc((n_missing)*sizeof(uint32_t));

	// 默认丢失后面的块
	// 不管丢失几个块，我都需要K个块来反向推导
	for(int i = 0; i < n_missing; i++){
		state->missing_indices[i] = m+i;
	}
	int remaining_data_block_num = k-erasures_count;
	int remaining_code_block_num = m;
	int align_size = 64*1024;

	posix_memalign((void**)&state->src_buffer, align_size, src_size);
	posix_memalign((void**)&state->dst_buffer, align_size, dst_size);

	char* src_segs;
	posix_memalign((void**)&src_segs, align_size, src_size);
	for(int j=0; j<=batch_size-1; j++){
		// prepare data
		writeRandomData(src_segs+j*src_size_seg, 
			remaining_data_block_num*block_size);

		// prepare parity
		writeRandomData(src_segs + j*src_size_seg + remaining_data_block_num*block_size, 
					remaining_code_block_num *block_size);
		
	}

	char* dst_segs;
	posix_memalign((void**)&dst_segs, align_size, dst_size);
	state->app_dst_buffer = dst_segs; // 供callback使用

	// dst 预热
	writeRandomData(dst_segs, dst_size);

	union doca_data ctx_user_data;
	union doca_data *task_user_data_ec_batch;
    struct doca_ec_task_recover **ec_task_recover_batch;
    struct doca_task **ec_task_batch;
	struct doca_ec_task_create **doca_task_reencode;
    struct doca_task **doca_task_reencode_general;
    doca_error_t* ec_task_result_batch;

	result = open_doca_device_with_pci("b1:00.0", (tasks_check)&doca_ec_cap_task_create_is_supported, &state->core_state.dev);

	int doca_src_buf_num = batch_size * data_block_count;
	int doca_dst_buf_num = batch_size; // doca ec 不支持dst使用link list
	
    result = create_core_objects(&state->core_state, doca_src_buf_num + doca_dst_buf_num);
    result = doca_ec_create(state->core_state.dev, &state->ec);
    state->core_state.ctx = doca_ec_as_ctx(state->ec);
    result = doca_pe_connect_ctx(state->core_state.pe, state->core_state.ctx);

	gettimeofday(&start_time, 0);
    result = doca_mmap_set_memrange(state->core_state.src_mmap, state->src_buffer, src_size);
	result = doca_mmap_start(state->core_state.src_mmap);
    result = doca_mmap_set_memrange(state->core_state.dst_mmap, state->dst_buffer, dst_size);
	result = doca_mmap_start(state->core_state.dst_mmap);
	gettimeofday(&end_time, 0);
	set_mmap_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        end_time.tv_usec - start_time.tv_usec;

	struct doca_buf ***ec_src_doca_buf_batch;
    ec_src_doca_buf_batch = (struct doca_buf***)malloc(batch_size * sizeof(struct doca_buf**));
	for(int i=0; i<=batch_size-1; i++){
		ec_src_doca_buf_batch[i] = (struct doca_buf**)malloc(data_block_count * sizeof(struct doca_buf*));
	}

	struct doca_buf **ec_dst_doca_buf_batch;
	ec_dst_doca_buf_batch = (struct doca_buf**)malloc(batch_size * sizeof(struct doca_buf*));

    for(int i=0; i<=batch_size-1; i++){
		for(int j=0; j<=data_block_count-1; j++){
			result = doca_buf_inventory_buf_get_by_addr(state->core_state.buf_inv,
								state->core_state.src_mmap,
								state->src_buffer, // 不起作用
								block_size,
								&ec_src_doca_buf_batch[i][j]);
		}

	    result = doca_buf_inventory_buf_get_by_addr(state->core_state.buf_inv,
						    state->core_state.dst_mmap,
						    state->dst_buffer+i*dst_size_seg,
						    dst_size_seg,
						    &ec_dst_doca_buf_batch[i]);
    }

    ctx_user_data.u64=0;
	ctx_user_data.ptr = state;
	result = doca_ctx_set_user_data(state->core_state.ctx, ctx_user_data);
	result = doca_ctx_set_state_changed_cb(state->core_state.ctx, ec_state_changed_callback);

	if(ot == Batch_Copy_Pipeline)
		result = doca_ec_task_recover_set_conf(state->ec, 
			ec_recover_completed_callback_memcpy, 
			ec_recover_error_callback, 
			batch_size);
	else
		result = doca_ec_task_recover_set_conf(state->ec,
				ec_recover_completed_callback,
				ec_recover_error_callback,
				batch_size);

	result = doca_ec_task_create_set_conf(state->ec,
					       ec_create_completed_callback,
					       ec_create_error_callback,
					       batch_size);

	ASSERT_DOCA_ERR(result, state, "Unable to set configuration for recover tasks");

	/* Start the task */
	result = doca_ctx_start(state->core_state.ctx);
	
	result = doca_ec_matrix_create(state->ec,
				       DOCA_EC_MATRIX_TYPE_VANDERMONDE,
				       data_block_count,
				       rdnc_block_count,
				       &state->encoding_matrix);

	gettimeofday(&start_time, 0);
	result = doca_ec_matrix_create_recover(state->ec,
					       state->encoding_matrix,
					       state->missing_indices,
					       n_missing,
					       &state->decoding_matrix);
	gettimeofday(&end_time, 0);
	matrix_create_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        end_time.tv_usec - start_time.tv_usec;

	task_user_data_ec_batch = calloc(batch_size, sizeof(union doca_data));

    ec_task_recover_batch = (struct doca_ec_task_recover**)malloc(batch_size * sizeof(struct doca_ec_task_recover*));
    ec_task_batch = (struct doca_task**)malloc(batch_size * sizeof(struct doca_task*));

	// set doca src buf data
	gettimeofday(&start_time, 0);
    for(int i=0; i<=batch_size-1; i++){
		// set remaining data
		for(int j=0; j<=remaining_code_block_num+remaining_data_block_num-1; j++)
			result = doca_buf_set_data(ec_src_doca_buf_batch[i][j], state->src_buffer + (src_size_seg*i) + block_size*j, block_size);

		for(int j=1; j<=remaining_code_block_num+remaining_data_block_num-1; j++)
			result = doca_buf_chain_list(ec_src_doca_buf_batch[i][0], ec_src_doca_buf_batch[i][j]);

		struct recover_task_user_data *batch_cb_data = malloc(sizeof(*batch_cb_data));
		batch_cb_data->state = state;
		batch_cb_data->batch_index = i;
		batch_cb_data->erasures_count = erasures_count;
		batch_cb_data->block_size = block_size;
		batch_cb_data->dst_size_seg = dst_size_seg;
		batch_cb_data->dst_buffer = state->dst_buffer;
		batch_cb_data->app_dst_buffer = state->app_dst_buffer;
		task_user_data_ec_batch[i].ptr = batch_cb_data;

        result = doca_ec_task_recover_allocate_init(state->ec,
                            state->decoding_matrix,
                            ec_src_doca_buf_batch[i][0],
                            ec_dst_doca_buf_batch[i],
                            task_user_data_ec_batch[i],
                            &ec_task_recover_batch[i]);

        ec_task_batch[i] = doca_ec_task_recover_as_task(ec_task_recover_batch[i]);
    }
	gettimeofday(&end_time, 0);
	task_init_time_us= (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        end_time.tv_usec - start_time.tv_usec;

	state->num_remaining_tasks = 0;
	if(ot == Naive){
		/*
			串行执行一个batch
		*/
		printf("Start Naive Decode\n");
		gettimeofday(&start_time, 0);

		printf("Batch size %d\n", batch_size);
		printf("Dst seg size %d MB\n", dst_size_seg / (1024*1024));

		uint64_t copy_src_time_us = 0;
		uint64_t copy_dst_time_us = 0;
		uint64_t polling_time_us = 0;
		struct timeval copy_start, copy_end, polling_start, polling_end;

		for(int j=0; j<=batch_size-1; j++){
			gettimeofday(&copy_start, 0);
			memcpy(state->src_buffer + j * src_size_seg, src_segs + j * src_size_seg, src_size_seg);
			gettimeofday(&copy_end, 0);
			copy_src_time_us += (copy_end.tv_sec - copy_start.tv_sec) * 1000000 + copy_end.tv_usec - copy_start.tv_usec;
			
			gettimeofday(&polling_start, 0);
			doca_error_t result = doca_task_submit(ec_task_batch[j]);
			if (result != DOCA_SUCCESS) {
				printf("submit error at task %d: %s\n", PTHREAD_CREATE_JOINABLE, doca_error_get_descr(result));
			}

			state->run_pe_progress = true;
			state->num_remaining_tasks ++;

			while (state->run_pe_progress) {
				if (doca_pe_progress(state->core_state.pe) == 0)
					nanosleep(&ts, &ts);
			}
			gettimeofday(&polling_end, 0);
			polling_time_us += (polling_end.tv_sec - polling_start.tv_sec) * 1000000 + polling_end.tv_usec - polling_start.tv_usec;

			// 3) memcpy dst
			gettimeofday(&copy_start, 0);
			memcpy(dst_segs + j*dst_size_seg, state->dst_buffer + j*dst_size_seg, dst_size_seg);
			gettimeofday(&copy_end, 0);
			copy_dst_time_us += (copy_end.tv_sec - copy_start.tv_sec) * 1000000 + copy_end.tv_usec - copy_start.tv_usec;
		}

		gettimeofday(&end_time, 0);

		doca_decode_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + end_time.tv_usec - start_time.tv_usec;
		float doca_Naive_Decode_perf = (float)(erasures_count*block_size*batch_size) / doca_decode_time;
		printf("DOCA Naive Decode Time %ld us\n", doca_decode_time);
		printf("DOCA Naive Decode Throughput %.2f MB/s\n", doca_Naive_Decode_perf);
		printf("memcpy time %lld us\n", copy_src_time_us + copy_dst_time_us);
		printf("polling time %ld us\n", polling_time_us);

	}else if(ot == Batch_Copy){
		gettimeofday(&start_time, 0);

		printf("Start Batch Copy Decode\n");
		printf("Batch size %d\n", batch_size);
		printf("Src seg size %d MB\n", src_size_seg / (1024*1024));
		printf("Dst seg size %d MB\n", dst_size_seg / (1024*1024));

		uint64_t copy_src_time_us = 0;
		uint64_t copy_dst_time_us = 0;
		uint64_t polling_time_us = 0;
		struct timeval copy_start, copy_end, polling_start, polling_end;

		gettimeofday(&copy_start, 0);
		for(int j=0; j<=batch_size-1; j++){
			memcpy(state->src_buffer + j * src_size_seg, src_segs + j * src_size_seg, src_size_seg);
		}
		gettimeofday(&copy_end, 0);
		copy_src_time_us += (copy_end.tv_sec - copy_start.tv_sec) * 1000000 + copy_end.tv_usec - copy_start.tv_usec;

		gettimeofday(&polling_start, 0);
		for(int j=0; j<=batch_size-1; j++){
			doca_error_t result = doca_task_submit(ec_task_batch[j]);
			if (result != DOCA_SUCCESS) {
				printf("submit error at task %d: %s\n", PTHREAD_CREATE_JOINABLE, doca_error_get_descr(result));
			}
			state->run_pe_progress = true;
			state->num_remaining_tasks ++;
		}

		while (state->run_pe_progress) {
			if (doca_pe_progress(state->core_state.pe) == 0)
				nanosleep(&ts, &ts);
		}
		gettimeofday(&polling_end, 0);
		polling_time_us += (polling_end.tv_sec - polling_start.tv_sec) * 1000000 + polling_end.tv_usec - polling_start.tv_usec;

		gettimeofday(&copy_start, 0);
		for(int j=0; j<=batch_size-1; j++){
			// 3) memcpy dst
			memcpy(dst_segs + j*dst_size_seg, state->dst_buffer + j*dst_size_seg, dst_size_seg);
		}
		gettimeofday(&copy_end, 0);
		copy_dst_time_us += (copy_end.tv_sec - copy_start.tv_sec) * 1000000 + copy_end.tv_usec - copy_start.tv_usec;

		gettimeofday(&end_time, 0);
		doca_decode_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + end_time.tv_usec - start_time.tv_usec;
		float doca_Batch_Copy_decode_perf = (float)(erasures_count*block_size*batch_size) / doca_decode_time;
		printf("DOCA Batch Copy Decode Time %ld us\n", doca_decode_time);
		printf("DOCA Batch Copy Decode Throughput %.2f MB/s\n", doca_Batch_Copy_decode_perf);
		printf("memcpy time %lld us\n", copy_src_time_us + copy_dst_time_us);
		printf("cp src time %lld us\n", copy_src_time_us);
		printf("cp dst time %lld us\n", copy_dst_time_us);
		printf("polling time %ld us\n", polling_time_us);

	}else if(ot == Batch_NoCopy){
		/*
			这里直接模拟，不记录copy时间；
		*/

		printf("Start Batch NoCopy Decode\n");
		printf("Batch size %d\n", batch_size);
		printf("Dst seg size %d MB\n", dst_size_seg / (1024*1024));

		for(int j=0; j<=batch_size-1; j++){
			memcpy(state->src_buffer + j * src_size_seg, src_segs, src_size_seg);
		}

		gettimeofday(&start_time, 0);
		for(int j=0; j<=batch_size-1; j++){
			doca_error_t result = doca_task_submit(ec_task_batch[j]);
			if (result != DOCA_SUCCESS) {
				printf("submit error at task %d: %s\n", PTHREAD_CREATE_JOINABLE, doca_error_get_descr(result));
			}
			state->run_pe_progress = true;
			state->num_remaining_tasks ++;
		}

		while (state->run_pe_progress) {
			if (doca_pe_progress(state->core_state.pe) == 0)
				nanosleep(&ts, &ts);
		}
		gettimeofday(&end_time, 0);

		for(int j=0; j<=batch_size-1; j++){
			// 3) memcpy dst
			memcpy(dst_segs + j*dst_size_seg, state->dst_buffer + j*dst_size_seg, dst_size_seg);
		}

		doca_decode_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + end_time.tv_usec - start_time.tv_usec;
		float doca_Batch_NoCopy_decode_perf = (float)(erasures_count*block_size*batch_size) / doca_decode_time;
		printf("DOCA Batch NoCopy Decode Time %ld us\n", doca_decode_time);
		printf("DOCA Batch NoCopy Decode Throughput %.2f MB/s\n", doca_Batch_NoCopy_decode_perf);
	
	}else if(ot == Batch_Copy_Pipeline){
		/*
			流水线执行一个batch
			Pipeline Process
			thread 1 主线程 poll doca pe progress, when task complete, copy data from doca buf to dst buffer；
			主线程在任务结束时触发回调函数，回调函数会拷贝结果至特定内存；
			thread 2 提交线程 memcpy src data to doca buf, then submit task to doca；
		*/
		gettimeofday(&start_time, 0);

		state->num_remaining_tasks = 0;
		state->run_pe_progress = true;

		int submit_thread_num = 2;
		pthread_t submit_threads[submit_thread_num];
		struct decode_batch_submit_data submit_datas[submit_thread_num];
		for(int t=0; t<submit_thread_num; t++){
			submit_datas[t].state = state;
			submit_datas[t].ec_task_batch = ec_task_batch;
			submit_datas[t].batch_size = batch_size/submit_thread_num;
			submit_datas[t].task_start_idx = t*(batch_size/submit_thread_num);
			submit_datas[t].src_buffer = state->src_buffer + t*(batch_size/submit_thread_num)*src_size_seg;
			submit_datas[t].src_seg = src_segs + t*(batch_size/submit_thread_num)*src_size_seg;
			submit_datas[t].src_size_seg = src_size_seg;
			submit_datas[t].submit_result = DOCA_SUCCESS;
			if (pthread_create(&submit_threads[t], NULL, ec_decode_submit_thread, &submit_datas[t]) != 0) {
				printf("failed to create submit thread %d\n", t);
				exit(-1);
			}
		}

		/* Wait for recover task completion and for context to return to idle */
		while (state->run_pe_progress || state->num_remaining_tasks > 0) {
			if (doca_pe_progress(state->core_state.pe) == 0) {
				// printf("sleep\n");
				nanosleep(&ts, &ts);
			}
		}

		for(int t=0; t<submit_thread_num; t++){
			pthread_join(submit_threads[t], NULL);
		}
		
		free(task_user_data_ec_batch);

		// submit data result check
		for(int t=0; t<submit_thread_num; t++){
			if (submit_datas[t].submit_result != DOCA_SUCCESS) {
				printf("submit error at thread %d: %s\n", t, doca_error_get_descr(submit_datas[t].submit_result));
			}
		}
		
		gettimeofday(&end_time, 0);
		doca_decode_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + end_time.tv_usec - start_time.tv_usec;
		float doca_raw_decode_perf = (float)(erasures_count*block_size*batch_size) / doca_decode_time;
		printf("DOCA Pipeline Decode Time %ld us\n", doca_decode_time);
		printf("DOCA Pipeline Decode Throughput %.2f MB/s\n", doca_raw_decode_perf);
	}

	gettimeofday(&start_time, 0);
	for(int i=0; i<=batch_size-1; i++){
		doca_buf_reset_data_len(ec_dst_doca_buf_batch[i]);
	}
	gettimeofday(&end_time, 0);
	reset_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        end_time.tv_usec - start_time.tv_usec;

	printf("\n");
	printf("\nOther Statistics:\n");
	printf("matrix create time %ld us\n", matrix_create_time_us);
	printf("set mmap time %ld us\n", set_mmap_time_us);
	printf("task init time %ld us\n", task_init_time_us);
	printf("reset time %ld us\n", reset_time_us);
	return DOCA_SUCCESS;
}

# define MB (1024*1024*1024)
# define BLOCK_SIZE MB
# define W 8

int size_convert(char* s) {
    double num = 0;
    char unit[3] = {0};
    int i = 0, j = 0;
    
    // 提取数字部分
    while (s[i] && (isdigit(s[i]) || s[i] == '.')) {
        i++;
    }
    
    // 将数字部分转换为double
    char num_str[32] = {0};
    strncpy(num_str, s, i);
    num = atof(num_str);
    
    // 提取单位部分并转换为大写
    while (s[i] && j < 2) {
        if (isalpha(s[i])) {
            unit[j++] = toupper(s[i]);
            i++;
        } else {
            i++;
        }
    }
    
    // 根据单位计算大小
    if (strcmp(unit, "KB") == 0) {
        return (int)(num * 1024);
    } else if (strcmp(unit, "MB") == 0) {
        return (int)(num * 1024 * 1024);
    } else if (strcmp(unit, "GB") == 0) {
        return (int)(num * 1024 * 1024 * 1024);
    } else if (strcmp(unit, "B") == 0 || unit[0] == '\0') {
        return (int)num;
    } else {
        // 未知单位，可以根据需要处理错误
        return -1;
    }
}


long long total_recover_size = (long long)1024*1024*1024;

int main(int argc, char** argv) {
	char* task_type = argv[1];
    int k = atoi(argv[2]);
    int m = atoi(argv[3]);
	int erasures_count = atoi(argv[4]);
    int block_size = size_convert(argv[5]);
	int batch_size = atoi(argv[6]);

	enum offload_type ot = Naive;
	if(strcmp(task_type, "Naive") == 0){
		ot = Naive;
	}else if(strcmp(task_type, "Batch_Copy") == 0){	
		ot = Batch_Copy;
	}else if(strcmp(task_type, "Batch_NoCopy") == 0){
		ot = Batch_NoCopy;
	}else if(strcmp(task_type, "Batch_Copy_Pipeline") == 0){
		ot = Batch_Copy_Pipeline;		
	}

	decode_Bench(batch_size, k, m, erasures_count, block_size, ot);

    return 0;
}