#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jerasure.h>
#include <jerasure/reed_sol.h>
#include <jerasure/cauchy.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define K 8 // 数据块数量
#define M 4 // 校验块数量
#define W 8 // Galois 域的宽度

void writeRandomData(char* buf, size_t len) {
	int i;
	for (i = 0; i < len; i++) {
		buf[i] = rand();
	}
}

int main() {
    size_t data_size = 1024 * 1024 * 64; 
    void* data;
    size_t alignment = 4096; // 通常为文件系统的块大小
    size_t buffer_size = data_size;
    int ret = posix_memalign(&data, alignment, buffer_size);
    writeRandomData(data, data_size);

    // 每个块的大小
    size_t block_size = 1024*1024;

    // 分配数据块和校验块
    char **data_blocks = (char **)malloc(K * sizeof(char *));
    char **coding_blocks = (char **)malloc(M * sizeof(char *));
    for (int i = 0; i < K; i++) {
        data_blocks[i] = (char *)malloc(block_size);
        memcpy(data_blocks[i], data + i * block_size, block_size);
    }
    for (int i = 0; i < M; i++) {
        coding_blocks[i] = (char *)malloc(block_size);
    }

    // get start time 
    struct timeval start_time, end_time;
    gettimeofday(&start_time, 0);
    int *matrix_RSvandermode = reed_sol_vandermonde_coding_matrix(K, M, 8);
    gettimeofday(&end_time, 0);
    int time_cost_gen = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                     end_time.tv_usec - start_time.tv_usec;
    printf("matrix gen time %d us\n", time_cost_gen);

    // 进行编码
    gettimeofday(&start_time, 0);
    jerasure_matrix_encode(K, M, W, matrix_RSvandermode, data_blocks, coding_blocks, block_size);
    gettimeofday(&end_time, 0);
    int time_cost_encoding = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                     end_time.tv_usec - start_time.tv_usec;
    printf("matrix encoding time %d us\n", time_cost_encoding);

    // 恢复测试
    // data   0 1 2 3 4 5 6 7
    // parity 8 9 10 11
    int erasures[] = {4,5,6,7,-1};
    int erasure_count = 4;
    // 解码恢复数据
    gettimeofday(&start_time, 0);
    jerasure_matrix_decode(K, M, W, matrix_RSvandermode, 1, erasures, data_blocks, coding_blocks, block_size);
    gettimeofday(&end_time, 0);
    int time_cost_decoding = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                     end_time.tv_usec - start_time.tv_usec;
    printf("One stripe matrix decoding time %d us, K=%d, M=%d, W=%d, erasure count = %d\n", 
            time_cost_decoding, K, M, W, erasure_count);
    return 0;
}
