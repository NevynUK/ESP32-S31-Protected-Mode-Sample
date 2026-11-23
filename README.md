# ESP32-S31: An ESP-IDF Application With Isolated User-Mode Programs

A port of `ESP32-P4-Protected-Mode-Sample` to the ESP32-S31. ESP-IDF and FreeRTOS
run in machine mode as usual. Eight user programs run in user mode, each in its
own Sv32 address space, and reach the kernel only through `ecall`.

The P4 has no page tables, so its users can still read and write each other's
memory. On the S31 they cannot.

This is a sample for learning and experiments, not a hardened security boundary.
See [Limits](#limits) before relying on it.

## Requirements

- An ESP32-S31 board. This was developed on the Function-Coreboard-1 with 16 MB of
  flash, chip revision v0.0.
- **ESP-IDF 6.1.1 or later.** Version 6.1 leaves the S31's PMP unprogrammed. The S31
  is still a preview target in ESP-IDF.
- macOS. The scripts look for macOS device names, but you can pass a port to them.

`scripts/idf-env.sh` expects the activation script that the ESP-IDF Installation
Manager creates at `~/.espressif/tools/activate_idf_v6.1.1.sh`. Set `IDF_VERSION` or
`IDF_ACTIVATE` to use a different installation.

The board has two USB connections, and both are useful:

| Port | Connection | Used for |
|---|---|---|
| `/dev/cu.usbmodem*` | The S31's USB-Serial/JTAG | Flashing and OpenOCD |
| `/dev/cu.usbserial-*` | External bridge on UART0 | Console |

## Quick Start

```bash
./flash.sh      # build and flash through /dev/cu.usbmodem*
./monitor.sh    # console on /dev/cu.usbserial-*
```

Both accept an explicit port, such as `./flash.sh /dev/cu.usbmodem101`. To build
without flashing:

```bash
. scripts/idf-env.sh
idf_py build
```

## Expected Output

At boot the kernel attacks its own isolation from user mode on both cores. It
starts the user programs only if every check passes. This is from a real run,
shortened:

```
=== ESP32-S31 protected mode: M-mode kernel, 8 isolated U-mode users ===
KERNEL: user0: asid 1, satp=8042f02a, data page at physical 2f013000, virtual 60200000
ISOLATION[0]: 60 passed, 0 failed
ISOLATION[1]: 60 passed, 0 failed
ISOLATION: 120 checks across both cores, 0 failed
Kernel 1 (core 0)
KERNEL: user0: first syscall, mcause=08000008 (ECALL from U-mode) on core 0, satp=8042f02a
Kernel 2 (core 0)
KERNEL: window:core/moves u0:c1/3 u1:c1/5 u2:c0/4 u3:c0/4 u4:c0/2 u5:c1/1 u6:c0/0 u7:c0/0
User 0.1 on core 0
User 1.1 on core 0
```

Addresses change between builds. If a check fails, the console prints an
`ISOLATION[n]: FAIL` line for it, followed by
`KERNEL: REFUSING TO START USERS`.

The `window:core/moves` line shows the core each user last ran on and how many
times it has changed core.

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

## How It Works

Each user is hosted by an unpinned FreeRTOS task, so it can run on either core.
The task calls `umode_enter()`, an ordinary C function that returns on the user's
next trap:

1. **Enter.** `umode_enter()` saves the kernel's state, turns off the coprocessors,
   swaps in the user's CSRs, loads the user's `satp` and executes `mret`.
2. **Run.** The user runs until an `ecall`, a fault or an interrupt.
3. **Trap.** A private vector saves the user's registers, restores the kernel's
   state and returns from `umode_enter()` with `mcause`.
4. **Handle.** The host task acts on the cause, then loops back to step 1:

| Cause | What the kernel does |
|---|---|
| `ecall` | Runs the syscall in task context, so it can block |
| Interrupt | Re-enters the user after ESP-IDF has serviced the pending source |
| Any other exception | Prints the fault and retires that user only |

Because blocking work happens in the host task, `SYS_DELAY_MS` is a real
`vTaskDelay()` and FreeRTOS schedules users like any other task.

## System Calls

The syscall number goes in `a7`, arguments in `a0` and `a1`, and the result comes
back in `a0`. Every pointer is a user virtual address.

| # | Name | Arguments | Returns |
|---|---|---|---|
| 0 | `SYS_NOP` | None | 0 |
| 1 | `SYS_WRITE` | buffer, length (up to 128) | Bytes written |
| 2 | `SYS_PUTS` | NUL-terminated string (up to 127 characters) | 0 |
| 3 | `SYS_DELAY_MS` | Milliseconds | 0 |
| 4 | `SYS_EXIT` | Status | Does not return |
| 5 | `SYS_GETCORE` | None | Current core |
| 6 | `SYS_YIELD` | None | 0 |

An unknown number returns `SYS_ERR_BADNR` (`-1`). A pointer that isn't valid in the
caller's address space returns `SYS_ERR_FAULT` (`-2`).

## What the Isolation Test Covers

Each core runs the same 60 checks from a real U-mode window:

| Group | Checks | What is attempted |
|---|---|---|
| Memory | 17 | Reach the kernel, page tables, peripherals, flash, ROM or another user; write read-only pages; execute data; overflow the stack |
| Privilege | 10 | Read M-mode CSRs; run `mret`, `sret`, `sfence.vma`, `wfi`, `ebreak`, FPU and PIE instructions; hold the core in an infinite loop |
| Syscall pointers | 9 | Pass kernel, foreign, unmapped, wrapping, page-crossing, oversized or unterminated buffers |
| Cross-user memory | 4 | Confirm the same virtual address reaches a different physical page in each user |
| CSR channels | 20 | Set each persistent bit of the ten user-level CSRs and look for it in another user and in the kernel |

The test is in `main/isolation_test.c`. Add new attacks there, with the user-side
action in `user_attack_start()` in `main/user_main.c`.

## Extending the Sample

### Changing the User Program

All users run the same image, `main/user_main.c`, and receive their slot number as
`id`, so branch on `id` to give them different behavior. That file runs at a
different address from the one it was linked at, which restricts it:

- **No library calls.** Nothing else is mapped.
- **No string literals or static data.** `.rodata` isn't mapped. Put strings in
  `user_rodata_t`, which the kernel fills in and passes to the user.
- **No FPU or PIE instructions.** They trap and retire the user.
- **At most 4 KiB of stack.** Overflowing it hits the guard page.

The compiler options in `main/CMakeLists.txt` keep generated code within these
rules, so keep them when you edit that file.

### Adding a System Call

1. Add a `SYS_` number to `main/syscall.h`.
2. Add a case to `syscall_dispatch()` in `main/kernel_main.c`.
3. Copy user data with `vm_copy_from_user()` or `vm_strncpy_from_user()`. Never
   dereference a user pointer directly.
4. Add checks for bad arguments to `test_syscall_pointers()`.

## Debugging

OpenOCD and esptool share the USB-Serial/JTAG port, so stop OpenOCD before flashing.

```bash
. scripts/idf-env.sh
openocd -f debug.cfg
riscv32-esp-elf-gdb -ex 'target extended-remote localhost:3333' \
    build/ESP32-S31-Protected-Mode-Sample.elf   # in a second terminal
```

With memory protection on, a normal OpenOCD attach resets the chip, which destroys
the state of a live hang. Attach without the reset like this:

```bash
openocd -c "set ESP_FLASH_SIZE 0" -c "set ESP_RTOS none" -f debug.cfg
```

For CLion, the `default` CMake preset needs a toolchain named `ESP-IDF 6.1.1` whose
environment file is `~/.espressif/tools/activate_idf_v6.1.1.sh`.

## Project Layout

| Path | Contents |
|---|---|
| `main/kernel_main.c` | Startup, host tasks and syscall dispatch |
| `main/umode.S`, `main/umode.h` | Entry to and exit from U-mode |
| `main/vm.c`, `main/vm.h` | Page tables and safe access to user memory |
| `main/isolation_test.c` | The boot-time attacks |
| `main/user_main.c` | The user program and the user side of the attacks |
| `main/syscall.h` | The kernel/user ABI and address layout |
| `main/linker.lf` | Places the user image in its own page-aligned IRAM |
| `probe/` | Separate project that measures the chip's privilege features |
| `scripts/idf-env.sh` | Sets up ESP-IDF for the scripts |
| `debug.cfg` | OpenOCD configuration |

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
- **OpenOCD resets the chip on attach when memory protection is on.** See
  [Debugging](#debugging) for how to avoid it.

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
- **Tested on one chip.** Later silicon revisions may behave differently.

## License

MIT. See [LICENSE](LICENSE).
