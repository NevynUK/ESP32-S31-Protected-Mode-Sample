/*
 * kernel_main.c -- the M-mode kernel.
 *
 * SPDX-License-Identifier: MIT
 *
 * ESP-IDF and FreeRTOS run in machine mode as usual.  Each user application runs
 * in user mode in its own Sv32 address space, hosted by a FreeRTOS task: the task
 * calls umode_enter(), which returns on the user's next trap.  Blocking work is
 * done in the host task, so SYS_DELAY_MS is a real vTaskDelay().
 *
 * The kernel refuses to start any user until the isolation test has passed on
 * both cores.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kernel.h"
#include "syscall.h"

#define KERNEL_TASK_STACK 3072
#define USER_HOST_STACK 4096
#define KERNEL_TASK_PRIO 5
#define USER_HOST_PRIO 5
#define KERNEL_PERIOD_MS 1000
#define MIGRATION_REPORT_EVERY 3
#define SYS_MSG_MAX 128

_Static_assert(offsetof(umode_ctx_t, pc) == UCTX_PC, "pc moved");
_Static_assert(offsetof(umode_ctx_t, mcause) == UCTX_MCAUSE, "mcause moved");
_Static_assert(offsetof(umode_ctx_t, mtval) == UCTX_MTVAL, "mtval moved");
_Static_assert(offsetof(umode_ctx_t, k_sp) == UCTX_K_SP, "k_sp moved");
_Static_assert(offsetof(umode_ctx_t, k_mtvec) == UCTX_K_MTVEC, "k_mtvec moved");
_Static_assert(offsetof(umode_ctx_t, k_mtvt) == UCTX_K_MTVT, "k_mtvt moved");
_Static_assert(offsetof(umode_ctx_t, k_mscratch) == UCTX_K_MSCRATCH, "k_mscratch moved");
_Static_assert(offsetof(umode_ctx_t, k_mstatus) == UCTX_K_MSTATUS, "k_mstatus moved");
_Static_assert(offsetof(umode_ctx_t, k_mintstatus) == UCTX_K_MINTSTATUS, "k_mintstatus moved");
_Static_assert(offsetof(umode_ctx_t, k_hwlp) == UCTX_K_HWLP, "k_hwlp moved");
_Static_assert(offsetof(umode_ctx_t, k_pie) == UCTX_K_PIE, "k_pie moved");
_Static_assert(offsetof(umode_ctx_t, satp) == UCTX_SATP, "satp moved");
_Static_assert(offsetof(umode_ctx_t, u_csr) == UCTX_U_CSR, "u_csr moved");
_Static_assert(offsetof(umode_ctx_t, k_csr) == UCTX_K_CSR, "k_csr moved");
_Static_assert(sizeof(umode_ctx_t) == UCTX_SIZE, "umode_ctx_t size changed");

user_slot_t g_slots[USER_SLOTS];

/* In UCSR order: 0x000 0x005 0x007 0x017 0x040 0x041 0x042 0x047 0x803 0x805 */
const uint32_t g_ucsr_defaults[UCSR_COUNT] = {0x00000001, 0x00000003, 0, 0, 0, 0, 0, 0x0000000f, 0, 0};
const uint32_t g_ucsr_masks[UCSR_COUNT] = {0x00000011, 0xffffffc0, 0xffffffc0, 0xffffffc0, 0xffffffff,
                                           0xfffffffe, 0x88ff0fff, 0x000000f0, 0x000000ff, 0x000000ff};

static char s_slot_names[USER_SLOTS][8];
static SemaphoreHandle_t s_console_mux;

void kprintf(const char *fmt, ...)
{
    va_list ap;

    xSemaphoreTake(s_console_mux, portMAX_DELAY);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    xSemaphoreGive(s_console_mux);
}

static void kwrite(const char *buf, uint32_t len)
{
    xSemaphoreTake(s_console_mux, portMAX_DELAY);
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    xSemaphoreGive(s_console_mux);
}

void user_ctx_setup(user_slot_t *u, uint32_t pc, uint32_t a0, uint32_t a1, uint32_t a2)
{
    umode_ctx_t *ctx = &u->ctx;

    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->u_csr, g_ucsr_defaults, sizeof(ctx->u_csr));
    ctx->satp = u->vm.satp;
    ctx->pc = pc;
    ctx->x[2] = U_STACK_TOP;
    ctx->x[10] = a0;
    ctx->x[11] = a1;
    ctx->x[12] = a2;
}

bool syscall_dispatch(user_slot_t *u)
{
    umode_ctx_t *ctx = &u->ctx;
    const uint32_t nr = ctx->x[17];
    const uint32_t arg0 = ctx->x[10];
    const uint32_t arg1 = ctx->x[11];
    uint32_t ret = 0;

    u->syscalls++;

    if (!u->umode_confirmed && !u->quiet)
    {
        u->umode_confirmed = true;
        kprintf("KERNEL: %s: first syscall, mcause=%08lx (ECALL from U-mode) on core %d, satp=%08lx\n", u->name, (unsigned long) ctx->mcause, xPortGetCoreID(), (unsigned long) ctx->satp);
    }

    switch (nr)
    {
    case SYS_NOP:
        break;

    case SYS_WRITE:
    {
        char buf[SYS_MSG_MAX];

        if (arg1 > sizeof(buf) || !vm_copy_from_user(&u->vm, buf, arg0, arg1))
        {
            if (!u->quiet)
            {
                kprintf("KERNEL: %s: SYS_WRITE rejected: buf=%08lx len=%lu\n", u->name, (unsigned long) arg0, (unsigned long) arg1);
            }
            ret = SYS_ERR_FAULT;
            break;
        }
        if (!u->quiet)
        {
            kwrite(buf, arg1);
        }
        ret = arg1;
        break;
    }

    case SYS_PUTS:
    {
        char msg[SYS_MSG_MAX];

        if (vm_strncpy_from_user(&u->vm, msg, arg0, sizeof(msg)) < 0)
        {
            if (!u->quiet)
            {
                kprintf("KERNEL: %s: SYS_PUTS rejected: msg=%08lx\n", u->name, (unsigned long) arg0);
            }
            ret = SYS_ERR_FAULT;
            break;
        }
        if (!u->quiet)
        {
            kprintf("%s\n", msg);
        }
        break;
    }

    case SYS_DELAY_MS:
        vTaskDelay(pdMS_TO_TICKS(arg0));
        break;

    case SYS_YIELD:
        taskYIELD();
        break;

    case SYS_GETCORE:
        ret = (uint32_t) xPortGetCoreID();
        break;

    case SYS_EXIT:
        if (!u->quiet)
        {
            kprintf("KERNEL: %s: exited, status %lu\n", u->name, (unsigned long) arg0);
        }
        return false;

    default:
        ret = SYS_ERR_BADNR;
        break;
    }

    ctx->pc += 4;
    ctx->x[10] = ret;
    return true;
}

static void report_fault(const user_slot_t *u)
{
    const umode_ctx_t *ctx = &u->ctx;

    kprintf("KERNEL: %s: user fault: mcause=%08lx mtval=%08lx pc=%08lx sp=%08lx\n", u->name, (unsigned long) ctx->mcause, (unsigned long) ctx->mtval, (unsigned long) ctx->pc, (unsigned long) ctx->x[2]);
}

static void kernel_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    uint32_t n = 1;

    (void) arg;

    for (;;)
    {
        kprintf("Kernel %lu (core %d)\n", (unsigned long) n++, xPortGetCoreID());

        if ((n % MIGRATION_REPORT_EVERY) == 0)
        {
            char line[USER_SLOTS * 24 + 1];
            size_t off = 0;

            for (int i = 0; i < USER_SLOTS; i++)
            {
                off += (size_t) snprintf(line + off, sizeof(line) - off, " u%d:c%d/%lu", i, g_slots[i].core, (unsigned long) g_slots[i].migrations);
            }
            kprintf("KERNEL: window:core/moves%s\n", line);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(KERNEL_PERIOD_MS));
    }
}

static void user_host_task(void *arg)
{
    user_slot_t *u = arg;

    u->core = xPortGetCoreID();
    u->running = true;

    while (u->running)
    {
        const uint32_t cause = umode_enter(&u->ctx);
        const int core = xPortGetCoreID();

        if (core != u->core)
        {
            u->core = core;
            u->migrations++;
        }

        if (cause & MCAUSE_INTERRUPT)
        {
            u->irqs_in_user++;
        }
        else if ((cause & MCAUSE_EXCCODE_MASK) == EXC_ECALL_U)
        {
            u->running = syscall_dispatch(u);
        }
        else
        {
            report_fault(u);
            u->running = false;
        }
    }

    kprintf("KERNEL: %s: retired after %lu syscalls, %lu core changes\n", u->name, (unsigned long) u->syscalls, (unsigned long) u->migrations);
    vTaskDelete(NULL);
}

void app_main(void)
{
    s_console_mux = xSemaphoreCreateMutex();
    configASSERT(s_console_mux != NULL);

    kprintf("\n=== ESP32-S31 protected mode: M-mode kernel, %d isolated U-mode users ===\n", USER_SLOTS);

    for (int i = 0; i < USER_SLOTS; i++)
    {
        user_slot_t *u = &g_slots[i];

        snprintf(s_slot_names[i], sizeof(s_slot_names[i]), "user%d", i);
        u->name = s_slot_names[i];
        u->id = (uint32_t) i;
        vm_space_init(&u->vm, (unsigned) i);

        user_rodata_t *ro = (user_rodata_t *) u->vm.ro;

        strncpy(ro->user_prefix, "User ", sizeof(ro->user_prefix) - 1);
        strncpy(ro->core_infix, " on core ", sizeof(ro->core_infix) - 1);
        strncpy(ro->syscall_prefix, "syscall ", sizeof(ro->syscall_prefix) - 1);
        kprintf("KERNEL: %s: asid %lu, satp=%08lx, data page at physical %08lx, virtual %08lx\n", u->name, (unsigned long) u->vm.asid, (unsigned long) u->vm.satp, (unsigned long) (uintptr_t) u->vm.data, (unsigned long) U_DATA_VA);
    }

    const unsigned failures = isolation_test_run();

    if (failures != 0)
    {
        kprintf("KERNEL: REFUSING TO START USERS: %u isolation checks failed\n", failures);
        return;
    }

    for (int i = 0; i < USER_SLOTS; i++)
    {
        user_slot_t *u = &g_slots[i];

        vm_space_clear(&u->vm);
        u->syscalls = 0;
        user_ctx_setup(u, vm_user_text_va(user_start), u->id, U_RO_VA, 0);
    }

    xTaskCreatePinnedToCore(kernel_task, "kernel", KERNEL_TASK_STACK, NULL, KERNEL_TASK_PRIO, NULL, tskNO_AFFINITY);

    for (int i = 0; i < USER_SLOTS; i++)
    {
        xTaskCreatePinnedToCore(user_host_task, g_slots[i].name, USER_HOST_STACK, &g_slots[i], USER_HOST_PRIO, NULL, tskNO_AFFINITY);
    }
}
