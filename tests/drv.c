/* host-side test driver: apply the save cipher + CRC to a file */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../3ds/source/codec.h"

int main(int argc, char **argv)
{
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
