
# EXCCD+
<img width="900" height="300" alt="exccdplus" src="https://github.com/user-attachments/assets/167387f4-b2bd-4155-afc7-9e13be0f9175" />

EXCCD+ v1.1.1 combines the original [CHDK](https://chdk.fandom.com/) menu and script loader
with the film looks from [EXCCD v1.10.2](https://github.com/35mmSebas/exccd). Each look
combines Canon's *My Colors* settings with a tone curve applied before the camera creates the JPEG.

The Canon PowerShot A620 with firmware 1.00f is the primary EXCCD+ target. Other camera
ports come from upstream EXCCD or this. See the [build target list](tools/filmsim/cameras.txt)
for model IDs and firmware versions. The table reports upstream camera testing for those ports.

If you'd like to test EXCCD+ on your camera, you're welcome to
[open an issue](https://github.com/FamousMaslina/exccd-plus/issues/new) with your camera model,
firmware version, and any test results.

| Camera | Firmware | Status |
|---|---|---|
| PowerShot A620 | 1.00f | Primary EXCCD+ build target |

## ExFilmSim Engine (EXFS)

On the PowerShot A620, **EXCCD+ → EXFS** enables the optional
ExFilmSim Engine. The checkbox defaults to off, which selects the original
EXCCD engine. EXFS uses the same recipes and Canon JPEG processing. It speeds
up the RAW hot-pixel scan, RGB tone curves, and white-balance correction after
RAW development.

These timings were measured on the A620 1.00f with **Kodak Gold 200**. Each row
compares one capture per engine, from RAW handling to the final JPEG close.

| Capture mode | Original EXCCD | EXFS | Time saved | RGB tone curve, EXCCD → EXFS |
|---|---:|---:|---:|---:|
| Single photo | 6.66 s | 5.85 s | **0.81 s (12.2%)** | 2.25 s → 1.17 s |
| Save Original JPEG | 14.01 s | 11.95 s | **2.06 s (14.7%)** | 2.26 s → 1.16 s |

## What's new

- **v1.1.1** Add a two-tap **Print** shortcut for the A620's other still-photo modes.
  The first tap opens **Quick ISO**, and the next tap opens the film picker. The
  **Left/Right** shortcuts are available in **P**, **Night Scene**, **Landscape**,
  **Portrait**, and **Auto**.
- **v1.1.0** Add **ExFilmSim Engine**, **Performance Metrics**, **Kodak Gold 200**, and
  **Reggie Porta**. Limit the A620's Quick ISO and film-picker shortcuts to **P**,
  **Night Scene**, **Landscape**, **Portrait**, and **Auto**.
- **v1.0.3** Add **Save Original JPEG** on the A620. It saves the original Canon JPEG and a
  recipe JPEG from the same exposure, both with standard Canon filenames.
- **v1.0.2** Always export the ISO used for the exposure in JPEG and DNG EXIF, including Auto ISO.
- **v1.0.1** Show a custom EXCCD+ splash screen on boot.
- **v1.0.0** Restore the standard CHDK menu and add **Quick ISO**, a configurable
  shortcut for the film picker, and the **Sony-CFL** look.

## Looks

| Look | Type | Character |
|---|---|---|
| Daylight 100 | Colour slide | Natural balance |
| Postcard 100 | Colour slide | Rich saturation |
| Blush 100 | Colour slide | Soft portrait |
| Journal 400 | Colour slide | Documentary chroma |
| Studio 160 | Colour negative | Neutral shadow |
| Drugstore 200 | Colour negative | Retro negative |
| Scene 500 | Cinema | Cinematic flat |
| Silver 400 | Black and white | Fine mono |
| Portrait 400 | Colour negative | Warm skin |
| Tungsten 800 | Cinema | Neon nights |
| Expired 200 | Colour negative | Faded memory |
| Cross 100 | Colour slide | Cross processed |
| Violet 400 | Colour negative | Purple foliage |
| Concrete 100 | Colour negative | Muted urban |
| Ember 200 | Colour negative | Red scale |
| Grit 400 | Black and white | Pushed grain |
| Summer 200 | Colour negative | Golden warmth |
| Chrome 64 | Colour slide | Deep reds |
| Bypass 400 | Cinema | Silver retained |
| Noir 3200 | Black and white | Hard pushed |
| Turquoise 400 | Colour negative | Teal leaves |
| Infra 200 | Colour negative | Red foliage |
| Tintype 50 | Alternative process | Warm metal |
| Cyano 25 | Alternative process | Sun print |
| Instant 600 | Instant | Creamy fade |
| Pastel 400 | Colour negative | Airy soft |

Every look also has two **Dynamic+** variants (DR+1, DR+2) that expose shorter to protect
highlights and lift the shadows with the curve. They only shift the exposure when the camera
picks the shutter speed: in Tv, M and long shutter mode, and with flash, the normal curve is used.

### Sony-CFL (PowerShot A620)

**Sony-CFL** is a custom look tuned on the PowerShot A620 against JPEGs from the Sony a6700
using its FL look. It combines a custom RGB tone curve with Canon Custom Color at saturation −1.

On a card with Sony-CFL installed, open **EXCCD+ → Show All Film Recipes** in ALT mode and
select **Sony-CFL**. It also appears in **Select Only Presets** when listed in `CHDK/FILMS/FILMS.BAK`.

To add Sony-CFL to a fresh card, run `python tools/filmsim/make_curves.py <output-dir>` from
the repository and copy `SONYCFL.CV`, `SONYCFL1.CV` and `SONYCFL2.CV` to `CHDK/CURVES/`.
The last two files are the DR+1 and DR+2 variants. Append this row to `CHDK/FILMS/FILMS.TXT`
(copy `CHDK/FILMS/EXAMPLE.TXT` as a starting point if needed), then restart the camera.

```text
Sony-CFL | 11 | 0 | 0 | -1 | 0 | 0 | 0 | 0 | SONYCFL.CV | - | 0
```

### Reggie Porta (PowerShot A620)

**Reggie Porta** adapts Reggie Ballesteros's **Reggie's Portra** Classic Chrome recipe for the A620. The curve
uses the Fuji samples as a visual reference and was previewed on A620 DNG/JPEG pairs. It has
slightly open shadows, warm highlights and subdued blues. [Reggie's Portra recipe](https://fujixweekly.com/2022/06/11/fujifilm-x-trans-iv-film-simulation-recipe-reggies-portra/)

Generate the curves as above and copy `REGPORT.CV`, `REGPORT1.CV` and `REGPORT2.CV` to
`CHDK/CURVES/`. Append the row to `CHDK/FILMS/FILMS.TXT` and optionally to `FILMS.BAK` for
the preset picker. If `FILMS.TXT` is absent, start with a copy of `CHDK/FILMS/EXAMPLE.TXT`.

```text
Reggie Porta | 11 | 0 | -1 | 0 | 0 | 0 | 0 | 0 | REGPORT.CV | - | 0
```

For the full recipe, use Auto white balance, Fine grain, Dynamic+ Off and colour noise filter
Off. The curve already applies the warm cast, so leave WB shift centered. Save those settings
to a free recipe slot if you want to recall them together.

### Kodak Gold 200 (PowerShot A620)

**Kodak Gold 200** adapts Ritchie Roesch's Classic Chrome recipe for the A620. It adds golden
midtones, deeper shadows and softer highlights. The curve was previewed on A620 DNG/JPEG pairs.
[Kodak Gold 200 Fuji recipe](https://fujixweekly.com/2023/10/24/kodak-gold-200-fujifilm-x-t5-x-trans-v-film-simulation-recipe/)

Generate and install `GOLD200.CV`, `GOLD2001.CV` and `GOLD2002.CV` as above, then add this row
to `CHDK/FILMS/FILMS.TXT` and optionally to `FILMS.BAK`.

```text
Kodak Gold 200 | 11 | 0 | -2 | 1 | 0 | 0 | 0 | 0 | GOLD200.CV | - | 0
```

For the full recipe, use Daylight white balance, centered WB shift, Medium grain and colour
noise filter Off. Use Dynamic+2 for highlight protection in automatic exposure modes.

## Installation

Build for your camera model and firmware version. For the A620 1.00f, run
`sh tools/filmsim/build.sh a620` from the repository with the [build tools](#camera-build-requirements)
on your PATH. Extract the resulting build from `dist/v1.1.1/` to the root of the SD card.
On the A620, start manually in playback with **MENU → Firm Update**. For
autostart, use **EXCCD+ → System → Make Card Bootable...**, then lock the SD card and restart.

## Usage

On the A620, a short press of **Print** toggles ALT in **P**, **Night Scene**, **Landscape**,
**Portrait**, **Auto**, and playback. In other still-photo modes, press **Print** once for
**Quick ISO** and again from that screen for the film picker. **MENU** in the film picker opens
EXCCD+ options, where **Back** returns to normal ALT. Use **MENU** in ALT to open the original
CHDK main menu, or **FUNC/SET** to open the script menu directly.
Choose **Load Script from File...**, select a Lua or uBASIC script in `CHDK/SCRIPTS`, and
press the shutter fully in ALT to run it. `QUICKISO.LUA` is included in that folder.

To choose a look, enter ALT in still-photo mode and go to **MENU → EXCCD+ → Film selection...**.

On the A620, **MENU → EXCCD+ → Save Original JPEG** is available with a film recipe selected.
In single-shot drive mode it saves the Canon render first, then automatically takes a helper shot
to render the first exposure's sensor data with the recipe. Canon numbers the files in sequence,
for example `IMG_8345.JPG` for the original and `IMG_8346.JPG` for the recipe JPEG.
The original uses the exposure and Canon white balance of that capture. Release the shutter
to let the helper shot start. With this setting off, the camera saves one JPEG.

To profile a look, open **EXCCD+ → Performance Metrics** and select **On**.
The selector is available with any EXCCD+ look or a saved recipe selected. Each capture appends
one row to `CHDK/LOGS/PERF.CSV`. Filter the `mode` column for `single_photo` or
`keep_original_jpeg`. Paired rows name both JPEGs and show the temporary RAW write and read,
original JPEG processing, and wait for the helper capture.

The `_ms` columns report elapsed milliseconds. A blank means the stage was unused or timing
was unavailable. A zero means the timer did not advance during that stage. On the A620,
`raw_to_output_ms` includes the final JPEG close when it was observed. The JPEG columns
include Canon processing and the card write. On other cameras, timing ends at the RAW handoff
to Canon.

- **Show All Film Recipes** opens the film picker with the full `FILMS.TXT` catalogue, or the 26 built-in looks if that file is absent.
- **Select Only Presets** opens the same picker with the looks in `CHDK/FILMS/FILMS.BAK`.
  Both lists include **Off**. The last list you choose becomes the default for **Film selection...**
  and the quick-open button. Choosing a list keeps the active look until you select another.
- On the **A620 only**, a single **Left** press opens the film picker and a single **Right** press
  opens **Quick ISO** from the Canon shooting screen in **P**, **Night Scene**, **Landscape**,
  **Portrait**, and **Auto**. Other shooting modes use Canon's button controls.
  In **ALT → FUNC/SET**, set **EXCCD+ Quick Open Button**
  and **Quick ISO Button** to **Left**, **Right**, or **None**. A button can belong to only one shortcut.
  To swap them, set one to **None**, remap the other, and then assign the free button.
- On the **A620**, in **Quick ISO**, use Left and Right to change the selection. Press **FUNC/SET**
  or half-press the shutter to apply, or press **MENU** to cancel. Open it manually through
  **MENU → EXCCD+ → Quick ISO...** in ALT, including in **Tv**, **Av**, and **M**.

| Button | In the film picker |
|---|---|
| Left/right | Flip through the looks |
| Up/down | Jump five looks in the full catalogue or move one look in a short preset list |
| **FUNC/SET** | Apply the look and return to shooting |
| **MENU** | Return to the EXCCD+ options |
| **Print** | Leave ALT |

The film selection shows the keys at the bottom (*SET OK, MENU Options*). The Options menu is
grouped like this:

| Entry | What is in it | Saved in a recipe |
|---|---|---|
| Recipe | Load C1 to C4 with left/right | |
| Look: *film* ▸ | White balance, WB shift, Dynamic+, colour noise filter, B/W filter, Effects ▸ (grain, halation, mist, vignette, light leak, date, frame), Save as recipe ▸ | yes |
| Shutter, Focus, Zoom, Flash, ND filter, Double exposure | Settings for the situation | no |
| RAW ▸ | Save RAW, RAW develop, delete RAW files | no |
| Reset to defaults, System ▸ | Palette, language, build info, bootable card | no |

The name of the active look is shown at the bottom of the screen. For every photo,
`A/CHDK/FILMLOG.CSV` records which look was active. If a step had to be skipped for lack of
memory, the line says which (`low memory, skipped: ...`) and the status line shows `low mem!`
until a photo goes through completely again.

**RAW develop** (Options → RAW): render a saved DNG again with a different look. Set Options → RAW →
Save RAW to DNG when shooting. Later, choose RAW develop, pick the DNG and the look, then take one
photo: what is in front of the lens does not matter, the camera only needs a shot to run its
JPEG engine. The status line shows `Develop <file>: <look>` until then; choose RAW develop
again to cancel. You get a new JPEG. The DNG stays untouched, your loaded look comes back.

White balance, ISO, Dynamic+ level, date stamp and Auto colour noise filter come from the original
photo when available. The JPEG's other Exif data (date, shutter speed, aperture, orientation)
comes from the helper shot, so hold the camera the same way and do not use digital zoom.
`FILMLOG.CSV` names the source file in a third column. Canon's JPEG engine only runs after
a shot, so this cannot run in playback mode.

**Deleting RAW files** (Options → RAW): Canon's own delete only removes the JPEG, so DNGs pile up
on the card. *Delete RAW...* opens the same browser as RAW develop; SET deletes the DNG under the
cursor, also in full view (Right), after a yes/no question. *Delete RAW without JPEG* removes every
DNG whose JPEG you already deleted in Canon playback. *Delete all RAW* removes every DNG on the card.
Both say how many files and MB first; JPEGs are never touched.

**White balance** (Options → Look): sets the camera's own white balance (Auto, Daylight, Cloudy,
Tungsten, Fluorescent, Fluorescent H, Custom). **WB shift** fine-tunes the colour on top of
that, on a grid like the white balance shift some cameras offer: left/right moves from cyan to
red, up/down from yellow to blue, each −9 to +9. SET applies, MENU cancels, the zoom lever goes
back to 0. The shift is worked into the look's tone curve,
so it costs no time per photo. It only shows in the photo, not on the screen, and it is off
while the film is Off (the menu then shows "needs a look"). With black-and-white looks it works
like a weak colour filter. One step is about 3 %; this is not calibrated yet.

**Recipes** (Options → Recipe): a recipe stores the film, white balance, WB shift, Dynamic+,
colour noise filter, B/W filter and all effects in one of four slots, C1 to C4. Choosing a
recipe sets all of it at once, including the camera's white balance. Shutter, focus, zoom,
flash, ND, RAW and double exposure are left alone, they depend on the situation. To save one,
set everything up, then Options → Look → Save as recipe and pick a slot: the list shows the four
slots with their names, one press saves (a used slot asks first). A new slot is named after the film. If you change something after loading a recipe, the menu and the status
line show a `*` after its slot. The recipes live in `A/CHDK/FILMS/RECIPES.TXT`, a text file you
can edit on the PC to rename recipes, write new ones or share them (the file explains the
format). `wb = keep` leaves the camera's white balance as it is. Mistakes in the file end up in
`A/CHDK/FILMS/RECIPES.LOG`, and so does the reason when saving fails (file over 8 KB or
unreadable, card full). Saving on the camera rewrites the file, so your own comments in it
are lost.

To compare engines on the A620, enable **Performance Metrics** and take the same
set of shots with **EXFS** off and then on. Keep the recipe, capture mode, and RAW
output settings the same. Use the JPEG numbers to identify each set in
`CHDK/LOGS/PERF.CSV` and compare `hot_pixels_ms`, `tone_curve_ms`,
`white_balance_ms`, and `raw_to_output_ms`.

## Camera build requirements

- Your exact Canon model and firmware revision. The [camera list](tools/filmsim/cameras.txt)
  shows the configured EXCCD+ targets and firmware versions.
- A CHDK 1.6.1 port for that model and firmware, plus an EXCCD+ target entry if it is missing
  from the list. The film engine supports 10-bit and 12-bit RAW with RGGB or GBRG Bayer patterns.
  Other sensor formats need film-engine support. See the
  [camera porting guide](tools/filmsim/cams/README.md).
- A POSIX shell with host GCC, GNU make, Info-ZIP `zip`, and
  [xPack GNU Arm Embedded GCC 10.3.1-2.3](https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/tag/v10.3.1-2.3)
  available on `PATH`. On Windows, WSL provides the shell environment.

## License

- **Code:** GNU General Public License v2, like CHDK. See [`COPYING`](COPYING).
- **Looks** (curves, labels, film table): [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
  See [`LICENSE-LOOKS.md`](LICENSE-LOOKS.md).
- **Font** Archivo Black: SIL Open Font License 1.1, see [`tools/filmsim/fonts/OFL.txt`](tools/filmsim/fonts/OFL.txt).

The original CHDK readme is in [`README.CHDK`](README.CHDK).

## Disclaimer

EXCCD+ is an independent project. It is not affiliated with, endorsed by or sponsored by
Canon Inc. Canon, IXUS and PowerShot are trademarks of Canon Inc. and are used only to
name the supported camera.

Use at your own risk. CHDK and EXCCD+ come without any warranty (see `COPYING`).
