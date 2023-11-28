#include <sys/time.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#define GB 1073741824ll
#define MB 1048576ll

int main(){
    struct timeval start,end;
    int fd = open("./testfile/linux-6.7-rc2.tar", O_RDWR);
    int src_len = 4*1024*8;  
    char * data = (char*)malloc(sizeof(char)*src_len);
    long long n_read = read(fd, data, src_len);
    
    printf("n_read = %lld\n", n_read);
    unsigned char digest[SHA512_DIGEST_LENGTH];

    gettimeofday(&start, 0); 
    SHA512_CTX shactx;
    SHA512_Init(&shactx);
    SHA512_Update(&shactx, data, n_read);
    SHA512_Final(digest, &shactx);
    gettimeofday(&end, 0);  

	long timeuse = 1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec;  
	printf("time = %ld us\n", timeuse);  
    return 0;
}