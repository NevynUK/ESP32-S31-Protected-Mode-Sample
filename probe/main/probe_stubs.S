/*
 * probe_stubs.S -- enter a privilege mode, run one stub, come back on its trap.
 *
 * SPDX-License-Identifier: MIT
 *
 * priv_call() masks interrupts, installs a private trap vector and mrets into
 * the requested mode.  Every stub ends in a trap (normally ecall), and the
 * vector records it and returns to the caller in M-mode.  So an instruction the
 * silicon does not implement is a recorded result, not a crash.
 *
 * Rules the code below depends on:
 *
 *  - mstatus.MIE does not mask M-mode interrupts while in S-mode or U-mode, so
 *    the CLIC threshold (mintthresh, a CSR on the S31) is raised for the window.
 *  - mcause.mpp aliases mstatus.MPP on the ESP32-P4's CLIC, and the S31 is
 *    treated the same way: mcause is written first and mstatus last.
 *  - mret restores the CLIC level from mcause.mpil, so both directions load it.
 *  - Stubs never touch sp, so the hardware stack guard stays armed and valid.
 *  - Stubs only use a0-a2 and t0-t1.  No callee-saved register changes.
 */

#include "probe.h"

#define CSR_MINTSTATUS 0xFB1
#define CSR_MINTTHRESH 0x347

#define MSTATUS_MIE 0x00000008
#define MSTATUS_MPIE 0x00000080
#define MSTATUS_MPP 0x00001800
#define MSTATUS_WINDOW_BITS 0x000E0000 /* MPRV | SUM | MXR */

#define SAVE_SP 0
#define SAVE_RA 4
#define SAVE_MTVEC 8
#define SAVE_MSTATUS 12
#define SAVE_THRESH 16
#define SAVE_REQ 20
#define SAVE_MINTSTATUS 24

    .section .bss.s31probe, "aw", @nobits
    .balign 4
g_priv_save:
    .space 28

    .section .iram1.s31probe, "ax", @progbits
    .option push
    .option norvc

/* void priv_call(priv_req_t *r) */
    .balign 4
    .global priv_call
    .type priv_call, @function
priv_call:
    la    t0, g_priv_save
    sw    sp, SAVE_SP(t0)
    sw    ra, SAVE_RA(t0)
    sw    a0, SAVE_REQ(t0)

    csrr  t1, mstatus
    sw    t1, SAVE_MSTATUS(t0)
    csrci mstatus, MSTATUS_MIE

    li    t1, 0xff
    csrrw t1, CSR_MINTTHRESH, t1
    sw    t1, SAVE_THRESH(t0)
    csrr  t1, CSR_MINTSTATUS
    sw    t1, SAVE_MINTSTATUS(t0)

    csrr  t1, mtvec
    sw    t1, SAVE_MTVEC(t0)
    la    t1, priv_trap
    ori   t1, t1, 3
    csrw  mtvec, t1

    lw    t1, REQ_PC(a0)
    csrw  mepc, t1

    /* mcause first: its mpp field may alias mstatus.MPP. */
    lw    t1, SAVE_MINTSTATUS(t0)
    srli  t1, t1, 24
    slli  t1, t1, 16
    csrw  mcause, t1

    csrr  t1, mstatus
    li    t2, ~(MSTATUS_MPP | MSTATUS_MPIE | MSTATUS_MIE)
    and   t1, t1, t2
    lw    t2, REQ_MODE(a0)
    slli  t2, t2, 11
    or    t1, t1, t2
    lw    t2, REQ_MSTATUS_SET(a0)
    or    t1, t1, t2
    csrw  mstatus, t1

    lw    a2, REQ_A2(a0)
    lw    a1, REQ_A1(a0)
    lw    a0, REQ_A0(a0)
    mret
    .size priv_call, . - priv_call

/* The private vector.  CLIC mode enters exceptions at mtvec[31:6] << 6. */
    .balign 64
    .type priv_trap, @function
priv_trap:
    la    t0, g_priv_save
    lw    t1, SAVE_REQ(t0)
    csrr  t2, mcause
    sw    t2, REQ_CAUSE(t1)
    csrr  t2, mepc
    sw    t2, REQ_EPC(t1)
    csrr  t2, mtval
    sw    t2, REQ_TVAL(t1)
    csrr  t2, mstatus
    sw    t2, REQ_OUT_MSTATUS(t1)
    sw    a0, REQ_OUT_A0(t1)
    sw    a1, REQ_OUT_A1(t1)

    lw    sp, SAVE_SP(t0)
    lw    t2, SAVE_MTVEC(t0)
    csrw  mtvec, t2

    /* Leave through an mret so the CLIC level is restored from mcause.mpil. */
    la    t2, .Lpriv_back
    csrw  mepc, t2

    lw    t3, SAVE_MINTSTATUS(t0)
    srli  t3, t3, 24
    slli  t3, t3, 16
    lw    t4, SAVE_MSTATUS(t0)
    andi  t4, t4, MSTATUS_MIE
    slli  t5, t4, 24            /* MIE (bit 3) -> mcause.mpie (bit 27) */
    li    t2, 0x30000000        /* mcause.mpp = M */
    or    t2, t2, t3
    or    t2, t2, t5
    csrw  mcause, t2

    csrr  t2, mstatus
    li    t6, ~(MSTATUS_MPP | MSTATUS_MPIE | MSTATUS_MIE | MSTATUS_WINDOW_BITS)
    and   t2, t2, t6
    li    t6, MSTATUS_MPP
    or    t2, t2, t6
    slli  t4, t4, 4             /* MIE (bit 3) -> MPIE (bit 7) */
    or    t2, t2, t4
    csrw  mstatus, t2
    mret

.Lpriv_back:
    la    t0, g_priv_save
    lw    t1, SAVE_THRESH(t0)
    csrw  CSR_MINTTHRESH, t1
    lw    ra, SAVE_RA(t0)
    ret
    .size priv_trap, . - priv_trap

/* ------------------------------------------------------------------------- */
/* Stubs that Sv32 aliases: one page, so offsets hold in the alias.           */

    .balign 4096
    .global probe_page
probe_page:

    .global stub_ecall
stub_ecall:
    ecall

    .global stub_load
stub_load:
    lw    a0, 0(a0)
    ecall

    .global stub_store
stub_store:
    sw    a1, 0(a0)
    ecall

    .global stub_jump
stub_jump:
    jr    a0

    .global stub_read_mstatus
stub_read_mstatus:
    csrr  a0, mstatus
    ecall

    .global stub_read_s_csrs
stub_read_s_csrs:
    csrr  a0, 0x100             /* sstatus */
    csrr  a1, 0x180             /* satp */
    ecall

/* M-mode: does mstatus.MPP hold the value in a0?  WARL, so read it back. */
    .global stub_mpp_write
stub_mpp_write:
    csrr  t0, mstatus
    li    t1, ~MSTATUS_MPP
    and   t1, t0, t1
    slli  a0, a0, 11
    or    t1, t1, a0
    csrw  mstatus, t1
    csrr  a0, mstatus
    csrw  mstatus, t0
    ecall

/* S-mode: sret to U-mode at a0. */
    .global stub_sret_to_u
stub_sret_to_u:
    li    t0, 0x100             /* sstatus.SPP */
    csrc  0x100, t0
    csrw  0x141, a0             /* sepc */
    sret

/* S-mode: install a1 as stvec, then sret to U-mode at a0. */
    .global stub_deleg_to_u
stub_deleg_to_u:
    csrw  0x105, a1             /* stvec */
    li    t0, 0x100
    csrc  0x100, t0
    csrw  0x141, a0
    sret

/* S-mode trap handler for the delegation test: report to M-mode.  stvec is in
 * CLIC mode on the S31 (it reads back with mode 3), which enters at
 * stvec[31:6] << 6, so this has to be 64-byte aligned. */
    .balign 64
    .global stub_s_handler
stub_s_handler:
    csrr  a0, 0x142             /* scause */
    csrr  a1, 0x141             /* sepc */
    ecall

/* M-mode: satp = a0, returns the value read back. */
    .global stub_satp_set
stub_satp_set:
    csrw  0x180, a0
    csrr  a0, 0x180
    sfence.vma
    ecall

/* M-mode: medeleg = a0, returns the old value in a0, the new in a1. */
    .global stub_medeleg_swap
stub_medeleg_swap:
    csrrw a0, 0x302, a0
    csrr  a1, 0x302
    ecall

/* M-mode: pmpcfg0 = a0, returns the value read back. */
    .global stub_pmpcfg0_write
stub_pmpcfg0_write:
    csrw  0x3A0, a0
    csrr  a0, 0x3A0
    ecall

    .global probe_page_end
probe_page_end:

/* ------------------------------------------------------------------------- */
/* Fixed-size stub tables, indexed from C.                                    */

    .balign 4
    .global stub_csr_read
stub_csr_read:
#define X(num, name) csrr a0, num; ecall;
    PROBE_CSR_LIST(X)
#undef X

    .global stub_pmpcfg_read
stub_pmpcfg_read:
    .set  idx, 0
    .rept 16
    csrr  a0, 0x3A0 + idx
    ecall
    .set  idx, idx + 1
    .endr

    .global stub_pmpaddr_read
stub_pmpaddr_read:
    .set  idx, 0
    .rept 64
    csrr  a0, 0x3B0 + idx
    ecall
    .set  idx, idx + 1
    .endr

    .global stub_pmpaddr_write
stub_pmpaddr_write:
    .set  idx, 0
    .rept 64
    csrw  0x3B0 + idx, a0
    csrr  a0, 0x3B0 + idx
    ecall
    .set  idx, idx + 1
    .endr

/* Every CSR number whose privilege field is 00: what U-mode could possibly reach.
 * Index 0..1023 is base[index >> 8] + (index & 0xff). */
    .macro ucsr_table base
    .set  idx, 0
    .rept 256
    csrr  a0, \base + idx
    ecall
    .set  idx, idx + 1
    .endr
    .endm

    .macro ucsr_write_table base
    .set  idx, 0
    .rept 256
    csrw  \base + idx, a0
    csrr  a0, \base + idx
    ecall
    .set  idx, idx + 1
    .endr
    .endm

    .global stub_ucsr_read
stub_ucsr_read:
    ucsr_table 0x000
    ucsr_table 0x400
    ucsr_table 0x800
    ucsr_table 0xC00

    .global stub_ucsr_write
stub_ucsr_write:
    ucsr_write_table 0x000
    ucsr_write_table 0x400
    ucsr_write_table 0x800
    ucsr_write_table 0xC00

    .option pop
