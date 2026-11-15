---
layout: post
title: "Linux Debugging Series — Phase 6: eBPF"
tags:
  - linux-debugging
  - ebpf
  - kernel-tracing
---

> Kernel v6.12.34, arm64 (Cortex-A76, BCM2712/RP1)
> Source references: `kernel/bpf/verifier.c`, `kernel/bpf/syscall.c`, `kernel/bpf/core.c`,
> `arch/arm64/net/bpf_jit_comp.c`, `kernel/trace/bpf_trace.c` · elixir.bootlin.com/linux/v6.12.34

## 1. Overview & Scope

eBPF is covered here as a **debugging/tracing tool riding on infrastructure already
established in Phase 4** (kprobe, uprobe, tracepoint) — not as a new instrumentation
surface. This phase does not introduce a new class of hook; it introduces a safe,
JIT-compiled execution engine that attaches to the hooks that already exist.

Networking (XDP, tc classifier) and security (LSM, cgroup) program types are noted
for completeness in §4 but are out of scope for hands-on work in this series —
tracing/debugging is the focus throughout.

## 2. Core Framing

**eBPF is an execution engine, not a hook mechanism.** It does not create new
attachment points in the kernel; it lets arbitrary (verified) user-supplied code
run at points the kernel already exposes — the same kprobe/uprobe/tracepoint
infrastructure documented in Phase 4. Understanding eBPF is therefore understanding
what happens to a program *between* those existing hooks and actual execution:
compile, verify, JIT, attach, run.

```
+-------------------------------+
| Phase 4: kprobe / uprobe /    |
| tracepoint infrastructure     |
| (the hook fires -- unchanged) |
+-------------------------------+
                |
                v
+-------------------------------+
| Phase 6: verify, JIT compile, |
| attach, run as native code,   |
| write map (the engine)        |
+-------------------------------+
```

eBPF's whole contribution is everything on the right — the hook on the left is
unchanged, untouched, and already understood.

## 3. Full Mechanism — Source to Running Code

**Compile → verify → JIT:**

```
+------------------------------------+
| .c source (C subset, BPF target)   |
+------------------------------------+
                   |  clang -g (sec 3.1)
                   v
+------------------------------------+
| ELF object: bytecode + BTF (if -g) |
+------------------------------------+
                   |  bpf(BPF_PROG_LOAD) + verifier (sec 3.2/3.3)
                   v
+------------------------------------+
| verified prog: CFG + type          |
| tracking proven safe               |
+------------------------------------+
                   |  JIT compile (sec 3.4)
                   v
+------------------------------------+
| native image: arm64 machine code   |
+------------------------------------+
```

**Attach → run → export (continuing from the native image above):**

```
+----------------------------+
| existing kprobe / uprobe / |
| tracepoint hook            |
+----------------------------+
               |  fires (sec 3.5)
               v
+----------------------------+
| trace_call_bpf()           |
| -> prog->bpf_func(ctx)     |
| (direct native call)       |
+----------------------------+
               |  writes result (sec 3.6)
               v
+----------------------------+
| BPF map                    |
| mmap'd to userspace tool   |
+----------------------------+
```

### 3.1 Compile (userspace, build time)

`clang -target bpf -c prog.c -o prog.o` compiles a restricted C subset to eBPF
bytecode — a fixed 64-bit instruction encoding, 11 registers (R0–R10) — stored in
an ELF object. Program code goes in named `SEC()` sections, map definitions in
`.maps`. If built with `-g`, a `.BTF`/`.BTF.ext` section records type information
for every struct field the program touches — this is what CO-RE (§5) relies on.

### 3.2 Load-time processing (userspace: libbpf or equivalent)

- Each map is created up front via `bpf(BPF_MAP_CREATE, ...)` (`kernel/bpf/syscall.c`),
  returning a map fd.
- Map-fd placeholders in the raw instruction stream get patched to real fds
  (`BPF_PSEUDO_MAP_FD`).
- If CO-RE relocations are present, field-offset immediates are rewritten against
  the *target* kernel's live BTF (`/sys/kernel/btf/vmlinux`) rather than the
  compile-time struct layout.
- `bpf(BPF_PROG_LOAD, ...)` hands the kernel the final instruction array, program
  type, and (optionally) attach target.

### 3.3 Verifier (`kernel/bpf/verifier.c`)

Runs before anything else can happen:
- `check_cfg()` builds a control-flow graph; rejects unbounded loops, unreachable
  code, stack-depth overflow.
- `do_check()` symbolically walks every path, tracking each register's type
  (scalar, map pointer, packet pointer, context pointer, etc.) and value-range
  bounds. Every pointer dereference and helper call is checked against what the
  register is currently proven to hold — an unproven path (e.g. a possibly-NULL
  pointer used without a check) is a **load-time rejection**, not a runtime fault.
- Helper calls are bound to real kernel function addresses; some instructions are
  rewritten/expanded (`bpf_patch_insn_single`).

Nothing downstream re-checks safety — JIT and runtime both trust the verifier's proof.

### 3.4 JIT compile (`arch/arm64/net/bpf_jit_comp.c`)

- **Register mapping:** `bpf2a64[]` pins each of the 10 general-purpose BPF
  registers to a fixed ARM64 GPR (e.g. R0 return value → x7, R1–R5 helper-call
  args → x0–x4) — no register allocation needed.
- **Multi-pass compilation:**
  1. Dry-run pass(es) — `build_body()` walks instructions with `ctx->image = NULL`,
     only computing native-instruction count / image size.
  2. `bpf_jit_binary_alloc()` allocates the executable buffer once size is known.
  3. Final pass re-runs `build_prologue()`/`build_body()`/`build_epilogue()`,
     actually encoding instructions into the image.
  4. **arm64-specific extra pass** for BPF-to-BPF calls — a callee's address isn't
     known until all subprogs are JITed, so arm64 needs one more pass than x86 to
     patch call offsets.

```
+-------------------------------+
| pass 1 (dry run): walk insns, |
| image=NULL, just count bytes  |
+-------------------------------+
                |  image size now known
                v
+-------------------------------+
| pass 2 (final): alloc image,  |
| re-walk, actually encode      |
+-------------------------------+
                |  only if calling another BPF subprog
                v
+-------------------------------+
| pass 3 (arm64 only): patch    |
| BPF-to-BPF call offsets       |
+-------------------------------+
```
*Pass 3 (arm64-only) only runs if the program calls another BPF subprog — x86 doesn't need it.*

- **Prologue/epilogue:** push FP/LR + callee-saved regs (AAPCS), set up a
  `BPF_FP`-relative stack frame sized from `stack_depth`, zero the tail-call-count
  register; epilogue mirrors this, leaving return value in x7.
- Final image is marked executable, W^X enforced; `prog->bpf_func` points at it.
  Fallback is the bytecode interpreter (`__bpf_prog_run()`) if JIT is disabled
  (`bpf_jit_enable` sysctl) or hits an unsupported case.

### 3.5 Attach — existing hooks only

- **kprobe:** `perf_event_open()` + `ioctl(..., PERF_EVENT_IOC_SET_BPF, prog_fd)`,
  or the newer kprobe-multi `BPF_LINK_CREATE` API. Either way, the kprobe's
  existing trap/single-step handler ends up calling `trace_call_bpf()`
  (`kernel/trace/bpf_trace.c`).
- **tracepoint:** `bpf_raw_tracepoint_open()` registers directly against the
  static tracepoint's probe list.
- **uprobe:** same shape as kprobe, userspace breakpoint instead of kernel.

No new trap/interrupt mechanism is introduced — attach means "call this native
function pointer when this pre-existing hook fires."

### 3.6 Runtime firing

Hook fires → existing handler runs → `trace_call_bpf()` sets the per-CPU
`bpf_prog_active` recursion guard, builds the context struct (e.g. `pt_regs *`
for a kprobe), calls `prog->bpf_func(ctx)` directly — native call, no dispatcher,
no interpreter. Map helper calls inside the program are themselves direct calls
to kernel functions, resolved at JIT time (§3.4).

### 3.7 Teardown

Closing the prog fd (and any perf-event/link fd holding the attach) drops the
refcount; prog and maps are freed once it hits zero. No persistent state survives.

## 4. Ecosystem Landscape

**Program types relevant here:** `BPF_PROG_TYPE_KPROBE`, `TRACEPOINT`,
`PERF_EVENT`, `RAW_TRACEPOINT`. (Out of scope: `XDP`/`SCHED_CLS` — networking;
`LSM`/`CGROUP_*` — security.)

**Map types for tracing:** `PERCPU_*` variants (avoid cross-CPU cache contention —
default choice for a counter hit from every CPU), `STACK_TRACE` (kernel stack
unwinding via `bpf_get_stackid()`), `RINGBUF` (since 5.8 — single-producer /
multi-consumer streaming to userspace, no polling needed).

**Loaders:** libbpf (C, reference implementation — CO-RE originates here);
cilium/ebpf (Go) and Aya (Rust) implement the same ELF/BTF contract in other
languages — context only.

**Frontends:**
- `bpftrace` — DSL (`kprobe:func { ... }`), own LLVM-based backend, loads via
  libbpf underneath. **Hands-on target tool for this phase.**
- BCC — pre-CO-RE (circa 2016), embeds a full Clang and compiles against the
  *running* kernel's headers at load time; superseded by CO-RE/bpftrace for new
  work, kept here for historical contrast with §5.
- `bpftool` — kernel-tree introspection CLI (`prog list/dump`, `map dump`,
  `btf dump`) — the natural first step to see what the kernel already exposes
  before writing anything.

**Version gating on v6.12.34:** ring buffer (5.8+), BTF/CO-RE (5.2+), sleepable
programs (5.10+), kfuncs (5.13+) — all mature and available; nothing here is
version-blocked on this kernel.

```
+----------------------------------------+
| bpftrace (DSL) / BCC (superseded)      |
+----------------------------------------+
                     |  loads via
                     v
+----------------------------------------+
| libbpf / cilium/ebpf (Go) / Aya (Rust) |
+----------------------------------------+
                     |  attaches as
                     v
+----------------------------------------+
| kprobe / uprobe / tracepoint program   |
| + PERCPU / RINGBUF / STACK_TRACE maps  |
+----------------------------------------+
```
`bpftool prog/map/btf dump` inspects any of the above without loading anything new — the
natural step 0 before writing a single line of new code.

## 5. CO-RE (Compile Once – Run Everywhere)

**Problem:** A BPF program reading a raw kernel struct (e.g. `task_struct->pid`)
is trusting a specific memory layout. That layout isn't a stable ABI — fields
shift offset, get renamed (`thread_struct`'s `fs` → `fsbase` between 4.6/4.7), or
get compiled out depending on Kconfig. A binary built against one kernel's
headers can silently misread on another.

**BCC's pre-CO-RE answer and its cost:** embed the BPF C source as a string,
compile it with an embedded Clang against the target's installed headers at
load time. Works, but: large embedded toolchain shipped with the app, real CPU
cost recompiling on every start, hard dependency on `kernel-devel` headers being
present on the target — a genuine problem for a custom-built kernel with no
such package, which is this board's situation.

**Four cooperating components:**
1. **BTF** — compact type format (~100x smaller than DWARF), built via
   `CONFIG_DEBUG_INFO_BTF=y`, exposed live at `/sys/kernel/btf/vmlinux` — the
   authoritative layout of the *actual running* kernel, more complete than any
   header package.
2. **Compiler relocations** — Clang builtins record *intent* at compile time
   ("field `pid`, type `pid_t`, in `task_struct`") rather than a literal offset.
3. **libbpf as loader** — matches recorded relocations against the target
   kernel's live BTF, patches correct offsets at load time.
4. **Kernel** — unchanged; after libbpf's patching it sees an ordinary verified
   program, which is why CO-RE didn't need a kernel rev to deploy.

```
+-----------------------------------+
| Kernel BTF (live layout):         |
| /sys/kernel/btf/vmlinux           |
+-----------------------------------+
                  |  built independently, see note
                  v
+-----------------------------------+
| Compiled .o: bytecode +           |
| BTF relocation metadata           |
+-----------------------------------+
                  |  load time
                  v
+-----------------------------------+
| libbpf: matches relocations       |
| against live BTF, patches offsets |
+-----------------------------------+
                  |  unmodified by CO-RE
                  v
+-----------------------------------+
| Kernel runs the patched,          |
| now-correct program               |
+-----------------------------------+
```

The first two boxes are built independently and in parallel — one from
`CONFIG_DEBUG_INFO_BTF=y` at kernel build time, the other from `clang -g` at
program build time — and only come together inside libbpf at load time. One
compiled object, patched differently per target kernel: this is the "compile
once" half of CO-RE's name.

**User-facing mechanics:**
- `bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h` —
  generates one header with every kernel type, removing the `kernel-devel`
  dependency entirely.
- `BPF_CORE_READ(task, mm, exe_file, f_inode, i_ino)` — chains a multi-hop
  pointer walk into one relocatable macro.
- `bpf_core_field_exists()` / `bpf_core_field_size()` — branch on whether a
  field is present/sized as expected on this kernel.
- **Struct flavors** — define multiple incompatible struct variants (a `___tag`
  suffix marks the flavor, stripped before matching), select and cast at runtime
  based on `LINUX_KERNEL_VERSION` or a Kconfig `__kconfig` extern.
- **Read-only global config**, set by the loader before load — the verifier
  treats these as known constants and can dead-code-eliminate branches using
  helpers unavailable on an older kernel, something a config BPF map can't get
  since the verifier must treat map contents as unknown.

**Relevance to this board:** custom `.config`, no `kernel-devel` package —
exactly the case CO-RE exists to solve, and exactly the case that breaks BCC's
approach. Whether CO-RE is usable at all here comes down to one check:
`CONFIG_DEBUG_INFO_BTF=y` and `/sys/kernel/btf/vmlinux` present.

## 6. Embedded Relevance

- **Field diagnostics without reflash** (primary win) — a kprobe/tracepoint
  program attached live via `bpftrace`/libbpf diagnoses an already-deployed unit
  without touching the boot image — no secure-boot re-signing, no OTA risk.
  Compares favorably to a custom out-of-tree debug kernel module, which needs
  rebuilding per kernel ABI and carries its own licensing/maintenance overhead.
- **In-kernel aggregation for constrained observability** — aggregating into a
  kernel-side map (histogram/counter) and exporting only the reduced result
  suits boards too small to stream raw events to userspace continuously.
- **Syscall/LSM sandboxing** (context) — `BPF_PROG_TYPE_LSM` (5.7+) gives
  finer-grained per-process policy than classic `seccomp` (which uses the older,
  much more limited cBPF — not to be conflated with eBPF), without a full
  SELinux/AppArmor policy engine.
- **Edge networking via XDP** (context) — relevant only if the device routes/
  filters traffic (a gateway role), not applicable to this board's UI/application
  role.

**Caveats:**
- `CONFIG_BPF_SYSCALL` + BTF add real image size — worth checking the actual
  delta against a flash-constrained board's budget rather than assuming it's
  negligible because BTF is deduplicated.
- JIT and even `BPF_SYSCALL` availability isn't guaranteed on every embedded
  arch/config the way it is on arm64/Cortex-A76 — confirm per target.
- `bpftrace` (full LLVM backend) is dev/debug-time tooling only — a production
  image ships a small libbpf-linked agent with a pre-compiled, CO-RE-relocated
  program, never the interactive tool itself.

## 7. References

- ebpf.io — ecosystem/use-case landscape; marketing-level, not a primary
  technical source, but the Applications/Infrastructure project-landscape pages
  are a maintained directory worth bookmarking.
- Nakryiko, "BPF CO-RE (Compile Once – Run Everywhere)" (nakryiko.com) — primary
  technical source for §5.
- elixir.bootlin.com/linux/v6.12.34 — verifier, JIT, and bpf_trace source
  references throughout §3.