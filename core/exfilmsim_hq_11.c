#include "platform.h"
#include "camera_info.h"
#include "exfilmsim.h"
#include "exfilmsim_hq.h"
#include "filmsim_raw.h"

#ifdef __arm__
// This translation unit only builds for the A620. Its ARM9 can multiply
// signed halfwords directly, including the rounding addition.
#pragma GCC target("arch=armv5te")
#endif

#define HQ11_INLINE static inline __attribute__((always_inline))
#define HQ11_FAST __attribute__((optimize("O3", "schedule-insns2", "no-peel-loops", "no-unswitch-loops")))

typedef struct {
    unsigned chroma;
    int green;
} chroma_site;

typedef struct {
    unsigned *chroma;
    short *green;
} chroma_row;

enum { HQ_RAW_LEVELS = 1024, CHROMA_CLIP = 1000, CHROMA_BIAS = 2048 };

HQ11_INLINE int chroma_valid(int r, int g1, int g2, int b)
{
    return (unsigned)(r - 1) < CHROMA_CLIP - 1 &&
           (unsigned)(g1 - 1) < CHROMA_CLIP - 1 &&
           (unsigned)(g2 - 1) < CHROMA_CLIP - 1 &&
           (unsigned)(b - 1) < CHROMA_CLIP - 1;
}

HQ11_INLINE chroma_site chroma_sample(int r, int g1, int g2, int b, int red_wb, int blue_wb)
{
    int g = ((g1 + g2) >> 1) - HP_BLK;
    unsigned cr = (((short)(r - HP_BLK) * (short)red_wb) >> 8) - g + CHROMA_BIAS;
    unsigned cb = (((short)(b - HP_BLK) * (short)blue_wb) >> 8) - g + CHROMA_BIAS;
    // Both biased lanes fit independently after adding all nine sites.
    return (chroma_site){ cr | (cb << 16), g };
}

HQ11_FAST
static void chroma_load(const unsigned char *a, int stride, int groups, chroma_row row,
                        int red_wb, int blue_wb)
{
    const unsigned char *b = a + stride;
    chroma_site last = { CHROMA_BIAS | (CHROMA_BIAS << 16), 0 };
    for (int i = 0; i < groups; ++i, a += 10, b += 10) {
        int p[8], q[8];
        dec8(a, p);
        dec8(b, q);
        for (int k = 0; k < 8; k += 2) {
            if (chroma_valid(p[k], p[k + 1], q[k], q[k + 1]))
                last = chroma_sample(p[k], p[k + 1], q[k], q[k + 1], red_wb, blue_wb);
            *row.chroma++ = last.chroma;
            *row.green++ = last.green;
        }
    }
}

typedef struct {
    int red_wb, blue_wb, strength;
    short recip[HQ_RAW_LEVELS];
    signed char inverse_r[193], inverse_b[193];
} chroma_filter;

HQ11_INLINE int abs_diff(int n)
{
    return n < 0 ? -n : n;
}

HQ11_INLINE void chroma_apply_cell(int *r, int g1, int g2, int *b, unsigned own,
                                  chroma_site left, chroma_site center, chroma_site right,
                                  const chroma_filter *f)
{
    unsigned sum = left.chroma + center.chroma + right.chroma;
    int cr = (int)(sum & 65535) - 9 * CHROMA_BIAS;
    int cb = (int)(sum >> 16) - 9 * CHROMA_BIAS;
    int dr = (((short)cr * 7281 + 32768) >> 16) - (int)(own & 65535) + CHROMA_BIAS;
    int db = (((short)cb * 7281 + 32768) >> 16) - (int)(own >> 16) + CHROMA_BIAS;
    int dc = abs_diff(dr) + abs_diff(db);
    if (!dc || dc >= 97)
        return; // Zero residual or a fully protected chroma edge.
    int g = ((g1 + g2) >> 1) - HP_BLK;
    int mean_g = ((short)(left.green + center.green + right.green) * 7281 + 32768) >> 16;
    int w = 256 - (((short)abs_diff(g - mean_g) * f->recip[mean_g + HP_BLK]) >> 8);
    int chroma_weight = 256 - (((short)dc * 682) >> 8);
    if (chroma_weight < w)
        w = chroma_weight;
    if (w <= 0)
        return;
    w = ((short)w * (short)f->strength + 128) >> 8;
    // Nonzero weight implies dc <= 96, so both rounded residuals fit the tables.
    int nr = clip10(*r + f->inverse_r[(((short)dr * (short)w + 128) >> 8) + 96]);
    int nb = clip10(*b + f->inverse_b[(((short)db * (short)w + 128) >> 8) + 96]);
    *r = nr ? nr : 1;
    *b = nb ? nb : 1;
}

typedef struct {
    int p[8], q[8];
    unsigned valid;
    unsigned own[4];
    chroma_site vertical[4];
} chroma_group;

HQ11_FAST
static void chroma_load_group(chroma_group *restrict group, const unsigned char *restrict a, int stride,
                              chroma_row above, chroma_row row, int x, chroma_site last[2],
                              const chroma_filter *restrict f)
{
    const unsigned char *b = a + stride;
    int np[8], nq[8];
    dec8(a, group->p);
    dec8(b, group->q);
    dec8(b + stride, np);
    dec8(b + 2 * stride, nq);
    chroma_site own = last[0], below = last[1];
    unsigned valid = 0;
    for (int k = 0; k < 4; ++k) {
        int i = 2 * k;
        int r = group->p[i], g1 = group->p[i + 1], g2 = group->q[i], blue = group->q[i + 1];
        int is_valid = chroma_valid(r, g1, g2, blue);
        if (is_valid)
            valid |= 1u << k;
        if (row.chroma) {
            own = (chroma_site){ row.chroma[x + k], row.green[x + k] };
        } else if (is_valid) {
            own = chroma_sample(r, g1, g2, blue, f->red_wb, f->blue_wb);
        }
        if (chroma_valid(np[i], np[i + 1], nq[i], nq[i + 1]))
            below = chroma_sample(np[i], np[i + 1], nq[i], nq[i + 1], f->red_wb, f->blue_wb);
        group->own[k] = own.chroma;
        group->vertical[k] = (chroma_site){
            above.chroma[x + k] + own.chroma + below.chroma,
            above.green[x + k] + own.green + below.green,
        };
        // The consumed upper guide becomes either the new lower or upper guide.
        chroma_site replacement = row.chroma ? below : own;
        above.chroma[x + k] = replacement.chroma;
        above.green[x + k] = replacement.green;
    }
    last[0] = own;
    last[1] = below;
    group->valid = valid;
}

HQ11_FAST
static void chroma_apply_row(unsigned char *restrict a, int stride, int cells, int x0, int x1,
                             chroma_row above, chroma_row row, const chroma_filter *restrict f)
{
    const chroma_site seed = { CHROMA_BIAS | (CHROMA_BIAS << 16), 0 };
    chroma_site last[2] = { seed, seed };
    chroma_group groups[2];
    chroma_group *current = groups, *next = groups + 1;
    chroma_load_group(current, a, stride, above, row, 0, last, f);
    chroma_site left = current->vertical[0];
    for (int i = 0; i < x1; i += 4, a += 10) {
        if (i + 4 < cells)
            chroma_load_group(next, a + 10, stride, above, row, i + 4, last, f);
        if (i + 4 > x0) {
            int min = i < x0 ? x0 - i : 0;
            int max = x1 - i < 4 ? x1 - i : 4;
            if (min)
                left = current->vertical[min - 1];
            for (int k = min; k < max; ++k) {
                int j = 2 * k;
                chroma_site right = k == 3 ? next->vertical[0] : current->vertical[k + 1];
                if (current->valid & (1u << k))
                    chroma_apply_cell(&current->p[j], current->p[j + 1], current->q[j],
                                      &current->q[j + 1], current->own[k], left,
                                      current->vertical[k], right, f);
                left = current->vertical[k];
            }
            enc8(a, current->p);
            enc8(a + stride, current->q);
        }
        left = current->vertical[3];
        chroma_group *swap = current;
        current = next;
        next = swap;
    }
}

int exfs_hq11_chroma(char *raw, int strength, int red_wb, int blue_wb)
{
    int groups = camera_sensor.raw_rowpix / 8;
    int cells = groups * 4;
    int stride = camera_sensor.raw_rowlen;
    int x0 = (camera_sensor.active_area.x1 + 1) / 2 + 1;
    int x1 = camera_sensor.active_area.x2 / 2 - 1;
    int y0 = (camera_sensor.active_area.y1 + 1) / 2 + 1;
    int y1 = camera_sensor.active_area.y2 / 2 - 1;
    if (x1 <= x0 || y1 <= y0)
        return 1;

    chroma_row above = { malloc(cells * sizeof(unsigned)), malloc(cells * sizeof(short)) };
    chroma_filter *f = malloc(sizeof(*f));
    int ok = above.chroma && above.green && f;
    if (ok) {
        f->red_wb = red_wb;
        f->blue_wb = blue_wb;
        f->strength = strength;
        // The guide cannot exceed a sensor code. Fold the per-cell division
        // and lower bound for its edge threshold into the reciprocal lookup.
        for (int i = 0; i < HQ_RAW_LEVELS; ++i) {
            int threshold = (i - HP_BLK) / 3;
            f->recip[i] = 65536 / (threshold < 40 ? 40 : threshold);
        }
        for (int i = -96; i <= 96; ++i) {
            f->inverse_r[i + 96] = i * 256 / red_wb;
            f->inverse_b[i + 96] = i * 256 / blue_wb;
        }
        unsigned char *a = (unsigned char *)raw + y0 * 2 * stride;
        chroma_load(a - 2 * stride, stride, groups, above, red_wb, blue_wb);
        chroma_row row = { malloc(cells * sizeof(unsigned)), malloc(cells * sizeof(short)) };
        if (row.chroma && row.green) {
            chroma_load(a, stride, groups, row, red_wb, blue_wb);
        } else {
            free(row.green);
            free(row.chroma);
            row = (chroma_row){ NULL, NULL };
        }
        for (int y = y0; y < y1; ++y, a += 2 * stride) {
            exfs_progress_row(y, camera_sensor.raw_rows / 2);
            chroma_apply_row(a, stride, cells, x0, x1, above, row, f);
            if (row.chroma) {
                chroma_row swap = above;
                above = row;
                row = swap;
            }
        }
        free(row.green);
        free(row.chroma);
    }
    free(f);
    free(above.green);
    free(above.chroma);
    return ok;
}

typedef struct {
    unsigned guide;
    unsigned dye;
} grain_site;

typedef struct {
    int red_dye, blue_dye, red_wb, blue_wb, inv_red, inv_blue;
    unsigned char spread[8];
    signed char fine[256];
    unsigned char amplitude[HQ_RAW_LEVELS];
} grain_filter;

enum { DENSITY_BIAS = 672, DYE_LANE = 11, DYE_BIAS = 6 * 128 };

HQ11_INLINE unsigned grain_random(unsigned x)
{
    x ^= x << 13;
    x ^= x >> 17;
    return x ^ (x << 5);
}

HQ11_INLINE grain_site grain_sample(int green, unsigned *state, const grain_filter *f)
{
    unsigned r = grain_random(*state);
    unsigned byte_sums = (r & 0x00ff00ffu) + ((r >> 8) & 0x00ff00ffu);
    int density = (int)(byte_sums >> 16) - (int)(byte_sums & 511);
    if (density > 192)
        density += (density - 192) / 2;
    if (density < -192)
        density += (density + 192) / 2;
    r = *state = grain_random(r);
    unsigned spread = f->spread[(r >> 4) & 7];
    unsigned fine = (unsigned char)f->fine[(r >> 8) & 255];
    // Leave headroom for adding the density and the two dye lanes independently.
    unsigned guide = (density + DENSITY_BIAS) | ((unsigned)green << 16) | (spread << 26);
    unsigned dye = (r >> 24) | (((r >> 16) & 255) << DYE_LANE) | (fine << 24);
    return (grain_site){ guide, dye };
}

HQ11_INLINE int grain_delta(int value, int delta)
{
    int limit = (1023 - HP_BLK) / 2 - abs_diff(value - (1023 + HP_BLK) / 2);
    if (limit < 0)
        limit = 0;
    if (delta > limit)
        delta = limit;
    if (delta < -limit)
        delta = -limit;
    return value + delta;
}

HQ11_INLINE int grain_green(grain_site site)
{
    return (site.guide >> 16) & 1023;
}

typedef struct {
    short red, green1, green2, blue;
    unsigned short edge;
} grain_pattern;

HQ11_INLINE grain_pattern grain_shape(grain_site s, grain_site l, grain_site h,
                                     grain_site u, grain_site d, const grain_filter *f)
{
    int spread = s.guide >> 26;
    int adjacent = (int)((l.guide + h.guide + u.guide + d.guide) & 65535) - 4 * DENSITY_BIAS;
    int luma = (int)(s.guide & 65535) - DENSITY_BIAS +
               (((short)adjacent * (short)spread + 16) >> 5) + (signed char)(s.dye >> 24);
    int dx = ((int)(h.guide & 65535) - (int)(l.guide & 65535)) >> 4;
    int dy = ((int)(d.guide & 65535) - (int)(u.guide & 65535)) >> 4;
    unsigned dyes = 2 * s.dye + l.dye + h.dye + u.dye + d.dye;
    int red = ((short)((int)(dyes & 2047) - DYE_BIAS) * (short)f->red_dye + 32) >> 6;
    int blue = ((short)((int)((dyes >> DYE_LANE) & 2047) - DYE_BIAS) * (short)f->blue_dye + 32) >> 6;
    int green = -(2 * red + blue) / 5;
    int edge = abs_diff(grain_green(h) - grain_green(l)) + abs_diff(grain_green(d) - grain_green(u));
    return (grain_pattern){
        luma - dx - dy + red, luma + dx - dy + green,
        luma - dx + dy + green, luma + dx + dy + blue, edge,
    };
}

HQ11_INLINE int grain_tone(int r, int g1, int g2, int b, const grain_filter *f)
{
    int wr = HP_BLK + (((short)(r - HP_BLK) * (short)f->red_wb) >> 8);
    int wb = HP_BLK + (((short)(b - HP_BLK) * (short)f->blue_wb) >> 8);
    return clip10((4 * wr + 5 * (g1 + g2) + 2 * wb + 8) >> 4);
}

HQ11_INLINE void grain_add_pattern(int *r, int *g1, int *g2, int *b, int tone, int amp,
                                  grain_pattern pattern, const grain_filter *f)
{
    int detail = pattern.edge + 2 * abs_diff(*g1 - *g2) - 12 - (tone >> 5);
    if (detail > 0) {
        int taper = detail >> 2;
        if (taper > 16)
            taper = 16;
        amp = (amp * (64 - taper) + 32) >> 6;
    }
    int red_amp = amp * f->inv_red;
    int blue_amp = amp * f->inv_blue;
    *r = grain_delta(*r, (pattern.red * red_amp + 32768) >> 16);
    *g1 = grain_delta(*g1, (pattern.green1 * (short)amp + 128) >> 8);
    *g2 = grain_delta(*g2, (pattern.green2 * (short)amp + 128) >> 8);
    *b = grain_delta(*b, (pattern.blue * blue_amp + 32768) >> 16);
}

HQ11_INLINE void grain_apply_cell(int *r, int *g1, int *g2, int *b,
                                 grain_site s, grain_site l, grain_site h,
                                 grain_site u, grain_site d, const grain_filter *f)
{
    if (!*r || !*g1 || !*g2 || !*b)
        return;
    int tone = grain_tone(*r, *g1, *g2, *b, f);
    int amp = f->amplitude[tone];
    if (amp)
        grain_add_pattern(r, g1, g2, b, tone, amp, grain_shape(s, l, h, u, d, f), f);
}

HQ11_INLINE int grain_group_first_green(const unsigned char *a, const unsigned char *b)
{
    const unsigned short *pa = (const unsigned short *)a;
    const unsigned short *pb = (const unsigned short *)b;
    return ((((pa[0] & 63) << 4) | (pa[1] >> 12)) + (pb[0] >> 6) + 1) >> 1;
}

// Each Canon packed group contains four 20-bit pairs of 10-bit codes.
HQ11_INLINE void unpack_pairs(const unsigned char *src, unsigned pairs[4])
{
    const unsigned short *p = (const unsigned short *)src;
    unsigned a = p[0], b = p[1], c = p[2], d = p[3], e = p[4];
    pairs[0] = (a << 4) | (b >> 12);
    pairs[1] = ((b & 4095) << 8) | (c >> 8);
    pairs[2] = ((c & 255) << 12) | (d >> 4);
    pairs[3] = ((d & 15) << 16) | e;
}

HQ11_INLINE void pack_pairs(unsigned char *dst, const unsigned pairs[4])
{
    unsigned short *p = (unsigned short *)dst;
    p[0] = pairs[0] >> 4;
    p[1] = (pairs[0] << 12) | (pairs[1] >> 8);
    p[2] = (pairs[1] << 8) | (pairs[2] >> 12);
    p[3] = (pairs[2] << 4) | (pairs[3] >> 16);
    p[4] = pairs[3];
}

HQ11_INLINE void grain_group_green(const unsigned char *a, const unsigned char *b, int green[4])
{
    const unsigned short *p = (const unsigned short *)a;
    const unsigned short *q = (const unsigned short *)b;
    green[0] = ((((p[0] & 63) << 4) | (p[1] >> 12)) + (q[0] >> 6) + 1) >> 1;
    green[1] = ((((p[1] & 3) << 8) | (p[2] >> 8)) + ((q[1] >> 2) & 1023) + 1) >> 1;
    green[2] = (((p[3] >> 4) & 1023) + (((q[2] & 255) << 2) | (q[3] >> 14)) + 1) >> 1;
    green[3] = ((p[4] & 1023) + (((q[3] & 15) << 6) | (q[4] >> 10)) + 1) >> 1;
}

HQ11_FAST
static unsigned grain_load_row(const unsigned char *a, int stride, int x0, int x1,
                               grain_site *row, const grain_filter *f, unsigned state)
{
    int i = 1;
    for (int x = x0 & ~7; x < x1; x += 8, a += 10) {
        int green[4];
        grain_group_green(a, a + stride, green);
        int first = x < x0 ? (x0 - x) / 2 : 0;
        int end = x1 - x < 8 ? (x1 - x) / 2 : 4;
        for (int k = first; k < end; ++k)
            row[i++] = grain_sample(green[k], &state, f);
    }
    row[0] = row[1];
    row[i] = row[i - 1];
    return state;
}

HQ11_FAST
static unsigned grain_filter_row(unsigned char *restrict a, int stride, int x0, int x1,
                                 grain_site *restrict above, const grain_site *restrict row,
                                 int last_row, const grain_filter *restrict f, unsigned state)
{
    unsigned char *b = a + stride;
    const unsigned char *lower = b + stride;
    int i = 1;
    grain_site left = row[0], site = row[1];
    for (int x = x0 & ~7; x < x1; x += 8, a += 10, b += 10, lower += 10) {
        int green[4] = {0};
        grain_pattern patterns[4];
        int first = x < x0 ? (x0 - x) / 2 : 0;
        int end = x1 - x < 8 ? (x1 - x) / 2 : 4;
        if (!last_row)
            grain_group_green(lower, lower + stride, green);
        for (int k = first; k < end; ++k, ++i) {
            grain_site right = row[i + 1];
            grain_site below = last_row ? site : grain_sample(green[k], &state, f);
            patterns[k] = grain_shape(site, left, right, above[i], below, f);
            above[i] = below;
            left = site;
            site = right;
        }
        unsigned p[4], q[4];
        unpack_pairs(a, p);
        unpack_pairs(b, q);
        for (int k = first; k < end; ++k) {
            int r = p[k] >> 10, g1 = p[k] & 1023;
            int g2 = q[k] >> 10, blue = q[k] & 1023;
            if (!r || !g1 || !g2 || !blue)
                continue;
            int tone = grain_tone(r, g1, g2, blue, f);
            int amp = f->amplitude[tone];
            if (!amp)
                continue;
            grain_add_pattern(&r, &g1, &g2, &blue, tone, amp, patterns[k], f);
            p[k] = (r << 10) | g1;
            q[k] = (g2 << 10) | blue;
        }
        pack_pairs(a, p);
        pack_pairs(b, q);
    }
    above[0] = above[1];
    above[i] = above[i - 1];
    return state;
}

HQ11_FAST
static unsigned grain_apply_row(unsigned char *a, int stride, int x0, int x1,
                                grain_site *above, int first_row, int last_row,
                                const grain_filter *f, unsigned state, unsigned next_state)
{
    unsigned char *b = a + stride;
    const unsigned char *next_a = b + stride, *next_b = next_a + stride;
    int i = 0;
    grain_site site = {0, 0}, left = {0, 0};
    for (int x = x0 & ~7; x < x1; x += 8, a += 10, b += 10, next_a += 10, next_b += 10) {
        int p[8], q[8], green[4];
        dec8(a, p);
        dec8(b, q);
        if (!last_row)
            grain_group_green(next_a, next_b, green);
        int first = x < x0 ? x0 - x : 0;
        int end = x1 - x < 8 ? x1 - x : 8;
        if (!i)
            left = site = grain_sample((p[first + 1] + q[first] + 1) >> 1, &state, f);
        for (int k = first; k < end; k += 2, ++i) {
            grain_site right = site;
            if (x + k + 2 < x1) {
                int g = k != 6 ? (p[k + 3] + q[k + 2] + 1) >> 1
                               : grain_group_first_green(a + 10, b + 10);
                right = grain_sample(g, &state, f);
            }
            grain_site below = last_row ? site : grain_sample(green[k / 2], &next_state, f);
            grain_site upper = first_row ? site : above[i];
            grain_apply_cell(&p[k], &p[k + 1], &q[k], &q[k + 1], site, left, right, upper, below, f);
            above[i] = site;
            left = site;
            site = right;
        }
        enc8(a, p);
        enc8(b, q);
    }
    return next_state;
}

int exfs_hq11_grain(char *raw, const exfs_hq_profile *profile, int red_wb, int blue_wb)
{
    static const unsigned char amount_scale[] = { 0, 35, 49, 64, 80, 100, 124 };
    static const unsigned char size_gain[] = { 71, 69, 67, 64, 61, 58, 54 };
    static const unsigned char color_scale[] = { 0, 6, 11, 16, 22, 29, 38 };
    int level = exfs_hq_clamp_level(*profile->levels[EXFS_HQ_AMOUNT]);
    int amount = amount_scale[level - EXFS_HQ_LEVEL_MIN];
    if (!amount)
        return 1;
    int x0 = (camera_sensor.active_area.x1 + 1) & ~1;
    int x1 = camera_sensor.active_area.x2 & ~1;
    int y0 = (camera_sensor.active_area.y1 + 1) & ~1;
    int y1 = camera_sensor.active_area.y2 & ~1;
    if (x1 <= x0 || y1 <= y0)
        return 1;

    int cells = (x1 - x0) / 2;
    grain_site *above = malloc((cells + 2) * sizeof(*above));
    grain_filter *f = malloc(sizeof(*f));
    int ok = above && f;
    if (ok) {
        int size = exfs_hq_clamp_level(*profile->levels[EXFS_HQ_SIZE]);
        int color = color_scale[exfs_hq_clamp_level(*profile->levels[EXFS_HQ_COLOR]) - EXFS_HQ_LEVEL_MIN];
        int coarse = profile->clump_weight + 2 * size;
        if (coarse < 0)
            coarse = 0;
        for (int i = 0; i < 8; ++i)
            f->spread[i] = coarse + i > 3 ? coarse + i - 3 : 0;
        int fine = profile->fine_weight - 4 * size;
        for (int i = 0; i < 256; ++i)
            f->fine[i] = (((i - 128) * fine + 64) >> 7);
        f->red_dye = (profile->red_dye * color + 8) >> 4;
        f->blue_dye = (profile->blue_dye * color + 8) >> 4;
        f->red_wb = red_wb;
        f->blue_wb = blue_wb;
        f->inv_red = 65536 / red_wb;
        f->inv_blue = 65536 / blue_wb;
        int gain = amount * size_gain[size - EXFS_HQ_LEVEL_MIN];
        for (int v = 0; v < HQ_RAW_LEVELS; ++v) {
            int i = v >> 6, frac = v & 63;
            int amp = profile->tone_amplitude[i] * (64 - frac) + profile->tone_amplitude[i + 1] * frac;
            f->amplitude[v] = (amp * gain + 131072) >> 18;
        }

        static unsigned shot_sequence;
        unsigned state = ((unsigned)get_tick_count() ^ (++shot_sequence * 0x9e3779b9u)) | 1u;
        int stride = camera_sensor.raw_rowlen;
        unsigned char *a = (unsigned char *)raw + y0 * stride + (x0 / 8) * 10;
        grain_site *row = malloc((cells + 2) * sizeof(*row));
        if (row) {
            state = grain_load_row(a, stride, x0, x1, row, f, state);
            memcpy(above, row, (cells + 2) * sizeof(*row));
            for (int y = y0; y < y1; y += 2, a += 2 * stride) {
                exfs_progress_row(y - y0, y1 - y0);
                state = grain_filter_row(a, stride, x0, x1, above, row, y + 2 == y1, f, state);
                grain_site *swap = above;
                above = row;
                row = swap;
            }
            free(row);
        } else {
            // One stream starts at the current row and the other starts one row ahead.
            unsigned next_state = state;
            for (int i = 0; i < cells; ++i)
                next_state = grain_random(grain_random(next_state));
            for (int y = y0; y < y1; y += 2, a += 2 * stride) {
                exfs_progress_row(y - y0, y1 - y0);
                unsigned after = grain_apply_row(a, stride, x0, x1, above, y == y0, y + 2 == y1, f,
                                                 state, next_state);
                state = next_state;
                next_state = after;
            }
        }
    }
    free(f);
    free(above);
    return ok;
}
