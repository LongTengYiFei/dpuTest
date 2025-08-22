#ifndef DPUPROXY_H
#define DPUPROXY_H

#include <string>
#include <vector>
#include <sys/time.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_mmap.h>
#include <doca_sha.h>
#include <doca_erasure_coding.h>
#include <doca_pe.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_argp.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_sha.h>
#include <doca_erasure_coding.h>
#include <doca_log.h>

#include "doca_common.h"

extern "C"
{
    extern int *reed_sol_vandermonde_coding_matrix(int k, int m, int w);
}


using namespace std;

struct sha_resources {
	struct program_core_objects state; /* Core objects that manage our "state" */
	struct doca_sha *sha_ctx;	   /* DOCA SHA context */
	size_t num_remaining_tasks;	   /* Number of remaining tasks to process */
	bool run_pe_progress;		   /* Should we keep on progressing the PE? */
};

struct ec_resources {
	struct program_core_objects state; /* Core objects that manage our "state" */
	struct doca_ec *ec_ctx;	   /* DOCA EC context */
	size_t num_remaining_tasks;	   /* Number of remaining tasks to process */
	bool run_pe_progress;		   /* Should we keep on progressing the PE? */
};

enum DPU_TYPE{
    DPU_TYPE_SHA,
    DPU_TYPE_EC
};  

class DPUProxy{

private:
    DPU_TYPE dt;
	struct program_core_objects *state;
	union doca_data ctx_user_data;
	union doca_data task_user_data;
    doca_error_t result, task_result;

    // used for hash
    struct sha_resources resources;
    struct doca_buf **src_doca_buf;
	struct doca_buf **dst_doca_buf;
    struct doca_sha_task_hash **sha_hash_task;
    struct doca_task **task;
    uint8_t *batch_dst_buffer;
    uint8_t * hash_buffer_binary;
    size_t src_buffer_len;
    uint32_t max_bufs;
    uint32_t min_dst_sha_buffer_size;
	uint64_t max_source_buffer_size;

    // used for ec
    struct ec_resources ec_resources;
    struct doca_ec_matrix *encoding_matrix; 
	struct doca_ec_matrix *decoding_matrix;
    char* ec_src;
    char* ec_dst;
    long ec_src_size;
    long ec_dst_size;
    int k;
    int m;

    struct doca_buf *ec_src_doca_buf;
    struct doca_buf *ec_dst_doca_buf;
    struct doca_ec_task_create *ec_task_create;
    struct doca_ec_task_recover *ec_task_recover;
    struct doca_task *ec_task;

    int ec_batch_size;
    struct doca_buf **ec_src_doca_buf_batch;
    struct doca_buf **ec_dst_doca_buf_batch;
    struct doca_ec_task_create **ec_task_create_batch;
    struct doca_task **ec_task_batch;
    doca_error_t* ec_task_result_batch;
    union doca_data* task_user_data_ec_batch;
    int ec_batch_process_time;
    int ec_batch_memcpy_time;

public:
    int copy_time_us;
    int encode_time_us;
    int test_time_us;
    struct timeval start_time, end_time;

public:
    DPUProxy();
    ~DPUProxy();

    static bool hashEngineAvailable();
    void initHash(int tasks_count, int average_chunk_size, const char* batch_src_buffer, size_t reserve_size);
    std::string getHashTaskResult(int task_number);
    void submitBatchHashTask(const string & segment, const vector<pair<int, int>>& segment_chunks, int now_batch_size);
    void waitingHashTasks();
    void resetHashDestBuf(int batch_size);

    static bool ecEngineAvailable();
    void encode_chunks(char**data, char** coding, int block_size);
    void initEC(int k, int m, int block_size);
    void registerMemoryEC(char** data, char** coding, int block_size);
    void submitOneECTask(char** data, int block_size);
    void waitingECTasks();
    void getECTaskResult(char** coding, int block_size);
    void resetECDestBuf();

    void initECBatch(int k, int m, int block_size, int batch_size);
    void resetECDestBufBatch();
    void submitECTaskBatch();
    void waitingECTasksBatch();
    void prepareECBatch(char* input_data, int block_size, int k, int batch_size);
    void encode_chunks(char* input_data, int block_size, int k, int batch_size);
    void getECTaskResultBatch(int i, char** coding, int block_size);
    int getECBatchProcessTime();
    int getECBatchMemcpyTime();
    

private:
    void clearSHA();
    void clearEC();
};
#endif