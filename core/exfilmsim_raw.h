#ifndef EXFILMSIM_RAW_H
#define EXFILMSIM_RAW_H

// These kernels operate on the A620's packed 10-bit RGGB data.
#include "filmsim_raw.h"

#define EXFS_PIXELS 1024
#define EXFS_PACKED_BYTES 10

// Evaluate both Bayer pairs in parallel using one 16-bit lane per pixel.
// For hi > 47, lo < HP_LIM(hi) is equivalent to hi + 13 >= 2 * lo.
static inline __attribute__((always_inline)) unsigned exfs_hot_lanes(unsigned hi, unsigned lo)
{
    const unsigned guard = 0x80008000u;
    const unsigned lanes = 0x00010001u;
    unsigned brighter_than_black = (hi | guard) - (HP_BLK + HP_MIN + 1) * lanes;
    unsigned brighter_than_neighbor = (hi | guard) + (HP_BLK - HP_MIN - 2) * lanes - (lo << 1);
    return brighter_than_black & brighter_than_neighbor & guard;
}

static inline __attribute__((always_inline)) void exfs_queue_hot_pixels(
    const unsigned char *raw, int rowlen, int rowpix, int x, int y, const int *pixels,
    short *pending, int *count)
{
    unsigned left = pixels[0] | ((unsigned)pixels[1] << 16);
    unsigned right = pixels[2] | ((unsigned)pixels[3] << 16);
    unsigned left_hot = exfs_hot_lanes(left, right);
    unsigned right_hot = exfs_hot_lanes(right, left);
    if (!(left_hot | right_hot))
        return;

    for (int i = 0; i < 2; i++) {
        unsigned lane = 0x8000u << (16 * i);
        if (!((left_hot | right_hot) & lane))
            continue;
        int offset = (right_hot & lane) ? i + 2 : i;
        int target = x + offset;
        if (target >= 2 && target < rowpix - 2) {
            int value = hp_check(raw, rowlen, target, y, pixels[offset]);
            if (value >= 0) {
                pending[2 * *count] = target;
                pending[2 * *count + 1] = value;
                ++*count;
            }
        }
    }
}

__attribute__((optimize("O3")))
static void exfs_hot_pixels(unsigned char *raw, int rows, int rowpix, int rowlen, short *scratch)
{
    int cap = rowpix + 4;
    int counts[3] = {0, 0, 0};
    int slot = 2, y;
    for (y = 2; y < rows - 2; y++) {
        FS_PROG_ROW(y, rows);
        short *pending = scratch + slot * cap;
        if (counts[slot])
            hp_flush(raw, rowlen, y - 3, pending, counts[slot]);
        int count = 0;
        const unsigned char *src = raw + y * rowlen;
        for (int x = 0; x < rowpix; x += 8, src += EXFS_PACKED_BYTES) {
            int pixels[8];
            dec8(src, pixels);
            exfs_queue_hot_pixels(raw, rowlen, rowpix, x, y, pixels, pending, &count);
            exfs_queue_hot_pixels(raw, rowlen, rowpix, x + 4, y, pixels + 4, pending, &count);
        }
        counts[slot] = count;
        if (++slot == 3) slot = 0;
    }
    for (; y < rows + 1; y++) {
        if (counts[slot])
            hp_flush(raw, rowlen, y - 3, scratch + slot * cap, counts[slot]);
        if (++slot == 3) slot = 0;
    }
}

static inline __attribute__((always_inline)) int exfs_gain_value(int value, int factor)
{
    return clip10(31 + (((value - 31) * factor + 2048) >> 12));
}

static inline __attribute__((always_inline)) int exfs_nonzero(int value)
{
    return value ? value : 1;
}

__attribute__((optimize("O3")))
static void exfs_raw_gain(unsigned char *raw, int rows, int rowpix, int rowlen,
                          const exfs_gain_params *params, unsigned short (*lut)[EXFS_PIXELS])
{
    for (int v = 1; v < EXFS_PIXELS; v++) {
        lut[0][v] = exfs_nonzero(exfs_gain_value(v, params->red));
        lut[1][v] = exfs_nonzero(exfs_gain_value(v, params->green));
        lut[2][v] = exfs_nonzero(exfs_gain_value(v, params->blue));
    }
    lut[0][0] = lut[1][0] = lut[2][0] = 0;
    int white = exfs_gain_value(1023, params->green);
    int white_red = exfs_nonzero(exfs_gain_value(white, params->neutral_red));
    int white_green = exfs_nonzero(white);
    int white_blue = exfs_nonzero(exfs_gain_value(white, params->neutral_blue));

    for (int y = 0; y + 1 < rows; y += 2) {
        FS_PROG_ROW(y, rows);
        unsigned char *a = raw + y * rowlen, *b = a + rowlen;
        for (int x = 0; x < rowpix; x += 8, a += EXFS_PACKED_BYTES, b += EXFS_PACKED_BYTES) {
            int p[8], q[8];
            dec8(a, p);
            dec8(b, q);
            for (int k = 0; k < 8; k += 2) {
                if (p[k + 1] >= 1000 || q[k] >= 1000 || (p[k] >= 1000 && q[k + 1] >= 1000)) {
                    p[k] = p[k] ? white_red : 0;
                    p[k + 1] = p[k + 1] ? white_green : 0;
                    q[k] = q[k] ? white_green : 0;
                    q[k + 1] = q[k + 1] ? white_blue : 0;
                } else {
                    p[k] = lut[0][p[k]];
                    p[k + 1] = lut[1][p[k + 1]];
                    q[k] = lut[1][q[k]];
                    q[k + 1] = lut[2][q[k + 1]];
                }
            }
            enc8(a, p);
            enc8(b, q);
        }
    }
}

__attribute__((optimize("O3")))
static void exfs_rgb_curve(unsigned char *raw, int rows, int rowpix, int rowlen,
                           const unsigned short *curve)
{
    for (int y = 0; y < rows; y++) {
        const unsigned short *even = curve + ((y & 1) ? 2 * EXFS_PIXELS : 0);
        const unsigned short *odd = even + EXFS_PIXELS;
        unsigned char *row = raw + y * rowlen;
        for (int x = 0; x < rowpix; x += 8, row += EXFS_PACKED_BYTES) {
            int v[8];
            dec8(row, v);
            for (int i = 0; i < 8; i += 2) {
                v[i] = even[v[i]];
                v[i + 1] = odd[v[i + 1]];
            }
            enc8(row, v);
        }
    }
}

#endif
