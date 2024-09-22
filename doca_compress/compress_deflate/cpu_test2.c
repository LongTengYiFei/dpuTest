#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define KB (1024UL)
#define MB (1024UL*KB)
#define GB (1024UL*MB)

int main(int argc, char const *argv[]){
    // read file data
    char* file_name = argv[1];
    FILE* fp = fopen(file_name,"rb");
    if(fp == NULL){
        printf("open file error\n");
        exit(0);
    }

    // get file length
    fseek(fp, 0, SEEK_END);
    unsigned long file_length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    u_int8_t* file_data = (u_int8_t*)malloc(file_length);
    fread(file_data, 1, file_length, fp);
    fclose(fp);

    unsigned long compressed_len = 4*GB;
    u_int8_t* compressed_data = (u_int8_t*)malloc(compressed_len);

    struct timeval start_time, end_time;
    int block_size = MB;
    long long cost_time_us = 0;
    unsigned long total_compressed_len = 0;
    int level = 6;
    for(int i=0; i<=file_length/block_size-1; i++){
        // 这里需要reset compressed_len，不然下一次压缩使用的是上次压缩后的长度，以为缓冲区变小了
        // 可能会导致压缩错误，时间和长度统计出错
        compressed_len = 4*GB; 
        gettimeofday(&start_time, NULL);
        compress2(compressed_data, &compressed_len, file_data + i*block_size, block_size, level);
        gettimeofday(&end_time, NULL);
        cost_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec); 
        total_compressed_len += compressed_len;
    }
    printf("Segment method:\n");
    printf("level %d cost time ms %lld\n", level, cost_time_us / 1000);
    printf("src data len %ld\n", file_length);
    printf("comrpessed data len %ld\n", total_compressed_len);

    cost_time_us = 0;
    total_compressed_len = 0;
    compressed_len = 4*GB;
    gettimeofday(&start_time, NULL);
    compress2(compressed_data, &compressed_len, file_data, file_length, level);
    gettimeofday(&end_time, NULL);
    cost_time_us += (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec); 
    total_compressed_len += compressed_len;
    printf("Whole file method:\n");
    printf("level %d cost time ms %lld\n", level, cost_time_us / 1000);
    printf("src data len %ld\n", file_length);
    printf("comrpessed data len %ld\n", total_compressed_len);
    return 0;
}