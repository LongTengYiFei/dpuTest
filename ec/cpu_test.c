#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "./Jerasure/include/jerasure.h"
#include "./Jerasure/include/reed_sol.h"

#define K 4 // 数据块数量
#define M 2 // 校验块数量
#define W 8 // Galois 域的宽度

int main() {
    // 数据初始化
    // 1 MB 数据
    // 用 'A' 字符填充数据，方便查看
    size_t data_size = 1024 * 1024; 
    char *data = (char *)malloc(data_size);
    for(int i=0, off=0; i<=K-1; i++, off+=(data_size/K)){
        memset(data+off, 'A'+i, (data_size/K)); 
    }
    
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

    // 生成编码矩阵
    int *matrix_RSvandermode = reed_sol_vandermonde_coding_matrix(K, M, W);
    int *matrix_RScauchy = reed_sol_cauchy_coding_matrix(K, M, W);

    // 进行编码
    jerasure_matrix_encode(K, M, W, matrix_RSvandermode, data_blocks, coding_blocks, block_size);

    printf("编码完成:生成%d个数据块和%d个校验块\n", K, M);
    for(int i=0; i<=K-1; i++){
        printf("数据恢复,数据块0内容:\n");
        for(int j=0; j<=100;j++)
            printf("%d ", data_blocks[i][j]);
        printf("\n");
    }

    // 示例：恢复一个丢失的数据块
    int erasures[] = {1, -1}; // 假设数据块1丢失
    memset(data_blocks[1], 0, block_size); // 将数据块1清空模拟丢失
    printf("数据丢失,数据块1内容:\n");
    for(int i=0; i<=10;i++)
        printf("%d ", data_blocks[1][i]);
    printf("\n");

    // 解码恢复数据
    jerasure_matrix_decode(K, M, W, matrix_RSvandermode, 1, erasures, data_blocks, coding_blocks, block_size);
    printf("数据恢复,数据块1内容:\n");
    for(int i=0; i<=10;i++)
        printf("%d ", data_blocks[1][i]);
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
