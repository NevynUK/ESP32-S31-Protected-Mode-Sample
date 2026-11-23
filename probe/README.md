# ESP32-S31 Privilege and Memory Protection Probe

A standalone ESP-IDF app that measures which privilege and memory protection
features the ESP32-S31 really implements. The preliminary datasheet claims them,
the HP CPU chapter of the Technical Reference Manual is not published yet, and
ESP-IDF runs entirely in machine mode, so the silicon is the only authority.

This is the S31 counterpart to the `probe/` experiment in
`ESP32-P4-Protected-Mode-Sample`, and its results shaped the kernel in the parent
directory.

## Quick Start

```bash
. ../scripts/idf-env.sh
idf_py -p /dev/cu.usbmodem101 build flash
```

Run it from this directory. The console is on `/dev/cu.usbserial-*`.

The summary is printed once after each reset and ends with `PROBE COMPLETE`.

## Measured Results

ESP32-S31 chip revision v0.0, both cores, ESP-IDF v6.1 and v6.1.1. All 37 checks
pass on each core under both versions.

| Feature | Result |
|---|---|
| `misa` | `0x40943127`: RV32 with A, B, C, F, I, M, N, S, U, X |
| Privilege modes | Machine, supervisor and user all work |
| Trap delegation | U-mode `ecall` delegates to an S-mode handler |
| Sv32 page tables | Work, including permissions and the SUM bit |
| Separate address spaces | Same virtual address reaches different physical pages |
| ASID bits | 9 |
| PTE accessed and dirty bits | Not set by hardware: the access page-faults |
| PMP entries | 16, not the datasheet's "up to 32" |
| PMP granularity | 128 bytes |
| `menvcfg`, `mseccfg` | Absent, so no Smepmp |
| `sie`, `sip` | Absent (the core runs in CLIC mode) |
| User-level CSRs | 16 readable, 10 hold state across U-mode windows |

## What a Kernel Must Handle

- **The PMP must grant S-mode and U-mode access.** With no matching entry, both
  fault on everything, even with page tables set up.
- **ESP-IDF v6.1 leaves the S31's PMP empty.** `CONFIG_ESP_SYSTEM_MEMPROT=y` has no
  effect there. v6.1.1 programs it, locking 14 of the 16 entries, so a kernel that
  wants its own layout must stop ESP-IDF doing that.
- **The page-fault handler must set A and D**, then retry the access.
- **`stvec` uses CLIC mode.** It reads back with mode 3 and enters at
  `stvec[31:6] << 6`, so S-mode handlers must be 64-byte aligned.
- **`scause` uses the CLIC format.** Mask it to the low 12 bits for the exception
  code. A delegated U-mode `ecall` reads `0x08000008`.

## How It Works

`priv_call()` in `main/probe_stubs.S` raises the CLIC threshold, installs a
private trap vector and `mret`s into the requested mode. Every stub ends in a
trap, which is recorded and returns to M-mode, so a missing feature is a result
rather than a crash. `main/probe.c` holds the test sequence.

If the PMP is empty, the probe installs a temporary unlocked grant over internal
RAM (code R+X, data R+W) and removes it afterwards.

## Limits

Only one chip at revision v0.0 was measured, so later revisions may differ.
Interrupts are masked in every S-mode and U-mode window. Interrupt delegation,
TLB behavior under ASID reuse, PSRAM mappings and sustained workloads are not
tested.
