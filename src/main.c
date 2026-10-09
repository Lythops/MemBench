// MemBench — what does a memory access cost, and who wins the RDRAM bus?
//
// Kaze's challenge (N64brew, 2026-10-07): nobody has published accurate RDRAM
// timings or the arbitration between the CPU, the RSP/RDP DMAs and the VI, and
// no emulator models them. This ROM measures them on a console, one question
// per sweep, and prints every cell as a CSV line over ISViewer and USB, so
// `tools/flash.sh -d` captures the whole table.
//
// Sweep 1 ("MB," lines): the CPU's cost per access for a few access patterns,
// under three loads: the VI off, the VI scanning out 320x240x16, and the RDP
// filling RDRAM flat out beside the VI. The RDP column also reports how much
// the fill pass itself slowed down, so the arbitration is seen from both sides.
//
// Sweep 2 ("M2," lines): the DMA clients. The RSP's DMA engine running a loop
// ucode (RDRAM->DMEM and DMEM->RDRAM, 64 B to 2 KiB per DMA) and the PI DMAing
// a 1 MiB block from the cart, alone and against the CPU, the RDP and each
// other. Every cell reports every running client's figure during one window.
//
// The RDP is fed straight from RDRAM (DP_START/DP_END, no RSP in the path) so
// the RSP is free for the DMA ucode, and rspq is only started at the very end
// for the results screen. Timing is the CPU COUNT register (TICKS_READ, CPU
// clock / 2, 21.3 ns per tick), so every cell makes thousands of accesses and
// prints the raw tick count beside the derived figure. Interrupts are off
// inside a CPU window. Code runs from RDRAM and nothing touches the PI bus
// (no debugf) inside a window, except the PI DMA cells, which are the point.

#include <libdragon.h>
#include <malloc.h>

#define BUF_BYTES     (256 * 1024)  // CPU buffer: 32x the 8 KiB data cache
#define SP_BUF_BYTES  (64 * 1024)   // RSP DMA region
#define PI_BYTES      (1024 * 1024) // one PI DMA pass
#define ARENA_BYTES   (4 * 1024 * 1024 + 256 * 1024) // 1 MiB aligned; everything placed inside
#define CPU_OFF       (1024 * 1024) // the CPU buffer: a 1 MiB block with room below and above
#define SCRATCH_OFF   (2048 * 1024) // default RDP color target: a different 1 MiB block
#define ZBUF_OFF      (SCRATCH_OFF + 160 * 1024)   // the RDP's Z-buffer, same block as its color target
#define TEX_OFF       (SCRATCH_OFF + 320 * 1024)   // the RDP's texture source (64x32 RGBA16), same block
#define SP_OFF        (3072 * 1024) // default RSP DMA region: a third block
#define PI_OFF        (3328 * 1024) // the PI's destination, 1 MiB at the top of the arena
#define RDP_CMD_QWORDS (32 * 1024)  // 256 KiB of RDP commands: enough for ~16k LOAD_TILEs

// What the RDP does in a pass. The first three are sweep 7's; the rest are
// texture-load variants for sweep 8, each moving 4 KiB per unit.
enum { K_FILL, K_LOAD, K_RECTZ, K_LOADBLK, K_LOAD32, K_LOADI8, K_LOADNARROW, K_LOADWIDE,
       K_LOAD48, K_LOAD64R, K_LOAD96, N_KIND };
static const char *KIND_NAME[] = { "fill", "load", "rectz", "loadblk", "load32", "loadi8", "loadnarrow", "loadwide",
                                   "load48", "load64r", "load96" };

// bytes: what one unit moves (rows x row bytes); the knee kinds are not exactly 4 KiB.
typedef struct { uint64_t fmtsiz; int line_qw; int w, h; bool block; int bytes; } loadvar_t;
#define SIZ(fmt, siz) (((uint64_t)(fmt) << 53) | ((uint64_t)(siz) << 51))
static const loadvar_t LOADVAR[N_KIND] = {
    [K_LOAD]       = { SIZ(0, 2), 16, 64,   32, false, 4096 },    // RGBA16 64x32, 128 B rows: the baseline
    [K_LOADBLK]    = { SIZ(0, 2), 16, 2048, 1,  true,  4096 },    // the same 4 KiB as one LOAD_BLOCK
    [K_LOAD32]     = { SIZ(0, 3), 16, 32,   32, false, 4096 },    // RGBA32 32x32
    [K_LOADI8]     = { SIZ(4, 1), 8,  64,   64, false, 4096 },    // I8 64x64
    [K_LOADNARROW] = { SIZ(0, 2), 4,  16,   128, false, 4096 },   // RGBA16 16x128: 128 rows of 32 B
    [K_LOADWIDE]   = { SIZ(0, 2), 64, 256,  8,  false, 4096 },    // RGBA16 256x8: 8 rows of 512 B
    // sweep 9: the knee between 32 B rows (latency-bound) and 128 B rows (pipelined)
    [K_LOAD48]     = { SIZ(0, 2), 6,  24,   85, false, 4080 },    // RGBA16 24x85: 85 rows of 48 B
    [K_LOAD64R]    = { SIZ(0, 2), 8,  32,   64, false, 4096 },    // RGBA16 32x64: 64 rows of 64 B (I8 64x64's rows, 16-bit)
    [K_LOAD96]     = { SIZ(0, 2), 12, 48,   42, false, 4032 },    // RGBA16 48x42: 42 rows of 96 B
};
#define TEX_W 64
#define TEX_H 32
#define TEXEL 0x1234                // the texture source is filled with this RGBA16 value
#define RUNS          5             // keep the fastest and the mean
#define SCREEN_W      320
#define SCREEN_H      240
#define RDP_TARGET_US 80000         // the fill pass aims to outlast any CPU window
#define RDP_MAX_LAYERS 240          // DP_PIPE_BUSY is 24 bits: 268 ms max
#define SPIN_WINDOW_US 25000        // window for cells with no CPU pattern

DEFINE_RSP_UCODE(rsp_dmaloop);

enum { L_VIOFF, L_IDLE, L_RDP, N_LOAD };
static const char *LOAD_NAME[] = { "vioff", "idle", "rdpfill" };

typedef struct {
    const char *name;
    void (*prep)(void);     // untimed set-up (cache state)
    int  (*run)(void);      // timed; returns the number of accesses made
    int   bytes;            // bytes moved per access
} pattern_t;

// One measurement: which clients run, and what each of them reported.
typedef struct {
    const pattern_t *cpu;   // NULL: a fixed spin window instead
    bool rdp;
    int  sp_dir, sp_len;    // sp_len 0: no RSP DMA; dir 0 read, 1 write
    bool pi;
    uint32_t rdp_off, sp_off;   // placement in the arena; 0 = the defaults
    int  rdp_kind;              // K_FILL (0), K_LOAD, K_RECTZ
    bool ai;                    // sweep 9: the AI streaming silence through the window
} cfg_t;

typedef struct {
    int      cpu_n;
    uint32_t cpu_ticks_min, cpu_ticks_sum;
    float    win_us_sum;                    // window length (CPU or spin)
    uint32_t sp_dmas_sum;                   // RSP DMAs issued inside the window
    float    rdp_us_min, rdp_us_sum;        // the RDP pass's DP_PIPE_BUSY time
    float    tmem_us_sum;                   // and its DP_TMEM_BUSY time (texture loads)
    int      rdp_started, rdp_busy_after, rdp_timeout;
    float    pi_us_sum;                     // the PI pass, issue to completion
    int      pi_busy_after;
    uint32_t ai_status_before, ai_status_after;   // AI_STATUS raw (bit 30 busy, bit 31 full), last run
} meas_t;

static uint8_t           *arena;    // 1 MiB aligned; the CPU buffer is its first 256 KiB
static uint8_t           *buf;      // cached alias
static volatile uint32_t *u32;      // uncached aliases
static volatile uint64_t *u64;
static volatile uint32_t  sink;
static uint8_t           *pibuf;    // the PI's destination, outside the arena

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

// Sweep 9: the write-back alone. 8 KiB (the whole D-cache) is made resident
// and dirty by the prep; the run then re-dirties it (all hits) and writes it
// back with CACHE Hit_Writeback, WB_ITERS times. `cdirty8k` does the same
// without the write-back, so the difference is 512 x WB_ITERS pure write-backs.
#define WB_BYTES 8192
#define WB_ITERS 64
static void prep_dirty8k(void) {
    data_cache_hit_writeback_invalidate(buf, BUF_BYTES);
    volatile uint32_t *c = (volatile uint32_t *)buf;
    for (int i = 0; i < WB_BYTES / 4; i += 4) c[i] = i;     // fills, then dirty and resident
}
static int run_cdirty8k(void) {
    volatile uint32_t *c = (volatile uint32_t *)buf;
    for (int it = 0; it < WB_ITERS; it++)
        for (int i = 0; i < WB_BYTES / 4; i += 4) c[i] = i + it;
    return WB_ITERS * (WB_BYTES / 16);
}
static int run_cwb8k(void) {
    volatile uint32_t *c = (volatile uint32_t *)buf;
    for (int it = 0; it < WB_ITERS; it++) {
        for (int i = 0; i < WB_BYTES / 4; i += 4) c[i] = i + it;
        data_cache_hit_writeback(buf, WB_BYTES);               // 512 dirty lines, kept valid
    }
    return WB_ITERS * (WB_BYTES / 16);
}
static const pattern_t PAT_CDIRTY = { "cdirty8k", prep_dirty8k, run_cdirty8k, 16 };
static const pattern_t PAT_CWB    = { "cwb8k",    prep_dirty8k, run_cwb8k,    16 };

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
#define PAT_U64R (&PATTERNS[1])
#define PAT_CR16 (&PATTERNS[6])

static meas_t results[N_PAT][N_LOAD];
static float  rdp_alone_us[N_LOAD];     // the fill pass with the CPU quiet, per VI state
static int    rdp_layers = 8;
static bool   has_counters;

// ------------------------------------------------------------------ RDP, fed from RDRAM

static uint64_t *rdp_cmds;              // uncached
static int       rdp_ncmds;

static inline void reset_counters(void) {
    *DP_STATUS = DP_WSTATUS_RESET_PIPE_COUNTER | DP_WSTATUS_RESET_TMEM_COUNTER |
                 DP_WSTATUS_RESET_CMD_COUNTER  | DP_WSTATUS_RESET_CLOCK_COUNTER;
}
static inline float pipe_us(void) { return (*DP_PIPE_BUSY & 0xFFFFFF) / 62.5f; }
static inline bool rdp_pipe_busy(void) { return *DP_STATUS & DP_STATUS_PIPE_BUSY; }

// `layers` full-screen fill-mode rectangles into a scratch buffer: the most
// RDRAM-hungry thing the RDP does (8 bytes a clock, no texture, no Z). Raw
// RDP commands; in fill mode the lower-right corner is inclusive, so the
// rectangle ends at (W-1, H-1) in 10.2 fixed point.
//
// Three kinds of pass:
//   K_FILL   n full-screen fill-mode rectangles into `target`: pure writes.
//   K_LOAD   n LOAD_TILEs of a 64x32 RGBA16 texture (4 KiB each) from TEX_OFF:
//            pure reads through the TMEM path.
//   K_RECTZ  the Z-buffer cleared to far, then n full-screen 1-cycle textured
//            rectangles with Z compare+write, each nearer than the last so
//            every pixel passes: a colour write, a Z read and a Z write per
//            pixel, which is what a 3D view costs the bus.
#define CMD(id) ((uint64_t)(id) << 56)
#define FMT_RGBA16_SIZ (2ull << 51)
static const uint64_t SET_SCISSOR_FULL = CMD(0x2D) | ((uint64_t)(SCREEN_W * 4) << 12) | (SCREEN_H * 4);
static inline uint64_t set_color_image(uint32_t phys) { return CMD(0x3F) | FMT_RGBA16_SIZ | ((uint64_t)(SCREEN_W - 1) << 32) | phys; }

static void build_rdp_list(int kind, int n, void *target) {
    int k = 0;
    uint32_t addr = PhysicalAddr(target);
    uint32_t zaddr = PhysicalAddr(arena + ZBUF_OFF), taddr = PhysicalAddr(arena + TEX_OFF);
    uint32_t c16  = color_to_packed16(RGBA32(32, 64, 96, 255));
    rdp_cmds[k++] = SET_SCISSOR_FULL;
    if (kind == K_FILL) {
        rdp_cmds[k++] = CMD(0x2F) | (3ull << 52);                                      // SET_OTHER_MODES, cycle type fill
        rdp_cmds[k++] = set_color_image(addr);
        rdp_cmds[k++] = CMD(0x37) | ((uint64_t)c16 << 16) | c16;                       // SET_FILL_COLOR
        for (int i = 0; i < n; i++)                                                     // FILL_RECTANGLE, lower-right inclusive in fill mode
            rdp_cmds[k++] = CMD(0x36) | ((uint64_t)((SCREEN_W - 1) * 4) << 44) | ((uint64_t)((SCREEN_H - 1) * 4) << 32);
    } else {
        // tile 0 at TMEM 0, masks 6/5 so the rect wraps the 64x32 baseline tile
        const loadvar_t *v = &LOADVAR[kind == K_RECTZ ? K_LOAD : kind];
        uint64_t set_tile = CMD(0x35) | v->fmtsiz | ((uint64_t)v->line_qw << 41) | (0ull << 24) | (5ull << 14) | (6ull << 4);
        uint64_t set_tex  = CMD(0x3D) | v->fmtsiz | ((uint64_t)((v->block ? 64 : v->w) - 1) << 32) | taddr;
        uint64_t load     = v->block
            ? CMD(0x33) | ((uint64_t)(v->w - 1) << 12)                                           // LOAD_BLOCK 0..w-1 texels, dxt 0
            : CMD(0x34) | ((uint64_t)((v->w - 1) * 4) << 12) | ((v->h - 1) * 4);                // LOAD_TILE 0,0 .. w-1,h-1
        if (kind != K_RECTZ) {
            rdp_cmds[k++] = CMD(0x2F) | SOM_CYCLE_1;
            rdp_cmds[k++] = set_tex;
            rdp_cmds[k++] = set_tile;
            for (int i = 0; i < n; i++) rdp_cmds[k++] = load;
        } else {
            rdp_cmds[k++] = CMD(0x2F) | (3ull << 52);                                  // clear Z to far, as a fill
            rdp_cmds[k++] = set_color_image(zaddr);
            rdp_cmds[k++] = CMD(0x37) | 0xFFFFFFFFull;
            rdp_cmds[k++] = CMD(0x36) | ((uint64_t)((SCREEN_W - 1) * 4) << 44) | ((uint64_t)((SCREEN_H - 1) * 4) << 32);
            rdp_cmds[k++] = CMD(0x27);                                                  // SYNC_PIPE
            rdp_cmds[k++] = CMD(0x2F) | SOM_CYCLE_1 | SOM_Z_COMPARE | SOM_Z_WRITE | SOM_ZSOURCE_PRIM | SOM_ZMODE_OPAQUE |
                            SOM_SAMPLE_POINT | SOM_TF0_RGB | SOM_RGBDITHER_NONE | SOM_ALPHADITHER_NONE;
            rdp_cmds[k++] = CMD(0x3C) | (uint64_t)RDPQ_COMBINER_TEX;                   // SET_COMBINE: colour = TEX0
            rdp_cmds[k++] = set_color_image(addr);
            rdp_cmds[k++] = CMD(0x3E) | zaddr;                                          // SET_Z_IMAGE
            rdp_cmds[k++] = set_tex;
            rdp_cmds[k++] = set_tile;
            rdp_cmds[k++] = load;
            rdp_cmds[k++] = CMD(0x26);                                                  // SYNC_LOAD
            rdp_cmds[k++] = CMD(0x28);                                                  // SYNC_TILE
            for (int i = 0; i < n; i++) {
                uint32_t z = 0xFFF0 - 16 * i;                                           // nearer every layer: all pixels pass
                rdp_cmds[k++] = CMD(0x2E) | ((uint64_t)z << 16);                        // SET_PRIM_DEPTH
                rdp_cmds[k++] = CMD(0x24) | ((uint64_t)(SCREEN_W * 4) << 44) | ((uint64_t)(SCREEN_H * 4) << 32) | (0ull << 24); // TEXTURE_RECTANGLE 0,0..W,H tile 0
                rdp_cmds[k++] = (0x400ull << 16) | 0x400ull;                            // s=0 t=0 dsdx=1.0 dtdy=1.0
            }
        }
    }
    rdp_cmds[k++] = CMD(0x29);                                                          // SYNC_FULL
    assertf(k <= RDP_CMD_QWORDS, "RDP list overflow: %d qwords", k);
    rdp_ncmds = k;
}

// Bytes a pass moves on the bus, for MB/s.
static float pass_bytes(int kind, int n) {
    switch (kind) {
    case K_FILL:  return (float)n * SCREEN_W * SCREEN_H * 2;
    case K_RECTZ: return (float)n * SCREEN_W * SCREEN_H * 2 * 3 + SCREEN_W * SCREEN_H * 2;  // colour w, Z r, Z w; plus the clear
    default:      return (float)n * LOADVAR[kind].bytes;                                    // the load variants: ~4 KiB a unit
    }
}

static void rdp_start(void) {
    while (*DP_STATUS & (DP_STATUS_START_VALID | DP_STATUS_END_VALID)) {}
    *DP_STATUS = DP_WSTATUS_RESET_XBUS_DMEM_DMA;        // commands come from RDRAM
    uint32_t s = PhysicalAddr(rdp_cmds);
    *DP_START = s;
    *DP_END   = s + rdp_ncmds * 8;
}

// Returns false on timeout (300 ms).
static bool rdp_wait(void) {
    uint32_t t = TICKS_READ();
    while (*DP_STATUS & (DP_STATUS_BUSY | DP_STATUS_PIPE_BUSY | DP_STATUS_START_VALID)) {
        if (TICKS_DISTANCE(t, TICKS_READ()) > TICKS_FROM_MS(300)) return false;
    }
    return true;
}

static float time_fill_alone(void) {
    float best = 1e9f;
    for (int r = 0; r < RUNS; r++) {
        reset_counters();
        rdp_start();
        bool ok = rdp_wait();
        float us = pipe_us();
        if (!ok) debugf("MB,# WARNING: fill pass timed out, DP_STATUS=%08lx\n", *DP_STATUS);
        if (us < best) best = us;
    }
    return best;
}

// Size each kind of pass so it lasts about RDP_TARGET_US; the CPU windows are shorter.
static int rdp_n[N_KIND];
static const int CAL_N[N_KIND]   = { 8, 256, 8, 256, 256, 256, 256, 256, 256, 256, 256 };
static const int MAX_N[N_KIND]   = { RDP_MAX_LAYERS, 16000, RDP_MAX_LAYERS, 16000, 16000, 16000, 16000, 16000, 16000, 16000, 16000 };

static void calibrate_rdp(void) {
    for (int kind = 0; kind < N_KIND; kind++) {
        build_rdp_list(kind, CAL_N[kind], arena + SCRATCH_OFF);
        float us = time_fill_alone();
        if (kind == K_FILL) has_counters = us > 0;
        int n = has_counters ? (int)((float)CAL_N[kind] * RDP_TARGET_US / us) : CAL_N[kind] * 8;
        if (n < CAL_N[kind]) n = CAL_N[kind];
        if (n > MAX_N[kind]) n = MAX_N[kind];
        rdp_n[kind] = n;
        debugf("MB,# %s pass: %d units %.1f us -> %d units (%.2f us per unit, %.0f MB/s)\n", KIND_NAME[kind],
               CAL_N[kind], us, n, us / CAL_N[kind], has_counters ? pass_bytes(kind, CAL_N[kind]) / us : 0.0f);
    }
    // Did the Z-buffered rectangles actually write? Read a pixel back.
    build_rdp_list(K_RECTZ, 2, arena + SCRATCH_OFF);
    time_fill_alone();
    uint16_t px = *(volatile uint16_t *)UncachedAddr(arena + SCRATCH_OFF + 2 * (100 * SCREEN_W + 100));
    debugf("MB,# rectz check: pixel (100,100) reads %04x, texel %04x -> %s\n", px, TEXEL,
           (px & 0xFFFE) == (TEXEL & 0xFFFE) ? "written" : "NOT WRITTEN, rectz rows are read-only Z");
    // Leave the fill list loaded: sweep 1 times it as "the fill pass alone".
    rdp_layers = rdp_n[K_FILL];
    build_rdp_list(K_FILL, rdp_layers, arena + SCRATCH_OFF);
}

// ------------------------------------------------------------------ RSP DMA client

static int sp_delay = 0;    // sweep 9: extra loop iterations between DMAs (64 ns each)

static void sp_start(int dir, int len, void *region) {
    rsp_wait();
    rsp_load(&rsp_dmaloop);
    SP_DMEM[0] = 0;
    SP_DMEM[1] = PhysicalAddr(region);
    SP_DMEM[2] = len - 1;
    SP_DMEM[3] = dir;
    SP_DMEM[4] = SP_BUF_BYTES - 1;
    SP_DMEM[5] = sp_delay;
    *SP_STATUS = SP_WSTATUS_CLEAR_SIG0;
    rsp_run_async();
}
static inline uint32_t sp_count(void) { return SP_DMEM[0]; }
static void sp_stop(void) {
    *SP_STATUS = SP_WSTATUS_SET_SIG0;
    rsp_wait();
}

// ------------------------------------------------------------------ PI DMA client

static inline bool pi_busy(void) { return *(volatile uint32_t *)0xA4600010 & 3; }   // PI_STATUS: DMA or IO busy

static void pi_start(void) {
    dma_read_raw_async(pibuf, 0x10000000, PI_BYTES);    // the cart, from its start
}

// ------------------------------------------------------------------ measure

static float ticks_us(uint32_t t) { return t * (1e6f / TICKS_PER_SECOND); }

static void measure(meas_t *m, const cfg_t *c) {
    memset(m, 0, sizeof *m);
    m->cpu_ticks_min = 0xFFFFFFFF;
    m->rdp_us_min = 1e9f;
    if (c->rdp) build_rdp_list(c->rdp_kind, rdp_n[c->rdp_kind], arena + (c->rdp_off ? c->rdp_off : SCRATCH_OFF));
    void *sp_region = arena + (c->sp_off ? c->sp_off : SP_OFF);
    for (int r = 0; r < RUNS; r++) {
        if (c->cpu) c->cpu->prep();
        bool started = false;
        uint32_t pi_t0 = 0;
        if (c->sp_len) { sp_start(c->sp_dir, c->sp_len, sp_region); wait_ticks(TICKS_FROM_US(500)); }
        if (c->pi)     { pi_t0 = TICKS_READ(); pi_start(); }
        if (c->ai) {
            // Every buffer full before the window: the AI's two DMA slots
            // then outlast it (interrupts are off inside, so nothing refills).
            while (audio_can_write()) audio_write_silence();
            m->ai_status_before = *(volatile uint32_t *)0xA450000C;
        }
        if (c->rdp) {
            reset_counters();
            rdp_start();
            uint32_t t = TICKS_READ();      // wait for the pipe to actually run
            while (!rdp_pipe_busy() && TICKS_DISTANCE(t, TICKS_READ()) < TICKS_FROM_MS(50)) {}
            started = rdp_pipe_busy();
        }
        disable_interrupts();
        uint32_t sp0 = c->sp_len ? sp_count() : 0;
        uint32_t t0 = TICKS_READ();
        int n = 0;
        if (c->cpu) n = c->cpu->run();
        else while (TICKS_DISTANCE(t0, TICKS_READ()) < TICKS_FROM_US(SPIN_WINDOW_US)) {}
        uint32_t t1 = TICKS_READ();
        uint32_t sp1 = c->sp_len ? sp_count() : 0;
        bool rdp_busy_after = rdp_pipe_busy();
        bool pi_busy_after  = pi_busy();
        if (c->ai) m->ai_status_after = *(volatile uint32_t *)0xA450000C;
        enable_interrupts();
        if (c->sp_len) sp_stop();
        if (c->pi) {
            while (pi_busy()) {}
            m->pi_us_sum += ticks_us(TICKS_DISTANCE(pi_t0, TICKS_READ()));
            m->pi_busy_after += pi_busy_after;
        }
        if (c->rdp) {
            bool ok = rdp_wait();
            float us = pipe_us();
            m->tmem_us_sum += (*DP_TMEM_BUSY & 0xFFFFFF) / 62.5f;
            m->rdp_us_sum += us;
            if (us < m->rdp_us_min) m->rdp_us_min = us;
            m->rdp_started    += started;
            m->rdp_busy_after += rdp_busy_after;
            m->rdp_timeout    += !ok;
        }
        uint32_t ticks = TICKS_DISTANCE(t0, t1);
        m->cpu_n = n;
        m->cpu_ticks_sum += ticks;
        if (ticks < m->cpu_ticks_min) m->cpu_ticks_min = ticks;
        m->win_us_sum += ticks_us(ticks);
        m->sp_dmas_sum += sp1 - sp0;
    }
    if (!c->rdp) m->rdp_us_min = 0;
}

static float ns_per(uint32_t ticks, int n) { return n ? ticks * (1e9f / TICKS_PER_SECOND) / n : 0; }

// ------------------------------------------------------------------ sweep 1

static void print_cell1(const meas_t *m, const pattern_t *p, int load) {
    float ns_min  = ns_per(m->cpu_ticks_min, m->cpu_n);
    float ns_mean = ns_per(m->cpu_ticks_sum / RUNS, m->cpu_n);
    debugf("MB,%s,%s,%d,%d,%lu,%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d\n",
           p->name, LOAD_NAME[load], m->cpu_n, p->bytes,
           (unsigned long)m->cpu_ticks_min, (unsigned long)(m->cpu_ticks_sum / RUNS),
           ns_min, ns_mean, p->bytes * 1000.0f / ns_min,
           load == L_RDP ? rdp_alone_us[L_IDLE] : 0.0f, m->rdp_us_min,
           m->rdp_started, m->rdp_busy_after);
}

static void sweep1_load(int load) {
    for (int p = 0; p < N_PAT; p++) {
        cfg_t c = { .cpu = &PATTERNS[p], .rdp = load == L_RDP };
        measure(&results[p][load], &c);
        print_cell1(&results[p][load], &PATTERNS[p], load);
    }
}

static void clear_screen(void) {
    surface_t *fb = display_get();
    memset(fb->buffer, 0, fb->stride * fb->height);
    display_show(fb);
}

static void sweep1(void) {
    // VI off: the one moment the CPU has RDRAM to itself. Runs before the
    // display exists, so the TV is black for a second.
    *(volatile uint32_t *)0xA4400000 = 0;       // VI_CTRL type 0: no fetch, no sync
    wait_ms(50);
    rdp_alone_us[L_VIOFF] = time_fill_alone();
    sweep1_load(L_VIOFF);

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    clear_screen();
    wait_ms(50);
    rdp_alone_us[L_IDLE] = time_fill_alone();
    debugf("MB,# fill pass alone (%d layers): VI off %.1f us, VI on %.1f us\n", rdp_layers, rdp_alone_us[L_VIOFF], rdp_alone_us[L_IDLE]);
    sweep1_load(L_IDLE);
    sweep1_load(L_RDP);
    debugf("MB,# done%s\n", has_counters ? "" : " -- RDP counters read 0: emulator, timings are not the console's");
}

// ------------------------------------------------------------------ sweep 2

typedef struct { const char *name; cfg_t cfg; } cell2_t;

static const cell2_t CELLS2[] = {
    // the RSP DMA engine alone: fixed cost per DMA + cost per byte
    { "sp_rd64",        { NULL,     false, 0, 64,   false } },
    { "sp_rd512",       { NULL,     false, 0, 512,  false } },
    { "sp_rd2k",        { NULL,     false, 0, 2048, false } },
    { "sp_wr64",        { NULL,     false, 1, 64,   false } },
    { "sp_wr512",       { NULL,     false, 1, 512,  false } },
    { "sp_wr2k",        { NULL,     false, 1, 2048, false } },
    // RSP DMA against the CPU
    { "sp_rd2k+cpu",    { PAT_U64R, false, 0, 2048, false } },
    { "sp_wr2k+cpu",    { PAT_U64R, false, 1, 2048, false } },
    { "sp_rd64+cpu",    { PAT_U64R, false, 0, 64,   false } },
    { "sp_rd2k+cr16",   { PAT_CR16, false, 0, 2048, false } },
    // RSP DMA against the RDP
    { "sp_rd2k+rdp",    { NULL,     true,  0, 2048, false } },
    { "sp_wr2k+rdp",    { NULL,     true,  1, 2048, false } },
    { "sp_rd64+rdp",    { NULL,     true,  0, 64,   false } },
    // all three
    { "sp_rd2k+cpu+rdp",{ PAT_U64R, true,  0, 2048, false } },
    { "sp_wr2k+cpu+rdp",{ PAT_U64R, true,  1, 2048, false } },
    // the PI
    { "pi",             { NULL,     false, 0, 0,    true  } },
    { "pi+cpu",         { PAT_U64R, false, 0, 0,    true  } },
    { "pi+rdp",         { NULL,     true,  0, 0,    true  } },
    { "pi+sp_rd2k",     { NULL,     false, 0, 2048, true  } },
    { "pi+sp_wr2k",     { NULL,     false, 1, 2048, true  } },
    // cross-checks against sweep 1 and the alone figures
    { "cpu",            { PAT_U64R, false, 0, 0,    false } },
    { "cpu+rdp",        { PAT_U64R, true,  0, 0,    false } },
    { "rdp",            { NULL,     true,  0, 0,    false } },
};
enum { N_CELL2 = sizeof CELLS2 / sizeof CELLS2[0] };
static meas_t results2[N_CELL2];

static void sweep2(void) {
    debugf("M2,cell,cpu,rdp,sp,pi,runs,win_us,cpu_n,cpu_ticks_min,cpu_ns_min,cpu_ns_mean,sp_dmas,sp_MBps,rdp_alone_us,rdp_us_min,rdp_us_mean,rdp_started,rdp_busy_after,rdp_timeout,pi_us_mean,pi_MBps,pi_busy_after\n");
    for (int i = 0; i < N_CELL2; i++) {
        const cfg_t *c = &CELLS2[i].cfg;
        meas_t *m = &results2[i];
        measure(m, c);
        float win_us = m->win_us_sum / RUNS;
        float sp_mbps = c->sp_len && m->win_us_sum > 0 ? (float)m->sp_dmas_sum * c->sp_len / m->win_us_sum : 0;
        float pi_us = c->pi ? m->pi_us_sum / RUNS : 0;
        char sp[16] = "-";
        if (c->sp_len) snprintf(sp, sizeof sp, "%s%d", c->sp_dir ? "wr" : "rd", c->sp_len);
        debugf("M2,%s,%s,%d,%s,%d,%d,%.1f,%d,%lu,%.1f,%.1f,%lu,%.2f,%.1f,%.1f,%.1f,%d,%d,%d,%.1f,%.2f,%d\n",
               CELLS2[i].name, c->cpu ? c->cpu->name : "-", c->rdp, sp, c->pi, RUNS, win_us,
               m->cpu_n, (unsigned long)m->cpu_ticks_min,
               ns_per(m->cpu_ticks_min, m->cpu_n), ns_per(m->cpu_ticks_sum / RUNS, m->cpu_n),
               (unsigned long)(m->sp_dmas_sum / RUNS), sp_mbps,
               c->rdp ? rdp_alone_us[L_IDLE] : 0.0f, m->rdp_us_min, c->rdp ? m->rdp_us_sum / RUNS : 0.0f,
               m->rdp_started, m->rdp_busy_after, m->rdp_timeout,
               pi_us, pi_us > 0 ? PI_BYTES / pi_us : 0.0f, m->pi_busy_after);
    }
    debugf("M2,# done\n");
}

// ------------------------------------------------------------------ sweep 3: placement
//
// Does WHERE the other client's memory sits matter? The CPU reads its fixed
// 256 KiB at arena+0 while the RDP fills, or the RSP DMAs, a region placed at
// each offset in turn. If RDRAM banks or rows are what folklore says, some
// offsets collide and some do not.

// Arena offsets in KiB. The CPU buffer is at CPU_OFF (1024..1280): the 1 MiB
// block below it, the rest of its own block, the next block, and the blocks
// beyond 4 MiB physical (the Expansion Pak's chips) are all probed.
static const uint32_t OFFS[] = { 0, 256, 512, 768, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 3840 };
enum { N_OFF = sizeof OFFS / sizeof OFFS[0] };

static void sweep3(void) {
    debugf("M3,off_kb,phys,client,win_us,cpu_ns_min,cpu_ns_mean,rdp_alone_us,rdp_us_mean,rdp_started,rdp_busy_after,sp_dmas,sp_MBps\n");
    for (int i = 0; i < N_OFF; i++) {
        uint32_t off = OFFS[i] * 1024;
        unsigned long phys = PhysicalAddr(arena + off);
        meas_t alone, rdp, sp;
        cfg_t c_alone = { .cpu = NULL,         .rdp = true,  .rdp_off = off };
        cfg_t c_rdp   = { .cpu = &PATTERNS[0], .rdp = true,  .rdp_off = off };
        cfg_t c_sp    = { .cpu = &PATTERNS[0], .sp_dir = 0, .sp_len = 2048, .sp_off = off };
        measure(&alone, &c_alone);
        measure(&rdp, &c_rdp);
        measure(&sp, &c_sp);
        float alone_us = alone.rdp_us_sum / RUNS;
        debugf("M3,%lu,%08lx,rdp,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,0,0\n", (unsigned long)OFFS[i], phys, rdp.win_us_sum / RUNS,
               ns_per(rdp.cpu_ticks_min, rdp.cpu_n), ns_per(rdp.cpu_ticks_sum / RUNS, rdp.cpu_n),
               alone_us, rdp.rdp_us_sum / RUNS, rdp.rdp_started, rdp.rdp_busy_after);
        debugf("M3,%lu,%08lx,sp_rd2k,%.1f,%.1f,%.1f,0,0,0,0,%lu,%.2f\n", (unsigned long)OFFS[i], phys, sp.win_us_sum / RUNS,
               ns_per(sp.cpu_ticks_min, sp.cpu_n), ns_per(sp.cpu_ticks_sum / RUNS, sp.cpu_n),
               (unsigned long)(sp.sp_dmas_sum / RUNS), sp.win_us_sum > 0 ? (float)sp.sp_dmas_sum * 2048 / sp.win_us_sum : 0);
    }
    debugf("M3,# done\n");
}

// ------------------------------------------------------------------ sweep 5: stride, for the row size
//
// Uncached reads of every word of the 256 KiB buffer once, consecutive
// accesses `stride` bytes apart, same loop shape at every stride so only the
// memory changes. Consecutive accesses inside one open RDRAM row are cheap;
// where the stride crosses the row size every access opens a row.

static const uint32_t STRIDES[] = { 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
enum { N_STRIDE = sizeof STRIDES / sizeof STRIDES[0] };
static int stride_words;

static int run_stride(void) {
    uint32_t s = 0;
    int w = stride_words;
    for (int k = 0; k < w; k++)
        for (int i = k; i < N32; i += w) s += u32[i];
    sink = s;
    return N32;
}
static const pattern_t PAT_STRIDE = { "stride", prep_none, run_stride, 4 };

static void sweep5(void) {
    debugf("M5,stride_B,accesses,ticks_min,ns_min,ns_mean\n");
    for (int i = 0; i < N_STRIDE; i++) {
        stride_words = STRIDES[i] / 4;
        meas_t m;
        cfg_t c = { .cpu = &PAT_STRIDE };
        measure(&m, &c);
        debugf("M5,%lu,%d,%lu,%.1f,%.1f\n", (unsigned long)STRIDES[i], m.cpu_n, (unsigned long)m.cpu_ticks_min,
               ns_per(m.cpu_ticks_min, m.cpu_n), ns_per(m.cpu_ticks_sum / RUNS, m.cpu_n));
    }
    debugf("M5,# done\n");
}

// ------------------------------------------------------------------ sweep 4: the rspq-fed fill
//
// Run 1 (2026-10-09, first ROM) fed the fill through an rspq block and saw
// the CPU pay far more under it than the direct feed shows. Same target,
// same layers, through rspq, in the same ROM: is the feed path the variable?

static void sweep4(void) {
    surface_t s = surface_make(arena + SCRATCH_OFF, FMT_RGBA16, SCREEN_W, SCREEN_H, SCREEN_W * 2);
    rspq_block_begin();
    rdpq_set_color_image(&s);
    rdpq_set_mode_fill(RGBA32(32, 64, 96, 255));
    for (int i = 0; i < rdp_layers; i++) rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H);
    rspq_block_t *blk = rspq_block_end();

    debugf("M4,cell,cpu_ns_min,cpu_ns_mean,rdp_us_min,rdp_started,rdp_busy_after\n");
    for (int which = 0; which < 3; which++) {         // 0: CPU alone, rspq idle; 1: rspq fill alone; 2: both
        const pattern_t *p = &PATTERNS[0];
        uint32_t tmin = 0xFFFFFFFF, tsum = 0; float rdp_min = 1e9f; int started = 0, busy_after = 0, n = 0;
        for (int r = 0; r < RUNS; r++) {
            rspq_wait();
            bool st = false;
            if (which) {
                reset_counters();
                rspq_block_run(blk);
                rspq_flush();
                uint32_t t = TICKS_READ();
                while (!rdp_pipe_busy() && TICKS_DISTANCE(t, TICKS_READ()) < TICKS_FROM_MS(50)) {}
                st = rdp_pipe_busy();
            }
            disable_interrupts();
            uint32_t t0 = TICKS_READ();
            if (which != 1) n = p->run();
            else while (TICKS_DISTANCE(t0, TICKS_READ()) < TICKS_FROM_US(SPIN_WINDOW_US)) {}
            uint32_t t1 = TICKS_READ();
            bool ba = rdp_pipe_busy();
            enable_interrupts();
            if (which) { rspq_wait(); float us = pipe_us(); if (us < rdp_min) rdp_min = us; started += st; busy_after += ba; }
            uint32_t ticks = TICKS_DISTANCE(t0, t1);
            tsum += ticks; if (ticks < tmin) tmin = ticks;
        }
        static const char *NAME[] = { "cpu_rspq_idle", "rspq_fill_alone", "cpu+rspq_fill" };
        debugf("M4,%s,%.1f,%.1f,%.1f,%d,%d\n", NAME[which], ns_per(tmin, n), ns_per(tsum / RUNS, n),
               which ? rdp_min : 0.0f, started, busy_after);
    }
    rspq_block_free(blk);
    debugf("M4,# done\n");
}

// ------------------------------------------------------------------ sweep 7: what the RDP does matters
//
// Fill was pure writes. A texture load is pure reads through TMEM, and a
// Z-buffered textured rectangle is a colour write, a Z read and a Z write per
// pixel: what the diorama costs the bus. Each kind alone, against the CPU's
// uncached reads and line fills, and against RSP DMA.

static void sweep7(void) {
    debugf("M7,kind,cell,units,bytes_per_pass,win_us,cpu_ns_min,cpu_ns_mean,sp_dmas,sp_MBps,rdp_alone_us,rdp_us_mean,tmem_us_mean,rdp_MBps_alone,rdp_started,rdp_busy_after\n");
    for (int kind = 0; kind <= K_RECTZ; kind++) {
        struct { const char *name; cfg_t cfg; } cells[] = {
            { "alone",   { .cpu = NULL,         .rdp = true, .rdp_kind = kind } },
            { "cpu",     { .cpu = &PATTERNS[0], .rdp = true, .rdp_kind = kind } },
            { "cr16",    { .cpu = PAT_CR16,     .rdp = true, .rdp_kind = kind } },
            { "sp_rd2k", { .cpu = NULL,         .rdp = true, .rdp_kind = kind, .sp_dir = 0, .sp_len = 2048 } },
            { "sp_rd64", { .cpu = NULL,         .rdp = true, .rdp_kind = kind, .sp_dir = 0, .sp_len = 64 } },
        };
        float alone_us = 0;
        for (int i = 0; i < 5; i++) {
            meas_t m;
            measure(&m, &cells[i].cfg);
            float rdp_mean = m.rdp_us_sum / RUNS;
            if (i == 0) alone_us = rdp_mean;
            const cfg_t *c = &cells[i].cfg;
            debugf("M7,%s,%s,%d,%.0f,%.1f,%.1f,%.1f,%lu,%.2f,%.1f,%.1f,%.1f,%.1f,%d,%d\n", KIND_NAME[kind], cells[i].name,
                   rdp_n[kind], pass_bytes(kind, rdp_n[kind]), m.win_us_sum / RUNS,
                   ns_per(m.cpu_ticks_min, m.cpu_n), ns_per(m.cpu_ticks_sum / RUNS, m.cpu_n),
                   (unsigned long)(m.sp_dmas_sum / RUNS),
                   c->sp_len && m.win_us_sum > 0 ? (float)m.sp_dmas_sum * c->sp_len / m.win_us_sum : 0.0f,
                   alone_us, rdp_mean, m.tmem_us_sum / RUNS, alone_us > 0 ? pass_bytes(kind, rdp_n[kind]) / alone_us : 0.0f,
                   m.rdp_started, m.rdp_busy_after);
        }
    }
    debugf("M7,# done\n");
}

// ------------------------------------------------------------------ sweep 8: texture loads in detail
//
// LOAD_TILE of a 16-bit texture ran at one texel a clock. Is that the texel
// or the byte? Does LOAD_BLOCK (the 64-bit path) do better? Do short rows
// cost? Every variant moves 4 KiB a unit; alone and against CPU reads.

static void sweep8(void) {
    debugf("M8,kind,cell,units,win_us,cpu_ns_min,cpu_ns_mean,rdp_alone_us,rdp_us_mean,tmem_us_mean,us_per_4k,MBps_alone,tmem_frac,rdp_started,rdp_busy_after\n");
    static const int KINDS[] = { K_LOAD, K_LOADBLK, K_LOAD32, K_LOADI8, K_LOADNARROW, K_LOADWIDE, K_LOAD48, K_LOAD64R, K_LOAD96 };
    for (int i = 0; i < 9; i++) {
        int kind = KINDS[i];
        meas_t alone, cpu;
        cfg_t c_alone = { .cpu = NULL,         .rdp = true, .rdp_kind = kind };
        cfg_t c_cpu   = { .cpu = &PATTERNS[0], .rdp = true, .rdp_kind = kind };
        measure(&alone, &c_alone);
        measure(&cpu, &c_cpu);
        float alone_us = alone.rdp_us_sum / RUNS;
        // Ares reads 0 for the counters: never divide by the pass time unguarded (the FPU traps).
        float mbps  = alone_us > 0 ? pass_bytes(kind, rdp_n[kind]) / alone_us : 0;
        float tfrac = alone_us > 0 ? alone.tmem_us_sum / RUNS / alone_us : 0;
        debugf("M8,%s,alone,%d,%.1f,0,0,%.1f,%.1f,%.1f,%.2f,%.1f,%.3f,%d,%d\n", KIND_NAME[kind], rdp_n[kind], alone.win_us_sum / RUNS,
               alone_us, alone_us, alone.tmem_us_sum / RUNS, alone_us / rdp_n[kind], mbps, tfrac, alone.rdp_started, alone.rdp_busy_after);
        debugf("M8,%s,cpu,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.1f,%.3f,%d,%d\n", KIND_NAME[kind], rdp_n[kind], cpu.win_us_sum / RUNS,
               ns_per(cpu.cpu_ticks_min, cpu.cpu_n), ns_per(cpu.cpu_ticks_sum / RUNS, cpu.cpu_n),
               alone_us, cpu.rdp_us_sum / RUNS, cpu.tmem_us_sum / RUNS, alone_us / rdp_n[kind], mbps, tfrac, cpu.rdp_started, cpu.rdp_busy_after);
    }
    debugf("M8,# done\n");
}

// ------------------------------------------------------------------ sweep 6b: the VI's buffer in the CPU's block
//
// Sweep 6 had the framebuffer in another block. Point the VI (320x240x16) at
// a buffer inside the CPU's 1 MiB block instead, by poking VI_ORIGIN, and
// read it back after the window to be sure it stuck. A third state leaves
// the VI in blank mode (sync, no picture) to price that state too.

static void sweep6b(void) {
    volatile uint32_t *VI_CTRL_REG   = (volatile uint32_t *)0xA4400000;
    volatile uint32_t *VI_ORIGIN_REG = (volatile uint32_t *)0xA4400004;
    volatile uint32_t *VI_HVIDEO_REG = (volatile uint32_t *)0xA4400024;
    // The display driver re-shows its own buffer every vblank, so it has to
    // go: the VI is driven directly for these measurements. Buffer 0 is in
    // another block (the baseline), buffer 1 in the CPU's block.
    //
    // Run g (2026-10-09) measured this with the VI in BLANK mode: in the
    // preview SDK display_close() ends in vi_show(NULL), which blanks, and
    // vi_set_origin() does not un-blank (only vi_show / vi_blank do). The
    // origin read-back cannot see that. So: vi_blank(false) after the
    // origin, VI_CTRL and VI_H_VIDEO printed beside it, and a third state
    // that measures blank mode on purpose.
    display_close();
    void *fbs[2] = { arena, arena + CPU_OFF + 512 * 1024 };
    for (int i = 0; i < 2; i++) memset(UncachedAddr(fbs[i]), 0, SCREEN_W * SCREEN_H * 2);
    debugf("M6b,fb_phys,state,client,cpu_ns_min,cpu_ns_mean,rdp_alone_us,vi_origin_after,vi_ctrl_after,vi_hvideo_after\n");
    const pattern_t *pats[] = { &PATTERNS[0], &PATTERNS[1], &PATTERNS[2], PAT_CR16 };
    static const char *STATE[] = { "shown", "shown", "blank" };
    for (int which = 0; which < 3; which++) {
        void *shown = fbs[which == 2 ? 0 : which];
        uint32_t origin = PhysicalAddr(shown);
        vi_set_origin(shown, SCREEN_W, 16);
        vi_blank(which == 2);
        wait_ms(100);
        for (int p = 0; p < 4; p++) {
            meas_t m;
            cfg_t c = { .cpu = pats[p] };
            measure(&m, &c);
            debugf("M6b,%08lx,%s,%s,%.1f,%.1f,0,%08lx,%08lx,%08lx\n", (unsigned long)origin, STATE[which], pats[p]->name,
                   ns_per(m.cpu_ticks_min, m.cpu_n), ns_per(m.cpu_ticks_sum / RUNS, m.cpu_n),
                   (unsigned long)*VI_ORIGIN_REG, (unsigned long)*VI_CTRL_REG, (unsigned long)*VI_HVIDEO_REG);
        }
        meas_t m;
        cfg_t c = { .cpu = NULL, .rdp = true };
        measure(&m, &c);
        debugf("M6b,%08lx,%s,rdp_fill,0,0,%.1f,%08lx,%08lx,%08lx\n", (unsigned long)origin, STATE[which], m.rdp_us_sum / RUNS,
               (unsigned long)*VI_ORIGIN_REG, (unsigned long)*VI_CTRL_REG, (unsigned long)*VI_HVIDEO_REG);
    }
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    clear_screen();
    debugf("M6b,# done\n");
}

// ------------------------------------------------------------------ sweep 6: the VI by mode
//
// The VI is the one client that never stops. Its share at each framebuffer
// mode, seen by the CPU (uncached reads, line fills, writes) and by the RDP
// fill pass. One framebuffer, shown and left alone; its address is printed
// because placement matters.

typedef struct { const char *name; resolution_t res; bitdepth_t depth; } vimode_t;
static const vimode_t VIMODES[] = {
    { "320x240x16", RESOLUTION_320x240, DEPTH_16_BPP },
    { "320x240x32", RESOLUTION_320x240, DEPTH_32_BPP },
    { "640x480x16", RESOLUTION_640x480, DEPTH_16_BPP },
    { "640x480x32", RESOLUTION_640x480, DEPTH_32_BPP },
};
enum { N_VIMODE = sizeof VIMODES / sizeof VIMODES[0] };

static void sweep6(void) {
    debugf("M6,mode,fb_phys,fb_bytes,client,cpu_ns_min,cpu_ns_mean,rdp_alone_us\n");
    const pattern_t *pats[] = { &PATTERNS[0], &PATTERNS[1], &PATTERNS[2], PAT_CR16 };
    for (int v = 0; v < N_VIMODE; v++) {
        display_close();
        display_init(VIMODES[v].res, VIMODES[v].depth, 1, GAMMA_NONE, FILTERS_RESAMPLE);
        surface_t *fb = display_get();
        unsigned long phys = PhysicalAddr(fb->buffer), bytes = fb->stride * fb->height;
        memset(fb->buffer, 0, bytes);
        display_show(fb);
        wait_ms(100);
        for (int p = 0; p < 4; p++) {
            meas_t m;
            cfg_t c = { .cpu = pats[p] };
            measure(&m, &c);
            debugf("M6,%s,%08lx,%lu,%s,%.1f,%.1f,0\n", VIMODES[v].name, phys, bytes, pats[p]->name,
                   ns_per(m.cpu_ticks_min, m.cpu_n), ns_per(m.cpu_ticks_sum / RUNS, m.cpu_n));
        }
        meas_t m;
        cfg_t c = { .cpu = NULL, .rdp = true };
        measure(&m, &c);
        debugf("M6,%s,%08lx,%lu,rdp_fill,0,0,%.1f\n", VIMODES[v].name, phys, bytes, m.rdp_us_sum / RUNS);
    }
    display_close();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    clear_screen();
    debugf("M6,# done\n");
}

// ------------------------------------------------------------------ sweep 6c: a 32-bit buffer in the CPU's block
//
// Sweep 6b found the VI's placement a 2.6% effect at 640 bytes a line. Does
// it scale with bytes a line? The same test at 320x240x32 (1280 B a line,
// the same bytes a line as 640x480x16), driven directly like 6b.

static void sweep6c(void) {
    volatile uint32_t *VI_CTRL_REG   = (volatile uint32_t *)0xA4400000;
    volatile uint32_t *VI_ORIGIN_REG = (volatile uint32_t *)0xA4400004;
    display_close();
    void *fbs[2] = { arena, arena + CPU_OFF + 512 * 1024 };
    for (int i = 0; i < 2; i++) memset(UncachedAddr(fbs[i]), 0, SCREEN_W * SCREEN_H * 4);
    debugf("M6c,fb_phys,client,cpu_ns_min,cpu_ns_mean,rdp_alone_us,vi_origin_after,vi_ctrl_after\n");
    const pattern_t *pats[] = { &PATTERNS[0], &PATTERNS[1], &PATTERNS[2], PAT_CR16 };
    for (int which = 0; which < 2; which++) {
        void *shown = fbs[which];
        uint32_t origin = PhysicalAddr(shown);
        vi_set_origin(shown, SCREEN_W, 32);
        vi_set_xscale(SCREEN_W);
        vi_set_yscale(SCREEN_H);
        vi_blank(false);
        wait_ms(100);
        for (int p = 0; p < 4; p++) {
            meas_t m;
            cfg_t c = { .cpu = pats[p] };
            measure(&m, &c);
            debugf("M6c,%08lx,%s,%.1f,%.1f,0,%08lx,%08lx\n", (unsigned long)origin, pats[p]->name,
                   ns_per(m.cpu_ticks_min, m.cpu_n), ns_per(m.cpu_ticks_sum / RUNS, m.cpu_n),
                   (unsigned long)*VI_ORIGIN_REG, (unsigned long)*VI_CTRL_REG);
        }
        meas_t m;
        cfg_t c = { .cpu = NULL, .rdp = true };
        measure(&m, &c);
        debugf("M6c,%08lx,rdp_fill,0,0,%.1f,%08lx,%08lx\n", (unsigned long)origin, m.rdp_us_sum / RUNS,
               (unsigned long)*VI_ORIGIN_REG, (unsigned long)*VI_CTRL_REG);
    }
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    clear_screen();
    debugf("M6c,# done\n");
}

// ------------------------------------------------------------------ sweep 9: the loose ends
//
// 9a. The RSP DMA floor: ~440 ns per 64 B DMA in sweep 2. Engine or loop?
//     The ucode takes a delay parameter (64 ns an iteration); if the cost
//     per DMA rises by 64 ns per iteration from zero, the loop was the
//     floor; if it stays flat until the delay exceeds it, the engine is.
// 9b. Cached write-back alone: 8 KiB resident and dirty, re-dirtied and
//     written back WB_ITERS times, against the same without the write-back.
// 9c. The AI as a client: 44.1 kHz stereo silence streaming (176 KB/s)
//     under the CPU's patterns and the RDP fill.

static void sweep9(void) {
    // 9a
    debugf("M9,spfloor,len,delay_iters,dmas,win_us,ns_per_dma,MBps\n");
    static const int LENS[]   = { 64, 128, 512 };
    static const int DELAYS[] = { 0, 1, 2, 4, 8, 16, 32 };
    for (int l = 0; l < 3; l++) {
        for (int d = 0; d < 7; d++) {
            if (LENS[l] == 512 && (DELAYS[d] == 1 || DELAYS[d] == 2 || DELAYS[d] == 8)) continue;
            sp_delay = DELAYS[d];
            meas_t m;
            cfg_t c = { .cpu = NULL, .sp_dir = 0, .sp_len = LENS[l] };
            measure(&m, &c);
            float dmas = (float)m.sp_dmas_sum / RUNS, win = m.win_us_sum / RUNS;
            debugf("M9,spfloor,%d,%d,%.0f,%.1f,%.1f,%.2f\n", LENS[l], DELAYS[d], dmas, win,
                   dmas > 0 ? win * 1000 / dmas : 0, win > 0 ? dmas * LENS[l] / win : 0);
        }
    }
    sp_delay = 0;

    // 9b
    debugf("M9,wb,cell,lines,ticks_min,ns_per_line_min,ns_per_line_mean,rdp_alone_us,rdp_us_mean,rdp_started,rdp_busy_after\n");
    {
        struct { const char *name; cfg_t cfg; } cells[] = {
            { "cdirty8k",     { .cpu = &PAT_CDIRTY } },
            { "cwb8k",        { .cpu = &PAT_CWB } },
            { "cdirty8k+rdp", { .cpu = &PAT_CDIRTY, .rdp = true } },
            { "cwb8k+rdp",    { .cpu = &PAT_CWB,    .rdp = true } },
        };
        uint32_t dirty_min[2] = { 0, 0 }, dirty_mean[2] = { 0, 0 };
        for (int i = 0; i < 4; i++) {
            meas_t m;
            measure(&m, &cells[i].cfg);
            if (i % 2 == 0) { dirty_min[i / 2] = m.cpu_ticks_min; dirty_mean[i / 2] = m.cpu_ticks_sum / RUNS; }
            debugf("M9,wb,%s,%d,%lu,%.1f,%.1f,%.1f,%.1f,%d,%d\n", cells[i].name, m.cpu_n, (unsigned long)m.cpu_ticks_min,
                   ns_per(m.cpu_ticks_min, m.cpu_n), ns_per(m.cpu_ticks_sum / RUNS, m.cpu_n),
                   cells[i].cfg.rdp ? rdp_alone_us[L_IDLE] : 0.0f, cells[i].cfg.rdp ? m.rdp_us_sum / RUNS : 0.0f,
                   m.rdp_started, m.rdp_busy_after);
            if (i % 2 == 1) {
                // the write-back alone: the pair's difference per line
                debugf("M9,wb,%s,%d,%lu,%.1f,%.1f,0,0,0,0\n", i == 1 ? "writeback_alone" : "writeback_alone+rdp", m.cpu_n,
                       (unsigned long)(m.cpu_ticks_min - dirty_min[i / 2]),
                       ns_per(m.cpu_ticks_min - dirty_min[i / 2], m.cpu_n),
                       ns_per(m.cpu_ticks_sum / RUNS - dirty_mean[i / 2], m.cpu_n));
            }
        }
    }

    // 9c
    audio_init(44100, AUDIO_INIT_LATENCY_MS(200));
    debugf("M9,ai,cell,accesses,cpu_ns_min,cpu_ns_mean,rdp_alone_us,rdp_us_mean,ai_status_before,ai_status_after,rdp_started,rdp_busy_after\n");
    {
        struct { const char *name; cfg_t cfg; } cells[] = {
            { "u32r",      { .cpu = &PATTERNS[0] } },
            { "u32r+ai",   { .cpu = &PATTERNS[0], .ai = true } },
            { "cr16",      { .cpu = PAT_CR16 } },
            { "cr16+ai",   { .cpu = PAT_CR16, .ai = true } },
            { "u32w",      { .cpu = &PATTERNS[2] } },
            { "u32w+ai",   { .cpu = &PATTERNS[2], .ai = true } },
            { "rdp",       { .cpu = NULL, .rdp = true } },
            { "rdp+ai",    { .cpu = NULL, .rdp = true, .ai = true } },
        };
        for (int i = 0; i < 8; i++) {
            meas_t m;
            measure(&m, &cells[i].cfg);
            debugf("M9,ai,%s,%d,%.1f,%.1f,%.1f,%.1f,%08lx,%08lx,%d,%d\n", cells[i].name, m.cpu_n,
                   ns_per(m.cpu_ticks_min, m.cpu_n), ns_per(m.cpu_ticks_sum / RUNS, m.cpu_n),
                   cells[i].cfg.rdp ? rdp_alone_us[L_IDLE] : 0.0f, cells[i].cfg.rdp ? m.rdp_us_sum / RUNS : 0.0f,
                   (unsigned long)m.ai_status_before, (unsigned long)m.ai_status_after, m.rdp_started, m.rdp_busy_after);
        }
    }
    audio_close();
    debugf("M9,# done\n");
}

// ------------------------------------------------------------------ results screen

static int font_id = 1;

static float sp_mbps_of(int i) {
    const cfg_t *c = &CELLS2[i].cfg;
    return results2[i].win_us_sum > 0 ? (float)results2[i].sp_dmas_sum * c->sp_len / results2[i].win_us_sum : 0;
}

static void show(int page) {
    surface_t *fb = display_get();
    rdpq_attach_clear(fb, NULL);
    rdpq_set_mode_standard();
    int y = 20;
    if (page == 0) {
        rdpq_text_printf(NULL, font_id, 8, y, "MemBench sweep 1: ns per access (fastest run)");
        y += 14;
        rdpq_text_printf(NULL, font_id, 8, y, "pattern   bytes   vioff    idle  rdpfill");
        y += 12;
        for (int p = 0; p < N_PAT; p++) {
            char line[96];
            int n = snprintf(line, sizeof line, "%-9s %3d ", PATTERNS[p].name, PATTERNS[p].bytes);
            for (int l = 0; l < N_LOAD; l++) {
                float v = ns_per(results[p][l].cpu_ticks_min, results[p][l].cpu_n);
                n += snprintf(line + n, sizeof line - n, v < 1000 ? "%8.1f" : "%8.0f", v);
            }
            rdpq_text_printf(NULL, font_id, 8, y, "%s", line);
            y += 12;
        }
        y += 6;
        rdpq_text_printf(NULL, font_id, 8, y, "fill pass alone: VI off %.0f us, VI on %.0f us", rdp_alone_us[L_VIOFF], rdp_alone_us[L_IDLE]);
        y += 12;
        rdpq_text_printf(NULL, font_id, 8, y, "fill pass with CPU: u32r %.0f  cr16 %.0f us",
                         results[0][L_RDP].rdp_us_min, results[6][L_RDP].rdp_us_min);
    } else {
        rdpq_text_printf(NULL, font_id, 8, y, "MemBench sweep 2: DMA clients");
        y += 14;
        rdpq_text_printf(NULL, font_id, 8, y, "cell              sp MB/s  cpu ns  rdp us   pi us");
        y += 12;
        for (int i = 0; i < N_CELL2; i++) {
            const meas_t *m = &results2[i];
            const cfg_t *c = &CELLS2[i].cfg;
            rdpq_text_printf(NULL, font_id, 8, y, "%-17s %7.1f %7.0f %7.0f %7.0f", CELLS2[i].name,
                             sp_mbps_of(i), ns_per(m->cpu_ticks_min, m->cpu_n),
                             c->rdp ? m->rdp_us_sum / RUNS : 0.0f, c->pi ? m->pi_us_sum / RUNS : 0.0f);
            y += 8;
        }
    }
    y += 10;
    if (!has_counters)
        rdpq_text_printf(NULL, font_id, 8, y, "RDP counters read 0 (emulator?)"), y += 12;
    rdpq_text_printf(NULL, font_id, 8, y, "A: other page    (Reset to rerun)");
    rdpq_detach_show();
}

int main(void) {
    debug_init_isviewer();
    debug_init_usblog();
    rsp_init();
    joypad_init();

    arena = memalign(1024 * 1024, ARENA_BYTES);
    rdp_cmds = malloc_uncached(RDP_CMD_QWORDS * 8);
    assertf(arena && rdp_cmds, "no room for the buffers");
    buf = arena + CPU_OFF;
    pibuf = arena + PI_OFF;
    memset(buf, 0, BUF_BYTES);
    data_cache_hit_writeback_invalidate(arena, ARENA_BYTES);
    u32 = UncachedAddr(buf);
    u64 = UncachedAddr(buf);
    volatile uint16_t *tex = UncachedAddr(arena + TEX_OFF);     // the RDP's texture source
    for (int i = 0; i < TEX_W * TEX_H; i++) tex[i] = TEXEL;

    debugf("MB,# MemBench: tv=%s mem=%d MiB ticks/s=%lu buf=%d KiB runs=%d arena=%08lx cpu_buf=%08lx rdp_target=%08lx sp_region=%08lx\n",
           get_tv_type() == TV_PAL ? "PAL" : get_tv_type() == TV_NTSC ? "NTSC" : "MPAL",
           get_memory_size() >> 20, (unsigned long)TICKS_PER_SECOND, BUF_BYTES / 1024, RUNS,
           (unsigned long)PhysicalAddr(arena), (unsigned long)PhysicalAddr(buf),
           (unsigned long)PhysicalAddr(arena + SCRATCH_OFF), (unsigned long)PhysicalAddr(arena + SP_OFF));
    calibrate_rdp();
    debugf("MB,pattern,load,accesses,bytes_per,ticks_min,ticks_mean,ns_min,ns_mean,MBps_min,rdp_alone_us,rdp_us,rdp_started,rdp_busy_after\n");
    sweep1();
    sweep2();
    sweep3();
    sweep5();
    sweep7();
    sweep8();
    sweep6();
    sweep6b();
    sweep6c();
    sweep9();

    // Only now the RSP goes to rspq, for sweep 4 and the text. A SYNC_FULL
    // interrupt may be pending from the raw passes; clear it so rdpq does not
    // count it.
    *(volatile uint32_t *)0xA4300000 = 0x800;   // MI_MODE: clear DP interrupt
    rdpq_init();
    rdpq_text_register_font(font_id, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));
    sweep4();

    int page = 0;
    while (1) {
        show(page);
        joypad_poll();
        joypad_buttons_t b = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if (b.a) page ^= 1;
    }
}
