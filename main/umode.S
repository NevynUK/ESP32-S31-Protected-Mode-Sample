/*
 * umode.S -- the M-mode side of the U-mode window.
 *
 * SPDX-License-Identifier: MIT
 *
 * umode_enter() is an ordinary C call from a FreeRTOS task.  It parks the
 * kernel's state, switches to the context's address space and mrets to U-mode.
 * Every trap comes back through the private vector and returns from that same
 * call, so blocking work happens in the caller, in task context.
 *
 * What differs from the ESP32-P4 version, and why:
 *
 *  - satp selects the user's page tables for the window.  M-mode is never
 *    translated, so the kernel is unaffected.
 *  - Interrupts are NOT masked in the window.  mintthresh only applies to
 *    M-mode on the S31, so an interrupt in U-mode traps to the private vector,
 *    which returns to the kernel with the (level-triggered) source still pending
 *    and ESP-IDF services it.  That is also what preempts a spinning user.
 *  - Ten user-level CSRs hold state between windows (measured by probe/).  They
 *    are swapped per context here, or they would be a channel between users.
 *  - gp and tp are not inherited from the kernel.  They would leak its pointers.
 *  - FPU, PIE and hardware loops are off for the window, so a user can neither
 *    read nor corrupt another task's coprocessor registers.
 *  - mstatus.TW is set, so a user's wfi traps instead of stalling a core that
 *    has interrupts masked.
 *  - The hardware stack guard is simply paused.  A user's stack overflow hits an
 *    unmapped guard page, and the kernel's own M-mode stack guard stays intact,
 *    which the P4 had to give up.
 *
 * Ordering rules carried over from the P4:
 *
 *  - MIE comes down before any per-hart CSR is read, so a migration cannot mix
 *    two cores' state.
 *  - mcause is written before mstatus, because mcause.mpp aliases mstatus.MPP.
 *  - The window is left through an mret, which restores the CLIC level.
 */

#include "umode.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h" /* must precede hw_stack_guard.h: selects _CUR_CORE dispatch */
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
#include "esp_private/hw_stack_guard.h"
#endif

#define KFRAME_SIZE 64
#define KF_RA 0
#define KF_GP 4
#define KF_TP 8
#define KF_S0 12
#define KF_S1 16
#define KF_S2 20
#define KF_S3 24
#define KF_S4 28
#define KF_S5 32
#define KF_S6 36
#define KF_S7 40
#define KF_S8 44
#define KF_S9 48
#define KF_S10 52
#define KF_S11 56

/* Save the hart's value of \csr as the kernel's and install the user's. */
    .macro UCSR_IN csr, idx
    csrr  t0, \csr
    sw    t0, (UCTX_K_CSR + \idx * 4)(a0)
    lw    t0, (UCTX_U_CSR + \idx * 4)(a0)
    csrw  \csr, t0
    .endm

/* Save the hart's value of \csr as the user's and put the kernel's back. */
    .macro UCSR_OUT csr, idx
    csrr  t0, \csr
    sw    t0, (UCTX_U_CSR + \idx * 4)(a0)
    lw    t0, (UCTX_K_CSR + \idx * 4)(a0)
    csrw  \csr, t0
    .endm

    .section .iram1.umode, "ax", @progbits
    .option push
    .option norvc

/* uint32_t umode_enter(umode_ctx_t *ctx) */
    .balign 4
    .global umode_enter
    .type umode_enter, @function
umode_enter:
    addi  sp, sp, -KFRAME_SIZE
    sw    ra, KF_RA(sp)
    sw    gp, KF_GP(sp)
    sw    tp, KF_TP(sp)
    sw    s0, KF_S0(sp)
    sw    s1, KF_S1(sp)
    sw    s2, KF_S2(sp)
    sw    s3, KF_S3(sp)
    sw    s4, KF_S4(sp)
    sw    s5, KF_S5(sp)
    sw    s6, KF_S6(sp)
    sw    s7, KF_S7(sp)
    sw    s8, KF_S8(sp)
    sw    s9, KF_S9(sp)
    sw    s10, KF_S10(sp)
    sw    s11, KF_S11(sp)

    /* Only MIE is used from this copy, and MIE is 1 on whichever hart a running
     * task is on, so reading it before the mask is safe. */
    csrr  t0, mstatus
    sw    t0, UCTX_K_MSTATUS(a0)
    csrci mstatus, MSTATUS_MIE

    csrr  t0, mtvec
    sw    t0, UCTX_K_MTVEC(a0)
    csrr  t0, CSR_MTVT
    sw    t0, UCTX_K_MTVT(a0)
    csrr  t0, mscratch
    sw    t0, UCTX_K_MSCRATCH(a0)
    csrr  t0, CSR_MINTSTATUS
    sw    t0, UCTX_K_MINTSTATUS(a0)
    sw    sp, UCTX_K_SP(a0)

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    ESP_HW_STACK_GUARD_MONITOR_STOP_CUR_CORE t0 t1
#endif

    /* Coprocessors off.  The state CSR is only a flag: the registers are kept. */
    csrrw t0, CSR_HWLP_STATE, zero
    sw    t0, UCTX_K_HWLP(a0)
    csrr  t1, mhartid
    beqz  t1, 1f                 /* PIE exists on core 1 only */
    csrrw t0, CSR_PIE_STATE, zero
    sw    t0, UCTX_K_PIE(a0)
1:

    UCSR_IN 0x000, 0
    UCSR_IN 0x005, 1
    UCSR_IN 0x007, 2
    UCSR_IN 0x017, 3
    UCSR_IN 0x040, 4
    UCSR_IN 0x041, 5
    UCSR_IN 0x042, 6
    UCSR_IN 0x047, 7
    UCSR_IN 0x803, 8
    UCSR_IN 0x805, 9

    /* The user's address space.  A full flush is deliberately conservative: the
     * ASIDs should make it unnecessary, but that has not been measured yet. */
    lw    t0, UCTX_SATP(a0)
    csrw  satp, t0
    sfence.vma

    la    t0, umode_mtvt_table
    csrw  CSR_MTVT, t0
    la    t0, umode_trap_entry
    ori   t0, t0, 3
    csrw  mtvec, t0
    csrw  mscratch, a0

    lw    t0, UCTX_PC(a0)
    csrw  mepc, t0
    csrw  mcause, zero

    /* Live mstatus (ustatus was swapped above), dropping to U with FP/vector off. */
    csrr  t0, mstatus
    li    t1, ~(MSTATUS_MPP | MSTATUS_MIE | MSTATUS_FS | MSTATUS_VS | MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR)
    and   t0, t0, t1
    li    t1, (MSTATUS_MPIE | MSTATUS_TW)
    or    t0, t0, t1
    csrw  mstatus, t0

    lw    ra, UCTX_X(1)(a0)
    lw    gp, UCTX_X(3)(a0)
    lw    tp, UCTX_X(4)(a0)
    lw    t2, UCTX_X(7)(a0)
    lw    s0, UCTX_X(8)(a0)
    lw    s1, UCTX_X(9)(a0)
    lw    a1, UCTX_X(11)(a0)
    lw    a2, UCTX_X(12)(a0)
    lw    a3, UCTX_X(13)(a0)
    lw    a4, UCTX_X(14)(a0)
    lw    a5, UCTX_X(15)(a0)
    lw    a6, UCTX_X(16)(a0)
    lw    a7, UCTX_X(17)(a0)
    lw    s2, UCTX_X(18)(a0)
    lw    s3, UCTX_X(19)(a0)
    lw    s4, UCTX_X(20)(a0)
    lw    s5, UCTX_X(21)(a0)
    lw    s6, UCTX_X(22)(a0)
    lw    s7, UCTX_X(23)(a0)
    lw    s8, UCTX_X(24)(a0)
    lw    s9, UCTX_X(25)(a0)
    lw    s10, UCTX_X(26)(a0)
    lw    s11, UCTX_X(27)(a0)
    lw    t3, UCTX_X(28)(a0)
    lw    t4, UCTX_X(29)(a0)
    lw    t5, UCTX_X(30)(a0)
    lw    t6, UCTX_X(31)(a0)
    lw    sp, UCTX_X(2)(a0)
    lw    t0, UCTX_X(5)(a0)
    lw    t1, UCTX_X(6)(a0)
    lw    a0, UCTX_X(10)(a0)
    mret
    .size umode_enter, . - umode_enter

/* The private vector.  CLIC mode enters at mtvec.base, so 64-byte aligned. */
    .balign 64
    .global umode_trap_entry
    .type umode_trap_entry, @function
umode_trap_entry:
    csrrw t0, mscratch, t0       /* t0 = ctx; mscratch = user's t0 */

    sw    ra, UCTX_X(1)(t0)
    sw    sp, UCTX_X(2)(t0)
    sw    gp, UCTX_X(3)(t0)
    sw    tp, UCTX_X(4)(t0)
    sw    t1, UCTX_X(6)(t0)
    sw    t2, UCTX_X(7)(t0)
    sw    s0, UCTX_X(8)(t0)
    sw    s1, UCTX_X(9)(t0)
    sw    a0, UCTX_X(10)(t0)
    sw    a1, UCTX_X(11)(t0)
    sw    a2, UCTX_X(12)(t0)
    sw    a3, UCTX_X(13)(t0)
    sw    a4, UCTX_X(14)(t0)
    sw    a5, UCTX_X(15)(t0)
    sw    a6, UCTX_X(16)(t0)
    sw    a7, UCTX_X(17)(t0)
    sw    s2, UCTX_X(18)(t0)
    sw    s3, UCTX_X(19)(t0)
    sw    s4, UCTX_X(20)(t0)
    sw    s5, UCTX_X(21)(t0)
    sw    s6, UCTX_X(22)(t0)
    sw    s7, UCTX_X(23)(t0)
    sw    s8, UCTX_X(24)(t0)
    sw    s9, UCTX_X(25)(t0)
    sw    s10, UCTX_X(26)(t0)
    sw    s11, UCTX_X(27)(t0)
    sw    t3, UCTX_X(28)(t0)
    sw    t4, UCTX_X(29)(t0)
    sw    t5, UCTX_X(30)(t0)
    sw    t6, UCTX_X(31)(t0)

    csrr  t1, mscratch
    sw    t1, UCTX_X(5)(t0)
    csrr  t1, mepc
    sw    t1, UCTX_PC(t0)
    csrr  t1, mcause
    sw    t1, UCTX_MCAUSE(t0)
    csrr  t1, mtval
    sw    t1, UCTX_MTVAL(t0)

    mv    a0, t0                 /* a0 = ctx from here on */

    csrw  satp, zero

    UCSR_OUT 0x000, 0
    UCSR_OUT 0x005, 1
    UCSR_OUT 0x007, 2
    UCSR_OUT 0x017, 3
    UCSR_OUT 0x040, 4
    UCSR_OUT 0x041, 5
    UCSR_OUT 0x042, 6
    UCSR_OUT 0x047, 7
    UCSR_OUT 0x803, 8
    UCSR_OUT 0x805, 9

    lw    t0, UCTX_K_HWLP(a0)
    csrw  CSR_HWLP_STATE, t0
    csrr  t1, mhartid
    beqz  t1, 2f
    lw    t0, UCTX_K_PIE(a0)
    csrw  CSR_PIE_STATE, t0
2:

    lw    t0, UCTX_K_MTVEC(a0)
    csrw  mtvec, t0
    lw    t0, UCTX_K_MTVT(a0)
    csrw  CSR_MTVT, t0
    lw    t0, UCTX_K_MSCRATCH(a0)
    csrw  mscratch, t0
    lw    sp, UCTX_K_SP(a0)

    /* gp and tp must be the kernel's BEFORE the mret below re-enables interrupts.
     * An ISR assumes both are valid, and the user's values are not.  Restoring
     * them after the mret locks the CPU up on the first interrupt in the gap. */
    lw    gp, KF_GP(sp)
    lw    tp, KF_TP(sp)

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    /* sp is the task's again and its bounds were never changed. */
    ESP_HW_STACK_GUARD_MONITOR_START_CUR_CORE t0 t1
#endif

    la    t0, .Lkresume
    csrw  mepc, t0

    lw    t1, UCTX_K_MSTATUS(a0)

    /* mcause: back to M with the kernel's MIE and CLIC level.  The interrupt bit
     * is kept from the trap: measured on the S31, mret only restores mil from
     * mpil when it is set, so clearing it after an interrupt taken in U-mode
     * leaves the core at that interrupt's level and the tick never fires again. */
    lw    t0, UCTX_MCAUSE(a0)
    li    t2, MCAUSE_INTERRUPT
    and   t0, t0, t2
    li    t2, MCAUSE_MPP_M
    or    t0, t0, t2
    andi  t2, t1, MSTATUS_MIE
    slli  t2, t2, 24             /* MIE (bit 3) -> mcause.mpie (bit 27) */
    or    t0, t0, t2
    lw    t2, UCTX_K_MINTSTATUS(a0)
    srli  t2, t2, 8              /* mil [31:24] -> mpil [23:16] */
    li    t3, MCAUSE_MPIL_MASK
    and   t2, t2, t3
    or    t0, t0, t2
    csrw  mcause, t0

    /* mstatus last.  FS and VS return to the kernel's values: they were off for
     * the whole window, so the user cannot have changed that state. */
    csrr  t3, mstatus
    li    t0, ~(MSTATUS_MPP | MSTATUS_MPIE | MSTATUS_MIE | MSTATUS_FS | MSTATUS_VS | MSTATUS_TW)
    and   t0, t3, t0
    li    t2, MSTATUS_MPP
    or    t0, t0, t2
    andi  t2, t1, MSTATUS_MIE
    slli  t2, t2, 4              /* MIE (bit 3) -> MPIE (bit 7) */
    or    t0, t0, t2
    li    t2, (MSTATUS_FS | MSTATUS_VS)
    and   t2, t1, t2
    or    t0, t0, t2
    csrw  mstatus, t0
    mret

.Lkresume:
    lw    a0, UCTX_MCAUSE(a0)
    lw    ra, KF_RA(sp)
    lw    s0, KF_S0(sp)
    lw    s1, KF_S1(sp)
    lw    s2, KF_S2(sp)
    lw    s3, KF_S3(sp)
    lw    s4, KF_S4(sp)
    lw    s5, KF_S5(sp)
    lw    s6, KF_S6(sp)
    lw    s7, KF_S7(sp)
    lw    s8, KF_S8(sp)
    lw    s9, KF_S9(sp)
    lw    s10, KF_S10(sp)
    lw    s11, KF_S11(sp)
    addi  sp, sp, KFRAME_SIZE
    ret
    .size umode_trap_entry, . - umode_trap_entry

/* uint32_t umode_read_ucsr(uint32_t idx) */
    .balign 4
    .global umode_read_ucsr
    .type umode_read_ucsr, @function
umode_read_ucsr:
    slli  t0, a0, 3
    la    t1, 3f
    add   t0, t0, t1
    jr    t0
3:
    csrr  a0, 0x000
    ret
    csrr  a0, 0x005
    ret
    csrr  a0, 0x007
    ret
    csrr  a0, 0x017
    ret
    csrr  a0, 0x040
    ret
    csrr  a0, 0x041
    ret
    csrr  a0, 0x042
    ret
    csrr  a0, 0x047
    ret
    csrr  a0, 0x803
    ret
    csrr  a0, 0x805
    ret
    .size umode_read_ucsr, . - umode_read_ucsr

    .option pop

/* The window's CLIC vector table: any interrupt is just another way back. */
    .section .iram1.umode_mtvt, "a", @progbits
    .balign 256
    .global umode_mtvt_table
umode_mtvt_table:
    .rept 64
    .word umode_trap_entry
    .endr
    .size umode_mtvt_table, . - umode_mtvt_table
