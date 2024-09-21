#include <math.h>
#include <sys/time.h>
#include <openssl/sha.h>
#include <stdio.h>
#define GB (1024*1024*1024)
int main(){
    // args
    int total_size = GB;
    int unit_size = 16*1024;
	int batch_num = total_size / unit_size;

	// random data gen
	char* data = (char*)malloc(unit_size);
	for(int i=0; i<=unit_size-1; i++){
		data[i] = 'b' + rand() % 26;
	}

    struct timeval start, end;
    char sha_1_result[SHA_DIGEST_LENGTH];
    gettimeofday(&start, NULL);
    for(int i=0; i<=batch_num-1; i++){
        SHA1(data, unit_size, sha_1_result);
    }
    gettimeofday(&end, NULL);
    int cost_time_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
    printf("sha1 used time: %d ms\n", cost_time_us / 1000);

    char sha_256_result[SHA256_DIGEST_LENGTH];
    gettimeofday(&start, NULL);
    for(int i=0; i<=batch_num-1; i++){
        SHA256(data, unit_size, sha_256_result);
    }
    gettimeofday(&end, NULL);
    cost_time_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
    printf("sha256 used time: %d ms\n", cost_time_us / 1000);

    char sha_512_result[SHA512_DIGEST_LENGTH];
    gettimeofday(&start, NULL);
    for(int i=0; i<=batch_num-1; i++){
        SHA512(data, unit_size, sha_512_result);
    }
    gettimeofday(&end, NULL);
    cost_time_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
    printf("sha512 used time: %d ms\n", cost_time_us / 1000);
}