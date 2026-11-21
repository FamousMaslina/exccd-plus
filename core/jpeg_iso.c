#include "platform.h"
#include "camera_info.h"
#include "dirent.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "remotecap_core.h"
#include "jpeg_iso.h"

#define ISO_TAG 0x8827
#define EXIF_IFD_TAG 0x8769
#define IFD_ENTRY_SIZE 12
#define EXIF_TIFF_MAX (65535 - 8) // APP1 length includes the length field and "Exif\0\0"

typedef struct {
    unsigned char *data;
    unsigned size;
    int little_endian;
} tiff_header;

typedef struct {
    unsigned entry;
    unsigned ifd;
    unsigned ifd_size;
} tiff_iso_patch;

static unsigned get_be16(const unsigned char *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static unsigned tiff_get(const tiff_header *tiff, unsigned offset, unsigned width)
{
    const unsigned char *p = tiff->data + offset;
    unsigned value = 0;
    for (unsigned i = 0; i < width; ++i)
        value = (value << 8) | p[tiff->little_endian ? width - 1 - i : i];
    return value;
}

static void tiff_put(const tiff_header *tiff, unsigned offset, unsigned width, unsigned value)
{
    unsigned char *p = tiff->data + offset;
    for (unsigned i = 0; i < width; ++i) {
        p[tiff->little_endian ? i : width - 1 - i] = value & 255;
        value >>= 8;
    }
}

static unsigned ifd_size(const tiff_header *tiff, unsigned offset)
{
    if (offset < 8 || offset > tiff->size || tiff->size - offset < 6)
        return 0;
    unsigned size = 6 + IFD_ENTRY_SIZE * tiff_get(tiff, offset, 2);
    return size <= tiff->size - offset ? size : 0;
}

static unsigned ifd_find(const tiff_header *tiff, unsigned offset, unsigned size, unsigned tag)
{
    for (unsigned entry = offset + 2; entry < offset + size - 4; entry += IFD_ENTRY_SIZE) {
        if (tiff_get(tiff, entry, 2) == tag)
            return entry;
    }
    return 0;
}

static unsigned ifd_pointer(const tiff_header *tiff, unsigned entry)
{
    if (entry && tiff_get(tiff, entry + 2, 2) == 4 && tiff_get(tiff, entry + 4, 4) == 1)
        return tiff_get(tiff, entry + 8, 4);
    return 0;
}

// Bound the IFD and all its out-of-line values, including the entire MakerNote.
static unsigned ifd_data_end(const tiff_header *tiff, unsigned offset, unsigned size)
{
    static const unsigned char type_width[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    unsigned end = offset + size;
    for (unsigned entry = offset + 2; entry < offset + size - 4; entry += IFD_ENTRY_SIZE) {
        unsigned type = tiff_get(tiff, entry + 2, 2);
        unsigned count = tiff_get(tiff, entry + 4, 4);
        if (type == 0 || type >= sizeof(type_width) || count > tiff->size / type_width[type])
            return tiff->size;
        unsigned bytes = count * type_width[type];
        if (bytes > 4) {
            unsigned start = tiff_get(tiff, entry + 8, 4);
            if (start > tiff->size - bytes)
                return tiff->size;
            if (end < start + bytes)
                end = start + bytes;
        }
    }
    return end;
}

static unsigned thumbnail_padding(const tiff_header *tiff, unsigned root, unsigned root_size,
                                  unsigned exif, unsigned exif_size, unsigned needed)
{
    unsigned preview = tiff_get(tiff, root + root_size - 4, 4);
    unsigned preview_size = ifd_size(tiff, preview);
    if (!preview_size || tiff_get(tiff, preview + preview_size - 4, 4) ||
        tiff_get(tiff, exif + exif_size - 4, 4) ||
        ifd_find(tiff, root, root_size, 0x014a) ||
        ifd_find(tiff, root, root_size, 0x0111) ||
        ifd_find(tiff, preview, preview_size, 0x0111))
        return 0;

    // Canon reserves zero-filled space before the embedded JPEG thumbnail.
    unsigned thumbnail = ifd_pointer(tiff, ifd_find(tiff, preview, preview_size, 0x0201));
    if (thumbnail < needed || thumbnail > tiff->size - 2 ||
        get_be16(tiff->data + thumbnail) != 0xffd8)
        return 0;
    unsigned start = (thumbnail - needed) & ~1u;
    if (start < ifd_data_end(tiff, root, root_size) ||
        start < ifd_data_end(tiff, exif, exif_size) ||
        start < ifd_data_end(tiff, preview, preview_size))
        return 0;

    unsigned nested[] = {ifd_find(tiff, root, root_size, 0x8825),
                         ifd_find(tiff, exif, exif_size, 0xa005)};
    for (unsigned i = 0; i < sizeof(nested) / sizeof(nested[0]); ++i) {
        if (nested[i]) {
            unsigned offset = ifd_pointer(tiff, nested[i]);
            unsigned size = ifd_size(tiff, offset);
            if (!size || tiff_get(tiff, offset + size - 4, 4) ||
                start < ifd_data_end(tiff, offset, size))
                return 0;
        }
    }
    for (unsigned i = start; i < thumbnail; ++i) {
        if (tiff->data[i] != 0)
            return 0;
    }
    return start;
}

// Relocate only the IFD entries when inserting the tag. All existing value
// offsets, including Canon's MakerNote and thumbnail offsets, stay valid.
static unsigned tiff_set_iso(unsigned char *data, unsigned size, unsigned capacity, unsigned iso,
                             tiff_iso_patch *patch)
{
    if (size < 8 || !iso || iso > 65535 ||
        !((data[0] == 'I' || data[0] == 'M') && data[0] == data[1]))
        return 0;
    tiff_header tiff = {data, size, data[0] == 'I'};
    if (tiff_get(&tiff, 2, 2) != 42)
        return 0;
    unsigned root = tiff_get(&tiff, 4, 4);
    unsigned root_size = ifd_size(&tiff, root);
    if (!root_size)
        return 0;
    unsigned pointer = ifd_find(&tiff, root, root_size, EXIF_IFD_TAG);
    unsigned exif = ifd_pointer(&tiff, pointer);
    unsigned exif_size = ifd_size(&tiff, exif);
    if (!exif_size)
        return 0;

    unsigned entry = ifd_find(&tiff, exif, exif_size, ISO_TAG);
    if (!entry) {
        unsigned needed = exif_size + IFD_ENTRY_SIZE;
        unsigned dest = thumbnail_padding(&tiff, root, root_size, exif, exif_size, needed);
        if (!dest) {
            dest = (size + 1) & ~1u;
            if (dest > capacity || needed > capacity - dest)
                return 0;
            if (dest > size)
                data[size] = 0;
            size = dest + needed;
        }
        unsigned first = exif + 2;
        unsigned insert = first;
        while (insert < exif + exif_size - 4 && tiff_get(&tiff, insert, 2) < ISO_TAG)
            insert += IFD_ENTRY_SIZE;
        entry = dest + 2 + insert - first;
        memcpy(data + dest + 2, data + first, insert - first);
        memcpy(data + entry + IFD_ENTRY_SIZE, data + insert, exif + exif_size - insert);
        tiff_put(&tiff, dest, 2, tiff_get(&tiff, exif, 2) + 1);
        tiff_put(&tiff, pointer + 8, 4, dest);
        if (patch) {
            patch->entry = pointer + 8;
            patch->ifd = dest;
            patch->ifd_size = needed;
        }
    } else if (patch) {
        patch->entry = entry;
        patch->ifd_size = 0;
    }
    tiff_put(&tiff, entry, 2, ISO_TAG);
    tiff_put(&tiff, entry + 2, 2, 3); // SHORT
    tiff_put(&tiff, entry + 4, 4, 1);
    tiff_put(&tiff, entry + 8, 2, iso);
    tiff_put(&tiff, entry + 10, 2, 0);
    return size;
}

// Locate the length field of the EXIF APP1 segment (before the image scan).
static unsigned jpeg_exif_length(const unsigned char *data, unsigned size)
{
    if (size < 2 || get_be16(data) != 0xffd8)
        return 0;
    unsigned pos = 2;
    while (size - pos >= 4 && data[pos++] == 0xff) {
        unsigned marker;
        do {
            marker = data[pos++];
        } while (marker == 0xff && size - pos >= 2);
        if (marker == 0xda || marker == 0xd9 || size - pos < 2)
            break;
        unsigned length = get_be16(data + pos);
        if (length < 2 || length > size - pos)
            break;
        if (marker == 0xe1 && length >= 16 && memcmp(data + pos + 2, "Exif\0\0", 6) == 0)
            return pos;
        pos += length;
    }
    return 0;
}

#define SHOT_SLOTS 16
#define ISO_RETRY_MS 250
#define ISO_TIMEOUT_MS 30000

typedef struct {
    volatile unsigned key; // file number and ISO, published together to FileWriteTask
    volatile unsigned patched_key;
    unsigned tick;
    unsigned next_check;
    char dir[32];
    int stitch;
} jpeg_iso_shot;

static jpeg_iso_shot shots[SHOT_SLOTS];

static unsigned jpeg_number(const char *name)
{
    unsigned len = strlen(name);
    if (len < 8 || strcmp(name + len - 4, ".JPG") != 0)
        return 0;
    unsigned number = 0;
    for (unsigned i = len - 8; i < len - 4; ++i) {
        unsigned digit = (unsigned char)name[i] - '0';
        if (digit > 9)
            return 0;
        number = number * 10 + digit;
    }
    return number;
}

void jpeg_iso_capture(int iso)
{
    unsigned number = get_target_file_num();
    if (iso <= 0 || iso > 65535 || number == 0 || number > 9999)
        return;
    jpeg_iso_shot *shot = &shots[number % SHOT_SLOTS];
    shot->key = 0;
    shot->patched_key = 0;
    shot->tick = get_tick_count();
    shot->next_check = shot->tick + ISO_RETRY_MS;
    shot->stitch = camera_info.state.mode_shooting == MODE_STITCH;
    shot->dir[0] = 0;
    if (!remotecap_get_target())
        get_target_dir_name(shot->dir);
    shot->key = number << 16 | (unsigned)iso;
}

int jpeg_iso_filewrite(const char *name, unsigned char *data, unsigned size)
{
    unsigned number = jpeg_number(name);
    jpeg_iso_shot *shot = &shots[number % SHOT_SLOTS];
    unsigned key = shot->key;
    if (!key || key >> 16 != number || (unsigned)get_tick_count() - shot->tick >= ISO_TIMEOUT_MS)
        return 0;
    unsigned length = jpeg_exif_length(data, size);
    if (!length)
        return 0;
    unsigned tiff_size = get_be16(data + length) - 8;
    if (!tiff_set_iso(data + length + 8, tiff_size, tiff_size, key & 0xffff, 0))
        return 0;
    if (shot->key == key)
        shot->patched_key = key;
    return 1;
}

// If there is no room for a new IFD in APP1, stage the larger JPEG beside the
// original. Keep the original until the entire replacement has been written.
static int jpeg_stage_file(int fd, const char *temp, unsigned char *header,
                           unsigned old_end, unsigned new_end, unsigned file_size)
{
    int out = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0)
        return 0;

    int done = write(out, header, new_end) == (int)new_end &&
               lseek(fd, old_end, SEEK_SET) >= 0;
    unsigned remaining = file_size - old_end;
    while (done && remaining) {
        unsigned size = remaining < 8192 ? remaining : 8192;
        done = read(fd, header, size) == (int)size && write(out, header, size) == (int)size;
        remaining -= size;
    }
    if (close(out) != 0)
        done = 0;
    if (!done)
        remove(temp);
    return done;
}

static int jpeg_install_staged_file(const char *path, const char *temp, const char *backup)
{
    if (rename(path, backup) == 0) {
        if (rename(temp, path) == 0) {
            remove(backup);
            return 1;
        }
        rename(backup, path);
    }
    remove(temp);
    return 0;
}

static int jpeg_write_at(int fd, unsigned offset, const unsigned char *data, unsigned size)
{
    return lseek(fd, offset, SEEK_SET) >= 0 && write(fd, data, size) == (int)size;
}

// Used when the platform has no FileWriteTask hook or the hook could not
// update the header in place. The complete JPEG must be available on disk.
static int jpeg_iso_update_file(const char *path, unsigned iso)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 2)
        return 0;
    int fd = open(path, O_RDWR, 0777);
    if (fd < 0)
        return 0;
    unsigned char end[2];
    if (lseek(fd, st.st_size - 2, SEEK_SET) < 0 || read(fd, end, 2) != 2 ||
        get_be16(end) != 0xffd9 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return 0;
    }

    unsigned header_size = st.st_size < 65537 ? st.st_size : 65537;
    unsigned char *header = malloc(65537);
    int done = 0;
    int staged = 0;
    char temp[64], backup[64];
    if (header && read(fd, header, header_size) == (int)header_size) {
        unsigned length = jpeg_exif_length(header, header_size);
        if (length) {
            unsigned tiff_size = get_be16(header + length) - 8;
            unsigned offset = length + 8;
            unsigned capacity = 65537 - offset;
            if (capacity > EXIF_TIFF_MAX)
                capacity = EXIF_TIFF_MAX;
            tiff_iso_patch patch;
            unsigned updated = tiff_set_iso(header + offset, tiff_size, capacity, iso, &patch);
            if (updated == tiff_size) {
                // Publish the directory pointer only after writing the new IFD.
                done = (!patch.ifd_size ||
                        jpeg_write_at(fd, offset + patch.ifd, header + offset + patch.ifd, patch.ifd_size)) &&
                       jpeg_write_at(fd, offset + patch.entry, header + offset + patch.entry,
                                     patch.ifd_size ? 4 : IFD_ENTRY_SIZE);
            } else if (updated > tiff_size && !shooting_in_progress()) {
                int base = strlen(path) - 4;
                strcpy(temp, path);
                strcpy(backup, path);
                strcpy(temp + base, ".TMP");
                strcpy(backup + base, ".BAK");
                if (stat(temp, 0) != 0 && stat(backup, 0) != 0) {
                    unsigned app1_size = updated + 8;
                    header[length] = app1_size >> 8;
                    header[length + 1] = app1_size & 255;
                    staged = jpeg_stage_file(fd, temp, header, offset + tiff_size,
                                             offset + updated, st.st_size);
                }
            }
        }
    }
    free(header);
    close(fd);
    if (staged)
        done = jpeg_install_staged_file(path, temp, backup);
    if (done) {
        struct utimbuf time = {st.st_mtime, st.st_mtime};
        utime(path, &time);
    }
    return done;
}

void jpeg_iso_tick(void)
{
    unsigned tick = get_tick_count();
    for (unsigned i = 0; i < SHOT_SLOTS; ++i) {
        jpeg_iso_shot *shot = &shots[i];
        unsigned key = shot->key;
        if (!key)
            continue;
        if (tick - shot->tick >= ISO_TIMEOUT_MS) {
            shot->key = 0;
            continue;
        }
        if (shot->patched_key == key || !shot->dir[0] || (int)(tick - shot->next_check) < 0)
            continue;

        shot->next_check = tick + ISO_RETRY_MS;
        char path[64];
        sprintf(path, "%s/IMG_%04u.JPG", shot->dir, key >> 16);
        int done = jpeg_iso_update_file(path, key & 0xffff);
        if (!done && shot->stitch) {
            DIR *dir = opendir(shot->dir);
            if (dir) {
                struct dirent *entry;
                while (!done && (entry = readdir(dir))) {
                    if (strlen(entry->d_name) == 12 && jpeg_number(entry->d_name) == key >> 16) {
                        sprintf(path, "%s/%s", shot->dir, entry->d_name);
                        done = jpeg_iso_update_file(path, key & 0xffff);
                    }
                }
                closedir(dir);
            }
        }
        if (done && shot->key == key)
            shot->key = 0;
    }
}
