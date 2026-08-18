#pragma once
#include <stdint.h>

/* Level-5 XORShift save cipher (symmetric) + zlib CRC-32. See NOTES.md. */
void ie_xor_body(uint8_t *buf, uint32_t len);
uint32_t ie_crc32(const uint8_t *buf, uint32_t len);
