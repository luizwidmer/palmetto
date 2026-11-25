# Clean-room feature programme: what the peer ports document, and what we implement next

The public tracker at <https://vuyp.github.io/gta6-pc-race/> reads the public repositories of the projects
running the same console build on a PC. This page records the feature claims those projects publish about
themselves, what each means for this runtime, and the clean-room slice we will implement next. It is a
policy page as much as a plan: the peer projects are **evidence**, not a code source.

## Rules of the programme

1. **Documentation only.** The raw material is each project's public README, status page and the tracker's
   own summaries of them. We do not read or copy peer source code for these slices. Where a peer's public
   description names a mechanism, we re-derive the behaviour by measuring our own title and implementing our
   own code against that measurement.
2. **Every claim is re-measured here.** A peer's sentence is a hypothesis until our boot log, probe or
   readback shows what our tree does. The slice records both.
3. **Same gates as every other slice**: opt-in until measured where behaviour can change, tests, one boot,
   honest numbers (a negative is a result), documentation with the peer claim cited and the attribution in
   `NOTICE.md` where a mechanism itself came from a named upstream change.
4. **No game data, no peer code, no extracted assets** enter the repository or this page.

## The peer claims that matter to our next stage

| Peer claim (public wording, summarised) | What it implies for this runtime | Our target slice |
|---|---|---|
| Five Star Recomp: the loader resolves all imports, boots through start-up, **answers its first-run screens through a scripted gamepad**, then starts loading story mode; later, **loading the game's saved settings makes later boots skip the first-run screens** and reach the story load in about 39 s. | Our title currently reads no input and makes no save calls in 300 s. A first-run flow that waits on a button press and on a settings store would look exactly like our post-16 s idle. | A no-SDL input backend with XInput/keyboard-as-pad plus a scripted first-run press schedule; an offline signed-in user and an on-disk settings/save store that survives a kill. |
| VIctory and XCL: runtime services are **one signed-in offline user, licences, packages, saves that survive a kill at any step**, with menus, video and loading sound; input is XInput pads with a keyboard stand-in pad. | Our user shim answers creation but delivers no signed-in event by default and has no persistent store; our GameInput shim is not built when SDL3 is absent, so the canonical build leans on the host. | Same slices as above: user event on demand, persistent local storage, and the input backend that is always built. |
| PercyScarlett: the world was black because it upscales with the console FSR 4 build, whose pipelines have no PC equivalent; **the upscale is now a stretch of the colour image**, so the lit picture reaches the screen. | Our frame's compose pass is exactly that console upscale (it samples the constant pass's 4K sibling). We have FSR stand-in switches; their state and the measured compose output must be settled. | Verify and, if needed, complete the upscale stand-in so the compose pass writes the sampled colour when the console pipelines cannot be made. |
| VIctory fork: while streaming, about 850 title-memory page remaps, 260 texture uploads and 450 disk reads a frame make the card idle about 330 ms of a 490 ms frame; batching page mappings, keeping copies on the card, and in-place title memory are the measured fixes. | Our page resolver and slot-31 mapping exist but are not batched; streamed content will hit the same cost when it starts. | Batch the page-mapping path (one alias/copy per run instead of per page) and measure the cost before/after on our mapping route. |
| XCL and Five Star: the game's own **Bink 2 video**, **XboxBvhBuilder**, and the **AMD FSR 4 console DLL** run from the package on a PC GPU; ray-traced light and reflections are the missing picture feature, with a **constant stand-in** used by several forks. | Our movie library loads but its code never runs; ray tracing is not reached yet. These become relevant once the content pass starts. | Deferred behind the stage/upscale slices; recorded here so the picture programme is not guessed later. |
| Five Star: GPU hangs from **translated shader loops that never end** are bounded. | We landed the loop guard in the translator this week for exactly this class. | Done; keep the falsifier (a guarded loop exits) in the translator page. |

## Slice order (why this order)

1. **Input backend + first-run press schedule** — the cheapest way to test the five-star observation
   ("reads input, starts its save system after a scripted button press") on our own title.
2. **Offline user + persistent settings/saves** — needed for the second half of that observation (later boots
   skip the first-run screens) and for the title's own session/save services.
3. **Upscale stand-in** — turns the compose pass's sampled colour into a presented picture when the console
   upscaler's pipelines cannot be made; this is the last picture step before ray-traced lighting.
4. **Batched page mapping** — the streamed-content performance feature; independent of the stage gate and
   measurable on our mapping route without waiting for content.
5. Deferred: Bink 2 playback route, XboxBvhBuilder, ray-traced light stand-in, audio decoding.

Each slice above gets its own page with the peer sentence quoted as the hypothesis, our measurement, the
implementation, the tests and the honest result. This page stays the ledger.
