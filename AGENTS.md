# Repository instructions

## Git publishing

- Push branches and tags and publish GitHub releases only to `https://github.com/FamousMaslina/exccd-plus`.
- Treat the original EXCCD repository, `https://github.com/35mmSebas/exccd`, as fetch-only. Keep its push URL set to `DISABLED`.
- Verify the repository owner and name before every remote write.

## Releases

- Use the same grammar, writing style, and update-log format as the previous EXCCD+ releases.
- Update the root `README.md` for every release. Follow the existing structure and version entry format.
- Publish source-only releases without firmware ZIPs or installer binaries attached.

## Engine terminology

- `EXFS` without `HQ`, or `STD` / `std` on its own, means standard EXFS.
- `HQ` or `EXFS HQ` means the high-quality ExFilmSim engine.
- Refer to the original engine as `EXCCD` or `standard EXCCD`.
- Preserve EXFS 1.0 when changing EXFS 1.1. With the same RAW input, recipe,
  settings, and Grain seed, both engines must produce byte-identical processed
  RAW. Finish camera testing by comparing the processed RAW and final JPEG for
  every preset in the card's `A/CHDK/FILMS/FILMS.BAK`.

## A620 over USB on Windows

Power on the A620 with the SD card inserted and CHDK running, then connect a USB data cable. Start in playback. Windows should list USB ID `04A9:30FC` with `wpdmtp.inf` in `pnputil /enum-devices /connected /bus USB`. [`tools/chdk-wpd.cpp`](tools/chdk-wpd.cpp) sends CHDK PTP commands through the built-in [Windows Portable Devices MTP driver](https://learn.microsoft.com/en-us/windows/win32/wpd_sdk/supporting-mtp-extensions).

### SD card changes and CHDK autoboot

- Get Tudi's explicit green light before modifying the SD card. Include the files or settings and the purpose in the request. This applies to writes over USB and through a card reader, including test captures, staging files, firmware changes, and enabling autoboot. Keep each action within the scope of the approval.
- Before changing a boot file, download the current `DISKBOOT.BIN`, `PS.FIR`, and any modules being replaced. Record their SHA-256 hashes and confirm that a card reader is available for recovery if CHDK USB stops responding.
- Prefer the Windows card reader for A620 boot-file changes. Two USB file switches passed readback and then lost the `DISKBOOT.BIN` directory entry after a CHDK restart. When Tudi requests a USB install, use CHDK's soft `shut_down()` from playback after verifying both files, then check autoboot after a physical power-on. That sequence preserved the boot file in testing. Use the restart command once the boot file is already verified after a cold boot.
- Stage the candidate under a separate name and verify its SHA-256. Keep the current boot file intact under a rollback name when switching to the verified candidate. Verify the installed boot file by readback, then safely eject the SD card. If CHDK fails to load, stop the deployment and get approval for the recovery step.
- At the end of the approved installation, confirm the A620 card is FAT16 with the `BOOTDISK` marker and the physical SD lock engaged. Power the camera fully off and on in playback, then verify that CHDK starts automatically and answers the CHDK PTP version command over USB.

Always take test photos with the flash off. In record mode, set property 16 to 2. Half-press the shutter and check that `get_flash_mode() == 2` and `get_prop(79) == 0` before pressing the shutter fully. Release the half-press and abort the shot if either check fails.

Build and run from the repository root in PowerShell. Adjust the MinGW and Windows SDK paths if needed.

```powershell
$out = Join-Path $env:TEMP 'exccd-a620-usb'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$chdk = Join-Path $out 'chdk-wpd.exe'
$sdk = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um'
& 'C:\msys64\mingw64\bin\g++.exe' -std=c++17 -O2 -municode -static -idirafter $sdk .\tools\chdk-wpd.cpp -o $chdk -lole32 -lportabledeviceguids -luuid
& $chdk version
& $chdk download A/CHDK/FILMLOG.CSV (Join-Path $out 'FILMLOG.CSV')
& $chdk lua 'return get_config_value(1277),get_config_value(1278)'
```

- CHDK Lua setting `1277` is **Save original JPEG** and `1278` is **Performance metrics**. Both use `0` for Off and `1` for On. Use `set_config_value(id, value)` and verify with `get_config_value(id)`. For a temporary test, read and restore the starting values in the same Lua script with `set_config_autosave(false)`.
- The engine settings are `1279` for **EXFS 1.0**, `1284` for **EXFS 1.1**, `1280` for **EXFS HQ 1.0**, and `1285` for **EXFS HQ 1.1**. Setting one to `1` clears the other three. Four zeros select EXCCD.
- To take a photo, run the Lua below with `& $chdk lua-file <local-script.lua>` in a single-shot still-photo mode. It returns the path of the new JPEG. Run `& $chdk download <camera-path> <local-path>` with that path. Download `A/CHDK/LOGS/PERF.CSV` to retrieve performance metrics. CHDK script captures produce one JPEG even when Save original JPEG reads On because they bypass EXCCD+ JPEG pairing.
- To upload, run `& $chdk upload <local-path> <A/camera-path>`. The USB smoke test used a temporary file under `A/CHDK/DATA`, verified the SHA-256 after downloading it again, and removed it.
- Before reboot, run `& $chdk lua 'set_record(false); sleep(1500); assert(not get_mode()); return get_tick_count()'`. Then run `& $chdk reboot` and retry `& $chdk version` every two seconds for up to a minute. The camera may disappear from Windows during startup. Once PTP responds, check that `get_tick_count()` has reset and download the log again.

```lua
if not get_mode() then set_record(true) end
local deadline = get_tick_count() + 8000
while not get_mode() and get_tick_count() < deadline do sleep(50) end
local rec, video = get_mode()
assert(rec and not video and get_drive_mode() == 0)
set_prop(16, 2)
local before = get_exp_count()
press("shoot_half")
deadline = get_tick_count() + 2500
while not get_shooting() and get_tick_count() < deadline do sleep(20) end
if get_flash_mode() ~= 2 or get_prop(79) ~= 0 then
    release("shoot_half")
    error("Flash can fire")
end
press("shoot_full_only")
sleep(150)
release("shoot_full_only")
release("shoot_half")
deadline = get_tick_count() + 30000
while get_exp_count() == before and get_tick_count() < deadline do sleep(20) end
local after = get_exp_count()
assert(after ~= before)
local path = string.format("%s/IMG_%04d.JPG", get_image_dir(), after)
while not os.stat(path) and get_tick_count() < deadline do sleep(20) end
assert(os.stat(path))
return path
```

On 2026-11-23, the A620 1.00f running CHDK 1.6.1 answered PTP 2.9. The initial `FILMLOG.CSV` download was 5,418 bytes. Both settings read back correctly at Off and On. Their starting values were restored to Save original JPEG On and Performance metrics Off. Temporarily enabling metrics for the test shot produced a `single_photo;ok` row for `IMG_8432.JPG`. The downloaded JPEG decoded at 3072 by 2304 pixels and matched the 2,515,459-byte size reported on camera. The 262,144-byte scratch upload matched its readback. After a USB-initiated reboot, PTP 2.9 returned with a reset uptime and the same settings. The 5,456-byte post-shot log had the same hash before and after reboot.
