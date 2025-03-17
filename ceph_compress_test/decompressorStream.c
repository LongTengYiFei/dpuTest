#include <stdio.h>
#include <string.h>
#include <zlib.h>

#define CHUNK 16384

int inflate_file(FILE *source, FILE *dest) {
    int ret;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    /* 初始化 zlib 流 */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit(&strm);  // 初始化解压缩
    if (ret != Z_OK)
        return ret;

    /* 解压缩数据 */
    do {
        strm.avail_in = fread(in, 1, CHUNK, source);
        if (ferror(source)) {
            (void)inflateEnd(&strm);
            return Z_ERRNO;
        }
        if (strm.avail_in == 0)
            break;
        strm.next_in = in;

        /* 解压缩并输出到目标文件 */
        do {
            strm.avail_out = CHUNK;
            strm.next_out = out;
            ret = inflate(&strm, Z_NO_FLUSH);  // 解压缩数据
            switch (ret) {
                case Z_NEED_DICT:
                    ret = Z_DATA_ERROR;  // 数据错误
                case Z_DATA_ERROR:
                case Z_MEM_ERROR:
                    (void)inflateEnd(&strm);
                    return ret;
            }
            have = CHUNK - strm.avail_out;
            if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
                (void)inflateEnd(&strm);
                return Z_ERRNO;
            }
        } while (strm.avail_out == 0);
    } while (ret != Z_STREAM_END);

    /* 清理并返回 */
    (void)inflateEnd(&strm);
    return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

int main(int argc, char **argv) {
    FILE *source, *dest;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <compressed file> <decompressed file>\n", argv[0]);
        return 1;
    }

    source = fopen(argv[1], "rb");
    dest = fopen(argv[2], "wb");

    if (!source || !dest) {
        fprintf(stderr, "Failed to open files.\n");
        return 1;
    }

    if (inflate_file(source, dest) != Z_OK) {
        fprintf(stderr, "Decompression failed.\n");
        return 1;
    }

    fclose(source);
    fclose(dest);

    return 0;
}