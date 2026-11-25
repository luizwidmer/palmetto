# XMA2 and Opus decode boundary (C ABI)

This crate exposes a C ABI around two separately licensed Rust packages, pinned in Cargo.toml:

- `xma` (LGPL-2.1-or-later) decodes one mono or stereo XMA2 stream from a non-empty buffer of
  complete 2048-byte packets into interleaved signed 16-bit PCM.
- `opusdec` (BSD-3-Clause) decodes one Opus packet into interleaved 32-bit float PCM, for a mono or
  stereo stream at an Opus decoder rate (8, 12, 16, 24 or 48 kHz).

The crate is the decode boundary only. It does not mix, resample, queue or output anything, and it
carries no game data. The PC runtime loads the cdylib as a plugin from
`port/shims/audio_decode.h` (behind `PALMETTO_AUDIO_DECODE=1`); the staticlib is for the C ABI smoke
test and a future link-time use.

Each decoder handle serializes decode calls. A returned PCM block owns its samples and stays valid
until its `_pcm_destroy`, even after the decoder is destroyed. Do not destroy a decoder concurrently
with a decode call or destroy a handle more than once. `palmetto_audio_decoder_abi_version()` names
the boundary version the runtime loader checks.

The tests build packets in memory; they do not open or contain audio fixtures. The synthetic C++
smoke test verifies the exported ABI and ownership boundary.

Run:

```powershell
cargo fmt --manifest-path port/xma_decoder/Cargo.toml -- --check
cargo test --manifest-path port/xma_decoder/Cargo.toml --locked
cargo build --manifest-path port/xma_decoder/Cargo.toml --locked
```

The C++ smoke test is run by the xma2-decoder workflow with MSVC. It links ws2_32, userenv, and
ntdll for Rust's Windows standard library. The workflow also produces
`palmetto_xma2_decoder.dll` (the `cdylib` crate type), which the runtime can load; placing that DLL
next to `palmetto.exe` is currently a manual copy, because the C++ build has no target for it yet.

Attribution for both dependencies is in each package's own `NOTICE`/`COPYING` in the public
`omonkeyz/xbox-emulator` repository and in `docs/audio_decode.md`. This crate's wrapper is
GPL-3.0-or-later.
