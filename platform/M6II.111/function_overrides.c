/** \file
 * Function overrides needed for R 1.8.0
 */
/*
 * Copyright (C) 2021 Magic Lantern Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <config.h>
#include <consts.h>
#include <lens.h>
#include <edmac.h>
#include <patch.h>

struct chs_entry
{
    uint8_t head;
    uint8_t sector;
    uint8_t cyl_lsb;
}__attribute__((packed));

struct partition
{
    uint8_t  state;
    struct   chs_entry start;
    uint8_t  type;
    struct   chs_entry end;
    uint32_t start_sector;
    uint32_t size;
}__attribute__((aligned,packed));

struct partition_table
{
    uint8_t  state;
    uint8_t  start_head;
    uint16_t start_cylinder_sector;
    uint8_t  type;
    uint8_t  end_head;
    uint16_t end_cylinder_sector;
    uint32_t sectors_before_partition;
    uint32_t sectors_in_partition;
}__attribute__((packed));

void fsuDecodePartitionTable(void * partIn, struct partition_table * pTable){
    struct partition * part = (struct partition *) partIn;
    pTable->state      = part->state;
    pTable->type       = part->type;
    pTable->start_head = part->start.head;
    pTable->end_head   = part->end.head;
    pTable->sectors_before_partition = part->start_sector;
    pTable->sectors_in_partition     = part->size;
    pTable->start_cylinder_sector = 0;
    pTable->end_cylinder_sector   = 0;
}

void gui_init_end(void){ }

extern void gui_enqueue_message(uint32_t, uint32_t, uint32_t, uint32_t);
void GUI_Control(int bgmt_code, int obj, int arg, int unknown){
    gui_enqueue_message(0, bgmt_code, obj, arg);
}

void LoadCalendarFromRTC(struct tm *tm)
{
    _LoadCalendarFromRTC(tm, 0, 0, 16);
}

int get_task_info_by_id(int unknown_flag, int task_id, void *task_attr)
{
    struct task *task = first_task + (task_id & 0xffff);
    return _get_task_info_by_id(task->taskId, task_attr);
}

extern int _FIO_GetFileSize64(const char *, void *);
int _FIO_GetFileSize(const char * filename, uint32_t * size){
    uint32_t size64[2];
    int code = _FIO_GetFileSize64(filename, &size64);
    *size = size64[0];
    return code;
}

/* Legacy EDMAC entry points are not used by the M6II D8 mlv_lite backend. */
void SetEDmac(unsigned int channel, void *address, struct edmac_info *ptr, int flags) { return; }
void ConnectWriteEDmac(unsigned int channel, unsigned int where) { return; }
void ConnectReadEDmac(unsigned int channel, unsigned int where) { return; }
void StartEDmac(unsigned int channel, int flags) { return; }
void AbortEDmac(unsigned int channel) { return; }
void RegisterEDmacAbortCBR(int channel, void (*cbr)(void*), void* cbr_ctx) { return; }
void UnregisterEDmacAbortCBR(int channel) { return; }
void RegisterEDmacPopCBR(int channel, void (*cbr)(void*), void* cbr_ctx) { return; }
void UnregisterEDmacPopCBR(int channel) { return; }
void _EngDrvOut(uint32_t reg, uint32_t value) { return; }

/* Bilal DIGIC 8 behavior: no shamem layer on D8; validated 0xDxxxxxxx
 * addresses are read directly. RAW_LV_EDMAC_CHANNEL_ADDR is supplied by
 * consts.h for the M6II RAW writer. */
uint32_t shamem_read(uint32_t addr)
{
    if ((addr >> 28) != 0xD)
    {
        DryosDebugMsg(0, 15, "shamem_read from %08x - aborted", addr);
        return 0;
    }

    return *(uintptr_t*)addr;
}

/* Same compatibility exports used by Bilal's M50 port. SRM is intentionally
 * disabled on M6II because Canon SRM allocation is known to crash this port. */
struct memSuite * srm_malloc_suite(int num_requested_buffers)
{
    return 0;
}

void srm_free_suite(struct memSuite * suite)
{
    return;
}

/* ErrCardForLVApp_handler does not exist on this D8 port; mlv_lite only needs
 * an addressable compatibility symbol. */
void ErrCardForLVApp_handler(void)
{
    return;
}

void _engio_write(uint32_t* reg_list) { return; }


/*
 * M6II DIGIC-8 port of Bilal/a1ex SD UHS post-register-write hook.
 *
 * Canon SetSDClkFrequency() converges at E012BEA4 immediately after
 * E012BA6C writes the selected 11-word D01006xx preset.  The original
 * UHS porting method hooks immediately after Canon sets these registers
 * and replaces them with uhs_vals[].  This diagnostic revision only traces
 * the preset sequence; it performs no register override.
 */
#define M6II_SD_POST_PRESET_HOOK 0xE012BEA4u
#define M6II_SD_DEVICE           0u

static const uint32_t m6ii_bilal_sd_regs[11] =
{
    0xD0100600u, 0xD0100618u, 0xD010061Cu, 0xD0100620u,
    0xD010062Cu, 0xD0100630u, 0xD0100624u, 0xD0100628u,
    0xD0100638u, 0xD0100604u, 0xD010060Cu,
};

#define M6II_BILAL_TRACE_MAX 8
static volatile uint32_t m6ii_bilal_sd_calls = 0;
static volatile uint32_t m6ii_bilal_sd_trace[M6II_BILAL_TRACE_MAX];
static volatile uint32_t m6ii_bilal_sd_div_trace[M6II_BILAL_TRACE_MAX];

static struct patch m6ii_bilal_sd_patch[1];
static uint8_t m6ii_bilal_sd_hook_code[8] __attribute__((aligned(4)));
static int m6ii_bilal_sd_hook_installed = 0;

void m6ii_bilal_sd_set_values(const uint32_t *vals)
{
    for (int i = 0; i < 11; i++)
        m6ii_bilal_sd_vals[i] = vals[i];
}

void m6ii_bilal_sd_reset_stats(void)
{
    m6ii_bilal_sd_calls = 0;
    for (int i = 0; i < M6II_BILAL_TRACE_MAX; i++)
    {
        m6ii_bilal_sd_trace[i] = 0xffffffffu;
        m6ii_bilal_sd_div_trace[i] = 0xffffffffu;
    }
}

void m6ii_bilal_sd_get_trace(uint32_t *calls, uint32_t *trace,
                             uint32_t *div_trace, uint32_t max_entries)
{
    if (calls) *calls = m6ii_bilal_sd_calls;

    uint32_t n = max_entries;
    if (n > M6II_BILAL_TRACE_MAX) n = M6II_BILAL_TRACE_MAX;

    for (uint32_t i = 0; i < n; i++)
    {
        if (trace) trace[i] = m6ii_bilal_sd_trace[i];
        if (div_trace) div_trace[i] = m6ii_bilal_sd_div_trace[i];
    }
}

static void m6ii_bilal_sd_override(uint32_t dev, uint32_t selector)
{
    uint32_t i = m6ii_bilal_sd_calls++;

    if (i < M6II_BILAL_TRACE_MAX)
    {
        m6ii_bilal_sd_trace[i] = ((dev & 0xffffu) << 16) | (selector & 0xffffu);
        m6ii_bilal_sd_div_trace[i] = MEM(0xD0100604u);
    }

    /*
     * Trace-only build: do not change Canon's controller state.
     * This is mapping the D8 equivalent of Bilal's setup-mode stage.
     */
}

static void __attribute__((noinline,naked,aligned(4)))
m6ii_bilal_sd_trampoline(void)
{
    asm volatile(
        "push {r0-r12, lr}\n"
        "mov  r0, r5\n"
        "mov  r1, r4\n"
        "mov  r3, %0\n"
        "blx  r3\n"
        "pop  {r0-r12, lr}\n"

        /* replay displaced E012BEA4..E012BEAB */
        "movs r1, #5\n"
        "mov  r0, r5\n"
        "movw r3, #0xDC09\n" /* E043DC08 | Thumb bit */
        "movt r3, #0xE043\n"
        "blx  r3\n"

        /* resume at E012BEAC */
        "movw r3, #0xBEAD\n"
        "movt r3, #0xE012\n"
        "bx   r3\n"
        :
        : "r"(m6ii_bilal_sd_override)
        : "r3"
    );
}

int m6ii_bilal_sd_install_hook(void)
{
    if (m6ii_bilal_sd_hook_installed)
        return E_PATCH_OK;

    struct function_hook_patch def =
    {
        .patch_addr = M6II_SD_POST_PRESET_HOOK,
        .target_function_addr = (uint32_t)m6ii_bilal_sd_trampoline,
        .description = "sd_clock: M6II Bilal post-preset override",
    };

    static const uint8_t expected[8] =
    {
        0x05, 0x21, 0x28, 0x46, 0x11, 0xF3, 0xAE, 0xFE
    };

    for (int i = 0; i < 8; i++)
        def.orig_content[i] = expected[i];

    int err = convert_f_patch_to_patch(&def,
                                      &m6ii_bilal_sd_patch[0],
                                      &m6ii_bilal_sd_hook_code[0]);
    if (err != E_PATCH_OK)
        return err;

    err = apply_patches(m6ii_bilal_sd_patch, 1);
    if (err == E_PATCH_OK)
        m6ii_bilal_sd_hook_installed = 1;

    return err;
}

int m6ii_bilal_sd_remove_hook(void)
{
    if (!m6ii_bilal_sd_hook_installed)
        return E_PATCH_OK;

    int err = unpatch_memory(M6II_SD_POST_PRESET_HOOK);
    if (err == E_UNPATCH_OK)
        m6ii_bilal_sd_hook_installed = 0;

    return err;
}
