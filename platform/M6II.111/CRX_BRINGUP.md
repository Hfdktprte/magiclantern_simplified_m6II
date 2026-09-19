# EOS M6 Mark II 1.1.1 — DIGIC 8 lossless RAW / CRX bring-up

This branch is a diagnostic branch for locating Canon's native still-image RAW
compression path before attempting to call it from `mlv_lite`.

## What is already proven

The existing M6II low-bit RAW work identifies the LiveView Mem1Path used for
ML RAW recording. It is not a CRX compressor.

Known LiveView RAW path facts:

- RAW writer EDMAC logical channel: 3
- channel MMIO: `0xD0420200`
- RAW state base: `0x00010970`
- state + `0x58`: current RAW buffer
- state + `0x10`: width
- state + `0x14`: height
- state + `0x74`: PackMode (2 = normal 14-bit on observed M6II state)
- Pack/unpack engine MMIO: `0xD0422200`
- channel 3 PUID: 2

The code slice dumped as `M6II_RAWCODE.BIN` contains
`LiveViewDrive::Mem1Path.c`, `Mem1Path EngRscLock`, and
`Mem1Path EngRscUnlock`. It contains no identified CRX compressor entry
point. Therefore the LiveView pack/unpack engine must not be treated as the
still-image lossless compressor.

## Canon/ML APIs already stubbed

Relevant M6II stubs include:

- `CreateResLockEntry`: `0xE057978A`
- `LockEngineResources`: `0xE0579972`
- `UnLockEngineResources`: `0xE0579A0C`
- `PwrMng_WakeSubChips`: `0xE0630EB6`
- `PwrMng_SuspendSubChips`: `0xE0630EE4`
- `edmac_set_address`: `0xE0580962`
- `edmac_set_size`: `0xE058096E`
- `edmac_set_transfer_mode`: `0xE0580E5A`

These are useful once the still-image compressor call chain is located.

## Probe v1

Debug -> `Trace Canon RAW still`:

1. Camera must be in a still-photo mode.
2. Select Canon image quality **RAW only** (not C-RAW and not RAW+JPEG).
3. Run the menu item.
4. The probe enables Canon's full stored DryOS debug stream, inserts
   `M6II_CRX_TRACE_BEGIN`, calls ML's normal `take_a_pic(0)`, inserts
   `M6II_CRX_TRACE_END`, then saves the ring with `dumpf`.
5. Retrieve the newest `logNNNN.log`.

Also run Debug -> `Dump ROM and RAM` once. On DIGIC 8 this writes:

- `ML/LOGS/ROM0.BIN`: `0xE0000000`, 0x02000000 bytes
- `ML/LOGS/ROM1.BIN`: `0xF0000000`, 0x01000000 bytes

The M6II implementation copies ROM through RAM before FIO writes because D8
rejects direct ROM pointers in `FIO_WriteFile`.

## What to extract from the trace + ROM

Search the bracketed still-capture trace for image-pipeline component names,
resource acquisition, DMA setup, and completion callbacks. Cross-reference
those strings/call sites in ROM0/ROM1. The objective is to identify:

1. still RAW input producer and buffer;
2. native CRX compressor setup function(s);
3. input DMA channel/connection and geometry;
4. compressed output buffer/memory suite and output-size return;
5. resource-lock IDs and required sub-chip power domains;
6. start/complete/cleanup sequence.

Only after those are known should a v2 probe call the compressor on a copied
single ML RAW frame.

## Validation requirement

Do not tag CRX output as `MLV_VIDEO_CLASS_FLAG_LJ92`. CRX and LJ92 are
different codecs. A CRX experiment must save the compressed payload separately
(or define explicit metadata/codec tagging) and verify it with an independent
CRX decoder. Lossless qualification requires:

`decode(encode(raw)) == raw` byte-for-byte for the selected input precision.
