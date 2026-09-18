# EOS M6 Mark II crop_rec / RAW preview port plan

Branch: `m6ii-crop-rec-preview`

Base: `m6ii-bilal-10-12bit-v1` at `e4ac7299e2f7751a4a216af67d82fb295d2a8c4b`

## Goal

Implement an M6 Mark II-safe crop_rec path whose first purpose is not legacy DIGIC-5 sensor overclocking, but a RAW-authoritative monitoring path:

1. Preview the same field of view / crop rectangle that MLV recording actually writes.
2. Use the RAW stream rather than Canon's scaled LiveView as the framing source.
3. Support the historical RAW-video "Kill Global Draw" optimization while retaining RAW zebras as a deliberate, narrow exception.
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

## Phase 4 - historical Kill Global Draw + RAW-zebra exception

The old crop-rec/mlv_lite branch had:
```c
static CONFIG_INT("raw.killgd", kill_gd, 0);

if (kill_gd)
{
    if (!unhack)
    {
        idle_globaldraw_dis();
        clrscr();
    }
    else
    {
        idle_globaldraw_en();
    }
}
```

That is a true Global Draw kill. In current core, `idle_globaldraw_dis()` increments `idle_globaldraw_disable`, and `get_global_draw()` returns false while it is set. As a result `zebra_should_run()` becomes false and the normal ML overlay loop stops. This is intentional and is why stock Kill Global Draw also kills zebras.

We still want that performance behavior, but with one explicit exception: RAW zebras.

Do NOT create a separate crop_rec zebra algorithm. Reuse Magic Lantern's existing RAW-zebra implementation:
- `draw_zebras()` selects RAW mode through `RAW_ZEBRA_ENABLE && can_use_raw_overlays()`.
- LiveView RAW zebras are actually drawn by `draw_zebras_raw_lv()`.
- `draw_zebras_raw_lv()` maps screen coordinates with `BM2RAW_X/Y`, reads RAW R/G/B samples, and writes only zebra colors into the ML bitmap/RGBA overlay and mirror.

Implementation:
- restore/port the historical `raw.killgd` recording option;
- add a small core flag/API meaning "RAW-video kill-GD is active but RAW zebras are requested";
- when the normal zebra task sees Global Draw killed, allow only a lightweight RAW-zebra iteration under that flag;
- call the existing `draw_zebras_raw_lv()` path, not histogram, focus peaking, cropmarks, waveform, lens bars or other Global Draw work;
- keep `get_global_draw()` false, so all normal overlay consumers remain disabled;
- clear the zebra overlay when the exception is disabled/stopped.

Required M6II core work:
- enable `FEATURE_RAW_ZEBRAS` for M6II RAW LiveView;
- make `can_use_raw_overlays()` accept validated M6II 10/12/14-bit streams;
- make the RAW sampling helpers understand M6II packed 10/12/14-bit data;
- make the recording rectangle drive `raw_set_preview_rect()`, so `BM2RAW_X/Y` maps zebra positions to the exact MLV crop.

Acceptance:
- Kill Global Draw OFF: normal ML overlays behave normally.
- Kill Global Draw ON: histogram/cropmarks/focus peaking/info overlays stop exactly as historically.
- with RAW zebras enabled, only RAW zebra stripes continue to draw.
- zebra computation uses stock `draw_zebras_raw_lv()`.
- zebra placement matches the recorded RAW crop.

## Phase 5 - Canon GUI suppression is separate

Historical mlv_lite also disabled Canon's front buffer under "Small hacks". That is independent from Kill Global Draw.

Do not conflate:
- `raw.killgd`: disables ML Global Draw for performance;
- Canon front-buffer suppression: disables Canon graphics;
- RAW preview: replaces/overlays the image using RAW geometry;
- RAW-zebra exception: the one ML overlay intentionally allowed while kill-GD is active.

Canon GUI suppression can be ported later after M6II front-buffer behavior is verified, but it is not the mechanism used to preserve zebras under Kill Global Draw.

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
- historical Kill Global Draw can be enabled while recording.
- all ordinary Global Draw work is stopped.
- RAW zebras remain as the only explicit exception and use Magic Lantern's existing RAW-zebra implementation.
- RAW recording remains functional and unchanged.
