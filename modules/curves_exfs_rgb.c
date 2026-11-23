#include "exfilmsim.h"
#include "stdlib.h"

#define CAM_SENSOR_BITS_PER_PIXEL 10
#include "exfilmsim_11_raw.h"

void exfs_module_rgb_curve(unsigned char *raw, int rows, int rowpix, int rowlen,
                           const unsigned short *curve)
{
    exfs_rgb_curve(raw, rows, rowpix, rowlen, curve);
}

void exfs11_module_rgb_curve(unsigned char *raw, int rows, int rowpix, int rowlen,
                            const unsigned short *curve)
{
    unsigned *packed = malloc(4 * EXFS_PIXELS * sizeof(*packed));
    if (!packed) {
        exfs_rgb_curve(raw, rows, rowpix, rowlen, curve);
        return;
    }
    exfs11_build_rgb_table(curve, packed);
    exfs11_rgb_curve(raw, rows, rowpix, rowlen, packed);
    free(packed);
}
