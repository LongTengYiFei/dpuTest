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

#include <utils.h>

#include "compress_common.h"

DOCA_LOG_REGISTER(COMPRESS_DEFLATE::MAIN);

/* Sample's Logic */
doca_error_t compress_deflate(struct compress_cfg *cfg, char *file_data, size_t file_size);
#define GB (1024ul * 1024ul * 1024ul)

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
	struct compress_cfg compress_cfg;
	uint8_t *file_data = (uint8_t*)malloc(GB);;
	size_t file_size;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	strcpy(compress_cfg.pci_address, "ca:00.0");
	strcpy(compress_cfg.file_path, "../../testfile/tarTest");
	strcpy(compress_cfg.output_path, "out.deflate");
	compress_cfg.zlib_compatible = false;
	compress_cfg.output_checksum = false;

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

	result = doca_argp_init("doca_compress_deflate", &compress_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_compress_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = register_deflate_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params for deflate tasks: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	// read file
    FILE* fp = fopen(compress_cfg.file_path,"rb");
    if(fp == NULL){
        printf("open file error\n");
        exit(0);
    }
    fseek(fp, 0, SEEK_END);
    //unsigned long file_length = ftell(fp);
	unsigned long file_length = GB;
    fseek(fp, 0, SEEK_SET);
    unsigned long n = fread(file_data, 1, file_length, fp);
    fclose(fp);

	result = compress_deflate(&compress_cfg, file_data, file_length);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("compress_deflate() encountered an error: %s", doca_error_get_descr(result));
		goto data_file_cleanup;
	}

	exit_status = EXIT_SUCCESS;

data_file_cleanup:
	if (file_data != NULL)
		free(file_data);
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
