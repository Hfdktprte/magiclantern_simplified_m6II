# EOS M6 Mark II crop_rec / RAW preview port plan

Branch: `m6ii-crop-rec-preview`

Base: `m6ii-bilal-10-12bit-v1` at `e4ac7299e2f7751a4a216af67d82fb295d2a8c4b`

## Goal

Implement an M6 Mark II-safe crop_rec path whose first purpose is not legacy DIGIC-5 sensor overclocking, but a RAW-authoritative monitoring path:

1. Preview the same field of view / crop rectangle that MLV recording actually writes.
2. Use the RAW stream rather than Canon's scaled LiveView as the framing source.
3. Preserve Magic Lantern's normal Global Draw architecture: Global Draw stays ON for ML overlays, while Canon graphics are suppressed separately with the existing Kill Canon GUI / front-buffer mechanism.
4. Work with M6II 14/12/10-bit uncompressed RAW.
5. Preserve the existing M6II low-bit RAW EDMAC patch and its single ROM-remap page.

A later phase may add true sensor readout crop presets, but that must not be conflated with the preview/overlay work.

## Why legacy crop_rec cannot simply be enabled

The existing module is a DIGIC-5 design. It installs hardcoded CMOS_WRITE, ADTG_WRITE and optionally ENGIO_WRITE ROM hooks for 5D3/EOSM/700D/650D/100D/6D.

M6II has no supported init path in that module, and the old register families / addresses are not valid assumptions for DIGIC 8.

The current M6II low-bit RAW implementation already reserves the camera's available ROM-remap resource for the RAW EDMAC pitch hook. A multi-hook DIGIC-5 crop_rec port would conflict with that architecture.

Therefore no legacy crop_rec hook should be installed on M6II.

## Existing pieces we should reuse

### Recorder knows the exact recording crop

mlv_lite already computes:
- `skip_x`
- `skip_y`
- `res_x`
- `res_y`

Those values are the actual RAW recording rectangle. They are more authoritative than Canon LiveView framing.

Add a small weak/exported API from mlv_lite, for example:

```c
int mlv_lite_get_recording_rect(
    int *x, int *y, int *w, int *h, int *bpp);
```

The crop_rec preview should use this rectangle directly. If mlv_lite is unavailable, fall back to `raw_info.active_area`.

This also automatically follows panning/dolly changes.

### Core already has a RAW framing renderer

Existing recorder code uses:
- `raw_set_preview_rect(skip_x, skip_y, res_x, res_y, 1)`
- `raw_force_aspect_ratio(0, 0)`
- `raw_preview_fast_ex(...)`

That is exactly the old "Framing" preview concept: slower than Canon LV, but geometrically correct.

M6II should reuse the geometry and rendering concepts, but not depend on legacy display-filter redirection.

### M6II display target exists

M6II currently maps:
- `YUV422_LV_BUFFER_DISPLAY_ADDR` -> `DV_VRAM_PANEL`

So a RAW preview can be rendered into the panel image plane.

Heavy rendering must never run in VSYNC/interrupt context. VSYNC should only signal a normal DryOS worker task that renders the newest RAW frame.

## Phase 1 - M6II preview-only crop_rec

Create an M6II-specific path inside crop_rec with no CMOS/ADTG/ENGIO hooks.

Menu proposal:

- Crop/RAW Preview: OFF / RAW framing / RAW framing + zebras
- Preview quality: Color / Fast grayscale
- Preview rate: Auto / 15 / 30
- RAW zebras: OFF / ON
- Zebra high threshold: reuse ML value if available, otherwise local threshold
- Zebra low threshold: reuse ML value if available, otherwise local threshold

Do not add legacy sensor crop presets to the M6II menu yet.

### Runtime model

1. Module init creates one locked semaphore and one long-lived preview worker.
2. CBR_VSYNC only:
   - checks whether preview is enabled,
   - records/signals that a newer RAW frame exists,
   - gives the semaphore.
3. Worker:
   - coalesces pending signals so it never queues old frames,
   - obtains current recording rectangle from mlv_lite,
   - snapshots the RAW buffer address / geometry,
   - renders directly to the M6II display image buffer,
   - optionally draws RAW zebras using the same RAW geometry.

No filesystem, menu, semaphore waits, message-queue waits, or full-frame conversion in the VSYNC callback.

## Phase 2 - make RAW preview bit-depth aware

Current `raw_preview_fast_ex()` returns immediately unless `raw_info.bits_per_pixel == 14`.

This must be fixed before enabling preview on 12/10-bit recordings.

Add a generic packed-pixel accessor for 10/12/14-bit buffers. Do not change the already-working capture path.

Required API shape:

```c
int raw_get_pixel_bpp_ex(
    const void *buffer,
    int pitch,
    int bpp,
    int x,
    int y);
```

Then implement a preview renderer that reads 10/12/14-bit RAW through that accessor and scales black/white values according to the MLV RAWI metadata/current `raw_info`.

The bit unpacking must be validated against an actual M6II 10-bit and 12-bit MLV frame before camera use. The decoded samples should agree with the host/DNG unpack path. Do not guess packing order.

Acceptance:
- 14-bit preview matches current RAW framing.
- 12-bit preview has same geometry and normal tonal ordering.
- 10-bit preview has same geometry and normal tonal ordering.
- switching BPP does not touch the existing EDMAC/PackMode low-bit hook.

## Phase 3 - exact FOV

The preview rectangle must be exactly the recorder rectangle:

```
RAW sensor buffer
+-----------------------------------+
|                                   |
|        +------------------+       |
|        | MLV recording    |       |
|        | skip_x/skip_y    |       |
|        | res_x x res_y    |       |
|        +------------------+       |
|                                   |
+-----------------------------------+
```

The module should call `raw_set_preview_rect(skip_x, skip_y, res_x, res_y, ...)` or use equivalent private geometry.

Do not derive framing from:
- Canon 1920x1080 UI selection,
- Canon YUV crop,
- assumed sensor crop factor,
- display zoom.

Acceptance test:
record a static scene with objects touching all four displayed borders. The extracted MLV frame must contain the same four borders at the same relative positions.

## Phase 4 - use Magic Lantern's existing RAW-zebra architecture

Do not bypass Global Draw.

Stock ML behavior is explicit:
- `Global Draw` is the master gate for ML overlay graphics.
- `zebra_should_run()` requires `get_global_draw()`.
- the Zebras menu itself depends on `DEP_GLOBAL_DRAW`.
- therefore Global Draw OFF intentionally disables zebras, cropmarks and the rest of the ML overlay loop.

The separate historical mechanism for a clean Canon screen is `CONFIG_KILL_FLICKER` / `Kill Canon GUI`. Its own source comment says it blocks Canon drawing routines while allowing ML graphics. That is the behavior wanted here.

RAW zebras themselves must stay in `zebra.c`:
- `draw_zebras()` chooses RAW zebras when `RAW_ZEBRA_ENABLE && can_use_raw_overlays()`.
- LiveView RAW zebras are drawn by `draw_zebras_raw_lv()`.
- it maps screen coordinates to RAW coordinates with `BM2RAW_X/Y`.
- it samples RAW R/G/B values from the RAW buffer.
- it writes only the zebra overlay color into ML's bitmap/RGBA overlay and mirror buffer.

So the M6II implementation should extend the existing RAW overlay path rather than create crop_rec-owned zebras.

Required M6II core work:
- enable `FEATURE_RAW_ZEBRAS` for supported `CONFIG_RAW_LIVEVIEW` cameras, not only `CONFIG_RAW_PHOTO`;
- make `can_use_raw_overlays()` accept validated 10/12-bit M6II RAW instead of hard-rejecting non-14-bit;
- make RAW pixel sampling helpers understand the packed 10/12/14-bit stream;
- make crop_rec / mlv_lite set the preview rectangle so `BM2RAW_X/Y` maps to the exact MLV recording rectangle.

Acceptance:
- Global Draw LiveView/ON -> ML overlay task runs normally.
- Kill Canon GUI -> Canon bitmap graphics disappear while ML graphics remain.
- RAW zebras use the stock Zebras menu and stock zebra drawing loop.
- zebra positions correspond to the exact recorded RAW rectangle.
- disabling Global Draw disables the RAW zebras too, exactly like normal Magic Lantern.

## Phase 5 - clean Canon GUI while preserving ML overlays

Port/enable the existing Kill Canon GUI behavior on M6II only after verifying the front-buffer primitives on this camera.

The existing ML flow is:
- Global Draw remains enabled.
- `idle_kill_flicker()` disables Canon's GUI front buffer and clears stale Canon bitmap graphics.
- the zebra/global-draw task continues drawing ML graphics.
- when Canon UI must return, `idle_stop_killing_flicker()` re-enables the Canon front buffer.

Do not replace this with Global Draw OFF.

M6II currently does not define `CONFIG_KILL_FLICKER`, so first verify that:
- `canon_gui_disable_front_buffer()`,
- `canon_gui_enable_front_buffer()`,
- `canon_gui_front_buffer_disabled()`

operate correctly with the M6II XCM/WINSYS implementation.

Only after that verification should `CONFIG_KILL_FLICKER` be enabled for M6II.

The RAW image-plane preview and Canon bitmap GUI suppression are separate concerns:
- RAW preview controls the image/FOV.
- Kill Canon GUI controls Canon's graphics layer.
- Global Draw controls ML overlays.
- RAW zebras remain part of ML overlays.

## Phase 6 - recording-time performance

The RAW recorder has priority.

Preview worker rules:
- never block recording;
- skip preview frames freely;
- if RAW writer backlog rises, reduce preview to grayscale / lower rate;
- never copy full-resolution RAW unless required;
- render directly by downsampling into the 720x480 panel target;
- no extra full-resolution frame buffer just for preview.

Target behavior:
- idle: color RAW preview around 15-30 fps if CPU allows;
- recording: adaptive preview, possibly lower rate;
- recording integrity always wins over preview smoothness.

## Phase 7 - optional true sensor crop_rec modes

Only after preview/FOV/zebras are stable.

A real DIGIC-8 sensor crop port requires identifying M6II equivalents for the sensor readout configuration path. We must not transplant:
- old CMOS_WRITE addresses,
- ADTG registers,
- C0F ENGIO timing registers,
- 5D3/EOSM mode signatures.

Because the low-bit RAW branch uses the available ROM-remap resource, prefer:
1. existing state-object callbacks,
2. writable RAM parameter structures,
3. Canon functions/properties that accept readout geometry,
4. an existing already-owned hook point,

before considering any additional ROM hook.

This phase needs ROM/call-path analysis from M6II firmware, and should be a separate commit series from the preview implementation.

## Branch safety rules

- Do not alter the working 10/12-bit RAW capture logic merely to make preview easier.
- Do not enable sd_clock's competing M6II ROM hook in this branch.
- Do not enable legacy crop_rec camera presets for M6II.
- No high-level work in VSYNC context.
- Every renderer path must handle missing/invalid RAW/display buffers by returning safely.
- Preview failure must never stop RAW recording.

## Implementation order

1. Export current mlv_lite recording rectangle.
2. Add M6II crop_rec preview worker + menu, 14-bit only.
3. Verify exact FOV against an MLV frame.
4. Implement/validate generic 10/12/14-bit packed-pixel reader.
5. Enable low-bit RAW preview.
6. Add module-owned RAW zebras independent of Global Draw.
7. Optimize cadence/back-pressure behavior.
8. Only then research true DIGIC-8 sensor crop presets.

## First milestone definition

The first build is considered successful when, on ordinary Canon FHD mode:

- MLV Lite can be set to a cropped RAW recording resolution such as 1920x1080.
- M6II screen shows a RAW-derived preview whose borders exactly match the recorded MLV frame.
- 14/12/10-bit each display correctly.
- Global Draw is ON for LiveView/ML overlays.
- Kill Canon GUI can suppress Canon bitmap graphics without suppressing ML RAW zebras.
- RAW zebras are the normal Magic Lantern RAW-zebra implementation, not a crop_rec-private renderer.
- RAW recording remains functional and unchanged.
