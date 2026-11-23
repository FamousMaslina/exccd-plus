#include "platform.h"
#include "camera_info.h"
#include "conf.h"
#include "modules.h"
#include "curves.h"
#include "filmsim.h"
#include "exfilmsim.h"
#include "exfilmsim_hq.h"

#define FS_PROG_ROW(y, rows) exfs_progress_row(y, rows)
#include "exfilmsim_11_raw.h"

extern void filmsim_double_hook(char *raw);
extern void filmsim_cnr_hook(char *raw);

void exfs11_grain(char *raw, int ir, int ib, const short *amp, short *up, unsigned seed)
{
    exfs11_grain_pixels((unsigned char *)raw, camera_sensor.raw_rows, camera_sensor.raw_rowpix,
                        camera_sensor.raw_rowlen, ir, ib, amp, up, seed);
}

static void exfs11_correct_hot_pixels(char *raw)
{
    if (!exfs_needs_hot_pixels())
        return;

    unsigned scratch[3 * EXFS11_HOT_WORDS(CAM_RAW_ROWPIX)] = {0};
    exfs11_hot_pixels((unsigned char *)raw, camera_sensor.raw_rows, camera_sensor.raw_rowpix,
                      camera_sensor.raw_rowlen, scratch);
}

static void exfs11_finish_develop(char *raw)
{
    exfs_gain_params params;
    if (exfs_get_develop_gains(&params)) {
        unsigned short (*lut)[EXFS_PIXELS] = malloc(3 * sizeof(*lut));
        if (!lut) {
            filmsim_develop_end(raw);
            return;
        }
        filmsim_prog_step(PS_WB);
        exfs_raw_gain((unsigned char *)raw, camera_sensor.raw_rows, camera_sensor.raw_rowpix,
                      camera_sensor.raw_rowlen, &params, lut);
        free(lut);
    }
    filmsim_develop_end(NULL);
}

void exfs11_render(char *raw, int developing, int pair_helper, const exfs_hq_profile *profile)
{
    if (!developing)
        filmsim_shot_prepare();

    filmsim_perf_mark(PERF_HOT_PIXELS);
    filmsim_prog_step(PS_HOT);
    exfs11_correct_hot_pixels(raw);

    filmsim_perf_mark(PERF_DOUBLE);
    filmsim_prog_step(PS_DOUBLE);
    if (!developing || pair_helper)
        filmsim_double_hook(raw);

    filmsim_perf_mark(PERF_COLOR_NR);
    filmsim_prog_step(PS_CNR);
    filmsim_cnr_hook(raw);

    filmsim_perf_mark(PERF_CURVE);
    filmsim_prog_step(PS_CURVE);
    filmsim_dr_curve_hook(0);
#ifdef OPT_CURVES
    if (conf.curve_enable)
        libcurves->curve_apply();
#endif

    filmsim_perf_mark(PERF_MIX);
    filmsim_prog_step(PS_MIX);
    filmsim_raw_apply(raw);
    filmsim_perf_mark(PERF_NONE);

    filmsim_prog_step(PS_GLOW);
    filmsim_effects_apply_hq(raw, profile);
    if (developing) {
        filmsim_perf_mark(PERF_WHITE_BALANCE);
        exfs11_finish_develop(raw);
        filmsim_perf_mark(PERF_NONE);
    } else {
        filmsim_shot_end();
    }
}
