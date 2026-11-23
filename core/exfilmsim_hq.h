#ifndef EXFILMSIM_HQ_H
#define EXFILMSIM_HQ_H

#define EXFS_HQ_VERSION "1.0"
#define EXFS_HQ_11_VERSION "1.1"

enum {
    EXFS_HQ_AMOUNT, EXFS_HQ_SIZE, EXFS_HQ_COLOR, EXFS_HQ_CONTROLS
};
#define EXFS_HQ_LEVEL_MIN (-3)
#define EXFS_HQ_LEVEL_MAX 3

typedef struct {
    const char *curve;
    int *levels[EXFS_HQ_CONTROLS];
    unsigned char tone_amplitude[17]; // Q8 RAW delta per unit of density, at 64-code intervals
    unsigned char clump_weight, fine_weight, red_dye, blue_dye;
    unsigned char sensor_chroma; // Q8 strength of the pre-curve chroma cleanup
} exfs_hq_profile;

static inline int exfs_hq_grain_enabled(const exfs_hq_profile *profile)
{
    return profile && *profile->levels[EXFS_HQ_AMOUNT] > EXFS_HQ_LEVEL_MIN;
}

const exfs_hq_profile *exfs_hq_profile_for_curve(const char *curve);
const exfs_hq_profile *exfs_hq_active_profile(void);
int exfs_hq_clamp_level(int level);
int exfs_hq_grain(char *raw, const exfs_hq_profile *profile, int red_wb, int blue_wb);
int exfs_hq11_grain(char *raw, const exfs_hq_profile *profile, int red_wb, int blue_wb);
int exfs_hq11_chroma(char *raw, int strength, int red_wb, int blue_wb);
void filmsim_effects_apply_hq(char *raw, const exfs_hq_profile *profile);
void exfs11_render(char *raw, int developing, int pair_helper, const exfs_hq_profile *profile);

#endif
