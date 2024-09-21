#include <math.h>
#include <sys/time.h>
#include <openssl/sha.h>
#include <stdio.h>

int main(){
    // args
	int batch_size = 256;
	int unit_size = 8*1024;
	int total_size = batch_size * unit_size;

	// random data gen
	char* data = (char*)malloc(unit_size * batch_size);
	for(int i=0; i<=total_size-1; i++){
		data[i] = 'b' + rand() % 26;
	}

    char sha_256_result[32];
    struct timeval start, end;
    gettimeofday(&start, NULL);
    for(int i=0; i<=batch_size-1; i++){
        SHA256(data + i*unit_size, unit_size, sha_256_result);
    }
    gettimeofday(&end, NULL);
    int cost_time_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
    printf("x86 used time: %d us\n", cost_time_us);
}