/** \file
 * EOS M6 Mark II one-frame native RAW EDMAC redirect test.
 *
 * ROM-derived M6II 1.1.1 RAW path:
 *   Engine::ShtRawAutoPath programs RAW destination EDMAC channel 0x4B.
 *   Channel 0x4B maps to MMIO block 0xD04C0300 (ram_addr +0xA0), but this
 *   test deliberately does NOT poke MMIO.  Instead it hooks Canon's exact
 *   edmac_set_address call at 0xE0646074 and substitutes one destination
 *   argument.  The following Canon frame submission automatically restores
 *   the original destination, which provides a real frame-boundary completion
 *   marker without sleeps or a guessed VSYNC hook.
 */

#ifndef CONFIG_HELLO_WORLD

#include <dryos.h>
#include <bmp.h>
#include <menu.h>
#include <timer.h>
#include <propvalues.h>
#include <patch.h>

#define M6II_RAW_STATE_BASE          0x00010970u
#define M6II_RAW_ENGINE_STATE        0x0002909Cu
#define M6II_RAW_WIDTH_EXPECTED      3568u
#define M6II_RAW_HEIGHT_EXPECTED     2000u
#define M6II_RAW_FRAME_BYTES         ((M6II_RAW_WIDTH_EXPECTED * M6II_RAW_HEIGHT_EXPECTED * 7u) / 4u)
#define M6II_RAW_GUARD_BYTES         4096u
#define M6II_RAW_ALLOC_BYTES         (M6II_RAW_FRAME_BYTES + M6II_RAW_GUARD_BYTES)
#define M6II_FILL                    0xA5u
#define M6II_LOG                     "M6II_RAW_REDIRECT.TXT"

#define M6II_RAW_ADDR_PATCH          0xE0646074u
#define M6II_RAW_ADDR_CONTINUE       0xE064607Du
#define M6II_EDMAC_SET_ADDRESS       0xE0580963u
#define M6II_RAW_DST_CHANNEL         0x4Bu
#define M6II_RAW_DST_MMIO            0xD04C0300u
#define M6II_RAW_DST_RAM_ADDR_REG    0xD04C03A0u

extern void *_alloc_dma_memory(size_t size);

static volatile int m6ii_redirect_busy = 0;
static volatile uint32_t m6ii_redirect_phase = 0;
static volatile uint32_t m6ii_redirect_hits = 0;
static volatile uint32_t m6ii_redirect_dst = 0;
static volatile uint32_t m6ii_first_canon_addr = 0;
static volatile uint32_t m6ii_restore_canon_addr = 0;

static struct patch m6ii_redirect_patch;
static uint8_t m6ii_redirect_hook_mem[8];

static uint32_t m6ii_state32(uint32_t off)
{
    return *(volatile uint32_t *)(M6II_RAW_STATE_BASE + off);
}

/* Referenced by name from the naked assembly hook, so this symbol must have
 * external linkage and must not be discarded by the compiler/linker. */
__attribute__((noinline,used)) uint32_t m6ii_raw_choose_addr(uint32_t canon_addr)
{
    uint32_t phase = m6ii_redirect_phase;

    if (phase == 1)
    {
        m6ii_first_canon_addr = canon_addr;
        m6ii_redirect_hits++;
        m6ii_redirect_phase = 2;
        return m6ii_redirect_dst;
    }

    if (phase == 2)
    {
        m6ii_restore_canon_addr = canon_addr;
        m6ii_redirect_hits++;
        m6ii_redirect_phase = 3;
        return canon_addr;
    }

    return canon_addr;
}

static void __attribute__((naked,noinline)) m6ii_raw_addr_hook(void)
{
    __asm__ volatile(
        "push {r2, r3, r6, lr}\n"
        "ldr  r1, [r4]\n"
        "mov  r0, r1\n"
        "bl   m6ii_raw_choose_addr\n"
        "mov  r1, r0\n"
        "ldr  r0, [r5, #0x50]\n"
        "ldr  r3, =0xE0580963\n"
        "blx  r3\n"
        "pop  {r2, r3, r6, lr}\n"
        "ldr  r3, =0xE064607D\n"
        "bx   r3\n"
    );
}

static uint32_t changed_bytes(const volatile uint8_t *p, uint32_t n)
{
    uint32_t changed = 0;
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != M6II_FILL)
            changed++;
    return changed;
}

static uint32_t checksum32(const volatile uint32_t *p, uint32_t bytes)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < bytes / 4u; i++)
        sum = (sum << 5) - sum + p[i];
    return sum;
}

static uint32_t guard_mismatches(const volatile uint8_t *p, uint32_t n)
{
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != M6II_FILL)
            mismatches++;
    return mismatches;
}

static int install_raw_addr_hook(void)
{
    struct function_hook_patch fp = {
        .patch_addr = M6II_RAW_ADDR_PATCH,
        .orig_content = { 0x01, 0x68, 0x28, 0x6d, 0x3a, 0xf7, 0x73, 0xfc },
        .target_function_addr = (uint32_t)m6ii_raw_addr_hook,
        .description = "M6II one-frame RAW EDMAC destination redirect",
    };

    memset(&m6ii_redirect_patch, 0, sizeof(m6ii_redirect_patch));
    memset(m6ii_redirect_hook_mem, 0, sizeof(m6ii_redirect_hook_mem));

    int ret = convert_f_patch_to_patch(&fp, &m6ii_redirect_patch, m6ii_redirect_hook_mem);
    if (ret)
        return ret;

    return apply_patches(&m6ii_redirect_patch, 1);
}

static void m6ii_raw_redirect_test(void)
{
    if (m6ii_redirect_busy)
    {
        NotifyBox(2000, "RAW redirect already running");
        return;
    }
    if (!LV_NON_PAUSED)
    {
        NotifyBox(3000, "RAW redirect needs active LiveView");
        return;
    }
    if (RECORDING)
    {
        NotifyBox(3000, "Stop Canon recording first");
        return;
    }

    m6ii_redirect_busy = 1;

    volatile uint8_t *dst = 0;
    int stage = 0;
    int raw_enabled = 0;
    int hook_installed = 0;
    int ret_mm = 0;
    int ret_on = 0;
    int ret_off = 0;
    int patch_ret = 0xffffffffu;
    int unpatch_ret = 0xffffffffu;
    uint32_t width = 0, height = 0, raw_state_ptr = 0;
    uint32_t engine_channel = 0;
    uint32_t wait_ms = 0;
    uint32_t changed = 0;
    uint32_t checksum = 0;
    uint32_t guard_bad = 0;
    uint32_t first_words[8] = {0};
    uint32_t last_words[8] = {0};

    dst = (volatile uint8_t *)_alloc_dma_memory(M6II_RAW_ALLOC_BYTES);
    if (!dst)
    {
        stage = -1;
        goto cleanup;
    }
    memset((void *)dst, M6II_FILL, M6II_RAW_ALLOC_BYTES);
    m6ii_redirect_dst = (uint32_t)dst;
    stage = 1;

    m6ii_redirect_phase = 0;
    m6ii_redirect_hits = 0;
    m6ii_first_canon_addr = 0;
    m6ii_restore_canon_addr = 0;
    patch_ret = install_raw_addr_hook();
    if (patch_ret)
    {
        stage = -2;
        goto cleanup;
    }
    hook_installed = 1;
    stage = 2;

    ret_mm = call("lv_set_mm", 1);
    ret_on = call("lv_save_raw", 1);
    raw_enabled = 1;

    for (wait_ms = 0; wait_ms < 1000; wait_ms++)
    {
        width = m6ii_state32(0x10);
        height = m6ii_state32(0x14);
        raw_state_ptr = m6ii_state32(0x58);
        engine_channel = *(volatile uint32_t *)(M6II_RAW_ENGINE_STATE + 0x50u);
        if (width == M6II_RAW_WIDTH_EXPECTED &&
            height == M6II_RAW_HEIGHT_EXPECTED &&
            raw_state_ptr && engine_channel == M6II_RAW_DST_CHANNEL)
            break;
        msleep(1);
    }

    if (width != M6II_RAW_WIDTH_EXPECTED ||
        height != M6II_RAW_HEIGHT_EXPECTED ||
        !raw_state_ptr || engine_channel != M6II_RAW_DST_CHANNEL)
    {
        stage = -3;
        goto cleanup;
    }
    stage = 3;

    m6ii_redirect_phase = 1;

    for (wait_ms = 0; wait_ms < 1500; wait_ms++)
    {
        if (m6ii_redirect_phase == 3)
            break;
        msleep(1);
    }

    if (m6ii_redirect_phase != 3 || m6ii_redirect_hits != 2)
    {
        stage = -4;
        goto cleanup;
    }
    stage = 4;

    unpatch_ret = unpatch_memory(M6II_RAW_ADDR_PATCH);
    hook_installed = 0;
    if (unpatch_ret)
    {
        stage = -5;
        goto cleanup;
    }
    stage = 5;

    ret_off = call("lv_save_raw", 0);
    raw_enabled = 0;
    stage = 6;

    changed = changed_bytes(dst, M6II_RAW_FRAME_BYTES);
    checksum = checksum32((const volatile uint32_t *)dst, M6II_RAW_FRAME_BYTES);
    guard_bad = guard_mismatches(dst + M6II_RAW_FRAME_BYTES, M6II_RAW_GUARD_BYTES);

    for (int i = 0; i < 8; i++)
        first_words[i] = ((volatile uint32_t *)dst)[i];
    volatile uint32_t *tail = (volatile uint32_t *)(dst + M6II_RAW_FRAME_BYTES - 32u);
    for (int i = 0; i < 8; i++)
        last_words[i] = tail[i];
    stage = 7;

cleanup:
    m6ii_redirect_phase = 0;

    if (hook_installed)
    {
        unpatch_ret = unpatch_memory(M6II_RAW_ADDR_PATCH);
        hook_installed = 0;
    }
    if (raw_enabled)
    {
        ret_off = call("lv_save_raw", 0);
        raw_enabled = 0;
    }

    FILE *f = FIO_CreateFile(M6II_LOG);
    if (f)
    {
        char text[2300];
        int len = snprintf(text, sizeof(text),
            "M6II native RAW EDMAC one-frame redirect v8\n"
            "raw_dst_channel=0x%08x\nraw_dst_mmio=0x%08x\nram_addr_reg=0x%08x\n"
            "patch_addr=0x%08x\ncontinue_addr=0x%08x\n"
            "width=0x%08x\nheight=0x%08x\nframe_bytes=0x%08x\n"
            "raw_state_ptr=0x%08x\nengine_channel=0x%08x\n"
            "ml_dst=0x%08x\nfirst_canon_addr=0x%08x\nrestore_canon_addr=0x%08x\n"
            "redirect_hits=0x%08x\nredirect_phase=0x%08x\n"
            "stage=0x%08x\npatch_ret=0x%08x\nunpatch_ret=0x%08x\n"
            "ret_mm=0x%08x\nret_raw_on=0x%08x\nret_raw_off=0x%08x\n"
            "changed_frame=0x%08x\nchecksum_frame=0x%08x\nguard_mismatches=0x%08x\n"
            "dst_kept_until_reboot=0x00000001\n"
            "first=%08x %08x %08x %08x %08x %08x %08x %08x\n"
            "last=%08x %08x %08x %08x %08x %08x %08x %08x\n",
            M6II_RAW_DST_CHANNEL, M6II_RAW_DST_MMIO, M6II_RAW_DST_RAM_ADDR_REG,
            M6II_RAW_ADDR_PATCH, M6II_RAW_ADDR_CONTINUE,
            width, height, M6II_RAW_FRAME_BYTES,
            raw_state_ptr, engine_channel,
            (uint32_t)dst, (uint32_t)m6ii_first_canon_addr, (uint32_t)m6ii_restore_canon_addr,
            (uint32_t)m6ii_redirect_hits, (uint32_t)m6ii_redirect_phase,
            (uint32_t)stage, (uint32_t)patch_ret, (uint32_t)unpatch_ret,
            (uint32_t)ret_mm, (uint32_t)ret_on, (uint32_t)ret_off,
            changed, checksum, guard_bad,
            first_words[0], first_words[1], first_words[2], first_words[3],
            first_words[4], first_words[5], first_words[6], first_words[7],
            last_words[0], last_words[1], last_words[2], last_words[3],
            last_words[4], last_words[5], last_words[6], last_words[7]);
        FIO_WriteFile(f, text, len);
        FIO_CloseFile(f);
    }

    m6ii_redirect_busy = 0;

    if (stage == 7 && changed != 0 && guard_bad == 0)
        NotifyBox(6000, "RAW WRITER REDIRECT PASS: changed=0x%08x", changed);
    else
        NotifyBox(6000, "RAW redirect result stage=0x%08x guard=0x%08x", (uint32_t)stage, guard_bad);
}

static struct menu_entry m6ii_redirect_menu[] = {
    {
        .name = "TEST RAW writer redirect v8",
        .priv = m6ii_raw_redirect_test,
        .select = run_in_separate_task,
        .help = "Redirect exactly one native packed RAW frame from Canon EDMAC channel 0x4B into ML DMA RAM."
    },
};

static void m6ii_redirect_init()
{
    menu_add("Debug", m6ii_redirect_menu, COUNT(m6ii_redirect_menu));
    DryosDebugMsg(0, 15, "M6II native RAW writer redirect v8 registered");
}

INIT_FUNC(__FILE__, m6ii_redirect_init);

#endif /* !CONFIG_HELLO_WORLD */
