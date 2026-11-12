// The package icon (sce_sys/icon0.png: 512x512, RGB) from docs/icon/icon_1024.png, without image tools: decoded with
// stb_image, halved by a 2x2 box filter over black, written as a PNG of stored (uncompressed) deflate blocks.
//   cc -O2 -Ithird_party/stb ps4/make_icon.c -o make_icon && ./make_icon docs/icon/icon_1024.png icon0.png
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    fwrite(data, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), data, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: make_icon in.png out.png\n"); return 1; }
    int w, h, n;
    uint8_t *src = stbi_load(argv[1], &w, &h, &n, 4);
    if (!src || w != 1024 || h != 1024) { fprintf(stderr, "make_icon: need a 1024x1024 image\n"); return 1; }
    enum { S = 512 };
    size_t row = 1 + S * 3, raw_n = (size_t)S * row;
    uint8_t *raw = calloc(raw_n, 1);
    for (int y = 0; y < S; y++) {
        uint8_t *o = raw + (size_t)y * row;
        o[0] = 0;  // (filter: none)
        for (int x = 0; x < S; x++)
            for (int c = 0; c < 3; c++) {
                unsigned sum = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        const uint8_t *p = src + ((size_t)(y * 2 + dy) * 1024 + (x * 2 + dx)) * 4;
                        sum += p[c] * p[3] / 255;  // (over black)
                    }
                o[1 + x * 3 + c] = (uint8_t)((sum + 2) / 4);
            }
    }
    // zlib stream of stored blocks (at most 65535 bytes each) + adler32
    size_t nblocks = (raw_n + 65534) / 65535, z_n = 2 + raw_n + nblocks * 5 + 4;
    uint8_t *z = malloc(z_n), *q = z;
    *q++ = 0x78, *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_n;) {
        size_t len = raw_n - off > 65535 ? 65535 : raw_n - off;
        *q++ = off + len == raw_n;
        *q++ = len & 0xFF, *q++ = len >> 8, *q++ = ~len & 0xFF, *q++ = (~len >> 8) & 0xFF;
        memcpy(q, raw + off, len);
        for (size_t i = 0; i < len; i++) a = (a + raw[off + i]) % 65521, b = (b + a) % 65521;
        q += len, off += len;
    }
    be32(q, (b << 16) | a);
    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, S), be32(ihdr + 4, S);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;  // 8-bit RGB
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)z_n);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    return 0;
}
