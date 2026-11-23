/*
 * vm.c -- per-user Sv32 address spaces.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every user sees the same small layout at the same virtual addresses, backed by
 * its own physical pages:
 *
 *   U_TEXT_VA   R-X  the user image, shared by all users
 *   U_RO_VA     R--  one private page
 *   U_DATA_VA   RW-  one private page
 *   U_GUARD_VA  ---  unmapped
 *   U_STACK_VA  RW-  one private page
 *
 * Nothing else exists for that user: not the kernel, not its page tables, not a
 * peripheral, not another user.  The S31 does not set the accessed and dirty bits
 * in hardware (measured by probe/), so they are set here up front.
 */

#include "vm.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "kernel.h"
#include "syscall.h"

#define VPTE_V (1u << 0)
#define VPTE_R (1u << 1)
#define VPTE_W (1u << 2)
#define VPTE_X (1u << 3)
#define VPTE_U (1u << 4)
#define VPTE_A (1u << 6)
#define VPTE_D (1u << 7)
#define VPTE_LEAF (VPTE_R | VPTE_W | VPTE_X)
#define VPTE(pa, flags) ((((uint32_t) (uintptr_t) (pa) >> 12) << 10) | (flags))
#define VPTE_PA(pte) ((uintptr_t) (((pte) >> 10) << 12))

#define SATP_SV32 (1u << 31)
#define SATP_ASID_SHIFT 22

#define VM_USER_PAGES 3u
#define L2_INDEX(va) (((va) >> 12) & 0x3ffu)

typedef uint32_t vm_table_t[VM_PTES];

static __attribute__((aligned(VM_PAGE_SIZE))) vm_table_t s_tables[USER_SLOTS * 2];
static __attribute__((aligned(VM_PAGE_SIZE))) uint8_t s_pages[USER_SLOTS * VM_USER_PAGES][VM_PAGE_SIZE];

extern const char _user_text_start[], _user_text_end[];

void vm_space_init(vm_space_t *s, unsigned idx)
{
    const uintptr_t start = (uintptr_t) _user_text_start;
    const uintptr_t end = (uintptr_t) _user_text_end;

    /* If these were not page-aligned, the user's text mapping would expose
     * whatever kernel code shares its first or last page. */
    configASSERT(idx < USER_SLOTS);
    configASSERT((start & (VM_PAGE_SIZE - 1)) == 0);
    configASSERT((end & (VM_PAGE_SIZE - 1)) == 0);
    configASSERT(end > start && (end - start) / VM_PAGE_SIZE <= L2_INDEX(U_RO_VA));
    configASSERT((U_TEXT_VA >> 22) == ((U_STACK_TOP - 1) >> 22));

    s->root = s_tables[idx * 2];
    s->l2 = s_tables[idx * 2 + 1];
    s->ro = s_pages[idx * VM_USER_PAGES];
    s->data = s_pages[idx * VM_USER_PAGES + 1];
    s->stack = s_pages[idx * VM_USER_PAGES + 2];
    s->asid = idx + 1;
    s->satp = SATP_SV32 | (s->asid << SATP_ASID_SHIFT) | ((uint32_t) (uintptr_t) s->root >> 12);

    memset(s->root, 0, VM_PAGE_SIZE);
    memset(s->l2, 0, VM_PAGE_SIZE);
    memset(s_pages[idx * VM_USER_PAGES], 0, VM_USER_PAGES * VM_PAGE_SIZE);

    s->root[U_TEXT_VA >> 22] = VPTE(s->l2, VPTE_V);

    for (uintptr_t pa = start; pa < end; pa += VM_PAGE_SIZE)
    {
        s->l2[L2_INDEX(U_TEXT_VA) + (pa - start) / VM_PAGE_SIZE] = VPTE(pa, VPTE_V | VPTE_R | VPTE_X | VPTE_U | VPTE_A);
    }

    s->l2[L2_INDEX(U_RO_VA)] = VPTE(s->ro, VPTE_V | VPTE_R | VPTE_U | VPTE_A);
    s->l2[L2_INDEX(U_DATA_VA)] = VPTE(s->data, VPTE_V | VPTE_R | VPTE_W | VPTE_U | VPTE_A | VPTE_D);
    s->l2[L2_INDEX(U_STACK_VA)] = VPTE(s->stack, VPTE_V | VPTE_R | VPTE_W | VPTE_U | VPTE_A | VPTE_D);
}

void vm_space_clear(const vm_space_t *s)
{
    memset(s->data, 0, VM_PAGE_SIZE);
    memset(s->stack, 0, VM_PAGE_SIZE);
}

uint32_t vm_user_text_va(const void *fn)
{
    const uintptr_t pa = (uintptr_t) fn;

    configASSERT(pa >= (uintptr_t) _user_text_start && pa < (uintptr_t) _user_text_end);
    return U_TEXT_VA + (uint32_t) (pa - (uintptr_t) _user_text_start);
}

/* The kernel's view of a user pointer: walk that user's own tables. */
static const uint8_t *translate_read(const vm_space_t *s, uint32_t va)
{
    const uint32_t l1 = s->root[va >> 22];

    if (!(l1 & VPTE_V) || (l1 & VPTE_LEAF))
    {
        return NULL;
    }

    const uint32_t *l2 = (const uint32_t *) VPTE_PA(l1);
    const uint32_t pte = l2[L2_INDEX(va)];

    if ((pte & (VPTE_V | VPTE_R | VPTE_U)) != (VPTE_V | VPTE_R | VPTE_U))
    {
        return NULL;
    }
    return (const uint8_t *) (VPTE_PA(pte) + (va & (VM_PAGE_SIZE - 1)));
}

bool vm_copy_from_user(const vm_space_t *s, void *dst, uint32_t va, size_t len)
{
    uint8_t *out = dst;

    if (len != 0 && va + (uint32_t) len - 1 < va)
    {
        return false;
    }

    while (len != 0)
    {
        const uint8_t *src = translate_read(s, va);

        if (src == NULL)
        {
            return false;
        }

        size_t chunk = VM_PAGE_SIZE - (va & (VM_PAGE_SIZE - 1));

        if (chunk > len)
        {
            chunk = len;
        }
        memcpy(out, src, chunk);
        out += chunk;
        va += (uint32_t) chunk;
        len -= chunk;
    }
    return true;
}

int vm_strncpy_from_user(const vm_space_t *s, char *dst, uint32_t va, size_t max)
{
    for (size_t n = 0; n < max; n++)
    {
        if (va + (uint32_t) n < va)
        {
            return -1;
        }

        const uint8_t *src = translate_read(s, va + (uint32_t) n);

        if (src == NULL)
        {
            return -1;
        }

        dst[n] = (char) *src;
        if (*src == '\0')
        {
            return (int) n;
        }
    }
    return -1;
}
