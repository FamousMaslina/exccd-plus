// Host tests for the exact parser, camera model and packed-RAW renderer.
// Build with gcc -std=c99 -O2 -Wall -Wextra -Icore tools/filmsim/test_exls.c
// core/exls_cube.c core/exls_lut.c -o bin/test-exls.exe
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "exls_lut.h"

#define CAM_SENSOR_BITS_PER_PIXEL 10
#include "filmsim_raw.h"

static int read_file(void *context, void *buffer, unsigned count)
{
    FILE *file = context;
    int n = fread(buffer, 1, count, file);
    return ferror(file) ? -1 : n;
}

static int read_short(void *context, void *buffer, unsigned count)
{
    return read_file(context, buffer, count > 2 ? 2 : count);
}

static int broken_read(void *context, void *buffer, unsigned count)
{
    (void)context;
    (void)buffer;
    (void)count;
    return -1;
}

static void write_cube(FILE *file, int size, int identity)
{
    fprintf(file, "\xef\xbb\xbf# test\r\nTITLE \"LUT #1\"  # inline comment\r\nLUT_3D_SIZE %d\r\n", size);
    fputs(identity ? "DOMAIN_MIN 0 0 0\r\nDOMAIN_MAX 1 1 1\r\n" :
          "LUT_3D_INPUT_RANGE 0.0e+00 +1.0e0\r\n", file);
    for (int b = 0; b < size; b++)
        for (int g = 0; g < size; g++)
            for (int r = 0; r < size; r++) {
                double a = (double)r / (size - 1), c = (double)g / (size - 1), d = (double)b / (size - 1);
                fprintf(file, "%.8f %.8f %.8f%s", identity ? a : d, c, identity ? d : a,
                        r == size - 1 && g == r && b == r ? "" : "\r\n");
            }
}

static int render_test(FILE *file, int strength)
{
    const int wb[] = {7092, 4096, 6738};
    exls_transform transform;
    unsigned short storage[40 / sizeof(short)];
    unsigned char *raw = (unsigned char *)storage;
    int a[8] = {170, 150, 200, 205, 1023, 180, 516, 751};
    int b[8] = {137, 70, 194, 0, 180, 180, 240, 122};
    enc8(raw + 10, a);
    enc8(raw + 20, b);
    memset(raw, 42, 10);
    memset(raw + 30, 42, 10);
    unsigned char before[40];
    memcpy(before, raw, sizeof(before));
    rewind(file);
    exls_result result = exls_build_transform(read_file, file, &transform, wb, strength);
    if (result == EXLS_BYPASS) {
        exls_apply_transform(&transform, raw, 10, 0, 1, 6, 3);
        assert(memcmp(raw, before, sizeof(before)) == 0);
        return 0;
    }
    assert(result == EXLS_OK);
    exls_apply_transform(&transform, raw, 10, 0, 1, 6, 3);
    free(transform.delta);
    int aa[8], bb[8];
    dec8(raw + 10, aa);
    dec8(raw + 20, bb);
    assert(memcmp(raw, before, 10) == 0 && memcmp(raw + 30, before + 30, 10) == 0);
    assert(memcmp(a + 2, aa + 2, 6 * sizeof(int)) == 0);
    assert(memcmp(b + 2, bb + 2, 6 * sizeof(int)) == 0);
    assert(aa[1] - bb[0] == a[1] - b[0]);
    assert(aa[0] < a[0] && bb[1] > b[1]);
    return (a[0] - aa[0]) + (bb[1] - b[1]);
}

static void test_cubes(void)
{
    const int sizes[] = {9, 17, 32, 33};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        for (int identity = 0; identity <= 1; identity++) {
            FILE *file = tmpfile();
            assert(file != NULL);
            write_cube(file, sizes[i], identity);
            rewind(file);
            exls_cube cube;
            assert(exls_read_cube(i ? read_file : read_short, file, &cube, NULL, NULL) == EXLS_OK);
            assert(cube.size == sizes[i] && cube.min[0] == 0 && cube.max[2] == 4096);
            int middle = render_test(file, 50), full = render_test(file, 100);
            assert(render_test(file, 0) == 0);
            assert(identity ? !middle && !full : middle > 0 && full > middle);
            fclose(file);
        }
    }
}

static void test_invalid(void)
{
    static const char *invalid[] = {
        "", "LUT_1D_SIZE 17", "LUT_3D_SIZE 64", "LUT_3D_SIZE 9\n0 0 0", "LUT_3D_SIZE 9\nNaN 0 0",
        "LUT_3D_SIZE 9\n0 +e4 0", "LUT_3D_SIZE 9\n0 9e100 0",
        "LUT_3D_SIZE 9\n0 0 0 0", "DOMAIN_MIN 0.2 0.4 1\nDOMAIN_MIN 0 0 0"
    };
    exls_cube cube;
    assert(exls_read_cube(broken_read, NULL, &cube, NULL, NULL) == EXLS_IO);
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        FILE *file = tmpfile();
        assert(file != NULL);
        fputs(invalid[i], file);
        rewind(file);
        assert(exls_read_cube(read_file, file, &cube, NULL, NULL) == EXLS_INVALID);
        fclose(file);
    }
    for (int i = 0; i < 2; i++) {
        FILE *file = tmpfile();
        assert(file != NULL);
        if (i) fputs("DOMAIN_MIN 0 1 0\n", file);
        fprintf(file, "LUT_3D_SIZE 9\n");
        for (int j = 0; j < 729 + !i; j++) fputs("0 0 0\n", file);
        rewind(file);
        assert(exls_read_cube(read_file, file, &cube, NULL, NULL) == EXLS_INVALID);
        rewind(file);
        exls_transform t;
        const int wb[] = {7092, 4096, 6738};
        assert(exls_build_transform(read_file, file, &t, wb, 100) == EXLS_INVALID && !t.delta);
        fclose(file);
    }
}

static void test_sample(const char *cube_path, const char *dng_path)
{
    enum { ROWS = 2340, STRIDE = 3940, BYTES = ROWS * STRIDE };
    const int wb[] = {7171, 4096, 6410}; // CRW_8512 daylight AsShotNeutral
    FILE *file = fopen(cube_path, "rb");
    assert(file != NULL);
    exls_cube cube;
    assert(exls_read_cube(read_file, file, &cube, NULL, NULL) == EXLS_OK);
    rewind(file);
    clock_t start = clock();
    exls_transform transform;
    assert(exls_build_transform(read_file, file, &transform, wb, 100) == EXLS_OK);
    fclose(file);
    clock_t setup = clock() - start;

    unsigned char *source = malloc(BYTES), *raw = malloc(BYTES);
    assert(source && raw);
    file = fopen(dng_path, "rb");
    assert(file && fseek(file, -BYTES, SEEK_END) == 0 && fread(source, 1, BYTES, file) == BYTES);
    fclose(file);
    for (int i = 0; i < BYTES; i += 2) {
        raw[i] = source[i + 1];
        raw[i + 1] = source[i];
    }
    memcpy(source, raw, BYTES);
    start = clock();
    exls_apply_transform(&transform, raw, STRIDE, 38, 14, 3146, 2338);
    clock_t render = clock() - start;
    assert(memcmp(raw, source, 14 * STRIDE) == 0);
    assert(memcmp(raw + 2338 * STRIDE, source + 2338 * STRIDE, 2 * STRIDE) == 0);
    int modified = 0;
    for (int i = 0; i < BYTES; i++) modified += raw[i] != source[i];
    assert(modified > BYTES / 10);
    printf("%d^3 full A620 RAW: %d changed packed bytes, host load+bake %.3fs, render %.3fs\n",
           cube.size, modified, (double)setup / CLOCKS_PER_SEC, (double)render / CLOCKS_PER_SEC);
    free(transform.delta);
    free(raw);
    free(source);
}

int main(int argc, char **argv)
{
    test_cubes();
    test_invalid();
    if (argc == 3) test_sample(argv[1], argv[2]);
    else assert(argc == 1);
    puts("EXLS tests passed");
    return 0;
}
