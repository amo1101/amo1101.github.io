---
layout: post
title: "Linux Debugging Series — Phase 1: earlycon & Console Handoff"
tags:
  - linux-debugging
  - earlycon
  - boot
---

> `drivers/tty/serial/earlycon.c`, `kernel/printk/printk.c` · Linux v6.12.34
> Verified against: `boot_earlycon_on_tty_off.txt` (RPi5, `uart10`, clean single boot)
>
> Log: [earlycon.log](/assets/linux-debug/earlycon.log)

## 1. Overview

**earlycon** is the kernel's minimal early-boot console — it exists to give `printk()`
output a place to go before the driver core, bus subsystems, and device model are running.
Without it, nothing is visible on any output device until a real driver probes, which can
be tens of thousands of instructions and several subsystem inits away from `start_kernel()`.

Its value only shows up when something goes wrong in that early window — a hang or panic
before the real console registers leaves no live output and no later replay, since replay
only happens when a console *successfully* registers. For a clean, successful boot, earlycon
adds nothing observable to the end result; `dmesg` looks identical either way.

This makes earlycon the visibility foundation later debugging stages build on directly.
**KGDB/KDB** reuses the same UART earlycon proves out — `kgdboc=` targets the same physical
console — so a correctly configured earlycon is a prerequisite, not just a nice-to-have, for
interactive kernel debugging later in this plan.

### earlycon vs the normal console — the core distinction

| | earlycon | normal console |
|---|---|---|
| Registers via | Early param parsing, before device model | Real driver probe, via device model |
| Mapping | Fixed/early ioremap | Normal `ioremap()` |
| Mode | Polling, register-banging | IRQ-driven, buffered |
| Clocks/pinctrl | Assumed pre-configured by bootloader | Actively requested/enabled by driver |
| Lifetime | Torn down once real console takes over | Persists for the life of the system |

## 2. Two consoles, two load paths

### earlycon path

```
start_kernel()
  └─ setup_arch(&command_line)
       └─ parse_early_param()
            └─ param_setup_earlycon()          [early_param("earlycon", ...)]
                 └─ setup_earlycon()
                      ├─ match name against EARLYCON_DECLARE table
                      ├─ call <driver>_early_console_setup()  → map MMIO, set .write callback
                      └─ register_console(&device->con)        [flags = CON_BOOT]
```

`parse_early_param()` runs inside `setup_arch()`, one of the first calls in `start_kernel()`
— well before `console_init()`, `mm_init()`, or `rest_init()`. The matched driver's setup
function does the bare minimum: map the given address with a fixed early mapping, and wire
up a polling `.write` callback. No clocks, no pinctrl, no IRQ — it assumes the bootloader
already left the UART in a working state.

### normal console path

```
start_kernel()
  └─ rest_init()
       └─ do_initcalls()
            ├─ driver_init(), bus registration (device model comes up)
            ├─ of_platform_populate()           → devicetree nodes become real devices
            └─ <bus>_driver_register()
                 └─ probe: device ↔ driver compatible match
                      └─ <driver>_probe()
                           └─ uart_add_one_port() → register_console()  [no CON_BOOT flag]
```

This is a much longer dependency chain: memory management, driver core init, bus type
registration, DT population, then the actual probe. That's why a hardware-backed console
always registers substantially later than earlycon, or than any software-only console.

### Why a software console can register earlier

`console_init()` runs its own dedicated initcall section (`con_initcall`), independent of
`do_initcalls()`. A software-only console like the VT/framebuffer console needs nothing
from the device model — it's always compiled in and has no hardware to discover. It can
register as soon as the generic console-registration machinery (`console_init()`) is up,
regardless of whether any real driver has probed anything yet.

`console_init()` itself never touches a hardware-backed console like `ttyAMA10` — that
registration happens entirely through the separate, later device-model path above.

## 3. The handoff mechanism

### `CON_CONSDEV` and boot-console teardown

`register_console()` (`kernel/printk/printk.c`) contains the logic that tears down boot
consoles:

```c
if (bcon &&
    ((newcon->flags & (CON_CONSDEV | CON_BOOT)) == CON_CONSDEV) &&
    !keep_bootcon) {
    for_each_console(con)
        if (con->flags & CON_BOOT)
            unregister_console_locked(con);
}
```

Any console that registers with `CON_CONSDEV` set — meaning it's considered the system's
device console, which happens if it's named in the `console=` cmdline list — triggers
teardown of **all** boot consoles, regardless of whether it's the same physical device as
the one being torn down. This matters: if more than one preferred console is configured
(e.g. a software console alongside a hardware one), whichever registers first with
`CON_CONSDEV` triggers the teardown — which can create a gap if the real hardware console
hasn't come up yet. Keeping the cmdline to a single, correct console avoids this entirely.

### Ring buffer basics

`printk()` always writes to a central ring buffer first, unconditionally — console output is
a side effect, not the primary destination. This is why `dmesg` can show the full boot log
at any later point, independent of what consoles existed when each line was printed.

Whether a newly-registered console gets the existing backlog depends on the `CON_PRINTBUFFER`
flag:

```c
if (bcon && ((newcon->flags & (CON_CONSDEV | CON_BOOT)) == CON_CONSDEV))
    newcon->flags &= ~CON_PRINTBUFFER;
```

When a real console takes over from a boot console **on the same physical device**, the
kernel deliberately clears `CON_PRINTBUFFER` for the new console — avoiding a redundant
reprint of everything the terminal already saw live via earlycon. The ring buffer itself is
never emptied or consumed by this; every entry remains available via `dmesg` regardless.

## 4. Worked example — RPi5 `uart10`

Configuration:
```
earlycon=pl011,0x107d001000 console=ttyAMA10,115200
```

Only one console is named on the cmdline — `ttyAMA10` — so there's exactly one point where
`CON_CONSDEV` gets set and exactly one handoff.

### earlycon registers at `0.000000`

```
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x414fd0b1]
[    0.000000] Machine model: Raspberry Pi 5 Model B Rev 1.0
[    0.000000] earlycon: pl11 at MMIO 0x000000107d001000 (options '115200n8')
[    0.000000] printk: legacy bootconsole [pl11] enabled
```

`setup_earlycon()` matches `pl011`, maps `0x107d001000`, and `register_console()` enables it
with `CON_BOOT` — all before any further kernel init runs. `pl11` is the truncated 4-char
name from `struct earlycon_device`'s fixed-size name field, not a typo.

### the VT console still initializes, but doesn't interfere

```
[    0.008172] calling  con_init+0x0/0x230 @ 0
[    0.012411] Console: colour dummy device 80x25
[    0.016894] initcall con_init+0x0/0x230 returned 0 after 4000 usecs
```

`con_init()` (the VT/framebuffer console) still runs — it's unconditional, dedicated
initcall infrastructure, unrelated to `console=`. But with no `console=tty1` on the cmdline,
it never gets `CON_CONSDEV`, so it doesn't trigger the boot-console teardown logic. The
earlycon boot console stays alive continuously from here on.

### real driver probes and takes over, cleanly, in one step

```
[    2.037223] Serial: AMBA PL011 UART driver
[    2.113330] probe of 107d001000.serial:0 returned 0 after 0 usecs
[    2.125833] 107d001000.serial: ttyAMA10 at MMIO 0x107d001000 (irq = 16, base_baud = 0) is a PL011 rev3
[    2.135245] printk: legacy console [ttyAMA10] enabled
[    2.145408] printk: legacy bootconsole [pl11] disabled
```

At `2.037223s`, deep into `do_initcalls()`, the AMBA bus matches `uart10`'s
`compatible = "arm,pl011"` against the registered `pl011` driver and calls its probe
function. `ttyAMA10` registers with `CON_CONSDEV` (it's the cmdline's only named console),
which immediately triggers `pl11`'s teardown — enable and disable land within ~10ms of each
other, back to back, with no gap. Same physical device (`107d001000.serial`) throughout: the
address never changes, only the driver behind it does, from polling earlycon code to the
full IRQ-driven `pl011` driver.

Boot continues uninterrupted on the same UART through to login:
```
rp5 login:
```