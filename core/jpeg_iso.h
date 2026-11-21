#ifndef JPEG_ISO_H
#define JPEG_ISO_H

void jpeg_iso_capture(int iso);
void jpeg_iso_tick(void);
int jpeg_iso_filewrite(const char *name, unsigned char *data, unsigned size);

#endif
