#pragma once
#include <stdint.h>

// Build a 4-byte big-endian header for an image size
inline void buildImageHeader(uint32_t img_size, uint8_t out[4]) {
  out[0] = (img_size >> 24) & 0xFF;
  out[1] = (img_size >> 16) & 0xFF;
  out[2] = (img_size >> 8) & 0xFF;
  out[3] = img_size & 0xFF;
}
