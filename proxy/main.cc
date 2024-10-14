
#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_sha.h>
#include <doca_log.h>
#include <utils.h>
#include "common.h"

DOCA_LOG_REGISTER(PROXY::MAIN);

#define GB (1024*1024*1024)


static doca_error_t sha1_hash_is_supported(struct doca_devinfo *devinfo)
{
	return doca_sha_cap_task_hash_get_supported(devinfo, DOCA_SHA_ALGORITHM_SHA1);
}

/**
 * @brief This function check the hash engine is available or not.
 * @return true or false.
 */
bool hash_engine_available(){
	struct doca_dev *dev;
	doca_error_t result = open_doca_device_with_capabilities(&sha1_hash_is_supported, &dev);
	if (result != DOCA_SUCCESS) 
		return false;
	return true;
}

int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	
	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return 0;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS){
		return 0;
	}
		
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS){
		return 0;
	}
		
	if(hash_engine_available()){
		printf("yes\n");
	}else{
		printf("no\n");
	}

	return 0;
}
