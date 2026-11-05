/* pt_util.h - the small helpers common/ had copied file to file: clamps, a lerp, the colour mix, the
 * little-endian byte order of the saves and blobs, the cell hash. Header-only (static inline): every
 * build (the sim's Makefile, tankcore's CMakeLists) picks it up with no new source. Each one is the
 * exact expression its copies used, so nothing it draws or stores moves by a bit. */
#ifndef PT_UTIL_H
#define PT_UTIL_H

#include <stdint.h>

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float lerpf(float a, float b, float p) { return a + (b - a) * p; }

/* 0xRRGGBB, p of the way from a to b (truncated per channel) */
static inline uint32_t rgb_mix(uint32_t a, uint32_t b, float p) {
    int ar = a >> 16 & 255, ag = a >> 8 & 255, ab = a & 255;
    int br = b >> 16 & 255, bg = b >> 8 & 255, bb = b & 255;
    return ((uint32_t)(ar + (br - ar) * p) << 16) |
           ((uint32_t)(ag + (bg - ag) * p) << 8) |
            (uint32_t)(ab + (bb - ab) * p);
}
/* 0xRRGGBB dimmed by dim, to RGB565 in native order (render.h FB_SWAP16 for the frame's) */
static inline uint16_t rgb565(uint32_t rgb, float dim) {
    uint32_t r = (uint32_t)(((rgb >> 16) & 255) * dim);
    uint32_t g = (uint32_t)(((rgb >> 8) & 255) * dim);
    uint32_t b = (uint32_t)((rgb & 255) * dim);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* little-endian fields at byte offset o (the saves, the shelf, the passport, the gift) */
static inline void le_put16(uint8_t *b, int o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static inline void le_put32(uint8_t *b, int o, uint32_t v) { le_put16(b, o, v & 0xffff); le_put16(b, o + 2, v >> 16); }
static inline uint32_t le_get16(const uint8_t *b, int o) { return b[o] | (uint32_t)b[o + 1] << 8; }
static inline uint32_t le_get32(const uint8_t *b, int o) { return le_get16(b, o) | le_get16(b, o + 2) << 16; }

/* the cell hash: the renderer's moss and rock, and the crab's volcano that must match them */
static inline uint32_t chash(int a, int b) {
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

#endif
