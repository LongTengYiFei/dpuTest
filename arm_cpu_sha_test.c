#include <sys/time.h>
#include <sys/mman.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#define GB 1073741824ll
#define MB 1048576ll

int main(){
    char * data = (char*)malloc(sizeof(char)*GB);
    int fd = open("./testfile/linux-6.7-rc2.tar", O_RDWR);
    if((data = mmap(NULL, GB, PROT_READ| PROT_WRITE, MAP_SHARED, fd, 0)) ==(void*) -1){
           perror("mmap") ;
    }  

    int block_size = 4*1024;
    int block_num = 128; 

    unsigned char digest[SHA512_DIGEST_LENGTH];
    struct timeval start,end;
    for(int i=0; i<=block_num-1; i++){
        gettimeofday(&start, 0); 
        SHA512_CTX shactx;
        SHA512_Init(&shactx);
        SHA512_Update(&shactx, data+i*block_size, block_size);
        SHA512_Final(digest, &shactx);
        gettimeofday(&end, 0); 

        printf("SHA512: ");
        for (int j = 0; j < SHA512_DIGEST_LENGTH; j++)
			printf("%02x", digest[j]);
        printf("\n");
    }
    
	long timeuse = 1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec;  
	printf("time = %ld us\n", timeuse);  
    return 0;
}