# EOS M6 Mark II RAW bring-up

This document tracks the staged RAW-video bring-up for M6II.111.

The first rule is to change one unknown at a time. Do not enable `mlv_lite`,
redirect RAW EDMAC, or copy DIGIC 8 MMIO addresses from another camera until the
preceding stage has passed repeatedly on the M6 Mark II.

## Stage 1: Canon RAW LiveView pulse

Branch: `m6ii-raw-probe-v1`

The Debug menu contains **M6II RAW bring-up** with:

- **RAW pulse (500 ms)**: calls `lv_set_mm(1)`, then `lv_save_raw(1)`, waits
  500 ms, then calls `lv_save_raw(0)`.
- **Force RAW off**: calls `lv_save_raw(0)` as a recovery request.
- **Probe state**: reports only the probe's internal requested/busy state. It
  does not prove that Canon's RAW pipeline or an EDMAC channel is active.

### Preconditions

1. EOS M6 Mark II running firmware 1.1.1 internal build 5.9.2, matching the
   existing M6II.111 port.
2. Start from a card/build that already boots the current `dev` branch
   reliably.
3. Use a fully charged battery.
4. Do not run the probe while Canon video recording is active.
5. Start with normal movie LiveView, LCD active, no HDMI cable, no USB cable.

### Test A: baseline

Before installing the probe build, verify normal `dev` can:

1. Boot 5 times.
2. Enter/leave LiveView 10 times.
3. Open/close the ML menu 10 times.
4. Shut down normally.

Do not continue if the baseline is already unstable.

### Test B: one pulse

1. Boot the probe build.
2. Enter normal movie LiveView.
3. Open `Debug -> M6II RAW bring-up`.
4. Select `RAW pulse (500 ms)` once.
5. Wait several seconds.
6. Verify LiveView still updates, buttons work, ML menu opens, and shutdown is
   normal.

Record the visible result and save any crash/log files before another test.

### Test C: repetition

Only after Test B passes:

1. Repeat one pulse every 5-10 seconds for 10 pulses.
2. If stable, reboot and repeat for 50 pulses.
3. Between groups, verify Canon menu, playback, LiveView, and shutdown.

### Stop conditions

Stop testing immediately and preserve the card logs if any of these occur:

- Err70/Err80 or other Canon error.
- camera freeze or watchdog reboot.
- LiveView stops updating after the pulse.
- controls become unresponsive.
- shutdown hangs.
- the camera becomes unusually hot.

Do not add EDMAC register reads as a workaround for a failing Stage 1 test.
A Stage 1 failure means the Canon RAW activation path itself needs reverse
engineering first.

## Stage 2: identify the M6II RAW writer

Not implemented yet.

Goal: identify the M6II RAW LiveView write path/channel without redirecting it.
Reference DIGIC 8 work may suggest candidates, but addresses from EOS R, M50,
or SX740 are hypotheses only.

Required evidence before Stage 3:

- a channel/path changes consistently when the Stage 1 RAW request is active;
- the same observation is repeatable after reboot;
- any required DIGIC 8 power domain is understood before MMIO access;
- no hard lock is produced by observation itself.

## Stage 3: redirect exactly one RAW frame

Not implemented yet.

Goal: allocate a buffer, redirect exactly one completed RAW frame into it,
restore the Canon path, and verify that the buffer contains changing Bayer
image data.

No disk video recording at this stage.

## Stage 4: double-buffer RAW capture

Not implemented yet.

Alternate two capture buffers on VSYNC and verify hundreds/thousands of frames
without writing an MLV file.

## Stage 5: DIGIC 8 memory-to-memory copy

Not implemented yet.

Port the algorithm from the working D8 experiments, but resolve M6II-specific
stubs and power-domain details from M6II firmware. First prove byte-exact
RAM-to-RAM copies before using the path to crop RAW frames.

## Stage 6: minimal MLV recorder

Not implemented yet.

Use a small crop, no audio, no lossless compression, and conservative data rate.
The first target is a valid 5-10 second MLV, not maximum resolution.

Only after this path is stable should `mlv_lite` be enabled for M6II.
