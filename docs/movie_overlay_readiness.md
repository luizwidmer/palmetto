# Movie overlay readiness contract

**Status: the existing runtime keeps its conservative readiness consumer; an additive identity/order gate is pure and not wired.** The runtime does not synthesize a Bink call, decode, upload, plane, or plane-role tag. The current measurements do not reach frame-buffer registration, per-frame decode, upload, or a movie overlay. The existing `PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY=1` caller still passes an empty chain to `gate_second_plane`, so an eligible plane-1 candidate is refused at `frame-registration-not-observed`. The new `gate_identified_second_plane` is not called by `xbox_present.cpp`; it does not change the current guard. Unset/empty/`0`, or an invalid switch value, retains the previous plane decision.

## Evidence and scope

The peer mechanism is deliberately narrow. XCL's title-side Bink wrapper leaves the shipped Bink DLL responsible for
frame-buffer sizing and registration; its present path consumes the title's `PresentX` plane list and composes additional
planes. The read-only peer clone was at **`623d02e465a17497a757a4ce867f482b0a6c8f3b`** (clean status when inspected):

- [`bink.rs` at that pin](https://github.com/xcl-dev/xcl/blob/623d02e465a17497a757a4ce867f482b0a6c8f3b/crates/xcl/src/hle/bink.rs)
  describes the opt-in skipper while leaving the real DLL's frame-buffer query/registration path intact.
- [`present.rs` at that pin](https://github.com/xcl-dev/xcl/blob/623d02e465a17497a757a4ce867f482b0a6c8f3b/crates/xcl/src/hle/d3d12x/present.rs)
  records the plane-count-based `PresentX` path and its multi-plane composition.

For the identity question, the clean read-only clones were also verified at Five Star Recomp
[`34b4dfed3524178efeed044514499ef1b5f0cb36`](https://github.com/ThatRecompDev/five-star-recomp/tree/34b4dfed3524178efeed044514499ef1b5f0cb36) and VIctory
[`63bcca43c6a00a758510398fa43096ce7e4f7df0`](https://github.com/Marvisak/VIctory/tree/63bcca43c6a00a758510398fa43096ce7e4f7df0). Five Star's [`PresentPlane.cs`](https://github.com/ThatRecompDev/five-star-recomp/blob/34b4dfed3524178efeed044514499ef1b5f0cb36/src/FiveStar.Loader/Shims/D3D12X/Present/PresentPlane.cs) and [`PlaneReader.cs`](https://github.com/ThatRecompDev/five-star-recomp/blob/34b4dfed3524178efeed044514499ef1b5f0cb36/src/FiveStar.Loader/Shims/D3D12X/Present/PlaneReader.cs) expose per-position token/resource fields, not a movie-versus-UI role. VIctory's [`queue.rs`](https://github.com/Marvisak/VIctory/blob/63bcca43c6a00a758510398fa43096ce7e4f7df0/crates/victory-gpu/src/queue.rs) carries a generic plane list; its [`host.rs`](https://github.com/Marvisak/VIctory/blob/63bcca43c6a00a758510398fa43096ce7e4f7df0/crates/victory-gpu/src/host.rs) composes later planes as overlays, including a scene-plus-UI example. These are behavior evidence only: the plane index/order is not a movie identity.

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

[`movie_overlay_readiness.h`](../port/src/movie_overlay_readiness.h) now exposes two pure policies. The existing `evaluate` and
`gate_second_plane` remain the previous chain-only contract. The additive `gate_identified_second_plane` first confirms the
plane identities/order, then applies the **first named refusal** from the registration/decode/upload/present chain to a movie
plane; a confirmed UI/other overlay is not treated as a movie. Its movie-readiness condition is independently confirmed in order:

1. title-owned frame-buffer registration is observed and confirmed;
2. a decode is confirmed for the same opaque decoder key, producing a nonzero frame key;
3. an upload is confirmed for that decoder/frame and names a nonzero resource key;
4. a successful present of exactly two planes is confirmed after upload; both plane identities belong to that same `PresentX` order;
5. plane 0 is independently identified as the primary plane, and plane 1 is independently identified as movie, UI overlay, or other overlay. Only the movie role is joined to the decoded frame and upload resource: its index is 1 and its resource key equals the upload/present resource.

The `order`, `decoder_key`, `frame_key` and `resource_key` values are opaque observation tokens, not guest pointers, ABI
fields, or frame contents. `PlaneIdentityEvidence` adds opaque role/index/resource tokens; its order must equal the successful
`PresentX` event order for both positions. `confirmed` must be set only after independent evidence establishes the corresponding
stage or role; it must not be inferred from an unmeasured return value or from index 1 alone. The helper does not validate
Bink's buffer ABI, pixel contents, XG layout, plane flags, color space, blend mode, or GPU correctness. More than two planes are
explicitly unsupported by this contract.

`Evidence{}` defaults to `opt_in == false`; `evaluate` immediately returns `opt-in-off`, even if synthetic event fields are populated. `evaluate` and the existing `gate_second_plane` retain their previous chain-only decision semantics. The additive `classify_second_plane` and `gate_identified_second_plane` contract requires confirmed plane 0/index 0/role primary and plane 1/index 1/role movie, UI, or other overlay, both tagged with the same present order. An unknown role, missing identity, wrong index/order, or uncorrelated movie resource is refused by name. With exact opt-in and a confirmed UI/other role, the identity-aware helper passes that non-movie plane without requiring a Bink chain; a confirmed movie role must pass the registration -> decode -> upload -> present chain and plane-1 resource correlation. The helper is pure and performs no I/O, logging, allocation, memory write, Bink call, or presentation. The existing caller opts in only when `PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY=1` and still uses the old `gate_second_plane`, so its current behavior remains conservative for every selected plane-1 candidate until role evidence is actually wired. Unset/empty/`0`, invalid/too-long/unreadable values preserve the old decision (invalid configuration is named once). The option is not movie playback or a decoder.

The switch is read once on the first candidate second plane. The current runtime consumer does not classify plane 1 as movie versus another overlay, so its existing strict policy still requires every otherwise-selected plane-1 candidate to satisfy the movie evidence chain; until the separate identity producer and wiring change land, do not enable it where an independently verified non-movie overlay must remain visible. The new pure helper models the future role-aware distinction but does not alter that live policy or the title's Bink exports, buffers, or upload path.

## Tests

[`movie_overlay_readiness_test.cpp`](../port/test/movie_overlay_readiness_test.cpp) uses only synthetic IDs. Its 11 test cases cover the existing default-off/exact-parser/legacy behavior and readiness-chain refusals, plus confirmed plane roles, same-PresentX order, primary/secondary indices, UI/other passthrough, unknown-role refusal, movie-resource correlation, and unchanged old-gate semantics. VS18's isolated C++20 harness compiled and ran the source: **11 cases, 0 failures** (evidence: `D:\leonida-work\scratch\r21\movie-plane-contract-r21l4-clean`). No shared `port/build` configure/build or CTest, CMake edit, shim rebuild, GPU, or boot was run in this slice. The existing `movie_overlay_readiness_test` / `movie_overlay_readiness` stanza remains registered in lead-owned `port/CMakeLists.txt`.

## Runtime gate falsifier and evidence limit

The current first refusal with exact opt-in requested remains `frame-registration-not-observed`: `xbox_present.cpp` still supplies no registration/decode/upload events to the old gate, which applies to every selected plane-1 candidate because it has no plane-role producer. This is a guard result, not proof that the movie path works or that the title registered no buffers in an unmeasured run. The existing peer-port measurement reports 11/11 sampled `PresentX` calls at one plane and 0 two-plane calls in that window; the movie draw/Bink sections report zero observed registration and decode calls in their measured clean arm.

A future measurement falsifies the movie refusal only with a title-owned registration confirmed from post-call buffer state, decode on that same decoder/frame identity, matching upload/resource, and a successful two-plane `PresentX` whose same-order descriptors prove primary/index 0 and movie/index 1 with that upload resource. A second plane identified as UI/other overlay is not evidence for the movie route; a missing or ambiguous role stays refused by the identity-aware contract. A registration without decode, decode without matching upload, or one-plane present is insufficient. If the title remains in its measured playing state while registration stays at zero, this overlay contract is not the current boot blocker; return to the title-side draw/load falsifiers in [peer_port_movie.md](peer_port_movie.md).

The current consumption point remains the second-plane decision in `port/shims/xbox_present.cpp`, after plane-list validation and the existing rule/flag decision but before `PresentPlanes` is assembled. The unchanged wiring hunk is the `Evidence{}` / `evidence.opt_in = true` / `gate_second_plane(...)` block (current lines 831-835); this slice does not edit it. A future separately reviewed wiring change could replace that empty record and old call with a measured plane-role/order plus registration/decode/upload evidence record and `gate_identified_second_plane(...)`. Until a producer can establish those correlations, do not infer movie identity from index 1 or source/resource pointer equality alone. No Bink ABI, content, or upload behavior is changed.
