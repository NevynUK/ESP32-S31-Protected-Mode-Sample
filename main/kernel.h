/*
 * kernel.h -- state shared between the kernel and its isolation test.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "umode.h"

#define USER_SLOTS 8

#include "vm.h"

typedef struct
{
    const char *name;
    uint32_t id;
    int core;
    uint32_t migrations;
    vm_space_t vm;
    umode_ctx_t ctx;
    volatile bool running;
    bool quiet; /* the isolation test expects rejected syscalls */
    uint32_t syscalls;
    uint32_t irqs_in_user;
    bool umode_confirmed;
} user_slot_t;

extern user_slot_t g_slots[USER_SLOTS];

/* The reset values of the per-context CSRs, as measured on the S31. */
extern const uint32_t g_ucsr_defaults[UCSR_COUNT];

/* Bits of each per-context CSR that hold state between windows, as measured. */
extern const uint32_t g_ucsr_masks[UCSR_COUNT];

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Point a slot's context at entry with up to three arguments. */
void user_ctx_setup(user_slot_t *u, uint32_t pc, uint32_t a0, uint32_t a1, uint32_t a2);

/* Service one ecall.  False means the user exited. */
bool syscall_dispatch(user_slot_t *u);

/* Run the isolation test on every core.  Returns the number of failures. */
unsigned isolation_test_run(void);
