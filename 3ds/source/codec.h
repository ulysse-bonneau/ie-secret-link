#pragma once
#include <stdint.h>

/* Level-5 XORShift save cipher (symmetric) + zlib CRC-32. See NOTES.md. */
void ie_xor_body(uint8_t *buf, uint32_t len);
uint32_t ie_crc32(const uint8_t *buf, uint32_t len);

/* decrypt just the u16 magic at offset 4, given the file's first 6 bytes
 * and the seed from its trailer — cheap save-type probe */
uint16_t ie_magic(const uint8_t *head6, uint32_t seed);
