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
doca_error_t sha_create_cyf(char *src_buffer, int len, int job_num);

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

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int
main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_logger_backend *stdout_logger = NULL;
	int exit_status = EXIT_SUCCESS;

	int fd = open("../testfile/linux-6.7-rc2.tar", O_RDWR);  
	if(fd<=0){
		printf("open source file error\n");
	}

	int block_size = 4*1024;
	int job_num = 8;
	int src_len = block_size*job_num;
    char * src_data = (char*)malloc(sizeof(char)*src_len);
	if(!src_data){
		printf("malloc error\n");
	}
    long long n_read = read(fd, src_data, src_len);
    printf("n_read = %lld\n", n_read);

	result = doca_log_create_file_backend(stdout, &stdout_logger);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_argp_init("doca_sha_create", &src_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_get_error_string(result));
		return EXIT_FAILURE;
	}

	result = register_sha_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_get_error_string(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_get_error_string(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = sha_create_cyf(src_data, n_read, job_num);
	if (result != DOCA_SUCCESS)
		exit_status = EXIT_FAILURE;

	doca_argp_destroy();
	return exit_status;
}
