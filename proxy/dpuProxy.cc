#include <string>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <vector>
#include <string>
#include <set>
#include <openssl/sha.h>

#include "dpuProxy.h"

/*
    other function
*/
static void free_cb(void *addr, size_t len, void *opaque)
{
	(void)len;
	(void)opaque;

	free(addr);
}

doca_error_t sha1_hash_is_supported(struct doca_devinfo *devinfo)
{
	return doca_sha_cap_task_hash_get_supported(devinfo, DOCA_SHA_ALGORITHM_SHA1);
}

static void sha_hash_completed_callback(struct doca_sha_task_hash *sha_hash_task,
					union doca_data task_user_data,
					union doca_data ctx_user_data)
{
	struct sha_resources *resources = (struct sha_resources *)ctx_user_data.ptr;
	doca_error_t *result = (doca_error_t *)task_user_data.ptr;

	*result = DOCA_SUCCESS;

	--resources->num_remaining_tasks;

	if (resources->num_remaining_tasks == 0)
		resources->run_pe_progress = false;

}

static void sha_hash_error_callback(struct doca_sha_task_hash *sha_hash_task,
				    union doca_data task_user_data,
				    union doca_data ctx_user_data)
{
    // 不用实现，反正我也不会让程序走到这里；
}

static void sha_state_changed_callback(const union doca_data user_data,
				       struct doca_ctx *ctx,
				       enum doca_ctx_states prev_state,
				       enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct sha_resources *resources = (struct sha_resources *)user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		resources->run_pe_progress = false;
		break;
	case DOCA_CTX_STATE_STARTING:
		break;
	case DOCA_CTX_STATE_RUNNING:
		break;
	case DOCA_CTX_STATE_STOPPING:
		break;
	default:
		break;
	}
}

static void ec_state_changed_callback(const union doca_data user_data,
				      struct doca_ctx *ctx,
				      enum doca_ctx_states prev_state,
				      enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct ec_resources *resources = (struct ec_resources *)user_data.ptr;

		switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		resources->run_pe_progress = false;
		break;
	case DOCA_CTX_STATE_STARTING:
		break;
	case DOCA_CTX_STATE_RUNNING:
		break;
	case DOCA_CTX_STATE_STOPPING:
		break;
	default:
		break;
	}
}

static void ec_create_error_callback(struct doca_ec_task_create *create_task,
				     union doca_data task_user_data,
				     union doca_data ctx_user_data)
{
    // 不用实现，反正我也不会让程序走到这里；
}

static void ec_create_completed_callback(struct doca_ec_task_create *create_task,
					 union doca_data task_user_data,
					 union doca_data ctx_user_data)
{
    doca_error_t *result = (doca_error_t *)task_user_data.ptr;
	struct ec_resources *resources = (struct ec_resources *)ctx_user_data.ptr;

	*result = DOCA_SUCCESS;

	resources->run_pe_progress = false;
}

DPUProxy::DPUProxy(){

}

DPUProxy::~DPUProxy(){
    if(this->dt == DPU_TYPE_SHA)
        this->clearSHA();
    else if(this->dt == DPU_TYPE_EC){
        this->clearEC();
    }else{
        
    }
}

void DPUProxy::clearSHA(){
    free(src_doca_buf);
    free(dst_doca_buf);
    free(sha_hash_task);
    free(task);
    free(batch_dst_buffer);
    free(hash_buffer_binary);
}

void DPUProxy::clearEC(){
    doca_error_t result = DOCA_SUCCESS;

    // doca resource 
    if(this->ec_src_doca_buf)
        result = doca_buf_dec_refcount(this->ec_src_doca_buf, NULL);
    if(this->ec_dst_doca_buf)
        result = doca_buf_dec_refcount(this->ec_dst_doca_buf, NULL);
    if(this->encoding_matrix)
        result = doca_ec_matrix_destroy(this->encoding_matrix);
    if(this->state->ctx)
        result = doca_ctx_stop(this->state->ctx);
    if(this->ec_resources.ec_ctx)
        result = doca_ec_destroy(this->ec_resources.ec_ctx);
    if(this->state)
        result = destroy_core_objects(this->state);
    if(this->ec_task_create)
        doca_task_free(doca_ec_task_create_as_task(ec_task_create));
        
    // regular resource
    if (this->ec_src)
		free(this->ec_src);
    if (this->ec_dst)
		free(this->ec_dst);

}

bool DPUProxy::hashEngineAvailable(){
    struct doca_dev *dev;
	doca_error_t result = open_doca_device_with_capabilities(&sha1_hash_is_supported, &dev);
	if (result != DOCA_SUCCESS){
        return false;
    }
	return true;
}

void DPUProxy::initHash(int batch_chunk_num, int average_chunk_size, const char* batch_src_buffer, size_t src_buffer_len){
    this->dt = DPU_TYPE_SHA;
    this->state = &resources.state;
    memset(&resources, 0, sizeof(resources));

    this->ctx_user_data = {0};
	this->task_user_data = {0};
    max_bufs = 2*batch_chunk_num; 

    hash_buffer_binary = (uint8_t*)malloc(SHA_DIGEST_LENGTH);

    src_doca_buf = (struct doca_buf **)malloc(sizeof(struct doca_buf *) * batch_chunk_num);
	dst_doca_buf = (struct doca_buf **)malloc(sizeof(struct doca_buf *) * batch_chunk_num);
	sha_hash_task = (struct doca_sha_task_hash**)malloc(sizeof(struct doca_sha_task_hash*) * batch_chunk_num);
	task = (struct doca_task**)malloc(sizeof(struct doca_task*) * batch_chunk_num);

    result = open_doca_device_with_capabilities(&sha1_hash_is_supported, &state->dev);
	if (result != DOCA_SUCCESS) {
		;
	}

    result = doca_sha_cap_get_max_src_buf_size(doca_dev_as_devinfo(state->dev), &max_source_buffer_size);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "Failed to get maximum source buffer size for DOCA SHA: ", <<doca_error_get_descr(result) << dendl;
        //exit(-1);
        ;
    }

    result = doca_sha_cap_get_min_dst_buf_size(doca_dev_as_devinfo(state->dev),
                        DOCA_SHA_ALGORITHM_SHA1,
                        &min_dst_sha_buffer_size);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "Failed to get minimum destination buffer size for DOCA SHA: "<< doca_error_get_descr(result) << dendl;
        //exit(-1);
        ;
    }

    result = doca_sha_create(state->dev, &resources.sha_ctx);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "Unable to create sha engine: " << doca_error_get_descr(result) << dendl;
        //exit(-1);
        ;
    }

    state->ctx = doca_sha_as_ctx(resources.sha_ctx);
    result = create_core_objects(state, max_bufs);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "Unable to create core object: " << doca_error_get_descr(result) << dendl;
        exit(-1);
    }

    result = doca_pe_connect_ctx(state->pe, state->ctx);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "Failed to connect progress engine to context: " << doca_error_get_descr(result) << dendl;
        exit(-1);
    }

    int LOG_NUM = log(batch_chunk_num)/log(2);
    result = doca_sha_task_hash_set_conf(resources.sha_ctx,
                        sha_hash_completed_callback,
                        sha_hash_error_callback,
                        LOG_NUM);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "Failed to set hash complete callback: " << doca_error_get_descr(result) << dendl;
        exit(-1);
    }

    result = doca_ctx_set_state_changed_cb(state->ctx, sha_state_changed_callback);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "Unable to set SHA state change callback: " << doca_error_get_descr(result) << dendl;
        exit(-1);
    }

    // Prepareing SHA result memory
    int dest_buf_size = min_dst_sha_buffer_size * batch_chunk_num;
    this->batch_dst_buffer = (uint8_t*)calloc(1, dest_buf_size);
    if (batch_dst_buffer == NULL) {
        //dout(10)<< __func__ << "Failed to allocate memory" << dendl;
        exit(-1);
    }

    result = doca_mmap_set_memrange(state->dst_mmap, batch_dst_buffer, dest_buf_size);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "DST doca_mmap_set_memrange error: " << doca_error_get_descr(result) << dendl;
        exit(-1);
    }

    result = doca_mmap_set_free_cb(state->dst_mmap, &free_cb, NULL);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "DST doca_mmap_set_free_cb error: " << doca_error_get_descr(result) << dendl;
        exit(-1);
    }

    result = doca_mmap_start(state->dst_mmap);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "doca_mmap_start error: " << doca_error_get_descr(result) << dendl;
        exit(-1);
    }

    // Preparing source data memory
	result = doca_mmap_set_memrange(state->src_mmap, (void*)batch_src_buffer, src_buffer_len);
	if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "SRC doca_mmap_set_memrange error: " << doca_error_get_descr(result) << dendl;
        exit(-1);
	}

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "SRC doca_mmap_start error " << doca_error_get_descr(result) << dendl;
        exit(-1);
	}

    for(int i=0; i<=batch_chunk_num-1; i++){
        result = doca_buf_inventory_buf_get_by_data(state->buf_inv,
								state->src_mmap,
								(void*)batch_src_buffer,
								0,
								&src_doca_buf[i]);

		if (result != DOCA_SUCCESS) {
			//dout(10)<< __func__   <<"SRC doca_buf_inventory_buf_get_by_addr error:", <<doca_error_get_descr(result);
			exit(-1);
		}

		result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
								state->dst_mmap,
								batch_dst_buffer + min_dst_sha_buffer_size*i,
								min_dst_sha_buffer_size,
								&dst_doca_buf[i]);
		if (result != DOCA_SUCCESS) {
			//dout(10)<< __func__   <<"DST doca_buf_inventory_buf_get_by_addr error:", <<doca_error_get_descr(result);
            exit(-1);
		}
	}

    /* Include tasks counter in user data of context to be decremented in callbacks */
	resources.num_remaining_tasks = batch_chunk_num;
	resources.run_pe_progress = true;
	ctx_user_data.ptr = &resources;
	doca_ctx_set_user_data(state->ctx, ctx_user_data);

	/* Start the context */
	result = doca_ctx_start(state->ctx);
	if (result != DOCA_SUCCESS) {
		//dout(10)<< __func__   <<  "doca_ctx_start erro:",  << doca_error_get_descr(result) << dendl;
		exit(-1);
	}
	/* Include result in user data of task to be used in the callbacks */
	task_user_data.ptr = &task_result;

    // 先把任务准备好
    for(int i=0; i<=batch_chunk_num-1; i++){
        result = doca_sha_task_hash_alloc_init(resources.sha_ctx,
                    DOCA_SHA_ALGORITHM_SHA1,
                    src_doca_buf[i],
                    dst_doca_buf[i],
                    task_user_data,
                    &sha_hash_task[i]);
        if (result != DOCA_SUCCESS) {
            //dout(10)<< __func__   << "Failed to allocate SHA hash task: " << doca_error_get_descr(result) <<dendl;
            exit(-1);
        }

        /* Number of tasks submitted to progress engine */
        task[i] = doca_sha_task_hash_as_task(sha_hash_task[i]);
        if (task[i] == NULL) {
            result = DOCA_ERROR_UNEXPECTED;
            //dout(10)<< __func__   << "Failed to get DOCA SHA hash task as DOCA task:" << doca_error_get_descr(result) << dendl;
            exit(-1);
        }
    }
}

void DPUProxy::waitingHashTasks(){
    while (resources.run_pe_progress) {
        if (doca_pe_progress(state->pe) == 0)
            ;
    }
}

void DPUProxy::waitingECTasks(){
    while (ec_resources.run_pe_progress) {
        if (doca_pe_progress(state->pe) == 0)
            ;
    }
}

std::string DPUProxy::getHashTaskResult(int task_number){
    // 实际上没必要调用，我现在只算SHA1
    // size_t hash_length;
    // doca_error_t result = doca_buf_get_data_len(dst_doca_buf[task_number], &hash_length);
    // if (result != DOCA_SUCCESS) {
    //     //dout(10)<< __func__   << "Failed to get the data length of DOCA buffer: task number"
    //             <<task_number << doca_error_get_descr(result) << dendl;
    //     exit(-1);
    // }

    result = doca_buf_get_data(dst_doca_buf[task_number], (void **)&hash_buffer_binary);
    if (result != DOCA_SUCCESS) {
        //dout(10)<< __func__ << "Failed to get the data of DOCA buffer: task number" << task_number << doca_error_get_descr(result) << dendl;
        exit(-1);
    }

    std::string binary_string((char*)hash_buffer_binary, SHA_DIGEST_LENGTH);
    return binary_string;
}

void DPUProxy::submitBatchHashTask(const string & segment, const vector<pair<int, int>>& segment_chunks, int now_batch_size){
    resources.num_remaining_tasks = now_batch_size;
	resources.run_pe_progress = true;
    for(int i=0; i<=now_batch_size-1; i++){
        result = doca_buf_set_data(src_doca_buf[i], (void*)segment.c_str()+segment_chunks[i].first, segment_chunks[i].second);
        if (result != DOCA_SUCCESS) {
            exit(-1);
        }
        result = doca_task_submit(task[i]);
        if (result != DOCA_SUCCESS) {
            exit(-1);
        }
    }
}

void DPUProxy::resetHashDestBuf(int batch_size){
    for(int i=0; i<=batch_size-1; i++){
		doca_buf_reset_data_len(dst_doca_buf[i]);
	}
}

// EC
bool DPUProxy::ecEngineAvailable(){
    // TODO
	return true;
}

void DPUProxy::initEC(int data_count, int rdnc_count, int block_size){
    this->copy_time_us = 0;
    this->dt = DPU_TYPE_EC;
    this->state = &ec_resources.state;
    memset(&ec_resources, 0, sizeof(ec_resources));
    this->ctx_user_data = {0};
	this->task_user_data = {0};

    // doca 默认最大block size 1MB
    this->ec_src_size = data_count * block_size;
    this->ec_dst_size = rdnc_count * block_size;
    this->ec_src = (char*)malloc(ec_src_size);
    this->ec_dst = (char*)malloc(ec_dst_size);

    this->k = data_count;
    this->m = rdnc_count;

    // TODO: configure PCI address
    result = open_doca_device_with_pci("b1:00.0", (tasks_check)&doca_ec_cap_task_create_is_supported, &state->dev);
    
    result = create_core_objects(this->state, 2);
    result = doca_ec_create(state->dev, &ec_resources.ec_ctx);
    state->ctx = doca_ec_as_ctx(ec_resources.ec_ctx);
    result = doca_pe_connect_ctx(this->state->pe, this->state->ctx);

    result = doca_mmap_set_memrange(this->state->src_mmap, this->ec_src, this->ec_src_size);
	result = doca_mmap_start(this->state->src_mmap);
    result = doca_mmap_set_memrange(this->state->dst_mmap, this->ec_dst, this->ec_dst_size);
	result = doca_mmap_start(this->state->dst_mmap);

	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    this->state->src_mmap,
						    this->ec_src,
						    this->ec_src_size,
						    &this->ec_src_doca_buf);
	result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
						    this->state->dst_mmap,
						    this->ec_dst,
						    this->ec_dst_size,
						    &this->ec_dst_doca_buf);

	result = doca_buf_set_data(this->ec_src_doca_buf, this->ec_src, this->ec_src_size);

	ctx_user_data.ptr = &ec_resources;
	result = doca_ctx_set_user_data(state->ctx, ctx_user_data);
	result = doca_ctx_set_state_changed_cb(state->ctx, ec_state_changed_callback);

	ec_resources.run_pe_progress = true;

	result = doca_ec_task_create_set_conf(ec_resources.ec_ctx,
					      ec_create_completed_callback,
					      ec_create_error_callback,
					      8);

    result = doca_ctx_start(state->ctx);

    // jerasure给的矩阵不能直接用，得转置一下，而且还得是uint8；
	int* tmp = reed_sol_vandermonde_coding_matrix(data_count,rdnc_count,8); // 偶发性bug，暂不清楚为啥
	uint8_t* vandermonde_matrix = (uint8_t*)malloc(sizeof(uint8_t)*data_count*rdnc_count);
	for(int i=0; i<=rdnc_count-1; i++){
		for(int j=0; j<=data_count-1; j++){
			vandermonde_matrix[i +j*rdnc_count] = tmp[i*data_count +j];
		}
	}
	result = doca_ec_matrix_create_from_raw(ec_resources.ec_ctx,
				       vandermonde_matrix,
				       data_count,
				       rdnc_count,
				       &this->encoding_matrix);
    free(vandermonde_matrix);
    free(tmp);

	task_user_data.ptr = &task_result;

	result = doca_ec_task_create_allocate_init(ec_resources.ec_ctx,
						   this->encoding_matrix,
						   this->ec_src_doca_buf,
						   this->ec_dst_doca_buf,
						   task_user_data,
						   &ec_task_create);

	this->ec_task = doca_ec_task_create_as_task(ec_task_create);
    if(this->ec_task == nullptr){
        printf("create task error\n");
    }

}

void DPUProxy::encode_chunks(char**data, char** coding, int block_size){
    submitOneECTask(data, block_size);
    waitingECTasks();
    getECTaskResult(coding, block_size);
    resetECDestBuf();
}

void DPUProxy::submitOneECTask(char** data, int block_size){
	ec_resources.run_pe_progress = true;

    gettimeofday(&start_time, 0);
    for(int i=0, off=0; i<=this->k-1; i++, off+=block_size)
        memcpy(this->ec_src + off, data[i], block_size);
    gettimeofday(&end_time, 0);
    this->copy_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                         end_time.tv_usec - start_time.tv_usec;
    
    result = doca_task_submit(this->ec_task);
    if (result != DOCA_SUCCESS) {
        printf("fatal error\n");
        exit(-1);
    }
}

void DPUProxy::getECTaskResult(char** coding, int block_size){
    gettimeofday(&start_time, 0);
    for(int i=0, coding_off=0; i<=this->m-1; i++, coding_off+=block_size)
        memcpy(coding[i], ec_dst+coding_off, block_size);
    gettimeofday(&end_time, 0);
    this->copy_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                         end_time.tv_usec - start_time.tv_usec;
}

void DPUProxy::resetECDestBuf(){
	doca_buf_reset_data_len(ec_dst_doca_buf);
}