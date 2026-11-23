// Nachgebildete Kamera fuer tools/filmsim/test_filmsim.py: alles, was core/filmsim.c von
// CHDK braucht, als Stummel (Tasten, Bildschirm, Karte im Speicher, Propcases), dazu die
// Testszenarien. Bewusst dieselben CHDK-Header wie die Firmware.

#include "platform.h"
#include "stdlib.h"
#include "camera_info.h"
#include "conf.h"
#include "properties.h"
#include "shooting.h"
#include "gui_draw.h"
#include "modules.h"
#include "curves.h"
#include "gui.h"
#include "gui_menu.h"
#include "keyboard.h"
#include "filmsim.h"
#ifdef FILMSIM_EXFS
#include "exfilmsim.h"
#include "exfilmsim_hq.h"
#endif
#include "filmsim_txt.h"
#include "lang.h"
#include "gui_lang.h"
#include "lang_host.h"

extern int printf(const char *, ...);
extern int _open(const char *, int, ...);        // Windows-Dateizugriff fuer die echten Kurven
extern int _read(int, void *, unsigned int);
extern int _close(int);
extern int _write(int, const void *, unsigned int);
extern void exit(int);

// --- Pruefen ----------------------------------------------------------------
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { if (++fails <= 25) { printf("FEHLER Zeile %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// --- Kamerazustand ----------------------------------------------------------
Conf conf;
#include "camera_info.c"                         // camera_sensor, camera_screen, camera_info wie in der Firmware

long link_text_start, link_data_start, link_bss_start, link_bss_end;
static unsigned char host_colors[64];
unsigned char *chdk_colors = host_colors;

int canon_menu_active = 0;
char canon_shoot_menu_active = 0;
CMenu root_menu;
CMenu exccd_menu;
int recreview_hold;
static CMenu *menu_open;                 // zuletzt geoeffnetes CHDK-Menue
CMenu *get_curr_menu(void) { return menu_open; }
static void menu_redraw(int f) { (void)f; }
static int menu_kbd(void) { return 0; }
gui_handler menuGuiHandler = { .redraw = menu_redraw, .kbd_process = menu_kbd };
// Eintrag mit diesem Text im offenen Menue
static const CMenuItem *menu_item(int text)
{
    const CMenuItem *i;
    for (i = menu_open ? menu_open->menu : 0; i && i->text; i++)
        if (i->text == text) return i;
    return 0;
}
static const char *menu_enum(int text, int change)
{
    const CMenuItem *i = menu_item(text);
    return i ? ((const char *(*)(int, int))i->value)(change, i->arg) : "?";
}
static int now_ms = 10000;
long get_tick_count() { return now_ms; }
int usb_remote_active = 0;

// --- Texte ------------------------------------------------------------------
char *lang_str(int id)
{
    int i;
    if (id >= 100000) return (char *)host_lits[id - 100000];
    for (i = 0; host_lang[i].s; i++)
        if (host_lang[i].id == id) return (char *)host_lang[i].s;
    return "?";
}

// --- Karte im Speicher ------------------------------------------------------
#define MAX_FILES 32
static struct { char name[64]; char *data; int size; int pos; } card[MAX_FILES];

static int fs_find(const char *n)
{
    int i;
    for (i = 0; i < MAX_FILES; i++)
        if (card[i].data && strcmp(card[i].name, n) == 0) return i;
    return -1;
}
static int fs_put(const char *n, const char *d, int len)
{
    int i = fs_find(n);
    if (i < 0) { for (i = 0; i < MAX_FILES && card[i].data; i++); }
    if (i >= MAX_FILES) { printf("Karte voll\n"); exit(2); }
    strcpy(card[i].name, n);
    card[i].data = malloc(len + 200000);
    if (len) memcpy(card[i].data, d, len);
    card[i].size = len;
    card[i].pos = 0;
    return i;
}
int open(const char *n, int flags, int mode)
{
    int i = fs_find(n);
    (void)mode;
    if (i < 0) {
        if (!(flags & O_CREAT)) return -1;
        i = fs_put(n, "", 0);
    }
    if (flags & O_TRUNC) card[i].size = 0;
    card[i].pos = 0;
    return i + 100;
}
int close(int fd) { (void)fd; return 0; }
int read(int fd, void *b, long n)
{
    int i = fd - 100, left = card[i].size - card[i].pos;
    if (n > left) n = left;
    memcpy(b, card[i].data + card[i].pos, n);
    card[i].pos += n;
    return n;
}
int write(int fd, const void *b, long n)
{
    int i = fd - 100;
    memcpy(card[i].data + card[i].pos, b, n);
    card[i].pos += n;
    if (card[i].pos > card[i].size) card[i].size = card[i].pos;
    return n;
}
int lseek(int fd, long off, int whence)
{
    int i = fd - 100;
    card[i].pos = whence == 2 ? card[i].size + off : off;
    return card[i].pos;
}
int stat(const char *n, struct stat *st)
{
    int i = fs_find(n);
    if (i < 0) return -1;
    if (st) st->st_size = card[i].size;
    return 0;
}
int remove(const char *n)
{
    int i = fs_find(n);
    if (i >= 0) card[i].data = 0;
    return 0;
}

// --- Propcases, Bildschirm, Tasten -----------------------------------------
static short props[512];
int shooting_get_prop(int id) { return props[id & 511]; }
void shooting_set_prop(int id, int v) { props[id & 511] = (short)v; }
short shooting_get_iso_market() { return 100; }
short shooting_get_canon_iso_mode(void) { return 0; }
static int selected_iso_mode;
void shooting_set_iso_mode(int v) { selected_iso_mode = v; }

// Belichtung fuer die Auto-ISO (APEX, 1/96 EV); Kamera: ISO 100..800, 35 mm Weitwinkel
#include <math.h>
const ISOTable iso_table[] = { {0, 0, "Auto", -1}, {1, 100, "100", -1}, {2, 200, "200", -1},
                               {3, 400, "400", -1}, {4, 800, "800", -1} };
const unsigned int ISO_SIZE = 5;
static short ex_tv, ex_sv, ex_isomode;
static int ex_fl = 35000;
short shooting_get_tv96() { return ex_tv; }
short shooting_get_sv96_real() { return ex_sv; }
int shooting_get_iso_mode() { return ex_isomode; }
void shooting_set_tv96_direct(short v, short now) { (void)now; ex_tv = v; }
void shooting_set_sv96(short v, short now) { (void)now; ex_sv = v; }
short shooting_iso_market_to_real(short i) { return i; }
short shooting_get_sv96_from_iso(short iso) { return iso > 0 ? (short)(log(iso * 32.0 / 100.0) / log(2.0) * 96.0 + 0.5) : 0; }
short shooting_get_tv96_from_shutter_speed(float t) { return (short)(-log(t) / log(2.0) * 96.0 + (t < 1 ? 0.5 : -0.5)); }
long lens_get_zoom_point() { return 0; }
int get_effective_focal_length(int zp) { (void)zp; return ex_fl; }

static char drawn[4096];
static int restore_count = 0;
void gui_set_need_restore(void) { restore_count++; }
static void drawn_add(const char *s)
{
    if (strlen(drawn) + strlen(s) + 2 < sizeof(drawn)) { strcat(drawn, s); strcat(drawn, "\n"); }
}
int draw_string(coord x, coord y, const char *s, twoColors c) { (void)x; (void)y; (void)c; drawn_add(s); return 0; }
void draw_string_scaled(coord x, coord y, const char *s, twoColors c, int a, int b) { (void)x; (void)y; (void)c; (void)a; (void)b; drawn_add(s); }
void draw_rectangle(coord a, coord b, coord c, coord d, twoColors e, int f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
// Bildschirm fuer das Vorschaubild-Szenario: Palettenindex je Pixel, -1 = nicht gezeichnet
#define FB_W 480
#define FB_H 270
static short fb[FB_H][FB_W];
void draw_vline(coord a, coord b, int c, color d)
{
    for (; c > 0; c--, b++)
        if (a >= 0 && a < FB_W && b >= 0 && b < FB_H) fb[b][a] = d;
}
void draw_hline(coord a, coord b, int c, color d)
{
    for (; c > 0; c--, a++)
        if (a >= 0 && a < FB_W && b >= 0 && b < FB_H) fb[b][a] = d;
}
twoColors user_color(confColor c) { (void)c; return 0; }

static gui_handler *cur_mode = 0;
gui_handler *gui_set_mode(gui_handler *h) { gui_handler *o = cur_mode; cur_mode = h; return o; }
int gui_is_mode(const gui_handler *h) { return cur_mode == h; }
void *vid_get_bitmap_active_palette(void) { return NULL; }
int vid_get_palette_size(void) { return 0; }
void gui_enum_value_change(int *value, int change, unsigned n)
{
    *value = (*value + change + (int)n) % (int)n;
}
static int menu_redraws = 0;
void gui_menu_erase_and_redraw(void) { menu_redraws++; }
void gui_menu_init(CMenu *m) { menu_open = m; }
void gui_activate_sub_menu(CMenu *m) { menu_open = m; }
static int alt_exits = 0;
void exit_alt(void) { alt_exits++; }
void exit_alt_on_release(void) { alt_exits++; }
// RAW-Serie (filmsim_batch_*): hier nur gelinkt, nicht ausgefuehrt
#include "action_stack.h"
AS_ID action_stack_create(action_func f) { (void)f; return 1; }
long action_top(int n) { (void)n; return 0; }
void action_push(long p) { (void)p; }
void action_push_press(long k) { (void)k; }
void action_push_release(long k) { (void)k; }
void action_push_click(long k) { (void)k; }
long action_pop_func(int n) { (void)n; return 0; }
void action_push_func(action_func f) { (void)f; }
int action_stack_is_finished(AS_ID id) { (void)id; return 1; }
void action_stack_kill(AS_ID id) { (void)id; }
int shooting_in_progress() { return camera_info.state.state_shooting_progress != SHOOTING_PROGRESS_NONE; }
int shooting_is_flash_ready() { return 1; }
long kbd_get_pressed_key() { return 0; }
void kbd_key_release_all() { }
static long clicked_key;
long kbd_is_key_clicked(long k) { return k && k == clicked_key; }
#ifdef FILMSIM_JPEG_PAIR
static long injected_key;
static int pair_errors;
void kbd_key_press(long k) { injected_key = k; }
void kbd_key_release(long k) { if (injected_key == k) injected_key = 0; }
int is_raw_possible(void) { return 1; }
short shooting_get_drive_mode(void) { return 0; }
short shooting_get_flash_mode(void) { return shooting_get_prop(PROPCASE_FLASH_MODE); }
int remotecap_get_target(void) { return 0; }
unsigned int GetFreeCardSpaceKb(void) { return 512000; }
void console_add_line(const char *text) { (void)text; pair_errors++; }
#endif
static int conf_saves = 0;
void conf_save(void) { conf_saves++; }

static long next_key = 0;
long kbd_get_autoclicked_key(void) { long k = next_key; next_key = 0; return k; }
static long key_held = 0;               // gedrueckt gehaltene Taste
long kbd_is_key_pressed(long k) { return k == key_held; }
long get_jogdial_direction(void) { return 0; }
// Modulstummel
#ifdef OPT_CURVES
static void curve_init_stub(void) {}
static libcurves_sym curves_stub;
libcurves_sym *libcurves = &curves_stub;
#endif
void filmsim_zoom_tick(void) {}
const char *filmsim_zoom_str(void) { return 0; }
const char *filmsim_tv_str(void) { return 0; }
const char *filmsim_focus_str(void) { return 0; }
static int file_number = 123;
long get_target_file_num(void) { return file_number; }
void get_target_dir_name(char *d) { strcpy(d, "A/DCIM/100CANON"); }
static struct tm tm_now = { .tm_year = 126, .tm_mon = 10, .tm_mday = 7 };
struct tm *get_localtime(void) { return &tm_now; }
#ifdef FILMSIM_JPEG_PAIR
struct tm *localtime(const time_t *t) { (void)t; return &tm_now; }
time_t time(time_t *t) { if (t) *t = 1; return 1; }
#endif

// RAW entwickeln: Datei, die raw.c laden wuerde; Weissabgleich des Hilfsschusses (G, R, B)
static char prep_fn[64];
void raw_prepare_develop(const char *f, int prompt) { (void)prompt; if (f) strcpy(prep_fn, f); else prep_fn[0] = 0; }
static int host_wb[3] = { 1000, 1000, 1000 };
long get_property_case(long id, void *buf, long n) { (void)id; memcpy(buf, host_wb, n); return 0; }
static int mbox_count = 0;
void gui_mbox_init(int title, int msg, const unsigned int flags, void (*on_select)(unsigned int btn))
{
    (void)title; (void)msg; (void)flags; (void)on_select;
    mbox_count++;
}

// --- Rohbild ----------------------------------------------------------------
static unsigned char *rawbuf;
static int bpp(void) { return CAM_SENSOR_BITS_PER_PIXEL; }

unsigned short get_raw_pixel(unsigned int x, unsigned int y)
{
#if CAM_SENSOR_BITS_PER_PIXEL == 10
    unsigned char *a = rawbuf + y * camera_sensor.raw_rowlen + (x / 8) * 10;
    switch (x % 8) {
        case 0: return ((0x3fc & (((unsigned short)a[1]) << 2)) | (a[0] >> 6));
        case 1: return ((0x3f0 & (((unsigned short)a[0]) << 4)) | (a[3] >> 4));
        case 2: return ((0x3c0 & (((unsigned short)a[3]) << 6)) | (a[2] >> 2));
        case 3: return ((0x300 & (((unsigned short)a[2]) << 8)) | (a[5]));
        case 4: return ((0x3fc & (((unsigned short)a[4]) << 2)) | (a[7] >> 6));
        case 5: return ((0x3f0 & (((unsigned short)a[7]) << 4)) | (a[6] >> 4));
        case 6: return ((0x3c0 & (((unsigned short)a[6]) << 6)) | (a[9] >> 2));
        default: return ((0x300 & (((unsigned short)a[9]) << 8)) | (a[8]));
    }
#else
    unsigned char *a = rawbuf + y * camera_sensor.raw_rowlen + (x / 4) * 6;
    switch (x % 4) {
        case 0: return ((unsigned short)(a[1]) << 4) | (a[0] >> 4);
        case 1: return ((unsigned short)(a[0] & 0x0F) << 8) | (a[3]);
        case 2: return ((unsigned short)(a[2]) << 4) | (a[5] >> 4);
        default: return ((unsigned short)(a[5] & 0x0F) << 8) | (a[4]);
    }
#endif
}
void set_raw_pixel(unsigned int x, unsigned int y, unsigned short v)
{
#if CAM_SENSOR_BITS_PER_PIXEL == 10
    unsigned char *a = rawbuf + y * camera_sensor.raw_rowlen + (x / 8) * 10;
    switch (x % 8) {
        case 0: a[0]=(a[0]&0x3F)|(v<<6); a[1]=v>>2;                 break;
        case 1: a[0]=(a[0]&0xC0)|(v>>4); a[3]=(a[3]&0x0F)|(v<<4);   break;
        case 2: a[2]=(a[2]&0x03)|(v<<2); a[3]=(a[3]&0xF0)|(v>>6);   break;
        case 3: a[2]=(a[2]&0xFC)|(v>>8); a[5]=v;                    break;
        case 4: a[4]=v>>2;               a[7]=(a[7]&0x3F)|(v<<6);   break;
        case 5: a[6]=(a[6]&0x0F)|(v<<4); a[7]=(a[7]&0xC0)|(v>>4);   break;
        case 6: a[6]=(a[6]&0xF0)|(v>>6); a[9]=(a[9]&0x03)|(v<<2);   break;
        case 7: a[8]=v;                  a[9]=(a[9]&0xFC)|(v>>8);   break;
    }
#else
    unsigned char *a = rawbuf + y * camera_sensor.raw_rowlen + (x / 4) * 6;
    switch (x % 4) {
        case 0: a[0] = (a[0]&0x0F) | (unsigned char)(v << 4); a[1] = (unsigned char)(v >> 4); break;
        case 1: a[0] = (a[0]&0xF0) | (unsigned char)(v >> 8); a[3] = (unsigned char)v;        break;
        case 2: a[2] = (unsigned char)(v >> 4); a[5] = (a[5]&0x0F) | (unsigned char)(v << 4); break;
        case 3: a[4] = (unsigned char)v; a[5] = (a[5]&0xF0) | (unsigned char)(v >> 8);        break;
    }
#endif
}
#if cam_CFAPattern == 0x02010100
#define FS_TEST_ROW0 0
#else
#define FS_TEST_ROW0 1
#endif
#define RAW10(x, y) ((int)(get_raw_pixel((x), (y)) >> (CAM_SENSOR_BITS_PER_PIXEL - 10)))

// --- filmsim.c --------------------------------------------------------------
extern void filmsim_log_shot(void);
extern void filmsim_hotpix_hook(char *);
extern void filmsim_double_hook(char *);
extern void filmsim_cnr_hook(char *);
extern void filmsim_dr_curve_hook(int);
extern void filmsim_raw_apply(char *);
extern void filmsim_effects_apply(char *);
#if CAM_SENSOR_BITS_PER_PIXEL == 12
extern void filmsim_curve_apply(char *);
#endif

// Reihenfolge wie raw_process() in core/raw.c
static int dev_shot = 0;                // raw.c: Entwickeln (filmsim_develop_prepare / _end)
static void shoot(char *raw)
{
#ifdef FILMSIM_EXFS
    if (!exfs_render_selected(raw, dev_shot, 0))
#endif
    {
    if (!dev_shot) filmsim_shot_prepare();
    filmsim_prog_step(PS_HOT);
    filmsim_hotpix_hook(raw);
    filmsim_prog_step(PS_DOUBLE);
    filmsim_double_hook(raw);
    filmsim_prog_step(PS_CNR);
    filmsim_cnr_hook(raw);
    filmsim_prog_step(PS_CURVE);
    filmsim_dr_curve_hook(0);
#if CAM_SENSOR_BITS_PER_PIXEL == 12
    filmsim_curve_apply(raw);
#endif
    filmsim_prog_step(PS_MIX);
    filmsim_raw_apply(raw);
    filmsim_prog_step(PS_GLOW);
    filmsim_effects_apply(raw);
    if (dev_shot) filmsim_develop_end(raw);
    else filmsim_shot_end();
    }
    filmsim_log_shot();
    filmsim_dr_curve_hook(1);
#ifdef FILMSIM_EXFS
    filmsim_log_shot_flush();
#endif
}

// Zeit laufen lassen, Hauptschleife nachbilden
void filmsim_batch_key(void);
int filmsim_batch_kbd(void);
static void ticks(int n)
{
    while (n-- > 0) {
        now_ms += 20;
        filmsim_batch_key();            // wie kbd_process: RAW-Serie weiterfuehren
        filmsim_batch_kbd();
        filmsim_tick();
    }
}

static void camera_on(void)
{
    camera_info.state.mode_rec = 1;
    camera_info.state.mode_video = 0;
    camera_info.state.mode_play = 0;
    camera_info.state.gui_mode_none = 1;
    camera_info.state.gui_mode = GUI_MODE_NONE;
    camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_NONE;
    camera_screen.width = 360;
    camera_screen.height = 240;
    canon_menu_active = (int)(long long)&canon_menu_active - 4;
    conf.film_iso_prev = -1;
    ticks(60);
}

static const char *osd_text(void)
{
    // Nach einem Filmwechsel zeigt die Zeile erst den Hinweis, bis Canon die Farben uebernimmt
    camera_info.state.is_shutter_half_press = 1;
    ticks(2);
    camera_info.state.is_shutter_half_press = 0;
    ticks(2);
    drawn[0] = 0;
    now_ms += 8000;                     // Nachlauf der Canon-Symbole abwarten
    filmsim_draw_osd();                 // setzt den Nachlauf beim ersten Aufruf nach dem Start
    now_ms += 8000;
    drawn[0] = 0;
    filmsim_draw_osd();
    return drawn;
}

static void press(long key)
{
    next_key = key;
    drawn[0] = 0;
    cur_mode->kbd_process();
    cur_mode->redraw(0);
}

// --- Szenarien ---------------------------------------------------------------
#define FILMS_TXT "A/CHDK/FILMS/FILMS.TXT"
static const char two_looks[] =
    "Pastel 400 | 11 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | - | 0\n"
    "Noir 3200  |  3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | - | - | 0\n";

// films_load laeuft einmal je Start, darum ein Prozess je Variante (argv[3], test_filmsim.py)
static void scenario_names(const char *variant)
{
    int v = variant ? atoi(variant) : 0;
    switch (v) {
    case 0:     // neue Tabelle, Name gewinnt gegen den Index
        strcpy(conf.film_name, "Noir 3200");
        conf.film_sim = 2;
        camera_on();
        CHECK(strcmp(filmsim_name(), "Noir 3200") == 0, "Name gewinnt: %s", filmsim_name());
        CHECK(conf.film_sim == 20, "Index %d statt 20", conf.film_sim);
        break;
    case 1:     // andere FILMS.TXT, gleicher Look an anderer Stelle
        fs_put(FILMS_TXT, two_looks, sizeof(two_looks) - 1);
        strcpy(conf.film_name, "Noir 3200");
        conf.film_sim = 20;
        camera_on();
        CHECK(conf.film_sim == 2, "FILMS.TXT: Index %d statt 2", conf.film_sim);
        CHECK(strcmp(filmsim_name(), "Noir 3200") == 0, "FILMS.TXT: %s", filmsim_name());
        break;
    case 2:     // Look gibt es nicht mehr: Aus
        fs_put(FILMS_TXT, two_looks, sizeof(two_looks) - 1);
        strcpy(conf.film_name, "Postcard 100");
        conf.film_sim = 2;
        camera_on();
        CHECK(conf.film_sim == 0, "fehlender Look: Index %d", conf.film_sim);
        CHECK(conf.film_name[0] == 0, "fehlender Look: Name %s", conf.film_name);
        break;
    case 3:     // alte Einstellung ohne Namen: Index bleibt, Name wird nachgetragen
        conf.film_name[0] = 0;
        conf.film_sim = 4;
        camera_on();
        CHECK(conf.film_sim == 4, "alt: Index %d", conf.film_sim);
        CHECK(strcmp(conf.film_name, "Journal 400") == 0, "alt: Name %s", conf.film_name);
        break;
    case 4:     // alter Index ausserhalb der Tabelle
        conf.film_name[0] = 0;
        conf.film_sim = 99;
        camera_on();
        CHECK(conf.film_sim == 0, "alt zu gross: Index %d", conf.film_sim);
        break;
    case 5:     // Auswahl merkt sich den Namen, Aus loescht ihn
        camera_on();
        filmsim_set(18);
        CHECK(strcmp(conf.film_name, "Chrome 64") == 0, "set: %s", conf.film_name);
        filmsim_set(0);
        CHECK(conf.film_name[0] == 0, "set Aus: %s", conf.film_name);
        break;
    }
}

static int picker_position(int position, int total)
{
    char buf[16];
    sprintf(buf, "%d/%d\n", position, total);
    return strstr(drawn, buf) != NULL;
}

static void scenario_picker(void)
{
#ifdef FILMSIM_EXFS
    const int total = 27;
#else
    const int total = 26;
#endif
    camera_on();
    filmsim_set(3);
    filmsim_open_menu();
    CHECK(cur_mode != 0, "Auswahl nicht offen");
    cur_mode->redraw(1);
    CHECK(picker_position(3, total), "Position fehlt: [%s]", drawn);
    CHECK(strstr(drawn, lang_str(LANG_EXCCD_PICK_HELP)) != 0, "Tastenhinweis fehlt: [%s]", drawn);
    press(KEY_RIGHT);
    CHECK(picker_position(4, total), "rechts: [%s]", drawn);
    press(KEY_LEFT); press(KEY_LEFT);
    CHECK(picker_position(2, total), "links: [%s]", drawn);
    press(KEY_DOWN);
    CHECK(picker_position(7, total), "runter +5: [%s]", drawn);
    press(KEY_UP); press(KEY_UP);
    CHECK(picker_position(total - 2, total), "hoch -5 ueber den Anfang: [%s]", drawn);
    for (int n = 0; n <= total; n++) press(KEY_RIGHT);
    CHECK(picker_position(total - 2, total), "Runde: [%s]", drawn);
    press(KEY_RIGHT); press(KEY_RIGHT);
    CHECK(picker_position(total, total), "Ende: [%s]", drawn);
    press(KEY_RIGHT);
    CHECK(strchr(drawn, '/') == NULL && strstr(drawn, "Off") != 0, "Off ohne Position: [%s]", drawn);
    press(KEY_SET);
    CHECK(conf.film_sim == 0, "SET: Film %d", conf.film_sim);
    CHECK(alt_exits == 1, "SET schliesst nicht");
}

#ifdef FILMSIM_QUICK_BUTTONS
static void scenario_quick_buttons(void)
{
    static const int enabled[] = { MODE_P, MODE_NIGHT_SCENE, MODE_LANDSCAPE, MODE_PORTRAIT, MODE_AUTO };
    static const int disabled[] = { MODE_TV, MODE_AV, MODE_M, MODE_NIGHT_SNAPSHOT, MODE_INDOOR, MODE_VIDEO_STD };
    camera_on();
    conf.film_quick_button = 1; // Left
    conf.iso_quick_button = 2; // Right

    for (unsigned i = 0; i < sizeof(enabled) / sizeof(enabled[0]); i++) {
        camera_info.state.mode_shooting = enabled[i];
        CHECK(filmsim_quick_mode(KEY_LEFT) == ALT_MODE_ENTER_FILM, "Film shortcut unavailable in mode %d", enabled[i]);
        CHECK(filmsim_quick_mode(KEY_RIGHT) == ALT_MODE_ENTER_ISO, "ISO shortcut unavailable in mode %d", enabled[i]);
        CHECK(filmsim_quick_open(ALT_MODE_ENTER_ISO), "ISO picker failed to open in mode %d", enabled[i]);
        CHECK(filmsim_quick_print_mode(0) == ALT_MODE_NORMAL && filmsim_quick_print_mode(1) == ALT_MODE_NORMAL,
              "Print changed behavior in an arrow shortcut mode %d", enabled[i]);
    }
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(disabled[0]); i++) {
        camera_info.state.mode_shooting = disabled[i];
        CHECK(filmsim_quick_mode(KEY_LEFT) == ALT_MODE_NORMAL && filmsim_quick_mode(KEY_RIGHT) == ALT_MODE_NORMAL,
              "Shortcut intercepted a button in mode %d", disabled[i]);
        CHECK(!filmsim_quick_open(ALT_MODE_ENTER_FILM) && !filmsim_quick_open(ALT_MODE_ENTER_ISO),
              "A queued shortcut opened after switching to mode %d", disabled[i]);
    }

    camera_info.state.mode_shooting = MODE_M;
    CHECK(filmsim_quick_iso_open(), "Manual Quick ISO entry unavailable in M");
    camera_info.state.mode_shooting = MODE_P;
    CHECK(filmsim_quick_open(ALT_MODE_ENTER_ISO), "ISO shortcut did not reactivate in P");
    camera_info.state.mode_shooting = MODE_AV;
    int exits = alt_exits;
    cur_mode->redraw(1);
    CHECK(alt_exits == exits + 1, "ISO picker stayed open after turning the dial");

    camera_info.state.mode_shooting = MODE_P;
    CHECK(filmsim_quick_open(ALT_MODE_ENTER_FILM), "Film shortcut did not reactivate in P");
    camera_info.state.mode_shooting = MODE_AV;
    exits = alt_exits;
    filmsim_tick();
    CHECK(alt_exits == exits + 1, "Film picker stayed open after turning the dial");

    filmsim_open_menu();
    camera_info.state.mode_shooting = MODE_M;
    exits = alt_exits;
    filmsim_tick();
    CHECK(alt_exits == exits, "Mode restriction affected the manual film picker");

    camera_info.state.mode_shooting = MODE_P;
    canon_shoot_menu_active = 1;
    CHECK(filmsim_quick_mode(KEY_LEFT) == ALT_MODE_NORMAL && filmsim_quick_mode(KEY_RIGHT) == ALT_MODE_NORMAL,
          "Shortcuts intercepted Canon's FUNC menu");
    canon_shoot_menu_active = 0;
    cur_mode = 0;

    static const int print_modes[] = { MODE_TV, MODE_AV, MODE_M, MODE_NIGHT_SNAPSHOT, MODE_INDOOR };
    for (unsigned i = 0; i < sizeof(print_modes) / sizeof(print_modes[0]); i++) {
        camera_info.state.mode_shooting = print_modes[i];
        CHECK(filmsim_quick_print_mode(0) == ALT_MODE_ENTER_PRINT_ISO, "Print did not route to ISO in mode %d", print_modes[i]);
        if (!filmsim_quick_open(ALT_MODE_ENTER_PRINT_ISO)) {
            CHECK(0, "Print ISO picker failed to open in mode %d", print_modes[i]);
            return;
        }
        cur_mode->redraw(1);
        CHECK(strstr(drawn, lang_str(LANG_EXCCD_PRINT_FOR_RECIPES)), "Print ISO picker lacks the recipe hint");
        CHECK(filmsim_quick_print_mode(1) == ALT_MODE_ENTER_PRINT_FILM, "Second Print did not route to recipes");
        CHECK(filmsim_quick_open(ALT_MODE_ENTER_PRINT_FILM), "Second Print failed to open the film picker");
        CHECK(filmsim_quick_print_mode(1) == ALT_MODE_NORMAL, "Print reopened a shortcut from the film picker");
        cur_mode = 0;
    }

    camera_info.state.mode_shooting = MODE_TV;
    if (filmsim_quick_open(ALT_MODE_ENTER_PRINT_ISO)) {
        selected_iso_mode = -1;
        clicked_key = KEY_RIGHT;
        cur_mode->kbd_process();
        clicked_key = KEY_SET;
        cur_mode->kbd_process();
        clicked_key = 0;
        CHECK(selected_iso_mode == 1, "Print ISO picker did not select and apply ISO with Right and SET");
    } else {
        CHECK(0, "Could not open Print ISO picker for ISO selection test");
    }

    CHECK(filmsim_quick_iso_open(), "Manual ISO picker failed to reopen");
    CHECK(filmsim_quick_print_mode(1) == ALT_MODE_NORMAL, "Manual ISO picker hijacked Print");
    camera_info.state.mode_rec = 0;
    camera_info.state.mode_play = 1;
    CHECK(filmsim_quick_print_mode(0) == ALT_MODE_NORMAL, "Print shortcut intercepted playback");
}
#endif

// Rohbild: Verlauf, Spalte fuer Spalte verschieden, je Bayer-Farbe plausibel
static void raw_fill_test(void)
{
    int x, y;
    for (y = 0; y < (int)camera_sensor.raw_rows; y++)
        for (x = 0; x < (int)camera_sensor.raw_rowpix; x++) {
            int v = 200 + (x * 500) / camera_sensor.raw_rowpix + ((y * 100) / camera_sensor.raw_rows);
            set_raw_pixel(x, y, (unsigned short)(v << (CAM_SENSOR_BITS_PER_PIXEL - 10)));
        }
}

static void load_card_curve(const char *root, const char *name)
{
    static char buf[8192];
    char path[256], fn[64];
    sprintf(path, "%s/CHDK/CURVES/%s", root, name);
    sprintf(fn, "A/CHDK/CURVES/%s", name);
    int f = _open(path, 0x8000);                // _O_RDONLY | _O_BINARY
    if (f < 0) { printf("Kurve fehlt: %s\n", path); exit(2); }
    int n = _read(f, buf, sizeof(buf));
    _close(f);
    fs_put(fn, buf, n);
}

static void scenario_raw(const char *root)
{
    int rows = camera_sensor.raw_rows, rowpix = camera_sensor.raw_rowpix, rowlen = camera_sensor.raw_rowlen;
    int ax = camera_sensor.active_area.x1, ay = camera_sensor.active_area.y1;
    int jx = ax + camera_sensor.jpeg.x, jy = ay + camera_sensor.jpeg.y;
    int jw = camera_sensor.jpeg.width, jh = camera_sensor.jpeg.height;
    int x, y, st, i;
    static const char *curves[] = { "SUMMER.CV", "CHROME.CV", "BYPASS.CV", "NOIR.CV", "TURQUOI.CV", "INFRA.CV",
                                    "TINTYPE.CV", "CYANO.CV", "INSTANT.CV", "PASTEL.CV", 0 };
    rawbuf = malloc(rows * rowlen + 8);
    unsigned char *orig = malloc(rows * rowlen);
    CHECK(rawbuf && orig, "Speicher");
    CHECK(((rowlen & 1) == 0), "ungerade Zeilenlaenge %d", rowlen);
    CHECK(jw > 0 && jh > 0 && jx + jw <= rowpix && jy + jh <= rows, "Ausschnitt %d %d %d %d im Rohbild %d x %d", jx, jy, jw, jh, rowpix, rows);
    for (i = 0; curves[i]; i++) load_card_curve(root, curves[i]);
    camera_on();

    // Daylight ohne Extras: Rohbild bleibt bitgleich
    raw_fill_test();
    memcpy(orig, rawbuf, rows * rowlen);
    filmsim_set(1);
    shoot((char *)rawbuf);
    CHECK(memcmp(orig, rawbuf, rows * rowlen) == 0, "Daylight veraendert das Rohbild");

    // Filmrahmen: an den Kanten des Ausschnitts, Dunkelbereich unberuehrt.
    // 2 (Negative aus dem Lab-Build) steht vielleicht noch in der CFG und gilt als Thin.
    for (st = 1; st <= 2; st++) {
        // Heller als jede Farbe des Rahmens: Ein gleicher Wert heisst sicher "nicht gefuellt"
        for (y = 0; y < rows; y++)
            for (x = 0; x < rowpix; x++)
                set_raw_pixel(x, y, (unsigned short)((850 + (x * 100) / rowpix) << (CAM_SENSOR_BITS_PER_PIXEL - 10)));
        memcpy(orig, rawbuf, rows * rowlen);
        filmsim_set(0);
        conf.film_frame = st;
        conf.film_grain = 0;
        shoot((char *)rawbuf);
        conf.film_frame = 0;
        // Der Rahmen beginnt bis zu 8 Pixel ausserhalb des Ausschnitts (aktiver Bereich) und
        // ist von dort aus gemessen: Auf dem Bild mindestens so breit wie diese Werte.
        int bh = jh / 48 - 8, bv = jh / 48 - 8, bad = 0, ob = 0, pic = 0;
        int ih = jh / 48 + 8, iv = jh / 48 + 8;
        for (y = 0; y < rows; y++)
            for (x = 0; x < rowpix; x++) {
                int v = get_raw_pixel(x, y), o;
                unsigned char *sv = rawbuf; rawbuf = orig; o = get_raw_pixel(x, y); rawbuf = sv;
                int out = x < ax || y < ay || x >= camera_sensor.active_area.x2 || y >= camera_sensor.active_area.y2;
                if (out && v != o) ob++;
                int ii = x >= jx + ih && x < jx + jw - ih && y >= jy + iv && y < jy + jh - iv;
                int rim = x >= jx && x < jx + jw && y >= jy && y < jy + jh &&
                          !(x >= jx + bh && x < jx + jw - bh && y >= jy + bv && y < jy + jh - bv);
                if (ii && v != o) pic++;        // Bild selbst bleibt
                if (rim && v == o) bad++;       // Rand: schwarz, nie Bild
            }
        CHECK(ob == 0, "Rahmen %d: %d Pixel ausserhalb des aktiven Bereichs veraendert", st, ob);
        CHECK(pic == 0, "Rahmen %d: %d Pixel im Bild veraendert", st, pic);
        CHECK(bad == 0, "Rahmen %d: %d Pixel im Rand sind noch Bild", st, bad);
    }

    // Alle neuen Looks mit allem an: laeuft durch, nichts wird 0, Bild aendert sich
    conf.film_grain = 1;
    conf.film_frame = 1;
    conf.film_datestamp = 1;
    conf.film_vignette = 1;
    conf.film_mist = 1;
    conf.film_halation = 1;
    conf.film_leak = 1;
    conf.film_cnr = 2;
    for (i = 17; i <= 26; i++) {
        raw_fill_test();
        memcpy(orig, rawbuf, rows * rowlen);
        filmsim_set(i);
        shoot((char *)rawbuf);
        int zeros = 0;
        for (y = 0; y < rows; y += 7)
            for (x = 0; x < rowpix; x += 3)
                if (get_raw_pixel(x, y) == 0) zeros++;
        CHECK(zeros == 0, "%s: %d neue Defekt-Markierungen", filmsim_name(), zeros);
        CHECK(memcmp(orig, rawbuf, rows * rowlen) != 0, "%s: Bild unveraendert", filmsim_name());
    }
    conf.film_mist = conf.film_halation = conf.film_leak = conf.film_cnr = conf.film_vignette = conf.film_datestamp = 0;
    conf.film_frame = 0;

    // Korn Auto folgt dem Filmnamen: Noir 3200 staerker als Chrome 64
    double sd[2] = { 0, 0 };
    int idx[2] = { 18, 20 };
    for (st = 0; st < 2; st++) {
        for (y = 0; y < rows; y++)
            for (x = 0; x < rowpix; x++) set_raw_pixel(x, y, (unsigned short)(300 << (CAM_SENSOR_BITS_PER_PIXEL - 10)));
        filmsim_set(idx[st]);
        extern void filmsim_effects_apply(char *);
        filmsim_effects_apply((char *)rawbuf);
        double s = 0; int n = 0;
        for (y = FS_TEST_ROW0; y + 1 < rows; y += 2)
            for (x = 1; x < rowpix; x += 2) {
                int d = RAW10(x, y) - 300;
                s += d * d; n++;
            }
        sd[st] = s / n;
    }
    CHECK(sd[1] > sd[0] * 4, "Korn Auto: Varianz Noir %.1f, Chrome %.1f", sd[1], sd[0]);
    conf.film_grain = 0;
    free(rawbuf);
    free(orig);
}

// --- RAW entwickeln -----------------------------------------------------------
#define DNG_HEAD 1024
static void put16(unsigned char *b, unsigned v) { b[0] = v & 255; b[1] = (v >> 8) & 255; }
static void put32(unsigned char *b, unsigned v) { put16(b, v & 0xffff); put16(b + 2, v >> 16); }
static void put_entry(unsigned char *ifd, int i, unsigned tag, unsigned type, unsigned count, unsigned value)
{
    unsigned char *e = ifd + 2 + i * 12;
    put16(e, tag); put16(e + 2, type); put32(e + 4, count); put32(e + 8, value);
}

// Kopf wie modules/dng.c (nur die Eintraege, die EXCCD liest), Rohdaten am Dateiende
static void make_dng(const char *name, int w, int h, int bits, int wr, int wg, int wb, int iso, const char *date)
{
    int rawsize = camera_sensor.raw_size;
    unsigned char *f = malloc(DNG_HEAD + rawsize);
    memset(f, 0, DNG_HEAD);
    f[0] = 'I'; f[1] = 'I'; put16(f + 2, 42); put32(f + 4, 8);
    put16(f + 8, 4);
    put_entry(f + 8, 0, 0x10E, 2, 1, 0);        // ImageDescription, leer
    put_entry(f + 8, 1, 0x14A, 4, 1, 200);
    put_entry(f + 8, 2, 0x8769, 4, 1, 300);
    put_entry(f + 8, 3, 0xC628, 5, 3, 400);
    put16(f + 200, 3);
    put_entry(f + 200, 0, 0x100, 4, 1, w);
    put_entry(f + 200, 1, 0x101, 4, 1, h);
    put_entry(f + 200, 2, 0x102, 3, 1, bits);
    put16(f + 300, 2);
    put_entry(f + 300, 0, 0x8827, 3, 1, iso);
    put_entry(f + 300, 1, 0x9003, 2, 20, 460);
    put32(f + 400, 1000); put32(f + 404, wr);
    put32(f + 408, 1000); put32(f + 412, wg);
    put32(f + 416, 1000); put32(f + 420, wb);
    strcpy((char *)f + 460, date);
    memcpy(f + DNG_HEAD, rawbuf, rawsize);
    fs_put(name, (char *)f, DNG_HEAD + rawsize);
    free(f);
}

// raw.c: Datei laden, dann die Kette
static void develop_shot(void)
{
    struct stat st;
    filmsim_develop_prepare();
    int fd = open(prep_fn, O_RDONLY, 0);
    CHECK(fd >= 0, "Entwickeln ohne Datei");
    stat(prep_fn, &st);
    lseek(fd, st.st_size - camera_sensor.raw_size, 0);
    read(fd, rawbuf, camera_sensor.raw_size);
    close(fd);
    prep_fn[0] = 0;
    camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_PROCESSING;
    filmsim_develop_begin((char *)rawbuf);
    dev_shot = 1;
    shoot((char *)rawbuf);
    dev_shot = 0;
}

static void fill_flat(int v)
{
    int x, y;
    for (y = 0; y < (int)camera_sensor.raw_rows; y++)
        for (x = 0; x < (int)camera_sensor.raw_rowpix; x++)
            set_raw_pixel(x, y, (unsigned short)(v << (CAM_SENSOR_BITS_PER_PIXEL - 10)));
}

// Mischmatrizen: Grau unter Kunstlicht (Weissabgleich der Aufnahme) bleibt grau
static void film_by_name(const char *n);
static void scenario_mixwb(void)
{
    int rows = camera_sensor.raw_rows, rowpix = camera_sensor.raw_rowpix, r0 = FS_TEST_ROW0, x, y;
    int cx = (rowpix / 2) & ~7, cy = ((rows / 2) & ~1) + r0;
    static const char *looks[] = { "Violet 400", "Turquoise 400", "Infra 200", "Concrete 100" };
    unsigned k;
    rawbuf = malloc(rows * camera_sensor.raw_rowlen + 8);
    camera_on();
    // Gluehlampe: R * 1,4, B * 3,0 machen Grau (G = 400)
    host_wb[0] = 1000; host_wb[1] = 1400; host_wb[2] = 3000;
    int gv = 400, rv = 31 + (369 * 1000 + 700) / 1400, bv = 31 + (369 + 1) / 3;
    for (k = 0; k < sizeof(looks) / sizeof(looks[0]); k++) {
        film_by_name(looks[k]);
        for (y = 0; y < rows; y++)
            for (x = 0; x < rowpix; x++) {
                int rrow = ((y - r0) & 1) == 0, v = rrow ? ((x & 1) ? gv : rv) : ((x & 1) ? bv : gv);
                set_raw_pixel(x, y, (unsigned short)(v << (CAM_SENSOR_BITS_PER_PIXEL - 10)));
            }
        filmsim_raw_apply((char *)rawbuf);
        int r = (RAW10(cx, cy) - 31) * 1400 / 1000, g = RAW10(cx + 1, cy) - 31, b = (RAW10(cx + 1, cy + 1) - 31) * 3;
        CHECK(r - g < 8 && g - r < 8 && b - g < 8 && g - b < 8, "%s Kunstlicht: R %d G %d B %d", looks[k], r, g, b);
    }
    host_wb[0] = host_wb[1] = host_wb[2] = 1000;
}

static void scenario_develop(void)
{
    const char *good = "A/DCIM/100CANON/CRW_0012.DNG";
    int rows = camera_sensor.raw_rows, rowpix = camera_sensor.raw_rowpix, rowlen = camera_sensor.raw_rowlen;
    int bits = camera_sensor.bits_per_pixel, r0 = FS_TEST_ROW0, saves, m;
    int cx = (rowpix / 2) & ~7, cy = (rows / 2) & ~1;    // Mitte, gerade
    rawbuf = malloc(rows * rowlen + 8);
    unsigned char *a = malloc(rows * rowlen);
    CHECK(rawbuf && a, "Speicher");
    camera_on();
    fill_flat(400);
    make_dng(good, rowpix, rows, bits, 2000, 1000, 1500, 100, "2025:03:04 10:11:12");

    // Fremde Dateien werden abgelehnt und nichts wird scharf gestellt
    make_dng("A/DCIM/W.DNG", rowpix + 8, rows, bits, 2000, 1000, 1500, 100, "2025:03:04 10:11:12");
    make_dng("A/DCIM/B.DNG", rowpix, rows, bits == 10 ? 12 : 10, 2000, 1000, 1500, 100, "2025:03:04 10:11:12");
    fs_put("A/DCIM/SMALL.JPG", "xx", 2);
    static const char *bad[] = { "A/DCIM/W.DNG", "A/DCIM/B.DNG", "A/DCIM/SMALL.JPG", "A/DCIM/FEHLT.DNG", 0 };
    for (m = 0; bad[m]; m++) {
        int c = mbox_count;
        filmsim_develop_select(bad[m]);
        CHECK(mbox_count == c + 1, "%s: keine Meldung", bad[m]);
        CHECK(!filmsim_develop_armed() && !prep_fn[0], "%s: scharf gestellt", bad[m]);
    }
    filmsim_develop_select(0);          // Dateiauswahl abgebrochen
    CHECK(!filmsim_develop_armed(), "Abbruch stellt scharf");

    // Look 5 eingelegt: der Film kommt danach zurueck
    filmsim_set(5);
    saves = conf_saves;
    cur_mode = 0;
    filmsim_develop_select(good);
    CHECK(filmsim_develop_armed() && strcmp(prep_fn, good) == 0, "nicht scharf: [%s]", prep_fn);
    CHECK(cur_mode != 0, "Filmauswahl nicht offen");
    filmsim_open_menu();
    for (m = 1; m < 5; m++) press(KEY_LEFT);    // Look 1, Daylight: keine Kurve, keine Mischung
    press(KEY_SET);
    CHECK(conf.film_sim == 1, "Look: %d", conf.film_sim);
    CHECK(conf_saves == saves, "Probierlook gespeichert");
    CHECK(cur_mode && cur_mode->redraw == menu_redraw && menu_open && menu_open->title == LANG_EXCCD_DEV_FX, "Effekt-Menue nicht offen");
    {   // Ausloeser im Effekt-Menue: Menue zu, scharf bleibt es
        int pc = alt_exits;
        key_held = KEY_SHOOT_HALF;
        cur_mode->kbd_process();
        key_held = 0;
        CHECK(alt_exits == pc + 1 && filmsim_develop_armed(), "Ausloeser schliesst das Effekt-Menue nicht");
    }
    CHECK(strstr(osd_text(), "Develop CRW_0012.DNG: Daylight 100") != 0, "Statuszeile: [%s]", drawn);

    // Hilfsschuss in anderem Licht: Rohdaten so umrechnen, dass der WB der Aufnahme herauskommt
    host_wb[0] = 1000; host_wb[1] = 1000; host_wb[2] = 3000;
    fill_flat(10);                      // Sensordaten des Hilfsschusses, muessen verschwinden
    int mark = card[fs_find("A/CHDK/FILMLOG.CSV")].size;
    develop_shot();
    CHECK(!filmsim_develop_armed(), "bleibt scharf");
    CHECK(RAW10(cx, cy + r0) == 31 + 369 * 2, "R: %d", RAW10(cx, cy + r0));
    CHECK(RAW10(cx + 1, cy + r0) == 400, "G: %d", RAW10(cx + 1, cy + r0));
    CHECK(RAW10(cx + 1, cy + r0 + 1) == 31 + 185, "B: %d", RAW10(cx + 1, cy + r0 + 1));
    {
        int li = fs_find("A/CHDK/FILMLOG.CSV");
        card[li].data[card[li].size] = 0;
        CHECK(strstr(card[li].data + mark, ";Daylight 100;CRW_0012.DNG") != 0, "Logbuch: [%s]", card[li].data + mark);
    }

    // Film erst zurueck, wenn Canon fertig ist
    ticks(60);
    CHECK(conf.film_sim == 1, "Ruecknahme waehrend der Verarbeitung");
    camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_NONE;
    ticks(60);
    CHECK(conf.film_sim == 5, "eingelegter Film nicht zurueck: %d", conf.film_sim);
    cur_mode = 0;

    // Normales Foto danach: keine dritte Spalte, Ueberschreiben klappt
    host_wb[0] = host_wb[1] = host_wb[2] = 1000;
    filmsim_set(5);
    mark = card[fs_find("A/CHDK/FILMLOG.CSV")].size;
    filmsim_log_shot();
    ticks(30);
    {
        int li = fs_find("A/CHDK/FILMLOG.CSV");
        card[li].data[card[li].size] = 0;
        CHECK(strstr(card[li].data + mark, ".DNG") == 0, "normales Foto mit Quelle: [%s]", card[li].data + mark);
    }

    // Abbrechen: Look und Zustand zurueck
    filmsim_develop_select(good);
    filmsim_open_menu(); press(KEY_RIGHT); press(KEY_SET);
    CHECK(conf.film_sim == 6, "Abbruch-Look: %d", conf.film_sim);
    filmsim_develop_cancel();
    CHECK(!filmsim_develop_armed() && !prep_fn[0], "Abbruch: noch scharf");
    ticks(60);
    CHECK(conf.film_sim == 5, "Abbruch: Film %d", conf.film_sim);
    cur_mode = 0;

    // Sofort neu waehlen, solange die Ruecknahme offen ist: Der eingelegte Film bleibt gemerkt
    filmsim_develop_select(good);
    filmsim_open_menu(); press(KEY_RIGHT); press(KEY_SET);
    filmsim_develop_cancel();
    camera_info.state.gui_mode_none = 0;
    ticks(60);
    filmsim_develop_select(good);
    camera_info.state.gui_mode_none = 1;
    filmsim_develop_cancel();
    ticks(60);
    CHECK(conf.film_sim == 5, "neu waehlen: Film %d", conf.film_sim);
    cur_mode = 0;

    // Datumsstempel: Tag der Aufnahme
    filmsim_set(1);
    conf.film_datestamp = 1;
    fill_flat(400);
    make_dng(good, rowpix, rows, bits, 1000, 1000, 1000, 100, "2025:03:04 10:11:12");
    filmsim_develop_select(good);
    filmsim_open_menu(); press(KEY_SET);
    develop_shot();
    camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_NONE;
    ticks(60);
    memcpy(a, rawbuf, rows * rowlen);
    fill_flat(400);
    shoot((char *)rawbuf);
    ticks(60);
    CHECK(memcmp(a, rawbuf, rows * rowlen) != 0, "Datum: heute und Aufnahmetag gleich");
    fill_flat(400);
    tm_now.tm_year = 125; tm_now.tm_mon = 2; tm_now.tm_mday = 4;
    shoot((char *)rawbuf);
    ticks(60);
    tm_now.tm_year = 126; tm_now.tm_mon = 10; tm_now.tm_mday = 7;
    CHECK(memcmp(a, rawbuf, rows * rowlen) == 0, "Datum: nicht der Aufnahmetag");
    conf.film_datestamp = 0;

    // Effekte nur fuer die Entwicklung: im RAW-Hook aktiv, danach wieder die der Optionen
    {
        filmsim_set(1);
        conf.film_grain = 0; conf.film_vignette = 2;
        make_dng(good, rowpix, rows, bits, 1000, 1000, 1000, 100, "2025:03:04 10:11:12");
        filmsim_develop_select(good);
        filmsim_open_menu(); press(KEY_SET);
        CHECK(menu_open && menu_open->title == LANG_EXCCD_DEV_FX, "Effekt-Menue nicht offen");
        CHECK(strcmp(menu_enum(LANG_EXCCD_GRAIN, 0), lang_str(LANG_EXCCD_OFF)) == 0, "Korn startet nicht mit den Optionen");
        menu_enum(LANG_EXCCD_GRAIN, 4); // Grob
        menu_enum(LANG_EXCCD_VIGNETTE, 1);  // Stark -> Aus
        CHECK(menu_item(LANG_EXCCD_DOUBLE) == 0, "Doppelbelichtung im Effekt-Menue");
        CHECK(conf.film_grain == 0 && conf.film_vignette == 2, "Menue aendert die Optionen");
        CHECK(strstr(osd_text(), "| FX") != 0, "Statuszeile ohne FX: [%s]", drawn);
        int pc = alt_exits;
        const CMenuItem *done = menu_item(LANG_EXCCD_DEV_FX_DONE);
        CHECK(done != 0, "Eintrag Fertig fehlt");
        if (done) ((void (*)(int))done->value)(done->arg);
        CHECK(alt_exits == pc + 1, "Fertig schliesst nicht");
        drawn[0] = 0;
        filmsim_develop_prepare();
        CHECK(conf.film_grain == 4 && conf.film_vignette == 0, "Hook ohne Entwicklungs-Effekte: %d %d",
              conf.film_grain, conf.film_vignette);
        CHECK(strstr(drawn, "Loading RAW") != 0, "Balken beim Laden: [%s]", drawn);
        drawn[0] = 0;
        filmsim_prog_step(7);           // Korn
        CHECK(strstr(drawn, "%") != 0, "Prozent fehlen: [%s]", drawn);
        CHECK(strstr(drawn, "Developing CRW_0012.DNG") != 0 && strstr(drawn, "Grain") != 0, "Balken: [%s]", drawn);
        filmsim_develop_end(0);
        drawn[0] = 0;
        filmsim_prog_step(7);
        CHECK(drawn[0] == 0, "Balken nach dem Ende");
        CHECK(conf.film_grain == 0 && conf.film_vignette == 2, "Optionen nicht zurueck: %d %d",
              conf.film_grain, conf.film_vignette);
        filmsim_develop_cancel();
        ticks(60);
        conf.film_vignette = 0;
        cur_mode = 0;
    }

    // Dritter Schritt: Belichtung und Farbtemperatur nur fuer die Entwicklung
    {
        filmsim_set(1);
        host_wb[0] = host_wb[1] = host_wb[2] = 1000;
        fill_flat(400);
        make_dng(good, rowpix, rows, bits, 1000, 1000, 1000, 100, "2025:03:04 10:11:12");
        filmsim_develop_select(good);
        filmsim_open_menu(); press(KEY_SET);
        const CMenuItem *next = menu_item(LANG_EXCCD_DEV_NEXT);
        CHECK(next != 0 && (next->type & MENUITEM_MASK) == MENUITEM_SUBMENU, "Weiter fehlt");
        if (next) {
            CMenu *fx = menu_open;
            menu_open = (CMenu *)next->value;
            CHECK(strcmp(menu_enum(LANG_EXCCD_DEV_EV, -3), "-1 EV") == 0, "EV: [%s]", menu_enum(LANG_EXCCD_DEV_EV, 0));
            CHECK(strcmp(menu_enum(LANG_EXCCD_DEV_EV, 1), "-2/3 EV") == 0, "EV: [%s]", menu_enum(LANG_EXCCD_DEV_EV, 0));
            menu_enum(LANG_EXCCD_DEV_EV, -1);
            CHECK(strcmp(menu_enum(LANG_EXCCD_DEV_TEMP, 1), "warm +1") == 0, "Temperatur: [%s]", menu_enum(LANG_EXCCD_DEV_TEMP, 0));
            CHECK(strcmp(menu_enum(LANG_EXCCD_DEV_TINT, -1), "magenta -1") == 0, "Toenung: [%s]", menu_enum(LANG_EXCCD_DEV_TINT, 0));
            menu_enum(LANG_EXCCD_DEV_TINT, 1);
            CHECK(strcmp(menu_enum(LANG_EXCCD_DYNAMIC, 0), "Auto") == 0, "Dynamik: [%s]", menu_enum(LANG_EXCCD_DYNAMIC, 0));
            CHECK(menu_item(LANG_EXCCD_DEV_FX_DONE) != 0, "Fertig fehlt im dritten Schritt");
            menu_open = fx;
        }
        // 360 px = 45 Zeichen: was nicht passt, faellt ganz weg; breiter Bildschirm zeigt alles
        CHECK(strstr(osd_text(), "| -1 EV") != 0 && strlen(drawn) <= 46 && !strstr(drawn, "warm"), "Statuszeile: [%s]", drawn);
        camera_screen.width = 720;
        CHECK(strstr(osd_text(), "| -1 EV | warm +1") != 0, "Statuszeile breit: [%s]", drawn);
        camera_screen.width = 360;
        develop_shot();
        CHECK(RAW10(cx + 1, cy + r0) == 216, "EV G: %d", RAW10(cx + 1, cy + r0));
        // Belichtung vor der Kette (185 ueber Schwarz), Temperatur danach: R * 1,07, B / 1,07
        CHECK(RAW10(cx, cy + r0) == 31 + ((185 * 4383 + 2048) >> 12), "EV R: %d", RAW10(cx, cy + r0));
        CHECK(RAW10(cx + 1, cy + r0 + 1) == 31 + ((185 * 3827 + 2048) >> 12), "EV B: %d", RAW10(cx + 1, cy + r0 + 1));
        camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_NONE;
        ticks(60);
        // Toenung gruener: G * 1,05, R und B / Wurzel(1,05)
        fill_flat(400);
        make_dng(good, rowpix, rows, bits, 1000, 1000, 1000, 100, "2025:03:04 10:11:12");
        filmsim_develop_select(good);
        filmsim_open_menu(); press(KEY_SET);
        if (next) {
            CMenu *fx = menu_open;
            menu_open = (CMenu *)next->value;
            menu_enum(LANG_EXCCD_DEV_TINT, 1);
            menu_open = fx;
        }
        CHECK(strstr(osd_text(), "| green +1") != 0, "Statuszeile Toenung: [%s]", drawn);
        develop_shot();
        CHECK(RAW10(cx + 1, cy + r0) == 418, "Toenung G: %d", RAW10(cx + 1, cy + r0));
        CHECK(RAW10(cx, cy + r0) == 391 && RAW10(cx + 1, cy + r0 + 1) == 391, "Toenung R/B: %d %d",
              RAW10(cx, cy + r0), RAW10(cx + 1, cy + r0 + 1));
        camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_NONE;
        ticks(60);
        // naechste Entwicklung beginnt wieder neutral
        filmsim_develop_select(good);
        filmsim_open_menu(); press(KEY_SET);
        if (next) {
            CMenu *fx = menu_open;
            menu_open = (CMenu *)next->value;
            CHECK(strcmp(menu_enum(LANG_EXCCD_DEV_EV, 0), "0 EV") == 0 && strcmp(menu_enum(LANG_EXCCD_DEV_TEMP, 0), "0") == 0
                  && strcmp(menu_enum(LANG_EXCCD_DEV_TINT, 0), "0") == 0 && strcmp(menu_enum(LANG_EXCCD_DYNAMIC, 0), "Auto") == 0,
                  "nicht zurueckgesetzt");
            menu_open = fx;
        }
        filmsim_develop_cancel();
        ticks(60);
        cur_mode = 0;
    }

    // Halation: Der Glow kommt beim Entwickeln in derselben Farbe heraus wie beim echten Foto, auch
    // wenn der Hilfsschuss unter anderem Licht entsteht (Weissabgleich erst nach den Effekten)
    {
        int x, y, sq = 64, gx = cx + sq / 2 + 8, gy = cy + r0;
        unsigned char *norm = malloc(rows * rowlen);
        filmsim_set(1);
        conf.film_halation = 1;
        host_wb[0] = host_wb[1] = host_wb[2] = 1000;
        fill_flat(200);
        for (y = cy - sq / 2; y < cy + sq / 2; y++)
            for (x = cx - sq / 2; x < cx + sq / 2; x++)
                set_raw_pixel(x, y, (unsigned short)(1023 << (CAM_SENSOR_BITS_PER_PIXEL - 10)));
        memcpy(norm, rawbuf, rows * rowlen);
        make_dng(good, rowpix, rows, bits, 1000, 1000, 1000, 100, "2025:03:04 10:11:12");
        shoot((char *)rawbuf);                  // echtes Foto mit Halation
        ticks(60);
        int nR = RAW10(gx, gy), nG = RAW10(gx + 1, gy);
        CHECK(nR > 200 + 20 && nR - 200 > 3 * (nG - 200), "kein roter Glow im Foto: R %d G %d", nR, nG);
        memcpy(a, rawbuf, rows * rowlen);
        memcpy(rawbuf, norm, rows * rowlen);
        // Hilfsschuss unter warmem Licht: Canon gibt Rot nur halb so stark aus
        host_wb[1] = 500;
        filmsim_develop_select(good);
        filmsim_open_menu(); press(KEY_SET);
        develop_shot();
        int dR = RAW10(gx, gy), dG = RAW10(gx + 1, gy);
        int want = 31 + (((nR - 31) * 8192 + 2048) >> 12);
        CHECK(abs(dR - (want > 1023 ? 1023 : want)) <= 2 && abs(dG - nG) <= 1, "Glow beim Entwickeln: R %d (soll %d) G %d (soll %d)",
              dR, want, dG, nG);
        camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_NONE;
        ticks(60);
        host_wb[1] = 1000;
        conf.film_halation = 0;
        cur_mode = 0;
        free(norm);
    }

    // Farbrauschen Auto richtet sich nach der ISO der Aufnahme, nicht nach dem Hilfsschuss
    {
        int x, y;
        filmsim_cnr_enum(1, 0);
        for (y = 0; y < rows; y++)
            for (x = 0; x < rowpix; x++)
                set_raw_pixel(x, y, (unsigned short)((300 + (x * 31 + y * 17) % 64) << (CAM_SENSOR_BITS_PER_PIXEL - 10)));
        memcpy(a, rawbuf, rows * rowlen);
        shoot((char *)rawbuf);          // ISO 100 laut Kamera: kein Filter
        CHECK(memcmp(a, rawbuf, rows * rowlen) == 0, "Auto bei ISO 100 filtert");
        ticks(60);
        memcpy(rawbuf, a, rows * rowlen);
        make_dng(good, rowpix, rows, bits, 1000, 1000, 1000, 800, "2025:03:04 10:11:12");
        filmsim_develop_select(good);
        filmsim_open_menu(); press(KEY_SET);
        develop_shot();
        camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_NONE;
        CHECK(memcmp(a, rawbuf, rows * rowlen) != 0, "Auto nimmt nicht die ISO der Aufnahme");
        ticks(60);
        filmsim_cnr_enum(-1, 0);
        cur_mode = 0;
    }

    // Dynamik+: Stufe im Kopf vermerken, Groesse und Rohdaten bleiben, Entwicklung nimmt ihre Kurve
    {
        int ci, size;
        fill_flat(400);
        make_dng(good, rowpix, rows, bits, 1000, 1000, 1000, 100, "2025:03:04 10:11:12");
        ci = fs_find(good);
        size = card[ci].size;
        memcpy(a, card[ci].data + DNG_HEAD, rows * rowlen);
        CHECK(filmsim_dng_mark(good) == -1, "Vermerk ohne Dynamik+");
        conf.film_dr = 2;
        CHECK(filmsim_dr_tv96_shift(1) == 192, "Dynamik+");
        CHECK(filmsim_dng_mark(good) >= 0, "Vermerk fehlt");
        conf.film_dr = 0;
        filmsim_dr_curve_hook(1);       // Normale Aufnahme zu Ende, Stufe wieder unbekannt
        CHECK(card[ci].size == size, "Groesse %d statt %d", card[ci].size, size);
        CHECK(memcmp(a, card[ci].data + DNG_HEAD, rows * rowlen) == 0, "Rohdaten veraendert");
        filmsim_set(0);
        filmsim_develop_select(good);
        filmsim_open_menu(); press(KEY_SET);
        filmsim_develop_begin((char *)rawbuf);
        filmsim_dr_curve_hook(0);
#if CAM_SENSOR_BITS_PER_PIXEL == 12     // bei 10 Bit laedt curves.flt, das gibt es am PC nicht
        CHECK(strstr(conf.curve_file, "DR2.CV") != 0, "Kurve: [%s]", conf.curve_file);
#endif
        filmsim_log_shot();
        filmsim_dr_curve_hook(1);
        ticks(60);
        // Dynamik+ beim Entwickeln fest gewaehlt: gilt statt des Vermerks, die Optionen bleiben
        filmsim_develop_select(good);
        filmsim_open_menu(); press(KEY_SET);
        {
            const CMenuItem *nx = menu_item(LANG_EXCCD_DEV_NEXT);
            CMenu *fx = menu_open;
            if (nx) {
                menu_open = (CMenu *)nx->value;
                CHECK(strcmp(menu_enum(LANG_EXCCD_DYNAMIC, 2), "+1") == 0, "Dynamik +1: [%s]", menu_enum(LANG_EXCCD_DYNAMIC, 0));
            }
            menu_open = fx;
        }
        CHECK(strstr(osd_text(), "| DR+1") != 0, "Statuszeile Dynamik: [%s]", drawn);
        filmsim_develop_begin((char *)rawbuf);
        filmsim_dr_curve_hook(0);
#if CAM_SENSOR_BITS_PER_PIXEL == 12
        CHECK(strstr(conf.curve_file, "DR1.CV") != 0, "Kurve gewaehlt: [%s]", conf.curve_file);
#endif
        filmsim_log_shot();
        filmsim_dr_curve_hook(1);
        CHECK(conf.film_dr == 0, "Einstellung geaendert");
        ticks(60);
    }
    free(a);
    free(rawbuf);
}

// Vorschaubild in der Dateiauswahl (filmsim_dng_thumb). Ohne Datei: kuenstliche DNG mit
// Verlauf. Mit Datei (argv[3]) wird zusaetzlich das Ergebnis als PGM geschrieben (argv[4]).
extern int filmsim_dng_thumb(const char *fn, int x, int y, int w, int h, int (*still_wanted)(void));
static int fb_luma(int col)
{
#ifdef FILMSIM_THUMB_GREYS
    static const unsigned char g[] = FILMSIM_THUMB_GREYS;
    int i;
    for (i = 0; i < (int)sizeof(g); i += 2) if (g[i] == col) return g[i + 1] * 255 / g[sizeof(g) - 1];
    return -1;
#else
    return col == COLOR_BLACK ? 0 : col == COLOR_WHITE ? 255 : col == COLOR_GREY ? 128 : -1;
#endif
}
static int stop_calls;
static int stop_after_one(void) { return ++stop_calls < 2; }
static void scenario_thumb(const char *dng, const char *pgm)
{
    int x, y, n = 0, bad = 0;
    memset(fb, 0xff, sizeof(fb));
    if (!dng) {
        // 128 x 96, waagrechter Verlauf schwarz -> weiss
        int w = 128, h = 96, off = 512, len = off + w * h * 3;
        unsigned char *f = malloc(len);
        memset(f, 0, len);
        f[0] = 'I'; f[1] = 'I'; f[2] = 42; put32(f + 4, 8);
        put16(f + 8, 6);
        put_entry(f + 8, 0, 0x100, 4, 1, w);
        put_entry(f + 8, 1, 0x101, 4, 1, h);
        put_entry(f + 8, 2, 0x103, 3, 1, 1);
        put_entry(f + 8, 3, 0x111, 4, 1, off);
        put_entry(f + 8, 4, 0x115, 3, 1, 3);
        put_entry(f + 8, 5, 0x117, 4, 1, w * h * 3);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                memset(f + off + (y * w + x) * 3, x * 2, 3);
        fs_put("A/T.DNG", (char *)f, len);
        CHECK(filmsim_dng_thumb("A/T.DNG", 10, 20, 228, 171, 0), "Vorschaubild fehlt");
        // 228 x 171 im Kasten, nichts ausserhalb, unbekannte Farben gibt es nicht
        for (y = 0; y < FB_H; y++)
            for (x = 0; x < FB_W; x++) {
                if (fb[y][x] < 0) continue;
                n++;
                if (x < 10 || x >= 238 || y < 20 || y >= 191 || fb_luma(fb[y][x]) < 0) bad++;
            }
        CHECK(n == 228 * 171 && bad == 0, "gezeichnet %d, falsch %d", n, bad);
        // Verlauf: links dunkel, rechts hell, im Mittel gleichmaessig steigend
        int col[4] = { 0, 0, 0, 0 };
        for (y = 20; y < 191; y++)
            for (x = 0; x < 228; x++) col[x * 4 / 228] += fb_luma(fb[y][10 + x]);
        for (x = 0; x < 4; x++) col[x] /= 171 * 57;
#ifdef FILMSIM_THUMB_GREYS             // ohne Tabelle: CHDK-Farben, am PC nicht unterscheidbar
        CHECK(col[0] < 50 && col[3] > 200 && col[1] < col[2], "Verlauf %d %d %d %d", col[0], col[1], col[2], col[3]);
#endif
        // Ohne Vorschaubild (z. B. reines CHDK-RAW): nichts zeichnen
        put16(f + 8, 3);
        fs_put("A/T.DNG", (char *)f, len);
        memset(fb, 0xff, sizeof(fb));
        CHECK(!filmsim_dng_thumb("A/T.DNG", 10, 20, 228, 171, 0) && fb[100][100] < 0, "ohne Vorschaubild gezeichnet");
        CHECK(!filmsim_dng_thumb("A/FEHLT.DNG", 10, 20, 228, 171, 0), "fehlende Datei");
        free(f);

        // Schaerferes Bild aus den Rohdaten: DNG dieser Kamera mit waagrechtem Verlauf im
        // JPEG-Ausschnitt, Bytepaare vertauscht wie in modules/dng.c
        {
            int rows = camera_sensor.raw_rows, rowpix = camera_sensor.raw_rowpix, rowlen = camera_sensor.raw_rowlen;
            int jx = camera_sensor.active_area.x1 + camera_sensor.jpeg.x, jw = camera_sensor.jpeg.width;
            rawbuf = malloc(rows * rowlen + 8);
            for (y = 0; y < rows; y++)
                for (x = 0; x < rowpix; x++) {
                    int v = x < jx ? 0 : x >= jx + jw ? 1023 : 31 + (x - jx) * 900 / jw;
                    set_raw_pixel(x, y, (unsigned short)(v << (CAM_SENSOR_BITS_PER_PIXEL - 10)));
                }
            make_dng("A/R.DNG", rowpix, rows, camera_sensor.bits_per_pixel, 1000, 1000, 1000, 100, "2025:03:04 10:11:12");
            int ci = fs_find("A/R.DNG");
            unsigned char *d = (unsigned char *)card[ci].data + card[ci].size - rows * rowlen;
            for (x = 0; x < rows * rowlen; x += 2) { unsigned char t = d[x]; d[x] = d[x + 1]; d[x + 1] = t; }
            memset(fb, 0xff, sizeof(fb));
            CHECK(filmsim_dng_thumb("A/R.DNG", 10, 20, 228, 171, 0), "Rohdaten-Vorschau fehlt");
            int col[4] = { 0, 0, 0, 0 }, cnt[4] = { 0, 0, 0, 0 }, up = 0, nn = 0;
            for (y = 0; y < FB_H; y++)
                for (x = 0; x < FB_W; x++)
                    if (fb[y][x] >= 0) { nn++; col[(x - 10) * 4 / 228] += fb_luma(fb[y][x]); cnt[(x - 10) * 4 / 228]++; }
            for (x = 0; x < 4; x++) col[x] = cnt[x] ? col[x] / cnt[x] : -1;
            for (x = 1; x < 4; x++) up += col[x] > col[x - 1];
            CHECK(nn > 228 * 160 && nn <= 228 * 171, "Rohdaten-Vorschau: %d Pixel", nn);
#ifdef FILMSIM_THUMB_GREYS
            CHECK(up == 3 && col[0] < 80 && col[3] > 180, "Rohdaten-Verlauf %d %d %d %d", col[0], col[1], col[2], col[3]);
#endif
            // Abbrechen, sobald der Cursor weiter ist
            memset(fb, 0xff, sizeof(fb));
            stop_calls = 0;
            filmsim_dng_thumb("A/R.DNG", 10, 20, 228, 171, stop_after_one);
            CHECK(stop_calls == 2, "Abbruch: %d Abfragen", stop_calls);
            free(rawbuf);
        }
        return;
    }
    static char big[24 << 20];
    int fh = _open(dng, 0x8000);
    if (fh < 0) { printf("Datei fehlt: %s\n", dng); exit(2); }
    int len = _read(fh, big, sizeof(big));
    _close(fh);
    fs_put("A/T.DNG", big, len);
    CHECK(filmsim_dng_thumb("A/T.DNG", 0, 0, 228, 171, 0), "Vorschaubild fehlt");
    if (pgm) {
        static char img[16 + 228 * 171];
        int hl = sprintf(img, "P5 228 171 255\n");
        for (y = 0; y < 171; y++)
            for (x = 0; x < 228; x++) img[hl + y * 228 + x] = fb[y][x] < 0 ? 0 : fb_luma(fb[y][x]);
        int o = _open(pgm, 0x8000 | 0x0001 | 0x0100 | 0x0200, 0600);   // binaer, schreiben, anlegen, leeren
        _write(o, img, hl + 228 * 171);
        _close(o);
    }
}

// Rezepte und Weissabgleich: speichern, laden, Aenderungs-*, WB-Shift in der Kurve (12 Bit)
extern const char* filmsim_wb_enum(int change, int arg);
extern const char* filmsim_wb_r_enum(int change, int arg);
extern const char* filmsim_wb_b_enum(int change, int arg);
extern const char* filmsim_wb_shift_enum(int change, int arg);
extern const char* filmsim_bwfilter_enum(int change, int arg);
extern const char* filmsim_recipe_enum(int change, int arg);
extern void filmsim_recipe_save_menu(int arg);
extern char filmsim_slot_text[4][32];
extern char filmsim_look_text[40];
CMenu filmsim_save_menu;
extern void filmsim_recipe_save(int arg);

static void film_by_name(const char *n)
{
    int i;
    for (i = 0; i < 40; i++) {
        filmsim_set(i);
        if (strcmp(filmsim_name(), n) == 0) return;
    }
    CHECK(0, "Film %s fehlt", n);
}

static const char *card_text(const char *n)
{
    int i = fs_find(n);
    if (i < 0) return "";
    card[i].data[card[i].size] = 0;
    return card[i].data;
}

#ifdef FILMSIM_EXFS
static void scenario_exfs(void)
{
    unsigned size = camera_sensor.raw_size;
    rawbuf = malloc(size);
    unsigned char *input = malloc(size), *reference = malloc(size);
    CHECK(rawbuf && input && reference, "RAW buffer allocation failed");
    if (!rawbuf || !input || !reference) return;
    camera_on();
    raw_fill_test();
    memcpy(input, rawbuf, size);

    static const char *looks[] = {"Daylight 100", "Chrome 64", "Violet 400", "Noir 3200"};
    for (unsigned i = 0; i < sizeof(looks) / sizeof(looks[0]); i++) {
        conf.film_exfs_11 = 0;
        film_by_name(looks[i]);
        conf.film_frame = i != 0;
        conf.film_grain = i ? 2 : 0;
        conf.film_mist = i == 3;
        conf.film_vignette = i == 3;
        conf.film_bwfilter = i == 3 ? 2 : 0;

        conf.film_exfs = 0;
        memcpy(rawbuf, input, size);
        shoot((char *)rawbuf);
        memcpy(reference, rawbuf, size);

        conf.film_exfs = 1;
        memcpy(rawbuf, input, size);
        shoot((char *)rawbuf);
        CHECK(memcmp(reference, rawbuf, size) == 0, "EXFS render differs for %s", looks[i]);

        conf.film_exfs = 0;
        conf.film_exfs_11 = 1;
        memcpy(rawbuf, input, size);
        shoot((char *)rawbuf);
        CHECK(memcmp(reference, rawbuf, size) == 0, "EXFS 1.1 render differs for %s", looks[i]);
    }

    free(reference);
    free(input);
    free(rawbuf);
}

static void scenario_exfs_hq_picker(void)
{
    camera_on();
    film_by_name("Kodak Gold 200");
    conf.film_exfs = 1;
    filmsim_open_menu();
    cur_mode->redraw(1);
    CHECK(strstr(drawn, lang_str(LANG_EXCCD_HQ_COMPATIBLE)), "Gold needs an HQ compatibility label");
    press(KEY_DOWN);
    CHECK(!strstr(drawn, lang_str(LANG_EXCCD_HQ_GRAIN_AMOUNT)), "EXFS opened HQ settings");
    CHECK(!strstr(drawn, lang_str(LANG_EXCCD_HQ_FALLBACK)), "EXFS showed a fallback hint");

    conf.film_exfs = 0;
    conf.film_exfs_hq = 1;
    conf.film_quick_button = 1;
    camera_info.state.mode_shooting = MODE_P;
    if (!filmsim_quick_open(filmsim_quick_mode(KEY_LEFT))) {
        CHECK(0, "Left shortcut did not open the film picker");
        return;
    }
    cur_mode->redraw(1);
    CHECK(strstr(drawn, lang_str(LANG_EXCCD_HQ_PICK_HELP)), "HQ picker needs the Down hint");
    press(KEY_DOWN);
    CHECK(strstr(drawn, lang_str(LANG_EXCCD_HQ_GRAIN_AMOUNT)), "Left shortcut did not expose HQ settings");
    for (int n = 0; n < 5; ++n) press(KEY_RIGHT);
    press(KEY_DOWN);
    press(KEY_LEFT);
    press(KEY_DOWN);
    press(KEY_RIGHT);
    CHECK(conf.gold200_grain_amount == 0, "Grain settings changed before SET");
    int exits = alt_exits, saves = conf_saves;
    key_held = KEY_SET;
    press(KEY_SET);
    press(KEY_SET);
    key_held = 0;
    CHECK(alt_exits == exits && conf_saves == saves + 1, "SET should save once and return to the picker");
    CHECK(conf.gold200_grain_amount == 3 && conf.gold200_grain_size == -1 &&
          conf.gold200_grain_color == 1, "Each grain level should save independently");

    camera_info.state.mode_shooting = MODE_M;
    if (!filmsim_quick_open(filmsim_quick_print_mode(0)) ||
        !filmsim_quick_open(filmsim_quick_print_mode(1))) {
        CHECK(0, "Two Print presses did not open the film picker");
        return;
    }
    press(KEY_DOWN);
    CHECK(strstr(drawn, "\n+3\n"), "The two-Print picker should reopen saved grain levels");
    press(KEY_LEFT);
    cur_mode->kbd_process_menu_btn();
    CHECK(conf.gold200_grain_amount == 3, "MENU should discard pending edits");
    press(KEY_LEFT);
    CHECK(strstr(drawn, lang_str(LANG_EXCCD_HQ_FALLBACK)), "HQ needs a fallback hint on unsupported looks");
    press(KEY_DOWN);
    CHECK(!strstr(drawn, lang_str(LANG_EXCCD_HQ_GRAIN_AMOUNT)), "An unsupported look opened HQ settings");
    press(KEY_SET);
    CHECK(conf.film_exfs_hq && !conf.film_exfs, "Selecting an unsupported look changed the engine preference");

    static const char presets[] = "Kodak Gold 200 | 11 | 0 | -2 | 1 | 0 | 0 | 0 | 0 | gold200.cv | - | 0\n";
    fs_put("A/CHDK/FILMS/FILMS.BAK", presets, sizeof(presets) - 1);
    filmsim_open_catalog(1);
    press(KEY_RIGHT);
    CHECK(strstr(drawn, lang_str(LANG_EXCCD_HQ_PICK_HELP)), "Gold needs the HQ label in the preset picker");
    press(KEY_DOWN);
    CHECK(strstr(drawn, "\n+3\n"), "The preset picker should share Gold's saved grain levels");
}

static void scenario_exfs_hq_render(void)
{
    unsigned size = camera_sensor.raw_size;
    rawbuf = malloc(size);
    unsigned char *input = malloc(size), *reference = malloc(size);
    CHECK(rawbuf && input && reference, "RAW buffer allocation failed");
    if (!rawbuf || !input || !reference) return;
    camera_on();
    raw_fill_test();
    memcpy(input, rawbuf, size);

    film_by_name("Kodak Gold 200");
    shoot((char *)rawbuf);
    memcpy(reference, rawbuf, size);

    conf.film_exfs = 1;
    memcpy(rawbuf, input, size);
    shoot((char *)rawbuf);
    CHECK(memcmp(reference, rawbuf, size) == 0, "EXFS should leave Gold's legacy rendering unchanged");

    conf.film_exfs = 0;
    conf.film_exfs_hq = 1;
    memcpy(rawbuf, input, size);
    shoot((char *)rawbuf);
    CHECK(exfs_hq_active_profile() && memcmp(reference, rawbuf, size) != 0, "Gold HQ should add grain");
    CHECK(memcmp(reference, rawbuf, camera_sensor.active_area.y1 * camera_sensor.raw_rowlen) == 0,
          "Grain changed the optical black rows");

    conf.gold200_grain_amount = -3;
    memcpy(rawbuf, input, size);
    shoot((char *)rawbuf);
    CHECK(memcmp(reference, rawbuf, size) == 0, "Gold HQ amount -3 should turn grain off");

    film_by_name("Chrome 64");
    conf.film_grain = 2;
    conf.film_exfs = 1;
    conf.film_exfs_hq = 0;
    memcpy(rawbuf, input, size);
    shoot((char *)rawbuf);
    memcpy(reference, rawbuf, size);
    conf.film_exfs = 0;
    conf.film_exfs_hq = 1;
    conf.gold200_grain_amount = 0;
    memcpy(rawbuf, input, size);
    shoot((char *)rawbuf);
    CHECK(!exfs_hq_active_profile() && memcmp(reference, rawbuf, size) == 0,
          "Unsupported looks should retain the EXFS render including legacy grain");
    film_by_name("Kodak Gold 200");
    CHECK(exfs_hq_active_profile() && !conf.film_exfs, "Returning to Gold should reactivate HQ");

    free(reference);
    free(input);
    free(rawbuf);
}

static void scenario_exfs_hq11_parity(void)
{
    unsigned size = camera_sensor.raw_size;
    rawbuf = malloc(size);
    unsigned char *input = malloc(size), *reference = malloc(size);
    CHECK(rawbuf && input && reference, "RAW buffer allocation failed");
    if (!rawbuf || !input || !reference)
        goto out;

    camera_on();
    film_by_name("Kodak Gold 200");
    raw_fill_test();
    for (int y = 7; y < (int)camera_sensor.raw_rows; y += 29) {
        for (int x = 0; x < (int)camera_sensor.raw_rowpix; x += 17) {
            set_raw_pixel(x, y, 0);
            set_raw_pixel(x, y - 1, 1023);
        }
    }
    for (int y = 40; y < 56; y += 2) {
        for (int x = 40; x < 56; x += 2) {
            set_raw_pixel(x, y, 1);
            set_raw_pixel(x + 1, y, 999);
            set_raw_pixel(x, y + 1, 999);
            set_raw_pixel(x + 1, y + 1, 1);
        }
    }
    memcpy(input, rawbuf, size);
    static const int levels[][3] = { {0, 0, 0}, {3, -3, 3}, {-2, 3, -3}, {-3, 0, 0} };
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
        conf.gold200_grain_amount = levels[i][0];
        conf.gold200_grain_size = levels[i][1];
        conf.gold200_grain_color = levels[i][2];
        conf.film_exfs_hq = 1;
        conf.film_exfs_hq_11 = 0;
        memcpy(rawbuf, input, size);
        now_ms = 1000;
        shoot((char *)rawbuf);
        memcpy(reference, rawbuf, size);

        conf.film_exfs_hq = 0;
        conf.film_exfs_hq_11 = 1;
        memcpy(rawbuf, input, size);
        // Both engines have processed the same number of shots at these ticks.
        now_ms = 1000;
        shoot((char *)rawbuf);
        CHECK(memcmp(reference, rawbuf, size) == 0, "HQ 1.1 RAW differs at grain setting %u", i);
    }
out:
    free(reference);
    free(input);
    free(rawbuf);
}

static int hq_patch_tone(int x)
{
    static const short tones[] = { 64, 160, 416, 896 };
    int patch = x / 512;
    if (patch < 4)
        return tones[patch];
    return 416 + ((x & 4) ? 40 : -40);
}

static int hq_neutral_raw(int tone, int wb)
{
    return 31 + ((tone - 31) * 256 + wb / 2) / wb;
}

static void scenario_exfs_hq_texture(void)
{
    const int red_wb = 486, blue_wb = 371;
    rawbuf = malloc(camera_sensor.raw_size);
    CHECK(rawbuf, "RAW buffer allocation failed");
    if (!rawbuf) return;
    camera_on();
    film_by_name("Kodak Gold 200");
    conf.film_exfs_hq = 1;
    const exfs_hq_profile *profile = exfs_hq_active_profile();
    for (int y = 0; y < (int)camera_sensor.raw_rows; y += 2) {
        for (int x = 0; x < (int)camera_sensor.raw_rowpix; x += 2) {
            int tone = hq_patch_tone(x);
            set_raw_pixel(x, y, hq_neutral_raw(tone, red_wb));
            set_raw_pixel(x + 1, y, tone);
            set_raw_pixel(x, y + 1, tone);
            set_raw_pixel(x + 1, y + 1, hq_neutral_raw(tone, blue_wb));
        }
    }
    CHECK(exfs_hq_grain((char *)rawbuf, profile, red_wb, blue_wb), "HQ grain allocation failed");
    double rms[5], chroma[5];
    for (int i = 0; i < 5; ++i) {
        double sum = 0, luma_sq = 0, chroma_sq = 0;
        int n = 0;
        for (int y = 90; y < 600; y += 6) {
            for (int x = 512 * i + 64; x < 512 * i + 448; x += 6) {
                int tone = hq_patch_tone(x);
                double r = (RAW10(x, y) - hq_neutral_raw(tone, red_wb)) * red_wb / 256.0;
                double g = (RAW10(x + 1, y) + RAW10(x, y + 1)) / 2.0 - tone;
                double b = (RAW10(x + 1, y + 1) - hq_neutral_raw(tone, blue_wb)) * blue_wb / 256.0;
                double luma = (4 * r + 10 * g + 2 * b) / 16;
                sum += luma;
                luma_sq += luma * luma;
                chroma_sq += ((r - g) * (r - g) + (b - g) * (b - g)) / 2;
                ++n;
            }
        }
        rms[i] = sqrt(luma_sq / n);
        chroma[i] = sqrt(chroma_sq / n);
        double mean = sum / n;
        CHECK(mean > -1.3 && mean < 1.3, "Patch %d has a mean brightness shift of %.2f", i, mean);
    }
    printf("Gold HQ luma RMS: black %.2f, shadow %.2f, midtone %.2f, highlight %.2f, detail %.2f\n",
           rms[0], rms[1], rms[2], rms[3], rms[4]);
    printf("Gold HQ midtone chroma RMS: %.2f\n", chroma[2]);
    CHECK(rms[2] > 18 && rms[2] < 25, "Default midtone grain differs from the tuned intensity");
    CHECK(rms[1] > rms[2] * .4 && rms[1] < rms[2] * .75 &&
          rms[0] < rms[1] && rms[3] < rms[1] * .5, "The tonal grain envelope is uneven");
    CHECK(chroma[2] < rms[2] * .23, "Default grain should be predominantly luminance");
    CHECK(rms[4] >= rms[2] * .70 && rms[4] < rms[2] * .93, "Fine details should retain visible, softer grain");

    int r0 = hq_neutral_raw(416, red_wb), b0 = hq_neutral_raw(416, blue_wb);
    unsigned seed = 19;
    for (int y = 80; y < 608; y += 2) {
        for (int x = 1056; x < 1504; x += 2) {
            seed = seed * 1664525u + 1013904223u;
            set_raw_pixel(x, y, r0 + (int)(seed >> 27) - 16);
            set_raw_pixel(x + 1, y, 416);
            set_raw_pixel(x, y + 1, 416);
            set_raw_pixel(x + 1, y + 1, b0 + (int)((seed >> 22) & 31) - 16);
        }
    }
    filmsim_cnr_hook((char *)rawbuf);
    double sensor_sq = 0;
    int n = 0, changed_green = 0;
    for (int y = 90; y < 600; y += 6) {
        for (int x = 1064; x < 1496; x += 6) {
            int r = RAW10(x, y) - r0, b = RAW10(x + 1, y + 1) - b0;
            sensor_sq += (r * r + b * b) / 2.0;
            changed_green += RAW10(x + 1, y) != 416 || RAW10(x, y + 1) != 416;
            ++n;
        }
    }
    CHECK(sqrt(sensor_sq / n) < 7.9 && !changed_green,
          "Sensor cleanup should reduce chroma noise and preserve both green samples");
    free(rawbuf);
}
#endif

#if CAM_SENSOR_BITS_PER_PIXEL == 12
static long long raw_sum(void)
{
    long long s = 0;
    int x, y;
    for (y = 100; y < (int)camera_sensor.raw_rows - 100; y += 7)
        for (x = 200; x < (int)camera_sensor.raw_rowpix - 200; x += 5) s += get_raw_pixel(x, y);
    return s;
}
#endif

static void scenario_recipe(const char *root)
{
    const char *t;
    load_card_curve(root, "SUMMER.CV");
    camera_on();
    filmsim_menu_enum(0, 0);

    // ohne RECIPES.TXT: kein Rezept
    CHECK(strcmp(filmsim_recipe_enum(0, 0), "-") == 0, "ohne Datei: %s", filmsim_recipe_enum(0, 0));
    CHECK(strcmp(filmsim_recipe_enum(1, 0), "-") == 0, "ohne Datei blaettern: %s", filmsim_recipe_enum(0, 0));

    // C1: Summer 200, Tageslicht, R+2 B-1, Korn fein
    film_by_name("Summer 200");
    conf.film_grain = 2;
    filmsim_wb_r_enum(1, 0); filmsim_wb_r_enum(1, 0); filmsim_wb_b_enum(-1, 0);
    CHECK(strcmp(filmsim_wb_r_enum(0, 0), "+2") == 0 && strcmp(filmsim_wb_b_enum(0, 0), "-1") == 0, "Shift-Anzeige");
    filmsim_wb_enum(1, 0);
    CHECK(shooting_get_prop(PROPCASE_WB_MODE) == 1, "WB Tageslicht: %d", shooting_get_prop(PROPCASE_WB_MODE));
    for (int k = 0; k < 20; k++) filmsim_wb_r_enum(1, 0);
    CHECK(conf.film_wb_r == 9, "Shift begrenzt: %d", conf.film_wb_r);
    for (int k = 0; k < 7; k++) filmsim_wb_r_enum(-1, 0);

    // WB-Shift-Raster: oeffnet aus dem Menue, Pfeile verschieben, SET erst nach Loslassen
    {
        gui_handler *menu = &menuGuiHandler;
        cur_mode = menu;
        CHECK(strcmp(filmsim_wb_shift_enum(0, 0), "R+2 B-1") == 0, "Raster-Eintrag: %s", filmsim_wb_shift_enum(0, 0));
        filmsim_wb_shift_enum(1, 0);
        CHECK(cur_mode != menu, "Raster nicht offen");
        gui_handler *g = cur_mode;
        g->redraw(1);
        key_held = KEY_SET; next_key = KEY_SET; g->kbd_process();      // Wiederholung der oeffnenden SET-Taste
        CHECK(cur_mode == g, "gehaltenes SET schliesst das Raster");
        key_held = 0;
        long keys[] = { KEY_RIGHT, KEY_RIGHT, KEY_DOWN, KEY_UP, KEY_DOWN };
        for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) { next_key = keys[k]; g->kbd_process(); g->redraw(0); }
        CHECK(conf.film_wb_r == 2 && conf.film_wb_b == -1, "Raster aendert vor SET: R%d B%d", conf.film_wb_r, conf.film_wb_b);
        next_key = KEY_SET; g->kbd_process();
        CHECK(cur_mode == menu && conf.film_wb_r == 4 && conf.film_wb_b == -2, "SET: R%d B%d", conf.film_wb_r, conf.film_wb_b);
        CHECK(strcmp(filmsim_wb_shift_enum(0, 0), "R+4 B-2") == 0, "Raster-Eintrag nach SET: %s", filmsim_wb_shift_enum(0, 0));
        // MENU bricht ab
        filmsim_wb_shift_enum(-1, 0); g = cur_mode;
        for (int k = 0; k < 30; k++) { next_key = KEY_LEFT; g->kbd_process(); }
        g->redraw(0);
        g->kbd_process_menu_btn();
        CHECK(cur_mode == menu && conf.film_wb_r == 4, "MENU: R%d", conf.film_wb_r);
        // Zoom setzt auf 0, Raender halten
        filmsim_wb_shift_enum(1, 0); g = cur_mode;
        for (int k = 0; k < 30; k++) { next_key = KEY_UP; g->kbd_process(); }
        g->redraw(0);
        next_key = KEY_ZOOM_IN; g->kbd_process();
        next_key = KEY_SET; g->kbd_process();
        CHECK(conf.film_wb_r == 0 && conf.film_wb_b == 0, "Zoom: R%d B%d", conf.film_wb_r, conf.film_wb_b);
        CHECK(strcmp(filmsim_wb_shift_enum(0, 0), "0") == 0, "Raster-Eintrag 0: %s", filmsim_wb_shift_enum(0, 0));
        filmsim_wb_r_enum(2, 0); filmsim_wb_b_enum(-1, 0);
    }
    filmsim_recipe_save_menu(0);
    CHECK(menu_open == &filmsim_save_menu && strcmp(filmsim_slot_text[0], "C1 (empty)") == 0, "Speichern-Menue: %s", filmsim_slot_text[0]);
    filmsim_recipe_save(0);
    CHECK(strcmp(filmsim_slot_text[0], "C1 Summer 200") == 0, "Platzname nach Speichern: %s", filmsim_slot_text[0]);
    CHECK(strcmp(filmsim_look_text, "Look: Summer 200") == 0, "Look-Eintrag: %s", filmsim_look_text);
    t = card_text("A/CHDK/FILMS/RECIPES.TXT");
    CHECK(strstr(t, "[C1] Summer 200") && strstr(t, "film      = Summer 200") && strstr(t, "wb        = daylight")
          && strstr(t, "shift     = R+2 B-1") && strstr(t, "grain     = fine"), "RECIPES.TXT:\n%s", t);
    CHECK(conf.film_recipe == 1, "aktiv nach Speichern: %d", conf.film_recipe);
    CHECK(strcmp(filmsim_recipe_enum(0, 0), "C1 Summer 200") == 0, "Anzeige: %s", filmsim_recipe_enum(0, 0));
    conf.film_grain = 3;
    CHECK(strcmp(filmsim_recipe_enum(0, 0), "C1 Summer 200*") == 0, "geaendert: %s", filmsim_recipe_enum(0, 0));
    conf.film_grain = 2;
    shooting_set_prop(PROPCASE_WB_MODE, 2);
    CHECK(strchr(filmsim_recipe_enum(0, 0), '*') != 0, "WB geaendert ohne *");

    // C2: Noir, Kunstlicht, Dynamik+ 1, kein Shift; belegter Platz fragt erst
    film_by_name("Noir 3200");
    conf.film_grain = 0; conf.film_wb_r = conf.film_wb_b = 0; conf.film_dr = 1;
    shooting_set_prop(PROPCASE_WB_MODE, 3);
    filmsim_recipe_save(0);                         // C1 belegt -> Rueckfrage (Stummel antwortet nicht)
    CHECK(strstr(card_text("A/CHDK/FILMS/RECIPES.TXT"), "Noir") == 0, "C1 ohne Rueckfrage ueberschrieben");
    CHECK(strcmp(filmsim_slot_text[1], "C2 (empty)") == 0, "Platz: %s", filmsim_slot_text[1]);
    filmsim_recipe_save(1);
    CHECK(strcmp(filmsim_look_text, "Look: Noir 3200") == 0, "Look-Eintrag: %s", filmsim_look_text);
    t = card_text("A/CHDK/FILMS/RECIPES.TXT");
    CHECK(strstr(t, "[C1] Summer 200") && strstr(t, "[C2] Noir 3200") && strstr(t, "wb        = tungsten")
          && strstr(t, "dynamic   = +1"), "zwei Rezepte:\n%s", t);

    // Laden: C2 -> (weiter) "-" -> C1
    CHECK(strcmp(filmsim_recipe_enum(1, 0), "-") == 0, "nach C2 kommt -");
    CHECK(strcmp(filmsim_recipe_enum(1, 0), "C1 Summer 200") == 0, "C1 laden: %s", filmsim_recipe_enum(0, 0));
    CHECK(strcmp(filmsim_name(), "Summer 200") == 0 && conf.film_grain == 2 && conf.film_dr == 0
          && conf.film_wb_r == 2 && conf.film_wb_b == -1, "C1 geladen: %s grain %d dr %d R%d B%d",
          filmsim_name(), conf.film_grain, conf.film_dr, conf.film_wb_r, conf.film_wb_b);
    CHECK(shooting_get_prop(PROPCASE_WB_MODE) == 1, "C1 WB: %d", shooting_get_prop(PROPCASE_WB_MODE));
    CHECK(strcmp(filmsim_recipe_enum(-1, 0), "-") == 0, "zurueck auf -");
    CHECK(strcmp(filmsim_recipe_enum(-1, 0), "C2 Noir 3200") == 0, "C2 laden: %s", filmsim_recipe_enum(0, 0));
    CHECK(strcmp(filmsim_name(), "Noir 3200") == 0 && conf.film_dr == 1 && conf.film_wb_r == 0
          && shooting_get_prop(PROPCASE_WB_MODE) == 3, "C2 geladen");

#if CAM_SENSOR_BITS_PER_PIXEL == 12
    // Shift wirkt in der Kurve: gleiche Aufnahme mit und ohne Verschiebung
    rawbuf = malloc(camera_sensor.raw_rows * camera_sensor.raw_rowlen + 8);
    film_by_name("Summer 200");
    conf.film_dr = 0; conf.film_wb_r = conf.film_wb_b = 0;
    filmsim_set(conf.film_sim);
    raw_fill_test(); filmsim_curve_apply((char *)rawbuf);
    long long base = raw_sum();
    filmsim_wb_r_enum(3, 0);
    raw_fill_test(); filmsim_curve_apply((char *)rawbuf);
    long long warm = raw_sum();
    filmsim_wb_r_enum(-3, 0);
    raw_fill_test(); filmsim_curve_apply((char *)rawbuf);
    CHECK(raw_sum() == base, "Shift 0 nicht wie vorher");
    CHECK(warm < base, "R+3 senkt G/B nicht: %lld / %lld", warm, base);
    // Film ohne Kurve (Daylight): Shift wirkt ueber die neutrale Kurve
    film_by_name("Daylight 100");
    raw_fill_test(); filmsim_curve_apply((char *)rawbuf);
    long long d0 = raw_sum();
    filmsim_wb_b_enum(-4, 0);
    raw_fill_test(); filmsim_curve_apply((char *)rawbuf);
    CHECK(raw_sum() < d0, "Daylight + B-4 ohne Wirkung");
    // Aus: keine Kurve, auch mit Shift
    filmsim_set(0);
    raw_fill_test(); long long o0 = raw_sum(); filmsim_curve_apply((char *)rawbuf);
    CHECK(raw_sum() == o0, "Aus mit Shift veraendert das Bild");
#endif

    filmsim_set(0);
    conf.film_vignette = 1;
    filmsim_recipe_save(2);
    CHECK(conf.film_recipe == 3 && strcmp(filmsim_perf_enum(1, 0), "On") == 0,
          "An effects-only recipe should allow metrics");
    filmsim_recipe_enum(1, 0);
    CHECK(strcmp(filmsim_perf_enum(0, 0), "needs a recipe") == 0,
          "Leaving an effects-only recipe should suspend metrics");
}

// Auto-ISO mit kuerzester Zeit: Canon misst, EXCCD verschiebt ISO und Zeit
static void scenario_aiso(void)
{
    camera_on();
    short s100 = shooting_get_sv96_from_iso(100), s800 = shooting_get_sv96_from_iso(800);
    camera_info.state.mode_shooting = MODE_P;
    conf.tv_override_enabled = 0;
    conf.film_aiso = 5; conf.film_aiso_max = 2;                 // 1/60, ISO max 800
    CHECK(strcmp(filmsim_aiso_enum(0, 0), "1/60") == 0, "Menue: %s", filmsim_aiso_enum(0, 0));
    CHECK(strcmp(filmsim_aiso_max_enum(0, 0), "800") == 0, "ISO max: %s", filmsim_aiso_max_enum(0, 0));

    // 1/8 s bei ISO 100 -> 3 EV mehr: ISO 800, 1/60
    ex_isomode = 0; ex_sv = s100; ex_tv = 288;
    CHECK(filmsim_autoiso() == 1 && ex_sv == s100 + 279 && ex_tv == 567, "1/8: sv %d tv %d", ex_sv, ex_tv);
    // 1/2 s bei ISO 100 -> nur bis ISO 800, Zeit bleibt laenger (1/15)
    ex_sv = s100; ex_tv = 96;
    CHECK(filmsim_autoiso() == 1 && ex_sv == s800 && ex_tv == 96 + (s800 - s100), "1/2: sv %d tv %d", ex_sv, ex_tv);
    // Canon nimmt ISO 400 bei 1/500 -> herunter auf ISO 100, 1/125 (Belichtung gleich)
    ex_sv = shooting_get_sv96_from_iso(400); ex_tv = 861;
    int ev = ex_tv - ex_sv;
    CHECK(filmsim_autoiso() == 1 && ex_sv == s100 && ex_tv - ex_sv == ev && ex_tv >= 567, "hell: sv %d tv %d", ex_sv, ex_tv);
    // schon passend: nichts tun
    ex_sv = s100; ex_tv = 700;
    CHECK(filmsim_autoiso() == 0 && ex_tv == 700, "passend veraendert");
    // feste Canon-ISO, Tv-Override, Blitz, Aus: nichts tun
    ex_isomode = 2; ex_tv = 288;
    CHECK(filmsim_autoiso() == 0, "feste ISO");
    ex_isomode = 0; conf.tv_override_enabled = 1;
    CHECK(filmsim_autoiso() == 0, "Tv-Override");
    conf.tv_override_enabled = 0; shooting_set_prop(PROPCASE_IS_FLASH_READY, 1);
    CHECK(filmsim_autoiso() == 0, "Blitz");
    shooting_set_prop(PROPCASE_IS_FLASH_READY, 0);
    // Szenenmodi mit eigener langer Zeit (Feuerwerk, Langzeit) und Tv/M: nichts tun
    ex_sv = s100; ex_tv = 96;
    camera_info.state.mode_shooting = MODE_FIREWORK;
    CHECK(filmsim_autoiso() == 0 && ex_tv == 96, "Feuerwerk: tv %d", ex_tv);
    camera_info.state.mode_shooting = MODE_LONG_SHUTTER;
    CHECK(filmsim_autoiso() == 0, "Langzeit");
    camera_info.state.mode_shooting = MODE_TV;
    CHECK(filmsim_autoiso() == 0, "Tv");
    camera_info.state.mode_shooting = MODE_AV;
    CHECK(filmsim_autoiso() == 1, "Av soll wirken");
    // Skript laeuft: Zeit bleibt, wie das Skript sie will
    camera_info.state.mode_shooting = MODE_P;
    ex_sv = s100; ex_tv = 96;
    camera_info.state.state_kbd_script_run = 1;
    CHECK(filmsim_autoiso() == 0 && ex_tv == 96, "Skript: tv %d", ex_tv);
    camera_info.state.state_kbd_script_run = 0;
    // Canon nimmt schon ISO 400, ISO max 200: nicht herunter (Zeit wuerde laenger)
    conf.film_aiso_max = 0;
    short s400 = shooting_get_sv96_from_iso(400);
    ex_sv = s400; ex_tv = 288;
    filmsim_autoiso();
    CHECK(ex_sv == s400 && ex_tv == 288, "ueber ISO max gesenkt: sv %d tv %d", ex_sv, ex_tv);
    conf.film_aiso_max = 2;
    // Dynamik+: nur wenn die Kamera die Zeit waehlt
    conf.film_dr = 1;
    CHECK(filmsim_dr_tv96_shift(1) == 96, "DR in P: %d", filmsim_dr_tv96_shift(1));
    camera_info.state.mode_shooting = MODE_TV;
    CHECK(filmsim_dr_tv96_shift(1) == 0, "DR in Tv");
    camera_info.state.mode_shooting = MODE_M;
    CHECK(filmsim_dr_tv96_shift(1) == 0, "DR in M");
    camera_info.state.mode_shooting = MODE_P;
    conf.film_dr = 0;
    // Statuszeile: eingestellt, wirkt aber nicht -> "A-ISO -"
    CHECK(strcmp(filmsim_aiso_str(), "A-ISO 1/60") == 0, "A-ISO P: %s", filmsim_aiso_str());
    camera_info.state.mode_shooting = MODE_TV;
    CHECK(strcmp(filmsim_aiso_str(), "A-ISO -") == 0, "A-ISO Tv: %s", filmsim_aiso_str());
    camera_info.state.mode_shooting = MODE_P;
    // S/W-Filter auch fuer Tintype, WB-Verschiebung braucht einen Look
    film_by_name("Tintype 50");
    CHECK(strcmp(filmsim_bwfilter_enum(0, 0), "B&W only") != 0, "Tintype ohne S/W-Filter");
    film_by_name("Summer 200");
    CHECK(strcmp(filmsim_bwfilter_enum(0, 0), "B&W only") == 0, "Farbfilm mit S/W-Filter");
    CHECK(strcmp(filmsim_wb_shift_enum(0, 0), "0") == 0, "WB mit Look: %s", filmsim_wb_shift_enum(0, 0));
    filmsim_set(0);
    CHECK(strcmp(filmsim_wb_shift_enum(0, 0), "needs a look") == 0, "WB bei Aus: %s", filmsim_wb_shift_enum(0, 0));
    // Auto: 1/Brennweite, Tele 105 mm -> mindestens 1/105
    conf.film_aiso = 1; ex_fl = 105000; ex_sv = s100; ex_tv = 480;
    CHECK(filmsim_autoiso() == 1 && ex_tv >= 640 && ex_tv <= 648, "Auto 105 mm: tv %d", ex_tv);
    // ISO max nur bis zur Kamera (800): 200 400 800, dann wieder 200
    conf.film_aiso_max = 2;
    CHECK(strcmp(filmsim_aiso_max_enum(1, 0), "200") == 0, "ISO max weiter: %s", filmsim_aiso_max_enum(0, 0));
    conf.film_aiso_max = 3;
    CHECK(strcmp(filmsim_aiso_max_enum(0, 0), "800") == 0, "1600 auf 800 begrenzt: %s", filmsim_aiso_max_enum(0, 0));
    conf.film_aiso = 0;
    CHECK(filmsim_autoiso() == 0 && filmsim_aiso_str() == 0, "Aus");
}

#ifdef FILMSIM_JPEG_PAIR
static void scenario_jpeg_pair(void)
{
    static const char perf_csv[] = "A/CHDK/LOGS/PERF.CSV";
    static const char single[] = "A/DCIM/100CANON/IMG_8344.JPG";
    static const char original[] = "A/DCIM/100CANON/IMG_8345.JPG";
    static const char helper[] = "A/DCIM/100CANON/IMG_8346.JPG";
    static const char raw_file[] = "A/CHDK/DATA/EXCCDJP.TMP";
    static const char jpeg[] = { (char)0xff, (char)0xd8, (char)0xff, (char)0xd9 };
    char sensor[64];
    memset(sensor, 42, sizeof(sensor));
    camera_on();
    camera_sensor.raw_size = sizeof(sensor);
    CHECK(!conf.film_save_original_jpeg && strcmp(filmsim_pair_enum(1, 0), "needs a look") == 0,
          "Original JPEG must be gated when the look is Off");
    CHECK(strcmp(filmsim_perf_enum(1, 0), "needs a recipe") == 0 && !conf.film_performance_metrics,
          "Performance Metrics must be gated when the look is Off");

    film_by_name("Chrome 64");
    CHECK(!filmsim_pair_original(sensor, 125) && shooting_get_prop(PROPCASE_MY_COLORS) == MC_CUSTOM,
          "Disabled setting must leave the recipe-only capture path active");
    file_number = 8344;
    filmsim_perf_begin(FILMSIM_PERF_SINGLE);
    filmsim_perf_raw_done(0);
    filmsim_pair_filewrite(single);
    filmsim_pair_file_closed();
    filmsim_tick();
    CHECK(fs_find(perf_csv) < 0, "Metrics logged with selector off");

    CHECK(strcmp(filmsim_perf_enum(1, 0), "On") == 0, "Look should allow metrics");
    filmsim_perf_begin(FILMSIM_PERF_SINGLE);
    filmsim_perf_mark(PERF_CURVE);
    now_ms += 100;
    filmsim_perf_raw_done(0);
    filmsim_pair_filewrite(original);
    filmsim_pair_file_closed();
    filmsim_tick();
    CHECK(fs_find(perf_csv) < 0, "Unrelated JPEG close completed the measurement");
    now_ms += 50;
    filmsim_pair_filewrite(single);
    filmsim_pair_file_closed();
    filmsim_tick();
    CHECK(strstr(card_text(perf_csv),
                 "single_photo;ok;100CANON/IMG_8344.JPG;;\"Chrome 64\";;150;;;;;;;;;;;100;;;;;;;;50\r\n"),
          "Single capture metrics do not match:\n%s", card_text(perf_csv));

    filmsim_pair_enum(1, 0);
    CHECK(shooting_get_prop(PROPCASE_MY_COLORS) == MC_OFF, "Original must use Canon colours");
    shooting_set_prop(PROPCASE_FLASH_MODE, 1);
    file_number = 8345;
    CHECK(filmsim_pair_original(sensor, 125), "Original capture did not queue a helper");
    int source = fs_find(raw_file);
    CHECK(source >= 0 && card[source].size == sizeof(sensor) &&
          memcmp(card[source].data, sensor, sizeof(sensor)) == 0, "Wrong retained sensor data");
    filmsim_perf_raw_done(1);
    filmsim_pair_filewrite(original);
    fs_put(original, jpeg, sizeof(jpeg));
    filmsim_tick();
    filmsim_pair_kbd();
    CHECK(!injected_key && !filmsim_develop_armed(), "Helper started before Canon closed the original");

    now_ms += 90;
    filmsim_pair_file_closed();
    filmsim_tick();
    filmsim_pair_kbd();
    CHECK(injected_key == KEY_SHOOT_HALF && filmsim_develop_iso() == 125 &&
          strcmp(prep_fn, raw_file) == 0 && shooting_get_prop(PROPCASE_MY_COLORS) == MC_CUSTOM,
          "Helper did not queue the original sensor data and recipe");
    camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_STARTED;
    filmsim_pair_kbd();
    CHECK(injected_key == KEY_SHOOT_FULL_ONLY, "Helper did not complete the shutter press");
    file_number = 8346;
    now_ms += 210;
    filmsim_pair_perf_helper_begin();
    filmsim_perf_mark(PERF_RAW_LOAD);
    now_ms += 60;
    filmsim_perf_mark(PERF_NONE);
    filmsim_pair_helper_raw(1);
    filmsim_perf_mark(PERF_COLOR_NR);
    now_ms += 80;
    filmsim_perf_raw_done(0);
    camera_info.state.state_shooting_progress = SHOOTING_PROGRESS_NONE;
    filmsim_pair_filewrite(helper);
    fs_put(helper, jpeg, sizeof(jpeg));
    now_ms += 140;
    filmsim_pair_file_closed();
    filmsim_tick();
    filmsim_pair_kbd();
    int first = fs_find(original), second = fs_find(helper);
    CHECK(first >= 0 && second >= 0 && fs_find(raw_file) < 0 && !pair_errors,
          "Both JPEGs must keep their Canon filenames");
    CHECK(first >= 0 && second >= 0 && card[first].size == sizeof(jpeg) &&
          card[second].size == sizeof(jpeg) &&
          memcmp(card[first].data, jpeg, sizeof(jpeg)) == 0 &&
          memcmp(card[second].data, jpeg, sizeof(jpeg)) == 0,
          "Completing a pair must preserve both JPEG files");
    CHECK(shooting_get_prop(PROPCASE_FLASH_MODE) == 1, "Original flash setting was not restored");
    CHECK(strstr(card_text(perf_csv),
                 "keep_original_jpeg;ok;100CANON/IMG_8346.JPG;100CANON/IMG_8345.JPG;\"Chrome 64\";;580;0;;90;210;60;;;;;80;;;;;;;;;140\r\n"),
          "Pair metrics do not match:\n%s", card_text(perf_csv));
    const char *header_end = strstr(card_text(perf_csv), "\r\n");
    CHECK(header_end && !strstr(header_end + 2, "mode;status;"),
          "Performance CSV has a duplicate header");

    filmsim_set(0);
    CHECK(!conf.film_save_original_jpeg, "Off must clear Save Original JPEG");
    CHECK(strcmp(filmsim_perf_enum(0, 0), "needs a recipe") == 0, "Off must suspend metrics");
}
#endif

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    const char *root = argc > 2 ? argv[2] : ".";
    if (strcmp(what, "names") == 0) scenario_names(argc > 3 ? argv[3] : "0");
    else if (strcmp(what, "picker") == 0) scenario_picker();
#ifdef FILMSIM_QUICK_BUTTONS
    else if (strcmp(what, "quick_buttons") == 0) scenario_quick_buttons();
#endif
    else if (strcmp(what, "raw") == 0) scenario_raw(root);
    else if (strcmp(what, "develop") == 0) scenario_develop();
    else if (strcmp(what, "recipe") == 0) scenario_recipe(root);
    else if (strcmp(what, "aiso") == 0) scenario_aiso();
    else if (strcmp(what, "mixwb") == 0) scenario_mixwb();
    else if (strcmp(what, "thumb") == 0) scenario_thumb(argc > 3 ? argv[3] : 0, argc > 4 ? argv[4] : 0);
#ifdef FILMSIM_EXFS
    else if (strcmp(what, "exfs") == 0) scenario_exfs();
    else if (strcmp(what, "exfs_hq_picker") == 0) scenario_exfs_hq_picker();
    else if (strcmp(what, "exfs_hq_render") == 0) scenario_exfs_hq_render();
    else if (strcmp(what, "exfs_hq11_parity") == 0) scenario_exfs_hq11_parity();
    else if (strcmp(what, "exfs_hq_texture") == 0) scenario_exfs_hq_texture();
    else if (strcmp(what, "exfs_develop") == 0) {
        conf.film_exfs = 1;
        scenario_develop();
    }
    else if (strcmp(what, "exfs_11_develop") == 0) {
        conf.film_exfs_11 = 1;
        scenario_develop();
    }
#endif
#ifdef FILMSIM_JPEG_PAIR
    else if (strcmp(what, "jpeg_pair") == 0) scenario_jpeg_pair();
#endif
    else { printf("Szenario?\n"); return 2; }
    printf("%s (%d Fehler)\n", fails ? "FEHLER" : "ok", fails);
    return fails != 0;
}
