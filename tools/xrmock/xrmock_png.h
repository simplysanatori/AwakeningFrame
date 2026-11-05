// Minimal PNG writer for the mock (8-bit RGBA).  Uses zlib when it is there
// (compressed), else stored deflate blocks (big but valid).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__has_include)
#if __has_include(<zlib.h>)
#include <zlib.h>
#define XRMOCK_ZLIB 1
#endif
#endif

static uint32_t png_crc(uint32_t crc, const uint8_t* p, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}
static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
    uint8_t h[8] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3]};
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = png_crc(0, (const uint8_t*)type, 4);
    if (n) c = png_crc(c, data, n);
    uint8_t t[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(t, 1, 4, f);
}

// rgba: w*h*4 bytes, top row first.  Returns 0 on success.
static int png_write_rgba(const char* path, const uint8_t* rgba, int w, int h) {
    size_t rowb = (size_t)w * 4;
    size_t rawn = (rowb + 1) * (size_t)h;
    uint8_t* raw = (uint8_t*)malloc(rawn);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (rowb + 1)] = 0;
        memcpy(raw + (size_t)y * (rowb + 1) + 1, rgba + (size_t)y * rowb, rowb);
    }
    uint8_t* z;
    size_t zn;
#ifdef XRMOCK_ZLIB
    uLongf zl = compressBound((uLong)rawn);
    z = (uint8_t*)malloc(zl);
    if (!z || compress2(z, &zl, raw, (uLong)rawn, 1) != Z_OK) {
        free(raw);
        free(z);
        return -1;
    }
    zn = zl;
#else
    size_t blocks = (rawn + 65534) / 65535;
    zn = 2 + rawn + blocks * 5 + 4;
    z = (uint8_t*)malloc(zn);
    if (!z) { free(raw); return -1; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < rawn;) {
        size_t n = rawn - pos > 65535 ? 65535 : rawn - pos;
        z[o++] = pos + n >= rawn;
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + pos, n); o += n; pos += n;
    }
    uint32_t ad = (b << 16) | a;
    z[o++] = (uint8_t)(ad >> 24); z[o++] = (uint8_t)(ad >> 16); z[o++] = (uint8_t)(ad >> 8); z[o++] = (uint8_t)ad;
    zn = o;
#endif
    free(raw);
    FILE* f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    return 0;
}
