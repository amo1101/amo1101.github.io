---
layout: post
title: "Linux Debugging Series — Phase 2: Dynamic Debug (dyndbg)"
tags:
  - linux
  - debugging
  - dynamic-debug
  - kernel
---

> `lib/dynamic_debug.c`, `include/linux/dynamic_debug.h` · Linux v6.12.34
> Verified against: RPi5 `dynamic_debug/control` export (bcm2712_defconfig, `CONFIG_DYNAMIC_DEBUG=y`)
>
> Log: [dynamic debug control snapshot](/assets/linux-debug/dyndbg_ctrl.log)

## 1. Overview

**Dynamic debug (dyndbg)** is a mechanism for selectively enabling or disabling individual
`pr_debug()`/`dev_dbg()` call sites at runtime, without rebuilding the kernel. Most kernel
subsystems and drivers ship with these calls already present in source but silent by
default — dyndbg's job is to turn specific ones on, at whatever granularity is useful:
a single line, a function, a whole file, or a whole module.

It sits at a specific point in the debugging toolkit: cheaper and lower-effort than
`ftrace`/`kprobe` (Phase 4) when the diagnostic output you need has already been written
by the driver author — but it can only surface what's already there. If a code path has no
`pr_debug()`/`dev_dbg()` call at the point you care about, dyndbg has nothing to control;
that gap is exactly what `ftrace`/`kprobe` exist to fill, since they can instrument
*any* function dynamically, not just ones with a pre-existing debug print.

In practice, it's one of the most commonly reached-for tools in real kernel driver
development and support work — driver bring-up, subsystem debugging (networking, PCIe
core, DRM are all heavily instrumented), and "enable dynamic debug and send us the log"
being a standard first step in bug reports, precisely because it requires no rebuild from
the person reproducing the issue.

## 2. How it works internally

### Compile-time transformation

With `CONFIG_DYNAMIC_DEBUG=y`, `pr_debug()`/`dev_dbg()` don't compile to either "always
print" or "compiled out." Each call site expands into a small descriptor plus a guarded
call:

```c
static struct _ddebug __attribute__((section("__dyndbg"))) descriptor = {
    .modname = KBUILD_MODNAME,
    .function = __func__,
    .filename = __FILE__,
    .format = fmt,
    .lineno = __LINE__,
    .flags = _DPRINTK_FLAGS_DEFAULT,   // disabled by default
};
if (unlikely(descriptor.flags & _DPRINTK_FLAGS_PRINT))
    __dynamic_pr_debug(&descriptor, fmt, ...);
```

Every `pr_debug()`/`dev_dbg()` call in the kernel gets one of these descriptors, placed in
a dedicated `__dyndbg` linker section. At link time, all descriptors — from the core kernel
and every built-in driver — are collected into one contiguous table. Loadable modules carry
their own `__dyndbg` section and register it into the same table at load time.

### The `control` file is a live view of that table

```bash
cat /sys/kernel/debug/dynamic_debug/control
```

Each line is one descriptor: file, line, module, function, current flags, and the format
string. This is exactly what a query matches against — nothing more, nothing less.

### What a query actually does

```bash
echo 'file amba-pl011.c +p' > /sys/kernel/debug/dynamic_debug/control
```

This parses the query, walks the table for matching descriptors, and flips the
`_DPRINTK_FLAGS_PRINT` bit in each match's `.flags` field. That's the entire mechanism —
a single-word memory write to already-allocated kernel data. No code is patched, no image
is modified. This is also why it resets to compiled-in defaults on every reboot: the flag
lives only in the running kernel's memory, never on disk.

(Some call sites additionally use jump labels/`static_key` for near-zero overhead when
disabled — that path does patch a few bytes of in-memory *code*, not the descriptor, but
it's still transient, gone on next boot, and never touches the kernel image on disk either
way.)

### Why `CONFIG_DYNAMIC_DEBUG` can't be a module

```
config DYNAMIC_DEBUG
    bool
```

Same architectural reasoning as `CONFIG_SERIAL_EARLYCON` and `CONFIG_TRACEFS`: the
infrastructure has to be compiled in and active before the module-loading subsystem even
exists, and it has to be present to receive descriptors from modules as they load later.
A feature that controls modules can't itself be one.

## 3. Enabling on RPi5

### Kernel option

```bash
zcat /proc/config.gz 2>/dev/null | grep CONFIG_DYNAMIC_DEBUG
```

If not set, enable it and rebuild:

```bash
scripts/config --enable CONFIG_DYNAMIC_DEBUG
make olddefconfig
```

`CONFIG_DYNAMIC_DEBUG` selects `CONFIG_DYNAMIC_DEBUG_CORE` automatically (per
`lib/Kconfig.debug`) — no need to enable it separately. `DYNAMIC_DEBUG_CORE` alone is a
minimal variant for out-of-tree modules wanting only `dynamic_pr_debug()`/
`dynamic_dev_dbg()` without the full `control`-file query interface; not relevant here.

### debugfs

```bash
mount | grep debugfs
# if not mounted:
sudo mount -t debugfs none /sys/kernel/debug
```

Persistent across reboots via `/etc/fstab`:
```
debugfs /sys/kernel/debug debugfs defaults 0 0
```

## 4. Query language reference

```
<selector> <selector> ... =<op><flags>
```

### Selectors (which call sites to match — AND logic when combined)

| Selector | Matches |
|---|---|
| `func <name>` | Function name |
| `file <name>` | Source file (basename or full path) |
| `module <name>` | Owning kernel module |
| `format "<string>"` | Substring of the format string |
| `line <n>` or `<n1>-<n2>` | Line number or range |

### Flags (what to do / what to include)

| Flag | Meaning |
|---|---|
| `p` | enable the print |
| `f` | include function name in output |
| `l` | include line number |
| `m` | include module name |
| `t` | include thread ID |

### Operators

| Op | Effect |
|---|---|
| `=` | set flags exactly |
| `+` | add flags |
| `-` | remove flags |

## 5. Boot-time vs runtime control

### Boot-time (cmdline)

```
dyndbg="file amba-pl011.c +p"
```

Multiple queries, semicolon-separated:
```
dyndbg="module pl011 +p;module pcie_brcmstb +p"
```

Useful when the behavior of interest happens during probe/init, before you'd have a chance
to write to `control` live.

### Runtime (debugfs)

```bash
echo 'func pl011_probe +p' > /sys/kernel/debug/dynamic_debug/control
```

Useful for anything reproducible on a running system — enable right before triggering the
behavior, disable right after, avoiding a full-boot's worth of verbose log to search
through.

## 6. Command reference

```bash
# status
cat /sys/kernel/debug/dynamic_debug/control
wc -l /sys/kernel/debug/dynamic_debug/control
grep -v '=_' /sys/kernel/debug/dynamic_debug/control      # currently enabled

# enable by selector
echo 'file amba-pl011.c +p'              > .../control
echo 'func pl011_probe +p'               > .../control
echo 'module pl011 +p'                   > .../control
echo 'file amba-pl011.c line 1200 +p'    > .../control
echo 'file amba-pl011.c line 1200-1250 +p' > .../control
echo 'format "probe" +p'                 > .../control

# combine selectors
echo 'module pl011 func pl011_probe +p'  > .../control

# extra output context
echo 'file amba-pl011.c +pflmt'          > .../control

# disable
echo 'file amba-pl011.c -p'              > .../control

# not-yet-loaded module — dyndbg's control file can't reach it;
# use a module parameter instead
modprobe some_driver dyndbg=+p

# watch live
dmesg -w
```

## 7. Constraints and gotchas

- **Only controls call sites that already exist in the descriptor table.** Nothing new can
  be instrumented — a code path with no `pr_debug()`/`dev_dbg()` has nothing for dyndbg to
  toggle, regardless of query.
- **Not-yet-loaded modules aren't in the table at all.** Their descriptors only appear once
  loaded. Use `modprobe <mod> dyndbg=<query>` (or `/etc/modprobe.d/*.conf`) to pre-apply a
  setting for the moment a module loads.
- **A query matching nothing may succeed silently.** Always verify with a follow-up `grep`
  against `control` rather than assuming a write worked.
- **Nothing persists across reboot.** Flags are runtime-only state in kernel memory; the
  boot-time `dyndbg=` cmdline is the only way to have a setting active from early boot,
  every boot.

## 8. RPi5 findings snapshot

From an exported `control` file on this board (`bcm2712_defconfig`, `CONFIG_DYNAMIC_DEBUG=y`):

- **4,177 total call sites**, 4,161 disabled by default, 16 pre-enabled (mostly
  `brcmfmac`/`brcmutil` Wi-Fi firmware/SDIO diagnostics, plus a few `init/main.c`
  initcall-blacklist messages)
- Busiest instrumented subsystems by call-site count: `bluetooth` (891), `dwc2`/USB (282),
  `xhci_hcd` (188), `bdc` (160), `usbcore` (152) — USB and Bluetooth dominate generally,
  not RPi5-specific
- Relevant to this project's hardware focus: **PCI** (~90 matching lines), **RP1** (~13),
  **`pl011`** (~7) — PCIe core is the richer target for investigating the open
  RP1/warm-reboot question from Phase 1; `pl011` remains a good minimal sanity-check target
  given its behavior is already well understood from Phase 1's log walkthrough.