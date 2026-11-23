/*
 * umode.h -- the kernel's interface to the U-mode window.
 *
 * SPDX-License-Identifier: MIT
 *
 * The context block is shared with umode.S, so its offsets are spelled out here
 * and checked against the struct with _Static_asserts in kernel_main.c.
 */

#pragma once

#define UCTX_X(n) ((n) * 4)
#define UCTX_PC 128
#define UCTX_MCAUSE 132
#define UCTX_MTVAL 136

/* Kernel state parked across the window */
#define UCTX_K_SP 140
#define UCTX_K_MTVEC 144
#define UCTX_K_MTVT 148
#define UCTX_K_MSCRATCH 152
#define UCTX_K_MSTATUS 156
#define UCTX_K_MINTSTATUS 160
#define UCTX_K_HWLP 164
#define UCTX_K_PIE 168

/* This context's address space */
#define UCTX_SATP 172

/* The user-reachable CSRs that hold state between windows, swapped per context */
#define UCSR_COUNT 10
#define UCTX_U_CSR 176
#define UCTX_K_CSR 216
#define UCTX_SIZE 256

#define CSR_MTVT 0x307
#define CSR_MINTSTATUS 0xFB1
#define CSR_HWLP_STATE 0x7F1
#define CSR_PIE_STATE 0x7F2 /* core 1 only */

#define MSTATUS_MIE 0x00000008
#define MSTATUS_MPIE 0x00000080
#define MSTATUS_VS 0x00000600
#define MSTATUS_MPP 0x00001800
#define MSTATUS_FS 0x00006000
#define MSTATUS_MPRV 0x00020000
#define MSTATUS_SUM 0x00040000
#define MSTATUS_MXR 0x00080000
#define MSTATUS_TW 0x00200000

#define MCAUSE_INTERRUPT 0x80000000
#define MCAUSE_MPP_M 0x30000000
#define MCAUSE_MPIL_MASK 0x00ff0000
#define MCAUSE_EXCCODE_MASK 0x00000fff

#define EXC_INSN_ACCESS 1
#define EXC_ILLEGAL_INSN 2
#define EXC_BREAKPOINT 3
#define EXC_LOAD_ACCESS 5
#define EXC_STORE_ACCESS 7
#define EXC_ECALL_U 8
#define EXC_INSN_PAGE 12
#define EXC_LOAD_PAGE 13
#define EXC_STORE_PAGE 15

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct
{
    uint32_t x[32]; /* x[0] unused */
    uint32_t pc;
    uint32_t mcause;
    uint32_t mtval;

    uint32_t k_sp;
    uint32_t k_mtvec;
    uint32_t k_mtvt;
    uint32_t k_mscratch;
    uint32_t k_mstatus;
    uint32_t k_mintstatus;
    uint32_t k_hwlp;
    uint32_t k_pie;

    uint32_t satp;

    uint32_t u_csr[UCSR_COUNT];
    uint32_t k_csr[UCSR_COUNT];
} umode_ctx_t;

/* Run ctx in U-mode until it traps; returns ctx->mcause.  Call from a task. */
uint32_t umode_enter(umode_ctx_t *ctx);

/* Read one of the UCSR_COUNT per-context CSRs on this hart, from M-mode. */
uint32_t umode_read_ucsr(uint32_t idx);

#endif /* __ASSEMBLER__ */
