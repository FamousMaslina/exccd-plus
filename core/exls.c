#include "platform.h"
#include "camera_info.h"
#include "conf.h"
#include "stdio.h"
#include "exls.h"
#include "filmsim.h"

int exls_valid_path(const char *path)
{
    unsigned len = 0;
    while (len < sizeof(conf.film_exls_file) && path[len]) len++;
    if (len < 7 || len >= sizeof(conf.film_exls_file) ||
        path[0] != 'A' || path[1] != '/') return 0;
    const char *ext = strrchr(path, '.');
    if (!ext || (len - (ext - path) != 4 && len - (ext - path) != 5)) return 0;
    // FAT short aliases for .cube files end in .CUB.
    return (ext[1] | 32) == 'c' && (ext[2] | 32) == 'u' && (ext[3] | 32) == 'b' &&
        (!ext[4] || ((ext[4] | 32) == 'e' && !ext[5]));
}

int exls_strength(void)
{
    int v = conf.film_exls_strength;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

const char *exls_name(void)
{
    if (!exls_valid_path(conf.film_exls_file)) return NULL;
    return strrchr(conf.film_exls_file, '/') + 1;
}

const char *exls_message(exls_result result)
{
    switch (result) {
        case EXLS_NO_LUT: return "No Cube selected";
        case EXLS_IO: return "Cannot read Cube file";
        case EXLS_INVALID: return "Invalid or unsupported Cube";
        case EXLS_MEMORY: return "Not enough memory for Cube";
        default: return NULL;
    }
}

typedef struct {
#ifdef HOST_TEST
    int fd;
#else
    FILE *handle;
#endif
    unsigned left;
} exls_file;

static exls_result open_file(const char *path, exls_file *file)
{
    if (!exls_valid_path(path)) return EXLS_NO_LUT;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) return EXLS_IO;
    file->left = st.st_size;
#ifdef HOST_TEST
    file->fd = open(path, O_RDONLY, 0777);
    return file->fd >= 0 ? EXLS_OK : EXLS_IO;
#else
    file->handle = fopen(path, "rb");
    return file->handle ? EXLS_OK : EXLS_IO;
#endif
}

static int read_chunk(void *context, void *buffer, unsigned count)
{
    exls_file *file = context;
    if (count > file->left) count = file->left;
    if (!count) return 0;
#ifdef HOST_TEST
    int n = read(file->fd, buffer, count);
#else
    int n = fread(buffer, 1, count, file->handle);
#endif
    if (n <= 0 || n > (int)count) return -1;
    file->left -= n;
    return n;
}

static void close_file(exls_file *file)
{
#ifdef HOST_TEST
    close(file->fd);
#else
    fclose(file->handle);
#endif
}

exls_result exls_check_file(const char *path)
{
    exls_file file;
    exls_result result = open_file(path, &file);
    if (result != EXLS_OK) return result;
    exls_cube cube;
    result = exls_read_cube(read_chunk, &file, &cube, NULL, NULL);
    close_file(&file);
    return result;
}

exls_result exls_render(char *raw, const int wb[3])
{
    int strength = exls_strength();
    if (!strength) return EXLS_BYPASS;
    exls_file file;
    exls_result result = open_file(conf.film_exls_file, &file);
    if (result != EXLS_OK) return result;

    // Keep large allocations shot-local, including on parsing and bake failure.
    exls_transform *transform = malloc(sizeof(*transform));
    result = transform ? exls_build_transform(read_chunk, &file, transform, wb, strength) : EXLS_MEMORY;
    close_file(&file);
    if (result == EXLS_OK) {
        filmsim_perf_mark(PERF_MIX);
        exls_apply_transform(transform, (unsigned char *)raw, camera_sensor.raw_rowlen,
                             camera_sensor.active_area.x1, camera_sensor.active_area.y1,
                             camera_sensor.active_area.x2, camera_sensor.active_area.y2);
        free(transform->delta);
    }
    free(transform);
    return result;
}
