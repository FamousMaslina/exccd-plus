#ifndef EXLS_H
#define EXLS_H

#include "exls_lut.h"

#ifdef FILMSIM_EXLS
int exls_valid_path(const char *path);
int exls_strength(void);
const char *exls_name(void);
const char *exls_message(exls_result result);
exls_result exls_check_file(const char *path);
exls_result exls_render(char *raw, const int wb[3]);
#endif

#endif
