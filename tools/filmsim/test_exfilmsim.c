// Build from the repo root with gcc -Os -fno-inline -Icore tools/filmsim/test_exfilmsim.c.
// Optionally pass a 10-bit RGB .CV file followed by one or more A620 CHDK DNGs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "exfilmsim.h"
#define CAM_SENSOR_BITS_PER_PIXEL 10
#include "exfilmsim_raw.h"
#include "filmsim_dev.h"

enum { WIDTH = 3152, HEIGHT = 2340, STRIDE = WIDTH * 10 / 8, RAW_SIZE = STRIDE * HEIGHT };

static unsigned random_state = 1;

static unsigned next_random(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state >> 12;
}

static int check_pixels(const char *stage, const unsigned char *reference, const unsigned char *fast)
{
    for (int i = 0; i < RAW_SIZE; i++) {
        if (reference[i] != fast[i]) {
            fprintf(stderr, "%s mismatch at RAW byte %d (%u vs %u)\n", stage, i, reference[i], fast[i]);
            return 0;
        }
    }
    return 1;
}

static int check_hot_lanes(void)
{
    for (unsigned hi = 0; hi < 1024; hi++) {
        for (unsigned lo = 0; lo < 1024; lo++) {
            unsigned a = hi | (lo << 16);
            unsigned b = lo | (hi << 16);
            unsigned expected = (hi > 47 && (int)lo < HP_LIM((int)hi)) ? 0x8000 : 0;
            if (lo > 47 && (int)hi < HP_LIM((int)lo))
                expected |= 0x80000000u;
            if (exfs_hot_lanes(a, b) != expected) {
                fprintf(stderr, "Hot pixel candidate mismatch for %u and %u\n", hi, lo);
                return 0;
            }
        }
    }
    return 1;
}

static void fill_frame(unsigned char *raw, int sample)
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x += 8, raw += 10) {
            int v[8];
            for (int i = 0; i < 8; i++) {
                unsigned n = next_random();
                v[i] = (n % 127 == 0) ? 0 : 32 + (n & 15) + (x / 32) + (y / 16);
                if (n % 257 == 0)
                    v[i] = 1000 + (n % 24);
                if (sample == 1 && (y < 10 || y > HEIGHT - 10 || x < 16 || x > WIDTH - 24))
                    v[i] = n & 1023;
                if (sample == 2 && (x + y) % 128 < 16)
                    v[i] = 991 + n % 33;
            }
            enc8(raw, v);
        }
    }
}

static void reference_rgb(unsigned char *raw, const unsigned short *curve)
{
    for (int y = 0; y < HEIGHT; y++) {
        const unsigned short *c = curve + (y & 1) * 2048;
        for (int x = 0; x < WIDTH; x += 8, raw += 10) {
            unsigned a = c[(raw[1] << 2) | (raw[0] >> 6)];
            unsigned b = c[1024 + ((raw[0] & 63) << 4) + (raw[3] >> 4)];
            unsigned d = c[((raw[3] & 15) << 6) | (raw[2] >> 2)];
            unsigned e = c[1024 + ((raw[2] & 3) << 8) + raw[5]];
            raw[1] = a >> 2;
            raw[0] = (a << 6) | (b >> 4);
            raw[3] = (b << 4) | (d >> 6);
            raw[2] = (d << 2) | (e >> 8);
            raw[5] = e;
            a = c[(raw[4] << 2) | (raw[7] >> 6)];
            b = c[1024 + ((raw[7] & 63) << 4) + (raw[6] >> 4)];
            d = c[((raw[6] & 15) << 6) | (raw[9] >> 2)];
            e = c[1024 + ((raw[9] & 3) << 8) + raw[8]];
            raw[4] = a >> 2;
            raw[7] = (a << 6) | (b >> 4);
            raw[6] = (b << 4) | (d >> 6);
            raw[9] = (d << 2) | (e >> 8);
            raw[8] = e;
        }
    }
}

typedef struct {
    clock_t hot, curve, wb;
} timings;

static int compare_render(unsigned char *reference, unsigned char *fast, short *scratch,
                          const unsigned short *curve, const exfs_gain_params *gains,
                          timings *exccd, timings *exfs)
{
    static unsigned short gain_lut[3][1024];
    memcpy(fast, reference, RAW_SIZE);
    clock_t start = clock();
    hotpix_fix(reference, HEIGHT, WIDTH, STRIDE, scratch);
    exccd->hot += clock() - start;
    start = clock();
    exfs_hot_pixels(fast, HEIGHT, WIDTH, STRIDE, scratch);
    exfs->hot += clock() - start;
    if (!check_pixels("Hot pixels", reference, fast))
        return 0;

    start = clock();
    reference_rgb(reference, curve);
    exccd->curve += clock() - start;
    start = clock();
    exfs_rgb_curve(fast, HEIGHT, WIDTH, STRIDE, curve);
    exfs->curve += clock() - start;
    if (!check_pixels("RGB curve", reference, fast))
        return 0;

    start = clock();
    raw_gain(reference, HEIGHT, WIDTH, STRIDE, 0, 31, gains->red, gains->green, gains->blue,
             1000, gains->neutral_red, gains->neutral_blue);
    exccd->wb += clock() - start;
    start = clock();
    exfs_raw_gain(fast, HEIGHT, WIDTH, STRIDE, gains, gain_lut);
    exfs->wb += clock() - start;
    return check_pixels("White balance", reference, fast);
}

static int read_curve(const char *name, unsigned short *curve)
{
    unsigned char data[8192];
    FILE *file = fopen(name, "rb");
    if (!file)
        return 0;
    int ok = fread(data, 1, sizeof(data), file) == sizeof(data) && fgetc(file) == EOF;
    fclose(file);
    if (!ok)
        return 0;
    for (int i = 0; i < 4096; i++) {
        curve[i] = dng_u16(data + 2 * i);
        if (curve[i] > 1023)
            return 0;
    }
    return 1;
}

static int read_camera_dng(const char *name, unsigned char *raw, dng_meta_t *meta)
{
    unsigned char head[DNG_HEAD_MAX];
    FILE *file = fopen(name, "rb");
    if (!file)
        return 0;
    int len = fread(head, 1, sizeof(head), file);
    int ok = dng_parse(head, len, WIDTH, HEIGHT, 10, meta);
    if (ok) {
        unsigned sub = dng_value(head, len, dng_u32(head + 4), 0x14A, 0);
        unsigned data_size = dng_value(head, len, sub, 0x117, 0);
        unsigned offset = dng_value(head, len, sub, 0x111, 0);
        ok = data_size == RAW_SIZE && offset >= 8
            && dng_value(head, len, sub, 0x103, 0) == 1
            && fseek(file, offset, SEEK_SET) == 0
            && fread(raw, 1, RAW_SIZE, file) == RAW_SIZE;
    }
    fclose(file);
    if (!ok)
        return 0;
    // Restore the packed byte order used by load_dng_to_rawbuffer on the camera.
    for (int i = 0; i < RAW_SIZE; i += 2) {
        unsigned char byte = raw[i];
        raw[i] = raw[i + 1];
        raw[i + 1] = byte;
    }
    return 1;
}

static void print_timings(timings exccd, timings exfs)
{
    const double ms = 1000.0 / CLOCKS_PER_SEC;
    printf("Host milliseconds, EXCCD / EXFS: hot pixels %.1f / %.1f, RGB curve %.1f / %.1f, white balance %.1f / %.1f.\n",
           exccd.hot * ms, exfs.hot * ms, exccd.curve * ms, exfs.curve * ms, exccd.wb * ms, exfs.wb * ms);
}

int main(int argc, char **argv)
{
    unsigned char *reference = malloc(RAW_SIZE);
    unsigned char *fast = malloc(RAW_SIZE);
    short *scratch = malloc(sizeof(*scratch) * HP_BUF_SHORTS(WIDTH));
    static unsigned short curve[4096];
    static const exfs_gain_params gains[] = {
        {4096, 4096, 4096, 2158, 2826},
        {7813, 4096, 2905, 1219, 8010},
        {1024, 16384, 19121, 6633, 2178},
    };
    timings exccd = {0}, exfs = {0};

    if (!reference || !fast || !scratch || !check_hot_lanes())
        return 1;

    for (int sample = 0; sample < 3; sample++) {
        fill_frame(reference, sample);
        for (int i = 0; i < 4096; i++)
            curve[i] = next_random() & 1023;
        if (!compare_render(reference, fast, scratch, curve, &gains[sample], &exccd, &exfs))
            return 1;
    }

    puts("Bit-identical hot pixels, RGB curve and white balance on three synthetic A620 RAW buffers.");
    print_timings(exccd, exfs);
    if (argc > 1) {
        if (argc < 3 || !read_curve(argv[1], curve)) {
            fprintf(stderr, "Expected a 10-bit RGB curve followed by A620 DNGs.\n");
            return 1;
        }
        exccd = (timings){0};
        exfs = (timings){0};
        for (int i = 2; i < argc; i++) {
            dng_meta_t meta;
            if (!read_camera_dng(argv[i], reference, &meta)) {
                fprintf(stderr, "Could not read the A620 RAW strip in %s\n", argv[i]);
                return 1;
            }
            const int helper_r = 1560, helper_g = 1000, helper_b = 1440;
            const exfs_gain_params wb = {
                wb_factor(meta.wb_r, meta.wb_g, helper_r, helper_g),
                WB_ONE,
                wb_factor(meta.wb_b, meta.wb_g, helper_b, helper_g),
                WB_ONE * helper_g / helper_r,
                WB_ONE * helper_g / helper_b,
            };
            if (!compare_render(reference, fast, scratch, curve, &wb, &exccd, &exfs)) {
                fprintf(stderr, "Mismatch while rendering %s\n", argv[i]);
                return 1;
            }
        }
        printf("Bit-identical output for %d captured A620 DNGs with %s.\n", argc - 2, argv[1]);
        print_timings(exccd, exfs);
    }

    free(scratch);
    free(fast);
    free(reference);
    return 0;
}
