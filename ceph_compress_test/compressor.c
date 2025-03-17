#include <stdio.h>
#include <string.h>
#include <zlib.h>

#define CHUNK 16384

int deflate_file(FILE *source, FILE *dest, int level) {
    int ret, flush;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    /* 初始化 zlib 流 */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, level);
    if (ret != Z_OK)
        return ret;

    /* 压缩数据 */
    do {
        strm.avail_in = fread(in, 1, CHUNK, source);
        if (ferror(source)) {
            (void)deflateEnd(&strm);
            return Z_ERRNO;
        }
        flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = in;

        /* 压缩并输出到目标文件 */
        do {
            strm.avail_out = CHUNK;
            strm.next_out = out;
            ret = deflate(&strm, flush);    /* 压缩数据 */
            have = CHUNK - strm.avail_out;
            if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
                (void)deflateEnd(&strm);
                return Z_ERRNO;
            }
        } while (strm.avail_out == 0);
    } while (flush != Z_FINISH);

    /* 清理并返回 */
    (void)deflateEnd(&strm);
    return Z_OK;
}

int main(int argc, char **argv) {
    FILE *source, *dest;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source> <dest>\n", argv[0]);
        return 1;
    }

    source = fopen(argv[1], "rb");
    dest = fopen(argv[2], "wb");

    if (!source || !dest) {
        fprintf(stderr, "Failed to open files.\n");
        return 1;
    }

    if (deflate_file(source, dest, Z_DEFAULT_COMPRESSION) != Z_OK) {
        fprintf(stderr, "Compression failed.\n");
        return 1;
    }

    fclose(source);
    fclose(dest);

    return 0;
}