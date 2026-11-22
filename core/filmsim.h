#ifndef FILMSIM_H
#define FILMSIM_H

// Filmsimulationen fuer CHDK (IXUS 60 / SD600)
// Kombiniert Canon "My Colors / Custom Color" (Propcases) mit
// RAW-Tonkurven (curves.flt) zu waehlbaren Looks.

#define FILMSIM_PRODUCT_NAME "EXCCD+"
#define FILMSIM_VERSION "v1.2"

extern const char* filmsim_menu_enum(int change, int arg);
extern const char* filmsim_name(void);
extern void filmsim_apply(void);
extern void filmsim_tick(void);
extern void filmsim_draw_osd(void);
extern void filmsim_set(int v);
extern void filmsim_open_menu(void);
extern void filmsim_open_catalog(int presets);
extern void filmsim_label_release(void);
extern int filmsim_splash_init(void);
extern void filmsim_draw_splash(void);
extern void filmsim_splash_free(void);
extern int filmsim_quick_mode(int key);
extern int filmsim_quick_print_mode(int in_alt);
extern int filmsim_quick_open(int state);
extern int filmsim_quick_iso_open(void);
extern void filmsim_normalize_quick_buttons(void);
extern void filmsim_raw_apply(char *raw);
extern int filmsim_photo_mode(void);
extern int filmsim_picker_allowed(void);
extern int filmsim_osd_hidden(void);
extern const char* filmsim_double_enum(int change, int arg);
extern const char* filmsim_bwfilter_enum(int change, int arg);
extern const char* filmsim_mist_enum(int change, int arg);
extern const char* filmsim_halation_enum(int change, int arg);
extern const char* filmsim_vignette_enum(int change, int arg);
extern const char* filmsim_leak_enum(int change, int arg);
extern const char* filmsim_datestamp_enum(int change, int arg);
extern const char* filmsim_grain_enum(int change, int arg);
extern const char* filmsim_frame_enum(int change, int arg);
extern int filmsim_develop_armed(void);
extern int filmsim_develop_iso(void);
extern void filmsim_develop_select(const char *fn);
extern void filmsim_develop_cancel(void);
extern void filmsim_develop_begin(char *raw);
extern void filmsim_develop_prepare(void);
extern void filmsim_develop_end(char *raw);
extern void filmsim_shot_prepare(void);
extern void filmsim_shot_end(void);

enum { PS_LOAD, PS_HOT, PS_DOUBLE, PS_CNR, PS_CURVE, PS_MIX, PS_GLOW, PS_GRAIN, PS_WB, PS_N };
extern void filmsim_prog_step(int step);

typedef enum {
    FILMSIM_PERF_SINGLE, FILMSIM_PERF_PAIR, FILMSIM_PERF_DEVELOP
} filmsim_perf_mode;

typedef enum {
    PERF_NONE = -1,
    PERF_RAW_SPOOL, PERF_RAW_OUTPUT, PERF_ORIGINAL_JPEG, PERF_HELPER_WAIT,
    PERF_RAW_LOAD, PERF_DEVELOP_SETUP, PERF_BAD_PIXELS, PERF_HOT_PIXELS,
    PERF_DOUBLE, PERF_COLOR_NR, PERF_CURVE, PERF_MIX, PERF_GLOW_TONE,
    PERF_GRAIN, PERF_FRAME, PERF_DATE_STAMP, PERF_WHITE_BALANCE, PERF_FILM_LOG, PERF_CUSTOM_JPEG,
    PERF_STAGE_COUNT
} filmsim_perf_stage;

extern const char *filmsim_perf_enum(int change, int arg);
extern void filmsim_perf_begin(filmsim_perf_mode mode);
extern void filmsim_perf_mark(filmsim_perf_stage stage);
extern void filmsim_perf_raw_done(int original);

#ifdef FILMSIM_JPEG_PAIR
extern const char *filmsim_pair_enum(int change, int arg);
extern void filmsim_pair_prepare_capture(void);
extern int filmsim_pair_original(char *raw, int iso);
extern int filmsim_pair_helper(void);
extern void filmsim_pair_perf_helper_begin(void);
extern void filmsim_pair_helper_raw(int loaded);
extern int filmsim_pair_kbd(void);
extern void filmsim_pair_filewrite(const char *name);
extern void filmsim_pair_file_closed(void);
#endif
extern int filmsim_dng_mark(const char *fn);
extern const char* filmsim_dr_enum(int change, int arg);
extern void filmsim_reset_settings(void);
extern const char* filmsim_wb_enum(int change, int arg);
extern const char* filmsim_wb_r_enum(int change, int arg);
extern const char* filmsim_wb_b_enum(int change, int arg);
extern const char* filmsim_wb_shift_enum(int change, int arg);
extern const char* filmsim_recipe_enum(int change, int arg);
extern void filmsim_recipe_save_menu(int arg);
extern char filmsim_slot_text[4][32];
extern char filmsim_look_text[40];
extern void filmsim_recipe_save(int arg);
extern int filmsim_dr_tv96_shift(int automatic);
extern int filmsim_autoiso(void);
extern const char* filmsim_aiso_enum(int change, int arg);
extern const char* filmsim_aiso_max_enum(int change, int arg);
extern const char* filmsim_aiso_str(void);
extern void filmsim_dr_curve_hook(int end);
extern const char* filmsim_cnr_enum(int change, int arg);
extern const char* filmsim_zoom_enum(int change, int arg);
extern const char* filmsim_zoom_str(void);
extern int filmsim_zoom_kbd(void);
extern void filmsim_zoom_tick(void);

#endif
