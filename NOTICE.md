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
