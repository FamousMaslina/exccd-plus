#include "platform.h"
#include "camera_info.h"
#include "conf.h"
#include "exfilmsim.h"
#include "exfilmsim_hq.h"
#include "filmsim_raw.h"

static const exfs_hq_profile profiles[] = {
    {
        .curve = "GOLD200.CV",
        .levels = { &conf.gold200_grain_amount, &conf.gold200_grain_size, &conf.gold200_grain_color },
        // Post-curve RAW knots, with a midtone gain about 65% of the first
        // Gold 200 profile's default and a gentler rolloff into highlights.
        .tone_amplitude = { 0, 7, 16, 23, 28, 32, 34, 34, 32, 28, 23, 17, 12, 8, 4, 1, 0 },
        .clump_weight = 6, .fine_weight = 32, .red_dye = 2, .blue_dye = 4,
        .sensor_chroma = 144,
    },
};

const exfs_hq_profile *exfs_hq_profile_for_curve(const char *curve)
{
    if (curve)
        for (unsigned i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
            if (strcmp(curve, profiles[i].curve) == 0)
                return &profiles[i];
    return NULL;
}

int exfs_hq_clamp_level(int level)
{
    if (level < EXFS_HQ_LEVEL_MIN) return EXFS_HQ_LEVEL_MIN;
    if (level > EXFS_HQ_LEVEL_MAX) return EXFS_HQ_LEVEL_MAX;
    return level;
}

typedef struct {
    short density;
    unsigned short green;
    signed char red, blue, fine, spread;
} grain_site;

typedef struct {
    unsigned short amplitude[1024];
    grain_site sites[];
} grain_scratch;

static inline __attribute__((always_inline)) unsigned grain_random(unsigned *state)
{
    unsigned x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

__attribute__((optimize("O3")))
static void grain_row(grain_site *row, int x0, int x1, const unsigned char *a, int rowlen, unsigned *state)
{
    const unsigned char *b = a + rowlen;
    int i = 1;
    for (int x = x0 & ~7; x < x1; x += 8, a += CAM_SENSOR_BITS_PER_PIXEL, b += CAM_SENSOR_BITS_PER_PIXEL) {
        int p[8], q[8];
        dec8(a, p);
        dec8(b, q);
        for (int k = (x < x0 ? x0 - x : 0); k < 8 && x + k < x1; k += 2, ++i) {
            unsigned r = grain_random(state);
            int d = (int)((r >> 24) + ((r >> 16) & 255)) - (int)(((r >> 8) & 255) + (r & 255));
            // Keep a soft tail of denser granules in the Bayer-cell texture.
            if (d > 192) d += (d - 192) / 2;
            if (d < -192) d += (d + 192) / 2;
            row[i].density = d;
            row[i].green = (p[k + 1] + q[k] + 1) >> 1;
            r = grain_random(state);
            row[i].red = (int)(r >> 24) - 128;
            row[i].blue = (int)((r >> 16) & 255) - 128;
            row[i].fine = (int)((r >> 8) & 255) - 128;
            row[i].spread = (int)((r >> 4) & 7) - 3;
        }
    }
    row[0] = row[1];
    row[i] = row[i - 1];
}

static inline __attribute__((always_inline)) int grain_abs(int x)
{
    return x < 0 ? -x : x;
}

static inline __attribute__((always_inline)) int grain_pixel(int value, int noise, int amp, int inverse_wb)
{
    if (value <= HP_BLK || value == 1023)
        return value;
    // Symmetric headroom keeps the texture centered near black and white.
    int limit = value - HP_BLK;
    if (1023 - value < limit) limit = 1023 - value;
    int delta = (noise * amp * inverse_wb + 32768) >> 16;
    if (delta > limit) delta = limit;
    if (delta < -limit) delta = -limit;
    return value + delta;
}

__attribute__((optimize("O3")))
int exfs_hq_grain(char *raw, const exfs_hq_profile *profile, int red_wb, int blue_wb)
{
    static const unsigned char amount_scale[] = { 0, 35, 49, 64, 80, 100, 124 };
    static const unsigned char size_gain[] = { 71, 69, 67, 64, 61, 58, 54 };
    static const unsigned char color_scale[] = { 0, 6, 11, 16, 22, 29, 38 };
    int amount = amount_scale[exfs_hq_clamp_level(*profile->levels[EXFS_HQ_AMOUNT]) - EXFS_HQ_LEVEL_MIN];
    if (!amount)
        return 1;
    int size = exfs_hq_clamp_level(*profile->levels[EXFS_HQ_SIZE]);
    int color = color_scale[exfs_hq_clamp_level(*profile->levels[EXFS_HQ_COLOR]) - EXFS_HQ_LEVEL_MIN];
    int coarse = profile->clump_weight + 2 * size;
    if (coarse < 0) coarse = 0;
    int fine = profile->fine_weight - 4 * size;
    int red_dye = (profile->red_dye * color + 8) >> 4;
    int blue_dye = (profile->blue_dye * color + 8) >> 4;
    int inv_red = 65536 / red_wb, inv_blue = 65536 / blue_wb;
    int gain = amount * size_gain[size - EXFS_HQ_LEVEL_MIN];

    // Process complete 2x2 cells inside the active area and leave optical black alone.
    int x0 = (camera_sensor.active_area.x1 + 1) & ~1;
    int x1 = camera_sensor.active_area.x2 & ~1;
    int y0 = (camera_sensor.active_area.y1 + 1) & ~1;
    int y1 = camera_sensor.active_area.y2 & ~1;
    if (x1 <= x0 || y1 <= y0)
        return 1;
    int rowlen = camera_sensor.raw_rowlen;
    int cells = (x1 - x0) / 2 + 2; // pad each end to clamp the guide at the active edge
    grain_scratch *scratch = malloc(sizeof(*scratch) + 3 * cells * sizeof(grain_site));
    if (!scratch)
        return 0;
    for (int v = 0; v < 1024; ++v) {
        int i = v >> 6, f = v & 63;
        int amp = profile->tone_amplitude[i] * (64 - f) + profile->tone_amplitude[i + 1] * f;
        scratch->amplitude[v] = (amp * gain + 131072) >> 18;
    }

    static unsigned shot_sequence;
    unsigned state = ((unsigned)get_tick_count() ^ (++shot_sequence * 0x9e3779b9u)) | 1u;
    int start = x0 & ~7;
    unsigned char *first = (unsigned char *)raw + y0 * rowlen + (start / 8) * CAM_SENSOR_BITS_PER_PIXEL;
    grain_site *above = scratch->sites, *row = above + cells, *below = row + cells;
    grain_row(row, x0, x1, first, rowlen, &state);
    memcpy(above, row, cells * sizeof(*row));
    for (int y = y0; y < y1; y += 2) {
        exfs_progress_row(y - y0, y1 - y0);
        unsigned char *a = first + (y - y0) * rowlen;
        if (y + 2 < y1)
            grain_row(below, x0, x1, a + 2 * rowlen, rowlen, &state);
        else
            memcpy(below, row, cells * sizeof(*row));
        unsigned char *b = a + rowlen;
        int cell = 1;
        for (int x = start; x < x1; x += 8, a += CAM_SENSOR_BITS_PER_PIXEL, b += CAM_SENSOR_BITS_PER_PIXEL) {
            int p[8], q[8];
            dec8(a, p);
            dec8(b, q);
            for (int k = (x < x0 ? x0 - x : 0); k < 8 && x + k < x1; k += 2, ++cell) {
                int r = p[k], g1 = p[k + 1], g2 = q[k], blue = q[k + 1];
                // Leave defect sentinels for Canon and keep their local interpolation clean.
                if (!(r && g1 && g2 && blue))
                    continue;
                int wr = HP_BLK + (((r - HP_BLK) * red_wb) >> 8);
                int wb = HP_BLK + (((blue - HP_BLK) * blue_wb) >> 8);
                int tone = clip10((4 * wr + 5 * (g1 + g2) + 2 * wb + 8) >> 4);
                int amp = scratch->amplitude[tone];
                if (!amp)
                    continue;
                const grain_site *s = row + cell;
                const grain_site *u = above + cell, *d = below + cell;
                const grain_site *l = s - 1, *h = s + 1;

                // The guide is the original green plane, retained across row
                // writes. Busy areas keep at least 75% of the local grain.
                int detail = grain_abs(h->green - l->green) + grain_abs(d->green - u->green)
                           + 2 * grain_abs(g1 - g2) - 12 - (tone >> 5);
                if (detail > 0) {
                    int taper = detail >> 2;
                    if (taper > 16) taper = 16;
                    amp = (amp * (64 - taper) + 32) >> 6;
                }

                // Each site has a different footprint. Interpolate between
                // sites to place fine luma variation within the CFA cell.
                int spread = coarse + s->spread;
                if (spread < 0) spread = 0;
                int adjacent = l->density + h->density + u->density + d->density;
                int luma = s->density + ((adjacent * spread + 16) >> 5) + ((s->fine * fine + 64) >> 7);
                int dx = (h->density - l->density) >> 4;
                int dy = (d->density - u->density) >> 4;

                // Small, zero-luma dye differences, separate from the density.
                int red = ((2 * s->red + l->red + h->red + u->red + d->red) * red_dye + 32) >> 6;
                int cb = ((2 * s->blue + l->blue + h->blue + u->blue + d->blue) * blue_dye + 32) >> 6;
                int green = -(2 * red + cb) / 5;
                p[k] = grain_pixel(r, luma - dx - dy + red, amp, inv_red);
                p[k + 1] = grain_pixel(g1, luma + dx - dy + green, amp, 256);
                q[k] = grain_pixel(g2, luma - dx + dy + green, amp, 256);
                q[k + 1] = grain_pixel(blue, luma + dx + dy + cb, amp, inv_blue);
            }
            enc8(a, p);
            enc8(b, q);
        }
        grain_site *next = above;
        above = row;
        row = below;
        below = next;
    }
    free(scratch);
    return 1;
}
