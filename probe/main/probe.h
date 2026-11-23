/*
 * probe.h -- the interface between the C test sequence and the privilege stubs.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Supervisor and delegation CSRs whose existence is tested from M-mode. */
#define PROBE_CSR_LIST(X) \
    X(0x100, sstatus)     \
    X(0x104, sie)         \
    X(0x105, stvec)       \
    X(0x106, scounteren)  \
    X(0x140, sscratch)    \
    X(0x141, sepc)        \
    X(0x142, scause)      \
    X(0x143, stval)       \
    X(0x144, sip)         \
    X(0x180, satp)        \
    X(0x302, medeleg)     \
    X(0x303, mideleg)     \
    X(0x306, mcounteren)  \
    X(0x30A, menvcfg)     \
    X(0x747, mseccfg)

/* priv_req_t field offsets, used by probe_stubs.S. */
#define REQ_PC 0
#define REQ_MODE 4
#define REQ_A0 8
#define REQ_A1 12
#define REQ_A2 16
#define REQ_MSTATUS_SET 20
#define REQ_CAUSE 24
#define REQ_EPC 28
#define REQ_TVAL 32
#define REQ_OUT_A0 36
#define REQ_OUT_A1 40
#define REQ_OUT_MSTATUS 44

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct
{
    uint32_t pc;          /* where to enter */
    uint32_t mode;        /* 0 = U, 1 = S, 3 = M */
    uint32_t a0, a1, a2;  /* arguments on entry */
    uint32_t mstatus_set; /* extra mstatus bits for the window (SUM, ...) */

    uint32_t cause;       /* mcause of the trap that ended the window */
    uint32_t epc;         /* mepc */
    uint32_t tval;        /* mtval */
    uint32_t out_a0;      /* a0 at the trap */
    uint32_t out_a1;      /* a1 at the trap */
    uint32_t out_mstatus; /* mstatus at the trap; MPP is the mode that trapped */
} priv_req_t;

/* Enter r->mode at r->pc with interrupts masked; returns at the first trap. */
void priv_call(priv_req_t *r);

/* Every stub ends in a trap.  Those between probe_page and probe_page_end share
 * one 4 KiB page, so that Sv32 can alias them into a user-accessible mapping. */
extern const char probe_page[], probe_page_end[];
extern const char stub_ecall[], stub_load[], stub_store[], stub_jump[];
extern const char stub_read_mstatus[], stub_read_s_csrs[], stub_mpp_write[];
extern const char stub_sret_to_u[], stub_deleg_to_u[], stub_s_handler[];
extern const char stub_satp_set[], stub_medeleg_swap[], stub_pmpcfg0_write[];

/* Tables of fixed-size stubs. */
extern const char stub_csr_read[];     /* 8 bytes each, PROBE_CSR_LIST order */
extern const char stub_pmpcfg_read[];  /* 8 bytes each, pmpcfg0..15 */
extern const char stub_pmpaddr_read[]; /* 8 bytes each, pmpaddr0..63 */
extern const char stub_pmpaddr_write[]; /* 12 bytes each, pmpaddr0..63 */
extern const char stub_ucsr_read[];     /* 8 bytes each, 1024 user-level CSRs */
extern const char stub_ucsr_write[];    /* 12 bytes each, same order */

#endif /* __ASSEMBLER__ */
