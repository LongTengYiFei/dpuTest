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
#include <sys/time.h>
#include <utils.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>

#include "common.h"
#include "compress_common.h"

DOCA_LOG_REGISTER(COMPRESS_DEFLATE::MAIN);

#define GB (1024ul * 1024ul * 1024ul)
#define MB (1024ul * 1024ul)

struct compress_deflate_result {
	doca_error_t status; /**< The completion status */
	uint32_t crc_cs;     /**< The CRC checksum */
	uint32_t adler_cs;   /**< The Adler Checksum */
};

// stat
unsigned long doca_compress_time_us = 0;
unsigned long doca_decompress_time_us = 0;
unsigned long cpu_compress_time_us = 0;
unsigned long cpu_decompress_time_us = 0;
unsigned long total_size_before_compression = 0;
unsigned long total_size_after_compression = 0;

struct compress_cfg compress_cfg;
struct doca_log_backend *sdk_log;
int block_size;
char* src_buffer;
char* read_buffer;
char* dst_buffer;
uint64_t max_buf_size, max_output_size;
uint32_t max_bufs;
int blob_size;
int batch_size;
struct compress_resources resources = {0};
struct program_core_objects *state;

struct doca_buf *src_doca_buf;
struct doca_buf *dst_doca_buf;
struct doca_task *task;
struct doca_compress_task_compress_deflate *compress_task;
union doca_data task_user_data={0};
struct compress_deflate_result task_result={0};

struct doca_buf **src_doca_buf_batch;
struct doca_buf **dst_doca_buf_batch;
struct doca_task **task_batch;
struct doca_compress_task_compress_deflate **compress_task_batch;
union doca_data *task_user_data_batch;
struct compress_deflate_result *task_result_batch;

doca_error_t result;
struct timespec ts = {
	.tv_sec = 0,
	.tv_nsec = SLEEP_IN_NANOS,
};
size_t blob_size_after_compression;

doca_error_t initCompressionResources(int _batch_size, int _blob_size_KB)
{	
	batch_size = _batch_size;
	max_bufs = _batch_size*2;
	blob_size = _blob_size_KB*1024;

	/* Allocate resources */
	resources.mode = COMPRESS_MODE_COMPRESS_DEFLATE;
	result = allocate_compress_resources("b1:00.0", max_bufs, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate compress resources: %s", doca_error_get_descr(result));
		return result;
	}

	state = resources.state;
	result = doca_compress_cap_task_compress_deflate_get_max_buf_size(doca_dev_as_devinfo(state->dev), &max_buf_size);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to query compress max buf size: %s", doca_error_get_descr(result));

	if(blob_size > max_buf_size){
		printf("blob size is too large: %d, the max buf size is: %d\n", blob_size, max_buf_size);
		exit(-1);
	}

	src_buffer = (char*)malloc(blob_size*batch_size);
	dst_buffer = (char*)malloc(blob_size*batch_size);

	/* Start compress context */
	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_descr(result));
		
	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, blob_size*batch_size);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));

	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));

	result = doca_mmap_set_memrange(state->src_mmap, src_buffer, blob_size*batch_size);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to set mmap memory range: %s", doca_error_get_descr(result));

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) 
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(result));

	src_doca_buf_batch = (struct doca_buf**)malloc(sizeof(struct doca_buf*)*(batch_size));
	dst_doca_buf_batch = (struct doca_buf**)malloc(sizeof(struct doca_buf*)*(batch_size));
	task_batch = (struct doca_task**)malloc(sizeof(struct doca_task*)*(batch_size));
	compress_task_batch = (struct doca_compress_task_compress_deflate**)malloc(sizeof(struct doca_compress_task_compress_deflate*)*(batch_size));
	task_user_data_batch = (union doca_data*)malloc(sizeof(union doca_data)*(batch_size));
	task_result_batch = (struct compress_deflate_result*)malloc(sizeof(struct compress_deflate_result)*(batch_size));
	// alloc task
	for(int i=0; i<=batch_size-1; i++){
		memset(&task_user_data_batch[i], 0, sizeof(struct compress_deflate_result));

		result = doca_buf_inventory_buf_get_by_addr(state->buf_inv, state->src_mmap, 
			src_buffer+i*blob_size, blob_size, &src_doca_buf_batch[i]);
		result = doca_buf_inventory_buf_get_by_addr(state->buf_inv, state->dst_mmap, 
			dst_buffer+i*blob_size, blob_size, &dst_doca_buf_batch[i]);

		task_user_data_batch[i].ptr = &task_result_batch[i];
		result = doca_compress_task_compress_deflate_alloc_init(resources.compress,
									src_doca_buf_batch[i],
									dst_doca_buf_batch[i],
									task_user_data_batch[i],
									&compress_task_batch[i]);
		task_batch[i] = doca_compress_task_compress_deflate_as_task(compress_task_batch[i]);
	}
}

doca_error_t compressFileDOCA(const char *file_name, int _batch_size, int _blob_size){
	/*
		切片读文件，每次读一点到src_buffer中，覆盖原来的数据；
	*/	

	int fd = open(file_name, O_RDONLY);
	struct timeval start_time, end_time;
	// 没搞懂为啥超过2GB就任务失败
	for(;;){
		int n = read(fd, src_buffer, blob_size*batch_size);
		total_size_before_compression += n;
		if(n == 0) break;

		gettimeofday(&start_time, NULL);
	
		for(int i=0; i<=batch_size-1; i++){
			// reset
			result = doca_buf_reset_data_len(dst_doca_buf_batch[i]);
			result = doca_buf_set_data(src_doca_buf_batch[i], src_buffer+i*blob_size, blob_size);

			// Submit 
			
			result = doca_task_submit(task_batch[i]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to submit compress task: %s", doca_error_get_descr(result));
				doca_task_free(task_batch[i]);
				return result;
			}
		}	
		resources.num_remaining_tasks = batch_size;
		resources.run_pe_progress = true;

		// wait
		while (resources.run_pe_progress) {
			if (doca_pe_progress(state->pe) == 0)
				// nanosleep(&ts, &ts);
				;
		}

		gettimeofday(&end_time, NULL);
		doca_compress_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);

		/* Check result of task according to the result we update in the callbacks */
		if (task_result.status != DOCA_SUCCESS)
			return task_result.status;

		for(int i=0; i<=batch_size-1; i++){
			result = doca_buf_get_data_len(dst_doca_buf_batch[i], &blob_size_after_compression);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to get data length in the DOCA buffer representing destination buffer: %s", doca_error_get_descr(result));
				return result;
			}
			total_size_after_compression += blob_size_after_compression;
		}
	}

	close(fd);
	return result;
}

char* cpu_dst_buffer_compress = NULL;
char* cpu_src_buffer_compress = NULL;
char* cpu_dst_buffer_decompress = NULL;
unsigned long cpu_dst_len_compress;
unsigned long cpu_dst_len_decompress;
unsigned long cpu_src_len_decompress;
int cpu_decompress_dst_buffer_size = 8*1024*1024;
int cpu_compress_dst_buffer_size = 4*1024*1024;
int cpu_read_block_size = 2*1024*1024;

void initBufferCPU(){
	cpu_dst_buffer_decompress = malloc(cpu_decompress_dst_buffer_size);
	cpu_dst_buffer_compress = malloc(cpu_compress_dst_buffer_size);
	cpu_src_buffer_compress = malloc(cpu_read_block_size);
}	

/*
	使用cpu压缩文件片段，然后再解压文件片段，并计算吞吐量；
*/
void test2(const char *file_name){
	int fd = open(file_name, O_RDONLY);
	for(;;){
		// read from file
		unsigned long n = read(fd, cpu_src_buffer_compress, cpu_read_block_size);
		total_size_before_compression += n;
		if(n == 0) break;
		cpu_dst_len_compress = cpu_compress_dst_buffer_size;
		cpu_dst_len_decompress = cpu_decompress_dst_buffer_size;
		struct timeval start_time, end_time;

		gettimeofday(&start_time, NULL);
		int comp_ret = compress2(cpu_dst_buffer_compress, &cpu_dst_len_compress, cpu_src_buffer_compress, n, 6);
		gettimeofday(&end_time, NULL);
		cpu_compress_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);

		cpu_src_len_decompress = cpu_dst_len_compress;
		gettimeofday(&start_time, NULL);
		int consume_bytes = uncompress2(cpu_dst_buffer_decompress, &cpu_dst_len_decompress, cpu_dst_buffer_compress, &cpu_src_len_decompress);
		gettimeofday(&end_time, NULL);
		cpu_decompress_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);
		
		total_size_after_compression += cpu_dst_len_compress;
	}

	close(fd);
}

void printStat(){
	printf("compress speed %.2f MB/s \n", total_size_before_compression / 1024.0 / 1024.0 / (doca_compress_time_us / 1000000.0));
	printf("total size before compression: %.2f MB\n", total_size_before_compression / 1024.0 / 1024.0);
	printf("total size after compression: %.2f MB\n", total_size_after_compression / 1024.0 / 1024.0);
	printf("compress ratio: %.2f%%\n", (1 - (float)total_size_after_compression / (float)total_size_before_compression)*100);
}

void resetStat(){
	doca_compress_time_us = 0;
	total_size_before_compression = 0;
	total_size_after_compression = 0;
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
                compressFileDOCA(path, batch_size, blob_size);
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
			printStat();
			resetStat();
        } else {
            perror("获取绝对路径错误");
        }
        free(subdirs[i]);
    }
}


int main(int argc, char **argv)
{
	doca_error_t result;

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS){
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}

	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS){
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}
		
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS){
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}
		
	result = doca_argp_init("doca_compress_deflate", &compress_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}

	result = register_compress_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}

	result = register_deflate_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("ERROR: %s", doca_error_get_descr(result));
		return 0;
	}

	char* workloads_path = argv[1];
    if(strcmp(argv[2], "dpuBatch") == 0){
		int _batch_size = atoi(argv[3]);
		int _blob_size_KB = atoi(argv[4]);
		initCompressionResources(_batch_size, _blob_size_KB);
		traverseDirOneLayer(workloads_path);
	}
}
