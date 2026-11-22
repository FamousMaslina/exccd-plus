#include "exfilmsim.h"

#define CAM_SENSOR_BITS_PER_PIXEL 10
#include "exfilmsim_raw.h"

void exfs_module_rgb_curve(unsigned char *raw, int rows, int rowpix, int rowlen,
                           const unsigned short *curve)
{
    exfs_rgb_curve(raw, rows, rowpix, rowlen, curve);
}
