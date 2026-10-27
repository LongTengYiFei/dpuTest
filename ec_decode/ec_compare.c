/*
 * Correctness and latency comparison for DOCA EC and Jerasure.
 *
 * The program deliberately keeps the two implementations independent:
 * each implementation creates its own parity blocks and is verified against
 * its own known-good stripe.  This avoids assuming that two libraries use an
 * identical representation for a named coding matrix.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <openssl/sha.h>

#include <cauchy.h>
#include <jerasure.h>
#include <reed_sol.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "common.h"

#define DATA_COUNT 4
#define PARITY_COUNT 2
#define TOTAL_COUNT (DATA_COUNT + PARITY_COUNT)
#define GF_WIDTH 8
#define ALIGNMENT (64U * 1024U)

enum matrix_choice {
	MATRIX_CAUCHY,
	MATRIX_VANDERMONDE,
};

struct options {
	const char *pci_addr;
	size_t block_size;
	unsigned int iterations;
	unsigned int warmups;
	enum matrix_choice matrix;
};

struct loss_case {
	const char *name;
	const char *doca_flow;
	uint32_t missing[PARITY_COUNT];
	size_t missing_count;
};

static const struct loss_case loss_cases[] = {
	{
		.name = "1 data (D1)",
		.doca_flow = "recover",
		.missing = {1},
		.missing_count = 1,
	},
	{
		.name = "1 parity (P0)",
		.doca_flow = "create",
		.missing = {DATA_COUNT},
		.missing_count = 1,
	},
	{
		.name = "2 data (D1,D2)",
		.doca_flow = "recover",
		.missing = {1, 2},
		.missing_count = 2,
	},
	{
		.name = "2 parity (P0,P1)",
		.doca_flow = "create",
		.missing = {DATA_COUNT, DATA_COUNT + 1},
		.missing_count = 2,
	},
	{
		.name = "1 data + 1 parity (D1,P0)",
		.doca_flow = "recover+create",
		.missing = {1, DATA_COUNT},
		.missing_count = 2,
	},
};

struct task_wait {
	volatile bool done;
	doca_error_t status;
};

struct doca_test_state {
	struct program_core_objects core;
	struct doca_ec *ec;
	struct doca_ec_matrix *encoding_matrix;
	struct doca_ec_task_create *create_task;
	struct doca_buf *create_src_buf;
	struct doca_buf *recover_src_buf;
	struct doca_buf *create_dst_buf;
	struct doca_buf *recover_dst_buf;
	uint8_t *src_region;
	uint8_t *dst_region;
	uint8_t *create_src;
	uint8_t *recover_src;
	uint8_t *create_dst;
	uint8_t *recover_dst;
	size_t block_size;
	struct task_wait wait;
};

struct case_result {
	bool doca_ok;
	bool cpu_ok;
	double doca_us;
	double cpu_us;
	double doca_mib_s;
	double cpu_mib_s;
};

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

static void fill_deterministic(uint8_t *buf, size_t len)
{
	uint32_t state = 0x4d595df4U;

	for (size_t i = 0; i < len; ++i) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		buf[i] = (uint8_t)(state >> 24);
	}
}

static void sha1_hex(const void *buf, size_t len, char out[SHA_DIGEST_LENGTH * 2 + 1])
{
	unsigned char digest[SHA_DIGEST_LENGTH];

	SHA1(buf, len, digest);
	for (size_t i = 0; i < SHA_DIGEST_LENGTH; ++i)
		sprintf(out + i * 2, "%02x", digest[i]);
	out[SHA_DIGEST_LENGTH * 2] = '\0';
}

static bool compare_block(const char *implementation,
			  const char *case_name,
			  uint32_t index,
			  const uint8_t *expected,
			  const uint8_t *actual,
			  size_t block_size)
{
	char expected_sha[SHA_DIGEST_LENGTH * 2 + 1];
	char actual_sha[SHA_DIGEST_LENGTH * 2 + 1];

	if (memcmp(expected, actual, block_size) == 0)
		return true;

	sha1_hex(expected, block_size, expected_sha);
	sha1_hex(actual, block_size, actual_sha);
	fprintf(stderr,
		"%s mismatch in %s, chunk %s%u: expected SHA1=%s actual SHA1=%s\n",
		implementation,
		case_name,
		index < DATA_COUNT ? "D" : "P",
		index < DATA_COUNT ? index : index - DATA_COUNT,
		expected_sha,
		actual_sha);
	return false;
}

static bool case_has_missing(const struct loss_case *test_case, uint32_t index)
{
	for (size_t i = 0; i < test_case->missing_count; ++i) {
		if (test_case->missing[i] == index)
			return true;
	}
	return false;
}

static size_t count_missing_data(const struct loss_case *test_case)
{
	size_t count = 0;

	for (size_t i = 0; i < test_case->missing_count; ++i) {
		if (test_case->missing[i] < DATA_COUNT)
			++count;
	}
	return count;
}

static size_t count_missing_parity(const struct loss_case *test_case)
{
	return test_case->missing_count - count_missing_data(test_case);
}

static const uint8_t *chunk_ptr(const uint8_t *data,
				const uint8_t *parity,
				uint32_t index,
				size_t block_size)
{
	if (index < DATA_COUNT)
		return data + (size_t)index * block_size;
	return parity + (size_t)(index - DATA_COUNT) * block_size;
}

static doca_error_t ec_device_is_supported(struct doca_devinfo *devinfo)
{
	doca_error_t result;

	result = doca_ec_cap_task_create_is_supported(devinfo);
	if (result != DOCA_SUCCESS)
		return result;
	return doca_ec_cap_task_recover_is_supported(devinfo);
}

static void create_success_cb(struct doca_ec_task_create *task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data)
{
	struct task_wait *wait = task_user_data.ptr;

	(void)ctx_user_data;
	wait->status = doca_task_get_status(doca_ec_task_create_as_task(task));
	wait->done = true;
}

static void create_error_cb(struct doca_ec_task_create *task,
			    union doca_data task_user_data,
			    union doca_data ctx_user_data)
{
	create_success_cb(task, task_user_data, ctx_user_data);
}

static void recover_success_cb(struct doca_ec_task_recover *task,
			       union doca_data task_user_data,
			       union doca_data ctx_user_data)
{
	struct task_wait *wait = task_user_data.ptr;

	(void)ctx_user_data;
	wait->status = doca_task_get_status(doca_ec_task_recover_as_task(task));
	wait->done = true;
}

static void recover_error_cb(struct doca_ec_task_recover *task,
			     union doca_data task_user_data,
			     union doca_data ctx_user_data)
{
	recover_success_cb(task, task_user_data, ctx_user_data);
}

static doca_error_t run_doca_task(struct doca_test_state *state, struct doca_task *task)
{
	doca_error_t result;
	double deadline;

	state->wait.done = false;
	state->wait.status = DOCA_ERROR_IN_PROGRESS;
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS)
		return result;

	deadline = now_us() + 30.0 * 1000000.0;
	while (!state->wait.done) {
		(void)doca_pe_progress(state->core.pe);
		if (now_us() > deadline)
			return DOCA_ERROR_TIME_OUT;
	}
	return state->wait.status;
}

static void doca_state_cleanup(struct doca_test_state *state)
{
	if (state->create_task != NULL)
		doca_task_free(doca_ec_task_create_as_task(state->create_task));
	if (state->create_src_buf != NULL)
		(void)doca_buf_dec_refcount(state->create_src_buf, NULL);
	if (state->recover_src_buf != NULL)
		(void)doca_buf_dec_refcount(state->recover_src_buf, NULL);
	if (state->create_dst_buf != NULL)
		(void)doca_buf_dec_refcount(state->create_dst_buf, NULL);
	if (state->recover_dst_buf != NULL)
		(void)doca_buf_dec_refcount(state->recover_dst_buf, NULL);
	if (state->encoding_matrix != NULL)
		(void)doca_ec_matrix_destroy(state->encoding_matrix);
	if (state->core.ctx != NULL) {
		(void)request_stop_ctx(state->core.pe, state->core.ctx);
		state->core.ctx = NULL;
	}
	if (state->ec != NULL)
		(void)doca_ec_destroy(state->ec);
	(void)destroy_core_objects(&state->core);
	free(state->src_region);
	free(state->dst_region);
	memset(state, 0, sizeof(*state));
}

static doca_error_t doca_state_init(struct doca_test_state *state,
				    const struct options *opts,
				    const uint8_t *original_data)
{
	doca_error_t result;
	union doca_data user_data = {.ptr = &state->wait};
	uint64_t max_block_size;
	size_t src_region_size = 2U * DATA_COUNT * opts->block_size;
	size_t dst_region_size = 2U * PARITY_COUNT * opts->block_size;
	enum doca_ec_matrix_type matrix_type = opts->matrix == MATRIX_CAUCHY ?
		DOCA_EC_MATRIX_TYPE_CAUCHY : DOCA_EC_MATRIX_TYPE_VANDERMONDE;

	memset(state, 0, sizeof(*state));
	state->block_size = opts->block_size;

	if (posix_memalign((void **)&state->src_region, ALIGNMENT, src_region_size) != 0 ||
	    posix_memalign((void **)&state->dst_region, ALIGNMENT, dst_region_size) != 0) {
		result = DOCA_ERROR_NO_MEMORY;
		goto fail;
	}
	memset(state->src_region, 0, src_region_size);
	memset(state->dst_region, 0, dst_region_size);
	state->create_src = state->src_region;
	state->recover_src = state->src_region + DATA_COUNT * opts->block_size;
	state->create_dst = state->dst_region;
	state->recover_dst = state->dst_region + PARITY_COUNT * opts->block_size;
	memcpy(state->create_src, original_data, DATA_COUNT * opts->block_size);

	result = open_doca_device_with_pci(opts->pci_addr, ec_device_is_supported, &state->core.dev);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = create_core_objects(&state->core, 4);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_ec_create(state->core.dev, &state->ec);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_ec_cap_get_max_block_size(doca_dev_as_devinfo(state->core.dev), &max_block_size);
	if (result != DOCA_SUCCESS)
		goto fail;
	if (opts->block_size > max_block_size) {
		fprintf(stderr,
			"Block size %zu exceeds device maximum %" PRIu64 "\n",
			opts->block_size,
			max_block_size);
		result = DOCA_ERROR_INVALID_VALUE;
		goto fail;
	}

	state->core.ctx = doca_ec_as_ctx(state->ec);
	if (state->core.ctx == NULL) {
		result = DOCA_ERROR_UNEXPECTED;
		goto fail;
	}
	result = doca_pe_connect_ctx(state->core.pe, state->core.ctx);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_set_memrange(state->core.src_mmap, state->src_region, src_region_size);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_mmap_start(state->core.src_mmap);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_mmap_set_memrange(state->core.dst_mmap, state->dst_region, dst_region_size);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_mmap_start(state->core.dst_mmap);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_buf_inventory_buf_get_by_addr(state->core.buf_inv,
						    state->core.src_mmap,
						    state->create_src,
						    DATA_COUNT * opts->block_size,
						    &state->create_src_buf);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_buf_set_data(state->create_src_buf,
				   state->create_src,
				   DATA_COUNT * opts->block_size);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_buf_inventory_buf_get_by_addr(state->core.buf_inv,
						    state->core.src_mmap,
						    state->recover_src,
						    DATA_COUNT * opts->block_size,
						    &state->recover_src_buf);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_buf_set_data(state->recover_src_buf,
				   state->recover_src,
				   DATA_COUNT * opts->block_size);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_buf_inventory_buf_get_by_addr(state->core.buf_inv,
						    state->core.dst_mmap,
						    state->create_dst,
						    PARITY_COUNT * opts->block_size,
						    &state->create_dst_buf);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_buf_inventory_buf_get_by_addr(state->core.buf_inv,
						    state->core.dst_mmap,
						    state->recover_dst,
						    PARITY_COUNT * opts->block_size,
						    &state->recover_dst_buf);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_ec_task_create_set_conf(state->ec, create_success_cb, create_error_cb, 1);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_ec_task_recover_set_conf(state->ec, recover_success_cb, recover_error_cb, 1);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_ctx_start(state->core.ctx);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_ec_matrix_create(state->ec,
				       matrix_type,
				       DATA_COUNT,
				       PARITY_COUNT,
				       &state->encoding_matrix);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_ec_task_create_allocate_init(state->ec,
						  state->encoding_matrix,
						  state->create_src_buf,
						  state->create_dst_buf,
						  user_data,
						  &state->create_task);
	if (result != DOCA_SUCCESS)
		goto fail;

	return DOCA_SUCCESS;

fail:
	fprintf(stderr, "DOCA initialization failed: %s\n", doca_error_get_descr(result));
	doca_state_cleanup(state);
	return result;
}

static doca_error_t run_create(struct doca_test_state *state)
{
	doca_error_t result = doca_buf_reset_data_len(state->create_dst_buf);

	if (result != DOCA_SUCCESS)
		return result;
	return run_doca_task(state, doca_ec_task_create_as_task(state->create_task));
}

static size_t prepare_recover_input(struct doca_test_state *state,
				    const struct loss_case *test_case,
				    const uint8_t *data,
				    const uint8_t *parity,
				    uint32_t matrix_missing[PARITY_COUNT])
{
	size_t available_count = 0;
	size_t matrix_missing_count = 0;

	/*
	 * Supply exactly K chunks in canonical codeword order.  With DOCA 3.0,
	 * missing_indices must also describe parity holes encountered before the
	 * Kth selected chunk.  For example D1+P0 loss produces source
	 * D0,D2,D3,P1 and matrix indices [1,4].  Only recovered original-data
	 * outputs are consumed; missing parity is regenerated with create.
	 */
	for (uint32_t index = 0; index < TOTAL_COUNT && available_count < DATA_COUNT; ++index) {
		if (case_has_missing(test_case, index)) {
			matrix_missing[matrix_missing_count++] = index;
			continue;
		}
		memcpy(state->recover_src + available_count * state->block_size,
		       chunk_ptr(data, parity, index, state->block_size),
		       state->block_size);
		++available_count;
	}
	return matrix_missing_count;
}

static bool verify_doca_case(struct doca_test_state *state,
			     const struct options *opts,
			     const struct loss_case *test_case,
			     const uint8_t *original_data,
			     const uint8_t *original_parity,
			     double *average_us)
{
	doca_error_t result;
	struct doca_ec_matrix *recover_matrix = NULL;
	struct doca_ec_task_recover *recover_task = NULL;
	union doca_data user_data = {.ptr = &state->wait};
	uint32_t matrix_missing[PARITY_COUNT] = {0};
	size_t matrix_missing_count = 0;
	size_t missing_data_count = count_missing_data(test_case);
	size_t missing_parity_count = count_missing_parity(test_case);
	uint8_t *result_data = NULL;
	uint8_t *result_parity = NULL;
	double elapsed = 0.0;
	bool ok = false;

	result_data = malloc(DATA_COUNT * opts->block_size);
	result_parity = malloc(PARITY_COUNT * opts->block_size);
	if (result_data == NULL || result_parity == NULL) {
		fprintf(stderr, "Unable to allocate DOCA result buffers\n");
		goto out;
	}
	memcpy(result_data, original_data, DATA_COUNT * opts->block_size);
	memcpy(result_parity, original_parity, PARITY_COUNT * opts->block_size);
	memcpy(state->create_src, original_data, DATA_COUNT * opts->block_size);

	if (missing_data_count > 0) {
		matrix_missing_count = prepare_recover_input(state,
						     test_case,
						     original_data,
						     original_parity,
						     matrix_missing);
		result = doca_ec_matrix_create_recover(state->ec,
						       state->encoding_matrix,
						       matrix_missing,
						       matrix_missing_count,
						       &recover_matrix);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "%s: recovery matrix creation failed: %s\n",
				test_case->name,
				doca_error_get_descr(result));
			goto out;
		}
		result = doca_ec_task_recover_allocate_init(state->ec,
							  recover_matrix,
							  state->recover_src_buf,
							  state->recover_dst_buf,
							  user_data,
							  &recover_task);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "%s: recover task allocation failed: %s\n",
				test_case->name,
				doca_error_get_descr(result));
			goto out;
		}
	}

	for (unsigned int iteration = 0; iteration < opts->warmups + opts->iterations; ++iteration) {
		double start = now_us();

		if (missing_data_count > 0) {
			result = doca_buf_reset_data_len(state->recover_dst_buf);
			if (result == DOCA_SUCCESS)
				result = run_doca_task(state, doca_ec_task_recover_as_task(recover_task));
			if (result != DOCA_SUCCESS) {
				fprintf(stderr, "%s: DOCA recover failed: %s\n",
					test_case->name,
					doca_error_get_descr(result));
				goto out;
			}

			/* Recovered original data blocks are the prefix of the output. */
			size_t output_index = 0;
			for (size_t i = 0; i < test_case->missing_count; ++i) {
				uint32_t index = test_case->missing[i];
				if (index >= DATA_COUNT)
					continue;
				memcpy(state->create_src + (size_t)index * opts->block_size,
				       state->recover_dst + output_index * opts->block_size,
				       opts->block_size);
				++output_index;
			}
		}

		if (missing_parity_count > 0) {
			result = run_create(state);
			if (result != DOCA_SUCCESS) {
				fprintf(stderr, "%s: DOCA parity re-create failed: %s\n",
					test_case->name,
					doca_error_get_descr(result));
				goto out;
			}
		}

		if (iteration >= opts->warmups)
			elapsed += now_us() - start;
	}

	if (missing_data_count > 0) {
		size_t output_index = 0;
		for (size_t i = 0; i < test_case->missing_count; ++i) {
			uint32_t index = test_case->missing[i];
			if (index >= DATA_COUNT)
				continue;
			memcpy(result_data + (size_t)index * opts->block_size,
			       state->recover_dst + output_index * opts->block_size,
			       opts->block_size);
			++output_index;
		}
	}
	if (missing_parity_count > 0)
		memcpy(result_parity, state->create_dst, PARITY_COUNT * opts->block_size);

	ok = true;
	for (size_t i = 0; i < test_case->missing_count; ++i) {
		uint32_t index = test_case->missing[i];
		const uint8_t *expected = chunk_ptr(original_data, original_parity, index, opts->block_size);
		const uint8_t *actual = chunk_ptr(result_data, result_parity, index, opts->block_size);

		ok = compare_block("DOCA", test_case->name, index, expected, actual, opts->block_size) && ok;
	}
	*average_us = elapsed / opts->iterations;

out:
	if (recover_task != NULL)
		doca_task_free(doca_ec_task_recover_as_task(recover_task));
	if (recover_matrix != NULL)
		(void)doca_ec_matrix_destroy(recover_matrix);
	free(result_data);
	free(result_parity);
	return ok;
}

static bool verify_cpu_case(const struct options *opts,
			    const struct loss_case *test_case,
			    int *coding_matrix,
			    int row_k_ones,
			    const uint8_t *original_data,
			    const uint8_t *original_parity,
			    double *average_us)
{
	char *data[DATA_COUNT] = {0};
	char *parity[PARITY_COUNT] = {0};
	int erasures[PARITY_COUNT + 1];
	double elapsed = 0.0;
	bool ok = false;

	for (size_t i = 0; i < DATA_COUNT; ++i) {
		if (posix_memalign((void **)&data[i], ALIGNMENT, opts->block_size) != 0)
			goto out;
	}
	for (size_t i = 0; i < PARITY_COUNT; ++i) {
		if (posix_memalign((void **)&parity[i], ALIGNMENT, opts->block_size) != 0)
			goto out;
	}
	for (size_t i = 0; i < test_case->missing_count; ++i)
		erasures[i] = (int)test_case->missing[i];
	erasures[test_case->missing_count] = -1;

	for (unsigned int iteration = 0; iteration < opts->warmups + opts->iterations; ++iteration) {
		for (size_t i = 0; i < DATA_COUNT; ++i)
			memcpy(data[i], original_data + i * opts->block_size, opts->block_size);
		for (size_t i = 0; i < PARITY_COUNT; ++i)
			memcpy(parity[i], original_parity + i * opts->block_size, opts->block_size);
		for (size_t i = 0; i < test_case->missing_count; ++i) {
			uint32_t index = test_case->missing[i];
			if (index < DATA_COUNT)
				memset(data[index], 0, opts->block_size);
			else
				memset(parity[index - DATA_COUNT], 0, opts->block_size);
		}

		double start = now_us();
		int decode_result = jerasure_matrix_decode(DATA_COUNT,
						   PARITY_COUNT,
						   GF_WIDTH,
						   coding_matrix,
						   row_k_ones,
						   erasures,
						   data,
						   parity,
						   (int)opts->block_size);
		if (decode_result != 0) {
			fprintf(stderr, "%s: Jerasure decode failed (%d)\n", test_case->name, decode_result);
			goto out;
		}
		if (iteration >= opts->warmups)
			elapsed += now_us() - start;
	}

	ok = true;
	for (size_t i = 0; i < test_case->missing_count; ++i) {
		uint32_t index = test_case->missing[i];
		const uint8_t *expected = chunk_ptr(original_data, original_parity, index, opts->block_size);
		const uint8_t *actual = index < DATA_COUNT ?
			(const uint8_t *)data[index] : (const uint8_t *)parity[index - DATA_COUNT];

		ok = compare_block("CPU", test_case->name, index, expected, actual, opts->block_size) && ok;
	}
	*average_us = elapsed / opts->iterations;

out:
	for (size_t i = 0; i < DATA_COUNT; ++i)
		free(data[i]);
	for (size_t i = 0; i < PARITY_COUNT; ++i)
		free(parity[i]);
	return ok;
}

static bool parse_size(const char *text, size_t *value)
{
	char *end;
	unsigned long long number;
	unsigned long long multiplier = 1;

	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno != 0 || end == text)
		return false;
	if (*end != '\0') {
		if (strcasecmp(end, "KB") == 0 || strcasecmp(end, "K") == 0)
			multiplier = 1024ULL;
		else if (strcasecmp(end, "MB") == 0 || strcasecmp(end, "M") == 0)
			multiplier = 1024ULL * 1024ULL;
		else if (strcasecmp(end, "GB") == 0 || strcasecmp(end, "G") == 0)
			multiplier = 1024ULL * 1024ULL * 1024ULL;
		else
			return false;
	}
	if (number == 0 || number > SIZE_MAX / multiplier)
		return false;
	*value = (size_t)(number * multiplier);
	return *value % 64 == 0 && *value <= INT32_MAX;
}

static bool parse_unsigned(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed == 0 || parsed > UINT32_MAX)
		return false;
	*value = (unsigned int)parsed;
	return true;
}

static void print_usage(const char *program)
{
	printf("Usage: %s [options]\n", program);
	printf("  --pci <BDF>              DOCA device (default: b1:00.0)\n");
	printf("  --block-size <size>      Chunk size, 64-byte aligned (default: 1MB)\n");
	printf("  --iterations <count>     Measured iterations per case (default: 100)\n");
	printf("  --warmups <count>        Warm-up iterations per case (default: 5)\n");
	printf("  --matrix <type>          cauchy or vandermonde (default: cauchy)\n");
	printf("  --help                   Show this help\n");
}

static bool parse_options(int argc, char **argv, struct options *opts)
{
	*opts = (struct options){
		.pci_addr = "b1:00.0",
		.block_size = 1024U * 1024U,
		.iterations = 100,
		.warmups = 5,
		.matrix = MATRIX_CAUCHY,
	};

	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			exit(EXIT_SUCCESS);
		} else if (strcmp(argv[i], "--pci") == 0 && i + 1 < argc) {
			opts->pci_addr = argv[++i];
		} else if (strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) {
			if (!parse_size(argv[++i], &opts->block_size))
				return false;
		} else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
			if (!parse_unsigned(argv[++i], &opts->iterations))
				return false;
		} else if (strcmp(argv[i], "--warmups") == 0 && i + 1 < argc) {
			if (!parse_unsigned(argv[++i], &opts->warmups))
				return false;
		} else if (strcmp(argv[i], "--matrix") == 0 && i + 1 < argc) {
			const char *matrix = argv[++i];
			if (strcasecmp(matrix, "cauchy") == 0)
				opts->matrix = MATRIX_CAUCHY;
			else if (strcasecmp(matrix, "vandermonde") == 0)
				opts->matrix = MATRIX_VANDERMONDE;
			else
				return false;
		} else {
			return false;
		}
	}
	return true;
}

int main(int argc, char **argv)
{
	struct options opts;
	struct doca_test_state doca_state;
	struct case_result results[sizeof(loss_cases) / sizeof(loss_cases[0])] = {0};
	uint8_t *original_data = NULL;
	uint8_t *doca_parity = NULL;
	uint8_t *cpu_parity = NULL;
	char *cpu_data_ptrs[DATA_COUNT];
	char *cpu_parity_ptrs[PARITY_COUNT];
	int *cpu_matrix = NULL;
	int row_k_ones;
	bool all_ok = true;
	char input_sha[SHA_DIGEST_LENGTH * 2 + 1];
	int exit_code = EXIT_FAILURE;

	if (!parse_options(argc, argv, &opts)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (posix_memalign((void **)&original_data,
			   ALIGNMENT,
			   DATA_COUNT * opts.block_size) != 0 ||
	    posix_memalign((void **)&doca_parity,
			   ALIGNMENT,
			   PARITY_COUNT * opts.block_size) != 0 ||
	    posix_memalign((void **)&cpu_parity,
			   ALIGNMENT,
			   PARITY_COUNT * opts.block_size) != 0) {
		fprintf(stderr, "Unable to allocate reference buffers\n");
		goto out;
	}
	fill_deterministic(original_data, DATA_COUNT * opts.block_size);
	sha1_hex(original_data, DATA_COUNT * opts.block_size, input_sha);

	if (opts.matrix == MATRIX_CAUCHY) {
		cpu_matrix = cauchy_original_coding_matrix(DATA_COUNT, PARITY_COUNT, GF_WIDTH);
		row_k_ones = 0;
	} else {
		cpu_matrix = reed_sol_vandermonde_coding_matrix(DATA_COUNT, PARITY_COUNT, GF_WIDTH);
		row_k_ones = 1;
	}
	if (cpu_matrix == NULL) {
		fprintf(stderr, "Unable to create Jerasure coding matrix\n");
		goto out;
	}
	for (size_t i = 0; i < DATA_COUNT; ++i)
		cpu_data_ptrs[i] = (char *)(original_data + i * opts.block_size);
	for (size_t i = 0; i < PARITY_COUNT; ++i)
		cpu_parity_ptrs[i] = (char *)(cpu_parity + i * opts.block_size);
	jerasure_matrix_encode(DATA_COUNT,
			       PARITY_COUNT,
			       GF_WIDTH,
			       cpu_matrix,
			       cpu_data_ptrs,
			       cpu_parity_ptrs,
			       (int)opts.block_size);

	if (doca_state_init(&doca_state, &opts, original_data) != DOCA_SUCCESS)
		goto out;
	if (run_create(&doca_state) != DOCA_SUCCESS) {
		fprintf(stderr, "Initial DOCA parity creation failed\n");
		doca_state_cleanup(&doca_state);
		goto out;
	}
	memcpy(doca_parity, doca_state.create_dst, PARITY_COUNT * opts.block_size);

	printf("4+2 EC correctness and repair latency comparison\n");
	printf("Device: %s, matrix: %s, block: %.2f MiB, iterations: %u (+%u warmups)\n",
	       opts.pci_addr,
	       opts.matrix == MATRIX_CAUCHY ? "cauchy" : "vandermonde",
	       (double)opts.block_size / (1024.0 * 1024.0),
	       opts.iterations,
	       opts.warmups);
	printf("Input SHA1: %s\n", input_sha);
	printf("Timing excludes initialization, matrix generation, reference encoding, and per-iteration CPU reset.\n");
	printf("DOCA mixed repair includes recover -> host memcpy -> create; parity repair creates both parity chunks.\n");
	printf("CPU/DOCA is the latency ratio; values above 1.0 mean DOCA is faster.\n\n");

	for (size_t i = 0; i < sizeof(loss_cases) / sizeof(loss_cases[0]); ++i) {
		const struct loss_case *test_case = &loss_cases[i];
		double repaired_mib = (double)(test_case->missing_count * opts.block_size) /
				      (1024.0 * 1024.0);

		results[i].doca_ok = verify_doca_case(&doca_state,
						      &opts,
						      test_case,
						      original_data,
						      doca_parity,
						      &results[i].doca_us);
		results[i].cpu_ok = verify_cpu_case(&opts,
						  test_case,
						  cpu_matrix,
						  row_k_ones,
						  original_data,
						  cpu_parity,
						  &results[i].cpu_us);
		results[i].doca_mib_s = repaired_mib / (results[i].doca_us / 1000000.0);
		results[i].cpu_mib_s = repaired_mib / (results[i].cpu_us / 1000000.0);
		all_ok = results[i].doca_ok && results[i].cpu_ok && all_ok;
	}

	printf("%-30s %-16s %-11s %-11s %-14s %-14s %-9s\n",
	       "Loss case",
	       "DOCA flow",
	       "DOCA us",
	       "CPU us",
	       "DOCA MiB/s",
	       "CPU MiB/s",
	       "CPU/DOCA");
	for (size_t i = 0; i < sizeof(loss_cases) / sizeof(loss_cases[0]); ++i) {
		double speedup = results[i].doca_us > 0.0 ? results[i].cpu_us / results[i].doca_us : 0.0;

		printf("%-30s %-16s %8.2f %s %8.2f %s %12.2f %12.2f %8.2fx\n",
		       loss_cases[i].name,
		       loss_cases[i].doca_flow,
		       results[i].doca_us,
		       results[i].doca_ok ? "PASS" : "FAIL",
		       results[i].cpu_us,
		       results[i].cpu_ok ? "PASS" : "FAIL",
		       results[i].doca_mib_s,
		       results[i].cpu_mib_s,
		       speedup);
	}

	printf("\nOverall correctness: %s\n", all_ok ? "PASS" : "FAIL");
	exit_code = all_ok ? EXIT_SUCCESS : EXIT_FAILURE;
	doca_state_cleanup(&doca_state);

out:
	free(cpu_matrix);
	free(original_data);
	free(doca_parity);
	free(cpu_parity);
	return exit_code;
}
