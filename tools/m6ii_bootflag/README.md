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
5. Enter playback with **PLAY**, then press **SET** once to trigger Canon Basic.
6. Wait about 5 seconds, then power the camera off normally.
7. Read `M6II_INFO.TXT` from the card root.
8. Continue only when the camera menu reports Canon firmware 1.1.1 and the
   internal ROM information identifies **5.9.2**. If it reports 5.9.3, is blank,
   or the script does not run, stop here.

The verifier only reads firmware information and writes one text file to the SD
card. It does not call `EnableBootDisk()`. On EOS/DIGIC 8 Canon Basic, the card is
addressed as `B:/`; this matches the maintained firmware-signature example tested
on EOS R / R6.

## Stage 2 - enable the camera BOOTDISK flag

Do this only after Stage 1 passes.

1. Keep the SD card marked **SCRIPT only**.
2. Delete the Stage 1 `extend.m` and copy `enable/extend.m` plus
   `enable/script.req` to the card root.
3. Insert the card and start the camera normally.
4. Enter playback with **PLAY**, then press **SET** once.
5. Wait about 5 seconds, then power the camera off normally.

The enable script intentionally contains only `EnableBootDisk()`, matching the
maintained `Universal/extend_bootdisk.m` behavior. There is no completion marker;
this minimizes extra Canon Basic calls during the persistent step.

## Stage 3 - make the Magic Lantern runtime card bootable

After Stage 2:

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

If the camera simply boots Canon firmware and Magic Lantern does not start, the
persistent BOOTDISK call may not have executed. Return to a SCRIPT-only card and
re-check the Canon Basic setup rather than repeatedly changing unrelated flags.

## Recovery / disable BOOTDISK

To reverse the persistent camera flag:

1. Use a card marked **SCRIPT only**, not a BOOTDISK autoboot card.
2. Copy `disable/extend.m` and `disable/script.req` to its root.
3. Boot normally, enter playback, press **SET** once.
4. Wait about 5 seconds and power off normally.
5. Remove the BOOTDISK marking from cards you want to use as normal Canon cards.

The disable script intentionally contains only `DisableBootDisk()`.

## Files

- `verify/extend.m` - read-only M6II firmware/ROM information probe, writes to `B:/M6II_INFO.TXT`.
- `enable/extend.m` - calls only `EnableBootDisk()`.
- `disable/extend.m` - calls only `DisableBootDisk()`.
- each stage contains the required `script.req` (`for DC_scriptdisk`).

The BOOTDISK calls are based on the maintained Canon Basic
`Universal/extend_bootdisk.m` example. The verifier is adapted from
`Universal/extend_fw_sign.m`, using the DIGIC 8 `E0040000` ROM0 base and the EOS
`B:/` card path.
