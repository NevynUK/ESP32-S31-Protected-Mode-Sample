/*
 * syscall.h -- the kernel/user ABI.
 *
 * SPDX-License-Identifier: MIT
 *
 *   a7      syscall number
 *   a0..a1  arguments
 *   a0      return value
 *
 * Every pointer is a USER VIRTUAL address.  The kernel translates it through the
 * caller's own page tables, so a user cannot name anyone else's memory.
 */

#pragma once

#include <stdint.h>

#define SYS_NOP 0      /* no-op; round-trips the trap path             */
#define SYS_WRITE 1    /* a0 = buffer, a1 = length -> bytes written    */
#define SYS_PUTS 2     /* a0 = NUL-terminated message -> 0             */
#define SYS_DELAY_MS 3 /* a0 = milliseconds -> 0                       */
#define SYS_EXIT 4     /* a0 = status; does not return                 */
#define SYS_GETCORE 5  /* -> the core this window is running on        */
#define SYS_YIELD 6    /* give up the core without blocking -> 0       */

#define SYS_ERR_BADNR ((uint32_t) -1)
#define SYS_ERR_FAULT ((uint32_t) -2) /* pointer not valid in the caller's space */

/* The address space every user sees.  All of it sits in one 4 MiB region, so one
 * second-level page table per user covers it. */
#define U_TEXT_VA 0x60000000u
#define U_RO_VA 0x60100000u
#define U_DATA_VA 0x60200000u
#define U_GUARD_VA 0x602FF000u /* never mapped: catches a stack overflow */
#define U_STACK_VA 0x60300000u
#define U_STACK_TOP 0x60301000u

/* Operations for the isolation test's user-side entry point. */
#define ATTACK_LOAD 0
#define ATTACK_STORE 1
#define ATTACK_EXEC 2
#define ATTACK_CSR_MSTATUS 3
#define ATTACK_CSR_SATP 4
#define ATTACK_MRET 5
#define ATTACK_SRET 6
#define ATTACK_SFENCE 7
#define ATTACK_WFI 8
#define ATTACK_EBREAK 9
#define ATTACK_FPU 10
#define ATTACK_PIE 11
#define ATTACK_STACK_OVERFLOW 12
#define ATTACK_SYS_WRITE 13
#define ATTACK_SYS_PUTS 14
#define ATTACK_UCSR_READ 15
#define ATTACK_UCSR_WRITE 16
#define ATTACK_SPIN 17

typedef struct
{
    char user_prefix[8];     /* "User "     */
    char core_infix[12];     /* " on core " */
    char syscall_prefix[12]; /* "syscall "  */
} user_rodata_t;

/* User entry points.  The kernel never calls these: it loads their address,
 * rebased to U_TEXT_VA, into a context's pc. */
void user_start(uint32_t id, const user_rodata_t *ro);
void user_attack_start(uint32_t op, uint32_t arg, uint32_t val);
