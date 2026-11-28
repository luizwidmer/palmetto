# Boot smoke arms: round 20 (2026-11-27) and round 21 (2026-11-28)

**Status: measured. Three 88 s light arms on one binary; no device loss; the shadow pass reported no divergence and
the class-0xa mitigation was installed but never exercised because that arm did not reach the class-0xa window.**

This page records the round-20 boot lock work: the first boot arms after the inherited-loader fixes and the
`PALMETTO_PE_LOADER_SHADOW_PARSE` wiring landed. It is an opt-in measurement page; the default boot path is unchanged,
no captures or readbacks were collected, and this page quotes no game content.

## Conditions

| item | value |
|---|---|
| binary | `D:\leonida\port\build\palmetto.exe` |
| SHA-256 | `BFA39770A2E114F71AB711FB8B43F125A1AA220F24CC57F34E8E46F79EE10AFE` (recorded before and after every arm) |
| binary mtime | `2026-11-27T21:28:54.0438879-03:00` |
| source state | worktree at `db10358b1` with the round-20 changes uncommitted; the lead's marker attributes this hash to a green `PALMETTO_OK` |
| host | Windows, NVIDIA GeForce RTX 4090 driver 32.0.16.1714; Vulkan backend (`PALMETTO_D3D12=vulkan`) |
| boot lock | held by `round20-266` through `_setup/boot_lock.ps1` for each arm; free before and after each run |
| marker | `D:\leonida-work\.round20-arm-ready`: `shadow=1`, hash and `built` time above |
| probes | each arm added `PALMETTO_TRACE_TLS=1`, `PALMETTO_TEB_CHECK=1` and the template's state trace; `PALMETTO_CAPTURE=` and `PALMETTO_READBACK_PPM=` were explicitly empty |

All arms are `boot_arm.ps1 -Light -TimeoutSec 88`, so the state trace samples every 2 s to 88 s. Arm 2 exists only
because the marker says `shadow=1`; without that line the plan skips it.

## Arm table

| arm | extra switch | wall | machine stage (last sample) | stop | device lost | captures |
|---|---|---:|---|---|---|---:|
| `a1` baseline | none (`TRACE_TLS`/`TEB_CHECK` only) | 88.4 s, harness timeout | 2 at 86.0 s (`current 2 next 2`) | none, ALIVE | no | 0 |
| `a2` shadow | `PALMETTO_PE_LOADER_SHADOW_PARSE=1` | 65.4 s | 3 at 62.0 s (`current 3 next 3`) | `game_scarlett_master.exe+0x13360a3`, read `0xffffffffffffffff` (`0xC0000005`) | no | 0 |
| `a3` content | `PALMETTO_TITLE_CLASS0A_SIZE=1` | 88.4 s, harness timeout | 2 at 86.0 s (`current 2 next 2`) | none, ALIVE | no | 0 |

Device loss is the runtime's `[rt] d3d12: present failed (VkResult -4)` line; it was searched in every arm's stderr
and is absent in all three. No two consecutive losses occurred, so the stop rule did not fire.

## Loader state is identical in every arm

The map/import/initialise/TLS surface is the same in all three arms and matches the closest pre-fix reference
(`boot-loader-tls-f1`, 90.3 s light arm, binary `22E9B14B...`, machine stage 2 at 88 s, zero exceptions):

| measure | measured value (all arms) |
|---|---|
| title image | `base=0000000140000000` (preferred, not relocated), size 184995840, entry `+0x56a1ec8` |
| import binding | `shim=95 host=420 module=72 unimplemented=0`; module map/import totals unchanged by the fixes |
| modules | `5 module(s) mapped, 0 unimplemented import(s)`; `unimplemented by return policy: hresult=0 other=0`; `5 module(s) initialised` |
| `/GA` TLS rewrite | `53246 index-0 TLS reads (gs:[58h]) now read its own TLS (TlsSlots[2]); its module TLS index is 6` |
| per-thread installs | 291 `tls: installed` lines over 97 distinct threads in `a1`/`a3`; 309 lines over 103 threads in `a2` |
| TEB check | slot 8 marker `1122334455667788`; `TlsGetValue` matches, `gs:[58]` non-null, `gs:[58+8*slot]=0`, `gs:[1480+8*slot]` matches, `gs:[1780+8*slot]=0` |
| runtime load | `xgameruntime.dll is served by the shims (run-time load)` |

## Arm 1: the 88 s baseline is a stage-2 boot

ALIVE at the 88 s timeout, 12,155 trace events (0 exceptions), 144,878 D3D12 calls. The state trace shows `init 2`
from 20.0 s, `loading 1` from 20.0 s, `session 0`, `machine current 2 next 2`, `movie ready 0x1`, `players 58`, and no
movie player handle (`state ?0x0`, `open ?0x0`). The class-0xa alloc watch never armed, so the 88 s light window does
not reach the class-0xa command; this is the honest limit of the baseline, not a loader failure.

## Arm 2: shadow parse reports no divergence; the boot reaches stage 3 and the known class-0xa fault

The shadow pass prints only mismatches (`[rt] loader shadow parse: <image>: <code>: <message>`; the comparator is
`compare_shadow_parse` in the round-20 `pe_loader.cpp`, and the switch is read once, exact `1`). Searches of stdout,
stderr and the JSONL trace found **0** shadow lines, so for the five mapped images the parsed PE facts matched the old
loader's facts in this boot. The switch was enabled: the same `--env` plumbing produced the `TRACE_TLS` and `TEB_CHECK`
lines in this arm. That is a no-divergence result for one boot, not a proof that the comparator can detect a seeded
mismatch; the synthetic wiring test is the positive control.

This arm did reach stage 3. State changes: `20.0 s` init 1->2 and loading 0->1; `58.0 s` loading 1->2 and session 0->1;
`62.0 s` init 2->3, session 1->3 and machine pending stage 2->3. At the 62.0 s sample the movie player is open
(`state 0x5 next 0x3`, handle non-null, `open 0x1`, `error 0x0`; slot0 entity active/armed, media-id `0x540714f3`).
Then the alloc watch records the known class-0xa sequence and the known stop:

- `t=120595.366` `FORMAT KEY key 0xa`, command vtable `game+0x66314B0`, creator vtable `game+0x658DB60`, caller `game+0x1837A2`;
- `t=120595.367525` `STAGE 3 ENTRY` job from `game+0x186424`;
- `t=120595.390` `HELPER ENTRY class 0xa plane-count 1`;
- `t=120595.401` `BLOCK ALLOC size 0x0 width 256 height 1`, caller `game+0x1335D35`, `overlap YES`;
- `t=120595.438` `ENTRY WRITTEN stride 0x0 size 0x0`;
- then `0xC0000005` read `-1` at `game+0x13360A3` at 65.4 s.

**Honest attribution.** One arm per condition, and stage-3 reach varies between boots on this tree, so the stage-3
reach in `a2` cannot be credited to the shadow switch or to the loader fixes; `a1`/`a3` stayed at stage 2 in the same
window. The class-0xa fault is the one earlier arms measured, unchanged in shape.

## Arm 3: the class-0xa mitigation is installed but not exercised in this window

The mitigation line is present: `class-0xa size mitigation: image+0x1555c10 redirected at 3 direct call site(s); class 0xa
row stride = width*0x10, every other class runs the original bytes`. The alloc watch, however, logs only `waiting for
machine stage 3` and never arms: this arm stayed at `machine current 2 next 2` (`init 2` from 18.0 s, `loading 1`,
`session 0`, no movie handle) through the 86.0 s sample, and was killed at 88.4 s with 0 exceptions and 163,064 D3D12
calls. So after the loader fixes the mitigation is armed at the three call sites but the measured window contains no
class-0xa request; the arm does not falsify or confirm the mitigation's effect.

## Negatives and limits

- No device loss in 3/3 arms, so the two-consecutive-loss stop rule was never reached and all three arms are usable.
- `a1` and `a3` do not reach stage 3 inside 88 s; `a3` therefore cannot answer whether the class-0xa mitigation changes
  the post-fix stop. The open falsifier is a stage-3 light arm with `PALMETTO_TITLE_CLASS0A_SIZE=1` in the window.
- The shadow result is one boot, five mapped images and a no-divergence observation; run `PALMETTO_PE_LOADER_SHADOW_STRICT=1`
  on a stage-3 arm to turn a mismatch into a named stop if one appears.
- Captures and readbacks were suppressed by design (`PALMETTO_CAPTURE=`/`PALMETTO_READBACK_PPM=` empty), so no frame
  content was measured; `capture-round20-266-a1..a3` and `readback-round20-266-a1..a3` are empty by design.
- One binary from one host; the arm record is the binary hash above, because the source tree was dirty at arm time.

## Evidence

| path | contents |
|---|---|
| `D:\leonida-work\fsrstable\boot-round20-266-a1` | arm 1 `meta.txt`, `summary.txt`, `stdout.log`, `stderr.log`, `boot.jsonl` |
| `D:\leonida-work\fsrstable\boot-round20-266-a2` | arm 2, same files (shadow arm) |
| `D:\leonida-work\fsrstable\boot-round20-266-a3` | arm 3, same files (class-0xa switch) |
| `D:\leonida-work\fsrstable\boot-loader-tls-f1` | pre-fix reference arm, same light environment |
| `D:\leonida-work\.round20-arm-ready` | lead marker with the hash and `shadow=1` |

Reproduce one arm under the boot lock (the script acquires and releases it; run one at a time):

```powershell
& D:\leonida-work\fsrstable\boot_arm.ps1 -Tag round20-266-a1 -Light -TimeoutSec 88 -Who round20-266 `
    -ExtraEnv 'PALMETTO_TRACE_TLS=1','PALMETTO_TEB_CHECK=1','PALMETTO_CAPTURE=','PALMETTO_READBACK_PPM='
```

Arm 2 adds `PALMETTO_PE_LOADER_SHADOW_PARSE=1`; arm 3 adds `PALMETTO_TITLE_CLASS0A_SIZE=1` to arm 1's list. The binary
hash is in each arm's `meta.txt` as `exe_sha256`.

## Round 21: arms on the census/F4-F7 loader-fix binary (2026-11-28)

**Status: measured. Three 88 s light arms on the round-21 binary `5C30DE03...`, device losses 0/1/0 so no two
consecutive; the census and shadow switches are in this binary and the shadow pass reported no divergence in its arm.
The census+shadow arm lost the device and stopped at the known post-loss family `game+0x86F97E`; the third arm
reached the class-0xa helper with the mitigation installed, logged a non-zero block/row stride and survived to the 88 s
timeout. Two earlier boots on the intermediate round-21 binary are recorded below as INVALID because the lead attached
cdb to the live process.**

This section continues the arm log for the round-21 loader slice: the loader F4/F7 fixes (marker `f4f7=final-fixed`),
the `PALMETTO_LOADER_CENSUS=1` wiring in `port/src/pe_loader.cpp`, and the round-20 `PALMETTO_PE_LOADER_SHADOW_PARSE=1`.
It is an opt-in measurement page: the default boot path is unchanged, no captures or readbacks were collected, and
this page quotes no game content.

## Round 21 conditions

| item | value |
|---|---|
| binary | `D:\leonida\port\build\palmetto.exe` |
| SHA-256 | `5C30DE03AE22B8FAB6568135DE4A992D71D548E4B12CEFAE3DABB5F9B200451A` (recorded before and after every valid arm, unchanged) |
| binary mtime | `2026-11-28T06:34:57.7928999-03:00` |
| source state | `D:\leonida` at HEAD `2785e429e` with 39 uncommitted round-21 paths at arm time; the binary hash above is the arm record |
| marker | `D:\leonida-work\.round21-arm-ready`: `shadow=1`, `census=1`, `f4f7=final-fixed`, the hash above |
| host | Windows, NVIDIA GeForce RTX 4090 driver 32.0.16.1714, Vulkan backend (`PALMETTO_D3D12=vulkan`) |
| boot lock | held by `round21` through `_setup/boot_lock.ps1` per arm; free before and after every run |
| probes | each arm added `PALMETTO_TRACE_TLS=1`, `PALMETTO_TEB_CHECK=1` and the state trace (2 s to 88 s); `PALMETTO_CAPTURE=` and `PALMETTO_READBACK_PPM=` were empty |
| debugger | no `cdb`, `windbg` or `vsjitdebugger` process existed before or after any valid arm |

All three valid arms are `boot_arm.ps1 -Light -TimeoutSec 88 -Who round21`; the state trace samples every 2 s to 88 s.

## Invalidated arms (lead debugger attach, not runtime results)

Two boots preceded the clean series on the intermediate round-21 binary `FB2F3FFA...`; the lead attached cdb to the
live `palmetto` process during both, and each boot ended when the debug object went away. Their exit code is
`0xC0000354` (`STATUS_DEBUGGER_INACTIVE`), which is debugger-attach interference, not a device loss or a runtime
regression. They are kept as diagnostic evidence only, are excluded from the arm table and from the stop rule, and
their useful observations are recorded here.

| arm | switch set | wall | cut at | exit | retained observations |
|---|---|---:|---|---|---|
| `x1` invalid | baseline (`TRACE_TLS`/`TEB_CHECK` only) | 86.1 s | stage 2 at the 76.0 s sample (`init 2`, `loading 1`, `session 0`, machine 2/2) | `0xC0000354` | loader surface identical, 291 `tls: installed` lines, 0 census/shadow lines (switches off), 0 device-loss lines |
| `x2` invalid | `PALMETTO_LOADER_CENSUS=1` + `PALMETTO_PE_LOADER_SHADOW_PARSE=1` | 12.2 s | `init 1` at the 10.0 s sample | `0xC0000354` | 15 census lines and 0 shadow divergence lines up to the cut, loader surface identical, 0 device-loss lines |

## Round 21 arm table

| arm | extra switch | wall | machine stage (last sample) | stop | device lost | captures |
|---|---|---:|---|---|---|---:|
| `a1` baseline | none (`TRACE_TLS`/`TEB_CHECK` only) | 88.4 s, harness timeout | 2 at 86.0 s (`current 2 next 2`, `init 2`, `loading 1`, `session 0`) | none, ALIVE | 0 | 0 |
| `a2` census+shadow | `PALMETTO_LOADER_CENSUS=1`, `PALMETTO_PE_LOADER_SHADOW_PARSE=1` | 82.1 s | 2 at 78.0 s (`current 2 next 2`, `init 2`, `loading 2`, `session 1`) | `game_scarlett_master.exe+0x86F97E`, read `0x1be00000006` (`0xC0000005`) | 1 | 0 |
| `a3` ds+class0xa | `PALMETTO_DS_REAP_ORDER=1`, `PALMETTO_TITLE_CLASS0A_SIZE=1` | 88.2 s, harness timeout | 3 at 86.0 s (`current 3 next 3`, `init 3`, `loading 2`, `session 3`) | none, ALIVE | 0 | 0 |

Device loss is the runtime's `[rt] d3d12: present failed (VkResult -4)` line; the counts were 0, 1 and 0 over `a1..a3`,
so the two-consecutive-loss stop rule never fired and all three valid arms are usable.

## Loader surface is identical in every valid arm

| measure | measured value (all three arms) |
|---|---|
| title image | `base=0000000140000000` (preferred, not relocated), size 184,995,840, entry `+0x56a1ec8` |
| import binding | `shim=95 host=420 module=72 unimplemented=0` (587 IAT slots; `unimplemented by return policy: hresult=0 other=0`) |
| modules | `5 module(s) mapped, 0 unimplemented import(s)`; `5 module(s) initialised` |
| `/GA` TLS rewrite | `53246 index-0 TLS reads (gs:[58h]) now read its own TLS (TlsSlots[2]); its module TLS index is 6` |
| per-thread installs | 291 `tls: installed` lines in `a1` and `a2`, 360 in `a3` (the arm reached more threads) |
| TEB check | slot 8 marker `1122334455667788`; `TlsGetValue` matches, `gs:[58]` non-null, `gs:[58+8*slot]=0`, `gs:[1480+8*slot]` matches, `gs:[1780+8*slot]=0` |
| runtime load | `xgameruntime.dll is served by the shims (run-time load)` |

## Arm 1: the 88 s baseline is a stage-2 boot

ALIVE at the 88 s timeout with 129,156 D3D12 calls, 11,278 trace events and 0 recorded exceptions. The state trace
shows `init 2` from 24.0 s, `loading 1` from 24.0 s, `session 0`, `machine current 2 next 2`, `movie ready 0x1`,
`players 58`, no movie player handle (`state ?0x0`, `open ?0x0`) and the front-end manager ready/running (`0x1`).
The census, DS-order and class-0xa switches were off in this arm, so it has none of their lines.

## Arm 2: census and shadow are present; no divergence; one device loss and the known post-loss stop

The census layer emitted **15** lines, five each of `mapped`, `bound` and `tls`, for the four helper modules and the
title image (taken from the 12.2 s diagnostic boot and the full valid arm, identical shapes):

- `[rt] loader: census v1 mapped game_scarlett_master.exe base=0x140000000 size=0xb06d000 preferred=0x140000000 relocated=false entry=+0x56a1ec8`;
- `[rt] loader: census v1 bound game_scarlett_master.exe total=587 shim=95 host=420 module=72 unimplemented=0`;
- `[rt] loader: census v1 tls game_scarlett_master.exe index=6 slot=0 ga_slot=2 ga_sites=53246`;
- `[rt] loader: census v1 mapped amd_fidelityfx_loader_xbox.dll base=0x1b2b3de0000 size=0x8000 preferred=0x180000000 relocated=true entry=+0x18d0`;
- `[rt] loader: census v1 tls libHttpClient.GDK.dll index=13 slot=2 ga_slot=none ga_sites=0` (the other helper modules show the same shape; `XboxBvhBuilder.dll` maps at its preferred `0x180000000`).

No `protect-failed`, `protect-omitted` or `tls-counters` census line appeared in this window; the census says nothing
about the protection plan unless a `VirtualProtect` fails.

The shadow pass prints only mismatches (`[rt] loader shadow parse: ...`); searches of stdout, stderr and the JSONL
trace found **0** divergence lines, so the parsed PE facts matched the old loader for the five mapped images in this
boot. That is a no-divergence observation for one boot; the synthetic wiring test is the positive control.

This arm advanced `loading 1->2` and `session 0->1` at 78.0 s but the machine stage stayed 2 (`current 2 next 2`); the movie player moved to `state 0x3 next 0x3`, `open 0x1`,
`probe 0x1`, slot0 entity active/armed with media-id `0x540714f3`. Two `ExecuteCommandLists` submissions were then
refused (`unsupported; nothing was submitted`), the next present failed with `VkResult -4` (the arm's one device-loss
line), and the first exception followed at trace 80.47 s: `game_scarlett_master.exe+0x86F97E` read
`0x1be00000006` (`0xC0000005`, wall 82.1 s). `0x86F97E` is the known family earlier arms record after a device loss,
so this is a loss-shaped stop, not a new loader stop; one boot, not investigated further.

## Arm 3: stage 3 reaches the class-0xa helper and survives the window

ALIVE at the 88.2 s timeout with 267,296 D3D12 calls, 12,306 trace events and 0 exceptions. State: `init 3` from
66.0 s, `loading 2` from 64.0 s, `session 3`, `machine current 3 next 3`, movie player `state 0x9 next 0x3`, handle
non-null, `open 0x1`, `error 0x0`, slot0 entity active/armed with media-id `0x540714f3`.

The mitigation line is present: `class-0xa size mitigation: image+0x1555c10 redirected at 3 direct call site(s); class
0xa row stride = width*0x10, every other class runs the original bytes`. The alloc watch then records the sequence
that the round-20 shadow arm crashed on, here with non-zero sizes and no overlap:

- `t=153126.855` stage-3 read, probe armed at the measured points;
- `t=153128.650` `FORMAT KEY key 0xa`, command vtable `game+0x66314B0`, creator vtable `game+0x658DB60`, caller `game+0x1837A2`;
- `t=153128.650956` `STAGE 3 ENTRY` job from `game+0x186424`;
- `t=153128.673` `HELPER ENTRY class 0xa plane-count 1`;
- `t=153128.684` `BLOCK ALLOC size 0x1000 width 256 height 1` caller `game+0x1335D35`, `overlap no`;
- `t=153128.720` `ENTRY WRITTEN stride 0x1000 size 0x1000`.

Against the round-20 shadow arm (no mitigation), whose class-0xa sequence was `BLOCK ALLOC size 0x0`, `overlap YES`,
`ENTRY WRITTEN stride 0x0 size 0x0` and then the AV at `game+0x13360A3`, this arm's block and row stride are non-zero
and it ran to its timeout. That is a cross-arm, cross-boot contrast of one arm each, not a controlled A/B, and the
stage-3 reach itself is not attributable to either switch (round-20's stage-3 arm had neither).

The ordered-publication switch printed its one line: `PALMETTO_DS_REAP_ORDER=1: request work runs on 2 lanes and
status/fence/event publication retires in enqueue order (bounded buffer 64; ...)`. No named refusal or lane-start
failure followed, the DS log had the same six interface/request lines as `a1` plus this line, and the same single
`zlib request decompressed` entry; no serial versus ordered request-count or limit difference is measurable in this
88 s stage-3 window, and there is no paired serial arm at the same stage to attribute one.

## Round 21 negatives and limits

- One valid arm per condition; stage-3 reach varies between boots on this tree, so `a3` reaching stage 3 cannot be
  credited to the census, shadow, DS-order or class-0xa switches.
- The class-0xa survival in `a3` is one boot and the round-20 comparison is a different boot; the open falsifier is a
  repeated stage-3 `PALMETTO_TITLE_CLASS0A_SIZE=1` arm and a paired `CLASS0A_SIZE=0` arm in the same window.
- `a2`'s device loss is one boot; its two refused submissions were not investigated and no causal link to the census or
  shadow switches is claimed (both are opt-in diagnostics that only format or compare parsed facts).
- The shadow result is one boot and five mapped images; a seeded mismatch was not run, so the comparator's positive
  control stays the synthetic `pe_loader_shadow_wiring` test.
- Captures and readbacks were suppressed by design (`PALMETTO_CAPTURE=`/`PALMETTO_READBACK_PPM=` empty), so no frame
  content was measured.
- One binary from one host; the source tree was dirty at arm time, so the binary hash above is the record, and the two
  invalid arms are only reproducible as the interference record they are.

## Round 21 evidence

| path | contents |
|---|---|
| `D:\leonida-work\fsrstable\boot-round21-r21d5-a1` | arm 1 `meta.txt`, `summary.txt`, `stdout.log`, `stderr.log`, `boot.jsonl` |
| `D:\leonida-work\fsrstable\boot-round21-r21d5-a2` | arm 2, same files (census + shadow arm; one device loss) |
| `D:\leonida-work\fsrstable\boot-round21-r21d5-a3` | arm 3, same files (DS order + class-0xa arm; stage 3, ALIVE) |
| `D:\leonida-work\fsrstable\boot-round21-r21d5-a1-invalid-cdb` | invalidated diagnostic boot, cdb attach (arm `x1`) |
| `D:\leonida-work\fsrstable\boot-round21-r21d5-a2-invalid-cdb` | invalidated diagnostic boot, cdb attach (arm `x2`) |
| `D:\leonida-work\scratch\r21\r21d5\arm-results.txt` | marker text, per-arm hash before/after, extracted state/census/shadow/DS/class-0xa lines |
| `D:\leonida-work\.round21-arm-ready` | lead marker with the final hash and the switch/fix lines |

Reproduce one valid arm under the boot lock (one at a time; the script acquires and releases it):

```powershell
& D:\leonida-work\fsrstable\boot_arm.ps1 -Tag round21-r21d5-a1 -Light -TimeoutSec 88 -Who round21 `
    -ExtraEnv 'PALMETTO_TRACE_TLS=1','PALMETTO_TEB_CHECK=1','PALMETTO_CAPTURE=','PALMETTO_READBACK_PPM='
```

Arm 2 adds `PALMETTO_LOADER_CENSUS=1` and `PALMETTO_PE_LOADER_SHADOW_PARSE=1` to arm 1's list; arm 3 adds
`PALMETTO_DS_REAP_ORDER=1` and `PALMETTO_TITLE_CLASS0A_SIZE=1`. The binary hash is in each arm's `meta.txt` as
`exe_sha256`.
