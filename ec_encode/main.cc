#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jerasure.h>
#include <jerasure/reed_sol.h>
#include <jerasure/cauchy.h>

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/sha.h>

#include "dpuProxy.h"

#define K 4 // 数据块数量
#define M 2 // 校验块数量
#define W 8 // Galois 域的宽度
#define MB (1*1024*1024)
#define KB (1024)
#define BLOCK_SIZE MB

using namespace std;

void cpuEncode(int k, int m, int block_size){
    struct timeval start_time, end_time;
    gettimeofday(&start_time, 0);
    int *matrix_RSvandermode = reed_sol_vandermonde_coding_matrix(k, m, W);
    gettimeofday(&end_time, 0);
    int time_cost_gen = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                     end_time.tv_usec - start_time.tv_usec;
    // printf("matrix gen time %d us\n", time_cost_gen);

    int data_size = k*block_size;
    char *input_data = (char *)malloc(data_size);

    // write random data
    for(int i=0; i<data_size; i++){
        input_data[i] = rand() % 256;
    }

    // allocate data block and code block space
    int stripe_size = block_size * k;
    char **data_blocks = (char **)malloc(k * sizeof(char *));
    char **coding_blocks = (char **)malloc(m* sizeof(char *));
    for (int i = 0; i < k; i++) 
        data_blocks[i] = (char *)malloc(block_size);
    for (int i = 0; i < m; i++) 
        coding_blocks[i] = (char *)malloc(block_size);
    
    // CPU encode and decode
    int erasures[] = {1, k, -1};
    int data_off = 0;
    int cpu_encoding_time = 0;
    int cpu_decoding_time = 0;

    printf("block size = %d\n", block_size);
    for(int i=0; i<=data_size/stripe_size -1;i++){
        /*encode*/
        for (int j = 0; j < k; j++) {
            memcpy(data_blocks[j], input_data + data_off, block_size);
            data_off += block_size;
        }

        gettimeofday(&start_time, 0);
        jerasure_matrix_encode(k, m, W, matrix_RSvandermode, data_blocks, coding_blocks, block_size);
        gettimeofday(&end_time, 0);
        cpu_encoding_time += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                        end_time.tv_usec - start_time.tv_usec;

        // writeBlocks("./dataBlocks", true, k, data_blocks, block_size); 
        // writeBlocks("./codeBlocks", false, m, coding_blocks, block_size);           

        /*decode*/
        // compute data block sha1
        // unsigned char sha1[SHA_DIGEST_LENGTH];
        // printf("original sha1\n");
        // SHA1((unsigned char*)data_blocks[1], block_size, sha1);
        // printf("SHA1 of data block 1: ");
        // for (int a = 0; a < SHA_DIGEST_LENGTH; a++)
        //     printf("%02x", sha1[a]);
        // printf("\n");

        // SHA1((unsigned char*)coding_blocks[0], block_size, sha1);
        // printf("SHA1 of code block 0: ");
        // for (int a = 0; a < SHA_DIGEST_LENGTH; a++)
        //     printf("%02x", sha1[a]);
        // printf("\n");
        
        // memset(data_blocks[1], 0, block_size);  // 模拟丢失  数据块 1
        // memset(coding_blocks[0], 0, block_size);    // 模拟丢失 校验块 0
        // gettimeofday(&start_time, 0);
        // jerasure_matrix_decode(k, m, w, matrix_RSvandermode, 0, erasures, data_blocks, coding_blocks, block_size);
        // gettimeofday(&end_time, 0);
        // cpu_decoding_time += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
        //                 end_time.tv_usec - start_time.tv_usec;

        // // 恢复后的SHA1值
        // printf("restore sha1\n");
        // SHA1((unsigned char*)data_blocks[1], block_size, sha1);
        // printf("SHA1 of data block 1: ");
        // for (int a = 0; a < SHA_DIGEST_LENGTH; a++)
        //     printf("%02x", sha1[a]);
        // printf("\n");

        // SHA1((unsigned char*)coding_blocks[0], block_size, sha1);
        // printf("SHA1 of code block 0: ");
        // for (int a = 0; a < SHA_DIGEST_LENGTH; a++)
        //     printf("%02x", sha1[a]);
        // printf("\n");
    }

    printf("CPU encoding time %d us\n", cpu_encoding_time);
    float cpu_throughput = ((float)data_size / MB) / ((float)cpu_encoding_time / 1000000);
    printf("CPU encoding throughput %.2f MB/s\n", cpu_throughput);

}

void dpuEncodeBatch(int k, int m, int block_size, int batch_size){
    struct timeval start_time, end_time;
    // DPU prepare
    DPUProxy *dpu;
    dpu = new DPUProxy();
    dpu->initECBatch(k, m, block_size, batch_size);
    
    int data_size = k*block_size*batch_size;
    char *input_data = (char *)malloc(data_size);

    // write random data
    for(int i=0; i<data_size; i++){
        input_data[i] = rand();
    }

    // allocate data block and code block space
    long stripe_size = block_size * k;

    for(int i=0; i<=data_size/stripe_size/batch_size -1; i++){
        dpu->encode_chunks(input_data + batch_size*stripe_size, block_size, k, batch_size);
    }

    float DOCA_Raw_Perf = ((float)data_size / MB) / ((float)dpu->getECBatchProcessTime() / 1000000);
    float DOCA_Proxy_Perf = 
        ((float)data_size / MB) / (((float)dpu->getECBatchProcessTime() + (float)dpu->getECBatchMemcpyTime()) / 1000000);
    printf("Data Size %d MB\n", data_size / MB);
    printf("DOCA Raw Encode Performance %.2f MB/s\n", DOCA_Raw_Perf);
    printf("DOCA Proxy Encode Performance %.2f MB/s\n", DOCA_Proxy_Perf);
    printf("DOCA Polling Encoding Time %d us\n", dpu->getECBatchProcessTime());
    printf("DOCA Memcpy Encoding Time %d us\n", dpu->getECBatchMemcpyTime());
}


int size_convert(char* s) {
    double num = 0;
    char unit[3] = {0};
    int i = 0, j = 0;
    
    // 提取数字部分
    while (s[i] && (isdigit(s[i]) || s[i] == '.')) {
        i++;
    }
    
    // 将数字部分转换为double
    char num_str[32] = {0};
    strncpy(num_str, s, i);
    num = atof(num_str);
    
    // 提取单位部分并转换为大写
    while (s[i] && j < 2) {
        if (isalpha(s[i])) {
            unit[j++] = toupper(s[i]);
            i++;
        } else {
            i++;
        }
    }
    
    // 根据单位计算大小
    if (strcmp(unit, "KB") == 0) {
        return (int)(num * 1024);
    } else if (strcmp(unit, "MB") == 0) {
        return (int)(num * 1024 * 1024);
    } else if (strcmp(unit, "GB") == 0) {
        return (int)(num * 1024 * 1024 * 1024);
    } else if (strcmp(unit, "B") == 0 || unit[0] == '\0') {
        return (int)num;
    } else {
        // 未知单位，可以根据需要处理错误
        return -1;
    }
}

int main(int argc, char** argv) {
    char* method = argv[1];
    int k = atoi(argv[2]);
    int m = atoi(argv[3]);
    int block_size = size_convert(argv[4]);

    int batch_size;
    if(argv[5])
        batch_size = atoi(argv[5]);

    if(strcmp(method, "dpuEncodeBatch") == 0){
        // batch处理，k=4，m=2，block大小1MB，batch大小32；
        // 使用方法：./proxy_test dpuBatch 4 2 1MB 32
        dpuEncodeBatch(k, m, block_size, batch_size);
    }else if(strcmp(method, "cpuEncode") == 0){
        // 使用方法：./proxy_test cpu 4 2 1MB
        cpuEncode(k, m, block_size);
    }else {
        printf("Unknown method\n");
    }
    
    return 0;
}