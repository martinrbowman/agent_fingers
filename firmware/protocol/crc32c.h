#pragma once
#include <stddef.h>
#include <stdint.h>

// Standard CRC32C (Castagnoli): poly 0x1EDC6F41 reflected 0x82F63B78,
// init/final XOR 0xFFFFFFFF — matches the SSE4.2 CRC32C / iSCSI convention,
// so the host-side Python implementation can use the same well-known
// parameters without a written-down bit convention.
uint32_t crc32c(const uint8_t *data, size_t len);
