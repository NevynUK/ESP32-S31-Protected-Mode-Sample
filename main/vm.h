/*
 * vm.h -- per-user Sv32 address spaces.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VM_PAGE_SIZE 4096u
#define VM_PTES 1024u

typedef struct
{
    uint32_t *root;  /* first-level table; never mapped into any user space  */
    uint32_t *l2;    /* second-level table for the 4 MiB the user can see    */
    uint8_t *ro;     /* one read-only page                                   */
    uint8_t *data;   /* one read/write page                                  */
    uint8_t *stack;  /* one read/write page, below it an unmapped guard page */
    uint32_t asid;
    uint32_t satp;
} vm_space_t;

/* Build address space number idx (0-based).  Aborts on a bad image layout. */
void vm_space_init(vm_space_t *s, unsigned idx);

/* Zero the user's writable pages. */
void vm_space_clear(const vm_space_t *s);

/* The user virtual address of a function in the user image. */
uint32_t vm_user_text_va(const void *fn);

/* Copy from user memory, translating through s.  False if any byte is not
 * mapped user-readable; nothing beyond the failing page has been read. */
bool vm_copy_from_user(const vm_space_t *s, void *dst, uint32_t va, size_t len);

/* Copy a NUL-terminated string into dst (size max).  Returns its length, or -1
 * if it is unmapped or not terminated within max - 1 bytes. */
int vm_strncpy_from_user(const vm_space_t *s, char *dst, uint32_t va, size_t max);
