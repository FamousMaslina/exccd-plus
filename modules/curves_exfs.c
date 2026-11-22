// Keep the stock file loader and module interface, so EXFS uses the exact
// same prepared tables as EXCCD, including white balance and dynamic range.
void curve_apply(void);
#define curve_apply() curve_apply_exccd()
#include "curves.c"
#undef curve_apply

void exfs_module_rgb_curve(unsigned char *raw, int rows, int rowpix, int rowlen,
                           const unsigned short *curve);

static int exfs_can_apply_rgb(void)
{
    if (!conf.film_exfs || conf.curve_enable != 1 || current_curve_type != CURVE_CUSTOM)
        return 0;
    for (int i = 0; i < 4 * CURVE_SIZE; i++)
        if (curve_data[i] > 1023)
            return 0;
    return 1;
}

void curve_apply(void)
{
    if (exfs_can_apply_rgb()) {
        exfs_module_rgb_curve((unsigned char *)get_raw_image_addr(), camera_sensor.raw_rows,
                              camera_sensor.raw_rowpix, camera_sensor.raw_rowlen, curve_data);
    } else {
        curve_apply_exccd();
    }
}
