/*
 * probe.c -- what privilege and memory protection does this ESP32-S31 really
 * implement?
 *
 * SPDX-License-Identifier: MIT
 *
 * The preliminary datasheet claims Machine, Supervisor and User modes, Sv32
 * page tables and up to 32 PMP regions.  ESP-IDF uses none of the first three
 * beyond M-mode and says PMP entries from 16 up fault.  This runs the same
 * measurements on each core and reports what the silicon does.
 *
 * Every test runs through priv_call(), so a missing feature is a recorded trap.
 */

#include <stdbool.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "probe.h"

#define MODE_U 0u
#define MODE_S 1u
#define MODE_M 3u

#define EXC_INSN_FAULT 1u
#define EXC_ILLEGAL 2u
#define EXC_LOAD_FAULT 5u
#define EXC_STORE_FAULT 7u
#define EXC_ECALL_U 8u
#define EXC_ECALL_S 9u
#define EXC_ECALL_M 11u
#define EXC_INSN_PAGE 12u
#define EXC_LOAD_PAGE 13u
#define EXC_STORE_PAGE 15u

#define MISA_S (1u << 18)
#define MISA_U (1u << 20)
#define PROBE_MSTATUS_SUM (1u << 18)
#define PROBE_SATP_SV32 (1u << 31)
#define SATP_ASID_SHIFT 22
#define SATP_ASID_MASK 0x1ffu

#define PROBE_PTE_V (1u << 0)
#define PROBE_PTE_R (1u << 1)
#define PROBE_PTE_W (1u << 2)
#define PROBE_PTE_X (1u << 3)
#define PROBE_PTE_U (1u << 4)
#define PROBE_PTE_A (1u << 6)
#define PROBE_PTE_D (1u << 7)
#define PTE(pa, flags) ((((uint32_t) (pa) >> 12) << 10) | (flags))

/* Entry 1: TOR R+X.  Entry 2: TOR R+W.  Entry 0 is OFF and only holds a base. */
#define PROBE_PMPCFG0_GRANT 0x000B0D00u

#define PAGE_SIZE 4096u
#define PTES 1024u
#define SRAM_BASE 0x2F000000u
#define SRAM_END 0x2F080000u

/* An otherwise unused part of the virtual address space. */
#define VA_BASE 0x60000000u
#define VA(n) (VA_BASE + (n) * PAGE_SIZE)
#define VA_USER VA(0)   /* user read/write */
#define VA_RO VA(1)     /* user read-only */
#define VA_CODE VA(2)   /* user execute: an alias of probe_page */
#define VA_KERNEL VA(3) /* supervisor only */
#define VA_INVALID VA(4)
#define VA_NO_A VA(5) /* accessed bit clear */
#define VA_NO_D VA(6) /* dirty bit clear */

#define USER_PC(stub) (VA_CODE + (uint32_t) ((stub) - probe_page))

#define LOG(fmt, ...) esp_rom_printf("S31P[%d]: " fmt "\n", s_core, ##__VA_ARGS__)

typedef __attribute__((aligned(PAGE_SIZE))) uint32_t page_t[PTES];

extern const char _iram_text_end[];

static page_t s_root_a, s_l2_a, s_root_b, s_l2_b;
static page_t s_data_a, s_data_b, s_data_ro, s_data_kernel;

typedef struct
{
    uint32_t misa;
    bool u_mode, s_csrs, s_mode, deleg, sv32, sv32_ran, spaces;
    unsigned pmp_impl, pmp_readable, pmp_locked, pmp_gran, asid_bits;
    bool ad_hw_a, ad_hw_d, pmp_from_idf;
} report_t;

static report_t s_report[2];
static int s_core;
static unsigned s_pass, s_fail;
static SemaphoreHandle_t s_done;

static const char *cause_name(uint32_t cause)
{
    if (cause & 0x80000000u)
    {
        return "interrupt";
    }

    switch (cause & 0xfffu)
    {
    case 0: return "insn misaligned";
    case EXC_INSN_FAULT: return "insn access fault";
    case EXC_ILLEGAL: return "illegal insn";
    case 3: return "breakpoint";
    case 4: return "load misaligned";
    case EXC_LOAD_FAULT: return "load access fault";
    case 6: return "store misaligned";
    case EXC_STORE_FAULT: return "store access fault";
    case EXC_ECALL_U: return "ecall from U";
    case EXC_ECALL_S: return "ecall from S";
    case EXC_ECALL_M: return "ecall from M";
    case EXC_INSN_PAGE: return "insn page fault";
    case EXC_LOAD_PAGE: return "load page fault";
    case EXC_STORE_PAGE: return "store page fault";
    default: return "other";
    }
}

static priv_req_t enter(uint32_t mode, const void *pc, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t mstatus_set)
{
    priv_req_t r = {
        .pc = (uint32_t) pc, .mode = mode, .a0 = a0, .a1 = a1, .a2 = a2, .mstatus_set = mstatus_set};

    priv_call(&r);
    return r;
}

static inline uint32_t code(const priv_req_t *r)
{
    return (r->cause & 0x80000000u) ? 0xffffffffu : (r->cause & 0xfffu);
}

static bool expect(const char *what, const priv_req_t *r, uint32_t want)
{
    const bool ok = code(r) == want;

    if (ok)
    {
        s_pass++;
    }
    else
    {
        s_fail++;
    }

    LOG("  %s %s: %s (mcause=%08x mepc=%08x mtval=%08x a0=%08x)", ok ? "PASS" : "FAIL", what, cause_name(r->cause), r->cause, r->epc, r->tval, r->out_a0);
    return ok;
}

static bool expect_value(const char *what, uint32_t got, uint32_t want)
{
    const bool ok = got == want;

    if (ok)
    {
        s_pass++;
    }
    else
    {
        s_fail++;
    }

    LOG("  %s %s: got %08x, want %08x", ok ? "PASS" : "FAIL", what, got, want);
    return ok;
}

static void section(const char *name)
{
    s_pass = 0;
    s_fail = 0;
    LOG("--- %s", name);
}

static void probe_identity(report_t *rep)
{
    uint32_t mvendorid, marchid, mimpid, mhartid;

    __asm__ volatile("csrr %0, misa" : "=r"(rep->misa));
    __asm__ volatile("csrr %0, mvendorid" : "=r"(mvendorid));
    __asm__ volatile("csrr %0, marchid" : "=r"(marchid));
    __asm__ volatile("csrr %0, mimpid" : "=r"(mimpid));
    __asm__ volatile("csrr %0, mhartid" : "=r"(mhartid));

    char ext[27];
    unsigned n = 0;

    for (unsigned i = 0; i < 26; i++)
    {
        if (rep->misa & (1u << i))
        {
            ext[n++] = (char) ('A' + i);
        }
    }
    ext[n] = 0;

    section("identity");
    LOG("  misa=%08x extensions=%s (S=%d U=%d)", rep->misa, ext, !!(rep->misa & MISA_S), !!(rep->misa & MISA_U));
    LOG("  mvendorid=%08x marchid=%08x mimpid=%08x mhartid=%u", mvendorid, marchid, mimpid, mhartid);
}

static void probe_csrs(report_t *rep)
{
    static const struct
    {
        uint16_t num;
        const char *name;
    } csrs[] = {
#define X(num, name) {num, #name},
        PROBE_CSR_LIST(X)
#undef X
    };
    unsigned s_present = 0;

    section("CSRs readable from M-mode");
    for (unsigned i = 0; i < sizeof(csrs) / sizeof(csrs[0]); i++)
    {
        const priv_req_t r = enter(MODE_M, stub_csr_read + 8 * i, 0, 0, 0, 0);
        const bool present = code(&r) == EXC_ECALL_M;

        LOG("  %s (0x%x): %s, value %08x", csrs[i].name, csrs[i].num, present ? "present" : "ABSENT", present ? r.out_a0 : 0);
        /* sie and sip do not exist in CLIC mode, so they are not counted. */
        if (present && csrs[i].num < 0x200 && csrs[i].num != 0x104 && csrs[i].num != 0x144)
        {
            s_present++;
        }
    }
    rep->s_csrs = s_present == 8;
}

static void probe_pmp(report_t *rep)
{
    uint8_t cfg[64] = {0};
    bool cfg_ok[64] = {false};

    section("PMP");
    for (unsigned j = 0; j < 16; j++)
    {
        const priv_req_t r = enter(MODE_M, stub_pmpcfg_read + 8 * j, 0, 0, 0, 0);

        if (code(&r) == EXC_ECALL_M)
        {
            for (unsigned b = 0; b < 4; b++)
            {
                cfg[j * 4 + b] = (uint8_t) (r.out_a0 >> (8 * b));
                cfg_ok[j * 4 + b] = true;
            }
        }
    }

    for (unsigned i = 0; i < 64; i++)
    {
        const priv_req_t rd = enter(MODE_M, stub_pmpaddr_read + 8 * i, 0, 0, 0, 0);

        if (code(&rd) != EXC_ECALL_M)
        {
            continue;
        }
        rep->pmp_readable++;

        const bool locked = cfg[i] & 0x80;
        const bool in_use = cfg[i] != 0;
        const bool next_is_tor = i < 63 && ((cfg[i + 1] >> 3) & 3) == 1;
        const char *state = "in use";

        if (locked)
        {
            rep->pmp_locked++;
        }

        if (in_use || next_is_tor)
        {
            rep->pmp_impl++;
        }
        else
        {
            /* Free entry: nothing matches it, so its address can be tested. */
            const priv_req_t wr = enter(MODE_M, stub_pmpaddr_write + 12 * i, 0xffffffffu, 0, 0, 0);

            if (code(&wr) != EXC_ECALL_M)
            {
                state = "write traps";
            }
            else if (wr.out_a0 == 0)
            {
                state = "reads as zero";
            }
            else
            {
                state = "free, writable";
                rep->pmp_impl++;
                if (rep->pmp_gran == 0)
                {
                    rep->pmp_gran = 4u << __builtin_ctz(wr.out_a0);
                }
                (void) enter(MODE_M, stub_pmpaddr_write + 12 * i, rd.out_a0, 0, 0, 0);
            }
        }

        if (i < 16 || strcmp(state, "free, writable") == 0)
        {
            LOG("  pmp%u addr=%08x cfg=%02x%s %s", i, rd.out_a0, cfg[i], cfg_ok[i] ? "" : "?", state);
        }
    }
    LOG("  %u readable, %u implemented, %u locked by ESP-IDF, granularity %u bytes", rep->pmp_readable, rep->pmp_impl, rep->pmp_locked, rep->pmp_gran);
}

/* With no PMP entry at all, S-mode and U-mode can reach nothing.  If ESP-IDF left
 * the PMP empty, grant internal RAM for the duration: code R+X, data R+W.  The
 * entries are unlocked, so they bind S-mode and U-mode and leave M-mode alone.
 * Addresses are written while every entry is still OFF, because the S31 faults
 * on even a transient TOR overlap. */
static bool pmp_grant_if_empty(void)
{
    for (unsigned j = 0; j < 4; j++)
    {
        const priv_req_t r = enter(MODE_M, stub_pmpcfg_read + 8 * j, 0, 0, 0, 0);

        if (code(&r) != EXC_ECALL_M || r.out_a0 != 0)
        {
            LOG("  ESP-IDF has programmed the PMP, using its layout");
            return false;
        }
    }

    (void) enter(MODE_M, stub_pmpaddr_write + 12 * 0, SRAM_BASE >> 2, 0, 0, 0);
    (void) enter(MODE_M, stub_pmpaddr_write + 12 * 1, (uint32_t) _iram_text_end >> 2, 0, 0, 0);
    (void) enter(MODE_M, stub_pmpaddr_write + 12 * 2, SRAM_END >> 2, 0, 0, 0);

    const priv_req_t r = enter(MODE_M, stub_pmpcfg0_write, PROBE_PMPCFG0_GRANT, 0, 0, 0);

    LOG("  PMP was empty: temporary grant installed, pmpcfg0=%08x, code ends at %08x", r.out_a0, (uint32_t) _iram_text_end);
    return true;
}

static void pmp_release(void)
{
    (void) enter(MODE_M, stub_pmpcfg0_write, 0, 0, 0, 0);
    for (unsigned i = 0; i < 3; i++)
    {
        (void) enter(MODE_M, stub_pmpaddr_write + 12 * i, 0, 0, 0, 0);
    }
}

static void probe_user(report_t *rep)
{
    static uint32_t s_rw_word;
    priv_req_t r;

    section("U-mode (no translation, ESP-IDF PMP)");
    r = enter(MODE_U, stub_ecall, 0, 0, 0, 0);
    if (!expect("mret to U, ecall", &r, EXC_ECALL_U))
    {
        return;
    }

    r = enter(MODE_U, stub_read_mstatus, 0, 0, 0, 0);
    expect("read mstatus from U", &r, EXC_ILLEGAL);

    s_rw_word = 0;
    r = enter(MODE_U, stub_store, (uint32_t) &s_rw_word, 0x5a5a1234u, 0, 0);
    expect("store to DRAM from U", &r, EXC_ECALL_U);
    expect_value("the store landed", s_rw_word, 0x5a5a1234u);

    r = enter(MODE_U, stub_store, (uint32_t) stub_ecall, 0, 0, 0);
    expect("store to IRAM code from U", &r, EXC_STORE_FAULT);

    r = enter(MODE_U, stub_load, (uint32_t) esp_rom_printf, 0, 0, 0);
    LOG("  info load from ROM text in U: %s (no PMP entry covers it, so a fault is expected)", cause_name(r.cause));

    rep->u_mode = s_fail == 0;
}

/* Which CSRs can U-mode really touch?  Anything whose value survives from one
 * U-mode window to the next is a channel between applications.  Each bit is
 * flipped in one window, read back in a separate one, then restored, so the
 * mask below holds exactly the bits that persist across windows. */
static void probe_user_csrs(void)
{
    static const uint16_t bases[4] = {0x000, 0x400, 0x800, 0xC00};
    unsigned readable = 0, channels = 0;

    section("CSRs reachable from U-mode");
    for (unsigned i = 0; i < 1024; i++)
    {
        const uint32_t num = bases[i >> 8] + (i & 0xff);
        const priv_req_t rd = enter(MODE_U, stub_ucsr_read + 8 * i, 0, 0, 0, 0);

        if (code(&rd) != EXC_ECALL_U)
        {
            continue;
        }
        readable++;

        const uint32_t old = rd.out_a0;
        uint32_t persists = 0;
        bool write_traps = false;

        for (unsigned bit = 0; bit < 32; bit++)
        {
            const priv_req_t wr = enter(MODE_U, stub_ucsr_write + 12 * i, old ^ (1u << bit), 0, 0, 0);

            if (code(&wr) != EXC_ECALL_U)
            {
                write_traps = true;
                break;
            }

            const priv_req_t again = enter(MODE_U, stub_ucsr_read + 8 * i, 0, 0, 0, 0);

            if (code(&again) == EXC_ECALL_U && ((again.out_a0 ^ old) & (1u << bit)))
            {
                persists |= 1u << bit;
            }
            (void) enter(MODE_U, stub_ucsr_write + 12 * i, old, 0, 0, 0);
        }

        if (persists)
        {
            channels++;
        }
        LOG("  csr 0x%x: value %08x, persistent bits %08x%s", num, old, persists, write_traps ? " (write traps)" : "");
    }
    LOG("  %u of 1024 user-level CSRs readable, %u hold state across windows", readable, channels);
}

static void probe_supervisor(report_t *rep)
{
    priv_req_t r;

    section("S-mode");
    r = enter(MODE_M, stub_mpp_write, MODE_S, 0, 0, 0);
    const uint32_t mpp = (r.out_a0 >> 11) & 3;

    LOG("  mstatus.MPP after writing S: %u (%s)", mpp, mpp == MODE_S ? "S-mode is a legal value" : "S-mode is NOT implemented");
    if (mpp != MODE_S)
    {
        return;
    }

    r = enter(MODE_S, stub_ecall, 0, 0, 0, 0);
    if (!expect("mret to S, ecall", &r, EXC_ECALL_S))
    {
        return;
    }

    r = enter(MODE_S, stub_read_mstatus, 0, 0, 0, 0);
    expect("read mstatus from S", &r, EXC_ILLEGAL);

    r = enter(MODE_S, stub_read_s_csrs, 0, 0, 0, 0);
    expect("read sstatus, satp from S", &r, EXC_ECALL_S);

    r = enter(MODE_U, stub_read_s_csrs, 0, 0, 0, 0);
    expect("read sstatus from U", &r, EXC_ILLEGAL);

    r = enter(MODE_S, stub_sret_to_u, (uint32_t) stub_ecall, 0, 0, 0);
    expect("sret from S to U", &r, EXC_ECALL_U);

    rep->s_mode = s_fail == 0;
    if (!rep->s_mode)
    {
        return;
    }

    /* Delegate U-mode ecall to S-mode and see S-mode's handler take it. */
    section("Trap delegation");
    const priv_req_t set = enter(MODE_M, stub_medeleg_swap, 1u << EXC_ECALL_U, 0, 0, 0);
    const uint32_t old_medeleg = set.out_a0;

    LOG("  medeleg was %08x, reads %08x after setting bit 8", old_medeleg, set.out_a1);
    if (set.out_a1 & (1u << EXC_ECALL_U))
    {
        r = enter(MODE_S, stub_deleg_to_u, (uint32_t) stub_ecall, (uint32_t) stub_s_handler | 3u, 0, 0);
        const bool took = expect("U ecall reaches S handler, which ecalls to M", &r, EXC_ECALL_S);

        /* scause is in CLIC format too: the exception code is its low 12 bits. */
        rep->deleg = expect_value("scause code seen by S", r.out_a0 & 0xfffu, EXC_ECALL_U) && took;
        expect_value("sepc seen by S", r.out_a1, (uint32_t) stub_ecall);
    }
    (void) enter(MODE_M, stub_medeleg_swap, old_medeleg, 0, 0, 0);
}

static void build_space(uint32_t *root, uint32_t *l2, const uint32_t *user_page)
{
    memset(root, 0, PAGE_SIZE);
    memset(l2, 0, PAGE_SIZE);

    /* S-mode runs on an identity 4 MiB superpage over internal RAM. */
    root[SRAM_BASE >> 22] = PTE(SRAM_BASE, PROBE_PTE_V | PROBE_PTE_R | PROBE_PTE_W | PROBE_PTE_X | PROBE_PTE_A | PROBE_PTE_D);
    root[VA_BASE >> 22] = PTE(l2, PROBE_PTE_V);

    l2[0] = PTE(user_page, PROBE_PTE_V | PROBE_PTE_R | PROBE_PTE_W | PROBE_PTE_U | PROBE_PTE_A | PROBE_PTE_D);
    l2[1] = PTE(s_data_ro, PROBE_PTE_V | PROBE_PTE_R | PROBE_PTE_U | PROBE_PTE_A);
    l2[2] = PTE(probe_page, PROBE_PTE_V | PROBE_PTE_R | PROBE_PTE_X | PROBE_PTE_U | PROBE_PTE_A);
    l2[3] = PTE(s_data_kernel, PROBE_PTE_V | PROBE_PTE_R | PROBE_PTE_W | PROBE_PTE_A | PROBE_PTE_D);
    /* l2[4] stays invalid */
    l2[5] = PTE(user_page, PROBE_PTE_V | PROBE_PTE_R | PROBE_PTE_W | PROBE_PTE_U);
    l2[6] = PTE(user_page, PROBE_PTE_V | PROBE_PTE_R | PROBE_PTE_W | PROBE_PTE_U | PROBE_PTE_A);
}

static uint32_t satp_for(const uint32_t *root, uint32_t asid)
{
    return PROBE_SATP_SV32 | (asid << SATP_ASID_SHIFT) | ((uint32_t) root >> 12);
}

static void probe_sv32(report_t *rep)
{
    priv_req_t r;

    section("Sv32 page tables");
    if ((uint32_t) probe_page_end - (uint32_t) probe_page > PAGE_SIZE || (uint32_t) s_root_a < SRAM_BASE || (uint32_t) s_data_kernel >= SRAM_END || (uint32_t) probe_page < SRAM_BASE || (uint32_t) probe_page >= SRAM_END)
    {
        LOG("  FAIL layout: probe_page=%08x root=%08x", (uint32_t) probe_page, (uint32_t) s_root_a);
        s_fail++;
        return;
    }

    /* Does satp accept Sv32, and how many ASID bits does it have? */
    const uint32_t want = PROBE_SATP_SV32 | (SATP_ASID_MASK << SATP_ASID_SHIFT) | ((uint32_t) s_root_a >> 12);

    build_space(s_root_a, s_l2_a, s_data_a);
    build_space(s_root_b, s_l2_b, s_data_b);
    r = enter(MODE_M, stub_satp_set, want, 0, 0, 0);
    LOG("  satp wrote %08x, read %08x", want, r.out_a0);
    if (code(&r) != EXC_ECALL_M || !(r.out_a0 & PROBE_SATP_SV32))
    {
        LOG("  Sv32 is NOT implemented (%s)", cause_name(r.cause));
        (void) enter(MODE_M, stub_satp_set, 0, 0, 0, 0);
        return;
    }
    rep->sv32_ran = true;
    rep->asid_bits = __builtin_popcount((r.out_a0 >> SATP_ASID_SHIFT) & SATP_ASID_MASK);
    LOG("  ASID bits: %u", rep->asid_bits);

    s_data_a[0] = 0xaaaa0001u;
    s_data_b[0] = 0xbbbb0002u;
    s_data_ro[0] = 0xcccc0003u;
    s_data_kernel[0] = 0xdddd0004u;

    r = enter(MODE_M, stub_satp_set, satp_for(s_root_a, 1), 0, 0, 0);

    r = enter(MODE_S, stub_read_s_csrs, 0, 0, 0, 0);
    expect("S runs with translation on", &r, EXC_ECALL_S);
    expect_value("S sees the active satp", r.out_a1, satp_for(s_root_a, 1));

    r = enter(MODE_S, stub_load, VA_KERNEL, 0, 0, 0);
    expect("S load through a mapping", &r, EXC_ECALL_S);
    expect_value("it read the mapped page", r.out_a0, 0xdddd0004u);

    r = enter(MODE_S, stub_store, VA_KERNEL, 0x11112222u, 0, 0);
    expect("S store through a mapping", &r, EXC_ECALL_S);
    expect_value("it wrote the mapped page", s_data_kernel[0], 0x11112222u);

    r = enter(MODE_U, (const void *) USER_PC(stub_ecall), 0, 0, 0, 0);
    expect("U executes from a mapped alias", &r, EXC_ECALL_U);
    expect_value("U's pc was virtual", r.epc, USER_PC(stub_ecall));

    r = enter(MODE_U, (const void *) USER_PC(stub_load), VA_USER, 0, 0, 0);
    expect("U load from its page", &r, EXC_ECALL_U);
    expect_value("U read its data", r.out_a0, 0xaaaa0001u);

    r = enter(MODE_U, (const void *) USER_PC(stub_store), VA_RO, 1, 0, 0);
    expect("U store to read-only page", &r, EXC_STORE_PAGE);
    expect_value("read-only page unchanged", s_data_ro[0], 0xcccc0003u);

    r = enter(MODE_U, (const void *) USER_PC(stub_load), VA_KERNEL, 0, 0, 0);
    expect("U load from supervisor page", &r, EXC_LOAD_PAGE);

    r = enter(MODE_U, (const void *) USER_PC(stub_load), VA_INVALID, 0, 0, 0);
    expect("U load from unmapped page", &r, EXC_LOAD_PAGE);

    r = enter(MODE_U, (const void *) USER_PC(stub_jump), VA_USER, 0, 0, 0);
    expect("U execute from data page", &r, EXC_INSN_PAGE);

    r = enter(MODE_S, stub_load, VA_USER, 0, 0, 0);
    expect("S load from user page, SUM=0", &r, EXC_LOAD_PAGE);

    r = enter(MODE_S, stub_load, VA_USER, 0, 0, PROBE_MSTATUS_SUM);
    expect("S load from user page, SUM=1", &r, EXC_ECALL_S);

    rep->sv32 = s_fail == 0;

    /* Accessed and dirty bits: either hardware sets them or the access faults.
     * Both are legal, so this is reported rather than judged. */
    r = enter(MODE_U, (const void *) USER_PC(stub_load), VA_NO_A, 0, 0, 0);
    rep->ad_hw_a = code(&r) == EXC_ECALL_U;
    LOG("  info access with A=0: %s, PTE now %08x", cause_name(r.cause), s_l2_a[5]);
    (void) enter(MODE_M, stub_satp_set, satp_for(s_root_a, 1), 0, 0, 0);

    r = enter(MODE_U, (const void *) USER_PC(stub_store), VA_NO_D, s_data_a[0], 0, 0);
    rep->ad_hw_d = code(&r) == EXC_ECALL_U;
    LOG("  info store with D=0: %s, PTE now %08x", cause_name(r.cause), s_l2_a[6]);

    /* Two address spaces: the same virtual address, different physical pages. */
    section("Address-space isolation");
    (void) enter(MODE_M, stub_satp_set, satp_for(s_root_a, 1), 0, 0, 0);
    r = enter(MODE_U, (const void *) USER_PC(stub_load), VA_USER, 0, 0, 0);
    expect("space A load", &r, EXC_ECALL_U);
    expect_value("space A sees its page", r.out_a0, s_data_a[0]);

    (void) enter(MODE_M, stub_satp_set, satp_for(s_root_b, 2), 0, 0, 0);
    r = enter(MODE_U, (const void *) USER_PC(stub_load), VA_USER, 0, 0, 0);
    expect("space B load, same virtual address", &r, EXC_ECALL_U);
    expect_value("space B sees a different page", r.out_a0, 0xbbbb0002u);

    const uint32_t a_before = s_data_a[0];

    r = enter(MODE_U, (const void *) USER_PC(stub_store), VA_USER, 0x22223333u, 0, 0);
    expect("space B store", &r, EXC_ECALL_U);
    expect_value("the store changed B", s_data_b[0], 0x22223333u);
    expect_value("the store did not change A", s_data_a[0], a_before);

    rep->spaces = s_fail == 0;
    (void) enter(MODE_M, stub_satp_set, 0, 0, 0, 0);
}

static void probe_task(void *arg)
{
    report_t *rep = &s_report[(int) arg];

    s_core = (int) arg;
    memset(rep, 0, sizeof(*rep));

    probe_identity(rep);
    probe_csrs(rep);
    probe_pmp(rep);

    const bool granted = pmp_grant_if_empty();

    rep->pmp_from_idf = !granted;
    probe_user(rep);
    if (rep->u_mode)
    {
        probe_user_csrs();
        probe_supervisor(rep);
    }
    if (rep->s_mode)
    {
        probe_sv32(rep);
    }
    if (granted)
    {
        pmp_release();
    }

    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static const char *yes_no(bool b)
{
    return b ? "YES" : "no";
}

void app_main(void)
{
    esp_chip_info_t chip;

    esp_chip_info(&chip);
    esp_rom_printf("\nS31P: ESP32-S31 privilege probe, ESP-IDF %s, chip revision v%d.%d, %d cores\n", esp_get_idf_version(), chip.revision / 100, chip.revision % 100, chip.cores);

    s_done = xSemaphoreCreateBinary();
    for (int core = 0; core < 2; core++)
    {
        xTaskCreatePinnedToCore(probe_task, "probe", 8192, (void *) core, 5, NULL, core);
        xSemaphoreTake(s_done, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_rom_printf("\nS31P: ================ SUMMARY ================\n");
    #define ROW(label, field) esp_rom_printf("S31P: %s: core0=%s core1=%s\n", label, yes_no(s_report[0].field), yes_no(s_report[1].field))
    ROW("User mode", u_mode);
    ROW("Supervisor CSRs", s_csrs);
    ROW("Supervisor mode", s_mode);
    ROW("Trap delegation", deleg);
    ROW("Sv32 translation", sv32);
    ROW("Isolated addr spaces", spaces);
    ROW("Hardware sets PTE.A", ad_hw_a);
    ROW("Hardware sets PTE.D", ad_hw_d);
    ROW("ESP-IDF programmed PMP", pmp_from_idf);
#undef ROW
    esp_rom_printf("S31P: %s: core0=%u core1=%u\n", "PMP entries", s_report[0].pmp_impl, s_report[1].pmp_impl);
    esp_rom_printf("S31P: %s: core0=%u core1=%u\n", "PMP granularity", s_report[0].pmp_gran, s_report[1].pmp_gran);
    esp_rom_printf("S31P: %s: core0=%u core1=%u\n", "ASID bits", s_report[0].asid_bits, s_report[1].asid_bits);
    esp_rom_printf("S31P: misa core0=%08x core1=%08x\n", s_report[0].misa, s_report[1].misa);
    esp_rom_printf("S31P: PROBE COMPLETE\n");
}
