#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_mmap.h>
#include <doca_sha.h>
#include <doca_pe.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"
#include <math.h>
#include <sys/time.h>

#include <vector>
#include <string>
#include <openssl/sha.h>

using namespace std;

DOCA_LOG_REGISTER(SHA_CREATE_CDC);

// predefined Gear Mask
uint64_t GEARv2[256] = {
    0xdc377e207d3c5d43, 0x626790b237a4ab52, 0xfad9bf3a472cfe4d,
    0xa2a6bc5395bbce52, 0xce0a8e4ef2f3ee3f, 0xb4b5b36cf31b4d66,
    0x468f4da9d12660f7, 0x9005e918384c13d0, 0x29ba77f861e74697,
    0x8237cf1734b2c668, 0xee06f9f1df6ced7b, 0x142936b9399add6a,
    0xcb3c0a27878d6de5, 0x49b827acbe6d77ac, 0x65903aad1d6c1e9f,
    0x7a7c66f221cf20ca, 0x9a0f5565415a795,  0xbe5a882391e5527e,
    0x14d2a06077f8339d, 0xcad2bce34bf19652, 0x5541cc588464c621,
    0xc44d981a4aa21e70, 0xb5f4dbb200e75029, 0xa0fc23f2eef70334,
    0xfe36dbfaed334ef3, 0xdd1b785765b2bb6,  0xcc3461a160502f47,
    0x64b1c122c7083b1a, 0x388e6a418b6fd359, 0x65809585f16ab490,
    0x3eb1ec4b9577e4c7, 0xd80e411261617dd8, 0x7da08fd8df71e169,
    0xa20af93edb933a86, 0x59fb1b86e1ff5415, 0x5b596e7a23e7af9a,
    0x50a905ffc0a402c3, 0x3a8cefabefd9dbbe, 0xc9762597ccab4a09,
    0xd8de3774872f3efe, 0xc9f1075d9ebc3c51, 0x3b55ac8bfcf8f51e,
    0xe4e88fb0fa555bd1, 0xfa725ab5e019900a, 0x4be7597f3fcb499d,
    0xe108b91410eb4788, 0xb9f7fc4896cbf4c3, 0x8cdaddac2fc852ee,
    0x5c1d3439307f3d03, 0x60e5ae5e93c82d50, 0x340d48d6be8c5b09,
    0x9a02c3be5b6c05e8, 0x56014afc31084d83, 0x85b888f604ea56f2,
    0x5cb1a8a10079cb23, 0xc52b57c63ff9a0ca, 0xf35659fc64e0143,
    0xbdf325035c594f38, 0xd1f8101a35320afd, 0xe899458626d703f8,
    0xbf97f530ac049837, 0x895021a6620ae70,  0x948596cd24280401,
    0x471f6accca7627f4, 0x4a129ff1164598bd, 0x71405c568bef229e,
    0x4fda2755c3737887, 0x909a3cc70ee44b32, 0xd1a8a3c3dc9fd44d,
    0x5ebb36a043ede1ca, 0xf6a89a10d5e68b53, 0xd9bf7a1aaa5016b2,
    0x71030478353d66cb, 0x6806d8ba045b5ae0, 0xa7672b4808466f67,
    0xf93e0da0d5011f0a, 0xa8aeeeec63465029, 0xe958e539a532a76e,
    0xfd8bf78ab9628da1, 0xca1eb1fe8a769e6a, 0x2a96875d6c8f9257,
    0x501c523d559d7d12, 0x9b7e72aa3d7f1a65, 0x25554c1d398077a,
    0xcdd8af6cd9c471e1, 0x39bea30bd9a49e,   0x233b737e0eeee721,
    0xd6d57c6896b14ed4, 0x8dabe80e8c70e265, 0x1b859ed8c291ddbc,
    0x6b3385f41e0a598b, 0xcf05250d14390e94, 0xc58d5e7d9c8b9f73,
    0x49a5c7a9ba27febc, 0x4841a129f0804005, 0xe7ffa313ce3144a,
    0x13d8768f158b32cb, 0x5d4a2c0fe9cd7afe, 0xe5d0ae6ab568df33,
    0xd5e94a4243d9b506, 0xc6dc1d3da6a23d23, 0xf3b4295d0dd364f2,
    0x57750173991256b5, 0x119097313522fa6a, 0xbeecd02c90273c43,
    0xef69efdc30e9077e, 0xcf28f3bbba364c83, 0xc8b80c742bfdd966,
    0x83f12924c9400e15, 0xa35b3222d11d583e, 0x9c9a9d426cde5fdd,
    0xf298ed021e76023a, 0xb9e8b29602ced7f3, 0x853af80c4f919742,
    0x505b7d96bf01b253, 0xa318b2bff19ed50c, 0x5308029ce76f358f,
    0x9a398f26b33f24a,  0x74595977a450ca7b, 0x4e8468ce85680390,
    0x2436fba706b7bf67, 0xfc0923f5563e8424, 0x5fcbfacf4f88506b,
    0xbe722684e90680f2, 0x1119ab5f71bd737d, 0xc739c71894e34ba8,
    0xcd822b0cb4e2e159, 0x67b583610a0410e0, 0xae18b6eb024bdfdb,
    0x865951f3e76f5834, 0x18eebfb065ebcb59, 0xd35f0999dd5e5b00,
    0x241ad24ae452fe07, 0x942b4c3c79dd1dbe, 0x99b14f06198c22a7,
    0xe2987fcf99376312, 0x844daa9a945c9067, 0xfa234ecf470184dc,
    0x6d97ccd39c1eb593, 0x8bf12e40249118d0, 0x923f72bcdb934d2f,
    0x69bd5c907fc76c8e, 0xe5827868949fc4f3, 0x4f5d13c3d6e20e0a,
    0xb3646a9d0111233,  0xf0af68353b17c0c8, 0xc920f56447f90673,
    0x8960c8168641fe16, 0xfee91d454a19219,  0xb25b446e135cf6b2,
    0x761b04f2362314f9, 0xa151afe2fb1ff5be, 0x2e973d6af5de4037,
    0x485c4501258ab54c, 0xf21bd1e05d869951, 0xd79097aaa1050314,
    0x2b5e8c12e04ff4f9, 0x4e43a881e78d9764, 0x16d02eca685abdab,
    0x7913757d06ccfaec, 0x513242305e9af1ef, 0xc847965583801b62,
    0x8862452b0de8c5e5, 0xce5ae051740dea5a, 0x1a028ca4bb2875a3,
    0x5680bba4aad7ffa6, 0x324d2adfb43a331b, 0xe456b7b1c0301b68,
    0x7801a00c795d859d, 0x41bdd48db6ae14a,  0x5fa8107e14c841fb,
    0xd0e4bdea28bef85c, 0x77bdb5eb30614b89, 0xdefa9fd302bbd858,
    0x8a54bfa54688dad,  0x682ec11a915f0980, 0xc4af0b1c0ffec719,
    0xf76fd41604e89104, 0xeb01bf3d9ced6817, 0xa87180c091474c6e,
    0x351bdb3557277969, 0xee81b4ce09b723aa, 0x3bd353a36b05adb,
    0x7bcb44892055af78, 0xadfb4e960a0ca951, 0x2273bad9b4ac3d74,
    0xebb4454444aaa94b, 0xe686846acad641a8, 0x6a6c096404d1c7a3,
    0x3d857f91fc5f6232, 0x5c63557a89fa3a27, 0x3ee7bc50de6d3e04,
    0x2490253782ef8a57, 0xad4d59ff8fa5f4c,  0x1c01a62b4c726533,
    0x9ef66ba35cb5ab2e, 0x4c47d62317d71cdf, 0x2a40c557d5a3f2a0,
    0xf758338d53442abd, 0x1eaefcd073bcdd10, 0xb9833e0eb50e8fd7,
    0x8c966374151a67b6, 0x3edf7efe33cdfd7b, 0x5a2c4a1a310be558,
    0x69e43fcb628fbeb7, 0x99bd77ef54c90d36, 0xbab1c4f59f066e65,
    0x797fd05581e14764, 0x75de5603975e3ea9, 0x11ef41165111c73c,
    0xb7ddf0821a4031f5, 0x3eec9e62cf45d24c, 0xcda86289d51cd2c7,
    0x127483edee743fae, 0x4c62baba754703cf, 0xcd8d463bb2e9583a,
    0xaa38a80cc6c609f3, 0x5f97356ca50a5986, 0x220a1b6bc88caeb,
    0x20420bdd061f48c4, 0xb0e0c20838c35c57, 0xeddd78004259c3d2,
    0xb7b854be9806aa3b, 0xa2256b1fc3bd00fe, 0x3ea3e4597d23917d,
    0xd952cb1a7656852e, 0x89e052b4e095f921, 0x4be913d1c4f79a82,
    0x9d36a507aa427257, 0x69b7818b57dbbf8,  0x21102213d2cae2cd,
    0x27ec5888afbdb47e, 0x4f16a4e8c63e27f9, 0x250a3857af744546,
    0x3e97bbeec0fcabdd, 0x4b6c65f8ca7fd632, 0xa91fce0f23e1be61,
    0x5050c5a688052206, 0x9e2e3f1a6b85328f, 0xe09ec18b956f5fea,
    0x4000f41b1b5b25e7, 0x3fbea3faba569084, 0xe471347f40647f65,
    0xda7abaa5f3caf2c8, 0x189201a4940001f9, 0x29e183e37d7da328,
    0xd2ae28418f093e67, 0x8e5cb797039be80e, 0x41604277260a071d,
    0x11edac1b01696b0e, 0x6e27fc4394cebee3, 0xeeffd1b2b382a2c0,
    0x2443385d4c4e195,  0x2ee724f1ad94f68e, 0xa4491e380c5cf7b,
    0xeb48a728aaf18d2e,
};

//NC level 2
//same to FastCDC
uint64_t Mask15 = 0x0003590703530000LL;
uint64_t Mask11 = 0x0000d90003530000LL;
int avg_chunk_size = 4*1024;
int min_chunk_size = avg_chunk_size / 4;
int max_chunk_size = avg_chunk_size * 8;

struct sha_resources {
	struct program_core_objects state; /* Core objects that manage our "state" */
	struct doca_sha *sha_ctx;	   /* DOCA SHA context */
	size_t num_remaining_tasks;	   /* Number of remaining tasks to process */
	bool run_pe_progress;		   /* Should we keep on progressing the PE? */
};

extern "C" {
doca_error_t sha_hash_is_supported(struct doca_devinfo *devinfo);
void free_cb(void *addr, size_t len, void *opaque);
doca_error_t sha_cleanup(struct sha_resources *resources);
void sha_hash_completed_callback(struct doca_sha_task_hash *sha_hash_task,
					union doca_data task_user_data,
					union doca_data ctx_user_data);
void sha_hash_error_callback(struct doca_sha_task_hash *sha_hash_task,
				    union doca_data task_user_data,
				    union doca_data ctx_user_data);
void sha_state_changed_callback(const union doca_data user_data,
				       struct doca_ctx *ctx,
				       enum doca_ctx_states prev_state,
				       enum doca_ctx_states next_state);
}

int fastcdc(unsigned char* p, int n){
    uint64_t fingerprint = 0;
    int i = min_chunk_size;
    int mid = avg_chunk_size;

    // cut point skipping
    if (n <= min_chunk_size)  
        return n;

    if (n > max_chunk_size)
        n = max_chunk_size;
    else if (n <= avg_chunk_size)
        mid = n;

    while (i < mid) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);
        if ((!(fingerprint & Mask15))) {
            return i;
        }
        i++;
    }

    while (i < n) {
        fingerprint = (fingerprint << 1) + (GEARv2[p[i]]);
        if ((!(fingerprint & Mask11))) {
            return i;
        }
        i++;
    }

    return n;
}

extern "C" doca_error_t sha_create_CDC(char *data, int total_size)
{
	// 固定chunk数量的CDC
	// chunk数量最好是2的指数，不然后续不好log
	int batch_chunk_num = 16;

	size_t src_buffer_len = max_chunk_size * batch_chunk_num;
	char *src_buffer = (char*)malloc(src_buffer_len);

	struct sha_resources resources;
	struct program_core_objects *state = &resources.state;
	memset(&resources, 0, sizeof(resources));

	union doca_data ctx_user_data = {0};
	union doca_data task_user_data = {0};
	
	struct doca_buf **src_doca_buf = (struct doca_buf **)malloc(sizeof(struct doca_buf *) * batch_chunk_num);
	struct doca_buf **dst_doca_buf = (struct doca_buf **)malloc(sizeof(struct doca_buf *) * batch_chunk_num);
	struct doca_sha_task_hash **sha_hash_task = (struct doca_sha_task_hash**)malloc(sizeof(struct doca_sha_task_hash*) * batch_chunk_num);
	struct doca_task **task = (struct doca_task**)malloc(sizeof(struct doca_task*) * batch_chunk_num);

	uint32_t max_bufs = 2*batch_chunk_num; 
	uint32_t min_dst_sha_buffer_size;
	uint64_t max_source_buffer_size;

	uint8_t* dst_buffer = NULL;
	uint8_t* hash_buffer_binary = NULL;
	char* hash_buffer_human_string = NULL;
	size_t hash_length;

	doca_error_t result, task_result;

	result = open_doca_device_with_capabilities(&sha_hash_is_supported, &state->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to open DOCA device for SHA hash task: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sha_cap_get_max_src_buf_size(doca_dev_as_devinfo(state->dev), &max_source_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get maximum source buffer size for DOCA SHA: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}
	if (src_buffer_len > max_source_buffer_size) {
		DOCA_LOG_ERR("User data length %lu exceeds the maximum length %lu for DOCA SHA: %s",
			     src_buffer_len,
			     max_source_buffer_size,
			     doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	result = doca_sha_cap_get_min_dst_buf_size(doca_dev_as_devinfo(state->dev),
						   DOCA_SHA_ALGORITHM_SHA1,
						   &min_dst_sha_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get minimum destination buffer size for DOCA SHA: %s",
			     doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	result = doca_sha_create(state->dev, &resources.sha_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create sha engine: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	state->ctx = doca_sha_as_ctx(resources.sha_ctx);
	result = create_core_objects(state, max_bufs);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&resources);
		return result;
	}

	result = doca_pe_connect_ctx(state->pe, state->ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect progress engine to context: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	int LOG_NUM = log(batch_chunk_num)/log(2);
	result = doca_sha_task_hash_set_conf(resources.sha_ctx,
					    sha_hash_completed_callback,
					    sha_hash_error_callback,
					    LOG_NUM);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set hash complete callback: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	result = doca_ctx_set_state_changed_cb(state->ctx, sha_state_changed_callback);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set SHA state change callback: %s", doca_error_get_descr(result));
		sha_cleanup(&resources);
		return result;
	}

	// SHA结果内存准备
	// 将所有结果并排放在一起
	int dest_buf_size = min_dst_sha_buffer_size * batch_chunk_num;
	dst_buffer = (uint8_t*)calloc(1, dest_buf_size);
	if (dst_buffer == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory");
		sha_cleanup(&resources);
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_set_memrange(state->dst_mmap, dst_buffer, dest_buf_size);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&resources);
		return result;
	}

	result = doca_mmap_set_free_cb(state->dst_mmap, &free_cb, NULL);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&resources);
		return result;
	}

	result = doca_mmap_start(state->dst_mmap);
	if (result != DOCA_SUCCESS) {
		free(dst_buffer);
		sha_cleanup(&resources);
		return result;
	}

	// 源数据内存准备
	// 我准备16个chunk能够得着的最大值
	// 假如一个chunk最大32KB，那么16个chunk最大512KB
	result = doca_mmap_set_memrange(state->src_mmap, src_buffer, src_buffer_len);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&resources);
		return result;
	}

	result = doca_mmap_start(state->src_mmap);
	if (result != DOCA_SUCCESS) {
		sha_cleanup(&resources);
		return result;
	}

	// 首次分配
	// 源和目标
	for(int i=0; i<=batch_chunk_num-1; i++){
		result = doca_buf_inventory_buf_get_by_data(state->buf_inv,
								state->src_mmap,
								src_buffer,
								0,
								&src_doca_buf[i]);

		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
					doca_error_get_descr(result));
			return result;
		}

		result = doca_buf_inventory_buf_get_by_addr(state->buf_inv,
								state->dst_mmap,
								dst_buffer + min_dst_sha_buffer_size*i,
								min_dst_sha_buffer_size,
								&dst_doca_buf[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer representing destination buffer: %s",
					doca_error_get_descr(result));
			return result;
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
		DOCA_LOG_ERR("Unable to start context: %s",
					doca_error_get_descr(result));
		return result;
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
            DOCA_LOG_ERR("Failed to allocate SHA hash task: %s", doca_error_get_descr(result));
            return result;
        }

        /* Number of tasks submitted to progress engine */
        task[i] = doca_sha_task_hash_as_task(sha_hash_task[i]);
        if (task[i] == NULL) {
            result = DOCA_ERROR_UNEXPECTED;
            DOCA_LOG_ERR("Failed to get DOCA SHA hash task as DOCA task: %s", doca_error_get_descr(result));
            return result;
        }
    }

	// 
	hash_buffer_human_string = (char*)calloc(1, (min_dst_sha_buffer_size * 2) + 1);
	hash_buffer_binary = (uint8_t*)calloc(1, min_dst_sha_buffer_size);

	// chunking
    // <off, len>
	vector<pair<int, int>> chunks;
    int rest_size = total_size;
    int off = 0;
	while(rest_size){
        int chunk_len = fastcdc((unsigned char*)data+off, rest_size);
        chunks.push_back(make_pair(off, chunk_len));
        off += chunk_len;
        rest_size -= chunk_len;
	}

    int rest_chunk_num = chunks.size();
    int off2 = 0;
	vector<pair<int, int>> batch_chunks;
    while(rest_chunk_num){
        int now_batch = rest_chunk_num >= batch_chunk_num ? batch_chunk_num : rest_chunk_num;
		resources.num_remaining_tasks = now_batch;
		resources.run_pe_progress = true;
		// move
		int src_buffer_off = 0;
		for(int i=0; i<=now_batch-1; i++){
			batch_chunks.push_back(make_pair(src_buffer_off, chunks[off2].second));
			memcpy(src_buffer + src_buffer_off, data + chunks[off2].first, chunks[off2].second);
			src_buffer_off += chunks[off2].second;
			off2 ++;
            rest_chunk_num --;
		}

        // submit
        for(int i=0; i<=now_batch-1; i++){
            doca_buf_set_data(src_doca_buf[i], src_buffer+batch_chunks[i].first, batch_chunks[i].second);
			result = doca_task_submit(task[i]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to submit SHA hash task: %s", doca_error_get_descr(result));
				return result;
			}
		}



        // waiting
        while (resources.run_pe_progress) {
			if (doca_pe_progress(state->pe) == 0)
					//nanosleep(&ts, &ts);
				;
		}

        // print result
		unsigned char sha1_verify[20];
		for(int i=0; i<=now_batch-1; i++){
			result = doca_buf_get_data_len(dst_doca_buf[i], &hash_length);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to get the data length of DOCA buffer: %s", doca_error_get_descr(result));
				return result;
			}

			result = doca_buf_get_data(dst_doca_buf[i], (void **)&hash_buffer_binary);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to get the data of DOCA buffer: %s", doca_error_get_descr(result));
				return result;
			}

			/* Convert the hex format to char format */
			for (int j = 0; j < hash_length; j++)
				snprintf(hash_buffer_human_string + (2 * j), 3, "%02x", hash_buffer_binary[j]);
			DOCA_LOG_INFO("SHA1 output is: %s", hash_buffer_human_string);

			// cpu sha verify
			SHA1((unsigned char*)src_buffer+batch_chunks[i].first , batch_chunks[i].second, sha1_verify);
			for (int j = 0; j < hash_length; j++)
				snprintf(hash_buffer_human_string + (2 * j), 3, "%02x", sha1_verify[j]);
			DOCA_LOG_INFO("SHA1 verify output is: %s", hash_buffer_human_string);
		}

        // reset
        for(int i=0; i<=now_batch-1; i++){
			doca_buf_reset_data_len(dst_doca_buf[i]);
		}


		batch_chunks.clear();
    }

	return DOCA_SUCCESS;
}
