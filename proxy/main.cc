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

void writeBlocks(string blocks_dir, bool isDataBlock, int blocks_num, char** blocks, int block_size){
    string block_file_name_prefix;
    if(isDataBlock){
        block_file_name_prefix = "data_block_";
    }else{
        block_file_name_prefix = "code_block_";
    }

    string block_file_name = blocks_dir + "/" + block_file_name_prefix;
    for(int i=0; i<=blocks_num-1; i++){
        block_file_name.push_back(i + '0');
        int fd = open(block_file_name.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
        write(fd, blocks[i], block_size);
        close(fd);
        block_file_name.pop_back();
    }
}

void cpuTest(string input_file_path, int k, int m, int w, int block_size){
    struct timeval start_time, end_time;
    gettimeofday(&start_time, 0);
    int *matrix_RSvandermode = reed_sol_vandermonde_coding_matrix(K, M, W);
    gettimeofday(&end_time, 0);
    int time_cost_gen = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                     end_time.tv_usec - start_time.tv_usec;
    printf("matrix gen time %d us\n", time_cost_gen);

    int fd = open(input_file_path.c_str(), O_RDONLY);
    lseek(fd, 0, SEEK_END);
    size_t data_size = lseek(fd, 0, SEEK_CUR);
    lseek(fd, 0, SEEK_SET);
    if(data_size % (k * block_size)){
        cout<< "文件长度不是K*block size的整数倍" <<endl;
        cout<< "k = " << k <<endl;
        cout<< "block size = " << block_size <<endl;
        return ;
    }
    char *input_data = (char *)malloc(data_size);
    read(fd, input_data, data_size);

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
        jerasure_matrix_encode(k, m, w, matrix_RSvandermode, data_blocks, coding_blocks, block_size);
        gettimeofday(&end_time, 0);
        cpu_encoding_time += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                        end_time.tv_usec - start_time.tv_usec;

        writeBlocks("./dataBlocks", true, k, data_blocks, block_size); 
        writeBlocks("./codeBlocks", false, m, coding_blocks, block_size);           

        /*decode*/
        // compute data block sha1
        unsigned char sha1[SHA_DIGEST_LENGTH];
        printf("original sha1\n");
        SHA1((unsigned char*)data_blocks[1], block_size, sha1);
        printf("SHA1 of data block 1: ");
        for (int a = 0; a < SHA_DIGEST_LENGTH; a++)
            printf("%02x", sha1[a]);
        printf("\n");

        SHA1((unsigned char*)coding_blocks[0], block_size, sha1);
        printf("SHA1 of code block 0: ");
        for (int a = 0; a < SHA_DIGEST_LENGTH; a++)
            printf("%02x", sha1[a]);
        printf("\n");
        
        memset(data_blocks[1], 0, block_size);  // 模拟丢失  数据块 1
        memset(coding_blocks[0], 0, block_size);    // 模拟丢失 校验块 0
        gettimeofday(&start_time, 0);
        jerasure_matrix_decode(k, m, w, matrix_RSvandermode, 0, erasures, data_blocks, coding_blocks, block_size);
        gettimeofday(&end_time, 0);
        cpu_decoding_time += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                        end_time.tv_usec - start_time.tv_usec;

        // 恢复后的SHA1值
        printf("restore sha1\n");
        SHA1((unsigned char*)data_blocks[1], block_size, sha1);
        printf("SHA1 of data block 1: ");
        for (int a = 0; a < SHA_DIGEST_LENGTH; a++)
            printf("%02x", sha1[a]);
        printf("\n");

        SHA1((unsigned char*)coding_blocks[0], block_size, sha1);
        printf("SHA1 of code block 0: ");
        for (int a = 0; a < SHA_DIGEST_LENGTH; a++)
            printf("%02x", sha1[a]);
        printf("\n");
    }

    printf("CPU encoding time %d us\n", cpu_encoding_time);
    float cpu_throughput = ((float)data_size / MB) / ((float)cpu_encoding_time / 1000000);
    printf("CPU encoding throughput %.2f MB/s\n", cpu_throughput);

}

void dpuEncodeTest(string input_file_path, int k, int m, int w, int block_size){
    struct timeval start_time, end_time;
    // DPU prepare
    DPUProxy *dpu;
    dpu = new DPUProxy();
    gettimeofday(&start_time, 0);
    dpu->initEC(k, m, block_size);
    gettimeofday(&end_time, 0);
    int init_ec_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                        end_time.tv_usec - start_time.tv_usec;
    printf("DPU accelerator init time %d us\n", init_ec_time);
    
    // file prepare
    int fd = open(input_file_path.c_str(), O_RDONLY);
    lseek(fd, 0, SEEK_END);
    size_t data_size = lseek(fd, 0, SEEK_CUR);
    if(data_size % (k * block_size)){
        cout<< "文件长度不是K*block size的整数倍" <<endl;
        cout<< "k = " << k <<endl;
        cout<< "block size = " << block_size <<endl;
        return ;
    }
    char *input_data = (char *)malloc(data_size);
    read(fd, input_data, data_size);

    // allocate data block and code block space
    int stripe_size = block_size * k;
    char **data_blocks = (char **)malloc(k * sizeof(char *));
    char **coding_blocks = (char **)malloc(m* sizeof(char *));
    for (int i = 0; i < k; i++) 
        data_blocks[i] = (char *)malloc(block_size);
    for (int i = 0; i < m; i++) 
        coding_blocks[i] = (char *)malloc(block_size);

    int dpu_encoding_time = 0;
    int data_off=0;
    for(int i=0; i<=data_size/stripe_size -1;i++){
        // prepare stripe
        for (int j = 0; j < K; j++) {
            memcpy(data_blocks[j], input_data + data_off, block_size);
            data_off += block_size;
        }

        // encode stripe
        gettimeofday(&start_time, 0);
        dpu->encode_chunks(data_blocks, coding_blocks, block_size);
        gettimeofday(&end_time, 0);
        dpu_encoding_time += (end_time.tv_sec - start_time.tv_sec) * 1000000 + 
                        end_time.tv_usec - start_time.tv_usec;

        writeBlocks("./dataBlocks", true, k, data_blocks, block_size); 
        writeBlocks("./codeBlocks", false, m, coding_blocks, block_size);        
    }
    
    printf("DPU accelerator total encoding time %d us\n", dpu_encoding_time);
    float dpu_real_thr = ((float)data_size / MB) / ((float)dpu_encoding_time / 1000000);
    printf("DPU accelerator encoding throughput REAL %.2f MB/s\n", dpu_real_thr);
}

void dpuEncodeTestBatch(string input_file_path, int k, int m, int w, int block_size, int batch_size){
    struct timeval start_time, end_time;
    // DPU prepare
    DPUProxy *dpu;
    dpu = new DPUProxy();
    dpu->initECBatch(k, m, block_size, batch_size);
    
    // file prepare
    int fd = open(input_file_path.c_str(), O_RDONLY);
    lseek(fd, 0, SEEK_END);
    size_t data_size = lseek(fd, 0, SEEK_CUR);
    if(data_size % (k * block_size)){
        cout<< "文件长度不是K*block size的整数倍" <<endl;
        cout<< "k = " << k <<endl;
        cout<< "block size = " << block_size <<endl;
        return ;
    }
    char *input_data = (char *)malloc(data_size);
    read(fd, input_data, data_size);

    // allocate data block and code block space
    int stripe_size = block_size * k;

    for(int i=0; i<=data_size/stripe_size/batch_size -1;i++){
        dpu->encode_chunks(input_data + batch_size*stripe_size, block_size, k, batch_size);
    }

    float dpu_real_thr = ((float)data_size / MB) / ((float)dpu->getECBatchProcessTime() / 1000000);
    printf("data size %d MB\n", data_size / MB);
    printf("DPU accelerator encoding throughput REAL %.2f MB/s\n", dpu_real_thr);
    printf("DPU accelerator total encoding time %d us\n", dpu->getECBatchProcessTime());
}

int main(int argc, char** argv) {
    char* file_name = argv[1];
    if(strcmp(argv[2], "dpu") == 0){
        dpuEncodeTest(file_name, K, M, W, BLOCK_SIZE);
    }else if (strcmp(argv[2], "dpuBatch") == 0){
        int batch_size = atoi(argv[3]);
        dpuEncodeTestBatch(file_name, K, M, W, BLOCK_SIZE, batch_size);
    }else if(strcmp(argv[2], "cpu") == 0){
        cpuTest(file_name, K, M, W, BLOCK_SIZE);
    }
    
    return 0;
}