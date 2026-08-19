/* host-side test driver: apply the save cipher + CRC to a file */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../3ds/source/codec.h"

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--magic")) {
        FILE *in = fopen(argv[2], "rb");
        if (!in) return 2;
        uint8_t head[6];
        uint32_t seed;
        fseek(in, 0, SEEK_END);
        long sz = ftell(in);
        fseek(in, 0, SEEK_SET);
        if (fread(head, 1, 6, in) != 6) return 2;
        fseek(in, sz - 4, SEEK_SET);
        if (fread(&seed, 1, 4, in) != 4) return 2;
        fclose(in);
        printf("%04X\n", ie_magic(head, seed));
        return 0;
    }
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    if (!in) return 2;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *buf = malloc(size);
    if (fread(buf, 1, size, in) != (size_t)size) return 2;
    fclose(in);
    ie_xor_body(buf, size);
    uint32_t crc = ie_crc32(buf, size - 8);
    memcpy(buf + size - 8, &crc, 4);
    FILE *out = fopen(argv[2], "wb");
    fwrite(buf, 1, size, out);
    fclose(out);
    return 0;
}
