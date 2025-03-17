#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <compressed file> <decompressed file>\n", argv[0]);
        return 1;
    }

    // 打开压缩文件
    FILE *source = fopen(argv[1], "rb");
    if (!source) {
        fprintf(stderr, "Failed to open compressed file.\n");
        return 1;
    }

    // 获取压缩文件大小
    fseek(source, 0, SEEK_END);
    size_t compressed_size = ftell(source);
    fseek(source, 0, SEEK_SET);

    // 读取压缩文件到内存
    unsigned char *compressed_data = (unsigned char *)malloc(compressed_size);
    if (!compressed_data) {
        fprintf(stderr, "Failed to allocate memory for compressed data.\n");
        fclose(source);
        return 1;
    }
    fread(compressed_data, 1, compressed_size, source);
    fclose(source);

    // 预估解压后数据的大小（通常为压缩数据的 5-10 倍）
    size_t uncompressed_size = compressed_size * 50;
    unsigned char *uncompressed_data = (unsigned char *)malloc(uncompressed_size);
    if (!uncompressed_data) {
        fprintf(stderr, "Failed to allocate memory for uncompressed data.\n");
        free(compressed_data);
        return 1;
    }

    // 使用 uncompress 接口一次性解压
    int ret = uncompress(uncompressed_data, &uncompressed_size, compressed_data, compressed_size);
    if (ret != Z_OK) {
        fprintf(stderr, "Decompression failed: %s\n",
                ret == Z_MEM_ERROR ? "Not enough memory" :
                ret == Z_BUF_ERROR ? "Output buffer too small" :
                ret == Z_DATA_ERROR ? "Corrupted input data" : "Unknown error");
        free(compressed_data);
        free(uncompressed_data);
        return 1;
    }

    // 写入解压后的数据到文件
    FILE *dest = fopen(argv[2], "wb");
    if (!dest) {
        fprintf(stderr, "Failed to open decompressed file.\n");
        free(compressed_data);
        free(uncompressed_data);
        return 1;
    }
    fwrite(uncompressed_data, 1, uncompressed_size, dest);
    fclose(dest);

    // 释放内存
    free(compressed_data);
    free(uncompressed_data);

    printf("Decompression successful!\n");
    return 0;
}