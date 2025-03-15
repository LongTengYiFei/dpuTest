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
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_compress.h>
#include <doca_error.h>
#include <doca_log.h>
#include <utils.h>
#include <fcntl.h>
#include <sys/time.h>
#include <zlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "common.h"
#include "compress_common.h"

DOCA_LOG_REGISTER(DECOMPRESS_DEFLATE::MAIN);

struct compress_deflate_result {
	doca_error_t status; /**< The completion status */
	uint32_t crc_cs;     /**< The CRC checksum */
	uint32_t adler_cs;   /**< The Adler Checksum */
};

struct doca_task *task;
struct doca_compress_task_decompress_deflate *decompress_task;
struct compress_cfg compress_cfg;
struct doca_log_backend *sdk_log;
struct compress_resources resources = {0};
union doca_data task_user_data = {0};
struct compress_deflate_result task_result = {0};
struct program_core_objects *state;
struct doca_buf *src_doca_buf;
struct doca_buf *dst_doca_buf;
/* The sample will use 2 doca buffers */
uint32_t max_bufs = 2;
uint64_t output_checksum = 0;
doca_error_t result;
uint64_t max_buf_size;
struct timespec ts = {
	.tv_sec = 0,
	.tv_nsec = SLEEP_IN_NANOS,
};

char* doca_src_buffer;
int doca_src_buffer_size = 2*1024*1024;

char* doca_dst_buffer;

char* cpu_read_buffer;
int cpu_read_block_size = 2*1024*1024;

char* cpu_compress_dst_buffer;
int cpu_compress_dst_buffer_size = 4*1024*1024;

size_t doca_decompress_result_len;
int cpu_compressed_len;

unsigned long copy_time_us = 0;
unsigned long cpu_compress_time_us = 0;
unsigned long doca_decompress_time_us = 0;
unsigned long workload_data_size = 0;

void printAndResetStat(){
	// print
	printf("copy_time_us: %lu\n", copy_time_us);
	printf("cpu_compress_time_us: %lu\n", cpu_compress_time_us);
	printf("doca_decompress_time_us: %lu\n", doca_decompress_time_us);
	printf("Decompress Speed %.2f MB/s\n", workload_data_size / 1024.0 / 1024.0 / (doca_decompress_time_us / 1000000.0));
	
	// reset
	copy_time_us = 0;
	cpu_compress_time_us = 0;
	doca_decompress_time_us = 0;
	workload_data_size = 0;
}

/*
	先CPU压缩文件片段，然后再DOCA decompression；
*/
doca_error_t decompress_test(char* file_name)
{
	uint64_t checksum;

	int fd = open(file_name, O_RDONLY);
	for(;;){
		// 1.CPU compress
		unsigned long n = read(fd, cpu_read_buffer, cpu_read_block_size);
		workload_data_size += n;
		if(n == 0) break;

		cpu_compressed_len = cpu_compress_dst_buffer_size;

		struct timeval start_time, end_time;
		gettimeofday(&start_time, NULL);
		int comp_ret = compress(cpu_compress_dst_buffer, &cpu_compressed_len, cpu_read_buffer, n);
		gettimeofday(&end_time, NULL);
		cpu_compress_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);

		// 1.5 copy prepare
		gettimeofday(&start_time, NULL);
		memcpy(doca_src_buffer, cpu_compress_dst_buffer, cpu_compressed_len);
		gettimeofday(&end_time, NULL);
		copy_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);

		// 2.DOCA decompress
		doca_buf_reset_data_len(dst_doca_buf);
		result = doca_buf_set_data(src_doca_buf, doca_src_buffer+ZLIB_HEADER_SIZE, 
												cpu_compressed_len-ZLIB_COMPATIBILITY_ADDITIONAL_MEMORY);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to get data length in the DOCA buffer representing source buffer: %s", doca_error_get_descr(result));
			return result;
		}

		gettimeofday(&start_time, NULL);
		resources.num_remaining_tasks++;
		result = doca_task_submit(task);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit compress task: %s", doca_error_get_descr(result));
			doca_task_free(task);
			return result;
		}
		resources.run_pe_progress = true;

		// wait
		while (resources.run_pe_progress) {
			if (doca_pe_progress(state->pe) == 0)
				nanosleep(&ts, &ts);
		}

		gettimeofday(&end_time, NULL);
		doca_decompress_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);
	
		result = doca_buf_get_data_len(dst_doca_buf, &doca_decompress_result_len);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to get DOCA buffer data length for destination buffer: %s",
					doca_error_get_descr(result));
			return result;
		}
	}

	close(fd);
	return result;
}

doca_error_t initDecompressResources()
{	
	/* Allocate memory resources */
	doca_src_buffer = (char*)malloc(doca_src_buffer_size);
	cpu_read_buffer = (char*)malloc(cpu_read_block_size);
	cpu_compress_dst_buffer = (char*)malloc(cpu_compress_dst_buffer_size);

	/* Allocate DOCA resources */
	resources.mode = COMPRESS_MODE_DECOMPRESS_DEFLATE;
	result = allocate_compress_resources("b1:00.0", max_bufs, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate compress resources: %s", doca_error_get_descr(result));
		return result;
	}
	state = resources.state;
	result = doca_compress_cap_task_decompress_deflate_get_max_buf_size(doca_dev_as_devinfo(state->dev),
									    &max_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query decompress max buf size: %s", doca_error_get_descr(result));
		return result;
	}

	/* Start compress context */
	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
		return result;
	}

	doca_dst_buffer = calloc(1, max_buf_size);
	if (doca_dst_buffer == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_ERR("Failed to allocate memory: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_memrange(state->dst_mmap, doca_dst_buffer, max_buf_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));
		return result;
	}
	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_set_memrange(state->src_mmap, doca_src_buffer, doca_src_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));
		return result;
	}

	/* Construct DOCA buffer for each address range */
	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv, state->src_mmap, doca_src_buffer, doca_src_buffer_size, &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Construct DOCA buffer for each address range */
	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    state->dst_mmap,
						    doca_dst_buffer,
						    max_buf_size,
						    &dst_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;
	/* Allocate and construct decompress task */
	result = doca_compress_task_decompress_deflate_alloc_init(resources.compress,
								  src_doca_buf,
								  dst_doca_buf,
								  task_user_data,
								  &decompress_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate decompress task: %s", doca_error_get_descr(result));
		return result;
	}

	task = doca_compress_task_decompress_deflate_as_task(decompress_task);

	return result;
}

void traverseDir(const char *base_path) {
    DIR *dir;
    struct dirent *entry;
    char path[1024];
    struct stat info;

    // 打开目录
    if ((dir = opendir(base_path)) == NULL) {
        perror("opendir() error");
        return;
    }

    // 遍历目录项
    while ((entry = readdir(dir)) != NULL) {
        // 忽略 "." 和 ".." 目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // 构建完整路径
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);

        // 获取文件信息
        if (stat(path, &info) != 0) {
            perror("stat() error");
            continue;
        }

        // 如果是目录，递归调用
        if (S_ISDIR(info.st_mode)) {
            traverseDir(path);
        }
        // 如果是文件且以 ".log" 结尾，打印文件路径
        else if (S_ISREG(info.st_mode)) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".log") == 0) {
                decompress_test(path);
            }
        }
    }

    // 关闭目录
    closedir(dir);
}

int compare(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

#define MAX_PATH_LENGTH 1024
void traverseDirOneLayer(char* base_dir){
	    DIR *dir;
    struct dirent *entry;
    char path[MAX_PATH_LENGTH];
    struct stat info;
    char *subdirs[MAX_PATH_LENGTH];
    int subdir_count = 0;

    // 打开目录
    if ((dir = opendir(base_dir)) == NULL) {
        perror("无法打开目录");
        return;
    }

    // 遍历目录项
    while ((entry = readdir(dir)) != NULL) {
        // 忽略 "." 和 ".." 目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // 构建完整路径
        snprintf(path, sizeof(path), "%s/%s", base_dir, entry->d_name);

        // 获取文件信息
        if (stat(path, &info) != 0) {
            perror("获取文件信息错误");
            continue;
        }

        // 如果是目录，保存目录名
        if (S_ISDIR(info.st_mode)) {
            subdirs[subdir_count] = strdup(entry->d_name);
            if (subdirs[subdir_count] == NULL) {
                perror("内存分配错误");
                closedir(dir);
                return;
            }
            subdir_count++;
        }
    }

    // 关闭目录
    closedir(dir);

    // 按字典序排序子目录名
    qsort(subdirs, subdir_count, sizeof(char *), compare);

    // 打印排序后的子目录的绝对路径
    for (int i = 0; i < subdir_count; i++) {
        char abs_path[MAX_PATH_LENGTH];
        snprintf(path, sizeof(path), "%s/%s", base_dir, subdirs[i]);
        if (realpath(path, abs_path) != NULL) {
            printf("%s\n", abs_path);
			traverseDir(abs_path);
			printAndResetStat();
        } else {
            perror("获取绝对路径错误");
        }
        free(subdirs[i]);
    }
}

int main(int argc, char **argv)
{
	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return 0;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return 0;
		
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return 0;

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init("doca_decompress_deflate", &compress_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return 0;
	}

	result = register_compress_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(result));
		return 0;
	}

	result = register_deflate_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params for deflate tasks: %s", doca_error_get_descr(result));
		return 0;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		return 0;
	}

	initDecompressResources();
	traverseDirOneLayer("/home/cyf/ssd1/benchmark_log_files/");

	return 0;
}
