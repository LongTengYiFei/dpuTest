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
    int min_level = 6;
    int max_level = 6;
    for(int level=min_level; level<=max_level; level++){
        gettimeofday(&start_time, NULL);
        compress2(compressed_data, &compressed_len, file_data, file_length, level);
        gettimeofday(&end_time, NULL);
        int cost_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec); 
        printf("level %d cost time ms %d\n", level, cost_time / 1000);
    }

    printf("src data len %ld\n", file_length);
    printf("comrpessed data len %ld\n", compressed_len);

    return 0;
}