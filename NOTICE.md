# Notice

Palmetto is an independent, clean-room interoperability project. It ships **no game content**: no executable, no
archives, no extracted files, no shaders, no textures, no audio, no keys, and no instructions for obtaining any of them.
Everything the tooling reads comes from a copy the user has already acquired lawfully and keeps locally; everything the
build produces from it must stay local and must never be redistributed.

The project does not decrypt content, bypass technical protection measures, or distribute any part of any work.

If you are a rights holder and believe something in this repository is a problem, please contact the repository owner
before filing a formal complaint: the maintainers remove anything that is shown to contain protected content, and they
are not aware of any file in this repository that does, because none is accepted under the project's rules.

## Third-party references

This repository is an independent implementation: prior-art projects are read as evidence of behaviour, not copied as
code. The porting policy is [docs/upstream_port.md](docs/upstream_port.md); each slice records its own credit here.

### ubiquitarianism/leonida - `xapu.dll` / `chad.dll` audio shims (GPL-3.0)

The audio shims in [`port/shims/xapu_chad.cpp`](port/shims/xapu_chad.cpp) and the measurement in
[docs/audio_shims.md](docs/audio_shims.md) were cross-checked against the public GPL-3.0 project
[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida) (GNU General Public License, version 3; see that
project's `LICENSE`). Reference files: `runtime/shims/xapu.cpp`, `runtime/shims/chad.cpp`,
`runtime/shims/xapu_chad.cpp` and `docs/runtime_imports.md`, at these commits:

- `e6515ea41` - Jadiac - "runtime: xapu.dll and chad.dll shims, connections that succeed with an empty result queue"
- `b95401617` - Jadiac - "xapu/chad: check the two connection allocations and return E_OUTOFMEMORY"
- `28f013c2f` - Jadiac - "docs/runtime_imports.md: move our paragraph into the RT-5 section ..."
- `eaeee26b3` - Jadiac - "xapu/chad shims: XDspActivate writes its outputs, open and close commands give results,
  XApuConnect leaves the title's block alone"
- `273b3bdf9` - Moriz82 - "[integration] Linux/Proton and native Windows: boot through the story load"

No code from that project is copied into this repository. Its evidence was used to re-derive and check behaviour that
this repository's own static measurement and A/B boots establish; the upstream project remains the copyright of its
contributors, and its GPL-3.0 license continues to apply to its own tree.

### xma / opusdec decoder packages (LGPL-2.1-or-later / BSD-3-Clause)

The XMA2 and Opus decoding path in [port/xma_decoder](port/xma_decoder) builds two prior-art decoder packages from
their public repositories at a pinned revision (omonkeyz/xbox-emulator, revision 27122f7d): an XMA2 decoder
(LGPL-2.1-or-later, a port of FFmpeg's XMA decoder) and an Opus decoder (BSD-3-Clause, a port of libopus). They are
fetched at build time by Cargo, not vendored here; the runtime loads the resulting DLL only when
PALMETTO_AUDIO_DECODE is set. The packages remain the copyright of their authors under their own licences (see the
xma and opusdec crates' license files in that repository). The boundary code that calls them
(port/shims/audio_decode.h) is our own.

### Compile-worker mechanisms (reference only)

The deferred pipeline-compilation slice ([docs/deferred_compile.md](docs/deferred_compile.md)) was informed by the public
descriptions of the peers' compile strategies: five-star-recomp's separate shader-compile process with a disk cache,
XCL's parallel shader compiler, VIctory's ahead-of-time translation with worker threads, and the reference fork's
pipeline-compile workers (3de22a1904). Those projects are read as evidence of behaviour only; their code is not copied,
and the implementation in this repository is our own.

### Mesh/merged-geometry stage evidence (reference only)

The mesh-stage readiness slice ([docs/mesh_stage.md](docs/mesh_stage.md)) cross-checked the stage shapes and the merged-geometry
residual against the public reference fork (GPL-3.0): files 
untime/shims/d3d12_geometry_views.h,

untime/shims/d3d12_xs_pso_stream.* and the translator work at commits db19bbd34 (Rain156), 273b3bdf9/a626820e
(Moriz82) and 154e4ec03 (Jan Trencansky), plus the peer ports' published stage tables. No code is copied; the stream
builder and the recorded shapes are our own measurements.

### Peer-port first-visible-frame evidence (reference only)

The first-visible-frame research ([docs/first_visible.md](docs/first_visible.md)) read the public peers' status pages,
READMEs and docs as evidence of behaviour: five-star-recomp (GPL-3.0-or-later, clone `0bfa755`), XCL (GPL-2.0-only,
clone `6d12f58`), PercyScarlett (GPL-2.0-only, clone `e44c465`), VIctory (GPL-3.0-or-later, clone `13f91ca`),
victory-fork (GPL-3.0-or-later, clone `9c184b3`), and the public tracker at
<https://vuyp.github.io/gta6-pc-race/>. No code, comment text, identifier table or measurement table from those projects
is copied into this repository; the page quotes short public status sentences as evidence and maps them to this
project's own measurements. Those projects remain the copyright of their contributors under their own licences.

### Root-signature packing evidence (reference only)

The root-signature packing slice ([docs/root_packing.md](docs/root_packing.md)) cross-checked the directly-indexed heap and
spill mechanisms against the public reference fork (GPL-3.0): files in its root-signature, D3D12 COM and root-signature
serializer sources, at the commits the page cites. No code is copied; the planner, the measured shapes and the tests are
our own work.

### Peer-port campaign evidence (reference only)

The peer-port audit pages ([peer_port_streaming.md](docs/peer_port_streaming.md),
[peer_port_movie.md](docs/peer_port_movie.md), [peer_port_first_ui.md](docs/peer_port_first_ui.md)) read five public
ports as evidence for mechanisms this repository does not yet implement: five-star-recomp (GPL-3.0-or-later, commit
`45071d52`), PercyScarlett (GPL-2.0-only, `e44c465`), VIctory (GPL-3.0-or-later, `63bcca4`), victory-fork
(GPL-3.0-or-later, `52ac4ec`) and XCL (GPL-2.0-only, `8b957be`). Their code and documentation are read to understand
behaviour; no source, comment, identifier or data table is copied into this repository, and the mechanisms described
are reimplemented here in our own structure with attribution when a port lands.

### Implemented peer ports (reference only)

The present-driven frame clock ([docs/peer_port_first_ui.md](docs/peer_port_first_ui.md), section 12) was informed by the
peers' present handshakes: VIctory (GPL-3.0-or-later, `63bcca4`) frame/queue code, Five Star Recomp (GPL-3.0-or-later,
`45071d52`) D3D12.X shim notes, XCL (GPL-2.0-only, `8b957be`) present code and PercyScarlett (GPL-2.0-only, `e44c465`)
runtime pacing. The non-blocking pipeline hold ([docs/peer_port_streaming.md](docs/peer_port_streaming.md), section 10)
was informed by Five Star Recomp's D3D12.X shim and debugging notes and by XCL's asynchronously built pipelines. Those
projects are read as evidence of behaviour only; the implementations in this repository are our own, no source, comment,
identifier or data table is copied, and the upstream projects remain the copyright of their contributors under their own
licences.

The plane refresh and plane composition ports ([docs/peer_port_movie.md](docs/peer_port_movie.md), sections 8-9) were
informed by XCL (GPL-2.0-only, `8b957be`) title-texture and mirror code, VIctory and victory-fork (GPL-3.0-or-later,
`63bcca4` / `52ac4ec`) texture re-upload and compositor code, Five Star Recomp (GPL-3.0-or-later, `45071d52`) per-plane
refresh code and PercyScarlett (GPL-2.0-only, `e44c465`) overlay code. The cross-list memory wait and GPU-ordered
bottom-of-pipe ports ([docs/peer_port_streaming.md](docs/peer_port_streaming.md), section 11) were informed by Five Star
Recomp's memory-wait/list-split/write code, XCL's list and CPU-write code and victory-fork's copy-lane queue code. All of
these are read as evidence of behaviour only; the implementations in this repository are our own, no source, comment,
identifier or data table is copied, and the upstream projects remain the copyright of their contributors under their own
licences.

The present attestation and hang-watchdog pause hold ([docs/peer_port_first_ui.md](docs/peer_port_first_ui.md), section 13)
were informed by PercyScarlett (GPL-2.0-only, `e44c465`), VIctory (GPL-3.0-or-later, `63bcca4`) and Five Star Recomp
(GPL-3.0-or-later, `ce2b333b`) pause/poke notes. The terminal read outcome port
([docs/peer_port_streaming.md](docs/peer_port_streaming.md), section 12) was informed by Five Star Recomp (`ce2b333b`),
PercyScarlett (`e44c465`), VIctory (`63bcca4`), victory-fork (`9a9091d`) and XCL (`623d02e`) loader/storage paths, and
cross-checked against the reference fork (`ubiquitarianism/leonida`, GPL-3.0, `779f901084`) first-touch handling. The
GPU-view reservation pairing ([docs/peer_port_movie.md](docs/peer_port_movie.md), section 11) was informed by XCL
(`623d02e') mirror code and Five Star Recomp (`ce2b333b`) reservation pairing. All are read as evidence of behaviour only;
the implementations are our own, no source, comment, identifier or data table is copied, and the upstream projects remain
the copyright of their contributors under their own licences.

The row-aware XG movie-plane upload ([docs/peer_port_movie.md](docs/peer_port_movie.md), section 12) was informed by XCL
(GPL-2.0-only, `623d02e`) XG code and Five Star Recomp (GPL-3.0-or-later, `ce2b333b`) XG writers, with the movie-plane
shape measurement cross-checked against the reference fork (`ubiquitarianism/leonida`, GPL-3.0, `273b3bdf9`). The
physical-page completion delivery ([docs/peer_port_streaming.md](docs/peer_port_streaming.md), section 13) was informed by
Five Star Recomp (`ce2b333b`) page-sink code, PercyScarlett (`e44c465`), VIctory (`63bcca4`), victory-fork (`9a9091d`) and
XCL (`623d02e`) storage paths and the reference fork's page resolver. The measured display answers and UI page probe
([docs/peer_port_first_ui.md](docs/peer_port_first_ui.md), section 14) were informed by PercyScarlett (`e44c465`), VIctory
(`63bcca4`), Five Star Recomp (`ce2b333b`) and XCL (`623d02e`) display/deferral handling. All are read as evidence of
behaviour only; the implementations are our own, no source, comment, identifier or data table is copied, and the upstream
projects remain the copyright of their contributors under their own licences.

The presenter attestation, fit rule and synthetic non-black control ([docs/peer_port_first_ui.md](docs/peer_port_first_ui.md),
section 15) were informed by Five Star Recomp (GPL-3.0-or-later, `ce2b333b`) aspect/colour-space present code, PercyScarlett
(GPL-2.0-only, `e44c465`) overlay present, VIctory (GPL-3.0-or-later, `63bcca4`) compositor and XCL (GPL-2.0-only,
`623d02e`) present window handling. The movie draw-path probes ([docs/peer_port_movie.md](docs/peer_port_movie.md), section
13) were informed by XCL (`623d02e`), Five Star Recomp (`ce2b333b`) and VIctory (`63bcca4`) movie draw/middleware notes. All
are read as evidence of behaviour only; the implementations are our own, no source, comment, identifier or data table is
copied, and the upstream projects remain the copyright of their contributors under their own licences.

The alias-aware title-memory watch and delivery fanout ([docs/title_memory_model.md](docs/title_memory_model.md), section
10) were cross-checked against public mechanism descriptions in XCL (GPL-2.0-only, `623d02e`), victory-fork
(GPL-3.0-or-later, `9a9091d`), Five Star Recomp (GPL-3.0-or-later, `ce2b333b`) and VIctory (GPL-3.0-or-later, `63bcca4`).
The submission-overlap bounds ([docs/peer_port_streaming.md](docs/peer_port_streaming.md), section 15) were cross-checked
against VIctory (`63bcca4`), victory-fork (`9a9091d`), XCL (`623d02e`) and PercyScarlett (GPL-2.0-only, `e44c465`). The
mesh publication ([docs/mesh_stage.md](docs/mesh_stage.md), section 8) was cross-checked against XCL (`623d02e`) and Five
Star Recomp (`ce2b333b`) mesh/geometry descriptions. The DPI window fit ([docs/peer_port_first_ui.md](docs/peer_port_first_ui.md),
section 16) was cross-checked against Five Star Recomp (`ce2b333b`), XCL (`623d02e`), VIctory (`63bcca4`), PercyScarlett
(`e44c465`) and victory-fork (`9a9091d`). The BVH route plan ([docs/bvh_route.md](docs/bvh_route.md)) was cross-checked
against XCL (`623d02e`) and Five Star Recomp (`ce2b333b`). The kind-7 upscaler readiness plan
([docs/fsr_route.md](docs/fsr_route.md), section 9) was cross-checked against XCL (`623d02e`) and PercyScarlett
(`e44c465`). All are read as evidence of behaviour only; the implementations are our own, no source, comment, identifier or
data table is copied, and the upstream projects remain the copyright of their contributors under their own licences.

### Implemented peer ports (second batch)

The measured console slot-70 per-frame matrix publication and the existing fence-slot-12 completed-value cell
([docs/device_slot70.md](docs/device_slot70.md), [docs/fence_slot12.md](docs/fence_slot12.md)) were cross-checked against
public descriptions in Five Star Recomp (GPL-3.0-or-later, `34b4dfed`, ThatRecompDev), XCL (GPL-2.0-only, `623d02e`,
Prime1997) and VIctory (GPL-3.0-or-later, `63bcca4`, Marvis). Five Star's
`src/FiveStar.Loader/Shims/D3D12X/XboxAddressDescriptors.cs`,
`src/FiveStar.Loader/Shims/D3D12X/Resources/AddressShaderViews.cs` and
`src/FiveStar.Loader/Shims/D3D12X/FenceValueMirror.cs` describe its generic address-view and completed-value mirror paths;
its slot-70 record is not the
per-frame matrix shape measured here. XCL's `crates/xcl/src/hle/d3d12x/fence_x.rs` and
`crates/xcl/tests/d3d12_resource_and_sync.rs` explicitly describe and test a fence-slot-12 completed-value cell, with an
event/thread mirror unlike this tree's watcher-backed proxy cell. VIctory's `crates/victory-gpu/src/fence.rs` was reviewed
as a contrasting atomic fence-value model; it does not establish this title's slot numbering. The target call shape,
record validation and encoding remain derived from this repository's own measurements. No source, comments, identifiers or
tables from these projects were copied; each upstream remains the copyright of its contributors under its own licence.
