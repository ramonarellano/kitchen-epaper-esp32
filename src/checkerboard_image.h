#pragma once
#include <stdint.h>

// This is a placeholder for the full 800x480 3bpp checkerboard image buffer.
// For demonstration, only a small portion is shown. Replace with the full array
// for production.

#define IMAGE_WIDTH 800
#define IMAGE_HEIGHT 480
#define IMAGE_BPP 3
#define IMAGE_SIZE ((IMAGE_WIDTH * IMAGE_HEIGHT * IMAGE_BPP) / 8)

// Example: 96 bytes (256 pixels) for test. Replace with full 144000 bytes for
// real use.
static const uint8_t checkerboard_image[IMAGE_SIZE] = {
    // ... fill with generated data ...
    0x00, 0x00, 0x00, 0x00,  // etc.
};
