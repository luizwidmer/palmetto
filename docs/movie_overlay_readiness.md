# Movie overlay readiness contract

**Status: pure readiness contract plus a conservative opt-in consumer at the second-plane decision.** The runtime does not synthesize a Bink call, decode, upload, or plane. The current measurements do not reach frame-buffer registration, per-frame decode, or a movie overlay. `PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY=1` makes a would-be plane-1 candidate conditional on the complete evidence chain; because no registration/decode/upload producer is wired, an eligible candidate is refused at `frame-registration-not-observed`. Unset/empty/`0` preserves the previous plane decision.

## Evidence and scope

The peer mechanism is deliberately narrow. XCL's title-side Bink wrapper leaves the shipped Bink DLL responsible for
frame-buffer sizing and registration; its present path consumes the title's `PresentX` plane list and composes additional
planes. The read-only peer clone was at **`623d02e465a17497a757a4ce867f482b0a6c8f3b`** (clean status when inspected):

- [`bink.rs` at that pin](https://github.com/xcl-dev/xcl/blob/623d02e465a17497a757a4ce867f482b0a6c8f3b/crates/xcl/src/hle/bink.rs)
  describes the opt-in skipper while leaving the real DLL's frame-buffer query/registration path intact.
- [`present.rs` at that pin](https://github.com/xcl-dev/xcl/blob/623d02e465a17497a757a4ce867f482b0a6c8f3b/crates/xcl/src/hle/d3d12x/present.rs)
  records the plane-count-based `PresentX` path and its multi-plane composition.

This is a fresh clone pin; the earlier audit table in [peer_port_movie.md](peer_port_movie.md#1-what-was-read-and-with-what)
records XCL at `8b957be`. The code here is independently written: it is a finite evidence classifier, not a copied wrapper,
decoder, upload path, compositor, peer identifier, or peer data structure.

Our measured frontier is different from the peer's. The current Bink witness section of
[peer_port_movie.md](peer_port_movie.md#14-implemented-the-bink-import-level-witness-and-the-non-looping-movie-fast-advance-section-4-item-1)
records a playing-state handle but no frame-buffer registration and no decode in the clean arm. The movie-plane upload
section records that no movie plane was created or uploaded in the measured window; the title's sampled `PresentX`
population remains one plane. These are the conditions behind the readiness-only decision, not a reason to add a fake
Bink implementation.

## Contract

[`movie_overlay_readiness.h`](../port/src/movie_overlay_readiness.h) is a header-only, bounded pure classifier. It accepts
one caller-supplied set of evidence tokens and returns the **first named refusal** unless the following chain is
independently confirmed in strict order:

1. title-owned frame-buffer registration is observed and confirmed;
2. a decode is confirmed for the same opaque decoder key, producing a nonzero frame key;
3. an upload is confirmed for that decoder/frame and names a nonzero resource key;
4. a successful present of exactly two planes is confirmed, with the movie on plane 1 and the same frame/resource keys.

The `order`, `decoder_key`, `frame_key` and `resource_key` values are opaque observation tokens, not guest pointers, ABI
fields, or frame contents. `confirmed` must be set only after independent evidence establishes the corresponding stage;
it must not be inferred from an unmeasured return value. The helper does not validate Bink's buffer ABI, pixel contents,
XG layout, plane flags, color space, blend mode, or GPU correctness. More than two planes are explicitly unsupported by
this contract.

`Evidence{}` defaults to `opt_in == false`; `evaluate` immediately returns `opt-in-off`, even if synthetic event fields are populated. The helper itself is pure and performs no I/O, logging, allocation, memory write, Bink call, or presentation. The caller opts in only when `PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY=1`, and currently passes an otherwise-empty record because no measured producer supplies registration/decode/upload evidence. At the second-plane decision, an opted-in non-ready chain refuses plane 1 by its first named reason; it does not alter plane 0. With the switch unset/empty/`0`, or with an invalid/too-long/unreadable value, the old gate is retained (invalid configuration is named once). The option is a conservative visibility gate, not movie playback or a decoder.

The switch is read once on the first candidate second plane. The current `PresentX` path does not classify plane 1 as movie versus another overlay, so enabling this strict option requires every otherwise-selected plane-1 candidate to satisfy the movie evidence chain; do not enable it where an independently verified non-movie overlay must remain visible. It never changes the title's Bink exports, buffers, or upload path.

## Tests

[`movie_overlay_readiness_test.cpp`](../port/test/movie_overlay_readiness_test.cpp) uses only synthetic IDs. It checks the default-off contract, exact switch parsing, the legacy passthrough, each stage's named refusal, ordering and identity mismatches, unsupported plane counts, an opted-in incomplete-chain refusal, and a fully correlated synthetic two-plane chain that the gate permits. It needs no GPU, boot, package, or game data. The existing `movie_overlay_readiness_test` target and `movie_overlay_readiness` CTest are already registered in `port/CMakeLists.txt`; this slice does not modify that lead-owned file. VS18 build succeeded and `movie_overlay_readiness` passed 1/1. `xbox_present_test.exe` was built only to compile/link the changed shim, not run because the test needs a Vulkan GPU/window; no GPU or boot was run.

## Runtime gate falsifier and evidence limit

The current first refusal with opt-in requested is `frame-registration-not-observed`. If the existing plane rule selects a plane-1 candidate, the runtime consumer in `port/shims/xbox_present.cpp` currently supplies no Bink or texture events, so the strict gate is expected to refuse that candidate at this name; that is an explicit negative/guard result, not proof that the movie path works or that the title registered no buffers in an unmeasured run.

A future clean arm falsifies that readiness result only when it yields a correlated chain: a title-owned registration confirmed by the player's post-call buffer state, a completed frame on the same handle, the matching plane upload/resource, and a successful two-plane `PresentX` containing that resource as plane 1. A registration without decode, a decode without a matching upload, or a one-plane present is not evidence for the overlay route. If the title remains in its measured playing state while registration stays at zero, this overlay contract is not the current boot blocker; return to the title-side draw/load falsifiers already recorded in [peer_port_movie.md](peer_port_movie.md#3-comparison-with-our-chain).

The consumption point is the second-plane decision in `port/shims/xbox_present.cpp`, after plane-list validation and the existing rule/flag decision but before `PresentPlanes` is assembled. `PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY=1` refuses an otherwise-composed plane unless its supplied evidence record is ready. This task intentionally does not populate registration/decode/upload from the Bink witness or infer source-to-resource association; a future measurement plus a separately reviewed evidence producer is required before this guard can allow a movie plane. No Bink ABI, content, or upload behavior is changed.
