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
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_sha.h>
#include <doca_log.h>

#include <utils.h>

#include <fcntl.h>

DOCA_LOG_REGISTER(SHA_CREATE::MAIN);

#define MAX_USER_DATA_LEN 1024		     /* max user data length */
#define MAX_DATA_LEN (MAX_USER_DATA_LEN + 1) /* max data length */
#define MIN_USER_DATA_LEN 1		     /* min user data length */

#define GB (1024*1024*1024)
#define MB (1024*1024)

/* Sample's Logic */
doca_error_t sha_create(char *src_buffer, int unit_size, int batch_size, int batch_num);
doca_error_t sha_create_CDC(char *src_buffer, int total_size);

/*
 * ARGP Callback - Handle user data parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t data_callback(void *param, void *config)
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
static doca_error_t register_sha_params(void)
{
	doca_error_t result;
	struct doca_argp_param *data_param;

	result = doca_argp_param_create(&data_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(data_param, "d");
	doca_argp_param_set_long_name(data_param, "data");
	doca_argp_param_set_description(data_param, "user data");
	doca_argp_param_set_callback(data_param, data_callback);
	doca_argp_param_set_type(data_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(data_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
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
int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	
	// args
	// doca 更新后，一个满队列2MB
	int full_queue = (2*1024*1024);
	int unit_size = 4*1024;
	int batch_size = full_queue / unit_size;
	int total_size = 4*MB;
	int batch_num = total_size / full_queue;

	// random data gen
	// 一个满队列的数据多算几遍，就不真的生成1GB数据了
	int real_data_size = unit_size * batch_size;
	char* data = (char*)malloc(total_size);
	for(int i=0; i<=real_data_size-1; i++){
		data[i] = 'b' + rand() % 26;
	}

	char* file_name = "/home/cyf/ssd0/linuxVersion_TAR/v6.0.tar";
	// open file
	int fd = open(file_name, O_RDONLY, 0777);
	int n_read = read(fd, data, total_size);

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init("doca_sha_create", &data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_sha_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	// result = sha_create(data, unit_size, batch_size, batch_num);
	result = sha_create_CDC(data, total_size);
	return 0;
	
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("sha_create() encountered an error: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	
	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
