---
layout: post
title: "Linux Debugging Series — Phase 4: ftrace, kprobe, tracepoints & trace-cmd"
tags:
  - linux-debugging
  - ftrace
  - kernel-tracing
---

> `kernel/trace/` · Linux v6.12.34 · RPi5 (BCM2712/RP1, arm64)
> Verified against: live tracefs listing (`rp5:/sys/kernel/tracing`), `kernel/trace/Kconfig`

## 1. Overview of tracing facilities, use scenarios

This phase covers three distinct kernel instrumentation mechanisms, plus one tool
that sits on top of all of them:

- **ftrace** (`function` / `function_graph` tracers) — traces function entry/exit
  across (almost) the whole kernel
- **kprobe / kretprobe** — dynamic breakpoint-based probing at an arbitrary
  instruction address
- **tracepoints** — static, developer-placed, typed instrumentation points
- **trace-cmd / KernelShark** — userspace tooling that drives all of the above
  via the tracefs interface and visualizes the result

`uprobe` (userspace-equivalent of kprobe) is in scope conceptually but
deprioritized for hands-on work — see §3.

### Common scenarios

| Scenario | Best-fit tool | Why |
|---|---|---|
| "Why did this take so long" — latency investigation | `function_graph` | Entry/exit pairs with duration, per call depth |
| Call-path / control-flow verification | `function_graph`, filtered | Confirms a call chain actually executes, and how deep it nests |
| Event correlation across subsystems | tracepoints (`events/sched/*`, `events/irq/*`, etc.) | Structured, typed data; safe to correlate multiple streams on one timeline in KernelShark |
| Probing a function you can't recompile | kprobe / kretprobe | Attach at runtime to arbitrary functions, e.g. a driver's `.probe()` |
| Userspace/kernel boundary | uprobe | Same mechanism as kprobe, applied to userspace binaries — optional for this series |
| Low-overhead production diagnosis | any of the above | All three are compiled in but dormant — near-zero cost until armed (see §2's jump-label discussion) |

For this project specifically, scenarios **latency investigation**, **call-path
verification**, and **probing a function you can't recompile** are the ones most
directly reachable from prior phases (boot-timing analysis from Phase 1, RP1
driver probe-path interest from Phase 2).

---

## 2. How they work? Comparison between them

### 2.1 ftrace (function / function_graph)

```
compile time                    boot time                      runtime (armed)
─────────────                   ──────────                     ────────────────
every traceable     ──patch──►  __fentry__ call    ──patch──►  __fentry__ call
function starts                 site rewritten                 site rewritten
with a call to                  to a 5-byte NOP                back to a call,
__fentry__/mcount               (CONFIG_DYNAMIC_FTRACE)         now pointing at a
                                                                 trampoline that
                                                                 logs into the
                                                                 ring buffer
```

Every kernel function begins with a compiled-in call to `__fentry__`. At boot,
if `CONFIG_DYNAMIC_FTRACE=y`, the kernel walks a table of every call site (built
at compile time) and patches each one into a NOP — this is why a tracing-capable
kernel runs at full native speed when tracing is off; the call sites are
physically gone. Arming a tracer (`echo function > current_tracer`, scoped via
`set_ftrace_filter`) re-patches selected NOPs back into calls. This live
patching is done SMP-safely so no CPU ever executes a half-written instruction.

`function_graph` is the same mechanism plus one trick: it also hijacks the
return address on the stack so it gets called again on exit — that's how it
produces matched entry/exit pairs with duration, indented by call depth.

### 2.2 kprobe / kretprobe

```
registration:  copy original instruction bytes elsewhere
               overwrite live instruction with a breakpoint (int3 / brk)

hit:           CPU traps ──► registers saved (pt_regs) ──► pre_handler runs
               ──► saved copy of original instruction single-stepped
                   out-of-line (never re-exposes the real instruction mid-step)
               ──► execution resumes after the probe point

kretprobe:     same idea as function_graph — hijacks the return address,
               redirects through a trampoline that has the return value
```

kprobe patches a **live breakpoint** at an arbitrary address — no compile-time
cooperation needed, which is exactly why it can probe functions ftrace's
function tracer never touched specially. The single-step-out-of-line step
exists so the breakpoint doesn't have to be removed even briefly — removing it
would open a window where another CPU could sail straight past the probe point.
Once proven safe, an optimized kprobe can replace the trap with a direct jump
into a detour buffer, cutting overhead roughly 5–10x versus a plain trap.

**Why kprobe can't export local variables:** the handler only ever sees raw
CPU registers (`pt_regs`) at the trap point — no symbol table, no notion of
"this is variable `size`," only "whatever's in `%x0` right now." Local variable
placement is entirely the compiler's call and isn't stable — a variable can live
purely in a register, get spilled to stack, or be optimized away entirely, and
that placement can shift across even minor compiler versions. The workaround is
`perf probe` with DWARF debug info (`CONFIG_DEBUG_INFO=y`), which resolves named
variables to their register/stack location *at that specific line* by reading
the compiler's own debug tables — but it's still best-effort and can fail if a
variable was optimized out of existence.

### 2.3 tracepoints

Static, hand-placed annotations compiled directly into the kernel source at
points a developer judged meaningful (`sched_switch`, `block_rq_issue`, etc.).
Defined via the `TRACE_EVENT()` macro, which expands into a function prototype
at the callsite, a struct for ring-buffer storage, developer-written assignment
code (`TP_fast_assign`) that copies live local variables into that struct as
ordinary compiled C, and a printk-style formatter.

This is *why* tracepoints can carry local variables when kprobe can't — the
extraction code is written by the person who placed the tracepoint, compiled
inline at that exact point, resolved by the compiler like any other C
statement. There's no runtime guessing involved.

Tracepoints are also treated as a **stable ABI** — a tool reading
`sched_switch`'s `prev_pid`/`next_pid` fields keeps working release after
release. kprobe has no such guarantee: it probes a function by name/address,
and if that function is renamed, inlined, or restructured, the probe silently
breaks.

### 2.4 Jump labels / static keys — the shared zero-overhead mechanism

```
disabled state:    [ no-op ]  ◄── one 5-byte NOP, nothing evaluated at runtime
                       │
                       │  probe/tracepoint/tracer armed
                       ▼
enabled state:     [ jmp trampoline ]  ◄── live-patched, unconditional jump
```

Both dynamic ftrace and tracepoints ultimately rely on the same trick to stay
cheap when disabled: instead of a runtime `if (enabled)` check — which always
costs a load + compare + branch, however small — the kernel physically patches
the machine code at the callsite. Disabled: a single NOP. Enabled: the NOP gets
overwritten with a real jump. There's nothing to *evaluate* in either state,
because the code itself was rewritten to already contain the answer. This is
the same "patch the machine code, don't branch on it" philosophy ftrace uses
for `__fentry__` call sites, applied instead to a boolean gate — and it's why
subsystem maintainers can scatter tracepoints liberally through hot paths
(e.g. `sched_switch`, which fires on every context switch) at effectively zero
idle cost. kprobe doesn't use this mechanism by default (it uses a trap), which
is exactly why the "optimized kprobe" jump-based variant exists — it's kprobe
borrowing this same idea once a probe's been proven safe.

### 2.5 Comparison table

| | ftrace (function tracer) | kprobe | tracepoint |
|---|---|---|---|
| Where it can attach | Any compiled function entry | Almost any instruction address | Only pre-placed developer annotations |
| Mechanism | Patch NOP ↔ call at `__fentry__` site | Patch breakpoint (`int3`/`brk`) into live instruction | Patch NOP ↔ jump via static key |
| Data captured | Entry/exit + timing only | Raw registers at trap point, or return value | Rich, typed, developer-chosen fields |
| Stability across kernel versions | Depends on function still existing | None — probing raw address/symbol | Treated as stable ABI |
| Setup cost to add one | None — compiled in universally | None — attach at runtime | Requires a kernel source patch |

---

## 3. How to enable them?

### Kconfig options

| Tool | Config option | Depends on | Notes |
|---|---|---|---|
| Basic function tracer | `CONFIG_FUNCTION_TRACER` | `HAVE_FUNCTION_TRACER` (arch support) | |
| Call-graph tracer | `CONFIG_FUNCTION_GRAPH_TRACER` | `FUNCTION_TRACER` + `HAVE_FUNCTION_GRAPH_TRACER` | Defaults to `y` if deps met |
| Runtime enable/disable per-function | `CONFIG_DYNAMIC_FTRACE` | `FUNCTION_TRACER` + `HAVE_DYNAMIC_FTRACE` | Defaults to `y`; this is what gives live `set_ftrace_filter` |
| kprobe/kretprobe dynamic events | `CONFIG_KPROBE_EVENTS` | `KPROBES` + `HAVE_REGS_AND_STACK_ACCESS_API` | Defaults to `y` |
| uprobe dynamic events | `CONFIG_UPROBE_EVENTS` | `ARCH_SUPPORTS_UPROBES`, `MMU`, `PERF_EVENTS` | Defaults to `y` — **not present** on RPi5's shipped kernel (see below) |
| Boot-time tracing (`ftrace=`, `trace_event=`) | `CONFIG_BOOTTIME_TRACING` | `TRACING`, selects `BOOT_CONFIG` | See §4's bootconfig note |
| Synthetic/histogram events | `CONFIG_SYNTH_EVENTS` | — | **Not present** on RPi5's shipped kernel |

All the arch-support `HAVE_*` symbols (`HAVE_FUNCTION_TRACER`,
`HAVE_DYNAMIC_FTRACE`, `HAVE_FUNCTION_GRAPH_TRACER`,
`HAVE_REGS_AND_STACK_ACCESS_API`) are satisfied on arm64 — nothing here is
x86-only.

`trace-cmd` itself is **not a kernel option** — it's a userspace CLI package
that talks to the tracefs interface above. Nothing to enable in Kconfig for it.

### RPi5 tracefs layout — confirmed findings

```
/sys/kernel/tracing/
├── current_tracer, available_tracers            ─┐
├── set_ftrace_filter, set_ftrace_notrace          │  function / function_graph
├── set_graph_function, max_graph_depth            │  tracer: PRESENT
├── dyn_ftrace_total_info, enabled_functions,      │
│   touched_functions                             ─┘  (confirms DYNAMIC_FTRACE)
├── kprobe_events, kprobe_profile                 ──  kprobe layer: PRESENT
├── events/, available_events, set_event          ──  tracepoints: PRESENT
├── instances/                                    ──  multi-buffer support: PRESENT
│
├── uprobe_events            ✗ MISSING  ──► CONFIG_UPROBE_EVENTS not set
└── synthetic_events         ✗ MISSING  ──► CONFIG_SYNTH_EVENTS not set
```

Everything needed for this phase's core deliverables (function_graph tracing,
kprobe events) is present and enabled out of the box on the Raspberry Pi OS
kernel. The two gaps (`uprobe_events`, `synthetic_events`) are both features
already deprioritized for this series — uprobe was marked optional, and
synthetic/histogram events weren't in scope. If either becomes relevant later
(e.g. cross-event latency correlation in Phase 5), it would require a kernel
rebuild, not just a runtime toggle.

---

## 4. How to use them? Practical commands

### 4.1 Raw tracefs interface — function tracer

```bash
# 1. Confirm tracefs is mounted (v6.12 mounts it directly at /sys/kernel/tracing)
mount | grep tracefs

# 2. Check what's available
cat available_tracers
cat available_filter_functions | head

# 3. Select a tracer (default is 'nop' — no tracer active)
echo function_graph > current_tracer

# 4. Scope it — tracing every function floods the buffer instantly
echo some_function > set_ftrace_filter
echo some_function > set_graph_function   # function_graph-specific, scopes by call-tree root

# 5. Start capture
echo 1 > tracing_on

# 6. Read output
cat trace          # non-consuming snapshot
cat trace_pipe      # consuming, blocking stream

# 7. Reset
echo 0 > tracing_on
echo nop > current_tracer
echo > set_ftrace_filter
```

**`trace` vs `trace_pipe` vs `trace_pipe_raw`:**

| File | Format | Consuming? | Mechanism |
|---|---|---|---|
| `trace` | Human-readable, rendered on read | No — safe to re-read; briefly pauses tracing while open | `seq_file`, formats ring-buffer entries live |
| `trace_pipe` | Human-readable, same formatting | Yes — each read removes what it returned | Blocking read, streams as new data arrives |
| `trace_pipe_raw` (per-CPU) | Raw binary, ring buffer's native layout | Yes | `splice()`, zero-copy — what `trace-cmd record` uses |

**Per-CPU buffers and the merge:** the kernel keeps one ring buffer per CPU
(lock-free writes, no cross-CPU contention). Reading the top-level `trace` file
merges all per-CPU buffers into one chronologically-ordered stream at read
time, via a merge-sort over each CPU's locally-ordered entries — the `CPU#`
column shows which buffer each line actually came from. `per_cpu/cpuN/trace`
gives one CPU's data unmerged. Because merge ordering depends on comparable
timestamps across CPUs, and the default `local` clock is fast but not
guaranteed monotonic across cores, `echo global > trace_clock` (or `counter`,
an atomic cross-CPU counter) is the safer choice when strict cross-CPU event
ordering matters.

### 4.2 kprobe_events

```bash
# 1. Define a probe — p: for entry, r: for return (kretprobe)
#    format: p:[GRP/]EVENT FUNC[+offs] [ARGS]
echo 'p:myprobe some_function' > kprobe_events

# 2. Optionally capture arguments (arm64 uses %x0-%x7, NOT x86's %di/%si)
echo 'p:myprobe some_function arg1=%x0' > kprobe_events

# 3. Confirm creation
ls events/kprobes/myprobe/

# 4. Enable
echo 1 > events/kprobes/myprobe/enable
echo 1 > tracing_on

# 5. Read
cat trace
cat kprobe_profile     # hit counts only

# 6. Disable and remove
echo 0 > events/kprobes/myprobe/enable
echo '-:myprobe' > kprobe_events
```

Without `CONFIG_DEBUG_INFO` and a matching `vmlinux`, argument capture is
limited to raw register fetches by position — named-variable capture via
`perf probe` requires debug info to be present (unconfirmed on the RPi5 kernel
as of this writing).

### 4.3 trace-cmd

```bash
trace-cmd list -t                 # available tracers/plugins
trace-cmd list -e sched            # available events, filtered by subsystem

# function_graph, scoped, one command instead of the manual current_tracer +
# set_ftrace_filter sequence above
trace-cmd record -p function_graph -l some_function

# tracepoint-based capture
trace-cmd record -e sched

# filter to a specific command or PID
trace-cmd record -p function_graph -F some_command
trace-cmd record -e sched -P 1234

trace-cmd report                   # read back trace.dat as text
trace-cmd reset                    # disable all tracing, clear buffers
kernelshark trace.dat              # open the same file in the GUI
```

**How `record` actually captures data:** ftrace's ring buffers are per-CPU, so
`trace-cmd record` spawns one collector process per CPU. Each process reads its
CPU's `trace_pipe_raw` file via `splice()` — a zero-copy kernel-to-kernel
transfer that never routes the trace bytes through userspace memory — into its
own temporary file, entirely during the capture window, with no cross-CPU
coordination needed:

```
CPU0 ring buffer ──splice()──► temp file 0   ┐
CPU1 ring buffer ──splice()──► temp file 1   │  during capture,
CPU2 ring buffer ──splice()──► temp file 2   │  fully independent,
CPU3 ring buffer ──splice()──► temp file 3   ┘  lock-free

                    │  capture stops (Ctrl-C / command exits)
                    ▼
        concatenate temp files → trace.dat
                    │
                    ▼
   trace-cmd report / kernelshark  ──►  parse + timestamp-merge on demand
```

The binary format is the kernel's own ring-buffer layout, unmodified — no
formatting work happens during capture at all; that's deferred entirely to
`report`/KernelShark, which parse and interleave-by-timestamp only when someone
actually reads the file. This mirrors the same "expensive work happens once, at
enable/disable or read time, not on every event" philosophy as jump labels and
NOP-patching.

### 4.4 KernelShark

Pure front-end reader for `trace.dat` — no separate capture mechanism, so
everything shown is exactly what `trace-cmd record` already produced.

- **Graph view** — one row per CPU, event marks over time; makes cross-CPU
  timing patterns (gaps, bursts) visible in a way scrolling text output can't.
- **List view** — same content as `trace-cmd report`'s text, but synced to the
  graph: clicking an event in one jumps to the matching entry in the other.
- **Latency lists** — sorted views of scheduling/wakeup latency.
- **Plugins** (e.g. `EventFieldPlot`) — can plot a captured kprobe argument's
  value over time directly, rather than reading raw numbers row by row.

### 4.5 Boot-time tracing — cmdline vs bootconfig (noted, not exercised this phase)

Two tiers exist for capturing trace data before userspace (and thus tracefs)
is even up, gated by `CONFIG_BOOTTIME_TRACING`:

- **Kernel cmdline params** (`kernel.`-prefixed style, e.g.
  `ftrace=function_graph trace_event=sched:sched_switch`) — parsed during
  `parse_early_param()` in `setup_arch()`, the same early-param path earlycon
  uses (Phase 1). This is the only tier early enough to catch anything before
  `core_initcall`.
- **bootconfig file** (`ftrace.`-prefixed keys, e.g. `ftrace = function`,
  `ftrace_filter = "vfs*"`) — a separate structured config attached to the
  kernel image, needed for anything richer than a flat function/event list
  (filters, kprobe definitions, histograms). Activates only at the end of
  `core_initcall`, meaningfully later than the cmdline tier.

This capability exists and was discussed, but is set aside as out of scope for
this phase's hands-on work — the deliverable here targets live, on-demand
tracing (function_graph/kprobe on a running system), not early-boot capture.

---

## 5. Tracers beyond `function` / `function_graph`

`current_tracer` accepts several other tracer plugins beyond the two covered
in §2.1. They're all part of the ftrace subsystem — same registration
(`register_tracer()`), same `current_tracer` selection, same per-CPU ring
buffer and `trace`/`trace_pipe` read path — but most don't use the
`__fentry__`/NOP-patching mechanism as their detection source. Worth a survey
for completeness, and worth being precise about which parts are "ftrace" vs
which are "the function-hooking mechanism specifically."

### 5.1 Latency-hunting tracers

These continuously monitor the system and only keep a trace when it beats the
previous worst case, stored in `tracing_max_latency` — instead of a flood of
every event, you get exactly one thing: the worst offender seen so far.

| Tracer | What it hunts | Detection hook (not `__fentry__`) |
|---|---|---|
| `irqsoff` | Longest stretch with IRQs disabled | `trace_hardirqs_off()`/`on()` — dedicated call sites at IRQ enable/disable, not generic function entry |
| `preemptoff` | Longest stretch with kernel preemption disabled | `preempt_count_add()`/`sub()` |
| `preemptirqsoff` | Longest stretch where neither IRQs nor preemption allow scheduling — the union `irqsoff`/`preemptoff` individually miss | Both of the above |
| `wakeup` | Latency between a task waking up and actually being scheduled | Scheduler wakeup/context-switch path (`try_to_wake_up()`, `schedule()`) |
| `wakeup_rt` | Same as `wakeup`, scoped to `SCHED_FIFO`/`SCHED_RR` tasks | Same |
| `wakeup_dl` | Same again, scoped to `SCHED_DEADLINE` tasks | Same |

Each of these has an `options/function-trace` toggle that layers the full
function tracer on top, showing the actual call chain during the latency
window — off by default, since these tracers are designed to be cheap enough
to run continuously, and full function tracing would defeat that.

### 5.2 Hardware/system-noise tracers

- **`hwlat`** — doesn't hook any code path at all. Runs a dedicated kernel
  thread that busy-loops with interrupts disabled, polling the timestamp
  counter for gaps only explainable by hardware (SMI or similar) — a
  measurement thread, not an instrumented call site.
- **`osnoise`** — same "dedicated sampling thread" model, generalized to
  measure OS-level interference (other threads, IRQs, softirqs) preventing a
  thread from running uninterrupted.
- **`timerlat`** — built on top of `osnoise` (same `osnoise/` config
  directory); measures the timer subsystem's own latency in firing.

### 5.3 Domain-specific tracers

- **`blk`** — the block-I/O tracer used by the `blktrace` userspace
  application; traces disk I/O request lifecycle. Distinct from the separate
  `block:*` tracepoint subsystem.
- **`mmiotrace`** — traps every memory-mapped I/O read/write via page-fault
  injection (marks ioremapped pages not-present) rather than any function or
  tracepoint hook. `CONFIG_MMIOTRACE` depends on `HAVE_MMIOTRACE_SUPPORT`,
  historically an x86-only symbol — **likely unavailable on the RPi5's arm64
  kernel**, unconfirmed pending an `available_tracers` check on the Pi.
- **`branch`** — branch-prediction hit/miss statistics; niche
  microarchitecture work, not general debugging.

---

## 6. Userspace tracing facilities

Everything above operates inside the kernel. This phase's tooling extends one
layer up via a small number of distinct mechanisms — worth a survey for
completeness, even though hands-on work stayed kernel-side.

### 6.1 uprobe — the userspace counterpart to kprobe

Same core mechanism as kprobe (§2.2), just applied to a userspace binary's
code instead of kernel text: the kernel injects a breakpoint (`int3`/`brk`)
into the target process's mapped code, traps on hit, and hands off to
whatever's listening. This is the `uprobe_events` tracefs file — confirmed
**absent** on the RPi5 kernel build in §3, so this mechanism isn't usable here
without a kernel rebuild (`CONFIG_UPROBE_EVENTS=y`).

### 6.2 USDT — the userspace counterpart to tracepoints

"Userland Statically Defined Tracing" — the developer manually annotates
their source with a probe macro (`DTRACE_PROBE`-family, from `sys/sdt.h`),
the same static/stable-ABI tradeoff as kernel tracepoints (§2.3) vs kprobe's
fragility. Mechanically a USDT probe is implemented as a uprobe at a fixed,
pre-marked address — so it inherits uprobe's kernel-side dependency (§6.1)
even though the annotation lives in userspace source.

### 6.3 ptrace-based tools — strace and ltrace

A genuinely separate mechanism from everything above — not kprobe/uprobe/
tracepoint-derived at all, but built on the older `ptrace(2)` syscall, which
lets one process observe and control another's execution, memory, and
registers.

- **strace** attaches via `ptrace` and is notified by the kernel at every
  syscall entry/exit boundary — no code patching needed, since syscall
  tracing is a built-in `ptrace` mode. Works even on statically linked
  binaries, since syscalls are unavoidable regardless of linking.
- **ltrace** traces shared-library calls instead. It parses the target's ELF
  file to find the PLT (Procedure Linkage Table — the indirection used to
  call shared-library functions whose real address isn't known until
  runtime), then uses `ptrace`'s `PTRACE_POKETEXT` to overwrite each PLT
  entry's trampoline with an `int3`. On hit, it records the call, restores
  the original instruction, and resumes — the same "patch a trap in, catch
  it, restore, resume" pattern as kprobe, just implemented entirely in
  userspace via `ptrace` rather than as a kernel facility. Because it hooks
  the PLT specifically, it can't see calls into statically-linked libraries
  — there's no indirection table to hook.

**Cost, and why this matters:** every `ptrace`-based event requires a full
tracer/tracee context-switch round-trip, which is significant overhead —
neither tool is suitable for continuous production use. This is a real
mechanism-level difference from kprobe/uprobe/tracepoints, which (thanks to
jump-label gating, §2.4) cost close to nothing when idle and stay in-kernel
even when armed. It's also why eBPF-based tools have largely superseded
ptrace-based tracing for performance-sensitive work — an eBPF-based ltrace
equivalent avoids the per-call context-switch entirely.

### 6.4 Full frameworks — pulling multiple sources together

These aren't new data sources so much as frontends that combine kprobe,
uprobe, tracepoints, and USDT into one tool:

| Tool | Model |
|---|---|
| **SystemTap** | Custom scripting language, compiled to C, built as a kernel module, then loaded — powerful, but heavier to set up than the eBPF-based tools below |
| **LTTng** (`lttng-ust` for the userspace-only half) | Correlated kernel + userspace traces at low overhead; `lttng-ust` compiles probes directly into the program, no kernel module needed for the userspace-only path |
| **bpftrace / BCC (eBPF-based)** | Compiles scripts to BPF bytecode, drawing on kprobes, uprobes, tracepoints, and USDT as data sources; `bpftrace` suits ad hoc one-liners, BCC suits complex long-running tools — this is what Phase 5 builds on |
| **sysdig** | Script-driven (Lua "chisels"), similar spirit to SystemTap |

### 6.5 Where this fits the picture so far

```
kernel-side (Phase 4 core):   __fentry__ hooks ─ kprobe traps ─ tracepoints
                                        │
                              same breakpoint/NOP-patch philosophy,
                              extended one layer up
                                        │
userspace-side (this section): uprobe ─ USDT ─ ptrace (strace/ltrace, separate lineage)
                                        │
                              consumed by frontends: SystemTap, LTTng, bpftrace/BCC, sysdig
```

`uprobe` and USDT are direct extensions of the kprobe/tracepoint ideas into
userspace; `strace`/`ltrace` are mechanistically unrelated, built on the
older and heavier `ptrace` primitive. All of it converges into the same
frontend tools — `bpftrace`/BCC in particular is the natural bridge into
Phase 5's eBPF work.

---
## Q & A

**Why can't kprobe access a function's local variables the way a tracepoint can?**
A bare kprobe only receives `pt_regs` — raw CPU registers at the trap address —
with no symbol table telling it where a named variable lives. Variable
placement (register vs stack vs eliminated entirely) is decided by the
compiler and isn't stable across even minor compiler-version changes.
Tracepoints don't have this problem because the extraction code is written by
the developer, compiled inline at the exact source point, and resolved like
any other C statement. The workaround for kprobe is `perf probe` with DWARF
debug info (`CONFIG_DEBUG_INFO=y`), which looks up a named variable's
register/stack location at a specific line from the compiler's own debug
tables — but it's best-effort and can fail if the variable was optimized away.

**What's the difference between a jump label and a static key?**
"Static key" is the on/off object/API (`static_key_enable()` /
`static_key_disable()`); "jump label" is the underlying technique that makes
checking it cheap — patching the callsite's machine code directly (NOP when
disabled, unconditional jump when enabled) instead of evaluating a runtime
`if` on every call. The two terms are used almost interchangeably in practice.

**Where does the content of `/sys/kernel/tracing/trace` actually come from?**
The same per-CPU ftrace ring buffers as every other trace file — `trace`,
`trace_pipe`, and `trace_pipe_raw` are three different read interfaces onto one
underlying data source, not three separate buffers. `trace` is rendered live
via `seq_file` (formatted text, generated at read time) and is non-consuming —
reading it doesn't remove entries, though it does briefly pause tracing while
the file is open. `trace_pipe` and `trace_pipe_raw` both consume what they
return.

**Does `cat trace` merge all the per-CPU ring buffers together?**
Yes — reading the top-level `trace` file walks every per-CPU buffer and
interleaves entries by timestamp into one chronological stream, via a
merge-sort in the kernel's trace iterator. This is a read-time merge, not a
write-time one. `per_cpu/cpuN/trace` gives one CPU's data with no merge. This
differs from `trace-cmd record`, which does **not** merge at capture time —
each CPU's `splice()` output goes to its own temp file, concatenated only
after capture stops; the timestamp-based merge for that path happens later, at
`report`/KernelShark time.

**Why does trace-cmd spawn one process per CPU instead of reading a single stream?**
Because the kernel itself keeps one independent, lock-free ring buffer per CPU
— there's no single stream to read in the first place. One dedicated collector
process per CPU maps 1:1 onto that layout, and each one draining only its own
buffer avoids introducing cross-CPU locking into the exact activity being
measured.