# Movie overlay readiness contract

**Status: pure, opt-in readiness contract only.** No runtime caller is wired, no Bink call is synthesized, and no plane or
frame is changed. The current measurements do not reach frame-buffer registration, per-frame decode, or a movie overlay;
this slice makes those prerequisites explicit and testable without claiming that the movie path works.

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

`Evidence{}` defaults to `opt_in == false`; `evaluate` immediately returns `opt-in-off`, even if synthetic event fields
are populated. Opting in only evaluates a supplied evidence record: it performs no I/O, logging, allocation, memory write,
Bink call, or presentation. Since there is no runtime caller, the default executable path and its bytes are unchanged.
This is not a new environment switch and does not enable movie playback.

## Tests

[`movie_overlay_readiness_test.cpp`](../port/test/movie_overlay_readiness_test.cpp) uses only synthetic IDs. It checks the
byte-identical-off contract, each stage's named refusal, ordering and identity mismatches, unsupported plane counts, and a
fully correlated synthetic two-plane chain. It needs no GPU, boot, package, or game data. The standalone test is not
registered in CMake because this slice intentionally does not modify CMake; compile and run it with the VS18 environment
as described in the task report.

## Falsifier and later integration point

The current first refusal with opt-in requested is `frame-registration-not-observed`. A future clean arm falsifies that
readiness result only when it yields a correlated chain: a title-owned registration confirmed by the player's post-call
buffer state, a completed frame on the same handle, the matching plane upload/resource, and a successful two-plane
`PresentX` containing that resource as plane 1. A registration without decode, a decode without a matching upload, or a
one-plane present is not evidence for the overlay route. If the title remains in its measured playing state while
registration stays at zero, this overlay contract is not the current boot blocker; return to the title-side draw/load
falsifiers already recorded in [peer_port_movie.md](peer_port_movie.md#3-what-would-make-the-movie-draw-issue).

If that evidence is ever measured, the exact consumption point is the existing second-plane decision in
`port/shims/xbox_present.cpp`: after the `PresentX` plane list is validated and before its second-plane composition rule
is applied. A later, separately coordinated change would populate the evidence record from the Bink witness's measured
registration/decode observations and the texture-upload path's source-to-resource association. This task does not edit
that shared shim, `main.cpp`, CMake, or any peer clone.
