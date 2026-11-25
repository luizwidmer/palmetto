# The upscale stand-in: the measured upscale pass, the console upscaler's absent pipelines, and the layer's own stretch of the sampled colour

**Status: measured; the stand-in is implemented, tested and exercised, and the presented frame is still honest-black.** The
public peer claim is that the console build of AMD FSR 4 upscales the frame and its pipelines have no PC equivalent, so the
peer replaced the upscale with **a stretch of the colour image** ([cleanroom_peers.md](cleanroom_peers.md), the
PercyScarlett row). On this tree the *first half* of that mechanism is refuted by our own boot: the frame's upscale
(compose) pass is the title's own graphics pipeline, it is created, and on the measured draws it already samples the
constant pass's colour image six times and writes the presenter's target; none of the 29 FidelityFX programs is requested
in the window, and the shipped FSR DLL is only driven by the optional package-gated `fsr_dispatch_test`. The *second
half* is implemented anyway, as a clean-room fallback for a tree where the upscaler's pipelines cannot be made: a
layer-owned full-screen triangle that texel-fetches the resolved colour image and stretches its whole texel rectangle
over the presented target's whole pixel rectangle (`PALMETTO_UPSCALE_STANDIN`, off by default, with named refusals). It
is verified with tests and one boot; the frame stays black because the colour image is black **by design** in the
stationary loop ([colour_path.md](colour_path.md)), which the stand-in cannot and does not change.

No game data, package path, shader byte, extracted table, hash or address from the user's copy appears here: switch
names, ABI/descriptor shapes, counts and readback statistics only.

## 1. Why this slice

[cleanroom_peers.md](cleanroom_peers.md) row 3 orders this slice after the input and save slices "because it turns the
compose pass's sampled colour into a presented picture when the console upscaler's pipelines cannot be made". The
preceding picture measurements left the frame at a specific point:

* the frame is a constant/background pass (`fspx/lib_ps_00820`, 0 image samples) plus an upscale/compose pass
  (`fspx/lib_ps_00822`, 6 image samples) that samples the constant pass's 4K sibling target and writes the presenter's
  4K plane-0 target ([frame_capture.md](frame_capture.md) section 7, [slot70_publication.md](slot70_publication.md)
  section 3.2);
* the compose pass's vertex stage, raster state and matrix fetch are measured: a PS-only control on that draw paints the
  measured half-screen triangle once the slot-70 matrix-A publication is on
  ([slot70_publication.md](slot70_publication.md) section 3.2);
* the presented frame is still black, and the cause is the constant pass's colour window, which is zero by design while
  the title is in its stationary pre-content loop ([colour_path.md](colour_path.md) section 5).

So the question this slice had to answer before writing code was: **is our tree in the peer's failure state at all**, or
does it already present its sampled colour through the title's own upscale pipeline? The policy of
[upstream_port.md](upstream_port.md) says measure first, implement our own, state the limits.

## 2. Measured: the upscale path and the stand-in switches at HEAD

### 2.1 The measured upscale pass

The frame is two draws per submission, both full-3-vertex triangles into 3840x2160 format-37 targets; the second one is
the upscale pass. The boot's own frame dump identifies it without a name guess: the constant draw's pixel stage has
**0 image samples** (`ps present 0 image(s), vs 77 load(s)`) and the upscale draw's has **6** (`ps present 6 image(s),
vs 213 load(s)`). Both are the title's own pipeline-library graphics records created through device slot 57 from the
package's pipeline library; the translated modules are the corpus entries `fspx/lib_vs_00823` + `fspx/lib_ps_00822`
([frame_capture.md](frame_capture.md) section 7 traces the PS's six samples and their one sampled T#). The sampled T# at
the upscale draw resolves to the sibling pass's 4K target ([frame_capture.md](frame_capture.md): same layer image, same
resource id), which is the colour image this slice's stand-in reads.

The measured shape the stand-in implements is therefore: **one colour image (3840x2160, format 37) stretched over one
presented target (3840x2160, format 37)** - the measured case is a 1:1 stretch, and the generic geometry (source texel
rectangle, destination pixel rectangle) is what the unit test pins for a non-1:1 source as well.

### 2.2 The console upscaler's pipelines: requested by nobody in the window

The peer's precondition - the game asks for the console FSR 4 pipelines and they cannot be made - does **not** hold on
this tree's boot:

| measurement | result |
|---|---|
| FidelityFX DLL programs requested | **0 of 29** on this boot: 33 distinct `no translated compute shader for FSPX kind-6 and kind-0` keys were refused, and none of the 33 equals any of the 29 FidelityFX programs' `fspx_code_sha256` in the build's `fspx/index.json` - all 33 are the title's own pipeline-library programs (the earlier [fspx_pipeline_map.md](fspx_pipeline_map.md) measurement, 39 distinct keys, says the same) |
| `amd_fidelityfx_upscaler_xbox.dll` loaded or called by the title in the boot | **no**: no boot-log line maps or calls it in the measured window; the FSR DLL is exercised only by the optional package-gated `fsr_dispatch_test` ([d3d12_vulkan.md](d3d12_vulkan.md) appendix A.3) |
| the upscale pass's own graphics pipeline | created: slot 57 converts **10,241 of 10,241** records with 0 refusals and the boot's graphics-pipeline population is 51,786; the compose draw reaches `prepare` with a resolved `VkPipeline` and the PS-only control draws through it (section 4.1) |
| console slot 57 records | the title's graphics records are converted; the only stand-in left there is the documented fixed-function attachment stand-in ([slot57_wiring.md](slot57_wiring.md)), not a pixel-stage substitute |

The `fspx/lib_cs_*` and `fspx/lib_ps_*` names in the boot log are the project's corpus directory for the **title's own
pipeline library**, not the FidelityFX DLL's 29 `FSPX` programs; reading the two as one is exactly the assumption this
measurement removes.

### 2.3 What the existing FSR/stand-in switches actually change

| switch | where it is read | measured state on the boot path |
|---|---|---|
| `PALMETTO_FSR_WARP`, `PALMETTO_FSR_DEVICE`, `PALMETTO_FSR_NO_STANDIN` | `port/test/fsr_dispatch_test.cpp` only (the optional package-gated test that maps the shipped FSR DLL and calls its public `ffx*` API) | not read by the runtime, not part of a boot; they change the test's adapter and whether it creates a simple stand-in texture, not the title's frame |
| `PALMETTO_STANDIN_PIPELINES` | `port/shims/d3d12_vulkan_device.cpp`: a compute pipeline whose program has no translation is handed the last translated module's pipeline; `port/shims/d3d12_xs_device.cpp`: a console slot-57 record the tree cannot convert is handed the project's VS+PS triangle pair | both **diagnostic placeholders**, off by default; neither samples an image and neither is a stretch; the measured boot never enters the slot-57 one because its records convert |
| `PALMETTO_PRESENT_*` (`present_compose.comp`) | the presenter's PresentX plane composition | a real measured stretch (source rectangle -> destination rectangle per plane) but **downstream of** the title's own upscale pass: it presents what the upscale pass wrote, it cannot substitute for it |

So the tree had no stand-in that *presents the sampled colour in place of the upscale pass*, and no switch that would do
that when the upscaler's pipelines cannot be made. That is what this slice adds, opt-in and off by default.

## 3. What we implemented

### 3.1 The decision and the geometry (`port/src/upscale_standin.h`)

Pure, GPU-free code that the unit test exercises directly:

* the measured predicate `is_compose_pass(has_pixel_stage, ps_images)`: the upscale pass is the draw whose pixel stage
  takes the colour image through the measured **six** image instructions; a 0-image constant pass or a depth-only
  pipeline is not it;
* `plan(Inputs)`: the stand-in draws only when the switch is on, the armed draw is the measured upscale pass, the colour
  image resolved to a layer image and both extents are non-zero; otherwise it returns a **named refusal**
  (`switch off`, `not the measured upscale pass`, `no resolved colour image`, `zero colour extent`, `zero target extent`);
* the geometry: source rectangle = the colour image's whole texel rectangle (`0,0,w,h`), destination rectangle = the
  presented target's whole pixel rectangle (`0,0,W,H`); `map_dest_pixel`/`sample_texel` are the shader's exact
  destination-centre-to-source-texel formulas (nearest filter), and `mapping_control_colour` is the project's diagnostic
  coordinate ramp (see below);
* `Push`/`write_push`: the two rectangles and the mode are the first 48 bytes of the 144-byte push block the replay
  already sends every graphics draw (`ShaderUserData`), so the layer's own draw needs no new push path;
* switch getters: `PALMETTO_UPSCALE_STANDIN` (the stand-in) and `PALMETTO_UPSCALE_STANDIN_CONTROL` (the diagnostic ramp
  described in section 3.4);
* the shipping plan **always samples the colour image** (`samples_image(plan) == true`, mode 0). It never synthesises,
  tints, clears or fills a colour; the only non-sampling plan is the explicit diagnostic ramp.

### 3.2 The layer's own draw (`port/src/vulkan_backend.h`, `vulkan_replay.h`)

When the switch is on, the queue (a) finds the measured upscale draw of the submission (`frame_dump::compose_target`,
the same prepared-list view the frame dump uses), (b) resolves the colour image from the draw's own recorded descriptor
tables (`SampledTable`/`SampledKey`: the first T# the shim resolved to a layer image - the same resolution the
sampled-texture readback uses), (c) runs the decision above, and (d) records **the project's own draw** through the same
`prepare`/`emit` path as the frame-dump controls, with a reserved pipeline id (3, distinct from the two control ids):

* a new full-screen-triangle vertex stage and a pixel stage that texel-fetches the colour image
  (`upscale_standin.vert`/`.frag`, compiled into `upscale_standin_spv.h` with the same `glslc --target-env=vulkan1.1 -O
  -mfmt=c` invocation as `present_compose.comp`);
* its own pipeline layout and **own descriptor set 0** (one `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`), fitted with an image view
  of the resolved colour image and bound by the replay's emit for that draw only;
* a colour-image layout transition from the layout the tracker names for the draw (GENERAL stays GENERAL - the layer's
  sampled-in-general mode - otherwise SHADER_READ_ONLY) and back afterwards, so the tracker's state for later commands is
  unchanged;
* the draw targets exactly the presented target resource the upscale draw writes (same resource id, format and full
  viewport/scissor), which is the presenter's plane-0 resource on the measured boot
  ([slot70_publication.md](slot70_publication.md) section 3.2);
* bounded logs: one line for the built pipeline (image, format, target format, sampling layout), one per recorded draw
  (source/destination extents, colour image, layout), and one named line per refusal. `PALMETTO_UPSCALE_STANDIN` unset
  leaves the measured path byte for byte.

### 3.3 Why it runs at the armed draw, and not every frame

The stand-in executes at the submission whose draw is the one the frame dump/readback armed, and only when that draw is
the measured upscale pass (the boot arms draw 2001, the compose draw). That is a deliberate limit, named in section 6:
the title rotates its descriptor tables and colour block through a ring (three sampled positions in
[colour_path.md](colour_path.md) section 3.1), so a colour image that changes from frame to frame needs a per-image view
and descriptor cache; one measured execution per boot keeps this slice's evidence exact and its failure modes bounded.

### 3.4 The mapping control

When the title's colour image is black (the measured state), a correct stretch of it is black too, and a black readback
cannot distinguish "the stand-in drew the sampled colour" from "nothing drew". `PALMETTO_UPSCALE_STANDIN_CONTROL=1` adds
a second project draw with the same geometry whose pixel stage writes **the mapped source coordinate** instead of a
colour. Its full-rect readback is a horizontal/vertical ramp with mean about 128 128 0 - a value neither the black frame
nor the constant `fs_solid` control colour (32 160 240) produces - so the boot measures the rectangle arithmetic even
while the colour is black. This is a diagnostic, not the shipping mode: the shipping mode always samples (section 3.1),
and the code never mixes the two (`samples_image` is tested both ways).

## 4. Boot measurement

One `150 s` boot per arm under the shared boot lock (`D:\leonida\_setup\boot_lock.ps1`), same frozen binary
(`D:\leonida-work\upscale\bin\palmetto-upscale.exe`, SHA-256
`72E4A8FF60EF67335E8E202A741D4C5899D9C0B7596990E6869ACD2DB4CC9CD8`, built 17:48:59 from the working tree at HEAD
`133cbd6ec` plus this slice's uncommitted files and the other slices' in-flight work; the arm's `arm_meta.txt` records
tree HEAD `77e44e0ab` at boot start - the lead's own stage-2 diagnostic commit landed between the build and the arm, and
the frozen binary is unaffected), started one at a time; both arms carry the same common set:

* `PALMETTO_SHADERS=<spirv dir>`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`;
* the two measured feeds `PALMETTO_VERTEX_STREAM_DESCRIPTOR=1` and `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`, the raster
  slice's `PALMETTO_RASTER_CULL_MEASURED=1`, and the slot-70 publication `PALMETTO_SLOT70_PUBLISH=1`,
  `PALMETTO_SLOT70_PROBE=1`;
* the readbacks `PALMETTO_READBACK_AT_DRAW=2000,2001`, `PALMETTO_READBACK_FULL=1`, the frame dump at draw 2001 with the
  sampled-texture probe `PALMETTO_FRAME_DUMP_SAMPLE=1`, `PALMETTO_PIPELINE_TIMELINE=10`, and 8 presenter captures every
  300 presented frames.

| arm | switches that differ | what it measures |
|---|---|---|
| Control | `PALMETTO_FRAME_DUMP_PS_CONTROL=1` + `PALMETTO_FRAME_DUMP_CONTROL_PS=<fs_solid.spv>` + `PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE=1` | the title's own upscale pipeline, geometry and target: the measured path this slice must not regress |
| Standin | `PALMETTO_UPSCALE_STANDIN=1` + `PALMETTO_UPSCALE_STANDIN_CONTROL=1` | the layer's own stretch draw: the resolved colour image, the layout transitions, the two readbacks |

### 4.1 Control arm: the measured path

Binary `D:\leonida-work\upscale\bin\palmetto-upscale.exe`, SHA-256
`72E4A8FF60EF67335E8E202A741D4C5899D9C0B7596990E6869ACD2DB4CC9CD8` (this is the binary of the first build; the
stand-in switch is off in this arm, so the later index fix - section 4.2 - is not compiled into this arm's behaviour:
the whole stand-in block is one false branch). 150.0 s, game ALIVE for the whole window, **0 exceptions** in the trace,
no device-lost or `Validation Error` line, 4 presenter captures. At +140 s: 3,759 submissions / 1,879 presents;
slot 57 **10,241 calls, 10,241 converted, 0 refused**; graphics pipelines created **51,786**; compute 269 calls / 226
created; console calls 135,603.

| measurement | result |
|---|---|
| armed draw 2000 | the **0-image constant pass**, target resource `0x2a89ff03b90` image `0x2abc4025ad0`, format 37, 3840x2160 |
| armed draw 2001 | the **6-image upscale pass** (`ps present 6 image(s) vs 213 load(s)`), target resource `0x2a81852acf0` image `0x2a7d8614ed0`, format 37, 3840x2160 |
| the upscale draw's colour image | its single T# (table `0x29a81c5dea0`, slot 1) resolves to image `0x2abc4025ad0` = draw 2000's target; sampled 128x128 readback `mean 0 0 0, non-black 0/16,384` |
| constant target B (full 4K readback) | `mean 0 0 0, non-black 0/8,294,400, peak 0`, alpha `0/8,294,400` |
| **PS-only control through the title's upscale pipeline** | `mean 16 80 120, non-black 4,147,200/8,294,400, peak 240`, alpha mean 127, alpha non-black 4,147,200: the measured half-screen triangle - the upscale pipeline exists and paints the presented plane |
| presenter captures (4, 2564x1421) | all `mean 0.00 0.00 0.00, non-black 0/3,643,444` (the frame's real output) |
| `upscale stand-in` log lines | none: the switch is off and the measured path is unchanged |

### 4.2 Standin arm: the layer's stretch

One 150 s arm was started and completed on the first build (binary SHA-256
`72E4A8FF60EF67335E8E202A741D4C5899D9C0B7596990E6869ACD2DB4CC9CD8`); it is an **honest negative** about that
build's wiring, and the fix is in the tree but not in a boot yet:

* the arm ran 150 s with the game alive, the same common switch set, and the two readbacks at draws 2000/2001;
* **no stand-in draw was recorded**: every submission logged `upscale stand-in: no draw: the armed draw is not in this
  submission ...` - the wiring compared the measured upscale draw's **per-submission** index with the **global** armed
  draw index, so the comparison could never be true outside the first submission;
* the readbacks show the unsubstituted frame: draw 2000 `mean 0 0 0, non-black 0/8,294,400`, draw 2001 `mean 0 0 0,
  non-black 0/8,294,400` - exactly the black frame this slice does not change;
* the fix is `armed.first + compose.index == armed.at` (the measured draw's global index); the binary was rebuilt
  (`D:\leonida-work\upscale\bin\palmetto-upscale.exe`, SHA-256
  `731934FD492D31E14C59C5A443D14DDCA7918C6EC287CFAF0287290AF07EA52E`, built 18:03:04) and the unit tests were rerun
  green, but the shared boot window for the second arm was yielded to the stage-gate measurement and the cdb session by
  the lead's boot-lock schedule, so the **fixed binary's stand-in arm is not measured yet**. The exact next falsifier is
  recorded in section 6, item 2: one boot with `PALMETTO_UPSCALE_STANDIN=1` and
  `PALMETTO_UPSCALE_STANDIN_CONTROL=1` must log one `upscale stand-in: drew the sampling full-screen triangle ...` and one
  `... mapping-control ...` line at the armed draw, and its presented-target readback must read the mapping-control ramp
  (`mean` about `128 128 0`, non-black all 8,294,400) while draw 2000's target stays black.

## 5. Verdict

1. **The peer's premise is absent on this tree, and that is the measurement's first result.** The frame's upscale pass is
   not an unmade console FSR pipeline: it is the title's own graphics pipeline, slot 57 converts its record, the pipeline
   is created, and the compose-draw control paints its measured half-screen triangle in the presented target
   (section 4.1). No FidelityFX program is requested in the window (0 of 29), and the shipped FSR DLL is not loaded by
   the boot. So replacing the upscale with a stretch is **not** what would turn our black frame into a picture - the
   sampled colour image is black by design ([colour_path.md](colour_path.md)).
2. **The stand-in the peer describes is nevertheless implemented in our own terms**: the decision and geometry are pure,
   tested code; the draw is the layer's own full-screen triangle, pipeline and descriptor set over the resolved colour
   image; the shipping mode samples and never invents colour; every refusal is named; the switch is off by default so
   the measured path is byte-for-byte the baseline (the Control arm logs no stand-in line at all).
3. **The implementation is test-verified but its boot is still the named next step.** The first arm's wiring bug was
   found by that boot and fixed; the fixed binary has not been measured because the shared boot window was scheduled to
   the stage-gate and cdb work. The slice's honest status is therefore: decision/geometry/tests measured, draw path
   implemented and reviewed, one boot negative on the earlier wiring, and one boot outstanding (section 6, item 2).
4. **The frame stays black, by design, in both arms.** No presented-target readback or presenter capture in the Control
   arm is non-black; the stand-in presents whatever the colour image holds, and today that is zero. The picture frontier
   remains the title's stage step, not this layer.

## 6. Limits and the next falsifiers

1. **The peer's precondition is absent on this tree.** The upscale pipeline is the title's own and is made; no FidelityFX
   program is requested. The stand-in is therefore a fallback for the failure the peer reports, not the fix for our
   current black frame - and the current black frame is the colour window, measured negative in
   [colour_path.md](colour_path.md).
2. **The fixed binary's stand-in draw is not booted yet, and the measured execution is one per boot.** The first arm
   found the per-submission/global index bug (section 4.2); the fix and rebuild are in the tree
   (`731934FD...A52E`) but the boot window went to the stage-gate/cdb measurements. The exact falsifier: the next arm
   with `PALMETTO_UPSCALE_STANDIN=1` + `PALMETTO_UPSCALE_STANDIN_CONTROL=1` logs the two `drew the ... full-screen
   triangle` lines at the armed draw and reads the ramp in the presented target. After that, a per-frame substitution
   needs the colour image ring (section 3.3) - the next switch step.
3. **The measured geometry is 1:1 (4K colour image over a 4K target).** The non-1:1 mapping is pinned by the unit test's
   formulas but no boot has a lower-resolution colour image yet; when one appears, the same plan (whole source rectangle
   to whole destination rectangle) applies without code change.
4. **Nearest filtering.** The stand-in is a nearest-filtered stretch, not a quality upscaler; it deliberately invents
   nothing. If the source resolution ever drops, a filtered variant is a separate, measure-first decision.
5. **No validation layer.** `PALMETTO_VK_VALIDATION` was not set in either arm; the layout transition pair and the
   descriptor binding are checked by the boot completing and the readbacks, not by the validation layer. A validation
   arm is the honest form of that claim.
6. **The presented frame is black by design.** The stand-in presents the sampled colour, and the sampled colour is black
   in the stationary loop; no picture is expected from this slice alone. The picture frontier stays the title's stage
   step or first content pass ([stage2_blocker.md](stage2_blocker.md), [frame_capture.md](frame_capture.md) section 10).

## 7. Tests, gates, evidence and files

* **Tests** (`port/test/frame_dump_test.cpp`, exercised by the existing `frame_dump_test` target): the six-image
  predicate and its near-misses; every named refusal (switch off, constant pass, 0-image pass, no resolved colour image,
  zero colour width/height, zero target width); the measured full-rect geometry; the 4K-to-4K 1:1 mapping (first, middle
  and last destination pixel sample their own texels); the 1080p-to-4K stretch (two destination pixels per source texel,
  last pixel clamped inside the source); the mapping control is not the shipping plan and keeps the geometry; its ramp
  endpoints and their mean; the push-block round trip; and the stand-in's pipeline id distinct from both frame-dump
  control ids.
* **Gates**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints `TESTS_BUILT`; the matching
  test and the other targets listed in the report pass; the documentation checks (encoding, links, env vars, list items,
  repo paths, table ids/rows, repeated lines, python deps) pass for this page.
* **Evidence** (outside the repository, under `D:\leonida-work\upscale\`): the frozen binaries and TLS host in `bin\`
  (`palmetto-upscale.exe` SHA-256 `72E4...9CD8` for the booted arms, `7319...A52E` for the fixed wiring), the boot
  harness `upscale_boot.ps1`, and `boot-control\` / `boot-standin\` with each arm's `boot\stderr.log`,
  `boot\boot.jsonl`, `boot\samples.csv`, `boot\arm_meta.txt` and `capture\frame_*.ppm`; the capture statistics are
  reproduced by `analyze_captures.py`. The page's numbers come from those files.
* **Files** (all uncommitted; the lead commits): new `port/src/upscale_standin.h`, `port/src/upscale_standin.vert`,
  `port/src/upscale_standin.frag`, `port/src/upscale_standin_vs.inc`, `port/src/upscale_standin_ps.inc`,
  `port/src/upscale_standin_spv.h`; changed `port/src/vulkan_backend.h` (the stand-in pipeline/descriptor/draw and its
  switch block), `port/src/vulkan_replay.h` (`compose_target`, the measured upscale draw of a submission, and
  `ArmedTarget::index`), `port/test/frame_dump_test.cpp` (the tests above), and this page. No git writes.

## 8. Attribution

The peer sentence this slice implements is public wording from **PercyScarlett's** project README as summarised in
[cleanroom_peers.md](cleanroom_peers.md): the Series S world was black because the game upscales with the console FSR 4
build, whose pipelines have no PC equivalent, and "the upscale is now a stretch of the colour image". Only that public
description was used as a hypothesis; no peer source code was read or copied. The FidelityFX-program measurement this
page relies on is this project's own ([fspx_pipeline_map.md](fspx_pipeline_map.md),
[d3d12_vulkan.md](d3d12_vulkan.md) appendix A.3), and the upscale pass's identity is drawn from this project's own
measurements ([frame_capture.md](frame_capture.md), [slot70_publication.md](slot70_publication.md)). The policy of
[upstream_port.md](upstream_port.md) is followed in the form "measure first, implement our own, state the limits", and no
upstream implementation was consulted for this page.

## 9. Verification measurement: the mapping-control arm (task-111)

The fixed-wiring arm that section 4.2 recorded as outstanding has now been booted. One `150 s` boot under the shared
boot lock (`D:\leonida\_setup\boot_lock.ps1 -Acquire -Who standinverify`, released immediately afterwards), on a frozen
copy of the current tree's own build: `D:\leonida-work\upscale\bin\palmetto-upscale-verify.exe`, SHA-256
`F32BCF50E3BBEB9646C53B01E95E8432DB0CB4DE82800289A6F56FBFCFEB68D2` (frozen from `D:\leonida\port\build\palmetto.exe`
at 18:21:27; `_setup\build_gta6rt.cmd` prints `ninja: no work to do. PALMETTO_OK`, so the binary is the current tree's).
Its source hashes are recorded in the arm's `arm_meta.txt` and are the fixed ones: `vulkan_backend.h` `EA25...A3A`
(including the `(armed.first + compose.index) == armed.at` comparison), `vulkan_replay.h` `0609...76B5`,
`upscale_standin.h` `B113...4967`, `upscale_standin_spv.h` `22A5...F2628`. The common switch set is section 4's, plus
`PALMETTO_UPSCALE_STANDIN=1` and `PALMETTO_UPSCALE_STANDIN_CONTROL=1`; the readbacks arm draws 2000/2001, the frame
dump arms 2001, and 8 presenter captures are taken every 300 presented frames.

| measurement | result |
|---|---|
| armed submission | submission 2005 at about `+90 s` (timeline: 2,006 submits / 1,002 presents); draw 2000 is the 0-image constant pass, draw 2001 the 6-image upscale pass (`ps present 6 image(s) vs 213 load(s)`) writing target resource `0x23174545700` (image `0x23134774ed0`, format 37, 3840x2160) |
| stand-in pipeline line | `[rt] replay: upscale stand-in: built the layer's stretch pipeline (target format 37) over colour image 0x234cc245ad0 (format 37) in layout 1`: the resolved T# is draw 2000's target, sampled in GENERAL (layout 1), and the line is only reached after the plan takes the drawing branch, so no named refusal fired |
| presented-target readback (draw 2001, full 4K) | `mean 127 127 0, non-black 8,294,360/8,294,400, peak 255`; alpha `mean 255, non-black 8,294,400/8,294,400`: the documented mapping ramp (`mean` about 128 128 0). The 8x8 tile map is 129,600/129,600 non-black in 63 tiles and 129,560/129,600 in the top-left one (the ramp's origin texels) |
| constant target readback (draw 2000, full 4K) | `mean 0 0 0, non-black 0/8,294,400, peak 0`; alpha `0/8,294,400`: black by design, unchanged |
| colour image readback | 128x128 window `mean 0 0 0, non-black 0/16,384`: the sampled image is the constant pass's black target, as the colour path measures |
| per-draw stand-in log lines | **not printed**: no `drew the sampling ...` and no `drew the mapping-control ...` line, and no named refusal line either (the log-gating paragraph explains why) |
| exceptions / validation | 0 exception, validation-error, device-lost or assert lines in the trace; `PALMETTO_VK_VALIDATION` is unset, so this is not a validation-layer claim (section 6 item 5) |
| presenter captures | 8, at presented frames 1, 301, 601, 901, 1201, 1501, 1801, 2101; every one `mean 0.00 0.00 0.00`. The ramp landed on presented frame about 1,002, between the 901 and 1201 captures, so no capture could show it |
| boot shape | 150.0 s, game alive; 4,022 submissions / 2,010 presents by `+140 s`; slot 57 10,241 calls, 10,241 converted, 0 refused; graphics pipelines 51,786 |

**Verdict: the stand-in path is proven with the mapping-control arm.** The layer built its own stretch pipeline over
the resolved colour image, the decision took the drawing branch, and the draw it recorded into the presented target is
read back as the full-rectangle coordinate ramp (127 127 0, peak 255, alpha 255 across the whole 4K plane) while the
constant target stays black. That is the acceptance result section 6 item 2 asked for; no named refusal appeared.

**The one gap is a logging defect, not a draw failure.** The `drew the sampling/mapping-control ...` lines and the
named refusal lines share one `standin_logged` counter with the "no draw: not armed" branch, and that branch increments
it on every submission before the armed one (about 2,005 increments on this boot). The draw lines are gated
`fetch_add(...) < 8` and the refusal lines `< 4`, so both budgets were exhausted long before the armed draw: the two
lines cannot print on any boot whose armed draw is not in its first few submissions. The mapping-control execution is
therefore proven by its readback instead of its own line. The sampling stretch cannot be separated on this boot
because it writes the black colour image and the control ramp is drawn over it; it is proven to the same degree as the
control draw - same plan and prepared-list inputs, same pipeline, descriptor set and emit path, differing only in the
`mode` push dword. The next falsifier for section 6 item 2 is now: give the sampling and mapping-control lines their
own once-per-arm budgets (or print them unconditionally) and repeat this arm; the sampling line should then appear
beside the same ramp readback.

Evidence (outside the repository): `D:\leonida-work\upscale\boot-standin2\boot\` with the arm's `stderr.log`,
`arm_meta.txt`, `monitor.log` and `samples.csv`, and `boot-standin2\capture\` with the 8 PPM captures and `index.txt`;
the arm harness is `D:\leonida-work\upscale\upscale_boot_verify.ps1`, the section 4 harness with this arm's switch set
and the lock taken and released by the caller. No repository code was changed by this measurement; this section is the
only edit.
