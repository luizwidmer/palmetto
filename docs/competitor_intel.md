# What VIctory does (and what we should take from it)

Another attempt at running the same title on PC, public since 2026-11-23: <https://github.com/Marvisak/VIctory>
(owner Marvisak, Rust, GPL-3.0, ~6.3k stars, ~800 forks). It is not a rival to ignore: it is the same problem solved
further along, by a different route. This note records what it teaches us and what we are not copying.

## Where it stands

| Stage | Their state |
|-------|-------------|
| Boot, first-boot screens (accessibility, language, settings, legal) | works, at the title's own 30 fps |
| Intro video with sound, start screen, profile, saves | works |
| Loading into the world (~50 s) | works |
| The world | reached, plays with indirect draws, title card renders, scene after it is dark; 0.5-1.8 s/frame |
| GPU hang risk on a real GPU | known; they ship a WARP (software) diagnostic mode because of it |

Test machine: Windows 11, RX 6700 XT. So: the whole platform layer boots and the *world* is where it still fights, at
frames measured in seconds. Our frontier is the draw's resource binding: the title's own frame loop runs, records
**43,750 `SetPipelineState` / 43,750 `DrawInstanced` per 150 s** and builds **all 10,241** slot-57 pipelines with no
refusal, but every draw-carrying submit is refused on the title VA the draw binds
([replay_resolution.md](replay_resolution.md)) and presentation is still empty
([display_path.md](display_path.md)), so their drawn world is still ahead of ours on the same road.

## The biggest structural difference: they do not use Vulkan

| | VIctory | Us (palmetto) |
|---|---|---|
| Shader output | **DXIL**, compiled at pipeline creation; cached on disk | SPIR-V, translated ahead of time from the shipped pipeline library |
| Host API | **D3D12** directly | D3D12.X console surface → Vulkan |
| Shader IR | SSA, one instruction per machine instruction; then `structure` removes EXEC (lane masks become per-lane booleans, wave mask tests become branches), `resource` traces every descriptor to its root parameter | instruction-by-instruction translation with refusal on anything it cannot model |

Their shader coverage, as their README states it: mesh 8,191/8,506, vertex+pixel 10,617/12,346, compute 1,643/2,539;
missing: 64-bit image atomics, merged geometry stages, the ray-tracing builder's shaders. That last one is our
`XboxBvhBuilder` finding, seen from the other side.

Their route is attractive because the console *is* D3D12: no console-to-PC surface translation, mesh shaders and
indirect execution are native, and DXIL is the format the console's own compiler family emits. Ours is attractive
because Vulkan runs everywhere and our index already covers 26.5k programs. A wholesale switch is not a decision to make
mid-boot; but it does mean "the shader index" is not the only possible design, and their coverage numbers are a useful
benchmark for ours.

## Loader lessons that validate or challenge ours

* **Static TLS at slot 0**: "an exe's code reaches its static TLS as slot 0; mapped as a DLL it gets another slot, so
  the loader redirects those loads". That is exactly our `patch_ga_tls_reads` (53,246 `gs:[58h]` sites), found
  independently. Our TEB validation (`443639062`) and theirs agree: this rewrite is required, not optional.
* **Prepare-then-map**: they patch a copy of the image (`IMAGE_FILE_DLL` set, entry 0, checksum 0, import directory
  pointed at an empty terminator, TLS callbacks pointer zeroed with a relocation made a no-op) so the host loader can map
  it without running its imports/TLS. We map manually instead. Their approach buys host-OS behaviour they do not have to
  re-implement; ours avoids the host loader entirely. Both work; theirs is smaller.
* **Crash reporting**: faults are named `module+rva` and the faulting thread's stack is unwound; every build appends to
  `crashes.log`. We have `module+0x...` and a watch, but no stack unwind — that is why we spent rounds inferring a call
  chain from static disassembly.
* **Import resolution order**: our HLE implementation → host function if policy allows that DLL → title DLL export →
  generated stub naming the import. Their `--unimplemented trap|zero` policy switch (trap in the debugger vs log-once
  and return 0) is a cheap, high-value idea: our refusals currently always continue, and continuing is what turns a
  refusal into a mystery fault ten frames later.

## Diagnostics worth adopting (all cheap, all local)

| Their variable | What it does | Our equivalent |
|---|---|---|
| `VICTORY_WATCH=<rva>,...` | logs a title u32 when it changes | `PALMETTO_WATCH_GLOBAL` (have) |
| `VICTORY_CRASH_SAVE=<rva>:<len>` | saves title memory at the crash | **none — adopt** |
| `VICTORY_STACKS=<s>,<s>` | every thread's stack at those seconds | **none — adopt (at the fault is enough)** |
| `VICTORY_SKIP_PIPELINES=<hash>` | omit draws/dispatches by shader hash | none (we refuse pipelines instead) |
| `VICTORY_D3D12_GBV`/`DRED`/`WARP` | validation, device-removed data, software adapter | n/a (Vulkan path) |

`VICTORY_CRASH_SAVE` and all-thread stacks are the two that would have shortened our current investigation: the value
that clobbers a callee-saved register would be visible in saved memory, and the call chain would be read rather than
reconstructed.

## Asset policy

Theirs is stricter than ours and enforced mechanically: nothing from the game in the repo; `.gitignore` for common data
types; `cargo xtask policy` runs on a pre-commit hook as an **allowlist** (file type, size cap, no binary data, no long
hex/base64 runs), with one documented exception (BCPack constant tables, 78 KB); every runtime path that writes derived
data calls `ensure_outside_repo`; if something slips in, history is rewritten rather than patched in a follow-up.

Our rules say the same things in prose and in review. Making ours mechanical — even a small pre-commit script that
refuses binaries, long hex runs and paths outside the work folder — would be the single most transferable piece of
process here. Their licence is GPL-3.0; ours is GPL-3.0 too, so code reuse is permitted with attribution and the same
licence, but the processes above are better re-implemented than copied.

## What we should not do

* Do not port their GPU stack or shader translator wholesale mid-boot: our pipeline index and translator work, and the
  current stop is a title-side pointer, not a shader gap.
* Do not adopt their BCPack-tables exception: our DirectStorage layer refuses what it cannot decode, on purpose.
* Do not copy their numbers into our docs as if measured here; cite them as theirs.


## XCL (ivall) - the same title, much further along, GPL-2.0-only

<https://github.com/ivall/xcl>, Rust, ~5.9k stars, ~700 forks, created 2026-11-20, GPL-2.0-only. A compatibility layer
in the Wine/Proton sense: the title's x64 code runs natively, console system libraries are reimplemented, and console
RDNA2 GPU code is recompiled to **HLSL and then DXIL** - the same destination as VIctory, reached through HLSL.

Its README reports four other target console titles fully playable (Minecraft Preview, Unicorn Overlord, Fortnite,
XDefiant) and, for the newer retail title we are on: **starts in ~4 s, a steady 30 fps without a crash, menus, and past
the first menu to its settings**, with gameplay not reached and "its minimap mesh pipelines are refused". It also
reports 26,698 / 26,698 shader programs translated and 909 / 912 console textures reproducing byte for byte.

**The licence is the first fact that matters to us: GPL-2.0-only is incompatible with our GPL-3.0-or-later, so no code,
no test and no data file from it can enter this repository.** Its documents and its measured claims are usable as
knowledge; its implementation is not. (VIctory is GPL-3.0, so its code would be compatible with attribution; XCL's is
not.)

What its feature table says it solved, and what that means for us:

| XCL area | Their state | Our state |
|---|---|---|
| PE loader, imports, TLS, game DLLs | done, data-driven import routing (HLE / Windows / stub) | done, same routing idea |
| Title memory shared by CPU and GPU, no copies, up to 16 GB in 4 GiB windows | done | **partly, and no longer the boot gap**: the boot reaches its frame loop without a CPU-visible fabricated VA (that earlier read was our own queue `GetDesc` clobber), and the remaining requirement is GPU-side addressing - import the title-placed pages (O16) and rebase the title VA into the one arena address space (O17), [replay_resolution.md](replay_resolution.md) section 5 |
| Console command lists *recorded in the console format and replayed* | done, graphics and compute, including lists recorded by console code | we translate live instead of recording/replaying |
| Pipelines | **non-blocking: a pipeline whose shaders are not compiled yet gets a stand-in at once and is built in the background** | 10,241 / 10,241 slot-57 records build with 0 refusals; the draw binds them, and the submit that carries it is refused on the resource VA, not on a pipeline ([input_layout.md](input_layout.md) section 6, [replay_resolution.md](replay_resolution.md)) |
| Shaders | RDNA2 -> HLSL -> DXIL, geometry/NGG/mesh/GS, ray-tracing instructions, wave ops, 100% of the pack | SPIR-V, 26,616 modules indexed, 0 pipeline failures at our stop |
| Wave64 on wave32 | a documented emulation that halves waves and exchanges masks/lane values | not addressed; we target subgroup 32 on the 4090 |
| Console textures | GPL-2.0-only code, all GFX10 modes, exact | tiling/swizzle still refuse rather than guess |
| Diagnostics | `report.json` per run: called stubs, unknown D3D12.X slots and interfaces, shader stats, audio formats, crash; frame dumps with a `non_dark_samples` count | we have per-slot counters, a crash report and a boot harness, but no per-run aggregate report |
| Tests | 370+, GPU tests that compare translated shaders instruction by instruction; real-data tests opt-in through `XCL_SAMPLE_*` environment variables | tests are synthetic and opt-in by construction; no GPU instruction-level oracle |

Three things worth acting on, in order:

1. **Non-blocking pipeline creation with a stand-in.** Their README says it plainly: a pipeline whose shaders are not
   compiled yet gets a stand-in immediately and is built in the background. That question is answered for the current
   tree by measurement: the graphics pipelines are built (10,241 / 10,241, [input_layout.md](input_layout.md) section 6),
   and the stand-in experiment had already shown the missing pipelines were not the boot blocker. The shape stays worth
   keeping for a pipeline the tree genuinely cannot build (the mesh path is still unbuilt).
2. **A per-run aggregate report.** Their `report.json` answers "what did the title ask for that we do not have" in one
   file. We have the events but read them by hand each round. A generated summary of one boot (unknown slots, refused
   interfaces, shader stats, stop) would pay for itself across a handful of rounds.
3. **The title-VA address space.** Their shared-memory window is how their loader names title memory; what our boot
   needs is narrower and now measured: import the title-placed pages (O16) and give every title-placed buffer an arena
   range so the recorded draws' title VAs can be rebased and execute (O17). A unified-memory design is not itself the
   requirement - the boot completes without one, and the register-only fix was reverted because it TDRs the GPU
   ([replay_resolution.md](replay_resolution.md) sections 4-5).

What not to copy: the HLSL/DXIL shader route (our SPIR-V index and translator work, and the current stop is not a shader
gap); the record-and-replay command-list design (a rewrite, not a fix); anything sourced from their repository.
