#include <string.h>
#include "codec.h"

static const uint16_t odd_primes[256] = {
       3,    5,    7,   11,   13,   17,   19,   23,   29,   31,   37,   41,   43,   47,   53,   59,
      61,   67,   71,   73,   79,   83,   89,   97,  101,  103,  107,  109,  113,  127,  131,  137,
     139,  149,  151,  157,  163,  167,  173,  179,  181,  191,  193,  197,  199,  211,  223,  227,
     229,  233,  239,  241,  251,  257,  263,  269,  271,  277,  281,  283,  293,  307,  311,  313,
     317,  331,  337,  347,  349,  353,  359,  367,  373,  379,  383,  389,  397,  401,  409,  419,
     421,  431,  433,  439,  443,  449,  457,  461,  463,  467,  479,  487,  491,  499,  503,  509,
     521,  523,  541,  547,  557,  563,  569,  571,  577,  587,  593,  599,  601,  607,  613,  617,
     619,  631,  641,  643,  647,  653,  659,  661,  673,  677,  683,  691,  701,  709,  719,  727,
     733,  739,  743,  751,  757,  761,  769,  773,  787,  797,  809,  811,  821,  823,  827,  829,
     839,  853,  857,  859,  863,  877,  881,  883,  887,  907,  911,  919,  929,  937,  941,  947,
     953,  967,  971,  977,  983,  991,  997, 1009, 1013, 1019, 1021, 1031, 1033, 1039, 1049, 1051,
    1061, 1063, 1069, 1087, 1091, 1093, 1097, 1103, 1109, 1117, 1123, 1129, 1151, 1153, 1163, 1171,
    1181, 1187, 1193, 1201, 1213, 1217, 1223, 1229, 1231, 1237, 1249, 1259, 1277, 1279, 1283, 1289,
    1291, 1297, 1301, 1303, 1307, 1319, 1321, 1327, 1361, 1367, 1373, 1381, 1399, 1409, 1423, 1427,
    1429, 1433, 1439, 1447, 1451, 1453, 1459, 1471, 1481, 1483, 1487, 1489, 1493, 1499, 1511, 1523,
    1531, 1543, 1549, 1553, 1559, 1567, 1571, 1579, 1583, 1597, 1601, 1607, 1609, 1613, 1619, 1621
};

static void build_table(uint32_t seed, uint8_t table[256])
{
    uint64_t s = seed;
    uint32_t st[4];
    for (int i = 0; i < 3; i++) {
        s ^= s >> 30;
        s = (uint32_t)((uint64_t)(i + 1) + s * 0x6C078965ULL);
        st[i] = (uint32_t)s;
    }
    st[3] = 0x03DF95B3;
    for (int i = 0; i < 256; i++) table[i] = i;
    for (int i = 0; i < 4096; i++) {
        uint32_t x = st[0], y = st[3];
        st[0] = st[1]; st[1] = st[2]; st[2] = st[3];
        x ^= x << 11;
        x ^= x >> 8;
        y ^= y >> 19;
        st[3] = x ^ y;
        uint32_t r = st[3] % 0x10000;
        uint8_t r1 = r & 0xFF, r2 = (r >> 8) & 0xFF;
        if (r1 != r2) {
            uint8_t a = table[r1], b = table[r2];
            uint8_t t = table[a]; table[a] = table[b]; table[b] = t;
        }
    }
}

/* symmetric: encrypts and decrypts everything but the 8-byte trailer;
 * seed is read from the trailer's last 4 bytes (LE) */
void ie_xor_body(uint8_t *buf, uint32_t len)
{
    uint8_t table[256];
    uint32_t seed;
    memcpy(&seed, buf + len - 4, 4);
    build_table(seed, table);
    uint32_t ka = 0;
    for (uint32_t i = 0; i < len - 8; i++) {
        if ((i & 0xFF) == 0) ka = odd_primes[table[(i & 0xFF00) >> 8]];
        buf[i] ^= table[(ka * (i + 1)) & 0xFF];
    }
}

uint16_t ie_magic(const uint8_t *head6, uint32_t seed)
{
    uint8_t table[256];
    build_table(seed, table);
    uint32_t ka = odd_primes[table[0]];
    uint8_t b4 = head6[4] ^ table[(ka * 5) & 0xFF];
    uint8_t b5 = head6[5] ^ table[(ka * 6) & 0xFF];
    return (uint16_t)(b4 | (b5 << 8));
}

uint32_t ie_crc32(const uint8_t *buf, uint32_t len)
{
    uint32_t c = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        c ^= buf[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
    }
    return ~c;
}
