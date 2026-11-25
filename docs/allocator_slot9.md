# Allocator slot 9: the console callback registration

**Status: measured and served, 2026-11-25**, against the 1.0.2.1 target console executable on this repository's Vulkan
backend, one 130 s A/B boot pair under the boot lock. This page quotes RVAs, argument positions, counts and outcomes
only: no executable bytes, game data, package paths or extracted tables.

**Answer: console allocator slot 9 is the title's allocator-callback registration, `(callback, context)`, and the title
ignores the result.** The PC `ID3D12CommandAllocator` chain ends at slot 8 (`Reset`), so the allocator proxy refused the
call as past the PC vtable: `XboxID3D12CommandAllocator::Slot9 unsupported`, `E_NOTIMPL`, once per presented frame. In
the 600 s log that named this slice that was **55,167** entries, the boot's most frequent active refusal. The proxy now
records the pair on the allocator proxy it was called on (lifetime: the proxy's), answers `S_OK`, and never invokes the
callback or addresses the real allocator. The A/B boot pair shows the call served and **nothing else changed**: the
loading state, pipeline population, submits and the black presented image are the same (section 5), so this is a
compatibility answer whose value is that the most frequent silent incompatibility is gone and recorded, not a step that
advances the boot. The slice follows [the port policy](upstream_port.md); the reference cross-check is section 4.

## 1. Why this call is needed

The allocator proxy's table was 64 slots: the public chain (slots 0-8) is forwarded to the real allocator, and every
slot past it was logged once and refused with `E_NOTIMPL`. Slot 9 is not unknown tail: the title calls it on a measured
path, tens of thousands of times per boot, and the runtime answered every one of those with a refusal the title never
reads. A persistent `E_NOTIMPL` on a hot setup path is exactly the kind of silent incompatibility whose meaning has to
be settled, even when - as measured here - the title tolerates it. Two measurements make the case:

- the 600 s boot log of this tree carried **55,167** `allocator slot 9 ... returning E_NOTIMPL` trace entries (the
  `XboxID3D12CommandAllocator::Slot9 unsupported` call record), more than any other active refusal in that log;
- the 130 s before-arm of this slice's A/B pair carried **11,874** of them, at a steady ~91 calls/s, one per presented
  frame (section 2).

## 2. Measured shapes

The call was measured twice: statically from this repository's own copy of the retail executable (disassembly of the
call site and its containing function), and dynamically in the A boot with `PALMETTO_XS_CALLERS=1` and
`PALMETTO_XS_CALLERS_PROFILE=1`, which logs the title's return address and the argument words for each proxy slot.

| measured | value |
|---|---|
| call site (return address after the call) | `game+0x188501`; one site only, in the function `game+0x188430..0x18857d` |
| call shape at that site | `mov rcx,[registry]`; `mov rax,[rcx]`; `call [rax+0x48]` - the allocator in RCX, the slot is `0x48/8 = 9` |
| first argument (RDX) | a title `.text` function pointer, `game+0x555df10` (function `game+0x555df10..0x555df6b`); the same value in both arms and at every call |
| second argument (R8) | the context: `rsi = rcx - 0x12780` in the caller, i.e. a pointer into the title's own recording-context object; a different heap pointer per boot, the same shape at every call |
| third and later words | not arguments: R9 held arithmetic from the call site (measured `0`) and the first stack word an unrelated pointer, so the registration passes exactly two words. The callback's own body reads two later argument words when the console invokes it; that is the console's call, not this registration |
| answer before this slice | `E_NOTIMPL`, once logged; the trace recorded the call as unsupported |
| does the title test the answer | **no.** At the call site the instructions after the return overwrite RAX before any test (`game+0x188501` onward), and dynamically the title kept calling it once per frame for the whole 130 s boot, receiving `E_NOTIMPL` 11,874 times without retry, stop or visible fallback |
| frequency, 130 s A boot | **11,874 calls, 11,873 presented frames** - one per presented frame (91.1 calls/s) |
| frequency, 600 s log | 55,167 calls, the same rate over the longer boot |
| allocator proxies created per boot | a few hundred: the device-slot-9 caller profile lists 432 and 4 at its two busiest creation sites, and two startup-only creation sites sit below the profile's 40-row cut. With about 440 allocator proxies created and 12,455 registrations, the same allocator is registered again about 28 times over the 130 s - the per-frame rate on a reused allocator |
| what the callback would do | `game+0x555df10` allocates a block through `game+0x4d9f400` and `game+0x4d9f420` and stores it in the context at `+0x12798`, returning it: the console runtime's own way to give the allocator's recording context a memory block when it runs out. Nothing in this title reads the registered pair back |

The call is therefore a registration the console runtime owns: the title hands a function and its context to the
allocator it is about to use, on the console possibly once per allocator, and the console calls it when the allocator
needs memory. In the title as measured here the registration repeats on every frame, and the answer is discarded.

## 3. What we implemented

In `port/shims/d3d12_xs_device.cpp` and `port/shims/d3d12_xs_device.h`, in our own structure:

- the allocator proxy keeps the registered pair (`callback`, `context`) as member state, written context first and
  callback second with a release store, so a reader that sees a callback sees the context registered with it. The pair
  belongs to the proxy and is dropped with it; nothing global is touched, and a new proxy cannot inherit a released
  one's registration.
- `child_slot` gains an explicit `K == allocator && Slot == 9` branch, before the generic past-the-chain path: it logs
  the first call once, records the pair, records the trace entry as
  `XboxID3D12CommandAllocator::SetAllocatorCallbackX`, and returns `S_OK`. It does not invoke the callback and does not
  forward anything to the real allocator - the real object's own slot 9 is a different method (or past its vtable).
- `allocator_public_name` names the slot for the first-call and trace lines. The name `SetAllocatorCallbackX` is the
  public console HLE name for this slot (the same name the cross-checked reference uses); the shape is our own
  disassembly, not the name.
- `allocator_proxy_callback` returns a small `AllocatorCallback` struct for tests and diagnostics: `{0, 0}` when the
  slot was never called on that proxy, when the last call passed zeros, or when the pointer is not an allocator proxy.
  Nothing in the runtime reads it; the registration's only consumer would be the console runtime, which our layer
  replaces.
- the tail is unchanged: slots 10-63 are still logged once and refused with `E_NOTIMPL`, and a vtable read past the
  table still faults in the guard region. The public chain's slot count (`9`, slots 0-8) is unchanged, so no forwarded
  slot moved.

The switch is on by default, which the policy allows for an unimplemented import served honestly, because the A/B pair
shows no regression and the limits below are stated.

## 4. Cross-check against the reference

The behaviour was cross-checked against the public GPL-3.0 project
[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida), commit
`e29e9899809347a730fd57a14532a22fe0ee7231` ("d3d12_xs: handle ID3D12CommandAllocator slot 9 (SetAllocatorCallbackX)
on allocator proxy", DevilNine), files `runtime/shims/d3d12_xs_device.cpp`, `runtime/shims/d3d12_xs_device.h`,
`runtime/test/allocator_proxy_test.cpp` and `runtime/test/xs_proxy_mock_test.cpp`. No code or comment text was copied;
the ABI shape below was written from our measurement.

| topic | reference | this slice | why |
|---|---|---|---|
| what the slot is | a console-only allocator callback registration, named `SetAllocatorCallbackX` after the public HLE | same reading from our own call-site disassembly; the title passes one function pointer and one context | independent agreement on the shape |
| recorded state | callback and context stored on the proxy | same pair, in our own per-proxy record with a documented write order | our structure; the record is read only by tests |
| answer | `S_OK` | `S_OK` | the title does not read the answer; a success is the console's |
| the callback itself | recorded, never invoked on the PC/Vulkan path | recorded, never invoked | the layer owns its recording memory; invoking a title function with unmeasured arguments would be a guess |
| forwarding to the real allocator | not forwarded | not forwarded | slot 9 is past the PC allocator's 8-slot chain |
| refused tail | slots 10 and up refused | slots 10-63 refused, guard region unchanged | measured tail behaviour kept |
| frequency | the commit's comment says the call is "observed past the 9-slot PC chain" and names "trailing words", with no count | one call per presented frame (11,874 in 130.3 s), one caller site; the trailing words in the thunk are not arguments | our measurement is the second source and it disagrees on the trailing words: R9 and the first stack word are call-site residue, so only two words are recorded. This difference is recorded here, not smoothed over |

The reference's implementation adds the branch inside its generic past-the-chain block and keeps the pair in two scalar
fields with two getters; this slice has an explicit typed branch before the generic path, a single struct-returning
getter, and tests for per-proxy independence and for the record not surviving its proxy. That is design, not semantics:
both trees record the pair and answer `S_OK`.

## 5. A/B measurement

Two 130 s harness boots of this repository, one on the build before this slice and one on the rebuilt working tree, with
`tooling/boot_report.py --timeout 130` and the boot lock held, one boot at a time. Both arms carried the same
measurement switches: `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_PIPELINE_TIMELINE=5`, `PALMETTO_XS_CALLERS=1`,
`PALMETTO_XS_CALLERS_PROFILE=1`, `PALMETTO_DS_LOAD_TRACE=1`, `PALMETTO_DS_ARRAYS=1`, `PALMETTO_DS_STACK=1`,
`PALMETTO_CAPTURE=<dir>` and `PALMETTO_CAPTURE_EVERY=2000`. None of them changes a call; the caller probe and the
timeline are how the slot's call count and the frame counters were read.

The before arm is the tree's `port\build\palmetto.exe` and its `palmetto_tls_host.dll` snapshotted before the slice's
edit (built 11:40:22 local). The after arm is the same tree rebuilt at 11:50:04 with this slice. The B binary also
carries the other slices' uncommitted changes in the shared tree, as the tree's other A/B pages record; no change in
either arm knowingly touches the allocator callback path except this one.

| measurement | A: before | B: after |
|---|---|---|
| boot verdict | ALIVE 130.3 s (harness timeout), 0 exceptions, 19,230 startup-trace events, trace sequence reached #504,656 | ALIVE 130.3 s (harness timeout), 0 exceptions, 19,719 events, #526,738 |
| allocator slot 9 answer | `E_NOTIMPL`: 11,874 `XboxID3D12CommandAllocator::Slot9 unsupported` entries | `S_OK`: 12,455 `XboxID3D12CommandAllocator::SetAllocatorCallbackX called` entries, 0 unsupported entries in the whole boot |
| allocator slot 9 caller | `game+0x188501`, args `(game+0x555df10, <context>)` | the same site and callback; the one-time line `recorded callback ... context ...` printed exactly once |
| pipeline timeline at +125 s | slot57 10,241 converted / 0 refused; slot55 41,545; graphics created 51,786; compute 95/95; submits 23,507; presents 11,753; console calls 282,777 | slot57 10,241 / 0; slot55 41,545; graphics 51,786; compute 95/95; submits 24,476; presents 12,237; console calls 290,045 |
| per-frame calls | presents 11,873 (`PresentX ok`), submit slot 35 23,746, queue Signal 23,746, fence slot 12 11,879, allocator Reset 11,658, GetTimestampFrequency 11,875 | presents 12,454, slot 35 24,908, Signal 24,908, fence slot 12 12,460, allocator Reset 12,239, GetTimestampFrequency 12,455 |
| loader | 4,199 requests in the first second, 4,200 total, 6 submits; last change +16.1 s; heartbeats to +20.1 s unchanged (enqueued = done = 4,200) | 4,199 first second, 4,234 total, 17 submits; a 34-request batch at +19.1 s, then unchanged to +20.1 s |
| presenter captures | 6 captures (frames 1-10,001 every 2,000), every one mean rgb 0.00 0.00 0.00 | 7 captures (frames 1-12,001), every one mean rgb 0.00 0.00 0.00 |
| other trace entries | 0 unsupported entries besides slot 9; the same two pre-existing failure lines (slot-133 page import fallback, trace #1652 `ShaderCacheControl failed`) | the same two failure lines; 0 unsupported entries; every other traced method's count scales exactly with B's 581 extra frames (+1,162 for the two per-frame submits, +581/+580 for the per-frame present, fence, allocator Reset and timestamp calls) |
| loading state advanced? | no | no: same pipeline population, same stationary loop, same black image, the loader not re-armed after its startup batches |

**Verdict: unchanged.** The slice serves the slot: the boot's most frequent refusal (11,874 `E_NOTIMPL` in 130.3 s at
91.1/s; 55,167 in the 600 s log) becomes a recorded registration with `S_OK`, and the only trace-entry class that
changes is slot 9. B presented 581 more frames (+4.9%) than A; every other counter moves with that frame count by the
same per-frame ratio, which is the host frame-rate variance the tree's other A/B pages record (4-5%), and B's one extra
loader batch moved that activity 3 s later without changing its end state. The loading state, the pipeline population,
the submit count per frame and the black presented image are unchanged. This is reported as a **negative result for
boot advancement**: serving the slot is correct and required for compatibility, and it does not by itself move the
loading gate.

## 6. Limits

- **The callback is never invoked.** On the console the runtime would call it when the allocator's recording context
  needs another block; our Vulkan layer allocates that memory itself, so there is nothing for the title callback to
  provide. If some console path depended on the callback's return value, that path is not modelled. **This is the
  next falsifier:** a boot in which a stage fails only when the registration exists but the callback is never called
  (for example an allocator `Reset` failure or title memory exhaustion after registration) would make the console's
  invocation contract measurable. Until then, invoking a title function with unmeasured arguments would be a guess.
- **The console's invocation conditions are unknown** - first use, reset, exhaustion, or never - and so is the
  callback's full signature: it reads argument words the registration call does not pass (its 5th and 7th words).
- **Only the last registration is kept.** The title re-registers once per frame on the allocator it reuses; the proxy's
  record is the latest pair, which is what a console registration would replace. Nothing reads it back.
- **The host backend has no slot 9 to serve.** With the host PC runtime the device hands out the real allocator
  unwrapped (its lists do not resolve a proxy), so there is no console-allocator proxy on that path; this slice's boot
  and the extended tests' slot-9 checks are on the Vulkan backend the title runs on.
- **The refused tail is unchanged:** slots 10-63 stay refused, and a vtable read past the table faults in the guard
  region. The page [console_slots.md](console_slots.md) still describes slot 9 as refused; that page is outside this
  slice's write scope and is left for the lead to update with this slice's line.

## 7. Tests

`port/test/xs_proxy_mock_test.cpp` (GPU-free, no D3D12 runtime and no GPU; registered with ctest as `xs_proxy_mock`,
`xs_proxy_mock_no_reserve`, `xs_proxy_mock_no_protect`, `xs_proxy_mock_no_handler`, `xs_proxy_mock_env_slots` and
`xs_proxy_mock_env_slots_invalid`) extends section 6, `check_allocators`, with **21 assertions** on the slot-9 shape:
the call answers `S_OK` and reaches no real allocator slot 9; the pair is recorded on the proxy that received it; a
different pair replaces it and two zeros clear it; two allocator proxies keep separate records and a released proxy's
record is not inherited by the next one; the getter answers `{0, 0}` for a fresh proxy, for a null pointer and for a
device proxy; the counter and the first-call flag move; slots 10-63 still answer `E_NOTIMPL` without reaching the real
allocator. All six ctest entries pass.

`port/test/allocator_proxy_test.cpp` (the real allocator proxy on the Vulkan backend) extends `check_proxy` with the
same shape: **18 assertions** covering the fresh record, the success answer without forwarding, replacement, clearing,
per-proxy separation and a released proxy's record not being inherited, on top of the existing forwarding, guard-region
and QueryInterface checks. The binary compiles, but in the current shared tree it fails before its Vulkan section on a
pre-existing, unrelated queue-proxy failure: a PC `ID3D12CommandQueue::Signal` call (PC slot 14) is refused by the
queue proxy, which models that index as one of the console's two inserted methods. The instance predates this slice:
at the committed tree's `HEAD` the proxy already refuses slot 14 and both `allocator_proxy_test` and
`device_proxy_test` already call `queue->Signal` on the proxy (`git show HEAD:port/...`), so both fail at `HEAD` too;
it is a stale proxy test owned outside this slice, not a slot-9 effect (this slice's diff touches no queue code). The
slot-9 path on a real Vulkan allocator proxy is exercised by the B boot (12,455 registrations on real allocator
proxies, all `S_OK`, boot ALIVE).

## 8. Attribution

The behaviour in this page was established by this repository's own call-site measurement and A/B boots, and
cross-checked against the public GPL-3.0 project [ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida),
files `runtime/shims/d3d12_xs_device.cpp`, `runtime/shims/d3d12_xs_device.h`, `runtime/test/allocator_proxy_test.cpp`
and `runtime/test/xs_proxy_mock_test.cpp`, commit `e29e989980` by DevilNine. No code from that project was copied; the
full notice is in [NOTICE.md](../NOTICE.md). The upstream project remains the copyright of its contributors under
GPL-3.0.

## 9. Addendum (2026-11-25): invoking the callback

*This section appends this tree's measurement of the callback's own body and the invocation the slice now performs. It
keeps sections 1-8 intact: section 6's first limit ("the callback is never invoked") is answered here, and the rest of
that limit list still stands.*

### 9.1 What the callback's body reads

A linear disassembly of `game+0x555DF10..0x555DF6B` (the function the registration names) reads exactly three of its
seven argument words; the console runtime's own calling convention on x64 puts the first four words in RCX/RDX/R8/R9 and
the next three on the caller's stack, so the body's accesses decode as:

| the callback reads | what it does with it |
|---|---|
| argument 3 (R9) | the recording pool's memory class: passed as `edx` through `game+0x4D9F400` -> `game+0x4D9F450(context+0x12780, class, size)` |
| argument 4 (stack word 0) | the size: `2^size` when `size < 16`, otherwise the explicit byte count (a shift with a `cmovae` bound) |
| argument 6 (stack word 2) | the recording context: the base of the pool object; the block is stored at `context+0x12798` and the block already there is freed first through `game+0x4DA9F50` |

The other four arguments are never read by the body. The registration itself (`game+0x188501`) passes only
`(callback, context)`, so the console's invocation supplies the class and the size. Two static scans bound who could
invoke it: the callback address is referenced by exactly one instruction in the image (the `lea` at `game+0x1884EE`,
inside the registration caller `game+0x188430`), and `context+0x12798` is read by only the callback's own body. The
title therefore never calls the callback itself and never reads the block back; the console runtime was the only
invoker.

The same caller gives the class a measured value: immediately before it registers, `game+0x188430` allocates 8 bytes
from the same pool with class `0x18` (`game+0x18845E`: `edx = 0x18`, `r8d = 8`), so `0x18` is a class the title's own
recording pool accepts. The size is the one input with no measurement; the model uses one console recording chunk,
`0x10000`.

### 9.2 What is implemented and how it is switched

In `port/shims/d3d12_xs_device.cpp` and `.h`, off by default:

- the allocator proxy keeps a per-proxy claim flag; the **first registration on a proxy** is the model's invocation
  point, because that is where a console runtime would set the allocator's recording memory up. Later registrations on
  the same proxy only replace the pair, as before;
- with the switch on, the pair is invoked with the measured argument shape
  `(0, 0, 0, class 0x18, size 0x10000, 0, context)`: the context is read at `+0x12798` through the kernel before the
  call (an unreadable context is refused before any title code runs), and the returned block plus the cell before and
  after are logged;
- a null return, an unreadable context or a pair missing either word is **refused by name** and counted; the block is
  never freed by the runtime (the title owns its pool) and nothing reads it back (section 9.1);
- the switch is `PALMETTO_XS_ALLOCATOR_CALLBACK=1`; `PALMETTO_XS_ALLOCATOR_CALLBACK_TRACE=1` logs the registrations,
  `PALMETTO_XS_ALLOCATOR_CALLBACK_PROBE=<seconds>` invokes the pair once after that second as a one-shot measurement
  (bounded log), and the model's two chosen inputs can be moved for a probe with
  `PALMETTO_XS_ALLOCATOR_CALLBACK_SIZE` and `PALMETTO_XS_ALLOCATOR_CALLBACK_CLASS`. The counters are
  `allocator_callback_counts()`: registrations, first registrations, invocations, refusals;
- the companion change to the console command list (its slots 70/71/72/74) is the slice page
  [bottom_of_pipe.md](bottom_of_pipe.md); its two switches are `PALMETTO_XS_CONSOLE_LIST_EXTRAS` and
  `PALMETTO_XS_CONSOLE_LIST_TRACE` and its count summary is in that page.

### 9.3 Measurement and gates

The probe boot and the model arm are the boot pair of [bottom_of_pipe.md](bottom_of_pipe.md) section 5; this section
records the allocator-specific numbers there rather than a second pair. The probe arm ran with
`PALMETTO_XS_ALLOCATOR_CALLBACK_TRACE=1` and `PALMETTO_XS_ALLOCATOR_CALLBACK_PROBE=30`; its trace showed the callback
`0x14555DF10` and the same recording context on every logged registration, and the probe did not fire (the one-shot's
firing condition was fixed after that arm, so the arm is the registration measurement, not the invocation one). The
model arm ran with `PALMETTO_XS_ALLOCATOR_CALLBACK=1` and no probe: the pair is invoked once per allocator proxy on its
first registration. The bounded log records the first eight invocations, all successful:

| logged invocation | returned block | `context+0x12798` before -> after | class / size |
|---:|---|---|---|
| 1 | `0x1BB75400000` | `0` -> `0x1BB75400000` | `0x18` / `0x10000` |
| 2 | `0x1BB75420000` | `0` -> `0x1BB75420000` | `0x18` / `0x10000` |
| 3 | `0x1BB75440000` | `0` -> `0x1BB75440000` | `0x18` / `0x10000` |
| 4 | `0x1BB75460000` | `0` -> `0x1BB75460000` | `0x18` / `0x10000` |
| 5 | `0x1BB75480000` | `0` -> `0x1BB75480000` | `0x18` / `0x10000` |
| 6 | `0x1BB754A0000` | `0` -> `0x1BB754A0000` | `0x18` / `0x10000` |

The pool rounds the 64 KiB request to an `0x20000` stride; every invocation returned a block, wrote it into the cell
and freed the one the context already held (the "before" cell is `0` for the first eight allocations recorded, i.e.
each is a fresh context for the log's first entries). No null return and no refusal occurred, the boot stayed
`ALIVE 130.4 s` with 0 exceptions and 0 refused submissions, and the stage-2 state and job graph are the negative
recorded on the slice page. The exact total invocation count is the `invocations` field of
`allocator_callback_counts()`; the log's `0x0 -> block` entries above are the bounded first eight, and the earlier
census's allocator population (about 440 proxies a boot) bounds what the rest would be.

`port/test/xs_proxy_mock_test.cpp` adds `check_allocator_callback_invocation`: a synthetic callback proves the
invocation happens once per allocator proxy with `(class 0x18, size 0x10000, context)`, later registrations only
replace the pair, a null return and an unreadable context are refused by name, and the off arm invokes nothing.

### 9.4 Attribution

Same policy and attribution as section 8: our own measurement of the image and boots; the reference fork's recorded
answer (never invoked) is the cross-check of section 4's "the callback itself" row, and no reference code was used.
The invocation semantics are this repository's model, with the limits of [bottom_of_pipe.md](bottom_of_pipe.md)
section 6.