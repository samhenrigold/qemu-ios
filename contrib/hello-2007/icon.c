/* icon OUT.png Hello|Tilt -- the apps' 57x57 home-screen icons (SpringBoard adds the gloss and corners).
 * An 8-bit RGB PNG with stored (uncompressed) deflate blocks; libc only. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define S 57
static uint8_t img[S][1 + 3 * S];                       /* each row: filter byte 0, then RGB */

static void px(int x, int y, int r, int g, int b) { uint8_t *p = &img[y][1 + 3 * x]; p[0] = r, p[1] = g, p[2] = b; }

static void hello(void)
{
    static const char *glyphs[2][7] = {
        { "10001", "10001", "10001", "11111", "10001", "10001", "10001" },   /* H */
        { "00100", "00000", "01100", "00100", "00100", "00100", "01110" },   /* i */
    };
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++)
            px(x, y, 30 + 40 * y / S, 60 + 80 * y / S, 140 + 90 * y / S);
    for (int gi = 0; gi < 2; gi++)
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++)
                if (glyphs[gi][gy][gx] == '1')
                    for (int dy = 0; dy < 4; dy++)
                        for (int dx = 0; dx < 4; dx++)
                            px(6 + gi * 24 + gx * 4 + dx, 14 + gy * 4 + dy, 255, 255, 255);
}

/* Tilt's level: rings and a crosshair on slate, the ball off-center. */
static void tilt(void)
{
    int c = S / 2;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            double d2 = (x - c) * (x - c) + (y - c) * (y - c);
            int line = (x == c || y == c) && d2 < 26 * 26;
            for (int r = 8; r <= 24; r += 8)
                line |= (r - 0.9) * (r - 0.9) < d2 && d2 < (r + 0.9) * (r + 0.9);
            px(x, y, 18, 23, 31);
            if (line)
                px(x, y, 77, 97, 122);
            if ((x - c - 9) * (x - c - 9) + (y - c + 7) * (y - c + 7) < 64)
                px(x, y, 242, 140, 38);
        }
}

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ c >> 8;
    return c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    fwrite(d, 1, n, f);
    be32(h, ~crc(crc(~0u, (const uint8_t *)type, 4), d, n));
    fwrite(h, 1, 4, f);
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "Hello") && strcmp(argv[2], "Tilt"))) {
        fprintf(stderr, "usage: icon OUT.png Hello|Tilt\n");
        return 1;
    }
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320 ^ c >> 1 : c >> 1;
        crc_table[n] = c;
    }
    (strcmp(argv[2], "Hello") ? tilt : hello)();

    /* zlib stream: header, one stored block (the image is under 64 KiB), adler32 */
    static uint8_t z[2 + 5 + sizeof img + 4];
    uint32_t n = sizeof img, a = 1, b = 0;
    z[0] = 0x78, z[1] = 0x01;
    z[2] = 1, z[3] = n, z[4] = n >> 8, z[5] = ~n, z[6] = ~n >> 8;
    memcpy(z + 7, img, n);
    for (uint32_t k = 0; k < n; k++)
        a = (a + ((uint8_t *)img)[k]) % 65521, b = (b + a) % 65521;
    be32(z + 7 + n, b << 16 | a);

    uint8_t ihdr[13] = { 0 };
    be32(ihdr, S), be32(ihdr + 4, S), ihdr[8] = 8, ihdr[9] = 2;
    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return perror(argv[1]), 1;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", ihdr, sizeof ihdr);
    chunk(f, "IDAT", z, sizeof z);
    chunk(f, "IEND", NULL, 0);
    return fclose(f) != 0;
}
