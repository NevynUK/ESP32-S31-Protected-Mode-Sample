/*
 * isolation_test.c -- attack the isolation from user mode, on every core.
 *
 * SPDX-License-Identifier: MIT
 *
 * Each check runs one deliberately bad action in a real U-mode window and
 * compares what the hardware did with what isolation requires.  The kernel will
 * not start the user applications unless every check passes on both cores.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kernel.h"
#include "soc/soc.h"
#include "syscall.h"

#define OUTCOME_EXIT 0xffffu /* the action completed and the user exited */
#define OUTCOME_PREEMPTED 0xfffeu /* still running after MAX_WINDOWS interrupts */
#define MAX_WINDOWS 16

typedef struct
{
    uint32_t outcome; /* exception code, or OUTCOME_EXIT */
    uint32_t value;   /* a0 passed to SYS_EXIT */
    uint32_t tval;
} result_t;

static unsigned s_pass, s_fail;
static SemaphoreHandle_t s_done;

static result_t run(user_slot_t *u, uint32_t op, uint32_t arg, uint32_t val)
{
    result_t r = {.outcome = OUTCOME_PREEMPTED};

    user_ctx_setup(u, vm_user_text_va(user_attack_start), op, arg, val);

    for (unsigned w = 0; w < MAX_WINDOWS; w++)
    {
        const uint32_t cause = umode_enter(&u->ctx);

        if (cause & MCAUSE_INTERRUPT)
        {
            continue;
        }

        const uint32_t code = cause & MCAUSE_EXCCODE_MASK;

        if (code != EXC_ECALL_U)
        {
            r.outcome = code;
            r.tval = u->ctx.mtval;
            return r;
        }

        const uint32_t a0 = u->ctx.x[10];

        if (!syscall_dispatch(u))
        {
            r.outcome = OUTCOME_EXIT;
            r.value = a0;
            return r;
        }
    }
    return r;
}

static void check(const char *what, bool ok, const result_t *r)
{
    if (ok)
    {
        s_pass++;
        return;
    }
    s_fail++;
    kprintf("ISOLATION[%d]: FAIL %s: outcome=%lu value=%08lx mtval=%08lx\n", xPortGetCoreID(), what, (unsigned long) r->outcome, (unsigned long) r->value, (unsigned long) r->tval);
}

static void expect_fault(user_slot_t *u, const char *what, uint32_t op, uint32_t arg, uint32_t val, uint32_t code)
{
    const result_t r = run(u, op, arg, val);

    check(what, r.outcome == code, &r);
}

static void expect_exit_value(user_slot_t *u, const char *what, uint32_t op, uint32_t arg, uint32_t val, uint32_t want)
{
    const result_t r = run(u, op, arg, val);

    check(what, r.outcome == OUTCOME_EXIT && r.value == want, &r);
}

static void test_memory(user_slot_t *a, user_slot_t *b)
{
    const uint32_t kernel_data = (uint32_t) (uintptr_t) &g_slots[0];
    const uint32_t other_data = (uint32_t) (uintptr_t) b->vm.data;
    const uint32_t tables = (uint32_t) (uintptr_t) a->vm.root;

    expect_fault(a, "read kernel data", ATTACK_LOAD, kernel_data, 0, EXC_LOAD_PAGE);
    expect_fault(a, "write kernel data", ATTACK_STORE, kernel_data, 1, EXC_STORE_PAGE);
    expect_fault(a, "read another user's page by physical address", ATTACK_LOAD, other_data, 0, EXC_LOAD_PAGE);
    expect_fault(a, "write another user's page by physical address", ATTACK_STORE, other_data, 1, EXC_STORE_PAGE);
    expect_fault(a, "read own page tables", ATTACK_LOAD, tables, 0, EXC_LOAD_PAGE);
    expect_fault(a, "write own page tables", ATTACK_STORE, tables, 0, EXC_STORE_PAGE);
    expect_fault(a, "read a peripheral", ATTACK_LOAD, SOC_PERIPHERAL_LOW, 0, EXC_LOAD_PAGE);
    expect_fault(a, "write a peripheral", ATTACK_STORE, SOC_PERIPHERAL_LOW, 0, EXC_STORE_PAGE);
    expect_fault(a, "read flash", ATTACK_LOAD, SOC_IROM_LOW, 0, EXC_LOAD_PAGE);
    expect_fault(a, "read ROM", ATTACK_LOAD, SOC_IROM_MASK_LOW, 0, EXC_LOAD_PAGE);
    expect_fault(a, "write own text", ATTACK_STORE, U_TEXT_VA, 0, EXC_STORE_PAGE);
    expect_fault(a, "write own read-only page", ATTACK_STORE, U_RO_VA, 0, EXC_STORE_PAGE);
    expect_fault(a, "execute own stack", ATTACK_EXEC, U_STACK_VA, 0, EXC_INSN_PAGE);
    expect_fault(a, "execute own data", ATTACK_EXEC, U_DATA_VA, 0, EXC_INSN_PAGE);
    expect_fault(a, "execute kernel code", ATTACK_EXEC, (uint32_t) (uintptr_t) syscall_dispatch, 0, EXC_INSN_PAGE);
    expect_fault(a, "read guard page", ATTACK_LOAD, U_GUARD_VA, 0, EXC_LOAD_PAGE);
    expect_fault(a, "overflow own stack", ATTACK_STACK_OVERFLOW, 0, 0, EXC_STORE_PAGE);
}

static void test_privilege(user_slot_t *a)
{
    expect_fault(a, "read mstatus", ATTACK_CSR_MSTATUS, 0, 0, EXC_ILLEGAL_INSN);
    expect_fault(a, "read satp", ATTACK_CSR_SATP, 0, 0, EXC_ILLEGAL_INSN);
    expect_fault(a, "mret", ATTACK_MRET, 0, 0, EXC_ILLEGAL_INSN);
    expect_fault(a, "sret", ATTACK_SRET, 0, 0, EXC_ILLEGAL_INSN);
    expect_fault(a, "sfence.vma", ATTACK_SFENCE, 0, 0, EXC_ILLEGAL_INSN);
    expect_fault(a, "wfi", ATTACK_WFI, 0, 0, EXC_ILLEGAL_INSN);
    expect_fault(a, "ebreak", ATTACK_EBREAK, 0, 0, EXC_BREAKPOINT);
    expect_fault(a, "FPU instruction", ATTACK_FPU, 0, 0, EXC_ILLEGAL_INSN);
    expect_fault(a, "PIE instruction", ATTACK_PIE, 0, 0, EXC_ILLEGAL_INSN);
    expect_fault(a, "an infinite loop is preempted", ATTACK_SPIN, 0, 0, OUTCOME_PREEMPTED);
}

static void test_syscall_pointers(user_slot_t *a, user_slot_t *b)
{
    const uint32_t kernel_data = (uint32_t) (uintptr_t) &g_slots[0];
    const uint32_t other_data = (uint32_t) (uintptr_t) b->vm.data;

    a->quiet = true;
    expect_exit_value(a, "SYS_WRITE of kernel memory", ATTACK_SYS_WRITE, kernel_data, 16, SYS_ERR_FAULT);
    expect_exit_value(a, "SYS_WRITE of another user's page", ATTACK_SYS_WRITE, other_data, 16, SYS_ERR_FAULT);
    expect_exit_value(a, "SYS_WRITE of an unmapped address", ATTACK_SYS_WRITE, U_GUARD_VA, 16, SYS_ERR_FAULT);
    expect_exit_value(a, "SYS_WRITE wrapping the address space", ATTACK_SYS_WRITE, 0xfffffff0u, 32, SYS_ERR_FAULT);
    expect_exit_value(a, "SYS_WRITE running off its page", ATTACK_SYS_WRITE, U_DATA_VA + VM_PAGE_SIZE - 8, 16, SYS_ERR_FAULT);
    expect_exit_value(a, "SYS_WRITE longer than the kernel buffer", ATTACK_SYS_WRITE, U_DATA_VA, 4096, SYS_ERR_FAULT);
    expect_exit_value(a, "SYS_PUTS of kernel memory", ATTACK_SYS_PUTS, kernel_data, 0, SYS_ERR_FAULT);
    expect_exit_value(a, "SYS_PUTS of an unmapped address", ATTACK_SYS_PUTS, U_GUARD_VA, 0, SYS_ERR_FAULT);

    vm_space_clear(&a->vm);
    memset(a->vm.data + VM_PAGE_SIZE - 8, 'x', 8);
    expect_exit_value(a, "SYS_PUTS of an unterminated string at a page end", ATTACK_SYS_PUTS, U_DATA_VA + VM_PAGE_SIZE - 8, 0, SYS_ERR_FAULT);
    vm_space_clear(&a->vm);
}

static void test_cross_user(user_slot_t *a, user_slot_t *b)
{
    const uint32_t secret = 0x5ec2e700u + (uint32_t) xPortGetCoreID();

    vm_space_clear(&a->vm);
    vm_space_clear(&b->vm);

    expect_exit_value(a, "store a secret at U_DATA_VA", ATTACK_STORE, U_DATA_VA, secret, 0);
    expect_exit_value(b, "same address in another space does not see it", ATTACK_LOAD, U_DATA_VA, 0, 0);
    expect_exit_value(a, "the owner still sees it", ATTACK_LOAD, U_DATA_VA, 0, secret);

    const result_t none = {0};

    check("the secret reached the right physical page", *(uint32_t *) a->vm.data == secret && *(uint32_t *) b->vm.data == 0, &none);
    vm_space_clear(&a->vm);
}

/* Every bit of every CSR that survives between windows: set it in one user and
 * look for it in another. */
static void test_csr_channels(user_slot_t *a, user_slot_t *b)
{
    uint32_t kernel_before[UCSR_COUNT];

    for (unsigned i = 0; i < UCSR_COUNT; i++)
    {
        kernel_before[i] = umode_read_ucsr(i);
    }

    for (unsigned i = 0; i < UCSR_COUNT; i++)
    {
        const uint32_t def = g_ucsr_defaults[i];
        bool sealed = true;
        result_t last = {0};

        for (unsigned bit = 0; bit < 32; bit++)
        {
            if (!(g_ucsr_masks[i] & (1u << bit)))
            {
                continue;
            }

            const result_t w = run(a, ATTACK_UCSR_WRITE, i, def ^ (1u << bit));

            last = run(b, ATTACK_UCSR_READ, i, 0);
            if (w.outcome != OUTCOME_EXIT || last.outcome != OUTCOME_EXIT || ((last.value ^ def) & g_ucsr_masks[i]) != 0)
            {
                sealed = false;
                break;
            }
        }

        char name[40];

        snprintf(name, sizeof(name), "CSR channel, index %u", i);
        check(name, sealed, &last);

        const result_t none = {.value = umode_read_ucsr(i)};

        snprintf(name, sizeof(name), "kernel value of CSR index %u", i);
        check(name, umode_read_ucsr(i) == kernel_before[i], &none);
    }
}

static void test_task(void *arg)
{
    user_slot_t *a = &g_slots[0];
    user_slot_t *b = &g_slots[1];
    const unsigned pass_before = s_pass, fail_before = s_fail;

    (void) arg;
    a->quiet = true;
    b->quiet = true;

    test_memory(a, b);
    test_privilege(a);
    test_syscall_pointers(a, b);
    test_cross_user(a, b);
    test_csr_channels(a, b);

    a->quiet = false;
    b->quiet = false;

    kprintf("ISOLATION[%d]: %u passed, %u failed\n", xPortGetCoreID(), s_pass - pass_before, s_fail - fail_before);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

unsigned isolation_test_run(void)
{
    s_done = xSemaphoreCreateBinary();
    configASSERT(s_done != NULL);

    for (int core = 0; core < 2; core++)
    {
        xTaskCreatePinnedToCore(test_task, "isolation", 6144, NULL, 6, NULL, core);
        xSemaphoreTake(s_done, portMAX_DELAY);
    }

    vSemaphoreDelete(s_done);
    kprintf("ISOLATION: %u checks across both cores, %u failed\n", s_pass + s_fail, s_fail);
    return s_fail;
}
