#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "./Jerasure/include/jerasure.h"
#include "./Jerasure/include/reed_sol.h"
#include "./Jerasure/include/cauchy.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

#include "dpuProxy.h"

#define K 4 // 数据块数量
#define M 2 // 校验块数量
#define W 8 // Galois 域的宽度
#define MB (1*1024*1024)
#define KB (1024*1024)

int main() {
    struct timeval start_time, end_time;
    gettimeofday(&start_time, 0);
    int *matrix_RSvandermode = reed_sol_vandermonde_coding_matrix(K, M, W);
    gettimeofday(&end_time, 0);
    int time_cost_gen = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                     end_time.tv_usec - start_time.tv_usec;
    printf("matrix gen time %d us\n", time_cost_gen);


    int fd = open("./testInput", O_RDONLY);
    lseek(fd, 0, SEEK_END);
    // size_t data_size = lseek(fd, 0, SEEK_CUR);
    size_t data_size = 64*MB;
    char *data = (char *)malloc(data_size);
    read(fd, data, data_size);

    // 分配数据块和校验块
    int block_size = MB;
    int stripe_size = block_size * K;
    char **data_blocks = (char **)malloc(K * sizeof(char *));
    char **coding_blocks = (char **)malloc(M * sizeof(char *));
    for (int i = 0; i < K; i++) 
        data_blocks[i] = (char *)malloc(block_size);
    for (int i = 0; i < M; i++) 
        coding_blocks[i] = (char *)malloc(block_size);
    
    // CPU 计算
    int data_off = 0;
    int cpu_encoding_time = 0;
    printf("block size = %d\n", block_size);
    for(int i=0; i<=data_size/stripe_size -1;i++){
        // prepare stripe
        for (int j = 0; j < K; j++) {
            memcpy(data_blocks[j], data + data_off, block_size);
            data_off += block_size;
        }

        gettimeofday(&start_time, 0);
        jerasure_matrix_encode(K, M, W, matrix_RSvandermode, data_blocks, coding_blocks, block_size);
        gettimeofday(&end_time, 0);
        cpu_encoding_time += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                        end_time.tv_usec - start_time.tv_usec;
    }
    printf("CPU encoding time %d us\n", cpu_encoding_time);
    printf("CPU encoding throughput %.2f MB/s\n", ((float)data_size / MB) / ((float)cpu_encoding_time / 1000000));
    
    // DPU 加速
    DPUProxy *dpu;
    dpu = new DPUProxy();
    gettimeofday(&start_time, 0);
    dpu->initEC(K, M, block_size);
    gettimeofday(&end_time, 0);
    int init_ec_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                        end_time.tv_usec - start_time.tv_usec;
    printf("DPU accelerator init time %d us\n", init_ec_time);
    
    int dpu_encoding_time = 0;
    data_off=0;
    for(int i=0; i<=data_size/stripe_size -1;i++){
        // prepare stripe
        for (int j = 0; j < K; j++) {
            memcpy(data_blocks[j], data + data_off, block_size);
            data_off += block_size;
        }

        // encode stripe
        gettimeofday(&start_time, 0);
        dpu->encode_chunks(data_blocks, coding_blocks, block_size);
        gettimeofday(&end_time, 0);
        dpu_encoding_time += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                        end_time.tv_usec - start_time.tv_usec;
    }
    printf("DPU accelerator copy time %d us\n", dpu->copy_time_us);
    printf("DPU accelerator encoding time %d us\n", dpu_encoding_time);

    printf("Save time FAKE %d us\n", cpu_encoding_time - (dpu_encoding_time-dpu->copy_time_us));
    printf("DPU accelerator encoding throughput FAKE %.2f MB/s\n", ((float)data_size / MB) / (((float)dpu_encoding_time-(float)dpu->copy_time_us) / 1000000));
    printf("DPU accelerator encoding throughput REAL %.2f MB/s\n", ((float)data_size / MB) / ((float)dpu_encoding_time / 1000000));

    return 0;
}
