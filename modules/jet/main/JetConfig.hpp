#pragma once

// SolarOS uses complete frames in LCD wire byte order. Keep every translation
// unit on the same configuration. Overrides support paired host validation.
#ifndef JET_FRAMEBUFFER_SWAP_BYTES
#define JET_FRAMEBUFFER_SWAP_BYTES 1
#endif
#define JET32_WORLD_SCALE 4
#ifndef JET_MESH_INSTANCING
#define JET_MESH_INSTANCING 1
#endif
#define RENDER_TILE_BUFFER 0
#define TILE_WIDTH 32
#define TILE_HEIGHT 32
#define FAST_Z 0
#define JET_PERSPECTIVE_DEPTH 0
#define LAZY_Z 0
#define SCREEN_DOOR_ALPHA 0
#define SKIP_ZERO_AREA_TRIANGLES 1
#define NOISE_ALPHA 0
#define Z_BUFFERING 1
#define SORT_TRIANGLES 0
#define JET_SORT_DEPTH_BUCKETS 64
#define SORT_SCENE_OBJECTS 1
#define SORT_SCENE_REVERSE 0
#define DEPTH_ALPHA_BLEND 0
#define TEXTURE_MAPPING 1
#define PERSPECTIVE_CORRECT_TEXTURES 1
#define JET_HIGH_PRECISION_UVS 0
#define BILINEAR_FILTER 1
#define LIGHTING 1
#define Z_BRIGHTNESS 0
#define FLOAT_CAMERA_ANGLES 0
#define FLOAT_SIN_CACHE_SCALE 1
#define FLOAT_TAN_CACHE_SCALE 1
#define HALF_WIDTH_BUFFERS 0
#define FIELD_BUFFERS 0
#define SSR_FIELD_REFLECT 0
#define POSTFX_CRT 0
#define POSTFX_CELLSHADING 0
#define POSTFX_ANTIALIASING 0
#define POSTFX_BLOOM 0
#define POSTFX_MOTION_BLUR 0
#define POSTFX_CHROMATIC 0
#define POSTFX_PIXELATE 0
#ifndef JET_RUNTIME_POSTFX
#define JET_RUNTIME_POSTFX 1
#endif
#define CRT_SCANLINE_INTENSITY 48
#define MOTION_BLUR_STRENGTH 50
#define CHROMATIC_OFFSET 2
#define PIXELATE_SIZE 4
#define CELLSHADING_CELL_BITS 4
#define DEBUG_OVERDRAW 0
#define zBrightFar (1600 * JET32_WORLD_SCALE)
#define zBrightNear (200 * JET32_WORLD_SCALE)
#define zBrightScale 48
#define depthFogFar (8192 * JET32_WORLD_SCALE)
#define depthFogNear (6144 * JET32_WORLD_SCALE)
#define CHECKERBOARD_MODE 0
#define CHECKERBOARD_RECONSTRUCTION 0
#define MAX_PICK_QUERIES 36
// The native-order optimized texture/constant-blend spans are bypassed until
// they support swapped storage. General rasterization retains all shading.
#if JET_FRAMEBUFFER_SWAP_BYTES
#define JET_FAST_SIMPLE_SPANS 0
#endif
