#ifndef EXLS_LUT_H
#define EXLS_LUT_H

#define EXLS_LUT_DIR "A/CHDK/LUTS"
#define EXLS_READ_SIZE 4096

typedef short exls_rgb[3];

typedef struct {
    int size;
    int min[3], max[3];
} exls_cube;

typedef enum {
    EXLS_OK, EXLS_BYPASS, EXLS_NO_LUT, EXLS_IO, EXLS_INVALID, EXLS_MEMORY
} exls_result;

typedef int (*exls_read_fn)(void *context, void *buffer, unsigned count);
typedef exls_result (*exls_node_fn)(void *context, const exls_cube *cube,
                                   unsigned index, const int value[3]);
exls_result exls_read_cube(exls_read_fn read_fn, void *context, exls_cube *cube,
                          exls_node_fn node_fn, void *node_context);

typedef struct {
    int size;
    unsigned short coord[1024];
    exls_rgb *delta;
} exls_transform;

// White balance gains relative to green in Q12, strength in percent.
exls_result exls_build_transform(exls_read_fn read_fn, void *context, exls_transform *transform,
                                 const int wb[3], int strength);
void exls_apply_transform(const exls_transform *transform, unsigned char *raw, int stride,
                          int left, int top, int right, int bottom);

#endif
