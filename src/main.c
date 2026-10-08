// MemBench — what does a memory access cost, and who wins the RDRAM bus?
//
// Kaze's challenge (N64brew, 2026-10-07): nobody has published accurate RDRAM
// timings or the arbitration between the CPU, the RSP/RDP DMAs and the VI, and
// no emulator models them. This ROM measures them on a console, one question
// per sweep, and prints every cell as a CSV line (prefix "MB,") over ISViewer
// and USB, so `tools/flash.sh -d` captures the whole table.
//
// Sweep 1 (this file): the CPU's cost per access for a few access patterns,
// under three loads: the VI off, the VI scanning out 320x240x16, and the RDP
// filling RDRAM flat out beside the VI. The RDP column also reports how much
// the fill pass itself slowed down, so the arbitration is seen from both sides.
//
// Timing is the CPU COUNT register (TICKS_READ, CPU clock / 2, 21.3 ns per
// tick), so every cell makes thousands of accesses and prints the raw tick
// count beside the derived figure. Interrupts are off inside a window. Code
// runs from RDRAM and nothing touches the PI bus (no debugf) inside a window,
// so the flashcart is out of the loop.

#include <libdragon.h>
#include <malloc.h>

#define BUF_BYTES   (256 * 1024)    // 32x the 8 KiB data cache
#define RUNS        5               // keep the fastest and the mean
#define SCREEN_W    320
#define SCREEN_H    240
#define RDP_TARGET_US 80000         // the fill pass aims to outlast any CPU window
#define RDP_MAX_LAYERS 240          // DP_PIPE_BUSY is 24 bits: 268 ms max

enum { L_VIOFF, L_IDLE, L_RDP, N_LOAD };
static const char *LOAD_NAME[] = { "vioff", "idle", "rdpfill" };

typedef struct {
    const char *name;
    void (*prep)(void);     // untimed set-up (cache state)
    int  (*run)(void);      // timed; returns the number of accesses made
    int   bytes;            // bytes moved per access
} pattern_t;

typedef struct {
    int      n;                 // accesses per run
    uint32_t ticks_min, ticks_sum;
    float    rdp_us;            // fill pass duration while the CPU hammered (min)
    int      rdp_started, rdp_busy_after;   // out of RUNS
} cell_t;

static uint8_t           *buf;      // cached alias
static volatile uint32_t *u32;      // uncached aliases
static volatile uint64_t *u64;
static volatile uint32_t  sink;

// ------------------------------------------------------------------ patterns

#define N32 (BUF_BYTES / 4)
#define N64_ (BUF_BYTES / 8)

static void prep_none(void) {}
static void prep_flush(void) { data_cache_hit_writeback_invalidate(buf, BUF_BYTES); }

// Uncached 32-bit reads, sequential.
static int run_u32r(void) {
    uint32_t s = 0;
    for (int i = 0; i < N32; i += 8)
        s += u32[i] + u32[i+1] + u32[i+2] + u32[i+3] + u32[i+4] + u32[i+5] + u32[i+6] + u32[i+7];
    sink = s;
    return N32;
}
// Uncached 64-bit reads, sequential.
static int run_u64r(void) {
    uint64_t s = 0;
    for (int i = 0; i < N64_; i += 8)
        s += u64[i] + u64[i+1] + u64[i+2] + u64[i+3] + u64[i+4] + u64[i+5] + u64[i+6] + u64[i+7];
    sink = (uint32_t)s;
    return N64_;
}
// Uncached 32-bit writes, sequential.
static int run_u32w(void) {
    for (int i = 0; i < N32; i += 8) {
        u32[i] = i; u32[i+1] = i; u32[i+2] = i; u32[i+3] = i;
        u32[i+4] = i; u32[i+5] = i; u32[i+6] = i; u32[i+7] = i;
    }
    return N32;
}
// Uncached 64-bit writes, sequential.
static int run_u64w(void) {
    for (int i = 0; i < N64_; i += 8) {
        u64[i] = i; u64[i+1] = i; u64[i+2] = i; u64[i+3] = i;
        u64[i+4] = i; u64[i+5] = i; u64[i+6] = i; u64[i+7] = i;
    }
    return N64_;
}
// Uncached 32-bit reads, every word once, consecutive accesses 64 B apart.
static int run_u32r_s64(void) {
    uint32_t s = 0;
    for (int k = 0; k < 16; k++)
        for (int i = k; i < N32; i += 16) s += u32[i];
    sink = s;
    return N32;
}
// Same, consecutive accesses 4 KiB apart (a different RDRAM row each time, if
// rows are what they are believed to be).
static int run_u32r_s4k(void) {
    uint32_t s = 0;
    for (int k = 0; k < 1024; k++)
        for (int i = k; i < N32; i += 1024) s += u32[i];
    sink = s;
    return N32;
}
// Cached reads, one word per 16 B line after invalidating: pure line fills.
static int run_cr16(void) {
    volatile uint32_t *c = (volatile uint32_t *)buf;
    uint32_t s = 0;
    for (int i = 0; i < N32; i += 4) s += c[i];
    sink = s;
    return N32 / 4;
}
// Cached writes, one word per line after invalidating: a line fill (the
// VR4300 write-allocates) and, once evicted, a write-back. The trailing
// write-back of the last 8 KiB is forced inside the window so every line
// pays both.
static int run_cw16(void) {
    volatile uint32_t *c = (volatile uint32_t *)buf;
    for (int i = 0; i < N32; i += 4) c[i] = i;
    data_cache_hit_writeback_invalidate(buf, BUF_BYTES);
    return N32 / 4;
}

static const pattern_t PATTERNS[] = {
    { "u32r",     prep_none,  run_u32r,     4  },
    { "u64r",     prep_none,  run_u64r,     8  },
    { "u32w",     prep_none,  run_u32w,     4  },
    { "u64w",     prep_none,  run_u64w,     8  },
    { "u32r_s64", prep_none,  run_u32r_s64, 4  },
    { "u32r_s4k", prep_none,  run_u32r_s4k, 4  },
    { "cr16",     prep_flush, run_cr16,     16 },
    { "cw16",     prep_flush, run_cw16,     32 },
};
enum { N_PAT = sizeof PATTERNS / sizeof PATTERNS[0] };

static cell_t results[N_PAT][N_LOAD];
static float  rdp_alone_us[N_LOAD];     // the fill pass with the CPU quiet, per VI state
static int    rdp_layers = 8;
static bool   has_counters;

// ------------------------------------------------------------------ RDP load

static surface_t     scratch;
static rspq_block_t *fill_blk;

static inline void reset_counters(void) {
    *DP_STATUS = DP_WSTATUS_RESET_PIPE_COUNTER | DP_WSTATUS_RESET_TMEM_COUNTER |
                 DP_WSTATUS_RESET_CMD_COUNTER  | DP_WSTATUS_RESET_CLOCK_COUNTER;
}
static inline float pipe_us(void) { return (*DP_PIPE_BUSY & 0xFFFFFF) / 62.5f; }

// `layers` full-screen fill-mode rectangles into a scratch buffer: the most
// RDRAM-hungry thing the RDP does (8 bytes a clock, no texture, no Z).
static void make_fill_block(int layers) {
    if (fill_blk) rspq_block_free(fill_blk);
    rspq_block_begin();
    rdpq_set_color_image(&scratch);
    rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);
    rdpq_set_mode_fill(RGBA32(32, 64, 96, 255));
    for (int i = 0; i < layers; i++) rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H);
    fill_blk = rspq_block_end();
}

static float time_fill_alone(void) {
    float best = 1e9f;
    for (int r = 0; r < RUNS; r++) {
        rspq_wait();
        reset_counters();
        rspq_block_run(fill_blk);
        rspq_wait();
        float us = pipe_us();
        if (us < best) best = us;
    }
    return best;
}

// Size the pass so it lasts about RDP_TARGET_US; the CPU windows are shorter.
static void calibrate_rdp(void) {
    make_fill_block(8);
    float us8 = time_fill_alone();
    has_counters = us8 > 0;
    int layers = has_counters ? (int)(8.0f * RDP_TARGET_US / us8) : 64;
    if (layers < 8) layers = 8;
    if (layers > RDP_MAX_LAYERS) layers = RDP_MAX_LAYERS;
    rdp_layers = layers;
    make_fill_block(layers);
    debugf("MB,# fill pass: 8 layers %.1f us -> %d layers (%.1f us per 320x240x16 layer, %.0f MB/s)\n",
           us8, layers, us8 / 8, has_counters ? SCREEN_W * SCREEN_H * 2 * 8 / us8 : 0.0f);
}

// ------------------------------------------------------------------ measure

static void measure(cell_t *c, const pattern_t *p, int load) {
    memset(c, 0, sizeof *c);
    c->ticks_min = 0xFFFFFFFF;
    c->rdp_us = 1e9f;
    for (int r = 0; r < RUNS; r++) {
        rspq_wait();                        // nothing of ours in flight
        p->prep();
        bool started = false;
        if (load == L_RDP) {
            reset_counters();
            rspq_block_run(fill_blk);
            rspq_flush();
            uint32_t t = TICKS_READ();      // wait for the pipe to actually run
            while (!(*DP_STATUS & DP_STATUS_PIPE_BUSY) && TICKS_DISTANCE(t, TICKS_READ()) < TICKS_FROM_MS(50)) {}
            started = *DP_STATUS & DP_STATUS_PIPE_BUSY;
        }
        disable_interrupts();
        uint32_t t0 = TICKS_READ();
        int n = p->run();
        uint32_t t1 = TICKS_READ();
        bool busy_after = *DP_STATUS & DP_STATUS_PIPE_BUSY;
        enable_interrupts();
        if (load == L_RDP) {
            rspq_wait();
            float us = pipe_us();
            if (us < c->rdp_us) c->rdp_us = us;
            c->rdp_started    += started;
            c->rdp_busy_after += busy_after;
        }
        uint32_t ticks = TICKS_DISTANCE(t0, t1);
        c->n = n;
        c->ticks_sum += ticks;
        if (ticks < c->ticks_min) c->ticks_min = ticks;
    }
    if (load != L_RDP) c->rdp_us = 0;
}

static float ns_per(uint32_t ticks, int n) { return ticks * (1e9f / TICKS_PER_SECOND) / n; }

static void print_cell(const cell_t *c, const pattern_t *p, int load) {
    float ns_min  = ns_per(c->ticks_min, c->n);
    float ns_mean = ns_per(c->ticks_sum / RUNS, c->n);
    debugf("MB,%s,%s,%d,%d,%lu,%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d\n",
           p->name, LOAD_NAME[load], c->n, p->bytes,
           (unsigned long)c->ticks_min, (unsigned long)(c->ticks_sum / RUNS),
           ns_min, ns_mean, p->bytes * 1000.0f / ns_min,
           load == L_RDP ? rdp_alone_us[L_IDLE] : 0.0f, c->rdp_us,
           c->rdp_started, c->rdp_busy_after);
}

static int font_id = 1;

static void progress(const char *what, int done, int total) {
    surface_t *fb = display_get();
    rdpq_attach_clear(fb, NULL);
    rdpq_set_mode_standard();
    rdpq_text_printf(NULL, font_id, 16, 110, "MemBench: %s %d/%d", what, done, total);
    rdpq_detach_show();
}

static void sweep_load(int load, bool show_progress) {
    for (int p = 0; p < N_PAT; p++) {
        if (show_progress) progress(LOAD_NAME[load], p, N_PAT);
        measure(&results[p][load], &PATTERNS[p], load);
        print_cell(&results[p][load], &PATTERNS[p], load);
    }
}

// ------------------------------------------------------------------ results

static void show(int metric) {
    surface_t *fb = display_get();
    rdpq_attach_clear(fb, NULL);
    rdpq_set_mode_standard();
    int y = 20;
    rdpq_text_printf(NULL, font_id, 8, y, "MemBench sweep 1  %s", metric ? "MB/s (fastest run)" : "ns per access (fastest run)");
    y += 14;
    rdpq_text_printf(NULL, font_id, 8, y, "pattern   bytes   vioff    idle  rdpfill");
    y += 12;
    for (int p = 0; p < N_PAT; p++) {
        char line[96];
        int n = snprintf(line, sizeof line, "%-9s %3d ", PATTERNS[p].name, PATTERNS[p].bytes);
        for (int l = 0; l < N_LOAD; l++) {
            const cell_t *c = &results[p][l];
            float ns = ns_per(c->ticks_min, c->n);
            float v = metric ? PATTERNS[p].bytes * 1000.0f / ns : ns;
            n += snprintf(line + n, sizeof line - n, v < 1000 ? "%8.1f" : "%8.0f", v);
        }
        rdpq_text_printf(NULL, font_id, 8, y, "%s", line);
        y += 12;
    }
    y += 6;
    rdpq_text_printf(NULL, font_id, 8, y, "fill pass alone: VI off %.0f us, VI on %.0f us", rdp_alone_us[L_VIOFF], rdp_alone_us[L_IDLE]);
    y += 12;
    rdpq_text_printf(NULL, font_id, 8, y, "fill pass with CPU: u32r %.0f  cr16 %.0f us",
                     results[0][L_RDP].rdp_us, results[N_PAT - 2][L_RDP].rdp_us);
    y += 12;
    if (!has_counters)
        rdpq_text_printf(NULL, font_id, 8, y, "RDP counters read 0 (emulator?)"), y += 12;
    y += 6;
    rdpq_text_printf(NULL, font_id, 8, y, "B: ns or MB/s   Start: rerun");
    rdpq_detach_show();
}

static void run_all(bool first) {
    // VI off: the one moment the CPU has RDRAM to itself. Before the display
    // exists on the first pass; afterwards the TV loses sync for a second.
    if (!first) { rspq_wait(); display_close(); }
    *(volatile uint32_t *)0xA4400000 = 0;       // VI_CTRL type 0: no fetch, no sync
    wait_ms(50);
    rdp_alone_us[L_VIOFF] = time_fill_alone();
    sweep_load(L_VIOFF, false);

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    wait_ms(50);
    rdp_alone_us[L_IDLE] = time_fill_alone();
    debugf("MB,# fill pass alone (%d layers): VI off %.1f us, VI on %.1f us\n", rdp_layers, rdp_alone_us[L_VIOFF], rdp_alone_us[L_IDLE]);
    sweep_load(L_IDLE, true);
    sweep_load(L_RDP, true);
    debugf("MB,# done%s\n", has_counters ? "" : " -- RDP counters read 0: emulator, timings are not the console's");
}

int main(void) {
    debug_init_isviewer();
    debug_init_usblog();
    rdpq_init();
    joypad_init();
    rdpq_text_register_font(font_id, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));

    buf = memalign(64, BUF_BYTES);
    assertf(buf, "no room for the %d KiB buffer", BUF_BYTES / 1024);
    memset(buf, 0, BUF_BYTES);
    data_cache_hit_writeback_invalidate(buf, BUF_BYTES);
    u32 = UncachedAddr(buf);
    u64 = UncachedAddr(buf);
    scratch = surface_alloc(FMT_RGBA16, SCREEN_W, SCREEN_H);

    debugf("MB,# MemBench sweep 1: tv=%s mem=%d MiB ticks/s=%lu buf=%d KiB runs=%d\n",
           get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL",
           get_memory_size() >> 20, (unsigned long)TICKS_PER_SECOND, BUF_BYTES / 1024, RUNS);
    calibrate_rdp();
    debugf("MB,pattern,load,accesses,bytes_per,ticks_min,ticks_mean,ns_min,ns_mean,MBps_min,rdp_alone_us,rdp_us,rdp_started,rdp_busy_after\n");
    run_all(true);

    int metric = 0;
    while (1) {
        show(metric);
        joypad_poll();
        joypad_buttons_t b = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if (b.b) metric ^= 1;
        if (b.start) run_all(false);
    }
}
