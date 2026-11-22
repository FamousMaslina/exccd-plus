// Build the A620's original engine verbatim, then expose the state the
// opt-in renderer needs to keep the same recipe and RAW development results.
#include "filmsim.c"
#include "exfilmsim.h"

int exfs_needs_hot_pixels(void)
{
    return filmsim_photo_mode() && raw_touched(0);
}

int exfs_get_develop_gains(exfs_gain_params *params)
{
    if (dev_end_fr == WB_ONE && dev_end_fg == WB_ONE && dev_end_fb == WB_ONE)
        return 0;
    *params = (exfs_gain_params){ dev_end_fr, dev_end_fg, dev_end_fb, dev_end_nr, dev_end_nb };
    return 1;
}

void exfs_progress_row(int row, int rows)
{
    prog_rows(row, rows);
}
