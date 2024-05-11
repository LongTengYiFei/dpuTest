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
#include <fcntl.h>
#include <unistd.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_sha.h>
#include <doca_log.h>

#include <utils.h>

DOCA_LOG_REGISTER(SHA_CREATE::MAIN);

#define MAX_USER_DATA_LEN 1024			/* max user data length */
#define MAX_DATA_LEN (MAX_USER_DATA_LEN + 1)	/* max data length */
#define MIN_USER_DATA_LEN 1			/* min user data length */
#define GB 1073741824ll
#define MB 1048576ll

/* Sample's Logic */
doca_error_t sha_create_cyf(char *src_buffer, long long src_len, int block_size, int queue_depth);
doca_error_t sha_create_cyf2(char *src_buffer, long long src_len, int block_size, int queue_depth);

/*
 * ARGP Callback - Handle user data parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
data_callback(void *param, void *config)
{
	char *data = (char *)config;
	char *input_data = (char *)param;
	int len;

	len = strnlen(input_data, MAX_DATA_LEN);
	if (len == MAX_DATA_LEN || len < MIN_USER_DATA_LEN) {
		DOCA_LOG_ERR("Invalid data length, should be between %d and %d", MIN_USER_DATA_LEN, MAX_USER_DATA_LEN);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(data, input_data);
	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t
register_sha_params()
{
	doca_error_t result;
	struct doca_argp_param *data_param;

	result = doca_argp_param_create(&data_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_get_error_string(result));
		return result;
	}
	doca_argp_param_set_short_name(data_param, "d");
	doca_argp_param_set_long_name(data_param, "data");
	doca_argp_param_set_description(data_param, "user data");
	doca_argp_param_set_callback(data_param, data_callback);
	doca_argp_param_set_type(data_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(data_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_get_error_string(result));
		return result;
	}
	return DOCA_SUCCESS;
}

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_sha.h>
#include <doca_error.h>
#include <doca_log.h>
#include "common.h"
#include <stdio.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <sys/time.h>
#define SLEEP_IN_NANOS (1000) /* Sample the job every 10 microseconds  */

static doca_error_t
job_sha_hardware_is_supported(struct doca_devinfo *devinfo)
{
	doca_error_t result;

	result = doca_sha_job_get_supported(devinfo, DOCA_SHA_JOB_SHA512);
	if (result != DOCA_SUCCESS)
		return result;
	return doca_sha_get_hardware_supported(devinfo);
}

static doca_error_t
job_sha_software_is_supported(struct doca_devinfo *devinfo)
{
	return doca_sha_job_get_supported(devinfo, DOCA_SHA_JOB_SHA512);
}

void
free_cb(void *addr, size_t len, void *opaque)
{
	(void)len;
	(void)opaque;

	free(addr);
}

struct program_core_objects state = {0};
struct doca_sha *sha_ctx;
doca_error_t result;
struct doca_event event = {0};
struct timespec ts;
uint8_t *resp_head;
uint32_t workq_depth = 1024;	
uint32_t max_bufs = workq_depth*2;
uint32_t round = 40;
struct doca_buf **src_doca_buf;
struct doca_buf **dst_doca_buf;
struct doca_sha_job *sha_jobs;

doca_error_t doca_SHA_batch_prepare(const unsigned char* src_buf, size_t chunk_size, unsigned char* dst_buf){
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

	init_core_objects(&state, workq_depth, max_bufs);

	src_doca_buf = (struct doca_buf **)malloc(sizeof(struct doca_buf *) * workq_depth);
	dst_doca_buf = (struct doca_buf **)malloc(sizeof(struct doca_buf *) * workq_depth);
	sha_jobs = (struct doca_sha_job*)malloc(sizeof(struct doca_sha_job) * workq_depth);

	doca_mmap_set_memrange(state.src_mmap, (void*)src_buf, chunk_size * workq_depth);
	doca_mmap_start(state.src_mmap);
	doca_mmap_set_permissions(state.src_mmap, DOCA_ACCESS_LOCAL_READ_WRITE);

	// destination memory (sha result) init
	doca_mmap_set_memrange(state.dst_mmap, (void*)dst_buf, DOCA_SHA1_BYTE_COUNT * workq_depth);
	doca_mmap_set_free_cb(state.dst_mmap, &free_cb, NULL);
	doca_mmap_start(state.dst_mmap);
	doca_mmap_set_permissions(state.dst_mmap, DOCA_ACCESS_LOCAL_READ_WRITE);

	for(int i=0; i<=workq_depth-1; i++){
	doca_buf_inventory_buf_by_addr(state.buf_inv, 
											state.src_mmap, 
											(void*)src_buf + i*chunk_size,
											chunk_size, &src_doca_buf[i]);
	doca_buf_set_data(src_doca_buf[i], (void*)src_buf + i*chunk_size, chunk_size);


	doca_buf_inventory_buf_by_addr(state.buf_inv, state.dst_mmap, 
											(void*)dst_buf + i*DOCA_SHA1_BYTE_COUNT,
											DOCA_SHA1_BYTE_COUNT, &dst_doca_buf[i]);
													sha_jobs[i].base.type = DOCA_SHA_JOB_SHA1;
		sha_jobs[i].base.flags = DOCA_JOB_FLAGS_NONE;
		sha_jobs[i].base.ctx = state.ctx;
		sha_jobs[i].base.user_data.u64 = DOCA_SHA_JOB_SHA1;
		sha_jobs[i].resp_buf = dst_doca_buf[i];
		sha_jobs[i].req_buf = src_doca_buf[i];
		sha_jobs[i].flags = DOCA_SHA_JOB_FLAGS_NONE;
	}
}

doca_error_t doca_SHA_batch(const unsigned char* src_buf, size_t chunk_size, unsigned char* dst_buf){

	for(int i=0; i<=workq_depth-1; i++){
		doca_workq_submit(state.workq, &sha_jobs[i].base);
	}
	
	for(int i=0; i<=workq_depth-1; i++){
		while ((result = doca_workq_progress_retrieve(state.workq, &event, DOCA_WORKQ_RETRIEVE_FLAGS_NONE)) ==
			DOCA_ERROR_AGAIN) {
			/* Wait for the job to complete */
			ts.tv_sec = 0;
			ts.tv_nsec = SLEEP_IN_NANOS;
			nanosleep(&ts, &ts);
		}
	}
}


int main(){
	int fd = open("./testfile/linux-6.7-rc2.tar", O_RDWR);  
	if(fd<=0){
		printf("open source file error\n");
	}

	// prepare
	unsigned char sha_buf[DOCA_SHA1_BYTE_COUNT]={0};
	unsigned char *doca_sha_bufs = (unsigned char *)malloc(DOCA_SHA1_BYTE_COUNT * workq_depth);;

	int chunk_size = 4*1024;
	int dst_len = DOCA_SHA1_BYTE_COUNT;
	char * src_data = (char*)malloc(chunk_size*workq_depth);

	doca_SHA_batch_prepare((unsigned char*)src_data, chunk_size, doca_sha_bufs);

	struct timeval start, end;
	for(int r=0; r<=round-1; r++){
		read(fd, src_data, chunk_size*workq_depth);
		
		gettimeofday(&start, 0); 
		doca_SHA_batch((unsigned char*)src_data, chunk_size, doca_sha_bufs);
		gettimeofday(&end, 0); 

		int time_use = 1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec;
		printf("%d\n", time_use);


	}
}

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
// int
// main(int argc, char **argv)
// {
// 	doca_error_t result;
// 	struct doca_logger_backend *stdout_logger = NULL;
// 	int exit_status = EXIT_SUCCESS;

// 	int fd = open("./testfile/linux-6.7-rc2.tar", O_RDWR);  
// 	if(fd<=0){
// 		printf("open source file error\n");
// 	}

// 	int block_size = 16*1024;
// 	int queue_depth = 8;
// 	int block_num = 8;
// 	int src_len = block_size * block_num;
//     char * src_data = (char*)malloc(sizeof(char)*src_len);
// 	if(!src_data){
// 		printf("malloc error\n");
// 	}
//     long long n_read = read(fd, src_data, src_len);
//     printf("n_read = %lld\n", n_read);

// 	// result = doca_log_create_file_backend(stdout, &stdout_logger);
// 	// if (result != DOCA_SUCCESS)
// 	// 	return EXIT_FAILURE;

// 	// result = doca_argp_init("doca_sha_create", &src_data);
// 	// if (result != DOCA_SUCCESS) {
// 	// 	DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_get_error_string(result));
// 	// 	return EXIT_FAILURE;
// 	// }

// 	// result = register_sha_params();
// 	// if (result != DOCA_SUCCESS) {
// 	// 	DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_get_error_string(result));
// 	// 	doca_argp_destroy();
// 	// 	return EXIT_FAILURE;
// 	// }

// 	// result = doca_argp_start(argc, argv);
// 	// if (result != DOCA_SUCCESS) {
// 	// 	DOCA_LOG_ERR("Failed to parse sample input: %s", doca_get_error_string(result));
// 	// 	doca_argp_destroy();
// 	// 	return EXIT_FAILURE;
// 	// }

// 	// sha_create_cyf2(src_data, src_len, block_size, queue_depth);


	
// 	doca_argp_destroy();
// 	return exit_status;
// }
