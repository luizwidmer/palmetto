# Boot frontier

**Status: measured** on the retail 1.0.2.1 package, Windows 11, RTX 4090, `PALMETTO_D3D12=vulkan`, with the shader corpus
in `PALMETTO_SHADERS` (26,555 game and XboxBvhBuilder modules plus the FSPX set that translates) and
`PALMETTO_SKIP_INT3=1` while the allocator assertion below is unmodelled.

This note records how far the boot currently gets and what stops it, so the next attempt starts from a measurement
rather than a guess. Counts, arities and error classes only; no package bytes or addresses.

## Where the run stops

| # | Stop | Class | Measured |
|---:|---|---|---|
| 1 | device slot 61 forwarded to PC `CreateMetaCommand` | wrong arity | fixed: refuses with E_NOTIMPL, logs both arguments |
| 2 | device slot 67 `DeserializeComputePipelineStateX`, 42 calls | missing translation | **solved**: they are the title's own precompiled compute programs from the pipeline library inside `shaders.arc`, not FidelityFX programs; 38 of the 42 translate, the corpus carries them, and 0 creations fail |
| 3 | device slot 108 forwarded to the PC table | unknown shape | **current stop**: called with two arguments (an output pointer and a structure pointer) from 29 sites |

The run reaches **2,238 D3D12 calls** and stops at the third row.

Two facts about the first two rows matter for planning:

* The title does **not** survive a failed compute pipeline. Full slot tracing shows the run reaching device slot 62 once
  more after the 42 refusals and then calling through a null pointer (`access execute` at 0x0), so the FSPX gap stops
  the boot and is not merely a rendering problem. An earlier reading of the same run - that the title continued past
  the failures - came from the slot log without full tracing and was wrong.
* The lookup key is the SHA-256 of the kind-6 program bytes the title passes in the pipeline description, so a pipeline
  can only be created when that exact program has a translated module.
* The shader library loads `<dir>/index.json` and then the `fspx/` and `bvh/` indexes beneath it, so the builder's
  subdirectories are found as written. An earlier note here claimed the opposite; it was wrong.

## Outstanding assertion

With `PALMETTO_SKIP_INT3` unset the run stops earlier, on the title's own check that a computed size is at most
2 MiB - 1 (`cmp rax, 0x1fffff; jbe ...; int3`), in the same allocator region as the earlier memory stops. It is a soft
assertion: skipping it lets the boot run on into pipeline creation, so it is hiding progress rather than blocking it.

## Method notes

* A console slot's arity is measured from the title's own call site: scan `.text` for `ff 90 <slot*8 as disp32>` and
  disassemble the window before the call. Register probing alone misleads, because arguments the console does not pass
  are whatever the registers happened to hold.
* The console device vtable is longer than any PC one (the title calls slot 127, a one-argument method, from 45 sites).
  Slots past the PC table are console extensions: they are stubbed with S_OK / 0 and named in the log.
* A console heap address is a 64 KiB-aligned title VA; any other value where an extended description would carry one
  is a field that happens to sit there, and treating it as an address takes the heap out of the arena.

## The FSPX translation gap, measured

The 29 FidelityFX programs unpack and disassemble, and 18 translate. The 11 that do not are refused by the translator
with `fall-through into undecodable words`: a reachable instruction runs into words the disassembler skipped, so the
path's later stores and exports would be dropped without a word.

| program | bytes | undecodable words |
|---|---:|---:|
| 03 | 10,156 | 9 |
| 04 | 16,696 | 9 |
| 05 | 17,040 | 27 |
| 06 | 41,300 | 155 |
| 07 | 21,620 | **1** |
| 08 | 25,508 | 10 |
| 09 | 25,972 | 32 |
| 12 | 8,656 | 8 |
| 15 | 22,272 | 8 |
| 18 | 8,600 | 8 |
| 28 | 13,724 | 55 |

The skipped words sit in the listing as gaps: program 07 has one, at +0x4EE4 (`08 19 2c f4`); programs 12 and 18 have
eight each, all of the form `xx 50 16 cc` (`19 50 16 cc`, `1a 50 16 cc`, `21 50 16 cc`, ...). LLVM's AMDGPU disassembler
refuses every one of them as `invalid instruction encoding` for `gfx1030`, `gfx1013`, `gfx1010`, `gfx1011` and `gfx1012`,
which points at an alignment problem in the listing rather than a missing opcode family: one mis-sized instruction
earlier in the stream would leave exactly this kind of short, repeated remnant.

### What the skipped words actually are

Program 07's single gap is the clearest case, and it settles the shape of the problem. The four bytes at the gap are the
**first half of an eight-byte instruction**; LLVM rejects them on their own and then re-anchors four bytes later, so the
second half is decoded as a different, unrelated instruction (`v_mul_hi_u32_u24_e32`). The translator then finds a
reachable instruction that runs into the skipped first half and refuses the whole shader.

The eight bytes together are still rejected, and the reason is a register-alignment rule rather than a missing opcode:

```
s_buffer_load_dwordx8 s[25:32], s[16:19], s92 offset:0x7800   -> error: invalid register alignment
s_buffer_load_dwordx8 s[92:99], s[16:19], s92 offset:0x7800   -> assembles, and matches an earlier instruction in the same program
```

Sweeping that field settles which reading is right. The byte selects the destination, and every value the disassembler
accepts lands inside the register file while the rejected ones do not:

| value | destination | result |
|---|---|---|
| 0x17 | `s[92:99]` | decodes |
| 0x18 | `s[96:103]` | decodes |
| **0x19** | `s[100:107]` | **invalid** |
| **0x1A** | `s[104:111]` | **invalid** |
| 0x1B..0x1D | `ttmp[0:15]` | decodes |

The rejected values name registers past the scalar-register file on every RDNA2 target LLVM models (gfx1010, gfx1011,
gfx1012, gfx1013, gfx1030 all refuse them), so these words are **not an instruction on any of those targets**. The
disassembler is right to reject them; the control-flow walk is wrong to reach them.

That points at the walk rather than at the decoder, and specifically at where the stream first drifts: a word the title
executes cannot sit there as data, so an earlier instruction must have been decoded at the wrong length or as the wrong
operation, leaving the walk falling through into what is actually a literal or padding word. Program 07 has exactly one
such word, which makes it the cheapest place to find that first divergence.

Either way the work is in the translator and its disassembly, not in the D3D12 layer: the runtime cannot create these
compute pipelines until the programs translate, and the title stops on a null pipeline when they do not. That is the only
thing between the runtime and a first frame, and it is a translator project rather than a wiring change.

### The fall-through site in program 07

The translator re-anchors every instruction to its true offset in the program blob when a listing contains undecodable
words, then translates only what is reachable from the entry. Doing that for program 07 puts the skipped word in a
precise place:

```
re-anchoring skipped 1 range: 0x4F08..0x4F0C (4 bytes)

0x4EF4  s_clause
0x4EF8  s_buffer_load_dwordx8
0x4F00  s_buffer_load_dwordx8      <- ends at 0x4F08
0x4F08  (skipped word)             <- the path falls in here
0x4F0C  28 2c 14 1a                <- code resumes here
```

Nothing branches to 0x4F08, so the path arrives by fall-through from the scalar load at 0x4F00, and the two loads before
it carry `s_clause`, whose whole purpose is to bundle consecutive scalar loads: the stream really does look like a run of
loads, with one member of the run undecodable.

An earlier version of this note claimed the listing and the blob disagreed about that neighbourhood, because a load
encoding seen nearby was not at that offset in the blob. That was wrong: the encoding occurs exactly once in the blob, at
0x509C, as an ordinary instruction elsewhere in the same program. The listing and the blob agree; only the one word at
0x4F08 is undecodable.

The program's own metadata says it is a wave32 compute program with 64 VGPRs, 14 user SGPRs and 4 KiB of LDS. The
destination the undecodable word names would be `s[100:107]`, past the scalar-register file that every RDNA2 target LLVM
models allows, which is why it decodes as invalid while its neighbours decode.

### The answer: the console's register file is larger than LLVM's model

The programs settle it themselves. Their compute metadata declares the register counts, and decoding the granularity
field of `rsrc1` (`0x402c0347`) gives **112 SGPRs** for a wave32 program; the same value appears in programs 07, 12 and
18. LLVM's RDNA2 model allows **106** scalar registers at most, and it says so plainly when asked to assemble the
instruction the word encodes:

```
s_buffer_load_dwordx8 s[100:107], s[16:19], s12 offset:0x7800   -> error: register index is out of range
s_buffer_load_dwordx8 s[96:103],  s[16:19], s12 offset:0x7800   -> assembles, encoding 08 18 2c f4 00 78 00 18
```

So the skipped word is a valid instruction on this console whose destination register LLVM's register file does not
contain, and the same will be true of every other reference to a register above 103 in these programs. The disassembler
is not wrong about the encoding; it is modelling a smaller machine.

**What that means for the fix.** No runtime change helps and no LLVM flag helps: the disassembler has to be told the
console's register file is bigger, or the pipeline has to decode these words itself. The pipeline route is available
without rebuilding LLVM: assemble the same instruction forms with destinations inside LLVM's range under each candidate
`mcpu`, derive the bit fields, and emit the listing line for the out-of-range cases from the bytes. The ground truth for
that derivation comes from `llvm-mc --show-encoding`, so nothing is guessed.

Program 07 is the trial case: one word, one instruction form, and a shader that already translates in every other
respect.

## Two classes among the eleven, and what each needs

Repairing the listings with `tooling/widen_smem_decode.py` split the eleven refusals into two classes, which need different
work:

| class | what it is | state |
|---|---|---|
| register file | a buffer load whose destination is above LLVM's 106-register model, skipped and re-anchored | **fixed for the words it covers**: the tool reconstructs them from the program bytes, and program 07 now translates and passes spirv-val |
| inline literals | an instruction LLVM decodes correctly but prints with a canonical encoding, because it drops an inline-literal or op-sel field | **open**: with the strict printed-encoding check relaxed as a measurement, the repaired set still translates 18 of 29 and the remaining failures are `branch target outside code`, so the driver is byte drift after the words the tool cannot yet rebuild |

Adding program 07's module to the corpus does **not** create any pipeline: the run still reports the same 42 failing
`DeserializeComputePipelineStateX` calls and stops in the same place. So that program is not one of the ones the title
actually asks for, and a translation that passes `spirv-val` is not by itself progress on the boot.

### What that implies for the remaining work

The 42 pipelines need specific programs among the untranslated ten. Getting there means finishing class 2 - making the
repaired listing's instruction offsets agree with the blob for the inline-literal forms - and only then re-measuring.
A partial set is measurably useless: it changes neither the failure count nor the stop.

### The inline-literal class, measured

After the repair tool was made non-invasive, the FidelityFX set translates 19 of 29 (program 07 is the new module) and
program 03 has 15 of its words reconstructed instead of one. What remains is one class, and the translator names it with a
single-bit difference:

```
v_dot4_i32_i8 at 0x13DC
  the blob holds  34 40 16 cc 27 8d d1 0c
  LLVM printed    34 40 16 cc 27 8d d1 1c
```

The last byte differs in one bit. That is a field the disassembler drops when it prints an encoding, not a misdecode:
the instruction is right and the text lost a field. The translator refuses it rather than translating the field as zero,
and rewriting the printed bytes to match the blob would defeat that refusal without making the translation correct, so
the repair tool leaves these lines alone.

Finishing this class means teaching the translator to read the dropped field from the blob for the VOP forms involved -
the same shape as `mix_literal`, which already exists for other families - and then re-measuring. Until that is done the
FidelityFX pipelines cannot be created, and the run stops where it has stopped since the pipelines were first reached.

## Correction: the refused pipelines are the title's own, not FidelityFX

An earlier section here said the 42 refused pipelines needed FidelityFX programs among the eleven the translator cannot
translate. That was wrong, and measuring it properly corrected the picture:

* the 42 refusals carry 39 distinct program keys, and none matches any of the 29 FidelityFX programs - not by key, and
  not by size class either;
* they are the title's own **precompiled compute pipelines**, deserialized from the pipeline library inside
  `shaders.arc`, whose format is in [shader_library.md](shader_library.md). 31 of the 39 keys are byte-identical to
  library compute programs 0..30.

Extracting that library, writing the missing `<name>.state` sidecars - the runtime filters a program's candidates by the
SHA-256 of its kind-0 state, so without them no library program can resolve - and translating its compute programs gives
**38 of 42 translated and `spirv-val` clean**. Four are refused with `CS thread-group-size SGPR`. With those in the
corpus and the stand-in pipeline switched off, **0 of 42 pipeline creations fail**, and the run reaches device slot 108
and stops inside the title itself.

Three consequences for anyone continuing:

* `tooling/shader_library.py` now writes `<name>.state`; a pipeline cannot resolve without it;
* nothing in `build_port.py` runs the pipeline-library step yet, so a fresh build still reproduces the old failure;
* `tooling/rdna2spirv.py`'s stage fallback reads `lib_cs_*` as stage `lib`, so a library listing needs an `index.json`
  beside it (or the fallback fixed) or it is refused as an unknown stage.

## The stop is independent of the compute pipelines

With all 42 compute pipelines created - 31 from translated library programs and the other eleven standing in - the run
still stops at the same place: `game+0xCC2861`, a read of `0x10` because the function is entered with a null object.

The caller loads that object from a process global and passes it straight in:

```
5BFEBD5: mov rcx, qword ptr [rip + ...]   ; the object, null here
5BFEC10: call 0x140CC2840                 ; test byte [rdi+0x10], 4  -> fault
```

So the missing pipelines were not the cause of this stop. The global is filled by something earlier in the boot that the
runtime either refused or answered without an object. Candidates measured in the same run, in order:

* device slots 86 and 108 are console extensions beyond the PC device table; they are stubbed with S_OK and, for 108, a
  zero dword, which publishes no object at all;
* `ShaderCacheControl` fails, `CreateLifetimeTracker` returns E_NOTIMPL, the texture computer path returns E_NOTIMPL
  (it does hand back a stub object), and DirectStorage reports zlib/BCPack/swizzle requests as not implemented.

The next step is to find which of those results the title stores in that global - by logging the arguments of the
beyond-table stubs and the value each caller keeps - rather than guessing at the crash site itself.

## The streaming gap was hiding most of the boot

The DirectStorage shim refused a request's zlib stage with `E_NOTIMPL`. Once it inflates it - the runtime already had
`zlib_inflate` for the asset tools' format, so nothing new was vendored - the boot goes much further before the same
stop: **2,238 -> 11,894 D3D12 calls**, the `not implemented` line disappears, and no zlib request fails. The wall-clock
time grows from about 14 s to 17-19 s for the same reason.

Two things this settles:

* a refused streaming stage does not stop the title, it *starves* it: the run continued, but with a fraction of the work
  the title wanted to do, so every measurement taken before this change described a much shallower boot than it appeared
  to;
* the remaining stop is unaffected, which makes it the more clearly isolated: after five times as many device calls the
  title still arrives at the same null object (`game+0xCC2861`, reading `0x10`) instead of anything downstream.

BCPack and swizzle requests are still refused explicitly; the log names the modes and size bands when they are asked for.

## The eleven "missing" programs were a decompression failure

For several rounds the boot was described as needing translations for eleven pipeline programs that matched no extracted
library record. That was an artifact. The DirectStorage shim refused a request's zlib stage, and the library bytes the
title handed to the device for those records were therefore zero pages. Dumping them captured the zeros, which is why
they matched nothing, why same-size library records differed in hundreds of bytes, and why a raw search of the package
found nothing.

With the zlib stage implemented, the same boot reports:

```
d3d12 calls: 11,894
pipeline failures: 0 of 42
programs dumped as untranslated: 0
```

and that is **without the stand-in pipeline path at all**. Every compute pipeline the title asks for resolves to a
translated library program. The stand-in remains available as a diagnostic but is no longer needed to get this far.

The lesson worth keeping: a refused decompression did not fail loudly at the point of use. It produced valid-looking
zeros downstream, and three separate investigations spent rounds on "missing programs" that did not exist. When a shim
refuses a *data* stage, the bytes it feeds every later consumer are wrong, and nothing downstream can tell.

## The null object was console slot 108

The stop that survived the pipeline work was not a pipeline problem at all. Tracing the factory that stores the null
object showed it creates the engine's default white render-target: the validator accepts the description, then finalize
asks the device for the resource's allocation info and the allocation fails, so the factory returns null and the renderer
asserts on it.

The allocation info comes from console device extension slot 108, called in the console's struct-return order - hidden
16-byte result in RCX, device proxy in RDX, counts in R8/R9, description array on the stack, per-description record array
as the sixth argument. The shim answered success and wrote at most one zero dword to what it took for an output pointer,
which for this call is the device proxy, so the return buffer stayed garbage. The title computed a bogus size, asserted
in its allocator, and the factory returned null.

Answering the slot with a real query - the device's allocation info for the passed description, with a 64 KiB fallback
when the host cannot size it - removes the stop entirely. Measured: the boot continues past `game+0xCC2861` to
`game+0x24844A9`, 11,902 D3D12 calls, 0 pipeline failures.

The new stop is the same class one layer on: a global 72-byte-record array (`image+0x75DA668`) is null when an entry is
written to it. A watcher over that global sees no transition at all during the boot, so whatever populates it never runs;
no direct store to it exists in the image, so it is filled through a computed pointer, and the next step is to find which
subsystem's failure skips that initialisation rather than the writer itself.


## Frontier 2026-11-24: the title reads our fabricated GPU address as CPU memory

The stop after the TLS rewrite landed at `game+0x2484133`, a read of address `0x100000000`. The value in `rbx` there is
exactly `1 << 32`, and `d3d12_vulkan_device.cpp` hands out console image addresses starting at `std::uint64_t(1) << 32`
(256 bytes apart) with the comment "Nothing dereferences it". **That comment is now falsified by measurement**: the title
dereferences the address as a CPU pointer.

Evidence:

1. A hardware-free probe (poking an int3 over the faulting instruction) logs `rbx = 0x100000000`, `rax = 1` immediately
   before the fault, and the trap fires on every pass - the block is reached by a computed transfer, not the fall-through
   path that a linear disassembly shows, which is why tracing the preceding `lea` never explained the value.
2. A poke at the call's return address never fires, confirming the indirect entry.
3. `PALMETTO_BACK_IMAGES=1` commits 64 MiB of zeroed memory at `0x100000000`. The read then **succeeds** and the fault
   moves to `game+0x2484140`, a write to `0x121A4` - a null pointer plus that offset, because our memory holds zero where
   the console would hold a pointer.

Interpretation: on the console, a resource's GPU virtual address is also a CPU address (unified memory). The title reads
through a T# image address expecting live data. Our Vulkan backend fabricates addresses in that range that are not
mapped, and no data is ever written there.

What this implies for the runtime: resource GPU addresses must become CPU-accessible at the address the title is handed -
either by backing the console address range and keeping it coherent with what shaders write, or by making the fabricated
address resolve to the resource's host memory. The descriptor-heap work already does this for heap memory
(`console_heap_back`, placeholder views); resource memory needs the same treatment.

Cross-reference: the public attempt (a public runtime project) does not need this, because it runs the title on host D3D12 where the
console's GPU virtual addresses are the host's own resource addresses.


## Frontier 2026-11-24 (later): queue slot 18 returned a struct the wrong way round

The stop at `game+0x2484133` (a read of `0x100000000`, a clobbered `rbx`) was localised with the new crash report and
register-log pokes: `rbx` is correct at `game+0x2484129`, and wrong at `game+0x248412E`, so the faulting frame's first
D3D12 call - queue slot 18, `GetDesc` - was the cause. The path reaches it by a `js` at `game+0x24840E3`, which is why
earlier probes on the fall-through instructions never fired.

Slot 18 was the audit's unmeasured convention: the title calls it as `(queue, buffer)`, and our queue is a C++ object
whose `GetDesc` returns `D3D12_COMMAND_QUEUE_DESC` by value, so MSVC wants the hidden return pointer in RCX and the object
in RDX. The generic forwarder passed them the other way round, so the descriptor was written into the front of the real
queue object and the title's buffer kept its previous contents; the title read `1 << 32` out of it and dereferenced that.

Measured after the fix: the stop moves to `game+0xcc2861` (a read of `0x10`) and the D3D12 call count rises from 11,906
to 11,986. Slot 18 now has its own conversion beside the device proxy's struct-return cases.

The next stop is a null-ish read in frame `image+0xcc2861`, whose caller is `image+0x186324`.


## Frontier 2026-11-24 (new stop): a null argument that the title traps on

After the slot 18 fix the stop is `game+0xcc2861`, `test byte ptr [rdi+0x10], 4` reading `0x10`. Instrumented pokes show
the routine is entered with a valid pointer on every early pass (`rdi` = a heap pointer), so this is not a state problem
inside the routine: the faulting pass is the one that enters with a null argument.

The routine begins by testing its first argument and branching to `game+0xcc2936` when it is null. That address is an
`int3`, and `game+0xcc2937` is a `jmp` back to the loop body that skips the argument load. `PALMETTO_SKIP_INT3` steps over
that `int3`, so the null argument then reaches `[rdi+0x10]` and faults at `0x10`. The trace records exactly this:
`int3_skipped at game+0xcc2936` is the last skip before the fault.

So the title's own null check fired, and skipping the trap converted a deliberate stop into a dereference. The question to
answer next is who passed the null: the caller frame is `game+0x186324`, whose `mov rcx, qword ptr [rax]` loads the
argument from `[rsi+0x60]`, and the first qword there is zero.

This is also a policy question for `PALMETTO_SKIP_INT3`: for a trap that follows a null check, stopping at the trap is more
informative than stepping over it, because the trap site is the title's own statement of what went wrong.


## Frontier 2026-11-24 (refusals at the null stop)

Counted in one boot at the `game+0xcc2861` stop. The dominant gap is not a slot arity question but an interface the title
creates constantly:

| refusal | calls |
|---|---|
| `ID3D12Device5::CreateLifetimeTracker` returns `E_NOTIMPL` | 10,242 |
| allocator slot 9 past the PC allocator table | 2 |
| queue slot 32 past the PC queue table | 2 |
| `ID3D12GraphicsCommandList::Close` failed | 1 |

Every `CreateLifetimeTracker` call leaves the title with a null tracker, and the current stop is the title's own null
pointer, so a tracker object is the first thing to rule in or out: an earlier prototype regressed the boot (D3D12 calls
11,894 -> 1,653) through a wrong vtable slot, which means the interface order must be exact, and the run has to be
compared against 11,986 calls and the `game+0xcc2861` stop before the change is kept.

## Frontier 2026-11-24 (slot 57 is a deserialized graphics pipeline record)

Console device slot 57 forwards by index to PC `ID3D12Device5::CreateLifetimeTracker`, which refused 10,242 calls in one
boot. Probed instead of assumed, it is a **deserialized graphics pipeline record**, the graphics analogue of slot 67's
compute record - not `CreateGraphicsPipelineState` and not six stage bytecodes:

| offset | size | content | matched against |
|---|---|---|---|
| +0x00 | - | root signature | - |
| +0x08 | 388 B | fixed-function **state blob** | library blob 11315 |
| +0x18 | 188 B | **input-layout** blob | library blob 11316 |
| +0x28 | 448 B | native **PS** program | program 842 = `lib_ps_00842` |
| +0x38 | 208 B | PS **kind-2 stage blob** | library blob 11317 |
| +0x48 | 384 B | native **VS** program | program 843 = `lib_vs_00843` |
| +0x58 | 752 B | VS **kind-2 stage blob** | library blob 11318 |

Exactly one library graphics record holds state 11315 + layout 11316 (record hash `2f04ef59bc062cf8`, slots 0 -> (842,
11317) and 1 -> (843, 11318), slots 2..6 unused), so the descriptor is that record expanded: root signature, state blob,
input-layout blob, then (program, stage blob) pairs **in record-slot order - PS first, VS second**. The first call is not
always the same record; every sample has the six-pair shape.

Programs are keyed by **SHA-256 of the native program bytes as passed** (`fspx_code_sha256`, the key `create_compute`
already resolves with). Stage blobs are kind 2 (compute layout blobs are kind 3): `+0x00` kind, `+0x04` size-12, `+0x08`
and `+0x0C` undecoded words, 32-byte tagged records from `+0x10` (`0x41`/`0x42` like the compute blob), then a `0x43`
word, the 16-byte source-container digest and exactly 12 zero bytes. **Stage identity is not in the blob** - it is the
record slot (0 PS, 1 VS, 2 MS).

Two gaps stand between this and a converter:

1. `PipelineState::create_graphics` resolves stages only through `palmetto::fssm_program`, so a native program is refused
   with "shader bytecode holds no FSSM program"; the FSPX resolver is compute-only (`palmetto::compute_program`). A native
   branch that hashes the program and asks `library->fspx_candidates(stage, hash)` - the same key compute uses - is the
   minimum runtime change, together with decoding the state blob and input-layout blob into the PC description (the
   console record has no `D3D12_INPUT_ELEMENT_DESC` array and no fixed-function fields).
2. The shader directory the boot loads carries only **61** `fspx_code_sha256` entries and **all are compute**; the
   library's vs/ps/ms programs are not published there, so a graphics resolver has nothing to find yet. The
   pipeline-library publication step must cover the graphics programs the title's records name, before the runtime change
   can be measured.

Measured sizes of the job, from the same boot: the title asks for **9,943 distinct records** but only **1,264 distinct
graphics programs** (945 PS, 319 VS). Records differ in their state and input-layout blobs, not in their code, so
publishing the programs is a bounded task - roughly the size of the compute set that was translated before.

The state blob is undecoded and is the main unknown; the input-layout blob is partially decoded (element count at +0x4C,
NUL-terminated semantics inline, 28 attribute bytes per element beginning with a format-sized code at +0x10). The
measurements come from a reassembled library held outside the repository; nothing here quotes game data.


## Frontier 2026-11-24 (the corpus was rebuilt; the missing pipelines are not the blocker)

Publishing the graphics programs triggered the shader stage's own fingerprint check: a commit of ours had changed a
translator source, so `build_port --stages shaders` deleted the root index and started a full retranslation. That run was
interrupted at 93% (an agent turn ended, and its background job with it), which left the work folder with no
`shaders/spirv/index.json` and every boot failing early with a host C++ exception. Recovery, in order: rerun the one
shard that crashed (shard 6, 0xC0000005; 1,657/1,657 translated, all `spirv-val` clean), merge the 16 parts (26,501
entries), then run the post-translation stages (fspx 29/29, bvh 52/52, graphics 1,266/1,266) and republish the 42
pipeline-library compute programs. Final loaded set: 26,501 + 1,337 + 52 = 27,890 entries, and the boot is back to the
baseline stop with 0 pipeline failures.

Two findings from it worth keeping:

* The current translator refuses two duplicate-name variants that the old corpus contained, so the completeness gate at
  the end of the shader stage blocks the designed `--stages shaders` path. The stages had to be run individually.
* The pipeline-library sub-step is pathological on this machine: `shader_library.py --stages cs --disasm` wrote 9.4 GB of
  listings in 17 minutes and reached 841 of 2,684 programs. The library's compute programs contain many invalid vcc-tuple
  words, and `llvm-mc` prints the whole input hex line in a warning comment for each one, so a 120 KB program becomes a
  100-134 MB listing (one file reached 137 MB). The main corpus (26,503 programs) disassembles in about 15 minutes, so the
  library class is abnormal; the listing pass and the translation pass that reads those listings both pay for it.

### The stand-in measurement: missing pipeline states are not the current blocker

With `PALMETTO_STANDIN_PIPELINES=1`, console device slot 57 hands the title a real `ID3D12PipelineState` built from a
project-owned trivial VS+PS pair, once per refusable record, logged every time. Measured:

| | stand-in off | stand-in on |
|---|---|---|
| stand-ins handed out | 0 | **10,241** |
| highest traced call | 11,986 | 11,986 |
| stop | `game+0xcc2861` (read at 0x10) | **`game+0xcc2861` (read at 0x10)** |

So the title's null container is not waiting for the graphics pipelines it asked for: it gets them and still stops at the
same instruction with the same null. The conversion is still needed for rendering (and for any draw that binds those
records), but it is not the next boot blocker. The next question is what should have populated the container element that
the faulting code reads - the object built by the class whose vtable is `image+0x6597C60` and whose constructor is
`game+0x1B37440`.


## Frontier 2026-11-24 (A8 render targets: the stop moved, and a garbage vtable is next)

The null container element was traced to a texture the title's own factory creates: a 3840x2160, 1-sample, A8_UNORM
**render target** (measured from the descriptor handed to the factory and the flag word that sets ALLOW_RENDER_TARGET).
The PC format gate refused A8 with the render-target flag, so console slot 108's allocation query returned the D3D12
invalid answer (SizeInBytes = UINT64_MAX), which the slot-108 bridge counted as a real answer instead of using its 64 KiB
fallback; the factory's finalize then returned null, and the title asserted its way through its own int3 traps
(game+0xABFE1B, then game+0xCC2936) into the crash at game+0xcc2861.

Fix, in two parts:

* A8_UNORM is accepted for a 2D, single-sample colour image and, with ALLOW_RENDER_TARGET, exposed as R8_UNORM with the
  colour-attachment usage (the A8 view path already moves the bits into alpha). CheckFeatureSupport reports
  RENDER_TARGET for A8 when the host can attach R8_UNORM.
* The slot-108 bridge treats SizeInBytes == UINT64_MAX as "cannot size" and uses its fallback, so an unsupported
  description can never publish an invalid allocation again.

Measured after the fix: the "format not supported yet: 65" line is gone, the stop moves from game+0xcc2861 to
**game+0x4fb0aec**, and the highest traced D3D12 call rises from 11,986 to **12,085**.

### The new stop

`game+0x4fb0aec` is `call qword ptr [rax + 0x60]`: rax comes from `mov rax, qword ptr [rcx]`, and the read that faults is
at 0x3b9aca60, so the object in rcx has **0x3b9aca00 (10^9) as its first qword** where a vtable pointer belongs. rcx
itself is a plausible heap address (registers captured with PALMETTO_DUMP_REGS). The call is reached from the small glue
wrapper game+0x4fb0a90 (which passes a two-word callback struct holding the pointer to game+0x10a330) and its caller is
game+0x23cf363.

10^9 in a pointer slot is not a wild read: it is a value that was written there, most plausibly a size (one gibibyte) or
a sentinel that a structure layout put at offset 0. The next step is to find the writer: either our shim wrote a 64-bit
size where the title's object expects a vtable pointer (a layout mismatch in an answered call), or the title stored a
sentinel that a class slot is expected to test before calling.


## Frontier 2026-11-24 (the queue's console slot shift, and the fence slot it exposed)

The garbage vtable at the previous stop was ours: the console queue inserts **two methods before Signal**
(docs/d3d12_xs_surface.md already recorded the public PresentX slot-30/32 split that way), so console slots 16..20 name
PC slots 14..18. The proxy forwarded console slot numbers unchanged, so console **Signal (16)** landed in PC
**GetTimestampFrequency (16)**, whose out pointer was the fence the title passed - and our `1e9 / timestampPeriod` answer
(the exact 0x3b9aca00 seen in the object) overwrote that fence proxy's first qword. The later `call [vptr+0x60]` then
jumped through 0x3b9aca60.

Fix: the queue proxy maps console 16->PC Signal (fence unwrapped), 17->Wait, 18->GetTimestampFrequency,
19->GetClockCalibration, 20->GetDesc (the struct-return conversion now lives there), and the two unidentified inserted
methods (console 14, 15) refuse with their arguments logged instead of landing on PC Signal/Wait. The proxy mock was
updated to the same mapping and passes.

Measured after the fix: queue slot 16 logs as Signal, queue slot 18 as GetTimestampFrequency, the highest traced call
rises from 12,085 to **12,087**, and the stop moves from game+0x4fb0aec to **game+0x4fb0b03**.

### The new stop: console fence slot 12

At game+0x4fb0b03 (`mov rdx, qword ptr [rdi]`) the fault address is **0x80004001 = E_NOTIMPL**, i.e. the call before it
returned E_NOTIMPL and the caller dereferenced the result. That call is `call [rax+0x60]` on the fence proxy - console
**fence slot 12**, which the fence proxy refuses as beyond the 12-slot PC chain. It is called as
`(fence, 2, &callback_struct, masked)` and the caller then reads `[result]`, comparing it with -1: the shape of "give me
the address of the value being waited on, -1 means not ready". The next step is to identify that console fence extension
(its arity and what the pointer must point at) and implement it - the same measurement-over-guess pattern as the queue
shift.


## Frontier 2026-11-24 (the boot runs: no exception for 300 s, and the frontier is presentation)

Console fence slot 12 was the last thing standing between the title and its own frame loop. Identified from the caller and
the channel table (docs/fence_slot12.md), it is the console extension that returns **the address of the fence's completed
value**: the title polls that 64-bit word in a `monitorx`/`mwaitx` loop and compares it with the value it signalled,
treating `UINT64_MAX` as the removed-device word. The fence proxy now keeps a 64-bit cell that is refreshed from the real
fence whenever a proxy call moves the value, published when the console queue's `Signal` or the fence's own calls move it,
and armed through the Vulkan layer's fence dispatcher for values reached later (held or GPU signals). Monotonic, honest
logs, and the mock test covers it.

Measured, twice (the owner's 400 s run and an independent 300 s run with the same environment):

| | before slot 12 | after |
|---|---|---|
| result | crash at game+0x4fb0b03 (read of 0x80004001) | **no exception at all** |
| verdict | STOP | **ALIVE 300 s / ALIVE 400 s, killed by the timeout** |
| highest traced call | 12,087 | **5,195,024 in 300 s** (6,887,539 in 400 s) |
| fence slot 12 answers | refused | 245,524+ |

So the title now boots into its own loop and keeps running. What it does there is the next problem, and the trace names it:

* **`PresentX` (console queue slot 32) is still refused** with E_NOTIMPL, once per frame iteration.
* The frame's two command lists come back **already closed**: about 625 `Close` calls succeed during startup, then every
  frame iteration's two `Close` calls fail (491,037 of them in one run), and the boot contains **no command-list `Reset`
  and no `ExecuteCommandLists` at all**. The lists are therefore never re-recorded and nothing is executed or presented.
* The rest of the frame iteration is two `Signal`s, a `GetTimestampFrequency`, an allocator `Reset` - a submit shape whose
  lists are missing.

The frontier is consequently presentation and command-list lifecycle, not the boot chain: why the title's lists are still
closed at the top of a frame (its `Reset`/`ExecuteCommandLists` calls either never happen or are not the console methods we
map), and what the console's `PresentX` needs.


## Frontier 2026-11-24 (frames are submitted now - PresentX is the last refusal)

Two fixes turned the spinning loop into a submitting renderer:

* **Console queue slot 35 is the submit**, not a clock calibration (docs/render_loop.md measured the call as
  `(count, list array, frame token, reserved)` from the frame function). The proxy was answering it with a calibration
  pair written through the caller's pointer - i.e. two QPC values over the title's own command-list array - and the whole
  boot had **zero** `ExecuteCommandLists` calls. It now unwraps the list array and forwards `(count, lists)` to the PC
  queue's `ExecuteCommandLists`.
* **MONITORX/MWAITX are AMD-only** (`0F 01 FA` / `0F 01 FB`) and the title's fence wait loop uses them; an Intel host
  raises STATUS_ILLEGAL_INSTRUCTION. They are now emulated like `clzero`: the monitor is a no-op, the wait is a pause that
  yields, and the loop's own memory comparison still decides readiness.

Measured after both: the boot stays ALIVE, and per 140 s run it now makes **78,745 successful `ExecuteCommandLists`
calls** (78,745 frame submits) where before there were none, 2,292,332 traced D3D12 calls in total.

What the frame still is not: a frame. Each iteration records barriers and clears into **command lists that are still
closed** (157,486 `Close` calls fail), signals twice, submits, and calls **PresentX (console queue slot 32) 78,745 times,
which the proxy still refuses with E_NOTIMPL**; no `SetPipelineState` and no draw is ever recorded, so the submission is a
clear-and-present at best. docs/render_loop.md has the lifecycle evidence: the console recording memory belongs to the
allocator, the title never re-opens a list, so the layer has to reset the real list against its creating allocator before
the first recording call that reaches it closed (measured: with the console barrier muted this yields 4,000/4,000
successful closes), and console list slot 146 needs a barrier conversion the PC runtime rejects today.

So the frontier is presentation, in this order: **PresentX (queue slot 32)** - the one call the frame makes that we refuse,
with the prepared path in port/shims/xbox_present.* - then the command-list lifecycle so the frame's recording is not
discarded, then the console barrier conversion.


## Frontier 2026-11-24 (what the present actually carries, and what it does not unlock)

Queue slot 32 is confirmed as console PresentX: called as `(1, planes, params)`, and the first call's plane measures as
**token 0x1, ResourceCount 1**, colour space 0 (SDR), scale filter 0, flags 0, with `ppResources` pointing into the
image's data (`image+0x75E0DE0`) - and the first resource there is **NULL**. So the frame's present currently names no
resource at all; that is a separate finding to chase (who is supposed to write that pointer, and why it is still zero),
not something the present wiring can fix.

A second measurement settles the order of work: answering PresentX with S_OK and presenting nothing
(`PALMETTO_PRESENT_STUB=1`, loudly logged, off by default) speeds the loop - 42,515 submits in 80 s against 30,786 when
the present is refused - but the drawing calls stay at **zero** (`SetPipelineState`, `DrawInstanced`,
`DrawIndexedInstanced` all 0 in both runs), and every frame's Close still fails. Present success therefore does not
unlock the renderer: the frame's recording is still being dropped because its command lists are closed.

So the order is: command-list lifecycle first (make the recording survive; the closed list is why no draw is ever
recorded), then the console barrier conversion at list slot 146, then the real present wiring - and the null plane
resource is a fourth thread worth pulling while the lifecycle work happens.


## Frontier 2026-11-24 (the frame's recording works; two console recording calls are invalid)

The list proxy now emulates the console's lifecycle (docs/list_lifecycle.md): it remembers the allocator a list was
created against and whether the real PC list is recording, and when a recording call reaches a closed list it resets the
real list against that allocator first (PC `Reset(allocator, nullptr)`), forwarding only on success. A successful Close
marks it closed, so the next frame's first recording call reopens it. Failures log once with the HRESULT and refuse
rather than forwarding.

Measured, two runs at the same tree: default behaviour still shows 625 successful closes and 104,340 failed ones, because
two console recording calls make the PC runtime mark the list invalid: **list slot 146** (a resource-less 64-byte texture
barrier group the PC runtime rejects with E_INVALIDARG - docs/render_loop.md) and **list slot 52**, which forwards to PC
`ClearRenderTargetView` and returns false because the console CPU handle and the PC target-view capture disagree. With
those two calls muted (`PALMETTO_XS_LIST_MUTE=146,52`, a measurement switch) the lifecycle runs every frame: **57,597 Close
S_OK, 0 failed, 56,915 reopens, 56,983 submits, ALIVE**, and the mock test covers reopen-on-recording, Close, GetType
never reopening, and a failed Reset refusing.

So the recording path is solved in principle; the next two pieces are the real conversions behind those two mutes:
console `ClearRenderTargetView` (slot 52) must map the console CPU descriptor handle onto the PC target view the capture
expects, and the console barrier (slot 146) must translate its 64-byte texture group (which can carry `pResource = 0`)
into the PC barrier the runtime accepts. No draw is recorded yet either way, which is expected: the graphics pipeline
records (device slot 57) are still refused, so the renderer has no pipeline state to draw with.


## Frontier 2026-11-24 (recording converted end to end; the draw path is the frontier)

Both invalid recording calls are now real conversions, verified with **no mutes**: a 60 s boot shows **30,999 Close
S_OK / 0 failed**, 30,376 list reopens and 30,376 submits, ALIVE with no exception (docs/handles_barriers.md). The frame
rate is roughly half the muted control (54,647 submits) because each frame now really carries ~2.5 barriers instead of the
call being dropped - no submission is lost.

* Console `ClearRenderTargetView` (list slot 52): the handle is not a console address needing a mapping; it is a CPU
  descriptor handle this proxy's own `CreateRenderTargetView` created **from a null resource**, so the clear is the no-op
  the console intends. A clear whose capture is a real view forwards unchanged; a null-view clear is answered as a no-op
  and never forwarded; an unreadable or emptied capture is refused with a named log.
* Console barrier (list slot 146): groups are PC-shaped. A resource-less texture entry becomes a `D3D12_GLOBAL_BARRIER`
  with the same sync/access words (layouts have no PC encoding without a resource and are logged once); real-resource
  entries pass through as texture barriers; unmeasured shapes are refused, never forwarded. Each refusal answers 0, so the
  list stays valid and the frame keeps its Close.

The graphics pipeline backend is ready too (docs/graphics_pso.md): `create_graphics` now resolves native (FSPX kind-6)
stages through `rt::graphics_pso::resolve_native_stage` with the same key `create_compute` uses - including the published
raw-key entries - and refuses ambiguity with the SHA-256 named; and `rt::graphics_pso::build` turns a decoded slot-57
record plus explicit caller inputs into a PC `D3D12_GRAPHICS_PIPELINE_STATE_DESC`. `graphics_pso_test` covers 128 checks.

What still stands between this and a drawn frame, in measured order:

1. **The draw path is never reached**: `SetPipelineState` and every draw call remain 0, and the frame clears a **null**
   render-target view the title itself created. Before slot 57 can produce the draw's pipeline, the frame has to have a
   real target to draw into.
2. **The slot-57 wiring** (next): the backend exists but the proxy still refuses the records. The documented wiring is in
   docs/graphics_pso.md section 5; it needs explicit attachment formats and the documented default state, because the
   record carries neither. The builder refuses any input layout with elements (format codes unmapped so far), and the
   library's vertex pipelines carry elements - so expect the first measurement to refuse most records with a named field,
   which is the next thing to decode.
3. **PresentX** (queue slot 32) is still refused; the prepared presenter is in xbox_present.* and the frame currently has
   only a null-target clear to present.


## Frontier 2026-11-24 (slot 57 wired: the first pipelines, and one field refusing 8,926 records)

The slot-57 handler now decodes the graphics record, builds the PC description through `rt::graphics_pso::build` and
forwards it to the PC device's own `CreateGraphicsPipelineState` (docs/graphics_pso.md sections 2-5). One 150 s boot:
ALIVE 150.2 s with no exception, 1,410,454 traced D3D12 calls, 10,241 slot-57 calls, **1,074 records build into real PC
graphics pipelines** and **8,926 refuse at one named field, `record.input_layout`** (ps+vs 8,923, vs 3). The only layout
shape that builds is the 80-byte empty one, and no record reaches the input-layout format code or the state blob.
`SetPipelineState` and the draws are still 0 and `PresentX` is still refused, so the pipelines are bound to no draw. The
answer to the wiring's question: the next decode is the **input-layout blob's element table**, not the fixed-function
state and not the attachment stand-in. The wiring and the full histogram are
[slot57_wiring.md](slot57_wiring.md) sections 1-2.


## Frontier 2026-11-24 (the input-layout table decoded: the frame's draws appear)

The element table is decoded - a 0x50-byte header, the count at `+0x4C`, then a length-prefixed name and a 24-byte
`D3D12_INPUT_ELEMENT_DESC` attribute block per element, whole blob filled exactly; the DXGI format words 2/6/16/28/30
fit the measured offsets and the earlier contiguous-names reading is refuted: [input_layout.md](input_layout.md). One
150 s boot: **10,241 of 10,241 records build with 0 refusals and 0 PC failures**, and the frame records its first draws -
**42,721 `SetPipelineState` and 42,722 `DrawInstanced`** - with `Close` 86,059 S_OK / 0 failed and the run ALIVE 150.2 s.
The draw path is reached but not executed: 42,724 submits succeed and **42,720 fail, one per recorded draw**, all
`ExecuteCommandLists failed: unknown resource; nothing was submitted`. The frontier is therefore the draw's resource
binding, not the layout: [replay_resolution.md](replay_resolution.md).


## Frontier 2026-11-24 (the display path: the title presents a null surface of its own)

Presentation is not waiting on a shim answer. A 120 s write watch on the present plane's resource cell
(`PALMETTO_WATCH_RVA_WRITE=75E0DE0`) records **23,819 writes of `0`, every one from `game+0xCD74DB`**, on the title's own
present-to-display path; the same surface class's `+0x20` resource field is what `CreateRenderTargetView` is called with
(`pResource = [surface+0x20] = 0`), so the null view the frame clears and the null present resource are one field. The
slot-133 path measurably creates and stores real surface resources, so the null population is a different set of
surfaces, not a missing shim answer. The writer chain, the surface dumps and the exact next poke
(`PALMETTO_POKE_LOG=325090` on the surface constructor) are [display_path.md](display_path.md).


## Frontier 2026-11-24 (replay resolution: the executed draw that TDR'd on the title VA)

The refused submits above are one check: `prepare_draw`'s vertex-buffer lookup (`buffer_at`) cannot cover the console VA
the draw binds - `...0B00000 + 0x3DF10`, inside the title's one 174 MiB placed buffer (device slot 133) whose pages
cannot be imported, so the stand-in is registered at its arena VA rather than the console VA. In the replay baseline
boot that is **43,750 refusals, one per draw** (`ExecuteCommandLists` 43,755 ok / 43,750 failed). Registering the
stand-in under the console VA removes **all 43,750 refusals** and the first draws execute (**3 `DrawInstanced`**) - then
the GPU faults: **TDR at 22.95 s**, two `nvlddmkm` event-153 records, title `0xC0000005` at 27.1 s. A control with every
vertex binding redirected to the zero page TDRs at the same point, so the fault is the un-rebased title VA in the
shader's `heap_base + console_va` arithmetic, not the fetched data; the candidate was reverted. The next work is the
title-placed page import (O16) and the arena range / VA rebase (O17): [replay_resolution.md](replay_resolution.md).
Until then a refused submit is the honest behaviour - executing an un-rebased title VA is a device reset.

Where the boot stands now: it runs its own frame loop for the whole 150 s timeout, builds every pipeline it asks for, and
records its draws - 42,722 in the decode boot, 43,750 in the later replay baseline - while presenting nothing. The
frontier is the title-VA address space (O17, with O16 behind it); the null display surface is the presentation frontier
behind that, and the fixed-function state blob (O15) is fidelity work that refuses nothing today.
