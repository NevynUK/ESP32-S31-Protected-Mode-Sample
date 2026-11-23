# ESP32-S31: An ESP-IDF Application With Isolated User-Mode Programs

A port of `ESP32-P4-Protected-Mode-Sample` to the ESP32-S31. ESP-IDF and FreeRTOS
run in machine mode as usual. Eight user programs run in user mode, each in its
own Sv32 address space, and reach the kernel only through `ecall`.

The P4 has no page tables, so its users can still read and write each other's
memory. On the S31 they cannot.

## Quick Start

```bash
./flash.sh      # build and flash through /dev/cu.usbmodem*
./monitor.sh    # console on /dev/cu.usbserial-*
```

Requires ESP-IDF 6.1.1. Version 6.1 leaves the S31's PMP unprogrammed.

At boot the kernel attacks its own isolation from user mode on both cores. It
starts the user programs only if every check passes:

```
ISOLATION[0]: 60 passed, 0 failed
ISOLATION[1]: 60 passed, 0 failed
ISOLATION: 120 checks across both cores, 0 failed
```

## What Each User Can See

| Virtual address | Access | Backing |
|---|---|---|
| `0x60000000` | R-X | The user image, shared by all users |
| `0x60100000` | R-- | One private page |
| `0x60200000` | RW- | One private page |
| `0x602FF000` | none | Unmapped guard page |
| `0x60300000` | RW- | One private stack page |

Nothing else exists for that user: not the kernel, its page tables, peripherals,
flash, ROM or another user. The user image is linked into page-aligned IRAM of
its own, so mapping it exposes no kernel code.

## How It Differs From the P4 Sample

- **ESP-IDF's locked PMP stays on.** Page tables confine the users, so the kernel
  keeps IDF's own write and execute protection. The P4 had to remove it.
- **The kernel's stack guard stays on.** A user stack overflow hits the guard
  page. The P4 had to give up IDF's M-mode stack overflow panic.
- **User programs are preempted.** A user in an infinite loop cannot hold a core.
- **Per-user CSR state.** Ten user-level CSRs keep their value between windows,
  so `umode.S` swaps them per context. Without this they are a covert channel.
- **Coprocessors are off in user mode.** A user cannot read another task's FPU,
  PIE or hardware loop registers.
- **Syscall pointers are virtual.** The kernel translates them through the
  caller's own page tables and copies the data, so a user cannot make it read
  memory on the user's behalf.

## S31 Behavior That Shaped the Code

All of this was measured on a chip revision v0.0 part.

- **`mintthresh` does not mask interrupts in user mode.** They trap to the private
  vector, which returns to the kernel with the level-triggered source still
  pending, and ESP-IDF then services it.
- **`mret` restores the CLIC level only when `mcause.interrupt` is set.** Clearing
  that bit after an interrupt taken in user mode leaves the core at that level,
  and the tick never fires again.
- **`gp` and `tp` must be restored before the `mret` that re-enables interrupts.**
  An ISR landing in the gap otherwise locks up the CPU.
- **Hardware does not set PTE accessed or dirty bits.** The kernel sets them when
  it builds the tables.
- **OpenOCD resets the chip on attach when memory protection is on.** To inspect a
  live hang, start it with `-c "set ESP_FLASH_SIZE 0"` before the board config.

The measurements are in [`probe/`](probe/README.md), a separate buildable project.

## Limits

- **Edge-triggered interrupts are untested.** Every source enabled in this build
  is level-triggered. An edge-triggered source taken in user mode would be claimed
  and could be lost.
- **No CPU quotas.** A busy user at this priority can starve lower-priority tasks,
  including the idle tasks, which raises a task watchdog warning.
- **Timing side channels are not addressed.** Users share caches and cores.
- **DMA is not confined.** The S31's APM is not supported by ESP-IDF yet, but no
  user can program a peripheral.
- **Users have no FPU, heap or PSRAM**, and each has fixed one-page regions.
- **The TLB is flushed on every window.** ASIDs should make this unnecessary, but
  that has not been measured.
