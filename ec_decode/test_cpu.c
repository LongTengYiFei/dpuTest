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

#define DEFAULT_K 8 // 默认数据块数量
#define DEFAULT_M 4 // 默认校验块数量
#define DEFAULT_ERASURE_COUNT 4 // 默认丢失块数量
#define W 8 // Galois 域的宽度
#define DEFAULT_BLOCK_SIZE (1024*1024) // 默认块大小
#define DEFAULT_DATA_SIZE (1024 * 1024 * 64) // 默认数据大小

void writeRandomData(char* buf, size_t len) {
    int i;
    for (i = 0; i < len; i++) {
        buf[i] = rand();
    }
}

void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -k <value>    Number of data blocks (default: %d)\n", DEFAULT_K);
    printf("  -m <value>    Number of parity blocks (default: %d)\n", DEFAULT_M);
    printf("  -e <value>    Number of erasures (default: %d)\n", DEFAULT_ERASURE_COUNT);
    printf("  -h            Show this help message\n");
}

int main(int argc, char** argv) {
    int k = DEFAULT_K;
    int m = DEFAULT_M;
    int erasure_count = DEFAULT_ERASURE_COUNT;
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) {
            if (i + 1 < argc) {
                k = atoi(argv[++i]);
                if (k <= 0) {
                    fprintf(stderr, "Error: k must be positive\n");
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: -k requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 < argc) {
                m = atoi(argv[++i]);
                if (m <= 0) {
                    fprintf(stderr, "Error: m must be positive\n");
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: -m requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 < argc) {
                erasure_count = atoi(argv[++i]);
                if (erasure_count < 0 || erasure_count > k + m) {
                    fprintf(stderr, "Error: erasure_count must be between 0 and %d\n", k + m);
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: -e requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    printf("Running with k=%d, m=%d, erasure_count=%d\n", k, m, erasure_count);
    
    size_t data_size = DEFAULT_DATA_SIZE;
    void* data;
    size_t alignment = 4096;
    size_t buffer_size = data_size;
    int ret = posix_memalign(&data, alignment, buffer_size);
    if (ret != 0) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return 1;
    }
    writeRandomData(data, data_size);

    // 每个块的大小
    size_t block_size = DEFAULT_BLOCK_SIZE;

    // 检查数据大小是否足够
    if (data_size < k * block_size) {
        fprintf(stderr, "Error: Data size too small for given k and block size\n");
        free(data);
        return 1;
    }

    // 分配数据块和校验块
    char **data_blocks = (char **)malloc(k * sizeof(char *));
    char **coding_blocks = (char **)malloc(m * sizeof(char *));
    if (!data_blocks || !coding_blocks) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        free(data);
        return 1;
    }
    
    for (int i = 0; i < k; i++) {
        data_blocks[i] = (char *)malloc(block_size);
        if (!data_blocks[i]) {
            fprintf(stderr, "Error: Memory allocation failed\n");
            goto cleanup;
        }
        memcpy(data_blocks[i], (char*)data + i * block_size, block_size);
    }
    for (int i = 0; i < m; i++) {
        coding_blocks[i] = (char *)malloc(block_size);
        if (!coding_blocks[i]) {
            fprintf(stderr, "Error: Memory allocation failed\n");
            goto cleanup;
        }
    }

    // 生成编码矩阵
    struct timeval start_time, end_time;
    gettimeofday(&start_time, 0);
    int *matrix_RSvandermode = reed_sol_vandermonde_coding_matrix(k, m, W);
    if (!matrix_RSvandermode) {
        fprintf(stderr, "Error: Failed to generate coding matrix\n");
        goto cleanup;
    }
    gettimeofday(&end_time, 0);
    int time_cost_gen = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                     end_time.tv_usec - start_time.tv_usec;
    printf("matrix gen time %d us\n", time_cost_gen);

    // 进行编码
    gettimeofday(&start_time, 0);
    jerasure_matrix_encode(k, m, W, matrix_RSvandermode, data_blocks, coding_blocks, block_size);
    gettimeofday(&end_time, 0);
    int time_cost_encoding = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                     end_time.tv_usec - start_time.tv_usec;
    printf("matrix encoding time %d us\n", time_cost_encoding);

    // 准备擦除列表 - 默认丢失最后的数据块
    int *erasures = (int *)malloc((erasure_count + 1) * sizeof(int));
    if (!erasures) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        goto cleanup;
    }
    
    // 填充擦除列表：默认丢失最后的数据块
    for (int i = 0; i < erasure_count; i++) {
        // 从数据块的末尾开始标记擦除
        erasures[i] = k - 1 - i;
    }
    erasures[erasure_count] = -1; // 结束标记

    printf("Erasures: ");
    for (int i = 0; i < erasure_count; i++) {
        printf("%d ", erasures[i]);
    }
    printf("\n");

    // 解码恢复数据
    gettimeofday(&start_time, 0);
    int decode_result = jerasure_matrix_decode(k, m, W, matrix_RSvandermode, 1, erasures, data_blocks, coding_blocks, block_size);
    gettimeofday(&end_time, 0);
    
    if (decode_result == 0) {
        int time_cost_decoding = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                               end_time.tv_usec - start_time.tv_usec;
        printf("One stripe matrix decoding time %d us, k=%d, m=%d, W=%d, erasure count = %d\n", 
                time_cost_decoding, k, m, W, erasure_count);
    } else {
        fprintf(stderr, "Error: Decoding failed\n");
    }

cleanup:
    // 清理资源
    free(erasures);
    free(matrix_RSvandermode);
    for (int i = 0; i < k; i++) {
        if (data_blocks[i]) free(data_blocks[i]);
    }
    for (int i = 0; i < m; i++) {
        if (coding_blocks[i]) free(coding_blocks[i]);
    }
    free(data_blocks);
    free(coding_blocks);
    free(data);
    
    return 0;
}