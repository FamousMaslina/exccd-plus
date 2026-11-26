#include "platform.h"
#include "camera_info.h"
#include "conf.h"
#include "modules.h"
#include "curves.h"
#include "filmsim.h"
#include "exfilmsim.h"
#include "exfilmsim_hq.h"

#define FS_PROG_ROW(y, rows) exfs_progress_row(y, rows)
#include "exfilmsim_raw.h"

extern void filmsim_hotpix_hook(char *raw);
extern void filmsim_double_hook(char *raw);
extern void filmsim_cnr_hook(char *raw);
extern void filmsim_effects_apply(char *raw);

static void exfs_correct_hot_pixels(char *raw)
{
    if (!exfs_needs_hot_pixels())
        return;

    short *scratch = malloc(HP_BUF_SHORTS(camera_sensor.raw_rowpix) * sizeof(*scratch));
    if (!scratch) {
        filmsim_hotpix_hook(raw);
        return;
    }
    exfs_hot_pixels((unsigned char *)raw, camera_sensor.raw_rows, camera_sensor.raw_rowpix,
                    camera_sensor.raw_rowlen, scratch);
    free(scratch);
}

static void exfs_finish_develop(char *raw)
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

static void exfs_render_pipeline(char *raw, int developing, int pair_helper,
                                 const exfs_hq_profile *hq_profile)
{
    if (!developing)
        filmsim_shot_prepare();

    filmsim_perf_mark(PERF_HOT_PIXELS);
    filmsim_prog_step(PS_HOT);
    exfs_correct_hot_pixels(raw);

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
    if (hq_profile)
        filmsim_effects_apply_hq(raw, hq_profile);
    else
        filmsim_effects_apply(raw);
    if (developing) {
        filmsim_perf_mark(PERF_WHITE_BALANCE);
        exfs_finish_develop(raw);
        filmsim_perf_mark(PERF_NONE);
    } else {
        filmsim_shot_end();
    }
}

static void exfs_render(char *raw, int developing, int pair_helper)
{
    exfs_render_pipeline(raw, developing, pair_helper, NULL);
}

static void exfs_hq_render(char *raw, int developing, int pair_helper, const exfs_hq_profile *profile)
{
    exfs_render_pipeline(raw, developing, pair_helper, profile);
}

int exfs_hq_11_selected(void)
{
    return conf.film_exfs_hq_11 || conf.film_exfs_hq_111;
}

int exfs_render_selected(char *raw, int developing, int pair_helper)
{
    filmsim_engine engine = filmsim_selected_engine();
    if (engine == FILMSIM_ENGINE_EXCCD || engine == FILMSIM_ENGINE_EXLS)
        return 0;

    const exfs_hq_profile *profile = exfs_hq_active_profile();
    if (engine == FILMSIM_ENGINE_EXFS_11
        || engine == FILMSIM_ENGINE_HQ_11 || engine == FILMSIM_ENGINE_HQ_111) {
        filmsim_perf_set_engine(profile ? engine : FILMSIM_ENGINE_EXFS_11);
        exfs11_render(raw, developing, pair_helper, profile);
        return 1;
    }
    filmsim_perf_set_engine(profile ? engine : FILMSIM_ENGINE_EXFS_10);
    if (profile)
        exfs_hq_render(raw, developing, pair_helper, profile);
    else
        exfs_render(raw, developing, pair_helper);
    return 1;
}
