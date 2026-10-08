# MemBench

What does a memory access cost on the Nintendo 64, and who wins the RDRAM bus
when the CPU, the RSP's DMA engine, the RDP, the PI and the VI all want it?
Nobody had published accurate numbers and no emulator models them. This ROM
measures them on a console and prints every cell as a CSV line.

Sibling of `FillBench/` (the RDP side). Same pattern: an LLM-written sweep,
timed on hardware, captured over a SummerCart64, kept as CSV in the repo.

## Findings (PAL console, Expansion Pak, 2026-10-09, six runs)

- **RDRAM conflicts live in 1 MiB-aligned blocks.** Another client whose
  memory shares the CPU's 1 MiB block costs the CPU +40 to +50% per access
  and halves itself; in any other block it costs +8% and keeps 73%.
- **Row size 2 KiB, row miss 85 ns.**
- **Priority: RSP DMA > CPU > RDP.** 2 KiB RSP DMAs stop the RDP completely,
  whatever it is doing. The PI at 5 MB/s is noise.
- **Uncached 32-bit read 364 ns; 64-bit read 385; a 16 B cached line fill
  406:** the same price for 4, 8 or 16 bytes. Uncached writes 179 ns.
- **Fill mode 215 MB/s; RSP DMA 381 MB/s read, 397 write; LOAD_TILE of a
  16-bit texture 133 MB/s (one texel a clock, TMEM-bound); a Z-buffered
  textured pixel 59 ns.**
- **The VI costs bytes per scanline:** 320x240x16 takes 5% of the CPU and 7%
  of the RDP; 320x240x32 and 640x480x16 take 11% / 16%; 640x480x32 takes
  30% / 42%.

Full analysis, tables and the follow-up list: `reference/membench.md` in the
workspace root.

## The sweeps

| prefix | sweep |
|---|---|
| `MB,` | 1: eight CPU access patterns under VI off / VI on / RDP filling |
| `M2,` | 2: RSP DMA (64 B to 2 KiB, read and write), PI DMA, RDP, CPU, alone and in pairs |
| `M3,` | 3: the other client's region moved across a 4 MiB arena (placement) |
| `M5,` | 5: uncached read stride 4 B to 32 KiB (the row) |
| `M7,` | 7: the RDP pass as fill, LOAD_TILE, or Z-buffered textured rects, against CPU and RSP |
| `M8,` | 8: texture loads: LOAD_TILE vs LOAD_BLOCK, RGBA16/RGBA32/I8, 32 B vs 512 B rows (built, awaiting a console run) |
| `M6,` | 6: the VI at 320x240x16/x32 and 640x480x16/x32, seen by the CPU and the RDP |
| `M6b,` | 6b: the VI's framebuffer inside the CPU's 1 MiB block vs elsewhere (built, awaiting a console run) |
| `M4,` | 4: the fill fed through rspq instead of straight from RDRAM |

Every row prints raw tick counts beside derived figures. RDP rows carry
`rdp_started` and `rdp_busy_after`, both of which must read 5 (of 5 runs) or
the window outlasted the pass.

## Build and run

    /c/msys64/usr/bin/bash -lc "/c/Nintendo64/MemBench/build.sh"
    /c/Nintendo64/tools/flash.sh -d --secs 200 membench.z64 > raw/hw-<date>.log
    grep -E '^M[B2-8]b?,' raw/hw-<date>.log > results-hw-<date>.csv

Press Reset promptly after the upload: an SC64 upload does not restart the
console, and the run takes about 80 s. The TV is black while the sweeps run
(the RSP is busy with the DMA ucode, so there is no text until the end),
flickers through the video modes near the end, then shows two result pages
(A flips between them).

`bash tools/ares.sh` runs it in Ares as a crash and format check only. Ares
reports 0 for the RDP counters and its CPU timings are not the console's.

## Files

- `src/main.c` — the sweeps; `src/rsp_dmaloop.S` — the RSP DMA client.
- `results-hw-2026-10-09*.csv` — six console runs (a: sweep 1 via rspq;
  b: sweeps 1-2; c: sweeps 1-4, colliding placement; d: sweeps 1-5, clean;
  e: all seven, twice, with a wrong "fill alone" reference in sweeps 1-2;
  f: the reference fixed, cut off during sweep 7).
- `raw/hw-*.log` — the complete SC64 captures those CSVs came from.
