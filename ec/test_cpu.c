#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "./Jerasure/include/jerasure.h"
#include "./Jerasure/include/reed_sol.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

#define K 4 // 数据块数量
#define M 2 // 校验块数量
#define W 8 // Galois 域的宽度

int main() {
    // 数据初始化
    // 1 MB 数据
    // 用 'A' 字符填充数据，方便查看
    size_t data_size = 1024 * 1024 * 4; 
    char *data = (char *)malloc(data_size);

    int fd = open("./testInput", O_RDONLY);
    read(fd, data, data_size);

    // 每个块的大小
    size_t block_size = data_size / K;

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
    // int *matrix_RSvandermode = reed_sol_extended_vandermonde_matrix(K, M, W);
    
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

    printf("编码完成:生成%d个数据块和%d个校验块\n", K, M);
    for(int i=0; i<=K-1; i++){
        printf("数据块 %d 内容:\n", i);
        for(int j=0; j<=10;j++)
            printf("%d ", data_blocks[i][j]);
        printf("\n");
    }
    for(int i=0; i<=M-1; i++){
        printf("校验块 %d 内容:\n", i);
        for(int j=0; j<=10;j++)
            printf("%d ", coding_blocks[i][j]);
        printf("\n");
    }

    // 恢复测试
    // data   0 1 2 3 
    // parity 4 5
    int erasures[] = {3, 4, -1}; 
    memset(data_blocks[3], 0, block_size); 
    memset(coding_blocks[0], 0, block_size); 

    // 解码恢复数据
    jerasure_matrix_decode(K, M, W, matrix_RSvandermode, 1, erasures, data_blocks, coding_blocks, block_size);
    printf("数据恢复,数据块3内容:\n");
    for(int i=0; i<=10;i++)
        printf("%d ", data_blocks[3][i]);
    printf("\n");
    printf("数据恢复,校验块0内容:\n");
    for(int i=0; i<=10;i++)
        printf("%d ", coding_blocks[0][i]);
    printf("\n");

    // 释放内存
    free(data);
    for (int i = 0; i < K; i++) free(data_blocks[i]);
    for (int i = 0; i < M; i++) free(coding_blocks[i]);
    free(data_blocks);
    free(coding_blocks);
    free(matrix_RSvandermode);

    return 0;
}
