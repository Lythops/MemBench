# MemBench

What does a memory access cost on the Nintendo 64, and who wins the RDRAM bus
when the CPU, the RDP and the VI all want it? Nobody has published accurate
numbers and no emulator models them. This ROM measures them on a console and
prints every cell as a CSV line.

Sibling of `FillBench/` (the RDP side). Same pattern: an LLM-written sweep,
timed on hardware, captured over a SummerCart64, kept as CSV in the repo.

## Sweep 1 — CPU access cost under load

Eight access patterns over a 256 KiB buffer, each under three loads:

| load | what else is on the bus |
|---|---|
| `vioff` | nothing: VI_CTRL type 0, no scan-out |
| `idle` | the VI scanning out 320x240x16 |
| `rdpfill` | the VI plus the RDP filling a scratch buffer flat out (fill mode) |

Patterns: uncached 32/64-bit reads and writes, uncached reads 64 B and 4 KiB
apart, cached line fills (`cr16`) and cached fill plus write-back (`cw16`).

Every row prints the raw tick count (CPU COUNT register, 46.875 MHz) beside
the derived ns per access, and for `rdpfill` how long the RDP's own fill pass
took with the CPU hammering, against the same pass with the CPU quiet.

## Build and run

    /c/msys64/usr/bin/bash -lc "/c/Nintendo64/MemBench/build.sh"
    /c/Nintendo64/tools/flash.sh -d --secs 120 membench.z64 > raw/hw-<date>.log
    grep '^MB,' raw/hw-<date>.log > results-hw-<date>.csv

Press Reset after the upload: an SC64 upload does not restart the console.
The TV goes black for the first second (the `vioff` cells run before the
display exists) and again on every Start-triggered rerun.

`bash tools/ares.sh` runs it in Ares as a crash and format check only. Ares
reports 0 for the RDP counters and its CPU timings are not the console's.

## Reading the numbers

- `ns_min` is the fastest of five runs; `ns_mean` beside it shows the spread.
- `rdp_started` and `rdp_busy_after` must both read 5 for an `rdpfill` row to
  mean anything: the RDP was running when the window opened and still running
  when it closed. A smaller number means the CPU window outlasted the pass.
- `rdp_us` versus `rdp_alone_us` is the arbitration seen from the RDP's side.

Full notes, planned sweeps and traps: `reference/membench.md` in the
workspace root.
