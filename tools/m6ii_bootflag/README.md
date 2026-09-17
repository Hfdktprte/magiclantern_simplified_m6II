# EOS M6 Mark II BOOTDISK enable/install procedure

This is an experimental developer procedure for EOS M6 Mark II. It does not use Canon's Firmware Update menu.

## Supported target

The current Magic Lantern M6II port targets Canon firmware 1.1.1 with internal ROM version 5.9.2. Canon also shipped cameras that display 1.1.1 but have internal version 5.9.3. Do not proceed to the persistent BOOTDISK step until the camera has been identified as the supported target.

## Stage 1 - proven M6II Canon Basic test

Use a FAT32 card <=32 GB. In EOSCard 1.40 set:

- EOS_DEVELOP = OFF
- BOOTDISK = ON
- SCRIPT = ON
- activate the CHDK-RAW image/button as in the published M6M2 procedure

Click Save.

Copy `verify/extend.m` and `verify/script.req` to the root. The verifier intentionally matches the M6 Mark II procedure published at photomacrography.net:

    private sub Initialize()
        System.Create()
        CamInfo_Debug(1)
    end sub

Power on, press PLAY, then SET. A short LED flash should occur and `CAM_INFO.XML` should be written to the card. The EOSCard BOOTDISK setting here is only a marker on the SD card; it does not call Canon's persistent `EnableBootDisk()` function.

If `CAM_INFO.XML` is not created, STOP. Do not run the enable script.

## Stage 2 - persistent camera BOOTDISK flag

Only after Stage 1 succeeds and the camera firmware target has been checked, replace the verifier with `enable/extend.m` + `enable/script.req`. The enable script intentionally contains only `EnableBootDisk()`, matching the maintained universal Canon Basic example.

## Stage 3 - Magic Lantern boot card

After Stage 2, prepare the runtime card with EOSCard `EOS_DEVELOP = ON`, `BOOTDISK = ON`, `SCRIPT = OFF`, remove Canon Basic files, and place `autoexec.bin` plus `ML/` on the card root. Power on normally; do not use Firmware Update.

## Recovery

`disable/extend.m` contains only `DisableBootDisk()` and is the camera-side reversal path.
