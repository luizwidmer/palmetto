# Boot smoke arms after the round-20 loader fixes (2026-11-27)

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
