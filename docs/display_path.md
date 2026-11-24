# The display path: the title presents a null surface of its own

**Status: diagnosed, nothing wired.** The frame's `PresentX` names no display resource because the title itself writes
a **null** resource pointer into its own present state on every present, out of its own output-surface record. It is
not a shim answer on the present path, and it is not a first-frame artefact: the write is measured 23,819 times in one
120 s boot, always from the same instruction, always with the value 0 (section 2). `XboxPresentation::PresentX` refuses
a plane whose first resource is null, so the prepared presenter has nothing to present until that record carries a
resource; the exact next measurement is in section 3.

## 1. What the present carries, and where the plane comes from

One 140 s baseline boot (retail image, Vulkan backend, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, no
diagnostics): **ALIVE 140.1 s, no exception**, 1,163,507 traced calls, **72,155 `ExecuteCommandLists` S_OK**, **0 failed
`Close`**, **36,084 `PresentX`** calls (queue slot 32), 7,267 device slot-57 calls.

The sampled plane is unchanged from the previous measurement: at calls 1, 2, 1,000 and 10,000 the token grows
(0x1, 0x2, 0x7cd, 0x4e1d) while `ResourceCount` is 1, `ppResources` is `image+0x75E0DE0`, the first resource there is
**0**, colour space 0, scale filter 0, flags 0.

The frame function `game+0x2481E90` builds the plane on its own stack (`game+0x2482302` clears the plane block,
`game+0x2482313` fills it) and calls `PresentX` at `game+0x24824C1`. The source of every field is a **static present
state** at `image+0x75E0DD8`:

| offset | field | filled by |
|---|---|---|
| `+0x00` | present type (measured 2 = present the planes) | the present-to-display path below |
| `+0x08` | plane 0 entry, stride 0x18: `+0x00` resource, `+0x08` colour space, `+0x0C`/`+0x10` two words from the surface | same |
| `+0x38` | plane count | same |
| `+0x40`.. | mode words (one is `0` when the mode descriptor's first byte is 2) | same |

The frame function takes the plane pointer from its per-frame record (`[record+0x38]`) and copies the entry onto the
stack; `ppResources` is `&entry[0]` = `image+0x75E0DE0`.

## 2. Who writes `image+0x75E0DE0`, and what it writes

One 120 s boot with `PALMETTO_WATCH_RVA_WRITE=75E0DE0` (the mapped image's page holding the cell made read-only; every
store logs the store instruction and registers, single-steps, re-protects): **ALIVE 120.1 s, no exception**, 924,720
traced calls. The cell was written **23,819 times in 120 s, every write from `game+0xCD74DB` (`mov [r10-0x10], rsi`),
every value `0x0`**. The page also carries the frame token and other hot counters, so the watch trapped 1,855,056
stores in total; the cell's own writes are the 23,819.

The writing function is the title's present-to-display path. `game+0xCD7320` fills the static present state
(`byte [image+0x75E0DD8] = 2`, the plane entries, the count, the mode words) from an array of title objects:

```
plane entry resource = [object + 0x20]        ; the surface's resource field
plane entry colour   = [colourArray + i*4]
plane count          = arg
```

and is reached through `game+0xCD72B0` (wrapper) from `game+0x24846D0` (present-to-display) from the job entry
`game+0xC9E5E0`. So the title is not writing a translated handle and it is not failing to write: **it writes its own
surface's resource field, and that field is null.**

A `PALMETTO_POKE_LOG` probe on the load before the store (`game+0xCD74D7`) dumped that surface object:

```
+0x00 class descriptor image+0x61ECDB0
+0x08 packed (0xA80004007F8000 measured here)
+0x10 id word (0x10981)
+0x18 0
+0x20 0            <- the resource pointer that reaches PresentX
+0x28 self+0x50
+0x38..0x48 packed mode/format words
+0x50 class descriptor image+0x61ECDA0 (the embedded GPU-resource child)
+0x58 self
```

## 3. The same null in the render path, and what is measured about the surface's creation

The present is not the only place this field is read. `CreateRenderTargetView` (device slot 20) is called from
`game+0xCC29E4` with `pResource = [surface + 0x20]` (the surface is `[record + 8]` of a render-target record) and that
is 0: **the null view the frame clears and the null present resource are the same field of the same class of surface
record** (the class descriptor is `image+0x61ECDB0` for both; the record object is class `image+0x61ECDC0` with its RTV
handle at `+0x20`). A poke at `game+0xCC29D8` dumped 16 surfaces created through this path; every one had `+0x20 = 0`.

The surface wrapper's constructor is `game+0x325090` (class stamped at `game+0x325072`). It calls `game+0x193DE10`,
which reaches **console device slot 133 (`CreatePlacedResourceX`) at `game+0x193E2D4`** with
`riid = IID_ID3D12Resource` (`game+0x63FA690`) and **out = surface+0x20** (`game+0x193E286`). The proxy's slot-133
handler takes `riid`/`out` from the fifth/sixth argument words, so the mapping matches the trace's `ok`.

**Poking that call (`PALMETTO_POKE_LOG=193E2DB`, the instruction after it) shows the path working:** every hit has
`EAX = 0` (`S_OK`) and the surface's `+0x20` holding a real new resource pointer (measured on the first hit, surface
`0x1E62BDFC730`, `+0x20 = 0x1E82AA74AE0`). The run made 30 such creations before it stopped, so **the slot-133 path
creates and stores surface resources**; the surfaces that reach the present and the render-target views are simply not
the ones created through it, or not yet.

**Exact next measurement, one 120 s boot:** `PALMETTO_POKE_LOG=325090` on the same image. The constructor entry fires
for every surface; its stack dump names the creating return address for each, which separates the resource-carrying
population (created through `game+0x193DE10`) from the null one that reaches the present and the render targets. Once
the null surface's creator is named, poke that creator's resource call the same way the slot-133 call was poked above.
(Do not run this while another boot is running: the boot that produced the slot-133 result below overlapped another
palmetto.exe and stopped at `game+0x3A32` reading `0x8`; the slot-133 observation itself is sound because it is read at
the instruction, but its boot numbers are not comparable.)

## 4. What the title calls around display (measured, and what it is not)

* **No DXGI display path**: the boot calls `XboxIDXGIDevice::GetAdapter` three times and nothing else from DXGI; no
  `EnumOutputs`, no `CreateSwapChain*`, no output description.
* **Frame events** are the only pacing calls: device slot 78 `SetFrameIntervalX(output 0, period 0x8235 µs, latency 3)`,
  slot 79 `ScheduleFrameEventX` for events 2 and 0, slot 80 `WaitFrameEventX` for event 2 with an INFINITE timeout and
  for event 0. They are answered from the device proxy's token counter (no vblank source before a presenter exists).
* **Present is the only display-surface call**: queue slot 32 `PresentX(1, planes, params)` once per frame.
* **The stubbed device extensions are not display**: slots 81 (`GetFrameStatisticsX`), 86 and 127 are beyond the PC
  device vtable and answered `S_OK / 0`. Slot 86 is called from `game+0x247A87` with `riid = IID_ID3D12PipelineState`
  (`image+0x61E06B8`) and a 0x2B0-byte descriptor: it is a pipeline-state creation, not a display/swapchain creator.

So there is no console display API whose answer could supply the resource: the missing piece is the title's own
surface record, and the only shim call on that path is the slot-133 resource creation in section 3.

### Side observation for the draw/pipeline frontier (not this page, but measured here)

Console device slot **55** is called from `game+0x247A5C2` with `riid = IID_ID3D12PipelineState` and `out` on the
stack, returning the new pipeline object. The proxy forwards console slot 55 unchanged to **PC slot 55, which is
`ID3D12Device4::CreateReservedResource1`**; the backend refuses the non-null "session" (the riid pointer) and the call
returns `E_INVALIDARG`, which the title does not test (it returns the out pointer unchecked). In the 120 s boots that
post-date the layout work this is **41,545 failures in 120 s** — every pipeline state created through that path is
null. This has the shape of the earlier mis-forwarded console extensions (slots 70 and 61) and is a candidate for the
"no `SetPipelineState`, no draw" frontier, not for the display resource.

## 5. Why the presenter is still not wired

`XboxPresentation::PresentX` refuses a plane whose `ppResources[0]` is null with `E_INVALIDARG` before it touches the
presenter (`port/shims/xbox_present.cpp`), by design: presenting a null resource would show a stale frame. The
prepared core (window presenter over the Vulkan swapchain, frame pacer, token bridge for the device proxy's frame-event
counter) has nothing to present while the title's surface carries no resource, and wiring it now would change no boot
number (the present already fails one call earlier). The honest order is section 3 first, then the registration and the
token bridge.

**No source file was changed for this page.** The tree is being edited for the replay/submit work in
`port/shims/d3d12_vulkan_device.cpp` and `port/src/vulkan_backend.h`; a build of that state would measure that
hunk, not this diagnosis, so no gate was run here.

## 6. Boots behind this page

| run | instrumentation | result |
|---|---|---|
| baseline, 140 s | none | ALIVE 140.1 s, no exception, 1,163,507 traced calls, 72,155 submits S_OK, 0 failed `Close`, 36,084 presents |
| writer of the plane resource, 120 s | `PALMETTO_WATCH_RVA_WRITE=75E0DE0` | ALIVE 120.1 s, no exception, 23,819 cell writes, all from `game+0xCD74DB`, all `0` |
| surface object, 120 s | `PALMETTO_POKE_LOG=CD74D7,24846D0` | ALIVE 120.1 s, no exception; surface dump in section 2; caller job `game+0xC9E5E0` |
| render-target surfaces, 120 s | `PALMETTO_POKE_LOG=CC29D8,248472A` | ALIVE 120.1 s, no exception; 16 surfaces all `+0x20 = 0`; `pResource` 0 at `game+0xCC29E4` |
| slot-133 out write, 120 s | `PALMETTO_POKE_LOG=193E2DB` | stopped at 52.6 s at `game+0x3A32` (read at `0x8`) with another boot running; 30 slot-133 creations, all `S_OK`, surface `+0x20` non-null |

The diagnostic runs trap a hot image page or a hot instruction, so their submit/present counts are lower than the
baseline's and their command-list/submit error counts are not comparable to it; they are used only for the named
findings above.

## 7. Limits

* One machine, one copy of 1.0.2.1, one Vulkan backend build, one measured boot per configuration.
* The class descriptors and the surface object's semantics are inferred from code shape and from these dumps, not from
  symbols; the object is called a "surface record" because of where it is read, not because a symbol says so.
* The slot-133 argument mapping is from the proxy's own dispatch and the title's call site, not from a console header;
  section 3 is the measurement that confirms it.
* The slot-55 observation is measured on the same boots and is a lead for the pipeline frontier; it is not needed for
  the display diagnosis and has not been fixed here.
