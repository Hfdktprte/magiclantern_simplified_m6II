# EOS M6 Mark II BOOTDISK enable/install procedure

This is an **experimental developer procedure** for EOS M6 Mark II.
It does not use Canon's Firmware Update menu and does not require a custom `.FIR`.
Instead it uses Canon Basic for a read-only preflight, then calls Canon's own
`EnableBootDisk()` function, and finally boots Magic Lantern from a card marked
`EOS_DEVELOP` + `BOOTDISK`.

## Supported target

The current Magic Lantern M6II port targets Canon firmware **1.1.1 with internal
ROM version 5.9.2**. Canon also shipped cameras that display 1.1.1 but have
internal version **5.9.3**. Do not proceed to the persistent BOOTDISK step unless
the verifier reports the supported 5.9.2 target.

## Important safety rules

- Use a fully charged battery.
- Use a spare SD card for the Canon Basic installer stage; FAT32 / <=32 GB is the
  conservative choice for this first test.
- Do **not** use Canon Firmware Update for these files. There is no installer FIR.
- During the Canon Basic stage, mark the card with **SCRIPT only**. Do not mark it
  BOOTDISK yet.
- After the camera BOOTDISK flag is enabled, never insert a card marked BOOTDISK
  unless a valid `autoexec.bin` is present on that card.
- If the camera freezes or remains black, remove the battery promptly and boot
  with a normal, non-bootable card.

## Stage 1 - read-only firmware verification

1. Format a spare SD card FAT32.
2. With EOSCard on Windows, mark **SCRIPT** only. Leave `EOS_DEVELOP` and
   `BOOTDISK` unchecked.
3. Copy these two files from `verify/` to the root of the card:

   - `extend.m`
   - `script.req`

4. Insert the card and start the camera normally.
5. Press **PLAY**, then **SET** to trigger Canon Basic.
6. Wait a few seconds for card activity, then power the camera off normally.
7. Read `M6II_INFO.TXT` from the card. `CAM_INFO.XML` may also be created.
8. Continue only when the camera menu reports Canon firmware 1.1.1 and the
   internal ROM information identifies **5.9.2**. If it reports 5.9.3, is blank,
   or the script does not run, stop here.

The verifier only reads firmware information and writes text/XML files to the SD
card. It does not call `EnableBootDisk()`.

## Stage 2 - enable the camera BOOTDISK flag

Do this only after Stage 1 passes.

1. Keep the SD card marked **SCRIPT only**.
2. Delete the Stage 1 `extend.m` and copy `enable/extend.m` plus
   `enable/script.req` to the card root.
3. Insert the card and start the camera normally.
4. Press **PLAY**, then **SET**.
5. Wait a few seconds, then power the camera off normally.
6. Check the card for `BOOTDISK_ENABLED.TXT`.

The enable script calls Canon's own `EnableBootDisk()` function. This is the one
persistent camera-side change required for normal Magic Lantern autoboot.

## Stage 3 - make the Magic Lantern runtime card bootable

Only after `BOOTDISK_ENABLED.TXT` exists:

1. Put the SD card back in the computer.
2. In EOSCard:
   - uncheck `SCRIPT`;
   - check `EOS_DEVELOP`;
   - check `BOOTDISK`;
   - save the card flags.
3. Remove `extend.m` and `script.req` from the card root.
4. Extract the M6II Magic Lantern runtime ZIP to the card root. The root must
   contain at least:

       autoexec.bin
       ML/

5. Safely eject the card, insert it in the camera and simply power the camera on.
   Do **not** select Firmware Update.

If the camera boots Canon firmware but Magic Lantern does not start, stop and
inspect the card flags/files rather than repeatedly modifying the camera flag.

## Recovery / disable BOOTDISK

To reverse the persistent camera flag:

1. Use a card marked **SCRIPT only**, not a BOOTDISK autoboot card.
2. Copy `disable/extend.m` and `disable/script.req` to its root.
3. Boot normally, press **PLAY**, then **SET**.
4. Power off and verify `BOOTDISK_DISABLED.TXT` was created.
5. Remove any BOOTDISK marking from cards you want to use as normal Canon cards.

## Files

- `verify/extend.m` - read-only M6II firmware/ROM information probe.
- `enable/extend.m` - calls `EnableBootDisk()` and writes a completion marker.
- `disable/extend.m` - calls `DisableBootDisk()` and writes a completion marker.
- each stage contains the required `script.req`.

The BOOTDISK enable/disable calls are intentionally tiny and are based on the
maintained Canon Basic `Universal/extend_bootdisk.m` example. The M6II verifier
uses the DIGIC 8 firmware-information pattern and the M6II `E0040000` main ROM
base used by the current port.
