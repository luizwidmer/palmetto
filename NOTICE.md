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

