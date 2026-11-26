#ifndef EXFILMSIM_H
#define EXFILMSIM_H

typedef struct {
    int red, green, blue;
    int neutral_red, neutral_blue;
} exfs_gain_params;

int exfs_render_selected(char *raw, int developing, int pair_helper);
int exfs_hq_11_selected(void);
void exfs11_grain(char *raw, int ir, int ib, const short *amp, short *up, unsigned seed);
int exfs_needs_hot_pixels(void);
int exfs_get_develop_gains(exfs_gain_params *params);
void exfs_progress_row(int row, int rows);

#endif
