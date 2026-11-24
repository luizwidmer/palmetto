# The console command-list lifecycle: why the frame's recording is dropped, and what was emulated

**Status: measured on one machine, 2026-11-24**, against the retail 1.0.2.1 image with the shader corpus and the
Vulkan backend. Nothing here quotes image bytes, package paths, shader bytes or extracted tables: call counts, close
codes, RVAs, register values and the project's own wording only.

Predecessor: [render_loop.md](render_loop.md) (why the frame loop records into closed lists and never submits a draw;
its reopen experiment and its `PALMETTO_XS_LIST_MUTE` bisection are the basis of this page). The present change turns
the render-loop investigation's environment-gated reopen probe (`PALMETTO_XS_LIST_REOPEN`) into the proxy's default,
honest lifecycle emulation, gives the list proxy the state it lacked, and covers it in the GPU-free mock test.
`render_loop.md` section 2 still lists the old switch: it no longer exists. Section 7's "muting slot 146 removes the
`E_INVALIDARG`" row needs the correction in section 5 below (slot 52 is a second invalid recording call).

## 1. The state the proxy lacked

`port/shims/d3d12_xs_device.cpp`'s `ListProxy` wrapped the real PC command list (device slot 73
CreateCommandListX -> PC CreateCommandList) and forwarded the console slots the title reaches. It had no lifecycle
state, so every forwarded call went straight to the real PC list:

* a PC command list is created **recording**; its first successful `Close` ends the recording;
* after that the PC layer records nothing more (the list's `CommandList` is closed: the appending calls return early
  or fail) and `Close` returns `E_FAIL` because `list_.close()` finds the list already closed
  (`port/shims/d3d12_com.cpp`, `GraphicsCommandList::Close`);
* the title, measured, creates and closes its lists during setup and then records a frame into them every frame
  without ever calling a list `Reset` (render_loop.md sections 1 and 4). That is legal on the console, whose list
  recording memory belongs to the command allocator; on the PC runtime it is not.

The proxy therefore needed three things it did not keep:

1. the real PC command list (already stored: `real`);
2. **the real allocator the list was created against**, resolved once at creation (`wrap_list` gets it after
   `allocator_proxy_real_allocator`; the proxy holds a reference while it lives);
3. **whether the real list is currently recording**.

## 2. What is emulated, and how

In `list_slot<Slot>` (`d3d12_xs_device.cpp`), after the console slot is mapped to its PC slot and the PC slot is
bounds-checked, and **before** the call is forwarded:

* a forwarded slot whose PC slot is **11 or later** (PC `ClearState` onward: the calls that append to a recording) is
  a recording call. Slots 0-10 (the IUnknown/Object/DeviceChild chain, `GetType` 8, `Close` 9, `Reset` 10) are not:
  they never reopen anything;
* if the proxy's state says the real list is not recording (its last `Close` succeeded), the real list is reset
  against its creating allocator with the PC signature `HRESULT Reset(ID3D12CommandAllocator*, ID3D12PipelineState*)`
  (vtable slot 10), pipeline state null for now, and the call is forwarded only after `S_OK`;
* after `S_OK` the state is "recording". The first four reopens are logged with list/allocator/console slot, then the
  emulation is silent: one reset per frame is normal;
* if `Reset` fails, the failure is logged **once** with the HRESULT and the console slot that needed it, and that
  call is **refused** (its HRESULT is returned) rather than forwarded into a list the PC runtime would drop it from;
* a **successful** `Close` marks the list closed, so the next recording call reopens it. A failed `Close` is not
  taken as closed (the task's rule; measured consequence in section 4).

There is no environment switch: this is the correct emulation of the console's allocator-owned recording memory, not
a measurement aid. `PALMETTO_XS_LIST_MUTE`/`PALMETTO_XS_LIST_DUMP`/`PALMETTO_XS_LIST_ID` remain the render-loop
investigation's measurement switches and are unchanged.

## 3. Mock coverage (`port/test/xs_proxy_mock_test.cpp`)

The mock's simulated real list now has the lifecycle the proxy depends on:

* `list_closed` (created recording, `Close` at PC slot 9 leaves it closed, `Reset` at PC slot 10 reopens it),
  `reset_result` (default `S_OK`; `E_FAIL` injects the reopen failure), and `list_events[]`, the order of the
  command-list slots the mock was called in.

`check_console_command_list` section 3b asserts, with console slot 85 (CopyTextureRegionX -> PC slot 16) as the
recording call:

| assertion | proves |
|---|---|
| one `Close`, then a recording call: exactly one `Reset` **then** the recording call (`list_events` order 10, 16), `Reset` gets the **real allocator**, not the proxy | the closed-list reopen happens before forwarding |
| a second `Close` and recording call: a second `Reset` | the per-frame case |
| `Close`, then PC slot 8 `GetType`: no `Reset`, call forwarded, list stays closed | only recording slots reopen |
| `Reset` returns `E_FAIL`: the call returns `E_FAIL`, nothing is forwarded, the list stays closed | honest failure path |
| a later recording call after a failed reopen, `Reset` `S_OK` again: reopened and forwarded | recovery |

`xs_proxy_mock_test` passes (`PASS (9 allocations refused on purpose)`); `crash_unwind_test` (54 checks),
`graphics_record_test` (101) and `unimplemented_policy_test` (98) pass; `d3d12_fence_timing_test` remains the known
pre-existing failure (0xC0000005). Gates: `build_palmetto.cmd` -> `PALMETTO_OK`, `build_tests.cmd` -> `TESTS_BUILT`.

## 4. Measured boot outcome

Boot environment as before: retail image, shader corpus, Vulkan backend, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`; one boot at a time, 150 s timeout, a boot runs to the timeout. The "after" run used
the existing `PALMETTO_XS_LIST_ID=1` measurement switch to log Close codes (it changes no call and no control flow).

| measurement (150 s) | before (lifecycle not emulated) | after (this change) |
|---|---:|---:|
| verdict | ALIVE, no exception | ALIVE, no exception |
| traced D3D12 calls | 2,364,674 | 2,408,262 |
| `Close` S_OK | 625 (startup only) | 625 (startup only) |
| `Close` failed | 162,444 | 165,471 |
| Close codes (first 4,000 logged) | not logged | 625 `S_OK`, 3 `E_INVALIDARG` (0x80070057), 3,372 `E_FAIL` (0x80004005) |
| list `Reset` (reopens) | 0 | 4 |
| `ExecuteCommandLists` ok | 81,222 | 82,736 |
| console `PresentX` (queue slot 32) | 81,222 | 82,736 |
| `SetPipelineState` | 0 | 0 |
| Draw / DrawIndexed | 0 | 0 |

What the after run shows:

* the reopen works: **four** `ID3D12GraphicsCommandList::Reset` forwards (the four `reopened list ...` log lines, all
  triggered by console slot 32 SetDescriptorHeaps), all succeeding, no reopen failure;
* the recording is **real**: the first `Close` of three of those four lists returned `E_INVALIDARG` instead of the
  old `E_FAIL`. Only an open PC list can fail that way (`d3d12_com.cpp`: `Close` returns `E_INVALIDARG` from the
  list's invalid flag, `E_FAIL` when `list_.close()` finds it already closed), so the frame's recorded calls reached
  the PC layer's validation. The fourth list was first reopened after the 4,000-line Close-code log had filled, so
  its code is not in the table;
* the frame still does not survive: the `E_INVALIDARG` poisons each list, the proxy only marks a list closed on a
  **successful** `Close`, so those lists are not reopened again and their later closes return `E_FAIL` (165,471
  failures against 625 startup successes). Nothing was drawn (`SetPipelineState` and draws still 0), so the first
  frame remains a clear-and-present frame; the submits did not drop (82,736 vs 81,222 in the same 150 s).

## 5. Bisection: the two invalid recording calls that remain

`PALMETTO_XS_LIST_MUTE` answers a console list slot with 0 instead of forwarding it (a measurement window, not a fix);
`PALMETTO_XS_LIST_ID=1` adds the Close codes (both change no call and no control flow).

| run | boot | `Close` S_OK / failed | list reopens | frames submitted | logged Close codes |
|---|---|---:|---:|---:|---|
| no mute | 150 s | 625 / 165,471 | 4 | 82,736 | 625 `S_OK`, 3 `E_INVALIDARG`, 3,372 `E_FAIL` |
| `MUTE=146` | 150 s | 624 / 174,850 | 6 | 87,439 | not logged |
| `MUTE=146` | 60 s | 625 / 56,492 | 5 | 28,248 | 626 `S_OK`, 3 `E_INVALIDARG`, 3,371 `E_FAIL` |
| `MUTE=146,52` | 60 s | 57,538 / **0** | 56,915 | 56,914 | 4,000 `S_OK` of 4,000 |

The `MUTE=146` rows are the new result: **muting the barrier at list slot 146 is not enough.** The first `Close`
after a reopen still returns `E_INVALIDARG` (three of them in the logged window), so another forwarded recording call
marks the PC list invalid. That call is **console slot 52 -> PC slot 48, ClearRenderTargetView**: muting 146 alone
still fails, muting 146 and 52 together makes every `Close` `S_OK` (4,000 of 4,000 logged codes), and nothing else
differs between those two boots - so 52 is the second, and only other, invalid call. The trace agrees: in the first
frame after the reopen it shows the `ClearRenderTargetView` call forwarded between the reopen and the failing
`Close`. Console slot 32 (SetDescriptorHeaps) is forwarded in the same frame and is valid (render_loop.md section 7's
"only slot 32 forwarded -> 4,000/4,000 `S_OK`"). The bottom row also shows the full lifecycle working once both
invalid calls are muted: one reopen per frame and every `Close` `S_OK`.

So two console recording calls still make the PC list invalid, and both are on the same first frame:

1. **list slot 146 -> PC `ID3D12GraphicsCommandList7::Barrier`**: a 64-byte texture-barrier group whose `pResource`
   is 0 (render_loop.md section 7). `CommandList::barrier(TextureBarrier)` refuses a barrier with no resource, so the
   call marks the list invalid. This is the barrier conversion render_loop.md already names.
2. **list slot 52 -> PC `ID3D12GraphicsCommandList::ClearRenderTargetView`** (new): the clear is the call whose
   muting changes the outcome, and the trace shows it forwarded between the reopen and the failing `Close`.
   `CommandList::clear_render_target` returns false when the view it captures has no resource or no layers
   (`port/src/command_list.h:916`), which sets the list's invalid flag. The slot-52 dump this measurement asked for
   was run: **the handle is right and the view is null** - the handle is the destination this proxy's own
   `CreateRenderTargetView` wrote, and it was created with `pResource = 0`, so the console's clear is the no-op it is,
   not a handle mismatch. Both calls are now converted (the null-view clear is refused as the no-op; a resource-less
   console texture barrier becomes a global barrier), and a 60 s boot with **no mutes** shows **30,999 `Close` `S_OK` /
   0 failed**, 30,376 reopens and submits, ALIVE: [handles_barriers.md](handles_barriers.md) sections 3-6.

Correction this measurement contributes: the barrier at 146 is not the only call that poisons a reopened list; the
render-loop page's successful mute runs all muted slot 52 as well, so its "muting 146 removes the E_INVALIDARG" line
only holds when 52 is muted too. The `E_INVALIDARG` at the first real close has two causes.

## 6. Files and gates

* `port/shims/d3d12_xs_device.cpp`: `ListProxy` holds the creating allocator and the real recording state;
  `note_list_reopen` bounds the logging; `list_slot` reopens a closed list before a forwarded recording call
  (PC slot >= 11) and refuses a call it cannot reopen. The old `PALMETTO_XS_LIST_REOPEN` switch is gone.
* `port/test/xs_proxy_mock_test.cpp`: the mock real list's Close/Reset state and event order, and the lifecycle
  assertions above.
* This page.
