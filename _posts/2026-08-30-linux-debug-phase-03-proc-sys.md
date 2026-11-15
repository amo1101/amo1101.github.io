---
layout: post
title: "Linux Debugging Series — Phase 3: /proc, /sys, and Introspection Tools"
tags:
  - linux
  - debugging
---

> Linux v6.12.34, RPi5 (BCM2712/RP1), arm64
> Verified against: live `/proc` and `/sys` tree listings on RPi5

## 1. Overview

**`/proc`** and **`/sys`** let you read live kernel and device state without any
instrumentation — no `pr_debug()` calls needed, no tracing overhead, just interfaces the
kernel already exposes. `/proc` is historically process-focused with a lot of accreted
kernel-wide state; `/sys` is a direct, structured reflection of the kernel's device/driver
model, added later specifically to give each device/bus/class its own real directory.

## 2. `/proc` breakdown

### System-wide entries

| Entry | Brief |
|---|---|
| `asound` | ALSA sound subsystem state |
| `buddyinfo` | Free memory blocks per zone/order |
| `bus` | Per-bus-type info |
| `cgroups` | Registered cgroup subsystems |
| `cmdline` | Kernel boot command line |
| `consoles` | Registered consoles and their flags |
| `cpuinfo` | Per-CPU hardware info |
| `crypto` | Registered kernel crypto algorithms |
| `devices` | Registered char/block device major numbers |
| `device-tree` | Symlink to live devicetree |
| `diskstats` | Per-block-device I/O statistics |
| `driver` | Misc driver-specific info |
| `dynamic_debug` | dyndbg's `/proc`-based interface (alt to debugfs `control`) |
| `execdomains` | Execution domains (legacy) |
| `fb` | Framebuffer device list |
| `filesystems` | Filesystem types the kernel supports |
| `fs` | Per-filesystem-type tunables |
| `interrupts` | Per-CPU IRQ counts by device |
| `iomem` | Physical memory address ranges claimed by devices |
| `ioports` | I/O port ranges claimed by devices |
| `irq` | Per-IRQ config, one dir per IRQ number |
| `kallsyms` | Kernel symbol table with addresses |
| `keys` / `key-users` | Kernel keyring contents / per-UID key quota |
| `kmsg` | Raw kernel log ring buffer |
| `kpagecgroup` / `kpagecount` / `kpageflags` | Per-physical-page cgroup/refcount/flags |
| `latency_stats` | Kernel scheduling-latency tracking (if enabled) |
| `loadavg` | System load averages |
| `locks` | Currently held file locks |
| `meminfo` | System memory usage summary |
| `misc` | Registered misc-class char devices |
| `modules` | Loaded kernel modules |
| `mounts` / `net` | Symlinks to `self/mounts` / `self/net` |
| `pagetypeinfo` | Free page counts by migrate type |
| `partitions` | Known block device partitions |
| `schedstat` | Scheduler statistics per CPU |
| `slabinfo` | Kernel slab allocator cache stats |
| `softirqs` | Per-CPU softirq counts |
| `stat` | Aggregate kernel/system statistics |
| `swaps` | Active swap devices/files |
| `sys` | Sysctl tunable tree |
| `sysrq-trigger` | Write-only — triggers SysRq magic commands |
| `sysvipc` | System V IPC objects |
| `timer_list` | Active kernel timers |
| `tty` | TTY driver/line-discipline info |
| `uptime` | System uptime and idle time |
| `version` | Kernel version string |
| `vmallocinfo` | vmalloc'd memory region details |
| `vmstat` | Virtual memory subsystem statistics |
| `zoneinfo` | Per-memory-zone detailed stats |

### Per-process (`/proc/<pid>/`) entries

| Entry | Brief |
|---|---|
| `attr` | LSM (SELinux/AppArmor) security attributes |
| `autogroup` | Scheduler autogroup nice-level control |
| `auxv` | ELF auxiliary vector passed at exec |
| `cgroup` | Cgroups this process belongs to |
| `clear_refs` | Write-only — clears page reference bits |
| `cmdline` | Command line this process was launched with |
| `comm` | Short process/thread name |
| `coredump_filter` | Bitmask controlling core dump contents |
| `cpuset` | Cgroup cpuset assignment |
| `cwd` | Symlink to current working directory |
| `environ` | Environment variables at exec |
| `exe` | Symlink to the executable binary |
| `fd` / `fdinfo` | Open file descriptors / per-fd metadata |
| `gid_map` / `uid_map` / `projid_map` | User-namespace ID mappings |
| `io` | Per-process I/O byte counters |
| `latency` | Scheduling latency stats (if enabled) |
| `limits` | Current resource limits (rlimits) |
| `loginuid` / `sessionid` | Audit login UID / session ID |
| `map_files` / `maps` / `smaps` / `smaps_rollup` | Memory mapping tables, detailed and summarized |
| `mem` | Raw process memory, seek+read/write |
| `mountinfo` / `mounts` / `mountstats` | Mount table views, detailed / simple / NFS stats |
| `net` | Process's network namespace stats |
| `ns` | Symlinks to this process's namespaces |
| `numa_maps` | Memory mappings annotated with NUMA node info |
| `oom_adj` / `oom_score` / `oom_score_adj` | OOM-killer scoring (legacy/current/adjustment) |
| `pagemap` | Virtual-to-physical page mapping table |
| `personality` | Execution domain personality flags |
| `root` | Symlink to process's filesystem root |
| `sched` / `schedstat` | Detailed / compact scheduler statistics |
| `setgroups` | User-namespace setgroups permission control |
| `stack` | Kernel-side call stack (if blocked in-kernel) |
| `stat` / `statm` / `status` | Compact / page-count / human-readable process state |
| `syscall` | Currently executing syscall and arguments |
| `task` | Per-thread subdirectories |
| `timens_offsets` | Time-namespace clock offsets |
| `timers` / `timerslack_ns` | Pending POSIX timers / timer coalescing slack |
| `wchan` | Kernel function the process is currently blocked in |

## 3. `/sys` breakdown

| Top-level dir | What it represents |
|---|---|
| `block` | Symlinks to registered block devices, pointing into `devices/` |
| `bus` | Every registered bus type (`amba`, `pci`, `platform`, `usb`, `i2c`, `spi`, ...) — devices/drivers per bus |
| `class` | Devices grouped by function rather than bus (`tty`, `net`, `gpio`, ...) |
| `dev` | Flat lookup by device number (`block/<major>:<minor>`, `char/<major>:<minor>`) |
| `devices` | The canonical device tree, organized by real parent/child bus topology |
| `firmware` | Kernel's view of firmware-provided data (`devicetree`, `fdt`) |
| `fs` | Per-filesystem-type kernel interfaces (`bpf`, `cgroup`, `pstore`, ...) |
| `kernel` | Core kernel tunables/interfaces (`debug` = debugfs, `tracing` = tracefs, `irq`, `mm`) |
| `module` | Every loaded module, exposing its parameters |
| `power` | Global system power-state controls |

## 4. Tools by scenario

### Process / system state

| Tool / interface | What it does | Example |
|---|---|---|
| `ps` | Process listing/state | `ps aux` |
| `top` / `htop` | Live process/resource monitor | `top` |
| `pgrep` / `pkill` | Find/signal processes by name | `pgrep -a sshd` |
| `lsof` | Open files/sockets/devices per process | `lsof /dev/ttyAMA10` — what's holding a device open |
| `pmap` | Human-readable per-process memory map | `pmap -x <pid>` |
| `/proc/<pid>/smaps_rollup` | Aggregated per-process memory breakdown (RSS/PSS) | `cat /proc/<pid>/smaps_rollup` |
| `/proc/<pid>/wchan`, `/proc/<pid>/stack` | Kernel function/call stack a blocked process is waiting in | `cat /proc/<pid>/wchan` — distinguishes I/O-wait from lock-wait from hung |
| `free` | System memory summary | `free -h` |
| `vmstat` | Context switches, run-queue length, I/O wait over time | `vmstat 1 5` |
| `mpstat` | Per-core CPU usage breakdown | `mpstat -P ALL 1 5` |
| `uptime` | Load average, uptime | `uptime` |

### Network state

| Tool | What it does | Example |
|---|---|---|
| `ss` | Socket state (modern `netstat` replacement) | `ss -tulpn \| grep <port>` — confirm what's listening |
| `netstat` | Legacy socket/routing table viewer | `netstat -tulpn` |
| `ip` | Interface/route/link config and status | `ip addr`, `ip route` |
| `ethtool` | NIC-level driver/link settings | `ethtool eth0` |

### Device / driver model

| Tool / interface | What it does | Example |
|---|---|---|
| `lspci` | List PCI devices | `lspci -vv` |
| `lsusb` | List USB devices/topology | `lsusb -t` |
| `udevadm info` | Full device ancestry, every attribute available for rule matching | `udevadm info -a /dev/ttyAMA10` |
| `udevadm monitor` | Live stream of uevents as devices are added/removed | `udevadm monitor --udev --property` — watch what fires on hotplug |
| `/sys/.../device/driver` | Confirms which driver is actually bound to a device | `ls -l /sys/class/tty/ttyAMA10/device/driver` |
| `/proc/iomem` | Physical address ranges claimed by devices | `grep -A2 -B2 <hex-address> /proc/iomem` — cross-check a DT `reg` value |
| `/proc/interrupts` | Per-CPU IRQ counts by device | `watch -n1 'grep <irq-number> /proc/interrupts'` — spot an IRQ storm |
| `dmesg` | Kernel ring buffer dump | `dmesg -T \| tail -50` |
| `journalctl -k` | Kernel log via systemd journal, time-range filterable | `journalctl -k -b --since "10 min ago"` |
| `sysctl` | Read/write kernel tunables under `/proc/sys` | `sysctl vm.swappiness`, `sudo sysctl -w vm.swappiness=10` |

### Hardware / topology

| Tool | What it does | Example |
|---|---|---|
| `lstopo` (hwloc) | Visualize CPU/NUMA/cache topology | `lstopo` |
| `dmidecode` | Hardware info from firmware tables (x86-centric, limited relevance on DT/ARM systems) | `dmidecode -t memory` |