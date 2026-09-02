#include "protocol/crc32c.h"

uint32_t crc32c(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0x82F63B78u : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
