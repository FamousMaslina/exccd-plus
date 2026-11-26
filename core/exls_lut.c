#include "stdlib.h"
#include "string.h"
#include "exls_lut.h"

#define CAM_SENSOR_BITS_PER_PIXEL 10
#include "filmsim_raw.h"

enum { ONE = 4096, BLACK = 31, WHITE = 1023, SPAN = WHITE - BLACK, GRID = 17 };

// Neutral A620 JPEG approximation. Q12, with unit row sums. Fitted to
// white-balanced linear RAW and My Colors Off JPEG pairs (see doc/exls.md).
static const short camera_to_rgb[3][3] = {
    { 5625, -1398, -131 }, { 96, 3848, 152 }, { -179, -2790, 7065 }
};
static const short rgb_to_camera[3][3] = {
    { 2965, 1100, 31 }, { -75, 4265, -94 }, { 45, 1712, 2339 }
};
static const unsigned short tone_x[] = {
    0, 44, 64, 87, 114, 144, 178, 215, 256, 300, 348, 400, 455, 514, 576, 642,
    711, 784, 860, 940, 1024, 1111, 1202, 1296, 1394, 1495, 1600, 1708, 1820,
    1936, 2055, 2178, 2304, 2434, 2567, 2704, 2844, 2988, 3136, 3287, 3442,
    3600, 3762, 3927, 4096
};
static const unsigned short tone_y[] = {
    0, 246, 284, 392, 518, 642, 789, 956, 1104, 1252, 1397, 1548, 1704, 1856,
    1963, 2069, 2202, 2315, 2427, 2535, 2633, 2764, 2857, 2933, 3044, 3134,
    3216, 3330, 3406, 3449, 3511, 3567, 3621, 3675, 3731, 3799, 3839, 3866,
    3906, 3924, 3957, 3991, 4025, 4060, 4096
};

static inline __attribute__((always_inline)) int limit(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// Select the four nodes along the tetrahedron's diagonal. A coordinate
// packs its cell index above a 9-bit fraction (0..256, inclusive).
static inline __attribute__((always_inline)) void interpolate(const exls_rgb *nodes,
                                                              int n, unsigned r, unsigned g,
                                                              unsigned b, int out[3])
{
    const exls_rgb *p = nodes + (r >> 9) + (g >> 9) * n + (b >> 9) * (n * n);
    int f0 = r & 511, f1 = g & 511, f2 = b & 511;
    int s0 = 1, s1 = n, s2 = n * n;
    int tmp;
#define SWAP(a, b) do { tmp = (a); (a) = (b); (b) = tmp; } while (0)
    if (f0 < f1) { SWAP(f0, f1); SWAP(s0, s1); }
    if (f1 < f2) {
        SWAP(f1, f2); SWAP(s1, s2);
        if (f0 < f1) { SWAP(f0, f1); SWAP(s0, s1); }
    }
#undef SWAP
    const short *a = p[0], *c = p[s0], *d = p[s0 + s1], *e = p[s0 + s1 + s2];
    int w0 = 256 - f0, w1 = f0 - f1, w2 = f1 - f2;
    for (int k = 0; k < 3; k++)
        out[k] = (a[k] * w0 + c[k] * w1 + d[k] * w2 + e[k] * f2 + 128) >> 8;
}

static unsigned position(int v, int lo, int hi, int steps)
{
    if (v <= lo) return 0;
    if (v >= hi) return ((steps - 1) << 9) | 256;
    unsigned q = lo == 0 && hi == ONE ? v * steps >> 4 : (v - lo) * (steps * 256) / (hi - lo);
    return (q >> 8 << 9) | (q & 255);
}

// Store every fourth Q12 code. Interpolate in between to save 12 KiB.
static void tone_table(unsigned short *table, const unsigned short *x, const unsigned short *y)
{
    unsigned i = 0;
    for (int j = 0; j < ONE; j += 4) {
        while (x[i + 1] <= j) i++;
        int span = x[i + 1] - x[i];
        table[j >> 2] = y[i] + ((y[i + 1] - y[i]) * (j - x[i]) + span / 2) / span;
    }
    table[ONE >> 2] = ONE;
}

static int tone_at(const unsigned short *table, int value)
{
    if (value <= 0) return 0;
    if (value >= ONE) return ONE;
    int i = value >> 2, f = value & 3;
    return (table[i] * (4 - f) + table[i + 1] * f + 2) >> 2;
}

static int dot(const short m[3], const int rgb[3])
{
    return (m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2] + ONE / 2) >> 12;
}

typedef struct {
    exls_transform *transform;
    const int *wb;
    int strength, identity;
    int balanced[3][GRID], scale[3];
    unsigned char next_blue[GRID][GRID];
    unsigned short (*tone)[(ONE >> 2) + 1];
    exls_rgb *slices;
    int slice, offset;
} transform_builder;

static exls_result start_builder(transform_builder *builder, int n)
{
    exls_transform *transform = builder->transform;
    builder->slices = malloc(2 * n * n * sizeof(exls_rgb));
    if (!builder->slices) return EXLS_MEMORY;
    builder->tone = malloc(2 * sizeof(*builder->tone));
    if (!builder->tone) return EXLS_MEMORY;
    transform->size = GRID;
    transform->delta = malloc(GRID * GRID * GRID * sizeof(exls_rgb));
    if (!transform->delta) {
        transform->size = 13;
        transform->delta = malloc(13 * 13 * 13 * sizeof(exls_rgb));
        if (!transform->delta) return EXLS_MEMORY;
    }
    tone_table(builder->tone[0], tone_x, tone_y);
    tone_table(builder->tone[1], tone_y, tone_x);
    // A quadratic axis spends more nodes near the noise floor and in shadows.
    const int *wb = builder->wb;
    int steps = transform->size - 1, axis[GRID];
    for (int i = 0; i <= steps; i++) {
        axis[i] = i * i * ONE / (steps * steps);
        for (int c = 0; c < 3; c++)
            builder->balanced[c][i] = (axis[i] * wb[c] + ONE / 2) >> 12;
    }
    int cell = 0;
    for (int i = 0; i <= WHITE; i++) {
        int lin = (limit(i - BLACK, 0, SPAN) * ONE + SPAN / 2) / SPAN;
        while (cell + 1 < steps && axis[cell + 1] <= lin) cell++;
        transform->coord[i] = (cell << 9) |
            ((lin - axis[cell]) * 256 / (axis[cell + 1] - axis[cell]));
    }
    for (int c = 0; c < 3; c++) builder->scale[c] = (SPAN * 8 * ONE + wb[c] / 2) / wb[c];
    return EXLS_OK;
}

static void bake_slices(transform_builder *builder, const exls_cube *cube)
{
    int n = cube->size;
    int grid = builder->transform->size;
    int strength = builder->strength;
    // At fixed sensor R and G, modelled display blue increases monotonically
    // with sensor B. Finish nodes while their two source slices are resident.
    for (int g = 0; g < grid; g++)
        for (int r = 0; r < grid; r++) {
            int b = builder->next_blue[g][r];
            int raw_rgb[3] = {builder->balanced[0][r], builder->balanced[1][g], 0};
            for (; b < grid; b++) {
                raw_rgb[2] = builder->balanced[2][b];
                int display[3], graded[3];
                display[2] = tone_at(builder->tone[0], dot(camera_to_rgb[2], raw_rgb));
                unsigned bz = position(display[2], cube->min[2], cube->max[2], n - 1);
                if ((int)(bz >> 9) >= builder->slice) break;
                display[0] = tone_at(builder->tone[0], dot(camera_to_rgb[0], raw_rgb));
                display[1] = tone_at(builder->tone[0], dot(camera_to_rgb[1], raw_rgb));
                unsigned rx = position(display[0], cube->min[0], cube->max[0], n - 1);
                unsigned gy = position(display[1], cube->min[1], cube->max[1], n - 1);
                interpolate(builder->slices, n, rx, gy, bz & 511, graded);
                for (int c = 0; c < 3; c++) {
                    int target = (display[c] * (100 - strength) + limit(graded[c], 0, ONE) * strength + 50) / 100;
                    // Invert only the look's delta to retain out-of-gamut RAW headroom.
                    graded[c] = tone_at(builder->tone[1], target) - tone_at(builder->tone[1], display[c]);
                }
                short *node = builder->transform->delta[(b * grid + g) * grid + r];
                for (int c = 0; c < 3; c++)
                    node[c] = (dot(rgb_to_camera[c], graded) * builder->scale[c] + ONE / 2) >> 12;
            }
            builder->next_blue[g][r] = b;
        }
}

static exls_result add_node(void *context, const exls_cube *cube,
                            unsigned index, const int value[3])
{
    transform_builder *builder = context;
    int n = cube->size, plane = n * n;
    if (!index) {
        exls_result result = start_builder(builder, n);
        if (result != EXLS_OK) return result;
    }
    if (builder->identity) {
        int v[] = {builder->offset % n, builder->offset / n, builder->slice};
        for (int c = 0; c < 3; c++) {
            int expected = cube->min[c] + (cube->max[c] - cube->min[c]) * v[c] / (n - 1);
            if (abs(value[c] - expected) > 1) builder->identity = 0;
        }
    }
    short *destination = builder->slices[(builder->slice ? plane : 0) + builder->offset];
    for (int c = 0; c < 3; c++) destination[c] = value[c];
    if (++builder->offset == plane) {
        builder->offset = 0;
        if (builder->slice) {
            bake_slices(builder, cube);
            memcpy(builder->slices, builder->slices + plane, plane * sizeof(exls_rgb));
        }
        builder->slice++;
    }
    return EXLS_OK;
}

exls_result exls_build_transform(exls_read_fn read_fn, void *context, exls_transform *transform,
                                const int wb[3], int strength)
{
    transform->delta = 0;
    if (strength <= 0) return EXLS_BYPASS;
    transform_builder builder = { .transform = transform, .wb = wb, .strength = strength, .identity = 1 };
    exls_cube cube;
    exls_result result = exls_read_cube(read_fn, context, &cube, add_node, &builder);
    free(builder.slices);
    free(builder.tone);
    if (result == EXLS_OK && builder.identity) result = EXLS_BYPASS;
    if (result != EXLS_OK) {
        free(transform->delta);
        transform->delta = 0;
    }
    return result;
}

static inline __attribute__((always_inline)) void adjust(const exls_transform *t, int *a, int *b)
{
    int r = a[0], g = (a[1] + b[0] + 1) >> 1, blue = b[1];
    int peak = r > blue ? r : blue;
    int peak_g = a[1] > b[0] ? a[1] : b[0];
    if (peak_g > peak) peak = peak_g;
    if (peak == WHITE || !r || !a[1] || !b[0] || !blue) return;
    int d[3];
    interpolate(t->delta, t->size, t->coord[r], t->coord[g], t->coord[blue], d);
    // Give clipped highlights back to Canon's native saturation handling.
    if (peak > WHITE - 64) {
        int weight = WHITE - peak;
        for (int c = 0; c < 3; c++) d[c] = (d[c] * weight + 32) >> 6;
    }
    a[0] = limit(r + ((d[0] + 4) >> 3), 1, WHITE);
    b[1] = limit(blue + ((d[2] + 4) >> 3), 1, WHITE);
    int floor_g = a[1] < b[0] ? a[1] : b[0];
    int dg = limit((d[1] + 4) >> 3, 1 - floor_g, WHITE - peak_g);
    a[1] += dg;
    b[0] += dg;
}

__attribute__((optimize("O2"))) void exls_apply_transform(const exls_transform *t, unsigned char *raw,
                                                          int stride, int left, int top,
                                                          int right, int bottom)
{
    if (!t->delta) return;
    // Keep optical-black margins and the original sampling grid intact.
    int first = left & ~7;
    for (int y = top; y < bottom; y += 2) {
        unsigned char *p = raw + y * stride + (first / 8) * 10;
        unsigned char *q = p + stride;
        for (int x = first; x < right; x += 8, p += 10, q += 10) {
            int a[8], b[8];
            dec8(p, a);
            dec8(q, b);
            if (x >= left && x + 8 <= right) {
                for (int i = 0; i < 8; i += 2) adjust(t, a + i, b + i);
            } else {
                for (int i = 0; i < 8; i += 2)
                    if (x + i >= left && x + i < right) adjust(t, a + i, b + i);
            }
            enc8(p, a);
            enc8(q, b);
        }
    }
}
