# Slot-70 matrix A at the current tree: the publication resolves, the compose pass paints, and the frame is black for a different reason

**Status: measured.** The console device slot-70 per-frame publication is already decoded, implemented and committed
(`port/shims/d3d12_xs_device.cpp`, `PALMETTO_SLOT70_PUBLISH=1`; [device_slot70.md](device_slot70.md) is the history,
including the falsified naive constant-buffer conversion). This page does not re-implement it. It measures the
combination that had never been booted together - the two measured vertex/constant feeds
([vertex_constant_feed.md](vertex_constant_feed.md)), the raster slice's measured cull axis ([raster_state.md](raster_state.md),
committed as `90075b9be`) and the slot-70 publication - with the PS-only control armed on the **compose** draw, whose
vertex stage is the one that multiplies by matrix A.

The result is positive on the descriptor axis and negative on the frame axis:

* **Matrix A resolves.** With the publication on, the compose pass's descriptor table carries the published matrix V#
  at its slot `s8+0x00` (stride 0, 5,768 declared bytes, the record's `0x1688` size and `30000fac` word), and the
  PS-only control on that draw paints **4,147,200 of 8,294,400** pixels of the presented 4K target - exactly the
  half-screen triangle the pass's own vertices and matrix fetch produce.
* **The control is what proves it.** With the publication off, the same switch set leaves the compose table's slot
  `s8+0x00` all zero and the same control paints **0 of 8,294,400**: the coverage is the publication's effect, not the
  feeds' or the cull axis's.
* **The presented frame is still black, and matrix A is not the reason any more.** Both the composed target `A` and the
  constant pass's target `B` read 0 of 8,294,400 in all four channels in every arm with the title's own pixel shaders,
  and all eight presenter captures are black. `B` stays zero even though the control proves the constant pass's
  geometry reaches the pixel stage: the remaining gate is the constant pass's **colour** path, not a descriptor.

No game data, package path, shader byte or extracted table appears here: ABI words, descriptor words, switch names,
readback statistics and counts only.

## 1. What this page adds over the committed pages

| already measured and committed | what this page adds |
|---|---|
| The slot-70 call's three-argument ABI, both call sites, the record layout and the destination kinds; the default refusal; the falsified CBV conversion ([device_slot70.md](device_slot70.md) sections 2-4) | Re-measured call shapes and publication lines on the current tree (section 3.1) |
| The matrix publication over the record's block (`772fa548b`), with its measured V# words and its PS-control acceptance at the compose draw on that tree | The same acceptance re-measured with the feeds and the measured cull axis, plus the publication-off causal control on the current tree (section 3) |
| The two feeds behind their switches, off by default (`58472232b`) | The first boot with feeds + cull axis + publication together |
| The measured cull axis, `CULL_NONE`, opt-in ([raster_state.md](raster_state.md)) | The compose draw, not only the constant draw, measured under it |

The earlier page's acceptance was a 128x128 window (`11776/16384`); this page repeats it as a full-4K readback so the
compose pass's coverage can be read directly, and adds the falsifier the earlier page did not have: the same arm with
the publication off.

## 2. Switches and arms

Every arm is one 150 s boot under the shared boot lock of one frozen binary (SHA-256
`099C347418098D6415204A0D7EE08FD78988F412FA808DBBB274CD7570EDC50C`, built 16:14:48 from the tree at HEAD `61f544a1b`
plus the slices' then-uncommitted work). The cull-axis code that binary ran is byte-identical to the content committed
at `90075b9be`; the slot-70 handler is the committed one; the tree advanced while the arms ran (by HEAD `53ea68331`,
16:45, with other slices' in-progress work), so every number below belongs to the frozen binary and not to the later
build. The common read-only set is `PALMETTO_FRAME_DUMP=1`, `PALMETTO_READBACK_FULL=1`,
`PALMETTO_READBACK_AT_DRAW=2000,2001`, `PALMETTO_PIPELINE_TIMELINE=10` and `PALMETTO_CAPTURE` every 300 presented
frames (8 captures); the lean arms drop the two device-memory diagnostics (`PALMETTO_FRAME_DUMP_SAMPLE=0`,
`PALMETTO_VERTEX_PATH=0`) the raster slice's decisive arm dropped, and the arm that needs the table dump sets
`PALMETTO_VERTEX_PATH=1`.

| arm | switches that differ | what it measures |
|---|---|---|
| RealCompose | feeds + cull axis + `PALMETTO_SLOT70_PUBLISH=1`, `PALMETTO_SLOT70_PROBE=1`; no substitution | the frame with the title's own pixel shaders |
| ComposeControl | the same + `PALMETTO_FRAME_DUMP=1` armed at draw 2001 with `PALMETTO_FRAME_DUMP_PS_CONTROL=1`, `PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE=1`, `PALMETTO_FRAME_DUMP_CONTROL_PS=fs_solid.spv`, `PALMETTO_VERTEX_PATH=1` | matrix A's delivery and the compose pass's rasterisation |
| PublishOff | ComposeControl without the publication (and with `VERTEX_PATH=1` on the retry) | the matrix-A-off falsifier |
| PublishOnly | `PALMETTO_SLOT70_PUBLISH=1`, `PALMETTO_SLOT70_PROBE=1` only (no feeds, no cull axis), readback at draws 4000/4001 | the publication alone: counters, refusals and that it does not disturb the shipping frame |

`PALMETTO_VERTEX_STREAM_DESCRIPTOR=1` and `PALMETTO_GRAPHICS_CONSOLE_ROOT=1` are the two feeds; they are off by
default in the committed tree and were set explicitly in the three feed arms. `PALMETTO_RASTER_CULL_MEASURED=1` is
the raster slice's opt-in cull axis; it is applied once per pipeline and logs one line
(`graphics_pso: measured cull axis: ... building with cull NONE and clockwise front`).

## 3. Measured numbers

### 3.1 Slot-70 call shapes and publications on the current tree

The probe prints the call shapes it measures. Both measured sites are the committed ones, unchanged:

| call | caller (return address) | arguments measured |
|---|---|---|
| call 1 (startup wrapper) | `game+0x4ECC821` | `arg1` a BUFFER resource description of the startup buffer (width `0xAE00000`), `arg2` a `0x40`-byte record whose first words are the CBV-shaped offline store, `arg3` a slot in the console descriptor heap page (`+0x220`), first word `0x12` before the call |
| call 36 and every later dominant site | `game+0x4D9F369` | `arg1` the 128-byte BUFFER description, `arg2` the `0x40`-byte record (count `0x20` at `+0x18`, block at `+0x28`, declared `0x1688` at `+0x08`, format word `0x16` at `+0x30`), `arg3` a zeroed slot in a host staging ring |

The publication lines are the committed ones. Four consecutive publications in one arm, with the V# the handler writes
(the base is the record's block title VA; the layer rebases it to the arena at submission, which is why the table dump
in section 3.2 shows an arena address with the same shape):

```text
slot 70 publish #1: block <title VA> (placed range <title VA> + 0xae00000, offset 0x3de20), declared 0x1688,
                    format 0x16 -> dest page=<host heap page> off=0xe650 V# <base lo> <base hi> 00001688 30000fac
```

* publications logged in the first four calls of the per-frame arm: 4 of the first 4 (the handler logs the first four,
  then every 4,096th slot-70 call);
* **refusals: none logged.** Each matrix-publication refusal category (block unreadable, block's first 64 bytes empty,
  block outside every slot-133-placed range) logs once, by name, the first time it happens; no such line appears in any
  arm, so no matrix call was refused in the measured windows;
* one **texture-shaped** publication is refused by name in every boot (the startup wrapper's 60-record census shape),
  exactly as the committed page records;
* the slot-70 call count in these windows is bounded, not exact: the probe's last milestone was call 2,048 and the
  4,096-call summary line was not reached (the boots present 2,109-2,487 frames in their 150 s; the committed
  publication boot presented 4,455 and recorded 12,214 applied / 0 refused). What these arms establish is the applied
  V# words and the absent refusal lines, not a new count.

### 3.2 The compose pass's descriptor table, and its control

The arm armed at the compose draw (identified from the same boot's frame dump: the substituted pipeline is the
6-image `fspx/lib_vs_00823` + `fspx/lib_ps_00822` draw, target `A`, the present plane) prints:

```text
frame dump:  #7 draw draw 2001 pipeline <compose pipeline> ps present 6 image(s) vs 213 load(s) ...;
             rt0 id <presented target> image <4K image> format 37 3840x2160
frame dump PS control: drew the armed draw's own vertices through the title pipeline (<compose pipeline>) with
             fs_solid as the pixel shader, into <presented target>; the readback below copies it
vertex path: fetch bases: s0/s1 pair 0xd4e2900, s8/s9 pair 0xca6dea0
vertex path: fetch slot table(s8)+0x00: <4 dwords> 0116de20 00000000 00001688 30000fac
             -> V# buffer address 0x116de20 stride 0 records 5768 format 0
vertex path: fetch slot table(s8)+0x20: <4 dwords> 01000016 c3800000 021bc3bf 90000fac
             -> T# image address <sampled image> format 56 3840x2160
vertex path: fetch slot table(s8)+0x40: <4 dwords> 0116dc40 00000000 000001e0 30014fac
             -> V# buffer address 0x116dc40 stride 0 records 480 format 20
```

Read as a statement: the compose pass's table slot `+0x00` **is the published matrix V#** - stride 0, a declared
`0x1688` bytes (5,768 records at stride 0), and the handler's `30000fac` word, with the base the layer rebased to an
arena address; slot `+0x20` is the sampled sibling 4K image; slot `+0x40` is the title's own 480-byte constants buffer
V# (matrix B). So both matrices the compose vertex stage reads are in the table, and matrix A is the one this slice's
publication produced. The same boot's `PresentX` call 1 names this draw's target as its plane-0 resource (a
slot-127/76-created 3840x2160 display surface), so the control's coverage below is the presented plane.

Its readback, with the substituted `fs_solid`:

```text
frame dump PS control readback [draw 2001 target <presented target>]: 3840x2160 at 0,0 of 3840x2160, 4-byte texel:
  mean 16 80 120, non-black 4147200/8294400, peak 240,
  alpha mean 127, alpha non-black 4147200/8294400, alpha peak 255
full-target tile map [draw 2001]: row 0: 64800 0 0 0 0 0 0 0
                                  row 1: 129600 64800 0 0 0 0 0 0 ... (one half tile, then full tiles stepping one
                                  column per row: the diagonal half-screen triangle)
```

The same arm's constant draw (the title's own pixel shader) read `B`:

```text
render-target readback [draw 2000 target <constant target B>]: 3840x2160 ...: mean 0 0 0, non-black 0/8294400,
  peak 0, alpha mean 0, alpha non-black 0/8294400, alpha peak 0
```

### 3.3 The causal control: publication off

The publication-off arm lands on a different draw parity (the boot's draw counter is not an even/odd frame rule - see
section 5, item 3), and the retry armed the compose draw:

```text
frame dump:  #7 draw draw 2001 ... ps present 6 image(s) ...; rt0 id <presented target>
frame dump PS control: drew the armed draw's own vertices through the title pipeline (<compose pipeline>) ...
vertex path: table VA <compose table>: 64 slot(s), 1 key(s): T#[1]<4K T# words>]
             data/V#[0]00000000 00000000 00000000 00000000] data/V#[2]0116dc40 00000000 000001e0 30014fac] ...
frame dump PS control readback [draw 2001 target <presented target>]: 3840x2160 ...: mean 0 0 0,
  non-black 0/8294400, peak 0, alpha mean 0, alpha non-black 0/8294400, alpha peak 0
```

So with the publication off the compose table's slot 0 is empty, matrix B is still present, and the same control paints
nothing. The only difference from section 3.2 is the publication; that is the causal pair.

### 3.4 Arm table

| arm | verdict | target `B` (constant, full 4K) | target `A` (presented, full 4K) | captures |
|---|---|---|---|---|
| RealCompose (real PS) | ALIVE 150.2 s, 0 exceptions | mean 0 0 0, non-black **0/8,294,400** | mean 0 0 0, non-black **0/8,294,400** | 8/8 black |
| ComposeControl (publish on, control on the compose draw) | ALIVE 150.2 s, 0 exceptions | 0/8,294,400 | **mean 16 80 120, non-black 4,147,200/8,294,400, peak 240**, alpha non-black 4,147,200 | 8/8 black (real frames) |
| PublishOff, first attempt (control landed on the constant draw) | ALIVE 150.2 s, 0 exceptions | control `mean 16 80 120, non-black 4,147,200/8,294,400` on `B`; the real compose readback on the other draw was 0/8,294,400 | not armed | 8/8 black |
| PublishOff, retry (control on the compose draw) | ALIVE 150.2 s, 0 exceptions | 0/8,294,400 | **0/8,294,400** (compose table slot 0 empty) | 8/8 black |
| PublishOnly (publication alone, no feeds, no cull axis) | ALIVE 150.2 s, 0 exceptions | 0/8,294,400 at draw 4000 | 0/8,294,400 at draw 4001 | 8/8 black |

Frame counters at +140 s in the same boots: 4,503/2,251 submissions/presents (RealCompose), 4,531/2,265
(ComposeControl), 4,219/2,109 (PublishOff retry), 4,821/2,410 (PublishOnly). Every boot's playlist still has two draws
per submission and one list per present, slot 57 converts 10,241/10,241 with 0 refusals and the graphics-pipeline
population is 51,786, as the committed pages record.

## 4. What the numbers say

1. **The matrix publication works end to end on this tree.** The compose pass's table slot `s8+0x00` is the V# the
   slot-70 handler wrote (declared `0x1688`, `30000fac`, stride 0, base rebased by the layer), and the compose pass's
   own vertices paint the half-screen triangle in the presented target under the control.
2. **The publication is the cause.** With it off, the same table slot is zero and the same control paints nothing; the
   matrix B slot and the sampled T# are unchanged in both arms.
3. **The compose pass is no longer the descriptor gap.** The earlier page left "whether the title copies the staging
   descriptor into its table" open; it does, and the copy plus the layer's rebase produce the shape the translated
   module gates its matrix loads on.
4. **Matrix A is not the presented frame's blocker.** With the title's own pixel shaders the presented target is still
   0/8,294,400 and every presenter capture is black. The compose pixel shader samples the constant pass's target `B`,
   and `B` is 0/8,294,400 even under the measured cull axis - although the control proves the constant pass's geometry
   reaches the pixel stage. So the loss is now in the constant pass's **colour path**, not in a missing descriptor.

## 5. Limits and the next falsifier

1. **The constant pass's colour source is unmeasured.** `fspx/lib_ps_00820` has 0 image samples; the data it reads is
   its own descriptor table (a 96-byte V# at slot 0, plus raw float dwords at `+0x40`/`+0x60` that are not descriptors).
   The 96-byte block's *content* was never read by a diagnostic: the constants printer walks non-image T# keys, and
   this table has 0 keys, so the block is invisible to it. **Next measurement:** read that block (predicate: the table's
   `data/V#[0]` base and its declared 96 bytes) and watch its title pages for writers. If the title writes it, the
   stationary loop's background is black by design ([frame_first_light.md](frame_first_light.md) section 10) and first
   light needs the title's content pass, not a layer fix; if nothing writes it, the console driver is expected to fill
   it (another driver block, like the vertex-stream one) and that is the next layer slice.
2. **The matrix values are not proven, only the delivery.** The control substitutes the pixel shader, so it proves the
   descriptor resolves and the vertex stage rasterises; it does not prove the 16 dwords fetched through the V# equal
   the console's intended transform. The matrix B slot and the sampled T# are present and unchanged; the transform's
   numeric agreement is what a real-PS, non-black `B` would exercise.
3. **The draw index parity is not a frame rule.** The 2001st draw was the compose pass in the ComposeControl boot and
   the constant pass in the first PublishOff boot (same switch set except the publication). Both arms identify the
   armed draw from the same boot's frame dump (the 6-image vs 0-image pixel stage) rather than from the index. A later
   slice arming by index alone should not assume even = constant.
4. **These arms ran no validation layer.** `PALMETTO_VK_VALIDATION` was not set; the failure scan found no
   `Validation Error`, `VUID`, `device-lost` or `ExecuteCommandLists failed` line and `boot.jsonl` records 0 exceptions,
   but a validation-layer arm is still the honest form of that claim. The first publish-off table attempt died at
   39.8 s with the title-side access violation (`game+0x2215c35`, write `0x0`) that
   [raster_state.md](raster_state.md) section 2 records as a boot-to-boot flake; its retry ran the window.
5. **The switches stay off by default.** The feeds and the cull axis were set explicitly in the feed arms; the
   publication is opt-in in the committed handler. This page changes no default: the publication's honest-answer case
   for default-on is exactly what item 1 blocks - the presented frame is still black, so the A/B that would justify a
   default has not been met.
6. The 8 captures are statistics over the presented swapchain image (mean 0.00 0.00 0.00, non-black 0/3,643,444 at
   2564x1421 in all arms); the full-target readbacks are the frame's own 4K targets. No boot measured past ~150 s, so
   the post-load frame is not claimed.

## 6. Tests, gates, evidence and files

* **Tests** (existing targets, no test changed by this slice): `port/build/xs_proxy_mock_test.exe` covers the slot-70
  publication (`check_slot70_console_view`: the V# words from the measured record, the empty/unplaced/element-count
  refusals, and the default refusal under `--env-slots-invalid`) and prints **PASS**; the `--env-slots` and
  `--env-slots-invalid` modes both exit 0 (`environment slot overrides outside 3..127 are ignored`).
  `unimplemented_policy_test` reports **98 checks, 0 failed** (the default-refusal policy); `frame_dump_test` passes
  (the driver-block decision and the ordered-writers/table-window checks); `graphics_pso_test` reports **272 checks
  passed** (including the measured cull axis); `graphics_record_test` reports **158 checks passed** (the record
  decode).
* **Gates:** `_setup\build_gta6rt.cmd` printed `PALMETTO_OK`, `_setup\build_tests.cmd` printed `TESTS_BUILT`; the
  documentation checks pass for this page (links, encoding, table rows/ids, list items, repeated lines, repo paths),
  and the two checks that fail on the surrounding tree fail on other slices' work, not on this page:
  `test_docs_env_vars.py` reports `PALMETTO_STAGE2_DISPATCH` read by the uncommitted `port/src/main.cpp` and named in
  no page, and `test_docs_cli_flags.py` reports `interval option` in `docs/frame_first_light.md`, `docs/memory_growth.md` and
  `docs/raster_state.md` not matched by any source script.
* **Evidence** (outside the repository, one directory per arm under `D:\leonida-work\slot70pub\`): `boot-real-compose`,
  `boot-compose-control`, `boot-publish-off`, `boot-publish-off-table` (the 39.8 s flake),
  `boot-publish-off-table2` (the retry), `boot-publish-only`; each has `boot\stderr.log`, `boot\boot.jsonl`,
  `boot\samples.csv`, `boot\arm_meta.txt` (frozen binary SHA-256, switch set, tree status, source hashes) and
  `capture\frame_*.ppm` for the presentation captures (black). The analysis script is
  `D:\leonida-work\slot70pub\analyze_slot70.py`; the boot harness is `D:\leonida-work\slot70pub\slot70_boot.ps1`.
* **Files:** this page only. No runtime or test file changed in this slice; the tree also carries other slices'
  uncommitted work (the lead commits through the normal flow).

This page adds no upstream material. The slot-70 handler it measures is another slice's committed work, cross-checked
and attributed on [device_slot70.md](device_slot70.md); the cull-axis switch is the raster slice's, attributed on
[raster_state.md](raster_state.md). The policy of [upstream_port.md](upstream_port.md) applies to the pages that
implemented those, not to this measurement.
