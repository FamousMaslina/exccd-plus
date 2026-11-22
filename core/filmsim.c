/*
 * Filmsimulationen fuer CHDK - IXUS 60 / PowerShot SD600
 *
 * Eine Simulation besteht aus:
 *   1. Canon "My Colors"-Modus (Custom Color, Schwarzweiss, Sepia ...)
 *   2. bei Custom Color: 7 Regler (-2..+2) Kontrast, Schaerfe, Saettigung,
 *      Rot, Gruen, Blau, Hautton
 *   3. optional einer RAW-Tonkurve (A/CHDK/CURVES/<name>.CV), die curves.flt
 *      vor der JPEG-Erzeugung auf die Rohdaten anwendet.
 *
 * filmsim_tick() wird aus der CHDK-Hauptschleife aufgerufen und stellt im
 * Aufnahmemodus sicher, dass die Canon-Einstellungen zur gewaehlten
 * Simulation passen (auch nach dem Einschalten oder wenn jemand im
 * Canon-Menue My Colors aendert).
 */

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
#include "filmsim_txt.h"
#include "filmsim_recipe.h"
#include "lens.h"
// Fortschrittsbalken beim RAW-Entwickeln (am Dateiende), auch aus filmsim_raw.h gerufen
static void prog_rows(int done, int total);
#define FS_PROG_ROW(y, n) prog_rows((y), (n))
#include "filmsim_raw.h"
#include "filmsim_dev.h"
#include "raw.h"
#include "gui_mbox.h"
#include "action_stack.h"
#include "lang.h"
#include "gui_lang.h"
#include "stdio.h"
#ifdef FILMSIM_JPEG_PAIR
#include "cachebit.h"
#include "console.h"
#include "remotecap_core.h"
#include "usb_remote.h"
static int pair_use_original(void);
static void pair_tick(void);
#endif

//-------------------------------------------------------------------
// Kameraeinstellungen fuer EXCCD
//   Jede Kamera setzt sie in platform/<kamera>/platform_camera.h, Block
//   "EXCCD". Der Kern fragt nur diese Schalter ab, nie den Kameranamen.
//   Was CHDK schon weiss (Bit-Tiefe, Bayer-Anordnung, Sensorgroesse),
//   kommt direkt aus camera.h.
//
//   FILMSIM_LABELS       1 = Laschen (.FLM) und Splash passen zur Palette
//                        der Kamera (gebaut aus tools/filmsim/cams/<kamera>/).
//                        Ohne: Textkarten, kein Splash.
//   FILMSIM_COLOR_BG     Palettenindizes fuer Textkarte und Leiste
//   FILMSIM_COLOR_GREY   (Standard: CHDK-Farben)
//   FILMSIM_COLOR_RULE
//   FILMSIM_WB_R/_B      Tageslicht-Gains des Sensors * 256
//                        (Standard: Werte der IXUS 60, bis kalibriert)
//-------------------------------------------------------------------
#ifndef FILMSIM_COLOR_BG
#define FILMSIM_COLOR_BG    COLOR_BLACK
#define FILMSIM_COLOR_GREY  COLOR_GREY
#define FILMSIM_COLOR_RULE  COLOR_GREY_DK
#endif
#ifndef FILMSIM_WB_R
#define FILMSIM_WB_R        486
#define FILMSIM_WB_B        371
#endif

//-------------------------------------------------------------------
// Rohdaten-Zugriff fuer 10- und 12-Bit-Sensoren
//   Alle RAW-Funktionen rechnen auf der 10-Bit-Skala der IXUS 60
//   (Schwarz 31, Weiss 1023). Bei 12 Bit wird beim Lesen durch 4 geteilt
//   (127 -> 31, 4095 -> 1023); beim Schreiben bleiben die unteren zwei Bit
//   des Originalpixels erhalten - unveraenderte Pixel sind damit bitgleich,
//   veraenderte behalten ihr feines Sensorrauschen als natuerlichen Dither.
//   FS_GRP = Bytes je 8 Pixel.
//
//   FS_ROW0: erste Zeile mit R G (die Funktionen erwarten R G / G B).
//   IXUS 60: R G / G B ab Zeile 0, IXUS 145: G B / R G -> ab Zeile 1.
//-------------------------------------------------------------------
#if CAM_SENSOR_BITS_PER_PIXEL == 10 || CAM_SENSOR_BITS_PER_PIXEL == 12
#define FS_RAW 1
#define FS_GRP CAM_SENSOR_BITS_PER_PIXEL

#if cam_CFAPattern == 0x02010100        // Red Green Green Blue
#define FS_ROW0 0
#elif cam_CFAPattern == 0x01000201      // Green Blue Red Green
#define FS_ROW0 1
#else
#error "Filmsim: Bayer-Anordnung nicht unterstuetzt (cam_CFAPattern)"
#endif
#endif

// dec8/enc8 lesen halbwortweise, das braucht gerade Zeilenlaengen
#if defined(FS_RAW) && ((CAM_RAW_ROWPIX * CAM_SENSOR_BITS_PER_PIXEL / 8) & 1)
#error "Filmsim: ungerade Zeilenlaenge im Rohbild (filmsim_raw.h)"
#endif

#ifndef PROPCASE_MY_COLORS
// Propset ohne bekannte My-Colors-Propcases (CHDK propset 3, z. B. G11): Looks wirken
// nur ueber Kurven und Rohdaten, Canons Farbeinstellungen bleiben unangetastet.
// Die Propcase-Nummern lassen sich auf der Kamera mit FILMSIM_DIAG finden.
#define FS_NO_MYCOLORS 1
#endif

// Eingebaute Tabelle, falls A/CHDK/FILMS/FILMS.TXT fehlt oder unbrauchbar ist
static const filmsim_t sims_builtin[] = {
    // name              mode        con shp sat  R   G   B  skin  curve           label      mix
    { "Off",             MC_OFF,      0,  0,  0,  0,  0,  0,  0,  0,              "AUS",      0 },
    { "Daylight 100",    MC_OFF,      0,  0,  0,  0,  0,  0,  0,  0,              "DAYLITE",  0 },
    { "Postcard 100",    MC_CUSTOM,   0,  1,  2,  1,  1,  1,  0,  "POSTCRD.CV",   "POSTCRD",  0 },
    { "Blush 100",       MC_CUSTOM,  -1,  0,  1,  0,  0,  0,  1,  "BLUSH.CV",     "BLUSH",    0 },
    { "Journal 400",     MC_CUSTOM,   0,  0, -1,  0, -1,  0,  0,  "JOURNAL.CV",   "JOURNAL",  0 },
    { "Studio 160",      MC_CUSTOM,  -1, -1, -1,  0,  0,  0,  1,  "STUDIO.CV",    "STUDIO",   0 },
    { "Drugstore 200",   MC_CUSTOM,   0,  0, -1,  0,  1, -1,  0,  "DRUGSTR.CV",   "DRUGSTR",  0 },
    { "Scene 500",       MC_CUSTOM,  -2, -1, -2,  0,  0,  0,  0,  "SCENE.CV",     "SCENE",    0 },
    { "Silver 400",      MC_BW,       0,  0,  0,  0,  0,  0,  0,  "SILVER.CV",    "SILVER",   0 },
    { "Portrait 400",    MC_CUSTOM,  -1, -1,  0,  0,  0, -1,  1,  "PORTRT.CV",    "PORTRT",   0 },
    { "Tungsten 800",    MC_OFF,      0,  0,  0,  0,  0,  0,  0,  "TUNGSTN.CV",   "TUNGSTN",  0 },
    { "Expired 200",     MC_CUSTOM,  -1, -1, -1,  0,  0,  0,  0,  "EXPIRED.CV",   "EXPIRED",  0 },
    { "Cross 100",       MC_CUSTOM,   0,  1,  2,  0,  0,  0,  0,  "CROSS.CV",     "CROSS",    0 },
    { "Violet 400",      MC_CUSTOM,   0,  0,  1,  0,  0,  0,  0,  "VIOLET.CV",    "VIOLET",   1 },
    { "Concrete 100",    MC_CUSTOM,   0,  1, -1,  0,  0,  0,  0,  "CONCRET.CV",   "CONCRET",  2 },
    { "Ember 200",       MC_CUSTOM,   0,  0,  0,  0,  0,  0,  0,  "EMBER.CV",     "EMBER",    0 },
    { "Grit 400",        MC_BW,       0,  0,  0,  0,  0,  0,  0,  "GRIT.CV",      "GRIT",     0 },
    { "Summer 200",      MC_CUSTOM,   0,  0,  1,  0,  0,  0,  1,  "SUMMER.CV",    "SUMMER",   0 },
    { "Chrome 64",       MC_CUSTOM,   0,  1,  1,  1,  0,  0,  0,  "CHROME.CV",    "CHROME",   0 },
    { "Bypass 400",      MC_CUSTOM,   0,  1, -2,  0,  0,  0,  0,  "BYPASS.CV",    "BYPASS",   0 },
    { "Noir 3200",       MC_BW,       0,  0,  0,  0,  0,  0,  0,  "NOIR.CV",      "NOIR",     0 },
    { "Turquoise 400",   MC_CUSTOM,   0,  0,  1,  0,  0,  0,  0,  "TURQUOI.CV",   "TURQUOI",  3 },
    { "Infra 200",       MC_CUSTOM,   0,  0,  1,  0,  0,  0,  0,  "INFRA.CV",     "INFRA",    4 },
    { "Tintype 50",      MC_SEPIA,    0,  0,  0,  0,  0,  0,  0,  "TINTYPE.CV",   "TINTYPE",  0 },
    { "Cyano 25",        MC_CUSTOM,   0,  0,  0,  0,  0,  0,  0,  "CYANO.CV",     "CYANO",    5 },
    { "Instant 600",     MC_CUSTOM,  -1, -1, -1,  0,  0,  0,  0,  "INSTANT.CV",   "INSTANT",  0 },
    { "Pastel 400",      MC_CUSTOM,  -1, -1, -1,  0,  0,  0,  1,  "PASTEL.CV",    "PASTEL",   0 },
};
#define NUM_BUILTIN ((int)(sizeof(sims_builtin)/sizeof(sims_builtin[0])))

// Aktive Tabelle: eingebaut oder aus FILMS.TXT (Eintrag 0 ist immer "Aus")
static const filmsim_t *sims = sims_builtin;
static int num_sims = NUM_BUILTIN;
#define NUM_SIMS num_sims

// Preset IDs stay distinct from FILMS.TXT indices across reboots.
#define PRESET_ID_OFFSET (FILMS_TXT_MAX + 1)
static const filmsim_t *preset_sims;
static int preset_count;

static const filmsim_t *film_at(int id)
{
    if (id >= 0 && id < NUM_SIMS)
        return &sims[id];
    if (preset_sims && id > PRESET_ID_OFFSET && id - PRESET_ID_OFFSET <= preset_count)
        return &preset_sims[id - PRESET_ID_OFFSET];
    return NULL;
}

// Anzeigename; "Aus" kommt aus der Sprachdatei (english.lng / german.lng)
static const char* sim_name(int i)
{
    const filmsim_t *film = film_at(i);
    return i && film ? film->name : lang_str(LANG_EXCCD_OFF);
}

#include "gui_osd.h"

// Nur im Foto-Aufnahmemodus (nicht Video, nicht Wiedergabe)
int filmsim_photo_mode(void)
{
    return camera_info.state.mode_rec && !camera_info.state.mode_video && !camera_info.state.mode_play;
}

// The film picker needs the still-photo palette.
// Nicht bei offenem Canon-Menue / FUNC-Menue (andere Bildschirmpalette)
static volatile int photo_since = 0;   // seit wann ununterbrochen Fotomodus (Tick)

int filmsim_picker_allowed(void)
{
    if (!filmsim_photo_mode()) return 0;
    // direkt nach dem Einschalten meldet die Kamera kurzzeitig falsche
    // Zustaende -> erst oeffnen, wenn seit >0,7 s stabil Fotomodus
    // und die Kamera schon >3 s laeuft
    int now = get_tick_count();
    if (now < 3000 || !photo_since || now - photo_since < 700) return 0;
    if (canon_menu_active != (int)&canon_menu_active - 4) return 0;
    if (canon_shoot_menu_active != 0) return 0;
    return 1;
}

//-------------------------------------------------------------------
// Gesamtes CHDK-OSD (Statuszeile, Gitter, Batterie ...) ausblenden, solange
// der Ausloeser halb gedrueckt oder das Canon-MENU / FUNC.SET-Menue offen ist.
//
// FILMSIM_OSD_KEYTRACK 1: Rueckfallebene, falls canon_shoot_menu_active auf
// der IXUS 60 nicht zuverlaessig ist - Menues ueber die Tasten verfolgen
// (wie Binaer-Patch v1.9.4).
//-------------------------------------------------------------------
#ifndef FILMSIM_OSD_KEYTRACK
#define FILMSIM_OSD_KEYTRACK 0
#endif

#if FILMSIM_OSD_KEYTRACK
static int canon_menus_open(void)
{
    static int func_open = 0, menu_open = 0, set_prev = 0, menu_prev = 0;
    if (!camera_info.state.mode_rec) {
        func_open = menu_open = 0;
        return 0;
    }
    int m = kbd_is_key_pressed(KEY_MENU) != 0;
    if (m && !menu_prev) { menu_open ^= 1; func_open = 0; }
    menu_prev = m;
    int s = kbd_is_key_pressed(KEY_SET) != 0;
    if (s && !set_prev && !menu_open) func_open ^= 1;  // im MENU ist SET normale Auswahl
    set_prev = s;
    if (camera_info.state.is_shutter_half_press) func_open = menu_open = 0;
    return func_open || menu_open;
}
#else
static int canon_menus_open(void)
{
    return canon_menu_active != (int)&canon_menu_active - 4 || canon_shoot_menu_active != 0;
}
#endif

// Canon zeigt seine Aufnahme-Symbole nach dem Schliessen von FUNC.SET/MENU,
// nach dem Einschalten bzw. Wechsel in den Aufnahmemodus und nach den
// Ringtasten (Blitz, Makro, Bildfolge ...) noch einige Sekunden
// -> so lange noch nichts zeichnen
#ifndef FILMSIM_MENU_HOLD_MS
#define FILMSIM_MENU_HOLD_MS 6000
#endif
// nach dem Loslassen des halb gedrueckten Ausloesers
#ifndef FILMSIM_HALF_HOLD_MS
#define FILMSIM_HALF_HOLD_MS 2000
#endif

int filmsim_osd_hidden(void)
{
    static int was_hidden = 0, was_menus = 0, was_rec = 0, was_half = 0;
    static int hold_until = 0;
    int now = get_tick_count();
    int menus = canon_menus_open();
    int rec = camera_info.state.mode_rec;
    int half = camera_info.state.is_shutter_half_press;
    int ring = rec && (kbd_is_key_pressed(KEY_UP) || kbd_is_key_pressed(KEY_DOWN)
                    || kbd_is_key_pressed(KEY_LEFT) || kbd_is_key_pressed(KEY_RIGHT));
    if ((was_menus && !menus) || (rec && !was_rec) || ring)
        hold_until = now + FILMSIM_MENU_HOLD_MS;
    else if (was_half && !half && hold_until - now < FILMSIM_HALF_HOLD_MS)
        hold_until = now + FILMSIM_HALF_HOLD_MS;
    was_menus = menus;
    was_rec = rec;
    was_half = half;
    int hidden = menus || half || hold_until - now > 0;
    if (hidden && !was_hidden) gui_set_need_restore();   // Reste wegwischen
    was_hidden = hidden;
    return hidden;
}

//-------------------------------------------------------------------
// Filmtabelle aus A/CHDK/FILMS/FILMS.TXT (einmalig nach dem Start).
// Fehlt die Datei oder enthaelt sie keinen gueltigen Look, bleibt die
// eingebaute Tabelle aktiv. Fehlerhafte Zeilen -> A/CHDK/FILMS/FILMS.LOG
//-------------------------------------------------------------------
#define FILMS_TXT   "A/CHDK/FILMS/FILMS.TXT"
#define FILMS_LOG   "A/CHDK/FILMS/FILMS.LOG"
#define FILMS_BAK   "A/CHDK/FILMS/FILMS.BAK"
#define PRESETS_LOG "A/CHDK/FILMS/PRESETS.LOG"
#define FILMS_LOG_SIZE 1024

static filmsim_t *films_read_file(const char *path, const char *log_path, int *count)
{
    struct stat st;
    filmsim_t *films = NULL;
    int n = 0, len = -1;

    if (stat(path, &st) != 0) {
        remove(log_path);
        return NULL;
    }
    char *log = malloc(FILMS_LOG_SIZE);
    if (!log) return NULL;
    log[0] = 0;
    char *buf = NULL;
    if (st.st_size > 0 && st.st_size <= FILMS_TXT_FILE_MAX)
        buf = malloc(st.st_size + 1);
    if (buf) {
        int fd = open(path, O_RDONLY, 0777);
        if (fd >= 0) {
            len = read(fd, buf, st.st_size);
            close(fd);
        }
        if (len > 0) {
            films = malloc((1 + FILMS_TXT_MAX) * sizeof(filmsim_t) + FILMS_TXT_MAX * sizeof(films_txt_str_t));
            if (films) {
                films_txt_str_t *strings = (films_txt_str_t *)(films + 1 + FILMS_TXT_MAX);
                n = films_txt_parse(buf, len, films + 1, strings, FILMS_TXT_MAX, log, FILMS_LOG_SIZE - 80);
            }
        }
        free(buf);
    }

    if (n > 0) {
        films[0] = sims_builtin[0];
        *count = n;
    } else {
        strcat(log, "No valid looks could be loaded\r\n");
        free(films);
        films = NULL;
    }

    if (log[0]) {
        char hdr[48];
        sprintf(hdr, "%s: %d looks loaded\r\n", strrchr(path, '/') + 1, n);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        if (fd >= 0) {
            write(fd, hdr, strlen(hdr));
            write(fd, log, strlen(log));
            close(fd);
        }
    } else {
        remove(log_path);
    }
    free(log);
    return films;
}

static int films_load_presets(void)
{
    if (!preset_sims)
        preset_sims = films_read_file(FILMS_BAK, PRESETS_LOG, &preset_count);
    return preset_sims != NULL;
}

static int film_find(int offset, int count, const char *name)
{
    for (int i = 1; i <= count; i++) {
        if (strcmp(film_at(offset + i)->name, name) == 0)
            return offset + i;
    }
    return 0;
}

// Der Look wird ueber seinen Namen gespeichert: Mit einer anderen FILMS.TXT oder
// neuer Reihenfolge bleibt so derselbe Film gewaehlt, statt dass der Index auf einen
// anderen zeigt. Gibt es ihn nicht mehr: "Aus". Aeltere Einstellungen ohne Namen
// behalten den Index und bekommen hier den Namen.
static void film_name_store(void)
{
    const filmsim_t *film = film_at(conf.film_sim);
    conf.film_name[0] = 0;
    if (conf.film_sim && film) {
        strncpy(conf.film_name, film->name, sizeof(conf.film_name) - 1);
        conf.film_name[sizeof(conf.film_name) - 1] = 0;
    }
}

static void films_load(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    int count = 0;
    const filmsim_t *full = films_read_file(FILMS_TXT, FILMS_LOG, &count);
    if (full) {
        sims = full;
        num_sims = count + 1;
    }
    int from_preset = conf.film_sim > PRESET_ID_OFFSET && films_load_presets();
    if (!conf.film_name[0]) {
        if (!film_at(conf.film_sim)) conf.film_sim = 0;
    } else {
        int id = from_preset ? film_find(PRESET_ID_OFFSET, preset_count, conf.film_name) : 0;
        conf.film_sim = id ? id : film_find(0, NUM_SIMS - 1, conf.film_name);
    }
    film_name_store();
}

static const filmsim_t* current(void)
{
    const filmsim_t *film = film_at(conf.film_sim);
    return film ? film : &sims[0];
}

const char* filmsim_name(void)
{
    int i = conf.film_sim;
    return sim_name(i);
}

// Schwarzweiss im Sinne des Farbfilters: S/W-Filme und Tintype (Sepia, auch einfarbig)
static int bw_look(const filmsim_t *s)
{
    return s->mycolors == MC_BW || s->mycolors == MC_SEPIA;
}

static int curve_file_ok = 0;     // Diagnose: Kurvendatei gefunden?
static int curve_dr = 0;          // Dynamik+-Stufe der geladenen Kurve
static volatile int shot_dr = -1; // Dynamik+-Stufe, mit der wirklich belichtet wurde (-1 = unbekannt)
static int set_count = 0;         // Diagnose: wie oft Propcases gesetzt

// Schritte, die bei diesem Foto mangels Speicher ausgefallen sind. Das Foto
// sieht dann anders aus als gewaehlt: FILMLOG.CSV nennt die Schritte, die
// Statuszeile zeigt "low mem!", bis ein Foto wieder vollstaendig klappt.
#define SKIP_HOT   1
#define SKIP_DBL   2
#define SKIP_CNR   4
#define SKIP_CURVE 8
#define SKIP_FX    16
#define SKIP_GRAIN 32
static int shot_skip = 0;         // dieses Foto (im RAW-Hook gesammelt)
static int shot_skip_last = 0;    // letztes Foto, fuer die Statuszeile

// Canon uebernimmt neue Farbwerte im Sucherbild erst beim naechsten
// Halbdruck -> bis dahin Hinweis anzeigen
static int preview_pending = 0;

// Canon-Propcases setzen
static void set_canon_props(const filmsim_t *s)
{
    set_count++;
#ifdef FS_NO_MYCOLORS
    (void)s;
#else
    shooting_set_prop(PROPCASE_MY_COLORS, s->mycolors);
    if (s->mycolors == MC_CUSTOM) {
        shooting_set_prop(PROPCASE_CUSTOM_CONTRAST,   s->contrast);
        shooting_set_prop(PROPCASE_CUSTOM_SHARPNESS,  s->sharpness);
        shooting_set_prop(PROPCASE_CUSTOM_SATURATION, s->saturation);
        shooting_set_prop(PROPCASE_CUSTOM_RED,        s->red);
        shooting_set_prop(PROPCASE_CUSTOM_GREEN,      s->green);
        shooting_set_prop(PROPCASE_CUSTOM_BLUE,       s->blue);
        shooting_set_prop(PROPCASE_CUSTOM_SKIN_TONE,  s->skin);
    }
#endif
}

static int canon_props_match(const filmsim_t *s)
{
#ifdef FS_NO_MYCOLORS
    (void)s;
    return 1;
#else
    if (shooting_get_prop(PROPCASE_MY_COLORS) != s->mycolors) return 0;
    if (s->mycolors != MC_CUSTOM) return 1;
    return shooting_get_prop(PROPCASE_CUSTOM_CONTRAST)   == s->contrast
        && shooting_get_prop(PROPCASE_CUSTOM_SHARPNESS)  == s->sharpness
        && shooting_get_prop(PROPCASE_CUSTOM_SATURATION) == s->saturation
        && shooting_get_prop(PROPCASE_CUSTOM_RED)        == s->red
        && shooting_get_prop(PROPCASE_CUSTOM_GREEN)      == s->green
        && shooting_get_prop(PROPCASE_CUSTOM_BLUE)       == s->blue
        && shooting_get_prop(PROPCASE_CUSTOM_SKIN_TONE)  == s->skin;
#endif
}

//-------------------------------------------------------------------
// Weissabgleich-Verschiebung (conf.film_wb_r / _b, -9..+9)
//   Wirkt wie auf der frueheren Webseite (site/curves.js, wbGains): je Stufe
//   Faktor 1,03 auf R bzw. B. Kein Faktor liegt ueber 1 - angehoben wird
//   relativ, indem die anderen Kanaele sinken, damit Lichter nicht frueher
//   ausfressen. Die Faktoren kommen beim Laden in die Tonkurve, kosten also
//   keine Rechenzeit. Ohne Filmkurve gilt eine neutrale Kurve. Bei "Aus"
//   bleibt alles bei Canon.
//   Die Stufe ist vorlaeufig und noch nicht gegen Referenzaufnahmen kalibriert.
//-------------------------------------------------------------------
static const short wb_step[RECIPE_SHIFT_MAX + 1] = { 4096, 4219, 4345, 4476, 4610, 4748, 4891, 5038, 5189, 5344 };

static int wb_shift_clamp(int v)
{
    return v < -RECIPE_SHIFT_MAX ? -RECIPE_SHIFT_MAX : (v > RECIPE_SHIFT_MAX ? RECIPE_SHIFT_MAX : v);
}

static int wb_shift_active(void)
{
    return conf.film_sim != 0 && (conf.film_wb_r || conf.film_wb_b);
}

// Faktoren R, G, B (4096 = 1)
static void wb_shift_gains(int *g)
{
    g[0] = g[1] = g[2] = 4096;
    if (!wb_shift_active()) return;
    int r = wb_shift_clamp(conf.film_wb_r), b = wb_shift_clamp(conf.film_wb_b);
    int kr = r >= 0 ? wb_step[r] : 4096 * 4096 / wb_step[-r];
    int kb = b >= 0 ? wb_step[b] : 4096 * 4096 / wb_step[-b];
    int top = kr > kb ? kr : kb;
    if (top < 4096) top = 4096;
    g[0] = kr * 4096 / top;
    g[1] = 4096 * 4096 / top;
    g[2] = kb * 4096 / top;
}

// Pseudo-Datei fuer "nur Verschiebung" (gibt es nicht, der Lader nimmt dann eine neutrale Kurve)
#define CURVE_WB_ONLY "WBSHIFT.CV"

//-------------------------------------------------------------------
// Eigene Tonkurven fuer 12-Bit-Sensoren
//   curves.flt kann nur 10 Bit (CHDK schaltet OPT_CURVES bei 12 Bit ab).
//   Die .CV-Dateien bleiben dieselben (4 x 1024 Werte, 10 Bit: R, G der
//   R-Zeile, G der B-Zeile, B). Beim Laden entsteht daraus eine Tabelle
//   mit 3 x 4096 Eintraegen (R, G, B), linear zwischen den 10-Bit-Stuetzstellen
//   interpoliert (Schwarz 127 / Weiss 4095 liegen exakt auf 31 / 1023).
//   Das Anwenden ist dann nur noch ein Tabellenzugriff je Pixel.
//-------------------------------------------------------------------
#if defined(FS_RAW) && CAM_SENSOR_BITS_PER_PIXEL == 12
#define FS_OWN_CURVES 1
// Die Tabelle (24 KB) wird einmal angelegt und bleibt: Beim Blitzen mit
// Dynamik+ laedt raw.c mitten in der Aufnahme eine andere Kurve, dort darf
// kein grosses malloc mehr scheitern. Die Datei wird kanalweise ueber einen
// 2-KB-Puffer gelesen (malloc, nicht static: Groessenlimit des SX110-Kerns).
// curve_lut_ok = 0: keine gueltige Kurve, nichts anwenden.
static unsigned short *curve_lut = 0;
static int curve_lut_ok = 0;
static int curve_in_hook = 0;      // 1 waehrend filmsim_dr_curve_hook: Tabelle behalten
static int curve_nomem = 0;        // 1: Kurve gewuenscht, aber kein Speicher fuer die Tabelle

// Ohne Kurve (Menue) wird der Speicher frei, im RAW-Hook bleibt er belegt
static void curve_lut_free(void)
{
    curve_lut_ok = 0;
    if (!conf.curve_enable) curve_nomem = 0;
    if (!curve_in_hook && curve_lut) {
        free(curve_lut);
        curve_lut = 0;
    }
}

static int curve_lut_load(const char *fn)
{
    int g[3], ch, v, fd, ok = 1;
    curve_lut_ok = 0;
    if (!curve_lut) curve_lut = malloc(3 * 4096 * sizeof(unsigned short));
    unsigned short *cv = curve_lut ? malloc(CV_N * sizeof(unsigned short)) : 0;
    curve_nomem = !cv;
    if (!cv) return 0;
    wb_shift_gains(g);
    int wb = wb_shift_active();
    fd = open(fn, O_RDONLY, 0777);
    if (fd < 0 && !wb) { free(cv); return 0; }
    // Datei: R, G (R-Zeile), G (B-Zeile), B. Die zweite G-Kurve wird nur ueberlesen.
    for (ch = 0; ch < 4 && ok; ch++) {
        if (fd >= 0) {
            ok = read(fd, cv, CV_N * sizeof(unsigned short)) == CV_N * (int)sizeof(unsigned short);
        } else {
            for (v = 0; v < CV_N; v++) cv[v] = v;   // neutral, nur Verschiebung
        }
        if (!ok || ch == 2) continue;
        if (wb) {
            // Werte ueber Schwarz (31) skalieren
            int k = g[ch == 0 ? 0 : (ch == 3 ? 2 : 1)];
            for (v = 0; v < CV_N; v++)
                if (cv[v] > 31) cv[v] = 31 + (((cv[v] - 31) * k + 2048) >> 12);
        }
        curve_lut_build_ch(cv, curve_lut + (ch == 3 ? 2 : ch) * 4096);
    }
    if (fd >= 0) close(fd);
    free(cv);
    curve_lut_ok = ok;
    return ok;
}

// Aus raw.c, an der Stelle von curves.flt
__attribute__((optimize("O2"))) void filmsim_curve_apply(char *raw)
{
    if (!curve_lut_ok || !raw) {
        if (curve_nomem && raw) shot_skip |= SKIP_CURVE;
        return;
    }
    int rows = camera_sensor.raw_rows, rowpix = camera_sensor.raw_rowpix;
    int rowlen = camera_sensor.raw_rowlen, y;
    // Wortweise nur bei ausgerichteten Zeilen aus ganzen 8-Pixel-Gruppen
    int words = !(((unsigned int)raw | (unsigned int)rowlen) & 3) && !(rowpix & 7);
    for (y = 0; y < rows; y++) {
        prog_rows(y, rows);
        // R-Zeile: Tabelle R (gerade Spalten) / G, B-Zeile: G / B
        const unsigned short *ce = curve_lut + ((y ^ FS_ROW0) & 1) * 4096;
        const unsigned short *co = ce + 4096;
        unsigned char *a = (unsigned char *)raw + y * rowlen;
        if (words) curve_row12_w((unsigned int *)a, rowpix / 8, ce, co);
        else       curve_row12(a, rowpix / 4, ce, co);
    }
}
#endif

// Kurve laden bzw. abschalten. dr: Dynamik+-Stufe (0..2), deren Kurve gilt
static void set_curve_dr(const filmsim_t *s, int dr)
{
#if defined(OPT_CURVES) || defined(FS_OWN_CURVES)
    curve_dr = dr;
    if (s->curve || dr || wb_shift_active()) {
        struct stat st;
        strcpy(conf.curve_file, CURVE_DIR "/");
        if (!s->curve && !dr) {
            strcat(conf.curve_file, CURVE_WB_ONLY);                    // nur WB-Verschiebung
        } else if (!s->curve) {
            strcat(conf.curve_file, dr == 1 ? "DR1.CV" : "DR2.CV");    // nur Dynamik+
        } else if (dr) {
            // POSTCRD.CV -> POSTCRD1.CV / POSTCRD2.CV
            char *e = conf.curve_file + strlen(conf.curve_file);
            strcat(conf.curve_file, s->curve);
            char *dot = strrchr(e, '.');
            if (dot) *dot = 0;
            strcat(conf.curve_file, dr == 1 ? "1.CV" : "2.CV");
        } else {
            strcat(conf.curve_file, s->curve);
        }
        curve_file_ok = (stat(conf.curve_file, &st) == 0 || (!s->curve && !dr)) ? 1 : -1;
        conf.curve_enable = 1;
    } else {
        conf.curve_enable = 0;
        curve_file_ok = 0;
    }
#ifdef FS_OWN_CURVES
    if (!conf.curve_enable || !curve_lut_load(conf.curve_file)) {
        curve_lut_free();
        if (conf.curve_enable) curve_file_ok = -1;
    }
    conf.curve_enable = 0;           // curves.flt gibt es hier nicht
#else
    if (conf.curve_enable) {
        int g[3];
        wb_shift_gains(g);
        libcurves->curve_set_gains(g[0], g[1], g[2]);
    }
    libcurves->curve_init_mode();
#endif
#else
    (void)s; (void)dr;
#endif
}

static void set_curve(const filmsim_t *s)
{
    set_curve_dr(s, (conf.film_dr >= 1 && conf.film_dr <= 2) ? conf.film_dr : 0);
}

// Simulation vollstaendig anwenden (bei Menueauswahl / Start)
void filmsim_apply(void)
{
    const filmsim_t *s = current();
    if (filmsim_photo_mode())
#ifdef FILMSIM_JPEG_PAIR
        set_canon_props(pair_use_original() ? &sims[0] : s);
#else
        set_canon_props(s);
#endif
    set_curve(s);
}

// RAW entwickeln (Ablauf siehe filmsim_develop_select)
static volatile int dev_armed = 0;      // das naechste Foto ist der Hilfsschuss
static volatile int dev_active = 0;     // der Hilfsschuss laeuft gerade durch den RAW-Hook
static volatile int dev_restore = 0;    // fertig: eingelegten Film wieder herstellen
static int dev_prev = -1;               // eingelegter Film
static int dev_has_meta = 0;            // 0: reines RAW ohne Aufnahmedaten
static dng_meta_t dev_meta;
static char dev_name[16];               // Dateiname fuer Statuszeile und Logbuch
static char dev_path[64];               // ganze DNG, fuer jeden Hilfsschuss einer Serie
static int dev_list_open(void);         // Looks ankreuzen (Serie, am Dateiende)
static void dev_pick_only(int i);
static volatile int batch_on;           // Serie laeuft (filmsim_batch_kbd)
static int batch_i, batch_n;            // laufender Look / Anzahl
static void dev_fx_init(void);          // Effekte fuer diese Entwicklung (am Dateiende)
// Belichtung in 1/3 EV (-6..+6) und Farbtemperatur in Stufen (-5 kuehler .. +5 waermer),
// nur fuer diese Entwicklung (Menue nach den Effekten, angewandt in filmsim_develop_begin)
static int dev_ev = 0, dev_temp = 0;
// Toenung in Stufen (-5 magenta .. +5 gruen), Dynamik+ (0 wie bei der Aufnahme, 1 aus, 2 = +1, 3 = +2)
static int dev_tint = 0, dev_dr = 0;
// Weissabgleich (Aufnahme statt Hilfsschuss), Temperatur und Toenung fuer filmsim_develop_end (*4096),
// dazu das Grau von Canon beim Hilfsschuss (Rohwert R bzw. B je G)
static int dev_end_fr = 4096, dev_end_fg = 4096, dev_end_fb = 4096, dev_end_nr = 4096, dev_end_nb = 4096;
static const char* dev_ev_enum(int change, int arg);
static const char* dev_temp_enum(int change, int arg);
static const char* dev_tint_enum(int change, int arg);
static void dev_fx_menu_open(void);
static int dev_fx_any(void);
static void recipes_load(void);         // Rezepte (weiter unten)
static void look_text_update(void);
static void recipe_osd(char *rc, char *wb);
static int recipe_active(void);
static void perf_tick(void);

// Aus filmsim_tick: Kamera im Fotomodus, keine Menues, kein Bild in Arbeit.
// Canon ist mit dem JPEG fertig, erst jetzt den Film zuruecknehmen
static void dev_tick(void)
{
    if (!dev_restore || camera_info.state.is_shutter_half_press) return;
    dev_restore = 0;
    if (dev_prev >= 0 && dev_prev != conf.film_sim) filmsim_set(dev_prev);
    dev_prev = -1;
}

//-------------------------------------------------------------------
// RAW entwickeln: eine gespeicherte DNG mit einem anderen Look neu ausgeben
//   Canon erzeugt JPEGs nur nach einer Aufnahme. Deshalb: DNG und Look waehlen, einmal
//   ausloesen. raw.c ersetzt die Sensordaten dieses Hilfsschusses durch die DNG, danach laeuft
//   die normale Kette, und das Original bleibt erhalten. Der eingelegte Film
//   aendert sich dabei nicht.
//
//   Vom Hilfsschuss stammen sonst nur Canons Einstellungen. Deshalb wird korrigiert:
//     Weissabgleich:  Canon rechnet mit den Faktoren des Hilfsschusses. Die Rohdaten werden so
//                     umgerechnet, dass im JPEG die Faktoren der Aufnahme herauskommen.
//     Dynamik+:       die Aufhell-Kurve gehoert zur Aufnahme, nicht zur Menueeinstellung
//     Farbrauschen:   Auto richtet sich nach der ISO der Aufnahme
//     Datum:          der Datumsstempel zeigt den Aufnahmetag
//   JPEGs uebernehmen die ISO aus dem DNG; die anderen Exif-Daten kommen vom Hilfsschuss.
//-------------------------------------------------------------------
int filmsim_develop_armed(void)
{
    return dev_armed;
}

int filmsim_develop_iso(void)
{
    return dev_armed && dev_has_meta ? dev_meta.iso : 0;
}

// Passt die Datei zu dieser Kamera? Liest auch die Aufnahmedaten.
static int dev_read_file(const char *fn)
{
    struct stat st;
    int ok = 0;
    if (strlen(fn) >= 64 || stat(fn, &st) != 0 || st.st_size < (int)camera_sensor.raw_size) return 0;
    dev_has_meta = 0;
    if (st.st_size == (int)camera_sensor.raw_size) return 1;    // CHDK-RAW ohne Kopf
    unsigned char *head = malloc(DNG_HEAD_MAX);
    if (!head) return 0;
    int fd = open(fn, O_RDONLY, 0777);
    if (fd >= 0) {
        int n = read(fd, head, DNG_HEAD_MAX);
        close(fd);
        ok = dng_parse(head, n, camera_sensor.raw_rowpix, camera_sensor.raw_rows, camera_sensor.bits_per_pixel, &dev_meta);
        dev_has_meta = ok;
    }
    free(head);
    return ok;
}

// Rueckruf der Dateiauswahl: Datei merken, dann die Filmauswahl zum Waehlen des Looks
void filmsim_develop_select(const char *fn)
{
    if (!fn) return;                    // abgebrochen
    if (!dev_read_file(fn)) {
        gui_mbox_init(LANG_MENU_RAW_DEVELOP, LANG_EXCCD_DEV_BAD, MBOX_FUNC_RESTORE|MBOX_TEXT_CENTER|MBOX_BTN_OK, NULL);
        return;
    }
    const char *b = strrchr(fn, '/');
    strncpy(dev_name, b ? b + 1 : fn, sizeof(dev_name) - 1);
    dev_name[sizeof(dev_name) - 1] = 0;
    strcpy(dev_path, fn);               // dev_read_file: kuerzer als 64
    raw_prepare_develop(fn, 0);
    // Ruecknahme der letzten Entwicklung noch offen: Dann ist dev_prev weiter der eingelegte Film
    if (!dev_restore || dev_prev < 0) dev_prev = conf.film_sim;
    dev_restore = 0;
    dev_armed = 1;
    dev_fx_init();
    dev_ev = dev_temp = dev_tint = dev_dr = 0;
    if (!dev_list_open()) filmsim_open_menu();  // ohne Speicher: Laschen, ein Look
}

void filmsim_develop_cancel(void)
{
    batch_on = 0;
    raw_prepare_develop(0, 0);
    dev_armed = 0;
    dev_restore = 1;                    // uebernimmt dev_tick
}

// Aus raw.c: Die Datei ist geladen, gleich laeuft die Kette
void filmsim_develop_begin(char *raw)
{
    dev_armed = 0;
    dev_active = 1;
    shot_dr = dev_dr ? dev_dr - 1 : (dev_has_meta ? dev_meta.dr : 0);
#ifdef FS_RAW
    // Ablauf: Die Film-Kette (Kurve, Mischung, Effekte) laeuft auf den Rohdaten wie aufgenommen,
    // nur die Belichtung kommt vorher (wie eine hellere Aufnahme, z. B. fuer die Halation-Schwelle).
    // Weissabgleich, Temperatur und Toenung kommen erst danach (filmsim_develop_end). Sonst legen
    // Kurve und Effekte ihre Farben in schon umgerechnete Daten, und z. B. der rote Glow der
    // Halation kaeme bei einem Hilfsschuss unter warmem Licht kaum rot heraus.
    // Belichtung: 2^(n/3) * 4096. Farbtemperatur: je Stufe R * 1,07 und B / 1,07 (waermer)
    static const short ev_gain[13] = { 1024, 1290, 1625, 2048, 2580, 3251, 4096, 5161, 6502, 8192, 10321, 13004, 16384 };
    static const short temp_gain[6] = { 4096, 4383, 4690, 5018, 5369, 5745 };
    // Toenung: je Stufe G * 1,05 und R, B / Wurzel(1,05) (gruener), die Helligkeit bleibt etwa
    static const short tint_g[6]  = { 4096, 4301, 4516, 4742, 4979, 5228 };
    static const short tint_rb[6] = { 4096, 3997, 3901, 3807, 3715, 3626 };
    dev_end_fr = dev_end_fg = dev_end_fb = WB_ONE;
    if (raw) {
        int wb[3] = { 0, 0, 0 };        // G, R, B wie in modules/dng.c
        get_property_case(camera_info.props.wb_adj, &wb, sizeof(wb));
        int fr = WB_ONE, fb = WB_ONE;
        if (dev_has_meta) {
            fr = wb_factor(dev_meta.wb_r, dev_meta.wb_g, wb[1], wb[0]);
            fb = wb_factor(dev_meta.wb_b, dev_meta.wb_g, wb[2], wb[0]);
        }
        int ev = dev_ev < -6 ? -6 : dev_ev > 6 ? 6 : dev_ev;
        int t = dev_temp < -5 ? -5 : dev_temp > 5 ? 5 : dev_temp;
        int eg = ev_gain[ev + 6], tg = temp_gain[t < 0 ? -t : t];
        int tr = t >= 0 ? tg : WB_ONE * WB_ONE / tg, tb = WB_ONE * WB_ONE / tr;
        int u = dev_tint < -5 ? -5 : dev_tint > 5 ? 5 : dev_tint, ua = u < 0 ? -u : u;
        int ug = u >= 0 ? tint_g[ua] : WB_ONE * WB_ONE / tint_g[ua];
        int urb = u >= 0 ? tint_rb[ua] : WB_ONE * WB_ONE / tint_rb[ua];
        // fuer filmsim_develop_end; hoechstens 16384 * 5745 bzw. * 4627: passt in 32 Bit
        dev_end_fr = ((fr * tr) >> 12) * urb >> 12;
        dev_end_fb = ((fb * tb) >> 12) * urb >> 12;
        dev_end_fg = ug;
        // Rohwert R bzw. B je G, den Canon mit dem Weissabgleich des Hilfsschusses grau ausgibt
        dev_end_nr = (wb[0] > 0 && wb[1] > 0) ? wb[0] * WB_ONE / wb[1] : WB_ONE * 256 / FILMSIM_WB_R;
        dev_end_nb = (wb[0] > 0 && wb[2] > 0) ? wb[0] * WB_ONE / wb[2] : WB_ONE * 256 / FILMSIM_WB_B;
        if (eg != WB_ONE) {
            // Grau der Aufnahme (AsShotNeutral), damit ausgefressene Stellen neutral bleiben
            int nr = (dev_has_meta && dev_meta.wb_r > 0) ? dev_meta.wb_g * WB_ONE / dev_meta.wb_r : WB_ONE * 256 / FILMSIM_WB_R;
            int nb = (dev_has_meta && dev_meta.wb_b > 0) ? dev_meta.wb_g * WB_ONE / dev_meta.wb_b : WB_ONE * 256 / FILMSIM_WB_B;
            raw_gain((unsigned char *)raw, camera_sensor.raw_rows, camera_sensor.raw_rowpix, camera_sensor.raw_rowlen,
                     FS_ROW0, 31, eg, eg, eg, 1000, nr, nb);
        }
    }
#else
    (void)raw;
#endif
}

// Aus raw.c nach dem Speichern der DNG: Dynamik+-Stufe in den Kopf schreiben, damit die spaetere
// Entwicklung die passende Kurve nimmt. Rueckgabe: offene Datei fuer raw_closefile, sonst -1.
// Die Datei behaelt ihre Groesse (die Rohdaten muessen am Dateiende bleiben).
int filmsim_dng_mark(const char *fn)
{
    if (shot_dr < 1 || shot_dr > 2) return -1;
    unsigned char *head = malloc(DNG_HEAD_MAX);
    if (!head) return -1;
    int fd = open(fn, O_RDWR, 0777);
    if (fd >= 0) {
        int n = read(fd, head, DNG_HEAD_MAX);
        int e = n > 8 ? dng_entry(head, n, dng_u32(head + 4), DNG_TAG_DESCRIPTION) : -1;
        if (e >= 0) {
            unsigned char v[8] = { 4, 0, 0, 0, 'D', 'R', (unsigned char)('0' + shot_dr), 0 };
            lseek(fd, e + 4, 0);        // SEEK_SET: Anzahl und Text direkt im Eintrag
            write(fd, v, sizeof(v));
        }
    }
    free(head);
    return fd;
}

// Wird ca. alle 20 ms aus der Hauptschleife aufgerufen
static gui_handler filmGuiHandler, wbgGuiHandler, devFxGuiHandler;
#ifdef FILMSIM_QUICK_BUTTONS
static int quick_film_mode; // Zero for a picker opened from the CHDK menu.
#endif

void filmsim_tick(void)
{
    static int cnt = 0;
    static int curve_done = 0;

#ifdef FILMSIM_JPEG_PAIR
    pair_tick();
#endif
    perf_tick();

    if (camera_info.state.is_shutter_half_press)
        preview_pending = 0;         // Canon hat die Werte jetzt uebernommen

    if (filmsim_photo_mode()) {
        if (!photo_since) photo_since = get_tick_count() | 1;
    } else {
        photo_since = 0;
    }

    // Only the photo-specific EXCCD screens need to close on a mode change.
    if (!filmsim_photo_mode() && (gui_is_mode(&filmGuiHandler)
        || gui_is_mode(&wbgGuiHandler) || gui_is_mode(&devFxGuiHandler))) {
        exit_alt();
    }
#ifdef FILMSIM_QUICK_BUTTONS
    else if (quick_film_mode && camera_info.state.mode_shooting != quick_film_mode
             && gui_is_mode(&filmGuiHandler)) {
        exit_alt();
    }
#endif

    filmsim_zoom_tick();

    if (++cnt < 25) return;          // ~ alle 0,5 s pruefen
    cnt = 0;

    if (!curve_done) {               // einmalig nach dem Start Kurve laden
        films_load();
        // Index aus aelterer Version / kuerzerer FILMS.TXT -> "Aus"
        if (!film_at(conf.film_sim)) conf.film_sim = 0;
        set_curve(current());
        recipes_load();
        look_text_update();
        curve_done = 1;
    }

    if (!filmsim_photo_mode()) return;
    if (!camera_info.state.gui_mode_none) return;   // nicht in Menues eingreifen
    if (camera_info.state.state_shooting_progress == SHOOTING_PROGRESS_PROCESSING) return;
    dev_tick();

    // Altlast aus v0.9: von der Koernung erzwungene ISO 800 zuruecknehmen
    if (conf.film_iso_prev >= 0) {
        extern short shooting_get_canon_iso_mode();
        if (shooting_get_canon_iso_mode() == 800)
            shooting_set_iso_mode(conf.film_iso_prev ? conf.film_iso_prev : 0);
        conf.film_iso_prev = -1;
        conf.film_grain = 0;
    }
    if (conf.film_sim == 0) return;  // "Aus": Canon-Farben nicht anfassen

    const filmsim_t *s = current();
#ifdef FILMSIM_JPEG_PAIR
    if (pair_use_original()) s = &sims[0];
#endif
    if (!canon_props_match(s))
        set_canon_props(s);
}

// Film auswaehlen und sofort anwenden
// Menue-Eintrag "Look: Summer 200" (Text des Untermenues, folgt dem eingelegten Film)
char filmsim_look_text[40];
static void look_text_update(void)
{
    const char *n = filmsim_name();
    sprintf(filmsim_look_text, "%s: %.24s", lang_str(LANG_EXCCD_LOOK), n);
}

void filmsim_set(int v)
{
    if (!film_at(v)) return;
    int was_off = (conf.film_sim == 0);
    if (v != conf.film_sim) preview_pending = 1;
    conf.film_sim = v;
    film_name_store();
    look_text_update();
    if (v == 0) {
        conf.film_save_original_jpeg = 0;
        // zurueck auf Canon-Standard
#ifndef FS_NO_MYCOLORS
        if (!was_off && filmsim_photo_mode())
            shooting_set_prop(PROPCASE_MY_COLORS, MC_OFF);
#else
        (void)was_off;
#endif
        set_curve(current());
    } else {
        filmsim_apply();
    }
}

//-------------------------------------------------------------------
// Filmauswahl im Stil eines Filmschachtel-Fensters
//   Film selection opens with the active look.
//   links/rechts oder hoch/runter -> blaettern
//   FUNC/SET     -> uebernehmen und schliessen
//   MENU         -> Optionen (RAW, System)
//   Print        -> abbrechen
//-------------------------------------------------------------------
extern CMenu exccd_menu;
extern gui_handler menuGuiHandler;

#define LABEL_TOP   2
#define LABEL_H     196
#define BAR_Y       (LABEL_TOP + LABEL_H + 3)
#define HELP_Y      (BAR_Y + FONT_HEIGHT + 3)    // Tastenhinweis unter der Leiste
#define LABEL_W     301                 // Breite der EXCCD-Laschen

// EXCCD-Farben wie in den Laschen (Kamerapalette, Aufnahmemodus, alle deckend)
#define EX_BG       FILMSIM_COLOR_BG
#define EX_GREY     FILMSIM_COLOR_GREY
#define EX_RULE     FILMSIM_COLOR_RULE

static int sel = 0;                 // gerade angezeigter Film
static int need_draw = 0;           // 1 = alles, 2 = nur Leiste
static unsigned char *label_buf = 0;
static int label_len = 0;
static int label_for = -1;          // fuer welchen Film label_buf gilt
static int picker_offset;
static int picker_count;

static void label_free(void)
{
    if (label_buf) free(label_buf);
    label_buf = 0; label_len = 0; label_for = -1;
}

// Kameras mit eigener Palette (FILMSIM_LABELS): .FLM mit Farbindizes der Kamera.
// Alle anderen: .FLC mit CHDK-Farbnummern (IDX_COLOR_*). CHDK kennt fuer jede
// Kamera den passenden Palettenindex, so stimmen die Farben ohne Messung.
#ifdef FILMSIM_LABELS
#define LABEL_EXT       ".FLM"
#define LABEL_MAGIC     "FLM1"
#define LABEL_CHDK_PALETTE 0
#else
#define LABEL_EXT       ".FLC"
#define LABEL_MAGIC     "FLC1"
#define LABEL_CHDK_PALETTE 1
#endif

static void label_load(int i)
{
    char fn[40];
    struct stat st;
    label_free();
    label_for = i;
    const filmsim_t *film = film_at(i);
    if (!film || !film->label) return;
    strcpy(fn, "A/CHDK/FILMS/");
    strcat(fn, film->label);
    strcat(fn, LABEL_EXT);
    if (stat(fn, &st) != 0 || st.st_size < 8 || st.st_size > 120000) return;
    label_buf = malloc(st.st_size);
    if (!label_buf) return;
    int fd = open(fn, O_RDONLY, 0777);
    if (fd < 0) { label_free(); label_for = i; return; }
    label_len = read(fd, label_buf, st.st_size);
    close(fd);
    if (label_len < 8 || memcmp(label_buf, LABEL_MAGIC, 4) != 0) { label_free(); label_for = i; }
}

static void draw_film_bitmap(const unsigned char *data, int len, int w, int h,
                             int ox, int oy, int chdk_palette)
{
    int x = 0, y = 0, p = 8;
    while (p + 1 < len && y < h) {
        int n = data[p], c = data[p + 1];
        p += 2;
        color pixel = chdk_palette ? chdk_colors[c <= IDX_COLOR_MAX ? c : IDX_COLOR_GREY] : c;
        while (n > 0 && y < h) {
            int run = w - x;
            if (run > n) run = n;
            if (c) draw_hline(ox + x, oy + y, run, pixel);
            x += run; n -= run;
            if (x >= w) { x = 0; y++; }
        }
    }
}

// RLE-Lasche zeichnen; 0 = keine Datei
static int label_draw(void)
{
    if (!label_buf) return 0;
    int w = label_buf[4] | (label_buf[5] << 8);
    int h = label_buf[6] | (label_buf[7] << 8);
    if (w <= 0 || h <= 0 || w > camera_screen.width || h > LABEL_H) return 0;
    int ox = (camera_screen.width - w) / 2;
    int oy = LABEL_TOP + (LABEL_H - h) / 2;
    draw_film_bitmap(label_buf, label_len, w, h, ox, oy, LABEL_CHDK_PALETTE);
    return 1;
}

// Ersatz, wenn es keine Lasche gibt: Textkarte im Aufbau der Laschen
static void card_draw(int i)
{
    const char *n = sim_name(i);
    int len = strlen(n);
    int x0 = (camera_screen.width - LABEL_W) / 2, y0 = LABEL_TOP, m = 16, k;
    draw_rectangle(x0, y0, x0 + LABEL_W - 1, y0 + LABEL_H - 1, MAKE_COLOR(EX_BG, EX_RULE),
                   RECT_BORDER1|DRAW_FILLED|RECT_ROUND_CORNERS);
    draw_string(x0 + m, y0 + 12, FILMSIM_PRODUCT_NAME, MAKE_COLOR(EX_BG, COLOR_WHITE));
    draw_hline(x0 + m, y0 + 34, LABEL_W - 2 * m, EX_RULE);
    int sx = (len * FONT_WIDTH * 2 <= LABEL_W - 2 * m) ? 2 : 1;
    draw_string_scaled(x0 + m, y0 + 60, n, MAKE_COLOR(EX_BG, COLOR_WHITE), sx, 2);
    // Perforationslinie
    draw_hline(x0 + m, y0 + 156, LABEL_W - 2 * m, EX_RULE);
    for (k = 0; k < 16; k++)
        draw_rectangle(x0 + m + 3 + k * 17, y0 + 166, x0 + m + 3 + k * 17 + 7, y0 + 176,
                       MAKE_COLOR(EX_RULE, EX_RULE), RECT_BORDER0|DRAW_FILLED);
}

// Leiste unter der Lasche: Pfeile grau, Name weiss, aktiver Film mit Quadrat
static void bar_draw(void)
{
    const int film = sel ? picker_offset + sel : 0;
    const char *n = sim_name(film);
    int len = strlen(n);
    int x0 = (camera_screen.width - LABEL_W) / 2, x1 = x0 + LABEL_W - FONT_WIDTH;
    int x = (camera_screen.width - len * FONT_WIDTH) / 2;
    draw_rectangle(0, LABEL_TOP + LABEL_H, camera_screen.width - 1, camera_screen.height - 1,
                   MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0|DRAW_FILLED);
    draw_string(x0, BAR_Y, "<", MAKE_COLOR(COLOR_BLACK, EX_GREY));
    draw_string(x1, BAR_Y, ">", MAKE_COLOR(COLOR_BLACK, EX_GREY));
    // Position in der Liste, bei vielen Looks sonst schwer zu schaetzen
    if (sel > 0) {
        char pos[12];
        sprintf(pos, "%d/%d", sel, picker_count - 1);
        draw_string(x0 + 2 * FONT_WIDTH, BAR_Y, pos, MAKE_COLOR(COLOR_BLACK, EX_GREY));
    }
    if (film == conf.film_sim) {
        x += 6;
        draw_rectangle(x - 12, BAR_Y + 5, x - 7, BAR_Y + 10, MAKE_COLOR(COLOR_WHITE, COLOR_WHITE),
                       RECT_BORDER0|DRAW_FILLED);
    }
    draw_string(x, BAR_Y, n, MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
    // Hinweis: wer die Kamera zum ersten Mal hat, weiss nicht, dass MENU die Optionen oeffnet
    const char *hl = lang_str(LANG_EXCCD_PICK_HELP);
    if (HELP_Y + FONT_HEIGHT <= camera_screen.height)
        draw_string((camera_screen.width - (int)strlen(hl) * FONT_WIDTH) / 2, HELP_Y, hl, MAKE_COLOR(COLOR_BLACK, EX_GREY));
}

static void film_gui_draw(int force)
{
    if (force) need_draw = 1;
    if (!need_draw) return;
    if (need_draw == 1) {
        const int film = sel ? picker_offset + sel : 0;
        draw_rectangle(0, 0, camera_screen.width - 1, camera_screen.height - 1,
                       MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0|DRAW_FILLED);
        if (label_for != film) label_load(film);
        if (!label_draw()) card_draw(film);
    }
    bar_draw();
    need_draw = 0;
}

void filmsim_label_release(void)
{
    label_free();
}

static void film_gui_close(void)
{
    label_free();
    // erst schliessen, wenn die Taste losgelassen ist - sonst bekommt
    // die Canon-Firmware das Loslassen von FUNC/SET mit und oeffnet ihr Menue
    exit_alt_on_release();
}

static int film_gui_kbd(void)
{
    const int step = picker_count > 5 ? 5 : 1;
    switch (kbd_get_autoclicked_key() | get_jogdial_direction())
    {
    case KEY_LEFT: case JOGDIAL_LEFT:
        sel = (sel > 0) ? sel - 1 : picker_count - 1;
        need_draw = 1;
        break;
    case KEY_RIGHT: case JOGDIAL_RIGHT:
        sel = (sel + 1) % picker_count;
        need_draw = 1;
        break;
    case KEY_UP:                    // grosser Sprung
        sel = (sel + picker_count - step) % picker_count;
        need_draw = 1;
        break;
    case KEY_DOWN:
        sel = (sel + step) % picker_count;
        need_draw = 1;
        break;
    case KEY_SET:
        filmsim_set(sel ? picker_offset + sel : 0);
        if (dev_armed) {                // RAW entwickeln: weiter zu den Effekten
            dev_pick_only(sel);
            label_free();
            dev_fx_menu_open();
            break;
        }
        conf_save();
        film_gui_close();
        break;
    }
    return 0;
}

static void film_gui_menu_btn(void)
{
    label_free();
    if (get_curr_menu() != &exccd_menu)
        gui_menu_init(&exccd_menu);
    gui_set_mode(&menuGuiHandler);
}

static gui_handler filmGuiHandler = { GUI_MODE_MODULE, film_gui_draw, film_gui_kbd, film_gui_menu_btn, 0, 0 };

#ifndef FILMSIM_LABELS
// Kameras ohne eigene Laschen: Canon-Palette einmal still auf die Karte schreiben
// (wie Options -> System -> Save palette). Mit dieser Datei bekommt die Kamera
// spaeter Laschen in ihren eigenen Farben; Nutzer muessen sie nur einschicken.
static void palette_autosave(void)
{
    struct stat st;
    const char *fn = "A/CHDK/PALREC.BIN";
    if (!camera_info.state.mode_rec || stat(fn, &st) == 0) return;
    char *pal = (char*)vid_get_bitmap_active_palette();
    int sz = vid_get_palette_size();
    if (!pal || sz <= 0) return;
    int fd = open(fn, O_WRONLY|O_CREAT|O_TRUNC, 0777);
    if (fd < 0) return;
    write(fd, pal, sz);
    close(fd);
}
#endif

void filmsim_open_menu(void)
{
#ifdef FILMSIM_QUICK_BUTTONS
    quick_film_mode = 0;
#endif
#ifndef FILMSIM_LABELS
    palette_autosave();
#endif
    films_load();
    int presets = !dev_armed && conf.film_presets_only && films_load_presets();
    picker_offset = presets ? PRESET_ID_OFFSET : 0;
    picker_count = presets ? preset_count + 1 : NUM_SIMS;
    int id = conf.film_sim ? film_find(picker_offset, picker_count - 1, current()->name) : 0;
    sel = id ? id - picker_offset : 0;
    need_draw = 1;
    gui_set_mode(&filmGuiHandler);
}

void filmsim_open_catalog(int presets)
{
    if (presets && !films_load_presets()) {
        gui_mbox_init(LANG_EXCCD_SELECT_PRESETS, LANG_EXCCD_PRESETS_UNAVAILABLE,
                      MBOX_FUNC_RESTORE|MBOX_TEXT_CENTER|MBOX_BTN_OK, NULL);
        return;
    }
    conf.film_presets_only = presets;
    conf_save();
    filmsim_open_menu();
}

#ifdef FILMSIM_QUICK_BUTTONS
enum { QUICK_NONE, QUICK_LEFT, QUICK_RIGHT, QUICK_BUTTON_COUNT };
static const int quick_keys[] = { 0, KEY_LEFT, KEY_RIGHT };
static const int quick_names[] = { LANG_EXCCD_NONE, LANG_EXCCD_LEFT, LANG_EXCCD_RIGHT };

void filmsim_normalize_quick_buttons(void)
{
    if ((unsigned)conf.film_quick_button >= QUICK_BUTTON_COUNT)
        conf.film_quick_button = QUICK_NONE;
    if ((unsigned)conf.iso_quick_button >= QUICK_BUTTON_COUNT
        || conf.iso_quick_button == conf.film_quick_button)
        conf.iso_quick_button = QUICK_NONE;
}

static const char* filmsim_quick_button_enum(int change, int iso)
{
    filmsim_normalize_quick_buttons();
    int *button = iso ? &conf.iso_quick_button : &conf.film_quick_button;
    int occupied = iso ? conf.film_quick_button : conf.iso_quick_button;
    if (change) {
        int direction = change > 0 ? 1 : -1;
        gui_enum_value_change(button, direction, QUICK_BUTTON_COUNT);
        if (*button != QUICK_NONE && *button == occupied)
            gui_enum_value_change(button, direction, QUICK_BUTTON_COUNT);
    }
    return lang_str(quick_names[*button]);
}

static int quick_shortcuts_available(int mode)
{
    return mode == MODE_P || mode == MODE_NIGHT_SCENE || mode == MODE_LANDSCAPE
        || mode == MODE_PORTRAIT || mode == MODE_AUTO;
}

int filmsim_quick_mode(int key)
{
    if ((key != KEY_LEFT && key != KEY_RIGHT) || !camera_info.state.gui_mode_none
        || camera_info.state.is_shutter_half_press || recreview_hold
        || !quick_shortcuts_available(camera_info.state.mode_shooting))
        return ALT_MODE_NORMAL;

    filmsim_normalize_quick_buttons();
    int mode = ALT_MODE_NORMAL;
    if (key == quick_keys[conf.film_quick_button])
        mode = ALT_MODE_ENTER_FILM;
    else if (key == quick_keys[conf.iso_quick_button])
        mode = ALT_MODE_ENTER_ISO;

    if (mode != ALT_MODE_NORMAL && filmsim_picker_allowed() && !shooting_in_progress()
        && camera_info.state.state_shooting_progress != SHOOTING_PROGRESS_PROCESSING)
        return mode;
    return ALT_MODE_NORMAL;
}

extern const ISOTable iso_table[];
extern const unsigned int ISO_SIZE;
static int quick_iso_mode;
static int quick_iso_first;
static int quick_iso_selected;

static int quick_iso_still_open(void)
{
    return filmsim_photo_mode() && !recreview_hold
        && camera_info.state.mode_shooting == quick_iso_mode;
}

static void quick_iso_draw(__attribute__ ((unused))int force)
{
    if (!quick_iso_still_open()) {
        exit_alt();
        return;
    }
    const char *title = lang_str(LANG_EXCCD_QUICK_ISO_TITLE);
    const int cell_width = 42, height = 51;
    const int count = (int)ISO_SIZE - quick_iso_first;
    const int width = count * cell_width + 28;
    const int x = (camera_screen.width - width) / 2;
    const int y = (camera_screen.height - height) / 2;
    draw_rectangle(x, y, x + width - 1, y + height - 1,
                   MAKE_COLOR(COLOR_BLACK, COLOR_WHITE), RECT_BORDER1|DRAW_FILLED);
    draw_string(x + (width - (int)strlen(title) * FONT_WIDTH) / 2,
                y + 4, title, MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
    for (int i = quick_iso_first; i < (int)ISO_SIZE; i++) {
        const char *label = iso_table[i].name;
        int left = x + 14 + (i - quick_iso_first) * cell_width;
        int selected = i == quick_iso_selected;
        if (selected)
            draw_rectangle(left + 1, y + 24, left + cell_width - 2, y + 45,
                           MAKE_COLOR(COLOR_WHITE, COLOR_WHITE), RECT_BORDER0|DRAW_FILLED);
        draw_string(left + (cell_width - (int)strlen(label) * FONT_WIDTH) / 2,
                    y + 27, label, selected ? MAKE_COLOR(COLOR_WHITE, COLOR_BLACK)
                                              : MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
    }
}

static int quick_iso_kbd(void)
{
    if (!quick_iso_still_open()) {
        exit_alt();
        return 0;
    }
    int half = kbd_is_key_clicked(KEY_SHOOT_HALF);
    if (half || kbd_is_key_clicked(KEY_SET)) {
        shooting_set_iso_mode(iso_table[quick_iso_selected].id);
        if (half)
            exit_alt();
        else
            exit_alt_on_release();
    } else if (kbd_is_key_clicked(KEY_LEFT) && quick_iso_selected > quick_iso_first) {
        quick_iso_selected--;
    } else if (kbd_is_key_clicked(KEY_RIGHT) && quick_iso_selected + 1 < (int)ISO_SIZE) {
        quick_iso_selected++;
    }
    return 0;
}

static gui_handler quickIsoGuiHandler = { GUI_MODE_MODULE, quick_iso_draw, quick_iso_kbd, exit_alt_on_release, 0, 0 };

int filmsim_quick_iso_open(void)
{
    int mode = camera_info.state.mode_shooting;
    int available = quick_shortcuts_available(mode) || mode == MODE_TV || mode == MODE_AV || mode == MODE_M;
    if (!available || !filmsim_picker_allowed() || recreview_hold)
        return 0;

    quick_iso_mode = mode;
    quick_iso_first = mode == MODE_M ? 1 : 0;
    quick_iso_selected = quick_iso_first;
    int current_iso = shooting_get_iso_mode();
    for (int i = quick_iso_first; i < (int)ISO_SIZE; i++) {
        if (iso_table[i].id == current_iso) {
            quick_iso_selected = i;
            break;
        }
    }
    gui_set_mode(&quickIsoGuiHandler);
    return 1;
}

int filmsim_quick_open(int state)
{
    int mode = camera_info.state.mode_shooting;
    if (!quick_shortcuts_available(mode))
        return 0;
    if (state == ALT_MODE_ENTER_ISO)
        return filmsim_quick_iso_open();
    if (state != ALT_MODE_ENTER_FILM || !filmsim_picker_allowed())
        return 0;

    filmsim_open_menu();
    quick_film_mode = mode;
    return 1;
}
#endif

const CMenuItem *filmsim_quick_button_items(int *count)
{
#ifdef FILMSIM_QUICK_BUTTONS
    static const CMenuItem items[] = {
        MENU_ITEM(0x22, LANG_EXCCD_FILM_BUTTON, MENUITEM_ENUM, filmsim_quick_button_enum, 0),
        MENU_ITEM(0x22, LANG_EXCCD_ISO_BUTTON,  MENUITEM_ENUM, filmsim_quick_button_enum, 1),
    };
    *count = sizeof(items) / sizeof(items[0]);
    return items;
#else
    *count = 0;
    return NULL;
#endif
}

// Menue-Eintrag "Filmsimulation" (MENUITEM_ENUM)
const char* filmsim_menu_enum(int change, __attribute__ ((unused))int arg)
{
    films_load();
    if (change) {
        int v = conf.film_sim + change;
        if (v < 0) v = NUM_SIMS - 1;
        if (v >= NUM_SIMS) v = 0;
        filmsim_set(v);
    }
    return filmsim_name();
}

// Anti-Farbrauschen: conf.film_cnr 0 aus, 1 leicht, 2 stark, 3 auto
// (3 statt 0, damit alte Einstellungen gueltig bleiben).
// Auto richtet sich nach der ISO des Bildes. Rueckgabe 0 / 1 / 2.
#define CNR_AUTO 3
static int cnr_level(void)
{
    if (conf.film_cnr != CNR_AUTO) return conf.film_cnr;
    int iso = (dev_active && dev_has_meta && dev_meta.iso > 0) ? dev_meta.iso : shooting_get_iso_market();
    return iso >= 800 ? 2 : (iso >= 400 ? 1 : 0);
}

// " | text" anhaengen, wenn die Zeile danach hoechstens max Zeichen hat
static void osd_add(char *buf, const char *text, int max)
{
    if ((int)(strlen(buf) + 3 + strlen(text)) > max) return;
    strcat(buf, " | ");
    strcat(buf, text);
}

// Kleine Anzeige des aktiven Films im Sucherbild
void filmsim_draw_osd(void)
{
    static char buf[160];
    static int was_pending = 0;
    // Modus gewechselt -> alten Filmnamen einmal wegwischen
    static int was_photo = -1;
    int is_photo = filmsim_photo_mode();
    if (was_photo == 1 && !is_photo) gui_set_need_restore();
    was_photo = is_photo;
    if (!is_photo) return;
    // Halbdruck (Canon zeigt unten Zeit/Blende), MENU, FUNC.SET -> ausblenden
    if (filmsim_osd_hidden()) return;
    if (was_pending && !preview_pending)
        gui_set_need_restore();      // laengeren Hinweis einmal wegwischen
    was_pending = preview_pending;
    if (!dev_armed && conf.film_sim == 0 && !preview_pending && !conf.tv_override_enabled
        && !conf.film_double && !conf.subj_dist_override_koef
        && !conf.film_mist && !conf.film_halation && !conf.film_vignette
        && !conf.film_leak && !conf.film_datestamp && !conf.film_dr && !conf.film_cnr
        && !conf.film_zoom && !conf.film_grain && !conf.film_frame && !conf.film_recipe
        && !conf.film_aiso) return;
    // Passt die Zeile nicht auf den Bildschirm, fallen die hinteren Angaben weg
    // (ganz, nicht abgeschnitten). Darum das Wichtigste zuerst.
    int max = camera_screen.width / FONT_WIDTH;
    if (max >= (int)sizeof(buf)) max = sizeof(buf) - 1;
    strcpy(buf, filmsim_name());
    if (dev_armed) {
        // Wichtiger als alles andere: Das naechste Foto ist kein Foto
        sprintf(buf, lang_str(LANG_EXCCD_DEV_OSD), dev_name, filmsim_name());
        if (batch_on && batch_n > 1) {
            char n[12];
            sprintf(n, "%d/%d", batch_i, batch_n);
            osd_add(buf, n, max);
        }
        if (dev_fx_any()) osd_add(buf, "FX", max);
        if (dev_ev) osd_add(buf, dev_ev_enum(0, 0), max);
        if (dev_temp) osd_add(buf, dev_temp_enum(0, 0), max);
        if (dev_tint) osd_add(buf, dev_tint_enum(0, 0), max);
        if (dev_dr == 1) osd_add(buf, "DR off", max);
        if (dev_dr > 1) osd_add(buf, dev_dr == 2 ? "DR+1" : "DR+2", max);
    } else if (preview_pending) {
        strcat(buf, " - ");
        strcat(buf, lang_str(LANG_EXCCD_HALF_PRESS));
    } else {
        char rc[8], wb[16];
        recipe_osd(rc, wb);
        if (shot_skip_last) osd_add(buf, "low mem!", max);
        if (rc[0]) osd_add(buf, rc, max);
        if (conf.film_double) osd_add(buf, conf.film_double == 1 ? "2x 1/2" : "2x 2/2", max);
        if (conf.film_dr == 1) osd_add(buf, "DR+1", max);
        if (conf.film_dr == 2) osd_add(buf, "DR+2", max);
        extern const char* filmsim_tv_str(void);
        const char *tv = filmsim_tv_str();
        if (tv) osd_add(buf, tv, max);
        else if (filmsim_aiso_str()) osd_add(buf, filmsim_aiso_str(), max);
        extern const char* filmsim_bwfilter_str(void);
        const char *bf = filmsim_bwfilter_str();
        if (bf) osd_add(buf, bf, max);
        if (wb[0]) osd_add(buf, wb, max);
        extern int filmsim_effects_active(void);
        if (filmsim_effects_active()) osd_add(buf, "FX", max);
        if (conf.film_cnr) osd_add(buf, conf.film_cnr == CNR_AUTO ? "NR auto" : "NR", max);
        extern const char* filmsim_focus_str(void);
        const char *fo = filmsim_focus_str();
        if (fo) osd_add(buf, fo, max);
        const char *zo = filmsim_zoom_str();
        if (zo) osd_add(buf, zo, max);
    }
    const char *n = buf;
    int len = strlen(n);
    static int last_len = 0;
    if (len < last_len) gui_set_need_restore();   // kuerzerer Text: Reste wegwischen
    last_len = len;
    int x = (camera_screen.width - len * FONT_WIDTH) / 2;
    if (x < 0) x = 0;
    int y = camera_screen.height - FONT_HEIGHT - 4;   // unten, weg von den Canon-Symbolen oben
    draw_string(x, y, n, user_color(conf.osd_color));
}


//-------------------------------------------------------------------
// RAW-Kanalmischung (Spezialfilme)
//   Wird im RAW-Hook nach den Tonkurven aufgerufen, vor der JPEG-Erzeugung.
//   Je 2x2-Bayer-Block (R G / G B) werden die Kanaele - auf den Weissabgleich
//   der Aufnahme normiert (shot_wb_gains) - per Matrix neu gemischt.
//   Zeilensummen = 1, damit Grau grau bleibt.
//-------------------------------------------------------------------
#ifdef FS_RAW
// Violet und Concrete: mix_violet / mix_concrete in filmsim_raw.h
// Farbfilter fuer Schwarzweiss: alle drei Kanaele = gewichtete Helligkeit
static const short mix_bw[4][3][3] = {
    {{0}},                                                          // kein Filter
    {{102,128,26},{102,128,26},{102,128,26}},                       // Gelb   .40 .50 .10
    {{141,102,13},{141,102,13},{141,102,13}},                       // Orange .55 .40 .05
    {{192, 64, 0},{192, 64, 0},{192, 64, 0}},                       // Rot    .75 .25 .00
};
// WB-Normierung (Rohwert * f / 256), Tageslicht-Gains des Sensors
#define WB_R FILMSIM_WB_R
#define WB_B FILMSIM_WB_B
#define BLK  31


// Rueckrechnung des Weissabgleichs: (n << 8) / WB als Tabelle statt Division.
// Die alten ARM-Prozessoren haben keinen Divisionsbefehl - jede Division
// kostet dort Dutzende Takte. Die Tabelle enthaelt exakt dieselben Werte
// wie die Division (gleiche Formel), das Ergebnis ist bitgleich.
#define WBDIV_MIN  (-1024)
#define WBDIV_MAX  4095
// 20 KB: erst anlegen, wenn der Farbrauschfilter sie braucht, und dann behalten.
// Fest im Kern wuerden sie gegen das Groessenlimit der Kamera zaehlen.
static short *wbdiv_r = 0, *wbdiv_b = 0;
static int wbdiv_init(void)
{
    int n, size = (WBDIV_MAX - WBDIV_MIN + 1) * sizeof(short);
    if (wbdiv_r) return 1;
    short *t = malloc(2 * size);
    if (!t) return 0;
    wbdiv_b = t + (WBDIV_MAX - WBDIV_MIN + 1);
    for (n = WBDIV_MIN; n <= WBDIV_MAX; n++) {
        t[n - WBDIV_MIN] = (short)((n << 8) / WB_R);
        wbdiv_b[n - WBDIV_MIN] = (short)((n << 8) / WB_B);
    }
    wbdiv_r = t;
    return 1;
}
static inline __attribute__((always_inline)) int div_wb_r(int n)
{
    return (n >= WBDIV_MIN && n <= WBDIV_MAX) ? wbdiv_r[n - WBDIV_MIN] : ((n << 8) / WB_R);
}
static inline __attribute__((always_inline)) int div_wb_b(int n)
{
    return (n >= WBDIV_MIN && n <= WBDIV_MAX) ? wbdiv_b[n - WBDIV_MIN] : ((n << 8) / WB_B);
}

// Weissabgleich dieses Fotos als Gains * 256 (R, B je G), wie ihn Canon gleich auf das
// JPEG anwendet (dieselben Werte schreibt die DNG als AsShotNeutral). Damit mischen
// Violet & Co. auch unter Kunstlicht mit Auto-WB von Grau aus und bekommen keinen
// eigenen Farbstich. Unplausible Werte (0, ausserhalb 0,5..4): Tageslicht-Gains.
static void shot_wb_gains(int *wr, int *wb)
{
    int w[3] = { 0, 0, 0 };                     // G, R, B wie in modules/dng.c
    *wr = WB_R; *wb = WB_B;
    get_property_case(camera_info.props.wb_adj, &w, sizeof(w));
    if (w[0] <= 0 || w[1] <= 0 || w[2] <= 0) return;
    int r = w[1] * 256 / w[0], b = w[2] * 256 / w[0];
    if (r < 128 || r > 1024 || b < 128 || b > 1024) return;
    *wr = r; *wb = b;
}

__attribute__((optimize("O2"))) void filmsim_raw_apply(char *raw)
{
    const filmsim_t *s = current();
    if (!raw || !filmsim_photo_mode()) return;
    const short (*m)[3];
    if (s->mix == 1)      m = mix_violet;
    else if (s->mix == 2) m = mix_concrete;
    else if (s->mix == 3) m = mix_turquoise;
    else if (s->mix == 4) m = mix_infra;
    else if (s->mix == 5) m = mix_cyano;
    else if (bw_look(s) && conf.film_bwfilter > 0 && conf.film_bwfilter < 4)
                          m = mix_bw[conf.film_bwfilter];
    else return;
    int rows = (camera_sensor.raw_rows - FS_ROW0) & ~1;
    int groups = camera_sensor.raw_rowpix / 8;
    int rowlen = camera_sensor.raw_rowlen;
    int y, g, k, wr, wb;
    shot_wb_gains(&wr, &wb);
    int ir = FS_WBINV(wr), ib = FS_WBINV(wb);
    for (y = 0; y < rows; y += 2) {
        prog_rows(y, rows);
        unsigned char *a0 = (unsigned char *)raw + (y + FS_ROW0) * rowlen;   // R G R G
        unsigned char *a1 = a0 + rowlen;                         // G B G B
        for (g = 0; g < groups; g++, a0 += FS_GRP, a1 += FS_GRP) {
            int p[8], q[8];
            dec8(a0, p); dec8(a1, q);
            for (k = 0; k < 8; k += 2)
                mix_block(m, p, q, k, BLK, wr, wb, ir, ib);
            enc8(a0, p); enc8(a1, q);
        }
    }
}
#else
void filmsim_raw_apply(__attribute__ ((unused))char *raw) {}
#endif

//-------------------------------------------------------------------
// Startup logo stored as an FLC bitmap so CHDK supplies the camera palette.
//-------------------------------------------------------------------
static unsigned char *splash_buf = 0;
static int splash_len = 0;

int filmsim_splash_init(void)
{
    const char *fn = "A/CHDK/FILMS/SPLASH.FLC";
    struct stat st;
    if (splash_buf) return 1;
    if (stat(fn, &st) != 0 || st.st_size < 10 || st.st_size > 120000 || (st.st_size & 1)) return 0;
    unsigned char *b = malloc(st.st_size);
    if (!b) return 0;
    int fd = open(fn, O_RDONLY, 0777);
    if (fd < 0) { free(b); return 0; }
    int len = read(fd, b, st.st_size);
    close(fd);
    if (len != st.st_size || memcmp(b, "FLC1", 4) != 0) { free(b); return 0; }
    int w = b[4] | (b[5] << 8);
    int h = b[6] | (b[7] << 8);
    if (w <= 0 || h <= 0 || w > camera_screen.width || h > camera_screen.height) { free(b); return 0; }
    int pixels = 0;
    for (int p = 8; p < len; p += 2) pixels += b[p];
    if (pixels != w * h) { free(b); return 0; }
    splash_buf = b; splash_len = len;
    return 1;
}

void filmsim_splash_free(void)
{
    if (splash_buf) free(splash_buf);
    splash_buf = 0; splash_len = 0;
}

void filmsim_draw_splash(void)
{
    if (!splash_buf) return;
    int w = splash_buf[4] | (splash_buf[5] << 8);
    int h = splash_buf[6] | (splash_buf[7] << 8);
    int ox = (camera_screen.width - w) / 2;
    int oy = (camera_screen.height - h) / 2;
    draw_rectangle(0, 0, camera_screen.width - 1, camera_screen.height - 1,
                   MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0|DRAW_FILLED);
    draw_film_bitmap(splash_buf, splash_len, w, h, ox, oy, 1);
}

//-------------------------------------------------------------------
// Filmlog: haelt zu jedem Foto fest, welcher Film aktiv war
//   A/CHDK/FILMLOG.CSV   z.B.  "100CANON/IMG_0123.JPG;Postcard 100"
//   (VxWorks kann nicht anhaengen -> oeffnen, ans Ende springen, schreiben)
//-------------------------------------------------------------------
#include "file_counter.h"

// Path relative to DCIM for the current capture.
static int shot_path(char *path)
{
    char dir[32];
    get_target_dir_name(dir);                     // "A/DCIM/100CANON"
    const char *d = strrchr(dir, '/');
    d = d ? d + 1 : dir;
    int number = get_target_file_num();
    sprintf(path, "%s/IMG_%04d.JPG", d, number);
    return number;
}

void filmsim_log_shot(void)
{
    char line[160];
    if (dev_active) {                   // Entwickeln: danach eingelegten Film zurueck
        dev_active = 0;
        if (!batch_on) dev_restore = 1; // Serie: erst nach dem letzten Look (batch_stop)
    } else {
        dev_name[0] = 0;
    }
    const char *fn = "A/CHDK/FILMLOG.CSV";
    struct stat st;
    int is_new = (stat(fn, &st) != 0);
    shot_path(line);
    sprintf(line + strlen(line), ";%s", filmsim_name());
    if (dev_name[0] || shot_skip) {     // dritte Spalte: entwickelt aus dieser Datei
        strcat(line, ";");
        strcat(line, dev_name);
        dev_name[0] = 0;
    }
    if (shot_skip) {                    // vierte Spalte: mangels Speicher ausgelassen
        static const char *nm[] = { "hotpixel", "double", "colorNR", "curve", "effects", "grain" };
        int k, first = 1;
        strcat(line, ";low memory, skipped:");
        for (k = 0; k < 6; k++)
            if (shot_skip & (1 << k)) {
                strcat(line, first ? " " : ",");
                strcat(line, nm[k]);
                first = 0;
            }
    }
    shot_skip_last = shot_skip;
    strcat(line, "\r\n");
    int fd = open(fn, O_RDWR | O_CREAT, 0777);
    if (fd < 0) return;
    if (is_new) {
        const char *hdr = "File;Film\r\n";
        write(fd, hdr, strlen(hdr));
    } else {
        lseek(fd, 0, 2);                          // SEEK_END
    }
    write(fd, line, strlen(line));
    close(fd);
}

//-------------------------------------------------------------------
// Performance CSV. The RAW and pair state machines run on SpyTask. The A620
// filewrite task only supplies the matching JPEG path and its close timestamp.
//-------------------------------------------------------------------
#define PERF_CSV "A/CHDK/LOGS/PERF.CSV"
#define PERF_JPEG_TIMEOUT_MS 30000

static struct {
    int active;
    filmsim_perf_mode mode;
    filmsim_perf_stage stage;
    unsigned start_tick, stage_tick, end_tick;
    int ms[PERF_STAGE_COUNT];
    int number, slot;
    char custom[48], original[48], look[24];
    const char *status;
    int wait_for_pair;
} perf;

#ifdef FILMSIM_JPEG_PAIR
static volatile int perf_jpeg_number, perf_jpeg_open, perf_jpeg_closed;
static volatile unsigned perf_jpeg_close_tick;
static char perf_jpeg_path[32];
static filmsim_perf_stage perf_jpeg_stage;
#endif

static int perf_recipe_available(void)
{
    return (conf.film_sim > 0 && film_at(conf.film_sim)) || recipe_active();
}

const char *filmsim_perf_enum(int change, __attribute__((unused)) int arg)
{
    if (!perf_recipe_available())
        return lang_str(LANG_EXCCD_NEEDS_RECIPE);
    if (change)
        conf.film_performance_metrics = !conf.film_performance_metrics;
    return lang_str(conf.film_performance_metrics ? LANG_EXCCD_ON : LANG_EXCCD_OFF);
}

static void perf_write(const char *status)
{
    static const char *const modes[] = { "single_photo", "keep_original_jpeg", "raw_develop" };
    static const char header[] =
        "mode;status;custom_jpeg;original_jpeg;look;recipe_slot;raw_to_output_ms;"
        "raw_spool_ms;raw_output_ms;original_jpeg_ms;helper_wait_ms;raw_load_ms;"
        "develop_setup_ms;bad_pixels_ms;hot_pixels_ms;double_exposure_ms;color_nr_ms;"
        "tone_curve_ms;channel_mix_ms;glow_tone_ms;grain_ms;frame_ms;date_stamp_ms;"
        "white_balance_ms;film_log_ms;custom_jpeg_ms\r\n";

    perf.active = 0;
#ifdef FILMSIM_JPEG_PAIR
    perf_jpeg_number = 0;
#endif
    int fd = open(PERF_CSV, O_WRONLY | O_CREAT, 0777);
    if (fd < 0)
        return;
    int end = lseek(fd, 0, SEEK_END);
    if (end >= 0 && (end || write(fd, header, sizeof(header) - 1) == sizeof(header) - 1)) {
        char line[400];
        char *p = line + sprintf(line, "%s;%s;%s;%s;\"", modes[perf.mode], status,
                                 perf.custom, perf.original);
        for (const char *c = perf.look; *c; c++) {
            if (*c == '"') *p++ = '"';
            *p++ = *c;
        }
        *p++ = '"';
        *p++ = ';';
        if (perf.slot) p += sprintf(p, "C%d", perf.slot);
        p += sprintf(p, ";%u", perf.end_tick - perf.start_tick);
        for (int i = 0; i < PERF_STAGE_COUNT; i++) {
            *p++ = ';';
            if (perf.ms[i] >= 0) p += sprintf(p, "%d", perf.ms[i]);
        }
        *p++ = '\r';
        *p++ = '\n';
        write(fd, line, p - line);
    }
    close(fd);
}

#ifdef FILMSIM_JPEG_PAIR
static void perf_collect_jpeg(void)
{
    if (!perf_jpeg_number || !perf_jpeg_closed)
        return;
    perf_jpeg_number = 0;
    perf.end_tick = perf_jpeg_close_tick;
    perf.ms[perf_jpeg_stage] = perf.end_tick - perf.stage_tick;
    strcpy(perf_jpeg_stage == PERF_ORIGINAL_JPEG ? perf.original : perf.custom, perf_jpeg_path);
}
#endif

static void perf_tick(void)
{
    if (!perf.active)
        return;
#ifdef FILMSIM_JPEG_PAIR
    perf_collect_jpeg();
    if (perf.wait_for_pair)
        return;
    if (perf_jpeg_number) {
        if ((unsigned)get_tick_count() - perf.end_tick < PERF_JPEG_TIMEOUT_MS)
            return;
        perf_write(perf.status ? perf.status : "jpeg_unconfirmed");
        return;
    }
#endif
    perf_write(perf.status ? perf.status : "ok");
}

void filmsim_perf_begin(filmsim_perf_mode mode)
{
    // A new RAW can arrive before the previous JPEG close in a burst.
    perf_tick();
    if (perf.active)
        perf_write(perf.status ? perf.status : "jpeg_unconfirmed");
    if (!conf.film_performance_metrics || !filmsim_photo_mode() || !perf_recipe_available())
        return;

    memset(&perf, 0, sizeof(perf));
    perf.active = 1;
    perf.mode = mode;
    perf.stage = PERF_NONE;
    for (int i = 0; i < PERF_STAGE_COUNT; i++)
        perf.ms[i] = -1;
    perf.slot = recipe_active();
    sprintf(perf.look, "%.23s", filmsim_name());
    perf.number = shot_path(mode == FILMSIM_PERF_PAIR ? perf.original : perf.custom);
    perf.start_tick = get_tick_count();
}

void filmsim_perf_mark(filmsim_perf_stage stage)
{
    if (!perf.active)
        return;
    unsigned now = get_tick_count();
    if (perf.stage != PERF_NONE) {
        int *ms = &perf.ms[perf.stage];
        if (*ms < 0) *ms = 0;
        *ms += now - perf.stage_tick;
    }
    perf.stage = stage;
    perf.stage_tick = now;
}

void filmsim_perf_raw_done(int original)
{
    if (!perf.active)
        return;
    filmsim_perf_mark(PERF_NONE);
    perf.end_tick = perf.stage_tick;
#ifdef FILMSIM_JPEG_PAIR
    if (!remotecap_get_target()) {
        perf_jpeg_open = 0;
        perf_jpeg_closed = 0;
        perf_jpeg_stage = original ? PERF_ORIGINAL_JPEG : PERF_CUSTOM_JPEG;
        perf_jpeg_number = perf.number;
    }
#else
    (void)original;
#endif
}

//-------------------------------------------------------------------
// Doppelbelichtung
//   Bild 1: Rohdaten (vor Kurven/Mischung) nach A/CHDK/DBL1.RAW schreiben,
//           Bild 1 wird ausserdem normal als JPEG gespeichert.
//   Bild 2: DBL1.RAW stueckweise einlesen und mit den neuen Rohdaten
//           verrechnen, danach laufen Filmkurve/-mischung wie gewohnt und
//           Canon erzeugt das JPEG aus dem gemischten Bild.
//   Mischung wie zwei Belichtungen auf einem Negativ: Summe * 5/8
//   (zwei normal belichtete Bilder ergeben so keine voellig ueberstrahlte
//   Summe, Lichter des einen Bildes zeichnen sich im Dunkel des anderen ab).
//-------------------------------------------------------------------
#include "cachebit.h"
#define DBL_FILE  "A/CHDK/DBL1.RAW"
#if CAM_SENSOR_BITS_PER_PIXEL == 12
#define DBL_CHUNK 60000                     // Vielfaches von 12 Byte (8 Pixel)
#else
#define DBL_CHUNK 64000                     // Vielfaches von 10 Byte (8 Pixel)
#endif

#ifdef FS_RAW
__attribute__((optimize("O2"))) static void dbl_merge(char *raw, int fd)
{
    unsigned char *buf = malloc(DBL_CHUNK);
    if (!buf) { shot_skip |= SKIP_DBL; return; }
    unsigned char *dst = (unsigned char *)raw;
    int total = camera_sensor.raw_size, done = 0;
    while (done < total) {
        int want = total - done;
        if (want > DBL_CHUNK) want = DBL_CHUNK;
        int got = read(fd, buf, want);
        if (got <= 0) break;
        got -= got % FS_GRP;
        int i, k;
        for (i = 0; i < got; i += FS_GRP) {
            int a[8], b[8];
            dec8(dst + done + i, a);
            dec8(buf + i, b);
            for (k = 0; k < 8; k++) {
                if (!a[k]) continue;                    // Defekt-Markierung erhalten
                if (!b[k]) b[k] = a[k];                 // Defekt im 1. Bild: nur 2. Bild nutzen
                int v = BLK + (((a[k] - BLK) + (b[k] - BLK)) * 5 >> 3);
                a[k] = clip10(v) ? clip10(v) : 1;      // nie versehentlich 0 erzeugen
            }
            enc8(dst + done + i, a);
        }
        done += got;
    }
    free(buf);
}
#endif

void filmsim_double_hook(char *raw)
{
#ifdef FS_RAW
    if (!conf.film_double || !raw || !filmsim_photo_mode()) return;
    if (conf.film_double == 1) {
        // erstes Bild sichern
        int fd = open(DBL_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        if (fd < 0) return;
        int w = write(fd, ADR_TO_UNCACHED(raw), camera_sensor.raw_size);
        close(fd);
        if (w == (int)camera_sensor.raw_size) {
            conf.film_double = 2;
            conf_save();
        }
    } else {
        // zweites Bild: mit dem gesicherten ersten verrechnen
        int fd = open(DBL_FILE, O_RDONLY, 0777);
        if (fd >= 0) {
            dbl_merge(raw, fd);
            close(fd);
        }
        remove(DBL_FILE);
        conf.film_double = 1;                    // bereit fuer das naechste Paar
        conf_save();
    }
#else
    (void)raw;
#endif
}

// Menue "Doppelbelichtung: Aus / An"
const char* filmsim_double_enum(int change, __attribute__ ((unused))int arg)
{
    static const int modes[] = { LANG_EXCCD_OFF, LANG_EXCCD_ON };
    if (change) {
        if (conf.film_double) { conf.film_double = 0; remove(DBL_FILE); }
        else conf.film_double = 1;
    }
    return lang_str(modes[conf.film_double ? 1 : 0]);
}

// Menue "Farbfilter (S/W)": wirkt nur bei Schwarzweissfilmen
const char* filmsim_bwfilter_enum(int change, __attribute__ ((unused))int arg)
{
    static const int modes[] = { LANG_EXCCD_OFF, LANG_EXCCD_YELLOW, LANG_EXCCD_ORANGE, LANG_EXCCD_RED };
    if (!bw_look(current())) return lang_str(LANG_EXCCD_BW_ONLY);
    if (change) conf.film_bwfilter = (conf.film_bwfilter + change + 4) % 4;
    return lang_str(modes[conf.film_bwfilter % 4]);
}

const char* filmsim_bwfilter_str(void)
{
    static const int osd[] = { 0, LANG_EXCCD_OSD_YELLOW, LANG_EXCCD_OSD_ORANGE, LANG_EXCCD_OSD_RED };
    if (!bw_look(current()) || !(conf.film_bwfilter % 4)) return 0;
    return lang_str(osd[conf.film_bwfilter % 4]);
}

//===================================================================
// Kreativeffekte (Optionen -> Effekte)
//   Black Mist, Halation, Vignette, Light Leak, Datumsstempel
//   Laufen im RAW-Hook nach Filmkurve und Farbmischung, vor dem JPEG.
//   Leuchteffekte nutzen ein stark verkleinertes, weichgezeichnetes
//   Abbild der hellen Bildteile (wenig Speicher, ein Lese- und ein
//   Schreibdurchgang ueber die Rohdaten).
//===================================================================
#include "raw.h"
#include "time.h"

#ifdef FS_RAW
// Zellgroessen in Pixeln, mit der Sensorbreite wachsend: gleiche Wirkung
// im Bild und hoechstens 256 Zellen je Richtung
#if CAM_RAW_ROWPIX > 4096
#define MIST_BS 48                   // Black Mist
#define HAL_BS  24                   // Halation
#else
#define MIST_BS 32
#define HAL_BS  16
#endif

// Weichzeichnen eines kleinen Gitters (Kastenfilter, mehrfach = annaehernd Gauss)
// tmp: 256 int Arbeitsspeicher. Alle Puffer der Effekte kommen aus malloc, nicht
// static: Die SX110 hat ein hartes Groessenlimit fuer den Kern.
static void grid_blur(unsigned short *g, int w, int h, int r, int passes, int *tmp)
{
    int x, y, p, n = 2 * r + 1;
    if (w > 256 || h > 256) return;
    for (p = 0; p < passes; p++) {
        for (y = 0; y < h; y++) {                     // horizontal
            unsigned short *row = g + y * w;
            for (x = 0; x < w; x++) {
                int s = 0, k;
                for (k = -r; k <= r; k++) {
                    int xx = x + k; if (xx < 0) xx = 0; if (xx >= w) xx = w - 1;
                    s += row[xx];
                }
                tmp[x] = s / n;
            }
            for (x = 0; x < w; x++) row[x] = tmp[x];
        }
        for (x = 0; x < w; x++) {                     // vertikal
            for (y = 0; y < h; y++) {
                int s = 0, k;
                for (k = -r; k <= r; k++) {
                    int yy = y + k; if (yy < 0) yy = 0; if (yy >= h) yy = h - 1;
                    s += g[yy * w + x];
                }
                tmp[y] = s / n;
            }
            for (y = 0; y < h; y++) g[y * w + x] = tmp[y];
        }
    }
}

// Interpolationstabelle: fuer jede Position Zellindex und Anteil (0..255)
static void make_interp(short *idx, unsigned char *frac, int n, int bs, int cells)
{
    int i;
    for (i = 0; i < n; i++) {
        int f = ((i * 256 + 128) / bs) - 128;         // Zellmitte-basiert, *256
        int c = f >> 8;
        int fr = f & 255;
        if (f < 0) { c = 0; fr = 0; }
        if (c >= cells - 1) { c = cells - 1; fr = 0; }
        idx[i] = c; frac[i] = fr;
    }
}

// Sichtbarer JPEG-Ausschnitt in Koordinaten des Rohbilds. camera_sensor.jpeg zaehlt
// ab dem aktiven Sensorbereich (so steht es in der DNG), die Rohdaten ab Zeile und
// Spalte 0 inklusive Dunkelbereich: Beides muss addiert werden.
static void jpeg_rect(int *jx, int *jy, int *jw, int *jh)
{
    *jx = camera_sensor.active_area.x1 + camera_sensor.jpeg.x;
    *jy = camera_sensor.active_area.y1 + camera_sensor.jpeg.y;
    *jw = camera_sensor.jpeg.width;
    *jh = camera_sensor.jpeg.height;
    if (*jw <= 0 || *jh <= 0) { *jx = 0; *jy = 0; *jw = camera_sensor.raw_rowpix; *jh = camera_sensor.raw_rows; }
    if (*jx < 0) *jx = 0;
    if (*jy < 0) *jy = 0;
    if (*jx + *jw > (int)camera_sensor.raw_rowpix) *jw = camera_sensor.raw_rowpix - *jx;
    if (*jy + *jh > (int)camera_sensor.raw_rows) *jh = camera_sensor.raw_rows - *jy;
}

__attribute__((optimize("O2"))) static void effects_glow_and_tone(char *raw)
{
    int do_mist = conf.film_mist > 0, do_hal = conf.film_halation > 0;
    int do_vig = conf.film_vignette > 0, do_leak = conf.film_leak > 0;
    if (!do_mist && !do_hal && !do_vig && !do_leak) return;

    int rows = camera_sensor.raw_rows & ~1;
    int rowpix = camera_sensor.raw_rowpix;
    int groups = rowpix / 8;
    int rowlen = camera_sensor.raw_rowlen;
    int mw = rowpix / MIST_BS, mh = rows / MIST_BS;
    int hw = rowpix / HAL_BS,  hh = rows / HAL_BS;
    if (mw > 256 || hw > 256 || mh > 256 || hh > 256) { shot_skip |= SKIP_FX; return; }

    unsigned short *gm = 0, *gh = 0;                  // Gitter: Mist (3 Kanaele), Halation
    int *acc = 0;
    short *ix_m = 0, *ix_h = 0;
    unsigned char *fx_m = 0, *fx_h = 0, *leak_x = 0;
    unsigned short *vig_x = 0;
    int x, y, k;

    if (do_mist) gm = malloc(3 * mw * mh * sizeof(unsigned short));
    if (do_hal)  gh = malloc(hw * hh * sizeof(unsigned short));
    // acc: Sammeln (3 x 256 + 256), dahinter Gitterzeilen Mist (3 x 256) und Halation (256),
    // Arbeitsspeicher fuer grid_blur (256)
    acc = malloc((3 * 256 + 256 + 3 * 256 + 256 + 256) * sizeof(int));
    ix_m = malloc(rowpix * sizeof(short)); fx_m = malloc(rowpix);
    ix_h = malloc(rowpix * sizeof(short)); fx_h = malloc(rowpix);
    leak_x = malloc(rowpix);
    vig_x = malloc(rowpix * sizeof(unsigned short));
    if ((do_mist && !gm) || (do_hal && !gh) || !acc || !ix_m || !fx_m || !ix_h || !fx_h || !leak_x || !vig_x) {
        shot_skip |= SKIP_FX;
        goto out;
    }

    // Schwellen (Rohwerte vor Weissabgleich, daher R/B niedriger)
    const int thr_m[4] = { BLK + 180, BLK + 340, BLK + 340, BLK + 235 };
    const int thr_h = BLK + 680;

    //---------------- 1. Durchgang: Lichter sammeln ----------------
    if (do_mist || do_hal) {
        int *am = acc, *ah = acc + 3 * 256;
        memset(acc, 0, (3 * 256 + 256) * sizeof(int));
        for (y = 0; y < rows; y++) {
            prog_rows(y, 2 * rows);                     // erste Haelfte des Balkens
            unsigned char *a = (unsigned char *)raw + y * rowlen;
            int odd = (y ^ FS_ROW0) & 1;
            for (k = 0; k < groups; k++, a += FS_GRP) {
                int v[8], j;
                dec8(a, v);
                for (j = 0; j < 8; j++) {
                    int xx = k * 8 + j;
                    int c = (odd << 1) | (xx & 1);   // 0 R, 1/2 G, 3 B
                    int val = v[j];
                    if (do_mist) {
                        int e = val - thr_m[c];
                        int cx = xx / MIST_BS;
                        if (e > 0 && cx < mw) am[(c == 3 ? 2 : (c ? 1 : 0)) * 256 + cx] += e;
                    }
                    if (do_hal && (c == 1 || c == 2)) {
                        int e = val - thr_h;
                        int cx = xx / HAL_BS;
                        if (e > 0 && cx < hw) ah[cx] += e;
                    }
                }
            }
            if (do_mist && ((y + 1) % MIST_BS) == 0 && (y / MIST_BS) < mh) {
                int cy = y / MIST_BS, ch;
                for (ch = 0; ch < 3; ch++)
                    for (x = 0; x < mw; x++) {
                        int cnt = (ch == 1) ? (MIST_BS * MIST_BS / 2) : (MIST_BS * MIST_BS / 4);
                        int val = am[ch * 256 + x] / cnt;
                        gm[(ch * mh + cy) * mw + x] = val > 65535 ? 65535 : val;
                        am[ch * 256 + x] = 0;
                    }
            }
            if (do_hal && ((y + 1) % HAL_BS) == 0 && (y / HAL_BS) < hh) {
                int cy = y / HAL_BS;
                for (x = 0; x < hw; x++) {
                    int val = ah[x] / (HAL_BS * HAL_BS / 2);
                    gh[cy * hw + x] = val > 65535 ? 65535 : val;
                    ah[x] = 0;
                }
            }
        }
        if (do_mist) { int ch; for (ch = 0; ch < 3; ch++) grid_blur(gm + ch * mw * mh, mw, mh, 2, 3, acc + 2048); }
        if (do_hal) grid_blur(gh, hw, hh, 1, 3, acc + 2048);
    }

    //---------------- Tabellen fuer den 2. Durchgang ----------------
    make_interp(ix_m, fx_m, rowpix, MIST_BS, mw);
    make_interp(ix_h, fx_h, rowpix, HAL_BS, hw);

    // Bildmitte / Bildbereich (sichtbarer JPEG-Ausschnitt)
    int jx, jy, jw, jh;
    jpeg_rect(&jx, &jy, &jw, &jh);
    int cxm = jx + jw / 2, cym = jy + jh / 2;
    int norm = ((jw / 2) * (jw / 2) + (jh / 2) * (jh / 2)) / 4096;   // Ecke = 4096
    if (norm < 1) norm = 1;
    for (x = 0; x < rowpix; x++) {
        int dx = x - cxm;
        int t = (dx * dx) / norm; if (t > 8191) t = 8191;
        vig_x[x] = t;
    }
    const int vig_k = conf.film_vignette >= 2 ? 2300 : 1300;            // Abdunklung in der Ecke (/4096)

    // Light Leak: warmer Streifen vom linken oder rechten Rand, Lage zufaellig
    unsigned int rnd = (unsigned int)get_tick_count() * 2654435761u;
    int leak_right = rnd & 1;
    int leak_yc = jy + jh / 5 + (int)((rnd >> 8) % (unsigned)(jh * 3 / 5));
    int leak_hh = jh / 3;
    int leak_w = jw / 3;
    for (x = 0; x < rowpix; x++) {
        int d = leak_right ? (jx + jw - x) : (x - jx);
        int t = (d <= 0) ? 255 : (d >= leak_w ? 0 : 255 - d * 255 / leak_w);
        leak_x[x] = (t * t) >> 8;                                     // weicher Auslauf
    }

    const int mist_k = conf.film_mist >= 2 ? 330 : 170;               // /256
    const int hal_k[4] = { 420, 70, 70, 0 };                          // R stark, etwas G, kein B
    const int leak_k[4] = { 700, 300, 300, 30 };                      // Rohwert-Einheiten bei voller Staerke

    //---------------- 2. Durchgang: anwenden ----------------
    unsigned int dith = rnd | 1;
    int (*rm)[256] = (int (*)[256])(acc + 1024);
    int *rh = acc + 1792;
    for (y = 0; y < rows; y++) {
        if (do_mist || do_hal) prog_rows(rows + y, 2 * rows);
        else                   prog_rows(y, rows);
        int odd = (y ^ FS_ROW0) & 1;
        // vertikal interpolierte Gitterzeilen fuer diese Rohdatenzeile
        if (do_mist) {
            int f = ((y * 256 + 128) / MIST_BS) - 128, c0 = f >> 8, fr = f & 255, ch;
            if (f < 0) { c0 = 0; fr = 0; }
            if (c0 >= mh - 1) { c0 = mh - 1; fr = 0; }
            int c1 = (c0 + 1 < mh) ? c0 + 1 : c0;
            for (ch = 0; ch < 3; ch++)
                for (x = 0; x < mw; x++) {
                    unsigned short *g0 = gm + (ch * mh) * mw;
                    rm[ch][x] = (g0[c0 * mw + x] * (256 - fr) + g0[c1 * mw + x] * fr) >> 8;
                }
        }
        if (do_hal) {
            int f = ((y * 256 + 128) / HAL_BS) - 128, c0 = f >> 8, fr = f & 255;
            if (f < 0) { c0 = 0; fr = 0; }
            if (c0 >= hh - 1) { c0 = hh - 1; fr = 0; }
            int c1 = (c0 + 1 < hh) ? c0 + 1 : c0;
            for (x = 0; x < hw; x++)
                rh[x] = (gh[c0 * hw + x] * (256 - fr) + gh[c1 * hw + x] * fr) >> 8;
        }
        int dy = y - cym;
        int vy = (dy * dy) / norm;
        int ly = 0;
        if (do_leak) {
            int d = y - leak_yc; if (d < 0) d = -d;
            ly = (d >= leak_hh) ? 0 : 255 - d * 255 / leak_hh;
            ly = (ly * ly) >> 8;
        }

        unsigned char *a = (unsigned char *)raw + y * rowlen;
        for (k = 0; k < groups; k++, a += FS_GRP) {
            int v[8], j;
            dec8(a, v);
            for (j = 0; j < 8; j++) {
                if (!v[j]) continue;                     // Defekt-Markierung erhalten
                int xx = k * 8 + j;
                int c = (odd << 1) | (xx & 1);
                int add = 0;
                if (do_mist) {
                    int ch = (c == 3) ? 2 : (c ? 1 : 0);
                    int i0 = ix_m[xx], fr = fx_m[xx];
                    int i1 = (i0 + 1 < mw) ? i0 + 1 : i0;
                    int g = (rm[ch][i0] * (256 - fr) + rm[ch][i1] * fr) >> 8;
                    add += (g * mist_k) >> 8;
                }
                if (do_hal) {
                    int i0 = ix_h[xx], fr = fx_h[xx];
                    int i1 = (i0 + 1 < hw) ? i0 + 1 : i0;
                    int g = (rh[i0] * (256 - fr) + rh[i1] * fr) >> 8;
                    add += (g * hal_k[c]) >> 8;
                }
                if (do_leak && ly)
                    add += (leak_k[c] * ((leak_x[xx] * ly) >> 8)) >> 8;
                int val = v[j] + add;
                if (do_vig) {
                    int r2 = vig_x[xx] + vy; if (r2 > 8191) r2 = 8191;
                    int f = 4096 - ((vig_k * r2) >> 12);
                    if (f < 0) f = 0;
                    dith = dith * 1103515245u + 12345u;          // Zufalls-Dither gegen Stufen
                    val = BLK + (((val - BLK) * f + (int)((dith >> 16) & 4095)) >> 12);
                }
                val = clip10(val);
                v[j] = val ? val : 1;
            }
            enc8(a, v);
        }
    }

out:
    if (gm) free(gm);
    if (gh) free(gh);
    if (acc) free(acc);
    if (ix_m) free(ix_m);
    if (fx_m) free(fx_m);
    if (ix_h) free(ix_h);
    if (fx_h) free(fx_h);
    if (leak_x) free(leak_x);
    if (vig_x) free(vig_x);
}

//---------------- Datumsstempel ----------------
// Orange 7-Segment-Ziffern, leicht kursiv, unten rechts: '26 10 25
static void stamp_rect(int x0, int y0, int w, int h, int slant_base, int slant)
{
    // Orange in Rohwerten (vor Weissabgleich): R hoch, G mittel, B niedrig
    static const unsigned short col[4] = { 620, 300, 300, 45 };
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        int off = ((slant_base - y) * slant) >> 8;    // kursiv: oben weiter rechts
        for (x = x0 + off; x < x0 + off + w; x++) {
            if (x < 0 || y < 0 || x >= (int)camera_sensor.raw_rowpix || y >= (int)camera_sensor.raw_rows) continue;
            int c = (((y ^ FS_ROW0) & 1) << 1) | (x & 1);
            if (get_raw_pixel(x, y) == 0) continue;     // Defekt-Markierung erhalten
            set_raw_pixel(x, y, col[c] << (CAM_SENSOR_BITS_PER_PIXEL - 10));
        }
    }
}

static void stamp_char(int x, int y, int H, int W, int T, char ch, int slant)
{
    static const unsigned char seg[10] = { 0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F };
    int base = y + H;
    if (ch == '\'') { stamp_rect(x + W / 3, y, T, H / 3, base, slant); return; }
    if (ch < '0' || ch > '9') return;
    int m = seg[ch - '0'], h2 = (H - T) / 2;
    if (m & 0x01) stamp_rect(x + T, y,               W - 2 * T, T, base, slant);   // a oben
    if (m & 0x02) stamp_rect(x + W - T, y + T,       T, h2 - T / 2, base, slant);  // b rechts oben
    if (m & 0x04) stamp_rect(x + W - T, y + h2 + T,  T, h2 - T / 2, base, slant);  // c rechts unten
    if (m & 0x08) stamp_rect(x + T, y + H - T,       W - 2 * T, T, base, slant);   // d unten
    if (m & 0x10) stamp_rect(x, y + h2 + T,          T, h2 - T / 2, base, slant);  // e links unten
    if (m & 0x20) stamp_rect(x, y + T,               T, h2 - T / 2, base, slant);  // f links oben
    if (m & 0x40) stamp_rect(x + T, y + h2,          W - 2 * T, T, base, slant);   // g Mitte
}

//---------------- Filmkorn ----------------
// Staerke im fertigen Bild in 0,1 %. Auto richtet sich nach der Zahl im Filmnamen.
static int grain_pct10(void)
{
    static const short fixed[] = { 0, 0, 7, 14, 24 };
    if (conf.film_grain != 1) return (conf.film_grain > 1 && conf.film_grain < 5) ? fixed[conf.film_grain] : 0;
    const char *n = current()->name, *e = n + strlen(n);
    int speed = 0, mul = 1;
    while (e > n && e[-1] >= '0' && e[-1] <= '9') { speed += (*--e - '0') * mul; mul *= 10; }
    if (speed <= 0) return 9;
    return speed <= 100 ? 6 : speed <= 200 ? 9 : speed <= 400 ? 13 : speed <= 800 ? 18 : 28;
}

__attribute__((optimize("O2"))) static void effects_grain(char *raw)
{
    int pct = grain_pct10();
    if (!pct) return;
    int nb = (camera_sensor.raw_rowpix / 8) * 4;
    short *amp = malloc(GRAIN_AMP_N * sizeof(short));
    short *up = malloc(nb * sizeof(short));
    if (!amp || !up) shot_skip |= SKIP_GRAIN;
    if (amp && up) {
        grain_amp_build(amp, pct);
        grain_apply((unsigned char *)raw, camera_sensor.raw_rows, camera_sensor.raw_rowpix, camera_sensor.raw_rowlen,
                    FS_ROW0, 65536 / WB_R, 65536 / WB_B, amp, up, (unsigned int)get_tick_count() * 2654435761u);
    }
    if (amp) free(amp);
    if (up) free(up);
}

//---------------- Filmrahmen ----------------
static void effects_frame(char *raw)
{
    if (!conf.film_frame) return;       // auch 2 (Negative aus dem Lab-Build) gilt als Thin
    int jx, jy, jw, jh;
    jpeg_rect(&jx, &jy, &jw, &jh);
    // Etwas ueber den Ausschnitt hinaus (nur im aktiven Bereich): Falls Canons JPEG
    // ein paar Pixel groesser ist, bleibt kein Bildstreifen ausserhalb des Rahmens.
    {
        int ax = camera_sensor.active_area.x1, ay = camera_sensor.active_area.y1;
        int ex = camera_sensor.active_area.x2, ey = camera_sensor.active_area.y2;
        int x1 = jx + jw + 8, y1 = jy + jh + 8;
        if (ex > ax && ey > ay) {
            jx = jx - 8 < ax ? ax : jx - 8;
            jy = jy - 8 < ay ? ay : jy - 8;
            jw = (x1 > ex ? ex : x1) - jx;
            jh = (y1 > ey ? ey : y1) - jy;
        }
    }
    static const short black[4] = { BLK, BLK, BLK, BLK };
    frame_draw((unsigned char *)raw, camera_sensor.raw_rowlen, FS_ROW0, jx, jy, jw, jh, black);
}

static void effects_datestamp(void)
{
    if (!conf.film_datestamp) return;
    struct tm *t = get_localtime();
    if (!t) return;
    char s[32];
    if (dev_active && dev_has_meta && dev_meta.year)    // Entwickeln: Tag der Aufnahme
        sprintf(s, "'%02d %02d %02d", dev_meta.year % 100, dev_meta.mon, dev_meta.day);
    else
        sprintf(s, "'%02d %02d %02d", t->tm_year % 100, t->tm_mon + 1, t->tm_mday);
    int jx, jy, jw, jh;
    jpeg_rect(&jx, &jy, &jw, &jh);
    int H = jh / 24, W = H * 11 / 20, T = H / 8, gap = W * 2 / 5;
    if (T < 2) T = 2;
    int n = strlen(s), i, total = 0;
    for (i = 0; i < n; i++) total += (s[i] == '\'' ? W / 2 : (s[i] == ' ' ? W / 2 : W)) + gap;
    int x = jx + jw - jw / 14 - total;
    int y = jy + jh - jh / 12 - H;
    for (i = 0; i < n; i++) {
        stamp_char(x, y, H, W, T, s[i], 40);
        x += (s[i] == '\'' ? W / 2 : (s[i] == ' ' ? W / 2 : W)) + gap;
    }
}
#endif

//-------------------------------------------------------------------
// Farbrauschen-Filter (Chroma-Rauschunterdrueckung)
//   Trennt je 2x2-Bayer-Block Helligkeit (Gruen) und Farbe (Rot-Gruen,
//   Blau-Gruen). Nur die Farbanteile werden ueber die Nachbarbloecke
//   gemittelt, Gruen und damit die Bilddetails bleiben unangetastet.
//   Bunte Flecken verschwinden, das feine Helligkeitsrauschen (wie Korn)
//   bleibt erhalten.
//-------------------------------------------------------------------
#ifdef FS_RAW
#define CNR_RMAX 4
static int *cnr_recip = 0;                           // 65536 / e, 512 Eintraege (malloc, bleibt)
#define CNR_CLIP 1000

// Farbanteile einer Blockzeile berechnen (aus den Originaldaten)
__attribute__((optimize("O2")))
static void cnr_row(const char *raw, int j, int nb, short *cr, short *cb, short *cl)
{
    int rowlen = camera_sensor.raw_rowlen, groups = nb / 4, g, k, i = 0;
    const unsigned char *a0 = (const unsigned char *)raw + (2 * j + FS_ROW0) * rowlen;
    const unsigned char *a1 = a0 + rowlen;
    int lr = 0, lb = 0, ll = 0;
    for (g = 0; g < groups; g++, a0 += FS_GRP, a1 += FS_GRP) {
        int p[8], q[8];
        dec8(a0, p); dec8(a1, q);
        for (k = 0; k < 8; k += 2, i++) {
            int R = p[k], G1 = p[k+1], G2 = q[k], B = q[k+1];
            if (!R || !G1 || !G2 || !B || R >= CNR_CLIP || G1 >= CNR_CLIP || G2 >= CNR_CLIP || B >= CNR_CLIP) {
                cr[i] = lr; cb[i] = lb; cl[i] = ll;   // Defekt/Clipping: Nachbarwert
                continue;
            }
            int gg = ((G1 + G2) >> 1) - BLK;
            ll = gg;
            lr = (((R - BLK) * WB_R) >> 8) - gg;
            lb = (((B - BLK) * WB_B) >> 8) - gg;
            cr[i] = lr; cb[i] = lb; cl[i] = ll;
        }
    }
}

__attribute__((optimize("O2")))
static void filmsim_cnr_apply(char *raw)
{
    int level = cnr_level();
    int R = level == 1 ? 2 : (level == 2 ? CNR_RMAX : 0);
    if (!R || !raw || !filmsim_photo_mode()) return;
    int RB = 2 * R + 1;
    int nb = (camera_sensor.raw_rowpix / 8) * 4;     // Bloecke pro Zeile
    int nbr = (camera_sensor.raw_rows - FS_ROW0) / 2; // Blockzeilen
    int rowlen = camera_sensor.raw_rowlen;
    if (!wbdiv_init()) { shot_skip |= SKIP_CNR; return; }
    short *ring_r = malloc(RB * nb * sizeof(short));
    short *ring_b = malloc(RB * nb * sizeof(short));
    short *ring_l = malloc(RB * nb * sizeof(short));
    int *vr = malloc(nb * sizeof(int));
    int *vb = malloc(nb * sizeof(int));
    int *vl = malloc(nb * sizeof(int));
    if (!ring_r || !ring_b || !ring_l || !vr || !vb || !vl) { shot_skip |= SKIP_CNR; goto out; }
    const int dmax = (R == 2) ? 170 : 250;           // ab dieser Farbabweichung: Kante, nicht Rauschen
    const int invd = 65536 / dmax;
    if (!cnr_recip && (cnr_recip = malloc(512 * sizeof(int)))) { int t; cnr_recip[0] = 0; for (t = 1; t < 512; t++) cnr_recip[t] = 65536 / t; }
    if (!cnr_recip) { shot_skip |= SKIP_CNR; goto out; }
    int inv = 65536 / (RB * RB);
    int computed = 0, j, i, d;

    for (j = 0; j < nbr; j++) {
        prog_rows(j, nbr);
        // benoetigte Zeilen bis j+R berechnen (Originaldaten, noch unveraendert)
        int need = j + R; if (need > nbr - 1) need = nbr - 1;
        while (computed <= need) {
            cnr_row(raw, computed, nb, ring_r + (computed % RB) * nb, ring_b + (computed % RB) * nb,
                    ring_l + (computed % RB) * nb);
            computed++;
        }
        // vertikale Summen
        for (i = 0; i < nb; i++) { vr[i] = 0; vb[i] = 0; vl[i] = 0; }
        for (d = -R; d <= R; d++) {
            int jj = j + d; if (jj < 0) jj = 0; if (jj > nbr - 1) jj = nbr - 1;
            short *sr = ring_r + (jj % RB) * nb, *sb = ring_b + (jj % RB) * nb, *sl = ring_l + (jj % RB) * nb;
            for (i = 0; i < nb; i++) { vr[i] += sr[i]; vb[i] += sb[i]; vl[i] += sl[i]; }
        }
        short *own_r = ring_r + (j % RB) * nb, *own_b = ring_b + (j % RB) * nb;
        // horizontal gleitend summieren und Block schreiben
        int sumr = 0, sumb = 0, suml = 0;
        for (d = -R; d <= R; d++) { int ii = d < 0 ? 0 : d; sumr += vr[ii]; sumb += vb[ii]; suml += vl[ii]; }
        unsigned char *a0 = (unsigned char *)raw + (2 * j + FS_ROW0) * rowlen;
        unsigned char *a1 = a0 + rowlen;
        int g, k; i = 0;
        for (g = 0; g < nb / 4; g++, a0 += FS_GRP, a1 += FS_GRP) {
            int p[8], q[8], changed = 0;
            dec8(a0, p); dec8(a1, q);
            for (k = 0; k < 8; k += 2, i++) {
                int Rv = p[k], G1 = p[k+1], G2 = q[k], Bv = q[k+1];
                if (Rv && G1 && G2 && Bv && Rv < CNR_CLIP && G1 < CNR_CLIP && G2 < CNR_CLIP && Bv < CNR_CLIP) {
                    int gg = ((G1 + G2) >> 1) - BLK;
                    int crs = (sumr * inv + 32768) >> 16;
                    int cbs = (sumb * inv + 32768) >> 16;
                    int mgg = (suml * inv + 32768) >> 16;
                    // Kantenschutz: weicht die Helligkeit oder Farbe der Umgebung
                    // stark ab, liegt eine Kante in der Naehe -> weniger glaetten
                    int e = mgg / 3; if (e < 40) e = 40; if (e > 511) e = 511;
                    int dl = gg - mgg; if (dl < 0) dl = -dl;
                    int w = 256 - ((dl * cnr_recip[e]) >> 8);  // 0..256 (ohne Division)
                    int dr_ = crs - own_r[i], db_ = cbs - own_b[i];
                    int dc = (dr_ < 0 ? -dr_ : dr_) + (db_ < 0 ? -db_ : db_);
                    int w2 = 256 - ((dc * invd) >> 8);
                    if (w2 < w) w = w2;
                    if (w < 0) w = 0;
                    crs = own_r[i] + ((dr_ * w) >> 8);
                    cbs = own_b[i] + ((db_ * w) >> 8);
                    int nr = clip10(div_wb_r(gg + crs) + BLK);
                    int nbv = clip10(div_wb_b(gg + cbs) + BLK);
                    p[k] = nr ? nr : 1;
                    q[k+1] = nbv ? nbv : 1;
                    changed = 1;
                }
                // Fenster eins weiter schieben
                int add = i + R + 1; if (add > nb - 1) add = nb - 1;
                int sub = i - R;     if (sub < 0) sub = 0;
                sumr += vr[add] - vr[sub];
                sumb += vb[add] - vb[sub];
                suml += vl[add] - vl[sub];
            }
            if (changed) { enc8(a0, p); enc8(a1, q); }
        }
    }
out:
    if (ring_r) free(ring_r);
    if (ring_b) free(ring_b);
    if (ring_l) free(ring_l);
    if (vr) free(vr);
    if (vb) free(vb);
    if (vl) free(vl);
}
#else
static void filmsim_cnr_apply(__attribute__ ((unused))char *raw) {}
#endif

void filmsim_cnr_hook(char *raw)
{
    filmsim_cnr_apply(raw);
}

//-------------------------------------------------------------------
// Heisse Pixel entfernen (siehe hotpix_fix in filmsim_raw.h), sobald
// EXCCD das Rohbild veraendert. Bei "Aus" ohne Extras bleibt alles bei Canon.
//-------------------------------------------------------------------
// Veraendert EXCCD das Rohbild ueberhaupt? (bw: Farbfilter fuer Schwarzweiss aktiv)
static int raw_touched(int *bw)
{
    extern int filmsim_effects_active(void);
    const filmsim_t *s = current();
    int bw_filter = bw_look(s) && conf.film_bwfilter > 0 && conf.film_bwfilter < 4;
    if (bw) *bw = bw_filter;
    return s->curve || s->mix || bw_filter || conf.film_dr || cnr_level() || conf.film_double
        || filmsim_effects_active();
}

void filmsim_hotpix_hook(char *raw)
{
#ifdef FS_RAW
    if (!raw || !filmsim_photo_mode()) return;
    // Daylight 100 hat keine Kurve und keine Mischung: Das Rohbild bleibt wie bei "Aus"
    if (!raw_touched(0))
        return;
    short *buf = malloc(HP_BUF_SHORTS(camera_sensor.raw_rowpix) * sizeof(short));
    if (!buf) { shot_skip |= SKIP_HOT; return; }
    hotpix_fix((unsigned char *)raw, camera_sensor.raw_rows, camera_sensor.raw_rowpix,
               camera_sensor.raw_rowlen, buf);
    free(buf);
#else
    (void)raw;
#endif
}

const char* filmsim_cnr_enum(int change, __attribute__ ((unused))int arg)
{
    // Anzeige-Reihenfolge Aus, Auto, Leicht, Stark -> gespeicherter Wert
    static const int m[] = { LANG_EXCCD_OFF, LANG_EXCCD_AUTO, LANG_EXCCD_LIGHT, LANG_EXCCD_STRONG };
    static const int   val[] = { 0, CNR_AUTO, 1, 2 };
    int i;
    for (i = 0; i < 4 && val[i] != conf.film_cnr; i++);
    if (i == 4) i = 0;
    if (change) { i = (i + change + 4) % 4; conf.film_cnr = val[i]; }
    return lang_str(m[i]);
}

void filmsim_effects_apply(char *raw)
{
#ifdef FS_RAW
    if (!raw || !filmsim_photo_mode()) return;
    filmsim_perf_mark(PERF_GLOW_TONE);
    effects_glow_and_tone(raw);
    filmsim_perf_mark(PERF_GRAIN);
    filmsim_prog_step(PS_GRAIN);
    effects_grain(raw);
    filmsim_perf_mark(PERF_FRAME);
    effects_frame(raw);
    filmsim_perf_mark(PERF_DATE_STAMP);
    effects_datestamp();
    filmsim_perf_mark(PERF_NONE);
#else
    (void)raw;
#endif
}

// Menue-Eintraege
const char* filmsim_mist_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_OFF, (int)"1/8", (int)"1/4" };
    if (change) conf.film_mist = (conf.film_mist + change + 3) % 3;
    return lang_str(m[conf.film_mist % 3]);
}
const char* filmsim_halation_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_OFF, LANG_EXCCD_ON };
    if (change) conf.film_halation = !conf.film_halation;
    return lang_str(m[conf.film_halation ? 1 : 0]);
}
const char* filmsim_vignette_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_OFF, LANG_EXCCD_LIGHT, LANG_EXCCD_STRONG };
    if (change) conf.film_vignette = (conf.film_vignette + change + 3) % 3;
    return lang_str(m[conf.film_vignette % 3]);
}
const char* filmsim_leak_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_OFF, LANG_EXCCD_ON };
    if (change) conf.film_leak = !conf.film_leak;
    return lang_str(m[conf.film_leak ? 1 : 0]);
}
const char* filmsim_datestamp_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_OFF, LANG_EXCCD_ON };
    if (change) conf.film_datestamp = !conf.film_datestamp;
    return lang_str(m[conf.film_datestamp ? 1 : 0]);
}
int filmsim_effects_active(void)
{
    return conf.film_mist || conf.film_halation || conf.film_vignette || conf.film_leak || conf.film_datestamp
        || conf.film_grain || conf.film_frame;
}
const char* filmsim_grain_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_OFF, LANG_EXCCD_AUTO, LANG_EXCCD_FINE, LANG_EXCCD_MEDIUM, LANG_EXCCD_COARSE };
    if (change) conf.film_grain = (conf.film_grain + change + 5) % 5;
    return lang_str(m[conf.film_grain % 5]);
}
const char* filmsim_frame_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_OFF, LANG_EXCCD_THIN };
    if (change) conf.film_frame = !conf.film_frame;
    return lang_str(m[conf.film_frame ? 1 : 0]);
}

//-------------------------------------------------------------------
// Weissabgleich (Canon, PROPCASE_WB_MODE)
//   Die Propsets 1, 2, 3, 5 und 6 zaehlen gleich: 0 Auto, 1 Tageslicht,
//   2 Bewoelkt, 3 Kunstlicht, 4 Leuchtstoff, 5 Leuchtstoff H, 7 Manuell.
//   Gesetzt wird nur auf Wunsch (Menue, Rezept), nicht dauerhaft erzwungen:
//   Wer danach im FUNC-Menue umstellt, behaelt seine Wahl.
//-------------------------------------------------------------------
#if defined(PROPCASE_WB_MODE) && (CAM_PROPSET <= 3 || CAM_PROPSET == 5 || CAM_PROPSET == 6)
#define FS_WB_MODE 1
static const signed char wb_prop[RECIPE_WB_N] = { 0, 1, 2, 3, 4, 5, 7 };

// RECIPE_WB_*, RECIPE_WB_KEEP = unbekannter Wert (Blitz, Unterwasser ...)
static int wb_mode_get(void)
{
    int p = shooting_get_prop(PROPCASE_WB_MODE), i;
    for (i = 0; i < RECIPE_WB_N; i++)
        if (wb_prop[i] == p) return i;
    return RECIPE_WB_KEEP;
}

static void wb_mode_set(int i)
{
    if (i < 0 || i >= RECIPE_WB_N || !filmsim_photo_mode()) return;
    if (shooting_get_prop(PROPCASE_WB_MODE) == wb_prop[i]) return;
    shooting_set_prop(PROPCASE_WB_MODE, wb_prop[i]);
    preview_pending = 1;
}
#else
static int wb_mode_get(void) { return RECIPE_WB_KEEP; }
static void wb_mode_set(__attribute__ ((unused))int i) {}
#endif

const char* filmsim_wb_enum(int change, __attribute__ ((unused))int arg)
{
#ifdef FS_WB_MODE
    static const int names[RECIPE_WB_N] = { LANG_EXCCD_AUTO, LANG_EXCCD_WB_DAYLIGHT, LANG_EXCCD_WB_CLOUDY,
        LANG_EXCCD_WB_TUNGSTEN, LANG_EXCCD_WB_FLUOR, LANG_EXCCD_WB_FLUOR_H, LANG_EXCCD_WB_CUSTOM };
    static char other[8];
    int i = wb_mode_get();
    if (change) {
        i = i < 0 ? 0 : (i + change + RECIPE_WB_N) % RECIPE_WB_N;
        wb_mode_set(i);
    }
    if (i >= 0) return lang_str(names[i]);
    sprintf(other, "(%d)", shooting_get_prop(PROPCASE_WB_MODE));
    return other;
#else
    (void)change;
    return "-";
#endif
}

static void shift_str(char *b, int v)
{
    sprintf(b, v > 0 ? "+%d" : "%d", v);
}

static void wb_shift_change(int *v, int change)
{
    int n = wb_shift_clamp(*v + change);
    if (n == *v) return;
    *v = n;
    set_curve(current());
}

const char* filmsim_wb_r_enum(int change, __attribute__ ((unused))int arg)
{
    static char b[8];
    if (change) wb_shift_change(&conf.film_wb_r, change);
    shift_str(b, conf.film_wb_r);
    return b;
}

const char* filmsim_wb_b_enum(int change, __attribute__ ((unused))int arg)
{
    static char b[8];
    if (change) wb_shift_change(&conf.film_wb_b, change);
    shift_str(b, conf.film_wb_b);
    return b;
}


//-------------------------------------------------------------------
// WB-Shift als Raster wie bei Fujifilm (Menue-Eintrag "WB shift")
//   waagerecht Rot (links Cyan, rechts Rot), senkrecht Blau (oben Blau,
//   unten Gelb), je -9..+9. Pfeiltasten verschieben, SET uebernimmt,
//   MENU bricht ab, Zoom setzt auf 0. Die Kurve wird erst bei SET neu
//   geladen, nicht bei jedem Schritt.
//-------------------------------------------------------------------
#define WBG_N       19                  // Felder je Achse
static int wbg_r, wbg_b;                // Cursor
static int wbg_draw_all;
static int wbg_shown_r = 99, wbg_shown_b = 99;  // zuletzt gezeichneter Cursor (99 = keiner)
static gui_handler *wbg_prev;
static int wbg_armed;                  // SET erst nach dem Loslassen (sonst schliesst die Wiederholung)

static void wbg_value_str(char *b, int r, int bl)
{
    char v[8];
    strcpy(b, "R"); shift_str(v, r); strcat(b, v);
    strcat(b, " B"); shift_str(v, bl); strcat(b, v);
}

static void wbg_draw(int force)
{
    int w = camera_screen.width, h = camera_screen.height;
    int cell = (h - 3 * FONT_HEIGHT - 20) / WBG_N;
    if (cell > 12) cell = 12;
    if (cell < 5) cell = 5;
    int gs = cell * WBG_N;
    int gx = (w - gs) / 2, gy = FONT_HEIGHT + 10;
    int i;
    char b[24];
    if (force) wbg_draw_all = 1;
    if (wbg_draw_all) {
        wbg_draw_all = 0;
        draw_rectangle(0, 0, w - 1, h - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0|DRAW_FILLED);
        draw_string(8, 4, lang_str(LANG_EXCCD_WB_SHIFT), MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
        // Farbleisten an den Raendern: zeigen, wohin eine Richtung faerbt
        draw_rectangle(gx, gy - 5, gx + gs - 1, gy - 3, MAKE_COLOR(COLOR_BLUE, COLOR_BLUE), RECT_BORDER0|DRAW_FILLED);
        draw_rectangle(gx, gy + gs + 2, gx + gs - 1, gy + gs + 4, MAKE_COLOR(COLOR_YELLOW, COLOR_YELLOW), RECT_BORDER0|DRAW_FILLED);
        draw_rectangle(gx - 5, gy, gx - 3, gy + gs - 1, MAKE_COLOR(COLOR_CYAN, COLOR_CYAN), RECT_BORDER0|DRAW_FILLED);
        draw_rectangle(gx + gs + 2, gy, gx + gs + 4, gy + gs - 1, MAKE_COLOR(COLOR_RED, COLOR_RED), RECT_BORDER0|DRAW_FILLED);
        draw_string(gx - 7 - 2 * FONT_WIDTH, gy + gs / 2 - FONT_HEIGHT / 2, "C", MAKE_COLOR(COLOR_BLACK, COLOR_CYAN));
        draw_string(gx + gs + 7 + FONT_WIDTH, gy + gs / 2 - FONT_HEIGHT / 2, "R", MAKE_COLOR(COLOR_BLACK, COLOR_RED));
        draw_string(gx + gs + 7 + FONT_WIDTH, gy, "B", MAKE_COLOR(COLOR_BLACK, COLOR_BLUE));
        draw_string(gx + gs + 7 + FONT_WIDTH, gy + gs - FONT_HEIGHT, "Y", MAKE_COLOR(COLOR_BLACK, COLOR_YELLOW));
        draw_string(8, h - FONT_HEIGHT - 4, lang_str(LANG_EXCCD_WB_SHIFT_HELP), MAKE_COLOR(COLOR_BLACK, EX_GREY));
        // Raster nur einmal zeichnen - die Redraw-Funktion laeuft bei jedem
        // GUI-Takt, ein komplettes Neuzeichnen dort flimmert sichtbar
        for (i = 0; i <= WBG_N; i++) {
            int c = (i == WBG_N / 2 || i == WBG_N / 2 + 1) ? EX_GREY : EX_RULE;
            draw_hline(gx, gy + i * cell - (i == WBG_N), gs, c);
            draw_vline(gx + i * cell - (i == WBG_N), gy, gs, c);
        }
        wbg_shown_r = wbg_shown_b = 99;     // Cursor/Wert neu zeichnen
    }
    if (wbg_r == wbg_shown_r && wbg_b == wbg_shown_b) return;
    // alten Cursor loeschen: nur das Innere des Feldes, die Linien bleiben
    if (wbg_shown_r != 99) {
        int ox = gx + (wbg_shown_r + 9) * cell, oy = gy + (9 - wbg_shown_b) * cell;
        draw_rectangle(ox + 1, oy + 1, ox + cell - 1, oy + cell - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0|DRAW_FILLED);
    }
    // Mitte (0/0) markieren (liegt im Inneren des Mittelfeldes), Cursor als weisses Feld
    int mx = gx + (WBG_N / 2) * cell, my = gy + (WBG_N / 2) * cell;
    draw_rectangle(mx + cell / 2 - 1, my + cell / 2 - 1, mx + cell / 2, my + cell / 2,
                   MAKE_COLOR(EX_GREY, EX_GREY), RECT_BORDER0|DRAW_FILLED);
    int cx = gx + (wbg_r + 9) * cell, cy = gy + (9 - wbg_b) * cell;
    draw_rectangle(cx + 1, cy + 1, cx + cell - 1, cy + cell - 1, MAKE_COLOR(COLOR_WHITE, COLOR_WHITE), RECT_BORDER0|DRAW_FILLED);
    wbg_value_str(b, wbg_r, wbg_b);
    strcat(b, "   ");
    draw_string(w - 8 - 10 * FONT_WIDTH, 4, b, MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
    wbg_shown_r = wbg_r; wbg_shown_b = wbg_b;
}

static void wbg_close(int apply)
{
    if (apply && (wbg_r != conf.film_wb_r || wbg_b != conf.film_wb_b)) {
        conf.film_wb_r = wbg_r;
        conf.film_wb_b = wbg_b;
        set_curve(current());
        conf_save();
    }
    gui_set_mode(wbg_prev);
    gui_menu_erase_and_redraw();
}

static int wbg_kbd(void)
{
    int k = kbd_get_autoclicked_key(), r = wbg_r, b = wbg_b;
    if (!kbd_is_key_pressed(KEY_SET)) wbg_armed = 1;
    switch (k) {
    case KEY_LEFT:  r--; break;
    case KEY_RIGHT: r++; break;
    case KEY_UP:    b++; break;
    case KEY_DOWN:  b--; break;
    case KEY_ZOOM_IN: case KEY_ZOOM_OUT: case KEY_DISPLAY: r = b = 0; break;
    case KEY_SET:   if (wbg_armed) wbg_close(1); return 0;
    }
    r = wb_shift_clamp(r); b = wb_shift_clamp(b);
    if (r != wbg_r || b != wbg_b) { wbg_r = r; wbg_b = b; wbg_draw(0); }
    return 0;
}

static void wbg_menu_btn(void)
{
    wbg_close(0);
}

static gui_handler wbgGuiHandler = { GUI_MODE_MODULE, wbg_draw, wbg_kbd, wbg_menu_btn, 0, 0 };

// Menue-Eintrag: zeigt "R+2 B-1", jede Taste (links/rechts/SET) oeffnet das Raster
const char* filmsim_wb_shift_enum(int change, __attribute__ ((unused))int arg)
{
    static char b[24];
    if (!conf.film_sim) return lang_str(LANG_EXCCD_NEEDS_LOOK);   // "Aus": keine Kurve, keine Wirkung
    if (change) {
        wbg_r = wb_shift_clamp(conf.film_wb_r);
        wbg_b = wb_shift_clamp(conf.film_wb_b);
        wbg_draw_all = 1;
        wbg_armed = 0;
        wbg_prev = gui_set_mode(&wbgGuiHandler);
    }
    if (!conf.film_wb_r && !conf.film_wb_b) strcpy(b, "0");
    else wbg_value_str(b, conf.film_wb_r, conf.film_wb_b);
    return b;
}
//-------------------------------------------------------------------
// Rezepte (A/CHDK/FILMS/RECIPES.TXT, Plaetze C1..C4)
//   Ein Rezept stellt Film, Weissabgleich, WB-Verschiebung, Dynamik+,
//   Farbrauschen, S/W-Filter und die Effekte auf einmal ein. Kamera-
//   Einstellungen fuer die Situation (Zeit, Fokus, Zoom, Blitz, ND, RAW,
//   Doppelbelichtung) gehoeren nicht dazu.
//   conf.film_recipe merkt sich das zuletzt geladene; weicht etwas davon ab,
//   zeigen Menue und Statuszeile ein * dahinter.
//-------------------------------------------------------------------
#define RECIPES_TXT "A/CHDK/FILMS/RECIPES.TXT"
#define RECIPES_LOG "A/CHDK/FILMS/RECIPES.LOG"

static recipe_t recipes[RECIPE_SLOTS];
static int recipes_state = 0;           // 0 ungelesen, 1 gelesen, -1 Datei unlesbar (nicht ueberschreiben)
static int recipe_save_slot = 0;

static int name_same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

// Grund in RECIPES.LOG schreiben, wenn Laden oder Speichern scheitert
// (die Kamera zeigt nur "Save failed")
static void recipes_log(const char *msg)
{
    int fd = open(RECIPES_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd >= 0) { write(fd, msg, strlen(msg)); close(fd); }
}

static void recipes_load(void)
{
    struct stat st;
    char *buf, *log;
    int len = -1;
    if (recipes_state) return;
    recipes_state = 1;
    for (len = 0; len < RECIPE_SLOTS; len++) recipe_clear(&recipes[len]);
    if (stat(RECIPES_TXT, &st) != 0) return;
    if (st.st_size <= 0) return;        // leer: wie keine Datei, Speichern darf sie fuellen
    if (st.st_size > RECIPE_FILE_MAX) {
        recipes_state = -1;
        recipes_log("RECIPES.TXT over 8 KB: kept, saving off\r\n");
        return;
    }
    if (!(buf = malloc(st.st_size + 1 + 512))) {
        recipes_state = 0;              // beim naechsten Mal neu versuchen
        recipes_log("Low memory: RECIPES.TXT not read\r\n");
        return;
    }
    log = buf + st.st_size + 1;
    int fd = open(RECIPES_TXT, O_RDONLY, 0777);
    len = -1;
    if (fd >= 0) {
        len = read(fd, buf, st.st_size);
        close(fd);
    }
    if (len == st.st_size) {
        recipes_parse(buf, len, recipes, log, 512);
        if (log[0]) {
            fd = open(RECIPES_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0777);
            if (fd >= 0) { write(fd, log, strlen(log)); close(fd); }
        } else if (stat(RECIPES_LOG, &st) == 0) {
            remove(RECIPES_LOG);
        }
    } else {
        recipes_state = -1;
        recipes_log("RECIPES.TXT unreadable: kept, saving off\r\n");
    }
    free(buf);
}

// Aktuelle Einstellungen als Rezept (Name bleibt)
static void recipe_from_conf(recipe_t *r)
{
    r->used = 1;
    r->film[0] = 0;
    if (conf.film_sim > 0 && film_at(conf.film_sim)) {
        strncpy(r->film, current()->name, FILMS_TXT_NAME_MAX);
        r->film[FILMS_TXT_NAME_MAX] = 0;
    }
    r->shift_r = wb_shift_clamp(conf.film_wb_r);
    r->shift_b = wb_shift_clamp(conf.film_wb_b);
    r->v[RF_WB]       = wb_mode_get();
    r->v[RF_DR]       = conf.film_dr;
    r->v[RF_NR]       = conf.film_cnr;
    r->v[RF_BWFILTER] = conf.film_bwfilter;
    r->v[RF_GRAIN]    = conf.film_grain;
    r->v[RF_VIGNETTE] = conf.film_vignette;
    r->v[RF_MIST]     = conf.film_mist;
    r->v[RF_HALATION] = conf.film_halation;
    r->v[RF_LEAK]     = conf.film_leak;
    r->v[RF_DATE]     = conf.film_datestamp;
    r->v[RF_FRAME]    = conf.film_frame;
}

// Stimmt der Zustand der Kamera noch mit dem Rezept ueberein?
static int recipe_matches(const recipe_t *r)
{
    recipe_t c;
    int i;
    recipe_from_conf(&c);
    if (!name_same(c.film, r->film) || c.shift_r != r->shift_r || c.shift_b != r->shift_b) return 0;
    for (i = 0; i < RF_N; i++) {
        if (i == RF_WB && r->v[i] == RECIPE_WB_KEEP) continue;
        if (c.v[i] != r->v[i]) return 0;
    }
    return 1;
}

static void recipe_apply(int k)
{
    const recipe_t *r = &recipes[k];
    int film = r->film[0] ? conf.film_sim : 0;
    if (r->film[0] && !name_same(filmsim_name(), r->film)) {
        int i;
        for (i = 1; i < NUM_SIMS; i++)
            if (name_same(sims[i].name, r->film)) { film = i; break; }
        if (i == NUM_SIMS && films_load_presets()) {
            for (i = 1; i <= preset_count; i++)
                if (name_same(preset_sims[i].name, r->film)) { film = PRESET_ID_OFFSET + i; break; }
        }
    }
    conf.film_dr        = r->v[RF_DR];
    conf.film_cnr       = r->v[RF_NR];
    conf.film_bwfilter  = r->v[RF_BWFILTER];
    conf.film_grain     = r->v[RF_GRAIN];
    conf.film_vignette  = r->v[RF_VIGNETTE];
    conf.film_mist      = r->v[RF_MIST];
    conf.film_halation  = r->v[RF_HALATION];
    conf.film_leak      = r->v[RF_LEAK];
    conf.film_datestamp = r->v[RF_DATE];
    conf.film_frame     = r->v[RF_FRAME];
    conf.film_wb_r      = r->shift_r;
    conf.film_wb_b      = r->shift_b;
    if (r->v[RF_WB] != RECIPE_WB_KEEP) wb_mode_set(r->v[RF_WB]);
    conf.film_recipe = k + 1;
    filmsim_set(film);                      // Canon-Farben und Kurve (mit Dynamik+ und Verschiebung)
}

// "C1 Name" bzw. "C1 Name*" (geaendert) / "C1 (leer)"
static const char* recipe_label(char *b, int k, int mark)
{
    const recipe_t *r = &recipes[k];
    sprintf(b, "C%d ", k + 1);
    strcat(b, !r->used ? lang_str(LANG_EXCCD_EMPTY) : (r->name[0] ? r->name : (r->film[0] ? r->film : sim_name(0))));
    if (mark && r->used && !recipe_matches(r)) strcat(b, "*");
    return b;
}

static int recipe_active(void)
{
    int k = conf.film_recipe;
    return (k >= 1 && k <= RECIPE_SLOTS && recipes[k - 1].used) ? k : 0;
}

// Menue "Recipe": blaettert durch die belegten Plaetze und laedt sofort
const char* filmsim_recipe_enum(int change, __attribute__ ((unused))int arg)
{
    static char b[32];
    int k, n;
    films_load();
    recipes_load();
    k = recipe_active();
    if (change) {
        for (n = 0; n <= RECIPE_SLOTS; n++) {
            k = (k + change + RECIPE_SLOTS + 1) % (RECIPE_SLOTS + 1);
            if (!k || recipes[k - 1].used) break;
        }
        if (k) recipe_apply(k - 1);
        else conf.film_recipe = 0;
    }
    return k ? recipe_label(b, k - 1, 1) : "-";
}

// Menue "Save as recipe": ein Eintrag je Platz, Text "C1 Summer 200" / "C3 (empty)"
char filmsim_slot_text[RECIPE_SLOTS][32];

static void slot_labels_update(void)
{
    int k;
    for (k = 0; k < RECIPE_SLOTS; k++) recipe_label(filmsim_slot_text[k], k, 0);
}

static void recipe_save_now(void)
{
    recipe_t *r = &recipes[recipe_save_slot];
    char *buf;
    int ok = 0, len;
    if (recipes_state >= 0 && (buf = malloc(RECIPE_FILE_MAX))) {
        recipe_t old = *r;
        if (!r->used || !r->name[0]) {      // neuer Platz: heisst wie der Film
            strncpy(r->name, conf.film_sim ? filmsim_name() : sim_name(0), RECIPE_NAME_MAX);
            r->name[RECIPE_NAME_MAX] = 0;
        }
        recipe_from_conf(r);
        len = recipes_format(recipes, buf, RECIPE_FILE_MAX);
        if (len > 0) {
            int fd = open(RECIPES_TXT, O_WRONLY | O_CREAT | O_TRUNC, 0777);
            if (fd >= 0) {
                ok = write(fd, buf, len) == len;
                close(fd);
            }
        }
        free(buf);
        if (ok) conf.film_recipe = recipe_save_slot + 1;
        else { *r = old; recipes_log("Writing RECIPES.TXT failed (card full?)\r\n"); }
    } else if (recipes_state >= 0) {
        recipes_log("Low memory: recipe not saved\r\n");
    }
    slot_labels_update();
    gui_mbox_init(LANG_EXCCD_RECIPE_SAVE, ok ? LANG_EXCCD_SAVED : LANG_EXCCD_SAVE_FAILED,
                  MBOX_FUNC_RESTORE|MBOX_TEXT_CENTER|MBOX_BTN_OK, NULL);
}

static void recipe_save_confirmed(unsigned int btn)
{
    if (btn == MBOX_BTN_YES) recipe_save_now();
}

// Menue "Save as recipe" -> Platz arg (0..3): belegter Platz erst nach Rueckfrage
void filmsim_recipe_save(int arg)
{
    static char text[64], label[32];
    recipes_load();
    if (arg < 0 || arg >= RECIPE_SLOTS) return;
    recipe_save_slot = arg;
    if (!recipes[recipe_save_slot].used) {
        recipe_save_now();
        return;
    }
    sprintf(text, lang_str(LANG_EXCCD_RECIPE_OVERWRITE), recipe_label(label, recipe_save_slot, 0));
    gui_mbox_init(LANG_EXCCD_RECIPE_SAVE, (int)text,
                  MBOX_FUNC_RESTORE|MBOX_TEXT_CENTER|MBOX_BTN_YES_NO|MBOX_DEF_BTN2, recipe_save_confirmed);
}

// Untermenue "Save as recipe" oeffnen (MENUITEM_SUBMENU_PROC): erst die Namen auffrischen
extern CMenu filmsim_save_menu;
void filmsim_recipe_save_menu(__attribute__ ((unused))int arg)
{
    recipes_load();
    slot_labels_update();
    gui_activate_sub_menu(&filmsim_save_menu);
}

// Statuszeile: rc = "C1" bzw. "C1*" (oder leer), wb = "R+2 B-1" (oder leer)
static void recipe_osd(char *rc, char *wb)
{
    int k = recipe_active();
    rc[0] = wb[0] = 0;
    if (k) {
        sprintf(rc, "C%d", k);
        if (!recipe_matches(&recipes[k - 1])) strcat(rc, "*");
    }
    if (wb_shift_active()) {
        char v[4];
        strcpy(wb, "R");
        shift_str(v, conf.film_wb_r); strcat(wb, v);
        strcat(wb, " B");
        shift_str(v, conf.film_wb_b); strcat(wb, v);
    }
}

// Menue "Reset to defaults": Look-Einstellungen und Effekte auf Standard, der Film bleibt.
// Die Kamera-Overrides (Zeit, Fokus, Blitz, ND, RAW) setzt gui.c zurueck.
void filmsim_reset_settings(void)
{
    if (conf.film_double) remove(DBL_FILE);
    conf.film_double = 0;
    conf.film_dr = 0;
    conf.film_cnr = 0;
    conf.film_bwfilter = 0;
    conf.film_zoom = 0;                 // frei
    conf.film_mist = 0;
    conf.film_halation = 0;
    conf.film_vignette = 0;
    conf.film_leak = 0;
    conf.film_datestamp = 0;
    conf.film_grain = 0;
    conf.film_frame = 0;
    conf.film_wb_r = conf.film_wb_b = 0;
    conf.film_recipe = 0;
    conf.film_save_original_jpeg = 0;
    conf.film_performance_metrics = 0;
    set_curve(current());               // ohne Dynamik+ und WB-Verschiebung
}

//-------------------------------------------------------------------
// Dynamik+: knapper belichten (Lichter schuetzen), Schatten per Kurve
// wieder aufhellen. Die Belichtungskorrektur passiert in shooting.c.
//-------------------------------------------------------------------
const char* filmsim_dr_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_OFF, (int)"+1", (int)"+2" };
    if (change) {
        conf.film_dr = (conf.film_dr + change + 3) % 3;
        set_curve(current());
    }
    return lang_str(m[conf.film_dr % 3]);
}

// Belichtungsverschiebung in 1/96 EV (0 = keine), von shooting.c bei jeder
// Aufnahme genutzt (bei Tv-Override oder Skript mit auto = 0, ohne Verschiebung)
int filmsim_dr_tv96_shift(int automatic)
{
    shot_dr = 0;
    if (!automatic) return 0;
    if (conf.film_dr < 1 || conf.film_dr > 2) return 0;
    if (!filmsim_photo_mode()) return 0;
    if (shooting_get_prop(PROPCASE_IS_FLASH_READY)) return 0;   // mit Blitz nicht
    // Zeit von Hand gewaehlt (Tv, M, Langzeit): nicht verkuerzen, dann auch keine Aufhell-Kurve
    int m = camera_info.state.mode_shooting;
    if (m == MODE_TV || m == MODE_M || m == MODE_LONG_SHUTTER) return 0;
    shot_dr = conf.film_dr;
    return conf.film_dr * 96;
}

// Vor den Kurven (raw.c): Wurde nicht knapper belichtet (Blitz, Tv-Override),
// darf auch die Aufhell-Kurve nicht wirken -> fuer dieses Bild die normale
// Kurve laden. Danach (end = 1) wieder die Kurve der Menueeinstellung.
void filmsim_dr_curve_hook(int end)
{
    int want = end ? ((conf.film_dr >= 1 && conf.film_dr <= 2) ? conf.film_dr : 0) : shot_dr;
    if (end) shot_dr = -1;
    if (want < 0 || !filmsim_photo_mode() || want == curve_dr) return;
#ifdef FS_OWN_CURVES
    curve_in_hook = 1;
#endif
    set_curve_dr(current(), want);
#ifdef FS_OWN_CURVES
    curve_in_hook = 0;
#endif
}

//-------------------------------------------------------------------
// Auto-ISO mit kuerzester Belichtungszeit (wie "ISO AUTO Min. VS" bei Sony)
//   Wirkt nur, wenn die Canon-ISO auf Auto (oder HI) steht. Nach der
//   Canon-Messung: Ist die Zeit laenger als die Grenze, steigt die ISO
//   (hoechstens bis ISO max) und die Zeit wird um genauso viel kuerzer.
//   Bei genug Licht sinkt die ISO bis zur Grund-ISO der Kamera, solange die
//   Zeit nicht unter die Grenze faellt. Die Belichtung (EV) bleibt gleich,
//   eine Dynamik+-Verschiebung ist schon in der gemessenen Zeit enthalten.
//   conf.film_aiso: 0 aus, 1 Auto (1/Brennweite KB), 2.. feste Zeit
//   conf.film_aiso_max: Index in aiso_max_iso[]
//-------------------------------------------------------------------
static const short aiso_tv96[]    = { 0, 0, 288, 375, 471, 567, 669, 765, 861 };   // 96*log2(den)
static const char *aiso_names[]   = { 0, 0, "1/8", "1/15", "1/30", "1/60", "1/125", "1/250", "1/500" };
static const short aiso_max_iso[] = { 200, 400, 800, 1600 };
#define AISO_N     ((int)(sizeof(aiso_tv96)/sizeof(aiso_tv96[0])))
#define AISO_MAX_N ((int)(sizeof(aiso_max_iso)/sizeof(aiso_max_iso[0])))

// kleinste und groesste ISO (Marktwert) dieser Kamera aus der Canon-Tabelle
static short aiso_cam_iso(int max)
{
    extern const ISOTable iso_table[];
    extern const unsigned int ISO_SIZE;
    unsigned i;
    short v = 0;
    for (i = 0; i < ISO_SIZE; i++) {
        short p = iso_table[i].prop_id;
        if (p < 50) continue;                       // Auto, HI
        if (!v || (max ? p > v : p < v)) v = p;
    }
    return v;
}

static short aiso_limit(void)
{
    int i = conf.film_aiso_max;
    if (i < 0 || i >= AISO_MAX_N) i = 1;
    short m = aiso_max_iso[i], cam = aiso_cam_iso(1);
    return (cam && m > cam) ? cam : m;
}

// Grenze als tv96, 0 = Funktion aus
static int aiso_min_tv96(void)
{
    int a = conf.film_aiso;
    if (a < 1 || a >= AISO_N) return 0;
    if (a > 1) return aiso_tv96[a];
    // Auto: Faustregel 1/Brennweite (Kleinbild), nicht langsamer als 1/30
    int f = (get_effective_focal_length(lens_get_zoom_point()) + 500) / 1000;
    if (f < 30) f = 30;
    return shooting_get_tv96_from_shutter_speed(1.0f / (float)f);
}

// 1 = Auto-ISO darf in der jetzigen Lage eingreifen (unabhaengig von der Zeit)
static int aiso_applies(void)
{
    if (!filmsim_photo_mode()) return 0;
    if (conf.tv_override_enabled) return 0;                     // feste Zeit gewaehlt
    // Skript oder USB-Fernbedienung steuert die Belichtung: deren Zeit bleibt
    extern int usb_remote_active;
    if (camera_info.state.state_kbd_script_run || usb_remote_active) return 0;
    if (shooting_get_iso_mode() > 0) return 0;                  // feste Canon-ISO gewaehlt
    // Nur in Modi, in denen die Kamera die Zeit frei waehlt. Szenenmodi wie Feuerwerk,
    // Nacht oder Langzeit wollen lange Zeiten, auch wenn ihre ISO intern auf Auto steht.
    switch (camera_info.state.mode_shooting) {
    case MODE_AUTO: case MODE_P: case MODE_AV: case MODE_PORTRAIT:
    case MODE_INDOOR: case MODE_KIDS_PETS: case MODE_LANDSCAPE: case MODE_DIGITAL_MACRO:
        break;
    default:
        return 0;
    }
    if (shooting_get_prop(PROPCASE_IS_FLASH_READY)) return 0;  // Blitz loest aus
    return 1;
}

// von shooting.c nach Tv-Override/Dynamik+; 1 = ISO wurde gesetzt
int filmsim_autoiso(void)
{
    int min_tv = aiso_min_tv96();
    if (!min_tv || !aiso_applies()) return 0;

    int tv = shooting_get_tv96();
    int sv = shooting_get_sv96_real();
    if (sv <= 0) return 0;
    int sv_min = shooting_get_sv96_from_iso(shooting_iso_market_to_real(aiso_cam_iso(0)));
    int sv_max = shooting_get_sv96_from_iso(shooting_iso_market_to_real(aiso_limit()));
    int want = sv + (min_tv - tv);                              // kleinste ISO, mit der die Zeit reicht
    // Anheben nur bis ISO max, aber nie unter Canons eigene Wahl: Liegt die schon ueber
    // ISO max, wuerde die Zeit sonst laenger als bei Canon. Senken nur bis zur Grund-ISO.
    if (want > sv) {
        if (want > sv_max) want = sv_max > sv ? sv_max : sv;
    } else if (want < sv_min) {
        want = sv_min < sv ? sv_min : sv;
    }
    if (want == sv) return 0;
    shooting_set_tv96_direct(tv + (want - sv), SET_NOW);
    shooting_set_sv96(want, SET_NOW);
    return 1;
}

const char* filmsim_aiso_enum(int change, __attribute__ ((unused))int arg)
{
    if (change) {
        conf.film_aiso = (conf.film_aiso + change + AISO_N) % AISO_N;
        conf.override_disable = 0;
    }
    int a = conf.film_aiso;
    if (a < 1 || a >= AISO_N) return lang_str(LANG_EXCCD_OFF);
    return a == 1 ? lang_str(LANG_EXCCD_AUTO) : aiso_names[a];
}

const char* filmsim_aiso_max_enum(int change, __attribute__ ((unused))int arg)
{
    static char buf[8];
    if (change) {
        int n = AISO_MAX_N, cam = aiso_cam_iso(1);
        while (n > 1 && aiso_max_iso[n - 1] > cam && aiso_max_iso[n - 2] >= cam) n--;   // nur Werte, die die Kamera kann
        int i = conf.film_aiso_max;
        if (i < 0 || i >= n) i = n - 1;
        conf.film_aiso_max = (i + change + n) % n;
    }
    sprintf(buf, "%d", aiso_limit());
    return buf;
}

// fuer die Anzeige unten: "A-ISO 1/60", "A-ISO -" (eingestellt, wirkt hier aber
// nicht: feste ISO, Tv/M, Szenenmodus, Blitz) oder 0
const char* filmsim_aiso_str(void)
{
    static char buf[16];
    int a = conf.film_aiso;
    if (a < 1 || a >= AISO_N) return 0;
    if (!aiso_applies()) return "A-ISO -";
    sprintf(buf, "A-ISO %s", a == 1 ? "auto" : aiso_names[a]);
    return buf;
}

//-------------------------------------------------------------------
// Defekte Sensorspalten (A/CHDK/BADCOLS.TXT)
//   Alte CCDs bekommen helle Spalten ueber die ganze Bildhoehe. Jede Zeile
//   der Datei: eine Spaltennummer im Rohbild (wie in der DNG), # = Kommentar.
//   Die Spalte wird aus den Nachbarn gleicher Farbe (x-2, x+2) neu
//   berechnet, vor DNG und JPEG. Jeder Sensor hat andere Spalten, darum
//   eine Datei auf der Karte und kein Wert im Code.
//-------------------------------------------------------------------
#define BADCOLS_TXT "A/CHDK/BADCOLS.TXT"
#define BADCOLS_MAX 16
static short badcols[BADCOLS_MAX];
static int badcols_n = -1;          // -1 = Datei noch nicht gelesen

static void badcols_load(void)
{
    char buf[256], *p;
    int fd, len;
    badcols_n = 0;
    fd = open(BADCOLS_TXT, O_RDONLY, 0777);
    if (fd < 0) return;
    len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) return;
    buf[len] = 0;
    for (p = buf; *p && badcols_n < BADCOLS_MAX; ) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p >= '0' && *p <= '9') {
            int x = strtol(p, &p, 10);
            if (x >= 2 && x < camera_sensor.raw_rowpix - 2) badcols[badcols_n++] = x;
        }
        while (*p && *p != '\n') p++;
    }
}

void filmsim_badcols_apply(void)
{
    int i, y;
    if (badcols_n < 0) badcols_load();
    for (i = 0; i < badcols_n; i++) {
        int x = badcols[i];
        for (y = 0; y < camera_sensor.raw_rows; y++)
            set_raw_pixel(x, y, (get_raw_pixel(x - 2, y) + get_raw_pixel(x + 2, y)) / 2);
    }
}

//-------------------------------------------------------------------
// RAW entwickeln: Effekte nur fuer diese Entwicklung
//   Nach der Filmauswahl folgt ein eigenes Menue. Es startet mit den Effekten aus den
//   Optionen, Aenderungen gelten nur fuer den Hilfsschuss. Die Werte stehen in dev_fx und
//   werden nur waehrend des RAW-Hooks mit conf getauscht (filmsim_develop_prepare / _end),
//   so bleiben die Einstellungen in den Optionen und in der CFG unveraendert.
//-------------------------------------------------------------------
#define DEV_FX_N 8
static int dev_fx[DEV_FX_N];
static int dev_fx_in = 0;               // 1: dev_fx steht gerade in conf

static int *fx_ptr(int i)
{
    switch (i) {
    case 0: return &conf.film_mist;
    case 1: return &conf.film_halation;
    case 2: return &conf.film_vignette;
    case 3: return &conf.film_leak;
    case 4: return &conf.film_datestamp;
    case 5: return &conf.film_grain;
    case 6: return &conf.film_frame;
    default: return &conf.film_cnr;
    }
}

static void dev_fx_swap(void)
{
    int i;
    for (i = 0; i < DEV_FX_N; i++) {
        int *p = fx_ptr(i), t = *p;
        *p = dev_fx[i];
        dev_fx[i] = t;
    }
}

static void dev_fx_init(void)
{
    int i;
    if (dev_fx_in) return;
    for (i = 0; i < DEV_FX_N; i++) dev_fx[i] = *fx_ptr(i);
}

static int dev_fx_any(void)
{
    int i;
    for (i = 0; i < DEV_FX_N - 1; i++)  // ohne Farbrauschen (kein Effekt)
        if (dev_fx[i]) return 1;
    return 0;
}

// Menue-Eintraege: die normalen Funktionen, aber auf dev_fx
#define DEV_FX_ENUM(name, orig) \
    static const char* name(int change, int arg) \
    { const char *r; dev_fx_swap(); r = orig(change, arg); dev_fx_swap(); return r; }
DEV_FX_ENUM(dev_mist_enum,      filmsim_mist_enum)
DEV_FX_ENUM(dev_halation_enum,  filmsim_halation_enum)
DEV_FX_ENUM(dev_vignette_enum,  filmsim_vignette_enum)
DEV_FX_ENUM(dev_leak_enum,      filmsim_leak_enum)
DEV_FX_ENUM(dev_datestamp_enum, filmsim_datestamp_enum)
DEV_FX_ENUM(dev_grain_enum,     filmsim_grain_enum)
DEV_FX_ENUM(dev_frame_enum,     filmsim_frame_enum)
DEV_FX_ENUM(dev_cnr_enum,       filmsim_cnr_enum)

static int dev_batch_start(void);
static void dev_fx_done(__attribute__ ((unused))int arg)
{
    dev_batch_start();                  // Serie (auch mit einem Look); ohne Haken selbst ausloesen
    exit_alt_on_release();
}

// Dritter Schritt: Belichtung und Farbtemperatur
static const char* dev_ev_enum(int change, __attribute__ ((unused))int arg)
{
    static char b[16];
    dev_ev += change;
    if (dev_ev < -6) dev_ev = -6;
    if (dev_ev > 6) dev_ev = 6;
    int a = dev_ev < 0 ? -dev_ev : dev_ev, w = a / 3, f = a % 3;
    char sg = dev_ev < 0 ? '-' : '+';
    if (!a)      strcpy(b, "0 EV");
    else if (!w) sprintf(b, "%c%d/3 EV", sg, f);
    else if (!f) sprintf(b, "%c%d EV", sg, w);
    else         sprintf(b, "%c%d %d/3 EV", sg, w, f);
    return b;
}

static const char* dev_temp_enum(int change, __attribute__ ((unused))int arg)
{
    static char b[24];
    dev_temp += change;
    if (dev_temp < -5) dev_temp = -5;
    if (dev_temp > 5) dev_temp = 5;
    if (!dev_temp) strcpy(b, "0");
    else sprintf(b, "%s %+d", lang_str(dev_temp > 0 ? LANG_EXCCD_DEV_WARM : LANG_EXCCD_DEV_COOL), dev_temp);
    return b;
}

static const char* dev_tint_enum(int change, __attribute__ ((unused))int arg)
{
    static char b[24];
    dev_tint += change;
    if (dev_tint < -5) dev_tint = -5;
    if (dev_tint > 5) dev_tint = 5;
    if (!dev_tint) strcpy(b, "0");
    else sprintf(b, "%s %+d", lang_str(dev_tint > 0 ? LANG_EXCCD_DEV_GREEN : LANG_EXCCD_DEV_MAGENTA), dev_tint);
    return b;
}

// Dynamik+ fuer die Entwicklung: Auto = Stufe der Aufnahme (DNG-Vermerk), sonst fest.
// Mit weniger Belichtung zusammen: Lichter schuetzen, Schatten per Kurve aufhellen.
static const char* dev_dr_enum(int change, __attribute__ ((unused))int arg)
{
    static const int m[] = { LANG_EXCCD_AUTO, LANG_EXCCD_OFF, (int)"+1", (int)"+2" };
    dev_dr = (dev_dr + change + 4) % 4;
    return lang_str(m[dev_dr]);
}

static void dev_fx_done(int arg);
static CMenuItem dev_adj_items[] = {
    MENU_ITEM   (0x5f,LANG_EXCCD_DEV_EV,                    MENUITEM_ENUM,      dev_ev_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_DYNAMIC,                   MENUITEM_ENUM,      dev_dr_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_DEV_TEMP,                  MENUITEM_ENUM,      dev_temp_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_DEV_TINT,                  MENUITEM_ENUM,      dev_tint_enum, 0 ),
    MENU_ITEM   (0x2a,LANG_EXCCD_DEV_FX_DONE,               MENUITEM_PROC,      dev_fx_done, 0 ),
    MENU_ITEM   (0x51,LANG_MENU_BACK,                       MENUITEM_UP,        0, 0 ),
    {0}
};
static CMenu dev_adj_menu = {0x2a,LANG_EXCCD_DEV_ADJ, dev_adj_items };

static CMenuItem dev_fx_items[] = {
    MENU_ITEM   (0x5f,LANG_EXCCD_GRAIN,                     MENUITEM_ENUM,      dev_grain_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_HALATION,                  MENUITEM_ENUM,      dev_halation_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_MIST,                      MENUITEM_ENUM,      dev_mist_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_VIGNETTE,                  MENUITEM_ENUM,      dev_vignette_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_LEAK,                      MENUITEM_ENUM,      dev_leak_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_DATESTAMP,                 MENUITEM_ENUM,      dev_datestamp_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_FRAME,                     MENUITEM_ENUM,      dev_frame_enum, 0 ),
    MENU_ITEM   (0x5f,LANG_EXCCD_CNR,                       MENUITEM_ENUM,      dev_cnr_enum, 0 ),
    MENU_ITEM   (0x2a,LANG_EXCCD_DEV_NEXT,                  MENUITEM_SUBMENU,   &dev_adj_menu, 0 ),
    MENU_ITEM   (0x2a,LANG_EXCCD_DEV_FX_DONE,               MENUITEM_PROC,      dev_fx_done, 0 ),
    MENU_ITEM   (0x51,LANG_MENU_BACK,                       MENUITEM_UP,        0, 0 ),
    {0}
};
static CMenu dev_fx_menu = {0x2a,LANG_EXCCD_DEV_FX, dev_fx_items };

// Wie das normale Menue, aber der Ausloeser schliesst es sofort: Wer hier ausloest, will
// entwickeln. Sonst bekaeme Canon den Ausloeser im ALT-Modus nicht mit, und das
// naechste Oeffnen von RAW develop braeche die Entwicklung ab.
static int dev_fx_kbd(void)
{
    if (kbd_is_key_pressed(KEY_SHOOT_HALF) || kbd_is_key_pressed(KEY_SHOOT_FULL)) {
        // Mit angekreuzten Looks loest die Serie selbst aus (der gehaltene Ausloeser wird
        // verschluckt), sonst geht er an Canon und entwickelt den eingelegten Film
        dev_batch_start();
        exit_alt();
        return 0;
    }
    return menuGuiHandler.kbd_process();
}

static void dev_fx_menu_open(void)
{
    devFxGuiHandler = menuGuiHandler;
    devFxGuiHandler.kbd_process = dev_fx_kbd;
    gui_menu_init(&dev_fx_menu);
    gui_set_mode(&devFxGuiHandler);
}

//-------------------------------------------------------------------
// RAW entwickeln in mehreren Looks hintereinander
//   Nach der Dateiwahl eine Liste aller Looks zum Ankreuzen. Start (oder der Ausloeser)
//   arbeitet sie ab: Look einlegen, Hilfsschuss selbst ausloesen (eigener Action-Stack wie
//   shoot() in Skripten), nach dem RAW-Hook der naechste Look. Simulierte Tasten erreichen
//   Canon nur, wenn kbd_process 1 liefert; deshalb haelt filmsim_batch_kbd die Tastatur,
//   solange die Serie laeuft. Jede Taste bricht ab (filmsim_batch_key). Liste und Haken
//   liegen nur waehrend einer Entwicklung im Speicher.
//-------------------------------------------------------------------
static int *dev_pick = 0;               // je Look 1 = entwickeln (Index wie sims, 0 "Aus" nie)
static int dev_pick_n = 0;              // Laenge von dev_pick (NUM_SIMS beim Oeffnen)
static CMenuItem *dev_list_items = 0;
static CMenu dev_list_menu;
static char dev_start_txt[32];
static volatile int batch_hold = 0;     // Taste vom Start noch gedrueckt: nicht abbrechen
static int batch_cur, batch_tries, batch_wait;
static AS_ID batch_as = 0;

static int dev_pick_count(void)
{
    int i, n = 0;
    for (i = 1; i < dev_pick_n; i++) n += dev_pick[i] != 0;
    return n;
}

static void dev_start_update(void)
{
    sprintf(dev_start_txt, lang_str(LANG_EXCCD_DEV_START), dev_pick ? dev_pick_count() : 0);
}

static void dev_list_free(void)
{
    if (dev_list_items) free(dev_list_items);
    if (dev_pick) free(dev_pick);
    dev_list_items = 0; dev_pick = 0; dev_pick_n = 0;
}

static void dev_pick_only(int i)
{
    int k;
    for (k = 0; k < dev_pick_n; k++) dev_pick[k] = (k == i);
}

static void dev_list_all(int arg)
{
    int i;
    for (i = 1; i < dev_pick_n; i++) dev_pick[i] = arg;
    dev_start_update();
    gui_menu_erase_and_redraw();
}

static void dev_list_start(__attribute__ ((unused))int arg)
{
    if (dev_batch_start()) exit_alt_on_release();
}

static void dev_item(CMenuItem *m, int sym, int text, int type, void *val, int arg)
{
    m->symbol = (char)sym; m->opt_len = 0; m->type = (short)type;
    m->text = text; m->value = (int *)val; m->arg = arg;
}

static int dev_list_open(void)
{
    int i, k = 0, n = NUM_SIMS;
    dev_list_free();
    dev_pick = malloc(n * sizeof(int));
    dev_list_items = malloc((n + 4) * sizeof(CMenuItem));
    if (!dev_pick || !dev_list_items) { dev_list_free(); return 0; }
    dev_pick_n = n;
    dev_pick_only(conf.film_sim > 0 && conf.film_sim < n ? conf.film_sim : -1);
    dev_item(&dev_list_items[k++], 0x2a, (int)dev_start_txt, MENUITEM_PROC, dev_list_start, 0);
    dev_item(&dev_list_items[k++], 0x2a, LANG_EXCCD_DEV_FX_MENU, MENUITEM_SUBMENU, &dev_fx_menu, 0);
    dev_item(&dev_list_items[k++], 0x5c, LANG_EXCCD_DEV_ALL, MENUITEM_PROC, dev_list_all, 1);
    dev_item(&dev_list_items[k++], 0x5c, LANG_EXCCD_DEV_NONE, MENUITEM_PROC, dev_list_all, 0);
    for (i = 1; i < n; i++)
        dev_item(&dev_list_items[k++], 0x5f, (int)sim_name(i), MENUITEM_BOOL|MENUITEM_ARG_CALLBACK,
                 &dev_pick[i], (int)dev_start_update);
    memset(&dev_list_items[k], 0, sizeof(CMenuItem));
    dev_list_menu.symbol = 0x2a;
    dev_list_menu.title = LANG_EXCCD_DEV_LIST;
    dev_list_menu.menu = dev_list_items;
    dev_start_update();
    devFxGuiHandler = menuGuiHandler;
    devFxGuiHandler.kbd_process = dev_fx_kbd;
    gui_menu_init(&dev_list_menu);
    gui_set_mode(&devFxGuiHandler);
    return 1;
}

// Naechsten angekreuzten Look einlegen und die DNG fuer den Hilfsschuss vormerken
static int batch_next(void)
{
    int i;
    for (i = batch_cur + 1; i < dev_pick_n; i++)
        if (dev_pick[i]) break;
    if (i >= dev_pick_n) return 0;
    batch_cur = i;
    batch_i++;
    batch_tries = 0;
    filmsim_set(i);
    const char *b = strrchr(dev_path, '/');
    strncpy(dev_name, b ? b + 1 : dev_path, sizeof(dev_name) - 1);
    raw_prepare_develop(dev_path, 0);
    dev_armed = 1;
    return 1;
}

// 0 = nichts angekreuzt (dann wie frueher: selbst ausloesen)
static int dev_batch_start(void)
{
    if (!dev_pick || !dev_armed) return 0;
    batch_n = dev_pick_count();
    if (!batch_n) return 0;
    batch_cur = 0; batch_i = 0;
    if (!batch_next()) return 0;
    batch_hold = 1;
    batch_wait = 0;
    batch_on = 1;
    return 1;
}

static void batch_stop(void)
{
    batch_on = 0;
    batch_hold = 0;
    if (batch_as) { action_stack_kill(batch_as); batch_as = 0; }
    kbd_key_release_all();
    if (dev_armed) { raw_prepare_develop(0, 0); dev_armed = 0; }
    if (!dev_active) dev_restore = 1;   // sonst nach dem Logbuch (filmsim_log_shot)
    dev_list_free();
}

// Hilfsschuss als Action-Stack (umgekehrte Reihenfolge): halb druecken, warten bis Canon
// bereit ist (hoechstens 5 s), Blitz geladen, ganz druecken, warten bis fertig, loslassen
static int batch_as_ready(void)
{
    if (shooting_in_progress() || get_tick_count() >= action_top(2)) { action_pop_func(1); return 1; }
    return 0;
}
static int batch_as_flash(void)
{
    if (shooting_is_flash_ready()) { action_pop_func(0); return 1; }
    return 0;
}
static int batch_as_done(void)
{
    if (!shooting_in_progress()) { action_pop_func(0); return 1; }
    return 0;
}
static int batch_as_shoot(void)
{
    action_pop_func(0);
    action_push_release(KEY_SHOOT_HALF);
    action_push_func(batch_as_done);
    action_push_click(KEY_SHOOT_FULL);
    action_push_func(batch_as_flash);
    action_push(get_tick_count() + 5000);
    action_push_func(batch_as_ready);
    action_push_press(KEY_SHOOT_HALF);
    return 1;
}

// Aus kbd_process vor allem anderen: eine echte Taste bricht die Serie ab
void filmsim_batch_key(void)
{
    if (!batch_on) return;
    long k = kbd_get_pressed_key();
    if (batch_hold) { if (!k) batch_hold = 0; return; }
    if (k) batch_stop();
}

// Aus kbd_process nach dem Action-Stack: Serie weiterfuehren. 1 = Tastatur halten
int filmsim_batch_kbd(void)
{
    if (!batch_on) return 0;
    if (!filmsim_photo_mode()) { batch_stop(); return 0; }
    if (batch_hold) return 1;           // Taste vom Start verschlucken
    // Nach exit_alt wechselt die Oberflaeche erst etwas spaeter (Menues oeffnen geht nur
    // per Taste, und die bricht ab)
    if (!camera_info.state.gui_mode_none) return 0;
    if (batch_as) {
        if (!action_stack_is_finished(batch_as)) return 1;
        batch_as = 0;
        batch_wait = get_tick_count() + 3000;   // RAW-Hook darf noch etwas brauchen
    }
    if (dev_active) return 1;
    if (!dev_armed) {                   // Hilfsschuss ist durch: naechster Look
        if (!batch_next()) { batch_stop(); return 0; }
        batch_wait = 0;
        return 1;
    }
    if (shooting_in_progress()) return 1;
    if (batch_wait && get_tick_count() < batch_wait) return 1;
    if (++batch_tries > 3) { batch_stop(); return 0; }  // Kamera loest nicht aus
    batch_as = action_stack_create(batch_as_shoot);
    if ((long)batch_as < 0) { batch_as = 0; batch_stop(); return 0; }
    return 1;
}

#ifdef FILMSIM_JPEG_PAIR
#define PAIR_RAW_FILE "A/CHDK/DATA/EXCCDJP.TMP"
#define PAIR_TIMEOUT_MS 30000
#define JPEG_PATH_SIZE 33

enum {
    PAIR_IDLE, PAIR_ORIGINAL, PAIR_READY,
    PAIR_METER, PAIR_HELPER, PAIR_RENDER, PAIR_LOAD_FAILED
};

static volatile int pair_phase;
static volatile int pair_cancel;
static volatile int pair_file_open;
static volatile int pair_file_closed;
static unsigned pair_deadline;
static unsigned pair_full_tick;
static int pair_keys;
static int pair_flash_mode = -1;
static int pair_number;
static char pair_jpeg[JPEG_PATH_SIZE];
static dng_meta_t pair_meta;

static int pair_use_original(void)
{
    return conf.film_save_original_jpeg && conf.film_sim > 0 &&
           pair_phase < PAIR_METER && !dev_armed && !dev_active &&
           !dev_restore && !batch_on && filmsim_photo_mode() &&
           !camera_info.state.state_kbd_script_run && !usb_remote_active &&
           !remotecap_get_target() && is_raw_possible() && shooting_get_drive_mode() == 0;
}

void filmsim_pair_prepare_capture(void)
{
    if (conf.film_save_original_jpeg && conf.film_sim && filmsim_photo_mode()) {
        const filmsim_t *s = pair_use_original() ? &sims[0] : current();
        if (!canon_props_match(s))
            set_canon_props(s);
    }
}

const char *filmsim_pair_enum(int change, __attribute__((unused)) int arg)
{
    if (conf.film_sim <= 0)
        return lang_str(LANG_EXCCD_NEEDS_LOOK);
    if (change && pair_phase == PAIR_IDLE) {
        conf.film_save_original_jpeg = !conf.film_save_original_jpeg;
        filmsim_apply();
    }
    return lang_str(conf.film_save_original_jpeg ? LANG_EXCCD_ON : LANG_EXCCD_OFF);
}

static void pair_expect_file(int number, int phase)
{
    pair_file_open = 0;
    pair_file_closed = 0;
    pair_number = number;
    pair_deadline = get_tick_count() + PAIR_TIMEOUT_MS;
    pair_phase = phase;
}

int filmsim_pair_original(char *raw, int iso)
{
    if (pair_phase != PAIR_IDLE || !pair_use_original())
        return 0;

    filmsim_perf_begin(FILMSIM_PERF_PAIR);
    filmsim_perf_mark(PERF_RAW_SPOOL);

    // Reserve room for the first JPEG and the optional user DNG while the
    // temporary sensor copy is on the card.
    unsigned size = camera_sensor.raw_size;
    int fd = -1;
    if (GetFreeCardSpaceKb() >= size * (conf.save_raw ? 3u : 2u) / 1024)
        fd = open(PAIR_RAW_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    int saved = 0;
    if (fd >= 0) {
#ifndef HOST_TEST
        raw = ADR_TO_UNCACHED(raw);
#endif
        saved = write(fd, raw, size) == (int)size;
        if (close(fd) != 0)
            saved = 0;
        if (!saved)
            remove(PAIR_RAW_FILE);
    }
    filmsim_perf_mark(PERF_NONE);
    if (!saved) {
        if (perf.active) perf.status = "raw_spool_failed";
        console_add_line(lang_str(LANG_EXCCD_JPEG_PAIR_FAILED));
        return 1; // keep the original even if the second render cannot be queued
    }

    int wb[3] = {0, 0, 0};
    get_property_case(camera_info.props.wb_adj, wb, sizeof(wb));
    time_t taken = camera_info.state.shutter_open_time;
    if (!taken) taken = time(NULL);
    struct tm *date = localtime(&taken);
    pair_meta = (dng_meta_t){
        .wb_r = wb[1], .wb_g = wb[0], .wb_b = wb[2], .iso = iso,
        .dr = shot_dr > 0 ? shot_dr : 0,
        .year = date->tm_year + 1900, .mon = date->tm_mon + 1, .day = date->tm_mday
    };
    pair_cancel = 0;
    if (perf.active) perf.wait_for_pair = 1;
    pair_expect_file(get_target_file_num(), PAIR_ORIGINAL);
    return 1;
}

int filmsim_pair_helper(void)
{
    return pair_phase == PAIR_METER || pair_phase == PAIR_HELPER;
}

void filmsim_pair_perf_helper_begin(void)
{
    if (!perf.active || !perf.wait_for_pair)
        return;
    perf_collect_jpeg();
    if (perf.ms[PERF_ORIGINAL_JPEG] >= 0)
        perf.ms[PERF_HELPER_WAIT] = (unsigned)get_tick_count() - perf.end_tick;
    perf.number = shot_path(perf.custom);
}

void filmsim_pair_helper_raw(int loaded)
{
    if (!loaded && perf.active && perf.wait_for_pair) {
        perf.custom[0] = 0;
        perf.status = "raw_load_failed";
    }
    dev_armed = 0;
    remove(PAIR_RAW_FILE);
    pair_expect_file(get_target_file_num(), loaded ? PAIR_RENDER : PAIR_LOAD_FAILED);
}

void filmsim_pair_filewrite(const char *name)
{
    int phase = pair_phase;
    int pair_expects_file = phase == PAIR_ORIGINAL || phase == PAIR_RENDER || phase == PAIR_LOAD_FAILED;
    if (!pair_expects_file && !perf_jpeg_number)
        return;
    if (name[0] != 'A' && name[0] != 'a' && name[0] != '/')
        return;
    const char *path = name[0] == '/' ? name : name + 1;
    unsigned len = strlen(path);
    if (strncmp(path, "/DCIM/", 6) != 0 || len < 15 || len >= JPEG_PATH_SIZE - 1 ||
        path[len - 9] != '_' || strcmp(path + len - 4, ".JPG") != 0)
        return;
    unsigned number = 0;
    for (unsigned i = len - 8; i < len - 4; ++i) {
        unsigned digit = (unsigned char)path[i] - '0';
        if (digit > 9)
            return;
        number = number * 10 + digit;
    }
    if (!number)
        return;
    if (pair_expects_file && number == pair_number) {
        pair_jpeg[0] = 'A';
        strcpy(pair_jpeg + 1, path);
        pair_file_open = 1;
    }
    if (number == perf_jpeg_number) {
        strcpy(perf_jpeg_path, path + 6);
        perf_jpeg_open = 1;
    }
}

void filmsim_pair_file_closed(void)
{
    if (perf_jpeg_open) {
        perf_jpeg_open = 0;
        perf_jpeg_close_tick = get_tick_count();
        perf_jpeg_closed = 1;
    }
    if (pair_file_open)
        pair_file_closed = 1;
}

static int jpeg_file_complete(const char *path)
{
    int fd = open(path, O_RDONLY, 0777);
    if (fd < 0)
        return 0;
    unsigned char end[2];
    int ok = lseek(fd, -2, SEEK_END) >= 0 && read(fd, end, 2) == 2 &&
             end[0] == 0xff && end[1] == 0xd9;
    close(fd);
    return ok;
}

static void pair_finish(int saved)
{
    const char *perf_status = saved ? "ok" : pair_cancel ? "cancelled" : "pair_failed";
    if (filmsim_pair_helper()) {
        raw_prepare_develop(NULL, 0);
        dev_armed = 0;
    }
    if (pair_flash_mode >= 0) {
        shooting_set_prop(PROPCASE_FLASH_MODE, pair_flash_mode);
        pair_flash_mode = -1;
    }
    remove(PAIR_RAW_FILE);
    pair_cancel = 0;
    pair_phase = PAIR_IDLE;
    if (!saved)
        console_add_line(lang_str(LANG_EXCCD_JPEG_PAIR_FAILED));
    if (filmsim_photo_mode() && conf.film_sim)
        set_canon_props(pair_use_original() ? &sims[0] : current());
    if (perf.active && perf.wait_for_pair) {
        perf_collect_jpeg();
        if (!saved) {
            perf.custom[0] = 0;
            perf.end_tick = get_tick_count();
        }
        perf_write(perf.status ? perf.status : perf_status);
    }
}

static void pair_tick(void)
{
    int phase = pair_phase;
    if (phase == PAIR_IDLE)
        return;
    if (pair_cancel || !filmsim_photo_mode() || (int)(get_tick_count() - pair_deadline) >= 0) {
        pair_finish(0);
        return;
    }
    if ((phase != PAIR_ORIGINAL && phase != PAIR_RENDER && phase != PAIR_LOAD_FAILED) ||
        !pair_file_closed || shooting_in_progress() || !jpeg_file_complete(pair_jpeg))
        return;
    if (phase == PAIR_ORIGINAL) {
        pair_phase = PAIR_READY;
    } else if (phase == PAIR_RENDER) {
        pair_finish(1);
    } else {
        remove(pair_jpeg); // this is the helper's own exposure after a failed RAW read
        pair_finish(0);
    }
}

int filmsim_pair_kbd(void)
{
    int phase = pair_phase;
    if (phase == PAIR_IDLE) {
        if (pair_keys) {
            kbd_key_release_all();
            pair_keys = 0;
        }
        return 0;
    }
    if (phase < PAIR_HELPER && kbd_is_key_clicked(KEY_MENU))
        pair_cancel = 1;
    if (pair_cancel)
        kbd_key_release_all();
    else if (phase == PAIR_READY && !kbd_get_pressed_key() &&
             camera_info.state.gui_mode_none && !shooting_in_progress()) {
        struct stat raw;
        if (stat(PAIR_RAW_FILE, &raw) != 0 || raw.st_size != camera_sensor.raw_size) {
            pair_cancel = 1;
            return 1;
        }
        raw_prepare_develop(PAIR_RAW_FILE, 0);
        dev_meta = pair_meta;
        dev_has_meta = 1;
        dev_armed = 1;
        dev_prev = conf.film_sim;
        dev_fx_init();
        dev_ev = dev_temp = dev_tint = dev_dr = 0;
        strcpy(dev_name, strrchr(pair_jpeg, '/') + 1);
        pair_flash_mode = shooting_get_flash_mode();
        shooting_set_prop(PROPCASE_FLASH_MODE, 2);
        set_canon_props(current());
        pair_deadline = get_tick_count() + PAIR_TIMEOUT_MS;
        kbd_key_release_all();
        kbd_key_press(KEY_SHOOT_HALF);
        pair_phase = PAIR_METER;
        pair_keys = 1;
    } else if (phase == PAIR_METER && shooting_in_progress() && shooting_is_flash_ready()) {
        kbd_key_press(KEY_SHOOT_FULL_ONLY);
        pair_full_tick = get_tick_count();
        pair_phase = PAIR_HELPER;
    } else if (phase == PAIR_HELPER && (unsigned)get_tick_count() - pair_full_tick >= 100) {
        kbd_key_release(KEY_SHOOT_FULL_ONLY);
    } else if (phase == PAIR_RENDER || phase == PAIR_LOAD_FAILED || phase == PAIR_ORIGINAL) {
        kbd_key_release_all();
    }
    return 1;
}
#endif

//-------------------------------------------------------------------
// Fortschrittsbalken (RAW entwickeln und jede Aufnahme mit Look oder Effekten)
//   Der RAW-Hook laeuft im selben Task wie die CHDK-Oberflaeche, die waehrenddessen
//   nicht zeichnet. Deshalb zeichnen die Rechenschleifen den Balken selbst (prog_rows,
//   hoechstens alle PROG_MS). Die Schritte sind grob nach den Messungen auf der SX110
//   gewichtet (1/10 s), die Zeit fuer Canons JPEG ist nicht enthalten.
//-------------------------------------------------------------------
#define PROG_MS 250
static int prog_on = 0;
static int prog_w[PS_N];
static int prog_total, prog_base, prog_step_w, prog_k;
static int prog_tick;
static int prog_dev;                    // 1: RAW entwickeln, 0: normale Aufnahme

#ifdef FILMSIM_OSD_UNMUTE
//   Waehrend der Bildverarbeitung setzt Canon in 0xC0F1407C Bit 4 ("MuteOn"), das blendet
//   das OSD aus, der Bildschirm ist schwarz. Zum Zeichnen das Bit im Register loeschen (wie
//   Canons eigene Schreibfunktion: Schattenkopie bei 0x0031xxxx, dann Register, Uebernahme
//   ueber 4078). Canons Variable bleibt unberuehrt, Canon stellt nach der Aufnahme selbst
//   wieder her. Die RAM-Adressen kommen aus platform_camera.h:
//     FILMSIM_OSD_VAR    Canons Kopie von 407C
//     FILMSIM_OSD_ON     1 = Display an
//     FILMSIM_OSD_CHECK  Adresse/Wert-Paare im ROM, die genau zu dieser Firmware passen
//   IXUS 60 1.00d: Canon muted bei jeder Aufnahme (ff932f6c). Gemessen 11.11.: Nur dieses
//   Bit bringt den Balken; 0xC0F14074 zusaetzlich zuruecksetzen zeigt beim RAW entwickeln
//   bunte Bildfehler, allein bringt es nichts.
//   SX110 1.00b: Canon muted nur bei Blitz (MuteOn ffc31640, Kopie 0x231C, gefunden 14.11.).
static void osd_reg(unsigned int a, unsigned int v)
{
    *(volatile unsigned int *)((a & 0x1FFFF) | 0x300000) = v;
    *(volatile unsigned int *)a = v;
}
static void osd_unmute(void)
{
#ifdef HOST_TEST
    return;                                             // PC-Test: kein ROM, keine Register
#endif
    // Andere Firmware (z. B. IXUS 60 1.00a): nichts tun
    static const unsigned int chk[] = FILMSIM_OSD_CHECK;
    unsigned i;
    for (i = 0; i + 1 < sizeof(chk) / sizeof(chk[0]); i += 2)
        if (*(const unsigned int *)chk[i] != chk[i + 1]) return;
    if (*(volatile int *)FILMSIM_OSD_ON != 1) return;   // Display aus: nichts anfassen
    unsigned int v = *(volatile unsigned int *)FILMSIM_OSD_VAR;
    if (v & 0x10) { osd_reg(0xC0F1407C, v & ~0x10); osd_reg(0xC0F14078, 1); }
}
#endif

static void prog_draw(int pm, int label)
{
    char buf[64];
    int w = camera_screen.width, h = camera_screen.height;
    int x0 = 16, x1 = w - 17;
    int bh = FONT_HEIGHT * 2 + 34;
    int y0 = (h - bh) / 2, y1 = y0 + bh;
    int bx0 = x0 + 10, bx1 = x1 - 10, by0 = y1 - 18, by1 = y1 - 8;
    int cols = (x1 - x0 - 20) / FONT_WIDTH;
    if (cols > (int)sizeof(buf) - 1) cols = sizeof(buf) - 1;
    if (pm < 0) pm = 0;
    if (pm > 1000) pm = 1000;
    draw_rectangle(x0, y0, x1, y1, MAKE_COLOR(EX_BG, EX_RULE), RECT_BORDER1|DRAW_FILLED);
    if (prog_dev) {
        sprintf(buf, lang_str(LANG_EXCCD_DEV_PROGRESS), dev_name);
        if (batch_on && batch_n > 1) sprintf(buf + strlen(buf), " %d/%d", batch_i, batch_n);
    } else          sprintf(buf, lang_str(LANG_EXCCD_SHOT_PROGRESS), filmsim_name());
    buf[cols] = 0;
    draw_string(x0 + 10, y0 + 6, buf, MAKE_COLOR(EX_BG, COLOR_WHITE));
    strncpy(buf, lang_str(label), sizeof(buf) - 1);
    buf[cols - 5] = 0;
    draw_string(x0 + 10, y0 + 8 + FONT_HEIGHT, buf, MAKE_COLOR(EX_BG, EX_GREY));
    sprintf(buf, "%3d%%", pm / 10);
    draw_string(x1 - 10 - 4 * FONT_WIDTH, y0 + 8 + FONT_HEIGHT, buf, MAKE_COLOR(EX_BG, COLOR_WHITE));
    draw_rectangle(bx0, by0, bx1, by1, MAKE_COLOR(COLOR_BLACK, COLOR_WHITE), RECT_BORDER1|DRAW_FILLED);
    int fill = bx0 + 1 + ((bx1 - bx0 - 2) * pm) / 1000;
    if (fill > bx0 + 1)
        draw_rectangle(bx0 + 1, by0 + 1, fill, by1 - 1, MAKE_COLOR(COLOR_WHITE, COLOR_WHITE), RECT_BORDER0|DRAW_FILLED);
#ifdef FILMSIM_OSD_UNMUTE
    osd_unmute();
#endif
}

static int prog_label(int k)
{
    static const int lab[PS_N] = { LANG_EXCCD_DEV_LOAD, LANG_EXCCD_DEV_HOTPIX, LANG_EXCCD_DOUBLE, LANG_EXCCD_CNR,
                                   LANG_EXCCD_DEV_CURVE, LANG_EXCCD_DEV_MIX, LANG_EXCCD_EFFECTS, LANG_EXCCD_GRAIN,
                                   LANG_EXCCD_DEV_TEMP };
    return lab[k];
}

static void prog_show(int done, int total)
{
    // Promille: (Basis + Schrittgewicht * done / total) / Summe, ohne 32-Bit-Ueberlauf
    int part = (int)(((long long)prog_step_w * done * 1000) / total);
    prog_draw(prog_total ? (prog_base * 1000 + part) / prog_total : 0, prog_label(prog_k));
    prog_tick = get_tick_count();
}

static void prog_rows(int done, int total)
{
    if (!prog_on || total <= 0) return;
    if (get_tick_count() - prog_tick < PROG_MS) return;
    prog_show(done, total);
}

// Schritt k beginnt
void filmsim_prog_step(int k)
{
    int i;
    if (!prog_on || k < 0 || k >= PS_N) return;
    prog_k = k;
    prog_base = 0;
    for (i = 0; i < k; i++) prog_base += prog_w[i];
    prog_step_w = prog_w[k];
    if (prog_step_w) prog_show(0, 1);
}

// Gewichte der Rechenschritte aus den aktuellen Einstellungen (ohne Laden und WB)
static void prog_plan(void)
{
    int i, bw = 0;
    const filmsim_t *s = current();
    for (i = 0; i < PS_N; i++) prog_w[i] = 0;
    if (!raw_touched(&bw)) return;      // z. B. Daylight 100 ohne Effekte: nichts zu rechnen
    prog_w[PS_HOT] = 15;
    int cl = cnr_level();
    if (cl) prog_w[PS_CNR] = cl == 2 ? 80 : 40;
    if (s->curve || conf.film_dr) prog_w[PS_CURVE] = 19;
    if (s->mix || bw) prog_w[PS_MIX] = 50;
    if (conf.film_halation) prog_w[PS_GLOW] = conf.film_mist ? 260 : 230;
    else if (conf.film_mist) prog_w[PS_GLOW] = 120;
    else if (conf.film_vignette || conf.film_leak) prog_w[PS_GLOW] = 40;
    if (grain_pct10()) prog_w[PS_GRAIN] = 90;
}

static void prog_start(int dev)
{
    int i;
    prog_total = 0;
    for (i = 0; i < PS_N; i++) prog_total += prog_w[i];
    prog_dev = dev;
    prog_on = prog_total > 0;
    prog_tick = get_tick_count();
}

static void prog_finish(void)
{
    if (prog_on) {
        prog_draw(1000, LANG_EXCCD_DEV_JPEG);
        prog_on = 0;
        gui_set_need_restore();         // wegwischen, sobald die Oberflaeche wieder zeichnet
    }
}

// Aus raw.c vor dem Laden der DNG: Effekte der Entwicklung einsetzen, Balken planen
void filmsim_develop_prepare(void)
{
    shot_skip = 0;
    if (!dev_fx_in) { dev_fx_swap(); dev_fx_in = 1; }
    prog_plan();
    prog_w[PS_LOAD] = dev_ev ? 35 : 20;                  // mit Belichtung ein Durchgang mehr
    prog_w[PS_WB] = (dev_has_meta || dev_temp || dev_tint) ? 15 : 0;   // Weissabgleich am Ende
    prog_start(1);
    filmsim_prog_step(PS_LOAD);
}

// Aus raw.c bei einer normalen Aufnahme, vor den Rechenschritten.
// Doppelbelichtung: Rohbild auf die Karte schreiben bzw. lesen.
void filmsim_shot_prepare(void)
{
    shot_skip = 0;
    if (!filmsim_photo_mode()) { prog_on = 0; return; }
    prog_plan();
    if (conf.film_double) prog_w[PS_DOUBLE] = 50;
    prog_start(0);
}

void filmsim_shot_end(void)
{
    prog_finish();
}

// Aus raw.c nach den Effekten: Weissabgleich der Aufnahme, Temperatur und Toenung, dann
// Balken voll und Einstellungen der Optionen zurueck
void filmsim_develop_end(char *raw)
{
#ifdef FS_RAW
    if (raw && (dev_end_fr != WB_ONE || dev_end_fg != WB_ONE || dev_end_fb != WB_ONE)) {
        filmsim_prog_step(PS_WB);
        raw_gain((unsigned char *)raw, camera_sensor.raw_rows, camera_sensor.raw_rowpix, camera_sensor.raw_rowlen,
                 FS_ROW0, 31, dev_end_fr, dev_end_fg, dev_end_fb, 1000, dev_end_nr, dev_end_nb);
    }
    dev_end_fr = dev_end_fg = dev_end_fb = WB_ONE;
#else
    (void)raw;
#endif
    prog_finish();
    if (dev_fx_in) { dev_fx_swap(); dev_fx_in = 0; }
}

//-------------------------------------------------------------------
// RAW entwickeln: Vorschaubild in der Dateiauswahl (modules/gui_fselect.c)
//   Zuerst sofort das kleine Bild aus dem DNG-Kopf (128 x 96, aus den Rohdaten, ohne
//   Weissabgleich), danach ein schaerferes direkt aus den Rohdaten der DNG: je Bildschirm-
//   zeile wird ein Zeilenpaar R G / G B gelesen und je Bildschirmpixel ein 2x2-Block
//   ausgewertet. Die Bildschirm-Bitmap kennt nur die Palette der Kamera, deshalb in
//   Graustufen: die deckenden Grautoene aus FILMSIM_THUMB_GREYS (Index, Helligkeit),
//   dazwischen Floyd-Steinberg-Dithering. Der Kontrast wird gespreizt (1 % / 99 %).
//   still_wanted: von der Dateiauswahl, 0 = Cursor ist weiter, Lesen abbrechen.
//   Rueckgabe 1 = gezeichnet, 0 = kein Vorschaubild (die Dateiauswahl zeigt dann nichts).
//-------------------------------------------------------------------
#ifdef FILMSIM_THUMB_GREYS
static const unsigned char thumb_gtab[] = FILMSIM_THUMB_GREYS;
#define THUMB_NG ((int)sizeof(thumb_gtab) / 2)
#endif

// Graubild w x h (1 Byte je Pixel) gedehnt, skaliert und gedithert in den Kasten
static int thumb_render(unsigned char *img, int w, int h, int x0, int y0, int bw, int bh)
{
#ifdef FILMSIM_THUMB_GREYS
    const unsigned char *gtab = thumb_gtab;
    int ng = THUMB_NG;
#else
    unsigned char gtab[6] = { COLOR_BLACK, 0, COLOR_GREY, 128, COLOR_WHITE, 255 };
    int ng = 3;
#endif
    int lvl[16], hist[64], i, k;
    if (ng > 16) ng = 16;
    for (i = 0; i < ng; i++) lvl[i] = gtab[2 * i + 1] * 255 / (gtab[2 * ng - 1] ? gtab[2 * ng - 1] : 255);

    for (i = 0; i < 64; i++) hist[i] = 0;
    for (i = 0; i < w * h; i++) hist[img[i] >> 2]++;
    int lo = 0, hi = 63, c = 0;
    while (lo < 63 && (c += hist[lo]) < w * h / 100) lo++;
    c = 0;
    while (hi > 0 && (c += hist[hi]) < w * h / 100) hi--;
    lo *= 4; hi = hi * 4 + 3;
    if (hi - lo < 64) { int m = (hi + lo) / 2; lo = m - 32; hi = m + 32; }

    // Seitenverhaeltnis halten, mittig in den Kasten
    int ow = bw, oh = bw * h / w;
    if (oh > bh) { oh = bh; ow = bh * w / h; }
    x0 += (bw - ow) / 2;
    y0 += (bh - oh) / 2;
    int *err = malloc(2 * (ow + 2) * sizeof(int));          // Fehler * 16, zwei Zeilen
    if (!err) return 0;
    memset(err, 0, 2 * (ow + 2) * sizeof(int));
    int *e0 = err, *e1 = err + ow + 2, oy, ox;
    for (oy = 0; oy < oh; oy++) {
        const unsigned char *row = img + (oy * h / oh) * w;
        int run = 0, run_c = -1, run_x = 0;
        for (ox = 0; ox < ow; ox++) {
            int v = (row[ox * w / ow] - lo) * 255 / (hi - lo);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            v += e0[ox + 1] / 16;
            for (k = 0; k < ng - 1 && lvl[k + 1] <= v; k++);
            if (k < ng - 1 && v - lvl[k] > lvl[k + 1] - v) k++;
            int e = v - lvl[k];
            e0[ox + 2] += e * 7;
            e1[ox]     += e * 3;
            e1[ox + 1] += e * 5;
            e1[ox + 2] += e;
            int col = gtab[2 * k];
            if (col != run_c) {
                if (run) draw_hline(x0 + run_x, y0 + oy, run, run_c);
                run = 0; run_c = col; run_x = ox;
            }
            run++;
        }
        if (run) draw_hline(x0 + run_x, y0 + oy, run, run_c);
        int *t = e0; e0 = e1; e1 = t;
        memset(e1, 0, (ow + 2) * sizeof(int));
    }
    free(err);
    return 1;
}

// Kleines Bild aus dem DNG-Kopf als Graubild (w x h)
static unsigned char *thumb_small(int fd, const unsigned char *head, int n, int *w, int *h)
{
    unsigned off;
    if (!dng_thumb_info(head, n, &off, w, h)) return 0;
    unsigned char *img = malloc(*w * *h * 3);
    if (!img) return 0;
    lseek(fd, off, 0);                                      // SEEK_SET
    if (read(fd, img, *w * *h * 3) != *w * *h * 3) { free(img); return 0; }
    // Rohdaten ohne Weissabgleich: Gruen liegt schon tiefer, deshalb staerker
    int i;
    for (i = 0; i < *w * *h; i++) {
        int y = (img[3 * i] * 5 + img[3 * i + 1] * 9 + img[3 * i + 2] * 2) >> 4;
        img[i] = y > 255 ? 255 : y;                         // in place: i <= 3 * i
    }
    return img;
}

#ifdef FS_RAW
// Schaerferes Graubild ow x oh aus den Rohdaten der DNG (am Dateiende, Bytepaare
// vertauscht wie in modules/dng.c). 0 = keine DNG dieser Kamera oder abgebrochen.
static unsigned char *thumb_raw(int fd, int fsize, const unsigned char *head, int n,
                                int bw, int bh, int *ow, int *oh, int (*still_wanted)(void))
{
    static unsigned char *gam = 0;          // Rohwert 0..1023 -> Bildschirm, Gamma etwa 1/2 (malloc, bleibt)
    dng_meta_t m;
    int rowlen = camera_sensor.raw_rowlen, jx, jy, jw, jh, x, y;
    if (!dng_parse(head, n, camera_sensor.raw_rowpix, camera_sensor.raw_rows, camera_sensor.bits_per_pixel, &m))
        return 0;
    if (fsize < (int)camera_sensor.raw_size) return 0;
    int base = fsize - camera_sensor.raw_size;
    jpeg_rect(&jx, &jy, &jw, &jh);
    *ow = bw; *oh = bw * jh / jw;
    if (*oh > bh) { *oh = bh; *ow = bh * jw / jh; }
    if (*ow < 8 || *oh < 8) return 0;
    if (!gam && (gam = malloc(1024)))
        for (x = 0; x < 1024; x++) {
            int v = x * 64, r = 0, b;
            for (b = 1 << 15; b; b >>= 1) if ((r + b) * (r + b) <= v) r += b;
            gam[x] = r > 255 ? 255 : r;
        }
    unsigned char *img = malloc(*ow * *oh);
    unsigned short *rows = malloc(2 * rowlen + 16);
    short *sx = malloc(*ow * sizeof(short));
    if (!gam || !img || !rows || !sx) goto fail;
    for (x = 0; x < *ow; x++) sx[x] = (jx + x * jw / *ow) & ~1;
    for (y = 0; y < *oh; y++) {
        if ((y & 7) == 0 && still_wanted && !still_wanted()) goto fail;
        int r0 = (((jy + y * jh / *oh) - FS_ROW0) & ~1) + FS_ROW0;
        if (r0 + 1 >= (int)camera_sensor.raw_rows) r0 -= 2;
        lseek(fd, base + r0 * rowlen, 0);
        if (read(fd, rows, 2 * rowlen) != 2 * rowlen) goto fail;
        int i;
        for (i = 0; i < rowlen; i++) rows[i] = (unsigned short)((rows[i] >> 8) | (rows[i] << 8));
        const unsigned char *a0 = (const unsigned char *)rows, *a1 = a0 + rowlen;
        int last = -1, p[8], q[8];
        for (x = 0; x < *ow; x++) {
            int k = sx[x] >> 3, j = sx[x] & 7;
            if (k != last) { dec8(a0 + k * FS_GRP, p); dec8(a1 + k * FS_GRP, q); last = k; }
            int rr = (p[j] - BLK) * WB_R >> 8, gg = (p[j + 1] + q[j]) / 2 - BLK, bb = (q[j + 1] - BLK) * WB_B >> 8;
            if (rr < 0) rr = 0;
            if (gg < 0) gg = 0;
            if (bb < 0) bb = 0;
            int lum = (rr * 77 + gg * 150 + bb * 29) >> 8;
            img[y * *ow + x] = gam[lum > 1023 ? 1023 : lum];
        }
    }
    free(rows); free(sx);
    return img;
fail:
    if (img) free(img);
    if (rows) free(rows);
    if (sx) free(sx);
    return 0;
}
#endif

int filmsim_dng_thumb(const char *fn, int x0, int y0, int bw, int bh, int (*still_wanted)(void))
{
    struct stat st;
    int w = 0, h = 0, n = 0, ok = 0;
    unsigned char *head, *img;
    if (bw < 8 || bh < 8 || stat(fn, &st) != 0) return 0;
    int fd = open(fn, O_RDONLY, 0777);
    if (fd < 0) return 0;
    head = malloc(DNG_HEAD_MAX);
    if (head) {
        n = read(fd, head, DNG_HEAD_MAX);
        img = thumb_small(fd, head, n, &w, &h);             // sofort etwas zeigen
        if (img) { ok = thumb_render(img, w, h, x0, y0, bw, bh); free(img); }
#ifdef FS_RAW
        int sw = w, sh = h;
        img = thumb_raw(fd, st.st_size, head, n, bw, bh, &w, &h, still_wanted);
        if (img) {
            // anderer Ausschnitt als das kleine Bild: Reste davon am Rand wegwischen
            if (ok && w * sh != h * sw)
                draw_rectangle(x0, y0, x0 + bw - 1, y0 + bh - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0|DRAW_FILLED);
            ok |= thumb_render(img, w, h, x0, y0, bw, bh);
            free(img);
        }
#else
        (void)still_wanted;
#endif
        free(head);
    }
    close(fd);
    return ok;
}
