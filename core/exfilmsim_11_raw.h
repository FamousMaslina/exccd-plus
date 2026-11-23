#ifndef EXFILMSIM_11_RAW_H
#define EXFILMSIM_11_RAW_H

#include "exfilmsim_raw.h"

static void exfs11_build_rgb_table(const unsigned short *curve, unsigned *packed)
{
    for (int channel = 0; channel < 4; ++channel) {
        int shift = (channel & 1) ? 0 : 10;
        for (int i = 0; i < EXFS_PIXELS; ++i)
            *packed++ = (unsigned)*curve++ << shift;
    }
}

// Word entries let ARM9 scale the table index in the load. The even
// channel is already shifted into the upper half of each 20-bit pair.
__attribute__((optimize("O3")))
static void exfs11_rgb_curve(unsigned char *raw, int rows, int rowpix, int rowlen,
                            const unsigned *curve)
{
    for (int y = 0; y < rows; ++y) {
        const unsigned *even = curve + ((y & 1) ? 2 * EXFS_PIXELS : 0);
        const unsigned *odd = even + EXFS_PIXELS;
        unsigned short *h = (unsigned short *)(raw + y * rowlen);
        for (int x = 0; x < rowpix; x += 8, h += 5) {
            unsigned a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            unsigned p0 = even[a >> 6] | odd[((a & 63) << 4) | (b >> 12)];
            unsigned p1 = even[(b >> 2) & 1023] | odd[((b & 3) << 8) | (c >> 8)];
            unsigned p2 = even[((c & 255) << 2) | (d >> 14)] | odd[(d >> 4) & 1023];
            unsigned p3 = even[((d & 15) << 6) | (e >> 10)] | odd[e & 1023];
            h[0] = p0 >> 4;
            h[1] = (p0 << 12) | (p1 >> 8);
            h[2] = (p1 << 8) | (p2 >> 12);
            h[3] = (p2 << 4) | (p3 >> 16);
            h[4] = p3;
        }
    }
}

// For h >= 48 and h + 13 >= 2*l, the upper eight bits satisfy H >= 12
// and H + 4 >= 2*L. Screen two same-color pairs in their packed lanes.
static inline __attribute__((always_inline)) unsigned exfs11_pair_maybe_hot(unsigned left, unsigned right)
{
    const unsigned lanes = (1u << 10) | 1u;
    const unsigned guard = 512 * lanes;
    left = (left >> 2) & (255 * lanes);
    right = (right >> 2) & (255 * lanes);
    unsigned l = left | guard;
    unsigned r = right | guard;
    unsigned left_hot = (l - 12 * lanes) & (l + 4 * lanes - (right << 1));
    unsigned right_hot = (r - 12 * lanes) & (r + 4 * lanes - (left << 1));
    return (left_hot | right_hot) & guard;
}

static __attribute__((noinline)) void exfs11_check_hot_group(
    const unsigned char *raw, int rowlen, int rowpix, int x, int y,
    const unsigned short *src, unsigned *pending, int *count)
{
    int pixels[8];
    short fixes[8];
    int found = 0;
    dec8((const unsigned char *)src, pixels);
    exfs_queue_hot_pixels(raw, rowlen, rowpix, x, y, pixels, fixes, &found);
    exfs_queue_hot_pixels(raw, rowlen, rowpix, x + 4, y, pixels + 4, fixes, &found);
    for (int i = 0; i < found; ++i) {
        unsigned target = fixes[2 * i];
        pending[target / 32] |= 1u << (target % 32);
    }
    *count += found;
}

#define EXFS11_HOT_WORDS(rowpix) (((rowpix) + 31) / 32)

// A marked pixel's four same-color neighbors cannot also be hot. Each
// would have to pass the brightness ratio test against the other. The
// replacement average can therefore be calculated when flushing the row.
static void exfs11_flush_hot_pixels(unsigned char *raw, int rowlen, int y,
                                   unsigned *pending, int words)
{
    static const short recip[5] = { 0, 256, 128, 85, 64 };
    for (int i = 0; i < words; ++i) {
        unsigned mask = pending[i];
        pending[i] = 0;
        while (mask) {
            int x = 32 * i + __builtin_ctz(mask);
            int l = hp_get(raw, rowlen, x - 2, y), r = hp_get(raw, rowlen, x + 2, y);
            int u = hp_get(raw, rowlen, x, y - 2), d = hp_get(raw, rowlen, x, y + 2);
            int count = (l != 0) + (r != 0) + (u != 0) + (d != 0);
            int value = ((l + r + u + d) * recip[count]) >> 8;
            short fix[2] = { x, value ? value : 1 };
            hp_flush(raw, rowlen, y, fix, 1);
            mask &= mask - 1;
        }
    }
}

__attribute__((optimize("O3")))
static void exfs11_hot_pixels(unsigned char *raw, int rows, int rowpix, int rowlen, unsigned *scratch)
{
    int words = EXFS11_HOT_WORDS(rowpix);
    int counts[3] = {0, 0, 0};
    int slot = 2, y;
    for (y = 2; y < rows - 2; y++) {
        FS_PROG_ROW(y, rows);
        unsigned *pending = scratch + slot * words;
        if (counts[slot])
            exfs11_flush_hot_pixels(raw, rowlen, y - 3, pending, words);
        int count = 0;
        const unsigned short *src = (const unsigned short *)(raw + y * rowlen);
        for (int x = 0; x < rowpix; x += 8, src += 5) {
            unsigned h0 = src[0], h1 = src[1], h2 = src[2], h3 = src[3], h4 = src[4];
            unsigned left0 = (h0 << 4) | (h1 >> 12);
            unsigned right0 = (h1 << 8) | (h2 >> 8);
            unsigned left1 = (h2 << 12) | (h3 >> 4);
            unsigned right1 = (h3 << 16) | h4;
            // Two pixels in the same 16-value bucket are either both below
            // 48 or too close together to satisfy the hot-pixel ratio.
            const unsigned buckets = (63u << 14) | (63u << 4);
            if ((((left0 ^ right0) | (left1 ^ right1)) & buckets) == 0)
                continue;
            if (exfs11_pair_maybe_hot(left0, right0) | exfs11_pair_maybe_hot(left1, right1))
                exfs11_check_hot_group(raw, rowlen, rowpix, x, y, src, pending, &count);
        }
        counts[slot] = count;
        if (++slot == 3) slot = 0;
    }
    for (; y < rows + 1; y++) {
        if (counts[slot])
            exfs11_flush_hot_pixels(raw, rowlen, y - 3, scratch + slot * words, words);
        if (++slot == 3) slot = 0;
    }
}

static inline __attribute__((always_inline)) unsigned exfs11_grain_clip(int value)
{
    return value < 1 ? 1 : (value > 1023 ? 1023 : value);
}

static inline __attribute__((always_inline)) int exfs11_add_grain(
    unsigned *rg, unsigned *gb, int delta, int ir, int ib)
{
    // The amplitude of a defect marker (green == 0) is zero.
    if (!delta || !(*rg >> 10) || !(*gb >> 10) || !(*gb & 1023))
        return 0;

    int red = (int)(*rg >> 10) + ((delta * ir + 128) >> 8);
    int g1 = (int)(*rg & 1023) + delta;
    int g2 = (int)(*gb >> 10) + delta;
    int blue = (int)(*gb & 1023) + ((delta * ib + 128) >> 8);
    unsigned range = (unsigned)(red - 1) | (unsigned)(g1 - 1) |
                     (unsigned)(g2 - 1) | (unsigned)(blue - 1);
    if (range > 1022u) {
        red = exfs11_grain_clip(red);
        g1 = exfs11_grain_clip(g1);
        g2 = exfs11_grain_clip(g2);
        blue = exfs11_grain_clip(blue);
    }
    *rg = (red << 10) | g1;
    *gb = (g2 << 10) | blue;
    return 1;
}

// Keep the original noise recursion and seed order, but generate the row
// before touching the packed RAW data to reduce ARM register pressure.
static inline __attribute__((always_inline)) unsigned exfs11_grain_noise_row(
    short *up, int count, unsigned random)
{
    int left = 0;
    for (int i = 0; i < count; ++i) {
        random = random * 1664525u + 1013904223u;
        int e = (int)((random >> 24) + ((random >> 16) & 255) + ((random >> 8) & 255)) - 382;
        left = (((left + up[i]) * 3 + 4) >> 3) + e;
        up[i] = left;
    }
    return random;
}

__attribute__((optimize("O3")))
static void exfs11_grain_pixels(unsigned char *raw, int rows, int rowpix, int rowlen,
                               int ir, int ib, const short *amp, short *up, unsigned seed)
{
    unsigned random = seed | 1;
    for (int i = 0; i < rowpix / 2; i++)
        up[i] = 0;

    for (int y = 0; y + 1 < rows; y += 2) {
        FS_PROG_ROW(y, rows);
        unsigned short *top = (unsigned short *)(raw + y * rowlen);
        unsigned short *bottom = (unsigned short *)(raw + (y + 1) * rowlen);
        random = exfs11_grain_noise_row(up, rowpix / 2, random);
        const short *noise = up;
        for (int x = 0; x < rowpix; x += 8, top += 5, bottom += 5) {
            unsigned a = top[0], b = bottom[0];
            // Each 20-bit pixel pair spans two halfwords. Carry the lower
            // halfword forward so each packed sample is loaded and stored once.
            for (int i = 0, shift = 12; i < 4; i++, shift -= 4) {
                a = (a << 16) | top[i + 1];
                b = (b << 16) | bottom[i + 1];
                unsigned original_rg = (a >> shift) & 0xfffff;
                unsigned original_gb = (b >> shift) & 0xfffff;
                unsigned rg = original_rg, gb = original_gb;
                int delta = (*noise++ * amp[rg & 1023] + 2048) >> 12;
                if (exfs11_add_grain(&rg, &gb, delta, ir, ib)) {
                    a ^= (original_rg ^ rg) << shift;
                    b ^= (original_gb ^ gb) << shift;
                }
                top[i] = a >> 16;
                bottom[i] = b >> 16;
            }
            top[4] = a;
            bottom[4] = b;
        }
    }
}

#endif
