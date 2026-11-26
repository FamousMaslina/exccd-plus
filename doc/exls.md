# EXLS (ExLUTShooting)

EXLS grades still photos in-camera on the Canon PowerShot A620 with firmware
1.00f. Canon finishes the processed Bayer data and writes the JPEG at the
resolution and quality selected in the Canon menu. Select **L** for 3072 × 2304.

## Shoot with a LUT

1. Copy a full-range SDR sRGB `.cube` file into `CHDK/LUTS` on the SD card.
   If the A620 file browser truncates the name, use a short name such as
   `CHROME17.CUB`. The short extension uses the same Cube format.
2. In **ALT → MENU → EXCCD+ → EXLS settings**, choose **Select Cube** and pick
   the file. Return to **EXCCD+** and turn on **EXLS 1.0** alongside the existing
   engine choices. An unreadable or invalid file selection leaves the previous
   selection in place.
3. Set **LUT intensity** from 0% to 100%. At 0%, EXLS skips the LUT and Canon
   renders the exposure with **My Colors Off**. The engine, file path, and
   intensity are saved in the CHDK configuration.
4. Exit ALT and shoot. The shooting OSD shows the Cube filename and intensity.
   **Show used engine OSD** adds the selected engine on its own line.

EXLS uses **My Colors Off** as its base and keeps Canon's selected white balance.
The film recipe, film curves, Dynamic+, film WB shift, multiple exposure and
creative RAW effects are bypassed while EXLS is active. Camera exposure
overrides and file options still apply. Selecting any EXFS or EXFS HQ engine
turns EXLS off. Turning the selected engine off returns to standard EXCCD.

**Save Original JPEG** keeps a neutral Canon JPEG from the original exposure
and writes the EXLS JPEG with the next Canon file number. Release the shutter
to let the existing helper-shot workflow finish. Pairing is available in
single-shot still mode outside script, USB remote and PTP remote capture.
It skips the helper at 0% and when no Cube path is selected. A failed EXLS
render preserves the original and discards the helper output. Save RAW/DNG
captures the original exposure before LUT processing in both single and
paired shooting. For manual **RAW develop**, select a compatible saved RAW or
DNG, exit ALT and take the helper shot to develop it with the selected Cube.

## Cube files

EXLS accepts one red-fastest 3D table at 9³, 17³, 32³ or 33³ points. It reads
optional `TITLE`, per-channel `DOMAIN_MIN` / `DOMAIN_MAX`, or the equivalent
shared `LUT_3D_INPUT_RANGE`. The default input domain is 0 to 1. EXLS reads
decimal and scientific notation, comments and CRLF or LF line endings.
Files can be up to 4 MiB, with up to 511 bytes per line. Numeric values must
fit signed Q12 (approximately −8 to +8). Out-of-domain inputs use the nearest
boundary and interpolated outputs are clipped to the display range.

Export the look as a **full-range, display-referred sRGB to sRGB** Cube from
the editor. For SonyLutPort Chrome 64, use the 17³ or 33³ file in **sRGB CUBE**,
or a 32³ variant when supplied. Lightroom/ACR profiles require their companion
sRGB Cube or a Cube baked from the creative colour transform. XMP/DCP
containers, 1D shapers, combined 1D/3D files, log, HDR and wide-gamut input
conversions are outside this engine's file and colour-space support. Spatial
effects such as sharpening, grain and local adjustments do not fit a 3D LUT.
The appearance depends on the base rendering and exposure used to author it.
See [Adobe's Photoshop Cube export documentation](https://helpx.adobe.com/photoshop/desktop/save-and-export/export-files-to-different-formats/export-color-lookup-tables.html).

## Bayer and colour processing

The EXLS profile models neutral A620 JPEGs using white-balanced 10-bit RAW,
a fitted 3×3 sensor-to-RGB matrix and an invertible display tone response.
The daylight fit used the matching `CRW_8512`, `CRW_8514` and `CRW_8516`
RAW/JPEG pairs in SonyLutPort. With the A620 samples, the JPEG crop registered
at sensor coordinates (56, 24). The fit used low-variation 12×12-pixel patches
away from display clipping, camera WB gains from AsShotNeutral, a unit-row-sum
least-squares matrix and a shared monotone tone curve. The deepest shadows and
highlight endpoint use interpolated extrapolation to black and white. The
engine uses the exposure's WB, with a daylight fallback for invalid gains.
The paired JPEG uses the original exposure's WB for the LUT and applies the
existing helper WB compensation before Canon encodes it.

EXLS bakes the Cube into sensor-space deltas for the current WB and intensity.
At each grid point it subtracts sensor black, maps the balanced RGB estimate
through the fitted matrix and display curve, samples the Cube with tetrahedral
interpolation, and blends in display RGB. It inverts the colour change through
the tone model, inverse matrix and WB, then adds that change to the original
sensor samples. This retains sensor headroom outside the model's display
gamut. The work grid allocates extra points near black. The packed RAW pass
uses fixed-point tetrahedral interpolation once per RGGB block and gives both
green sites the same delta. The optical-black margins and bad-pixel sentinels
stay intact. The delta fades over the final 64 RAW codes approaching
sensor saturation, leaving saturated blocks to Canon's highlight handling.

Canon still controls demosaicing, noise reduction, tone, sharpening, colour
and JPEG encoding. Its actual response varies with scene, ISO, illumination
and camera settings, so the daylight profile approximates the appearance.
Strong grades can show colour error around fine saturated edges because EXLS
estimates colour on a 2×2 grid. Intensity, the sensor's 10-bit quantization,
highlight protection and the JPEG pipeline all affect the attainable grade.
The live view shows the Canon base and the LUT name. Review the captured JPEG
to see the grade.

## Timing and failures

Enable **Performance Metrics** to measure EXLS with or without a film recipe.
In `CHDK/LOGS/PERF.CSV`, `tone_curve_ms` is LUT load and bake time,
`channel_mix_ms` is the packed RAW pass, and `raw_to_output_ms` covers through
the JPEG close when observed. `engine` records EXLS 1.0 on success and Canon on
bypass or fallback. `status` distinguishes `exls_bypass` (0% or an identity
table), `exls_no_lut`, `exls_io`, `exls_invalid` and `exls_memory`.

The renderer uses Canon's existing RAW buffer. It streams the supplied Cube
two blue-axis slices at a time and samples the full source grid when baking
the 17³ camera-space work grid. Peak EXLS scratch space is about 43 KiB for
a 17³ input and 53 KiB for 33³, including parser and tone tables. A 13³ work
grid is used when the 17³ allocation fails. Fine variations in a dense Cube
can soften when sampled into the smaller work grid. LUTs are read for each
capture so card replacements take effect and memory is available between shots. Load,
validation and allocations finish before any LUT data is written into RAW.
Failures show a message and leave Canon with the original image data for a
single capture. Use 17³ for lower memory use and faster file loading.

Native shooting was confirmed on the A620 with Chrome 64. A USB-initiated
Lua capture on the same camera fell back to Canon after a Cube read error.
The result of a scripted capture can differ from a normal shutter press.

## Developer verification

Build with the repository's supported ARM toolchain.

```sh
make PLATFORM=a620 PLATFORMSUB=100f fir
```

The standalone host test exercises all four grid sizes, input syntax,
identity and 0% bypass, intensity, Bayer margins and failure handling. It
also accepts a Cube and the original A620 `CRW_8512.DNG` to exercise the
full-size packed sensor buffer.

```sh
gcc -std=c99 -O2 -Icore tools/filmsim/test_exls.c core/exls_cube.c core/exls_lut.c -o bin/test-exls
bin/test-exls "path/to/Chrome 64 - 17x17x17 - sRGB.cube" "path/to/CRW_8512.DNG"
```

The MinGW host integration suite also exercises a neutral single capture and
a paired fallback when the Cube goes missing. Run
`python tools/filmsim/test_filmsim.py` to include the EXCCD, EXFS and EXFS HQ
regression scenarios.

On the three SonyLutPort samples, the host camera model predicted the Chrome 64
17³ output within 2.4–3.5 8-bit RGB levels RMS of the result of applying the Cube
directly to the corresponding Canon JPEG. The comparison used flat,
unclipped patches from the three calibration scenes. With no grade, the
distance to the Cube target on those patches was 9.2–11.3 levels RMS.

CHDK Lua exposes the engine at config ID `1288`, intensity at `1289` and
the SD path at `1290`. Setting `1288` to 1 clears the EXFS and EXFS HQ flags.
