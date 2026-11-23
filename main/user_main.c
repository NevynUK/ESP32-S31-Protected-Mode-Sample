/*
 * user_main.c -- the user application.  Runs in U-mode, in its own address space.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This code executes at U_TEXT_VA, not at the address it was linked at, and that
 * page holds nothing but this file.  So:
 *
 *  - no library calls, because nothing else is mapped;
 *  - no string literals or other static data, because .rodata is not mapped.
 *    The kernel puts the strings in a read-only page and passes a pointer;
 *  - no absolute references to anything in this file.  Direct calls are
 *    PC-relative and are fine.  CMakeLists.txt sets the compiler options.
 *
 * `ecall` is the only way out.
 */

#include <stdint.h>

#include "syscall.h"

#define USER_TICK_MS 200
#define USER_MSG_PERIOD 6
#define SYSCALL_PERIOD 9
#define USER_BURST 2000 /* far higher starves the idle tasks into a task watchdog warning */
#define USER_STAGGER_MS (USER_TICK_MS / 3)
#define USER_MSG_MAX 48

static inline __attribute__((always_inline)) uint32_t u_syscall2(uint32_t nr, uint32_t arg0, uint32_t arg1)
{
    register uint32_t a0 __asm__("a0") = arg0;
    register uint32_t a1 __asm__("a1") = arg1;
    register uint32_t a7 __asm__("a7") = nr;

    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

#define u_syscall1(nr, arg0) u_syscall2((nr), (arg0), 0)
#define u_syscall0(nr) u_syscall2((nr), 0, 0)

static __attribute__((noreturn)) void u_exit(uint32_t status)
{
    for (;;)
    {
        u_syscall1(SYS_EXIT, status);
    }
}

static uint32_t u_append(char *dst, uint32_t pos, const char *src)
{
    while (*src != '\0')
    {
        dst[pos++] = *src++;
    }
    return pos;
}

static uint32_t u_append_u32(char *dst, uint32_t pos, uint32_t val)
{
    char tmp[10];
    uint32_t n = 0;

    if (val == 0)
    {
        dst[pos++] = '0';
        return pos;
    }

    while (val != 0)
    {
        tmp[n++] = (char) ('0' + (val % 10));
        val /= 10;
    }

    while (n != 0)
    {
        dst[pos++] = tmp[--n];
    }
    return pos;
}

static void user_main(uint32_t id, const user_rodata_t *ro)
{
    char msg[USER_MSG_MAX];
    uint32_t tick = 0;
    uint32_t user_n = 1;
    uint32_t syscall_n = 1;
    uint32_t pos;

    u_syscall1(SYS_DELAY_MS, id * USER_STAGGER_MS);

    for (;;)
    {
        for (uint32_t i = 0; i < USER_BURST; i++)
        {
            u_syscall0(SYS_YIELD);
        }

        u_syscall1(SYS_DELAY_MS, USER_TICK_MS);
        tick++;

        if ((tick % USER_MSG_PERIOD) == 0)
        {
            pos = u_append(msg, 0, ro->user_prefix);
            pos = u_append_u32(msg, pos, id);
            msg[pos++] = '.';
            pos = u_append_u32(msg, pos, user_n++);
            pos = u_append(msg, pos, ro->core_infix);
            pos = u_append_u32(msg, pos, u_syscall0(SYS_GETCORE));
            msg[pos++] = '\n';
            u_syscall2(SYS_WRITE, (uint32_t) (uintptr_t) msg, pos);
        }

        if ((tick % SYSCALL_PERIOD) == 0)
        {
            pos = u_append(msg, 0, ro->syscall_prefix);
            pos = u_append_u32(msg, pos, id);
            msg[pos++] = '.';
            pos = u_append_u32(msg, pos, syscall_n++);
            msg[pos] = '\0';
            u_syscall1(SYS_PUTS, (uint32_t) (uintptr_t) msg);
        }
    }
}

void user_start(uint32_t id, const user_rodata_t *ro)
{
    user_main(id, ro);
    u_exit(0);
}

/* ------------------------------------------------------------------------- */
/* The isolation test's user side: one deliberately bad action per call.      */

static __attribute__((noinline)) uint32_t u_recurse(uint32_t n)
{
    volatile uint32_t pad[32];

    pad[0] = n;
    if (n == 0xffffffffu)
    {
        return pad[0];
    }
    return u_recurse(n + 1) + pad[0];
}

#define UCSR_OP(idx, num)                                                  \
    case idx:                                                              \
        if (write)                                                         \
        {                                                                  \
            __asm__ volatile("csrw " #num ", %0" : : "r"(val));            \
        }                                                                  \
        __asm__ volatile("csrr %0, " #num : "=r"(out));                    \
        break;

static uint32_t u_csr_access(uint32_t idx, uint32_t val, int write)
{
    uint32_t out = 0;

    switch (idx)
    {
        UCSR_OP(0, 0x000)
        UCSR_OP(1, 0x005)
        UCSR_OP(2, 0x007)
        UCSR_OP(3, 0x017)
        UCSR_OP(4, 0x040)
        UCSR_OP(5, 0x041)
        UCSR_OP(6, 0x042)
        UCSR_OP(7, 0x047)
        UCSR_OP(8, 0x803)
        UCSR_OP(9, 0x805)
    default:
        break;
    }
    return out;
}

void user_attack_start(uint32_t op, uint32_t arg, uint32_t val)
{
    uint32_t out = 0;

    switch (op)
    {
    case ATTACK_LOAD:
        out = *(volatile uint32_t *) (uintptr_t) arg;
        break;
    case ATTACK_STORE:
        *(volatile uint32_t *) (uintptr_t) arg = val;
        break;
    case ATTACK_EXEC:
        ((void (*)(void))(uintptr_t) arg)();
        break;
    case ATTACK_CSR_MSTATUS:
        __asm__ volatile("csrr %0, mstatus" : "=r"(out));
        break;
    case ATTACK_CSR_SATP:
        __asm__ volatile("csrr %0, satp" : "=r"(out));
        break;
    case ATTACK_MRET:
        __asm__ volatile(".word 0x30200073");
        break;
    case ATTACK_SRET:
        __asm__ volatile(".word 0x10200073");
        break;
    case ATTACK_SFENCE:
        __asm__ volatile(".word 0x12000073");
        break;
    case ATTACK_WFI:
        __asm__ volatile(".word 0x10500073");
        break;
    case ATTACK_EBREAK:
        __asm__ volatile("ebreak");
        break;
    case ATTACK_FPU:
        __asm__ volatile(".word 0xe0050553" : : : "a0"); /* fmv.x.w a0, fa0 */
        break;
    case ATTACK_PIE:
        __asm__ volatile(".word 0x0000101b"); /* esp.zero.q q0 */
        break;
    case ATTACK_STACK_OVERFLOW:
        out = u_recurse(0);
        break;
    case ATTACK_SYS_WRITE:
        out = u_syscall2(SYS_WRITE, arg, val);
        break;
    case ATTACK_SYS_PUTS:
        out = u_syscall1(SYS_PUTS, arg);
        break;
    case ATTACK_UCSR_READ:
        out = u_csr_access(arg, 0, 0);
        break;
    case ATTACK_UCSR_WRITE:
        out = u_csr_access(arg, val, 1);
        break;
    case ATTACK_SPIN:
        for (;;)
        {
            __asm__ volatile("nop");
        }
    default:
        break;
    }

    u_exit(out);
}
