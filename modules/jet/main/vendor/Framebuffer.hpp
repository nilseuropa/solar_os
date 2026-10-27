#pragma once

#include <cstdint>
#include "JetConfig.hpp"

// Geometry, materials and textures always use native RGB565. Only framebuffer
// storage changes byte order. This is compile-time policy, including on reads
// for blending/reflections; there is no conversion pass after rendering.
#ifndef JET_FRAMEBUFFER_SWAP_BYTES
#define JET_FRAMEBUFFER_SWAP_BYTES 0
#endif

#if JET_FRAMEBUFFER_SWAP_BYTES && (HALF_WIDTH_BUFFERS || FIELD_BUFFERS)
#error "Byte-swapped framebuffer output currently requires complete full-width frames"
#endif

#if JET_FRAMEBUFFER_SWAP_BYTES && CHECKERBOARD_RECONSTRUCTION
#error "Byte-swapped framebuffers do not yet support checkerboard reconstruction"
#endif

namespace Renderer {
inline uint16_t framebufferEncode(uint16_t color) {
#if JET_FRAMEBUFFER_SWAP_BYTES
    return uint16_t((color << 8) | (color >> 8));
#else
    return color;
#endif
}
inline uint16_t framebufferDecode(uint16_t pixel) {
    return framebufferEncode(pixel);
}
}
