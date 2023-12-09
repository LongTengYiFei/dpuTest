#include <sys/time.h>
#include <sys/mman.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#define GB 1073741824ll
#define MB 1048576ll

long long timeuse_comrpession = 0;  
long long timeuse_decompression = 0;
struct timeval start,end;

int compress_data(const char* input, size_t input_size, char** compressed_data, size_t* compressed_size) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));

    if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK) {
        return -1; // Compression initialization failed
    }

    stream.next_in = (Bytef*)input;
    stream.avail_in = (uInt)input_size;

    size_t buffer_size = 2 * input_size; // Adjust this as needed
    *compressed_data = (char*)malloc(buffer_size);

    stream.next_out = (Bytef*)*compressed_data;
    stream.avail_out = (uInt)buffer_size;

    gettimeofday(&start, 0);
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        free(*compressed_data);
        deflateEnd(&stream);
        return -1; // Compression failed
    }
    gettimeofday(&end, 0);
    timeuse_comrpession += 1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec; 

    *compressed_size = buffer_size - stream.avail_out;
    deflateEnd(&stream);
    return 0; // Compression succeeded
}

int decompress_data(const char* compressed_data, size_t compressed_size, char** decompressed_data, size_t* decompressed_size) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));

    if (inflateInit(&stream) != Z_OK) {
        return -1; // Decompression initialization failed
    }

    stream.next_in = (Bytef*)compressed_data;
    stream.avail_in = (uInt)compressed_size;

    size_t buffer_size = 2 * compressed_size; // Adjust this as needed
    *decompressed_data = (char*)malloc(buffer_size);

    stream.next_out = (Bytef*)*decompressed_data;
    stream.avail_out = (uInt)buffer_size;

    gettimeofday(&start, 0);
    if (inflate(&stream, Z_FINISH) != Z_STREAM_END) {
        free(*decompressed_data);
        inflateEnd(&stream);
        return -1; // Decompression failed
    }
    gettimeofday(&end, 0);
    timeuse_decompression += 1000000 * ( end.tv_sec - start.tv_sec ) + end.tv_usec - start.tv_usec; 

    *decompressed_size = buffer_size - stream.avail_out;
    inflateEnd(&stream);
    return 0; // Decompression succeeded
}


int main(){
    int mmap_legnth = 1*GB;
    char * data = (char*)malloc(sizeof(char)*mmap_legnth);
    int fd = open("./testfile/linux-6.7-rc2.tar", O_RDWR);
    if((data = mmap(NULL, mmap_legnth, PROT_READ| PROT_WRITE, MAP_SHARED, fd, 0)) ==(void*) -1){
           perror("mmap") ;
    }  

    int file_size = lseek(fd, 0, SEEK_END);;
    int min_block_size = 1024;
    int max_block_size = 64*1024*1024;

    for(int bs=min_block_size; bs<=max_block_size; bs*=2){
        int block_num = file_size / bs + 1;

        for(int i=0; i<=block_num-1; i++){
            char* compressed_data;
            size_t compressed_size;
            compress_data(data+bs*i, bs, &compressed_data, &compressed_size);
            //printf("compressed_size %ld, ", compressed_size);

            char* decompressed_data;
            size_t decompressed_size;
            decompress_data(compressed_data, compressed_size, &decompressed_data, &decompressed_size);
            //printf("decompressed_size %ld\n", decompressed_size);
        }

        /* time */
        printf("Block size %d\n", bs);
        printf("time used for compression = %lld us\n", timeuse_comrpession);  
        printf("time used for decompression = %lld us\n", timeuse_decompression); 
        printf("\n");
        timeuse_comrpession = 0;
        timeuse_decompression = 0;
    }

    return 0;
}