// MemBench: an RSP ucode that DMAs between RDRAM and DMEM in a loop, as fast
// as the DMA engine lets it, until the CPU sets SIG0. A pure RSP DMA client
// for the RDRAM bus, with no CPU in the path.
//
// DMEM parameters (the CPU writes them before rsp_run_async):
//   0x00  counter: DMAs issued so far (the RSP writes it, the CPU reads it)
//   0x04  RDRAM base, physical, 8-byte aligned
//   0x08  DMA size register value (bytes - 1), 8-byte granular
//   0x0C  direction: 0 = RDRAM -> DMEM (read), else DMEM -> RDRAM (write)
//   0x10  region mask (bytes - 1, power of two): the RDRAM offset wraps on it
// DMEM 0x100.. is the transfer target, so a DMA may be up to 3840 bytes.

#include <rsp.inc>

    .set noreorder
    .set at

    .data
    .align 3
PARAMS:
    .long 0     # counter
    .long 0     # rdram base
    .long 0     # size - 1
    .long 0     # direction
    .long 0     # region mask

    .text
    .globl _start
_start:
    lw s0, 0x04(zero)           # base
    lw t1, 0x08(zero)           # size register value
    lw t2, 0x0C(zero)           # direction
    lw t3, 0x10(zero)           # mask
    addiu t4, t1, 1             # bytes per DMA
    move t5, zero               # offset
    move t6, zero               # counter
    li s1, 0x100                # DMEM target

loop:
    mfc0 t0, COP0_SP_STATUS
    andi t0, SP_STATUS_SIG0
    bne t0, zero, done
    nop
wait_slot:
    mfc0 t0, COP0_DMA_FULL      # the engine queues one DMA behind the running one
    bne t0, zero, wait_slot
    nop
    addu t7, s0, t5
    mtc0 s1, COP0_DMA_SPADDR
    mtc0 t7, COP0_DMA_RAMADDR
    beq t2, zero, issue_read
    nop
    mtc0 t1, COP0_DMA_WRITE
    j issued
    nop
issue_read:
    mtc0 t1, COP0_DMA_READ
issued:
    addu t5, t5, t4
    and t5, t5, t3
    addiu t6, t6, 1
    sw t6, 0x00(zero)
    j loop
    nop

done:
    mfc0 t0, COP0_DMA_BUSY      # let the last DMA land before halting
    bne t0, zero, done
    nop
    break
    nop
