# The networking poll that runs for the whole boot: measured, and the console-correct offline hint

**Status: measured negative on the stage gate.** The title polls `XNetworkingGetConnectivityHint` (`XNetworkingImpl`
slot 15) about **120 times a second for the rest of the boot**, from the loading stage to the kill, and our offline
answer is already the console one: `S_OK` with `networkInitialized = true` and connectivity level `None`. The title's
call sites read exactly the two fields the public GDK documents use - level 3/4 for "has internet" and
`networkInitialized` for its start-up wait - so the poll is a connectivity **change monitor** that takes the title's
offline path, not a retry of a failed answer and not a wait this runtime answers wrongly. The experiment arm that reports
`InternetAccess` is consumed (the title then starts its preferred-UDP-port query once) and still leaves the stage, the
stage-2 holder and the pending counter unchanged. This slice therefore makes **no default behaviour change**; it adds
the measurement (a per-slot `XNetworkingImpl` trace), a testable hint-level seam, tests that replay the measured shapes,
and records the negative with this page.

## 1. Why this surface, and what was measured

The task-queue/threading parity measurement ([task_queue_parity.md](task_queue_parity.md) section 3) named this as the
frontier fact it could not explain: after +25.5 s the only platform class the title keeps requesting is
`XNetworkingImpl` - one request at ~+1.1 s, then #100 at +26.31 s, #1000 at +33.77 s, #10000 at +108.43 s in that boot -
on a title thread that is not the frozen stage job. Its owner and its slot were not identified. This slice measures
both, decides what an offline console would answer, and tests whether the answer can gate the stage machine.

The title reaches the class through the shared `xgameruntime.lib` thunk helper (`game+0x55DE08C`), so the existing
`QueryApiImpl` line names the class but not the method. `port/shims/xgameruntime.cpp` now wraps each `XNetworkingImpl`
vtable entry the way the task-queue slice wraps `XThreadingImpl`: one forwarding template prints **slot, method name,
thread, caller RVA, up to five scalar arguments and the answer** under `PALMETTO_USER_TRACE`, bounded per slot (first
eight calls, then the tenth, hundredth, ...). With the switch unset the wrapper only reads the cached
`platform_trace_enabled()` flag. The same boot carries the existing `PALMETTO_STAGE2_READY` probe, the
`PALMETTO_STATE_TRACE` state words and `PALMETTO_PIPELINE_TIMELINE`.

## 2. Measured shapes

### 2.1 The measured call table

One 130 s measurement boot (M1) of `port/build/palmetto.exe` SHA-256
`C294F7893E8EB9AA5304FDF66D525713502514EEF2BE5808E6936F6A5CF38B7B` with the standard environment
(`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`,
`PALMETTO_STATE_TRACE=2,130`, `PALMETTO_USER_TRACE=1`, `PALMETTO_STAGE2_READY=20`, `PALMETTO_PIPELINE_TIMELINE=10`,
`PALMETTO_CAPTURE`/`PALMETTO_CAPTURE_EVERY=2000`/`PALMETTO_CAPTURE_LIMIT=8`). Slot numbers are
[tooling/gdk_runtime_classes.json](../tooling/gdk_runtime_classes.json); the caller RVA is where the slot was called
from.

| slot | method | calls | measured arguments | answer | thread(s) | when (s) |
|---:|---|---:|---|---|---|---|
| 15 | `XNetworkingGetConnectivityHint` | 10,000+ in 130 s (the class is requested 10,000 times by +112.26 s) | `(out)` - one pointer to the caller's 16-byte `XNetworkingConnectivityHint` on its stack; `a1`-`a4` zero | `S_OK` (0x0), every call | 45212 first; 32092 then 11560 for the sustained poll | +1.20, then +28.06 - +130.4 |

The frequency is readable between the bounded log lines: `#100` at +28.87 s, `#1000` at +36.33 s and `#10000` at
+112.26 s, i.e. **900 / 7.46 s = 120.6/s** and **9000 / 75.93 s = 118.5/s**. The call table fits one row because the
title calls exactly one networking slot: no other `XNetworkingImpl` slot is called in the boot, and the class is
requested once per poll (the `QueryApiImpl` trace and the slot trace agree call for call).

The last line before the kill is still the same poll: nothing about the answer, the caller or the frequency changes
from the start of the polling to the kill. A second full-boot measurement on the post-change binary (section 5) says
the same: #100 at +25.92 s, #1000 at +33.37 s, #10000 at +108.03 s, 120.8/s and 120.5/s, `S_OK` throughout.

### 2.2 The slot's thunk and the title's three readers (static, from the image)

`tooling/gdk_runtime_calls.py` reads ten referenced `XNetworkingImpl` thunks from the retail image, all with
`IXNetworkingImpl2`'s interface id: slots 3, 4, 5, 8, 9, 10, 14, 15, 16 and 17. The thunk that reaches slot 15 is
`game+0x55E0E30..0x55E0E9D`: it calls the shared helper `game+0x55DE048` with the class and `IXNetworkingImpl2` ids,
then `mov rax,[rax+0x78]` (slot 15, `0x78 = 15 * 8`) and calls it through the control-flow-guard dispatch. The vtable
slot's return address, which the trace prints as the caller, is `game+0x55E0E7D`.

That thunk is called from exactly three places in the image (all RVAs `game+0x...`, read with a targeted capstone
disassembly in `D:\leonida-work\netgate\netdis.py`):

| call site | function | what the caller does with the answer |
|---|---|---|
| `0xF9D9A` | `0xF9D80`, the "has internet" helper | `HRESULT` must be `>= 0` **and** `connectivityLevel - 3 < 2` (level `InternetAccess` 3 or `ConstrainedInternetAccess` 4); a first call to `0x4F942E0` that returns true short-circuits it to false (the peer reads this as a cached global flag) |
| `0x18E726D` | `0x18E7260`, the "network initialized" helper | `HRESULT >= 0` **and** the byte at hint offset `0xC` (`networkInitialized`); the caller, `0x41E1D40`, keeps a countdown at `[rsi+0x120]` and drives a network bring-up state machine |
| `0x1CF8F0A` | `0x1CF8F00`, the start-up wait | if the call fails **or** `networkInitialized` is false: create a queue, register `XNetworkingRegisterConnectivityHintChanged` with an event as the context, wait on it, unregister; otherwise return immediately |

The three readers use only two facts: the level (to decide online/offline) and `networkInitialized` (to decide whether
the network stack may be touched at all). The 16-byte structure's other fields - cost, IANA interface type, the data
limit flags, roaming - are read by none of these three sites. The sustained poll's caller at run time is identified in
section 2.3.

### 2.3 The caller: the slot's thunk, and the title's three sites

The slot's caller is the same on every one of the 10,000+ calls: **`game+0x55E0E7D`**, the return address inside the
`XNetworkingGetConnectivityHint` thunk of section 2.2 (`0x55E0E30..0x55E0E9D`). The three short functions of section 2.2
are the only callers of that thunk (16-60 bytes each, ending at the thunk), so the in-process trace cannot tell them
apart by argument or answer; a per-call stack capture of the calling thread (an experimental addition to this trace,
not kept) printed the shim's and host's frames and stopped at the thunk frame. **The title site above the thunk is
therefore not separated by this measurement**; what is measured is the slot, the answer, the frequency, the thread and
the thunk-level caller. The one-shot call at ~+1.1 s (before the sustained poll starts) is the title's start-up wait
shape; the sustained poll is on a single title thread (11560 in M1, 73144 in the final boot) that starts when the
loading stage begins. Two run-time facts narrow the sustained poll's reader without proving it: in the internet arm the
same thread starts the title's preferred-UDP-port query (`0x41F1150`) within 30 ms of a poll, and the two readers that
can be on a networking thread - the network-initialized helper (`0x18E7260`, driven by the state machine at
`0x41E1D40`) and the online monitor (`0x266EA0`, which calls the has-internet helper) - read exactly the two documented
fields. The next falsifier is a backtrace at the poll's call site (section 6).

## 3. What the console would answer, and why

The offline console state this title is asking about has two parts, and both are documented in Microsoft's GDK
reference:

- **The network stack is up.** `XNetworkingConnectivityHint::networkInitialized` "indicates whether the network is
  initialized"; the GDK's network initialization page tells titles to wait for it to be `true` before calling WinSock,
  WinHTTP, BCrypt and the other networking and security APIs, and the sample start-up sequence returns `E_PENDING` if
  it is false. A console that is up, with no cable or no reachable network, has the stack initialized, so the
  console-correct answer is `networkInitialized = true`. A runtime that answered `false` would make the title take the
  wait in its start-up site `0x1CF8F00` - which is the shape of a real gate, not of the idle measured here.
- **Nothing is reachable.** Microsoft documents that titles "can expect `XNetworkingConnectivityLevelHint::None` to
  reflect network medium disconnect and general lack of connectivity in steady state where the network environment
  hasn't changed for several minutes". That is the offline answer for the level: `None` (1), not `LocalAccess`,
  `InternetAccess` or `ConstrainedInternetAccess`. The title's has-internet helper accepts only levels 3 and 4, so
  `None` takes its offline path - which is what this runtime has always answered.

The remaining fields are hints. For a machine with no reachable network the console's guesses are Unknown cost and an
unknown interface type; this runtime answers cost `Unknown` (0) and interface type 0, which VIctory's offline console
model uses as well. The call itself is documented as a real-time, safe query that returns immediately, so `S_OK` is the
console answer even when the state is "offline"; a failed query would be a different contract.

The current shim already returns exactly that, so the measured-correct answer needs no change: `S_OK`, level `None`,
cost `Unknown`, interface 0, `networkInitialized = true`, all other flags false, and the change registrations send the
initial notification Microsoft documents ("this function ... sends an initial notification callback"). The experiment
arm in section 5 reports `InternetAccess` with `Unrestricted` cost instead, to test what the title does with the only
state it can tell apart from offline.

## 4. What we implemented

- **The measurement, in `port/shims/xgameruntime.cpp`**: every `XNetworkingImpl` vtable entry now goes through
  `NetMethod<slot, fn>`, which prints the measured line under `PALMETTO_USER_TRACE` and forwards exactly as before
  otherwise. A `networking_slot_name` table names slots 3-20. The trace is bounded per slot and prints the calling
  thread, the caller RVA, the first five scalar arguments and the answer; a poll cannot flood the log, and a call that
  blocks would show an entry with no return line.
- **The hint-level seam.** `net_connectivity_hint` now composes the hint from two values - level and cost - that come
  from `PALMETTO_NETWORK_HINT` (read once): unset, `none` or anything unrecognised keep the measured offline answer
  (`None`, `Unknown`); `unknown` reports level 0; `local` reports `LocalAccess` (2) with `Unrestricted` cost; `internet`
  reports `InternetAccess` (3) with `Unrestricted` cost. The switch is an **experiment arm, never a console answer**:
  what it proves is that the title reads the level, not that a network is there. `network_initialized` stays `true` in
  every mode, because that is the console-correct offline answer and the field the title's start-up wait needs.
- **The test seam** `rt::shims::xgameruntime_set_network_hint(level, cost)` forces the two values (or `-1` to follow the
  environment) and returns the level the hint answered before the call, so one test process can check both faces.
- **No default answer change**, and a measured reason for it: the console-correct offline answer is the one this shim
  already gave (section 3), and the A/B in section 5 shows the answer is consumed without gating the stage. A change to
  the default here would be unmeasured guesswork.

The 16-byte structure layout, its field offsets and the two readers that use them are the existing `ConnectivityHint`
struct in this file (level at 0, cost at 4, IANA interface type at 8, four bools from 12, `networkInitialized` at
`0xC`), which the static reader at `game+0x18E7260` corroborates: it tests the byte at `+0xC`.

## 5. A/B measurement

One pair, both arms on the same binary (`port\build\palmetto.exe` SHA-256
`CFEFFE245B2AE608D7777437F1D090B0CC7B1E261678F28C5E2A35714A2E3B95`), one at a time under
`_setup/boot_lock.ps1`, each with its own overlay and capture directory outside the repository. The environment is the
one of section 2.1; arm B adds `PALMETTO_NETWORK_HINT=internet`.

| measurement | A control (offline answer) | B internet arm |
|---|---|---|
| verdict | ALIVE 130.4 s, killed by the timeout, 0 exceptions | ALIVE 130.3 s, killed by the timeout, 0 exceptions |
| traced D3D12 calls (max #) | 235,935 (#238,230) | 237,384 (#239,946) |
| `XNetworkingImpl` slot 15 calls logged | 12 lines of 10,000+ calls | 13 lines of 10,000+ calls |
| slot 15 answer | `S_OK`, level `None`, initialized | `S_OK`, level `InternetAccess`, initialized |
| slot 15 poll frequency | 120.8/s (#100 +25.92, #1000 +33.37, #10000 +108.03) | 121.0/s (#100 +23.95, #1000 +31.39, #10000 +105.84) |
| other networking slots | none | **slot 4 `XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync` once**, +23.20 s, `S_OK` (the title's online branch starts its multiplayer-port query) |
| stage words | `init 2` / `loading 1` from +22.0 s, `machine current 2 / next 2` to the kill | `init 2` / `loading 1` from +20.0 s, `machine current 2 / next 2` to the kill |
| stage-3 request | never written (the machine stays `2/2`) | never written |
| pending counter / holder | `0x69` (105) at the steady sample +29.81 s, holder job `109` (`f0 4`, inside its work function) | `0x69` (105) at +27.96 s, the same holder |
| slot-57 records / slot-55 states | 10,241 / 41,545 | 10,241 / 41,545 |
| graphics / compute pipelines | 51,786 / 474 | 51,786 / 474 |
| submits / presents at the +120 s row | 9,325 / 4,662 | 9,415 / 4,707 |
| presenter captures | frames 1, 2,001, 4,001: 3 files, 2564x1421, mean rgb 0.00 0.00 0.00 | the same, 3 black frames |

Reading the table: the switch is **consumed** - the internet arm makes the title start its preferred UDP multiplayer
port query, a call the control arm never makes, so the title really reads the level and branches - and the answer
still does not move the stage: the stage words, the stage-3 request, the holder job `109` and its frozen pending
counter are the same in both arms, and the population figures are identical (the D3D12 call totals and the present
count differ by boot speed, as in every pair on this shared host). The poll continues at the same ~120/s in both arms,
which is the behaviour of a change monitor: the level flip is seen and does not stop the polling.

Verdict: **negative for the stage gate.** A console-correct or fabricated networking answer cannot move the loading
stage of this title; the hint is read for the title's online/offline selection and for its network-ready wait, both of
which the default answer already satisfies. The default answer is unchanged.

A before/after check of the default path: the M1 measurement boot (before the change, binary `C294F789...`) had the
same shape - `init 2`/`loading 1` from +24.0 s, machine `2/2`, holder `109` with pending `0x69` at +32.66 s, no
stage-3 request, three black captures - so the instrumentation and the seam changed no default behaviour. The final
gate build (SHA-256 `8C7AAC7529385795E2BCA42D470105066ADC27616D2CF59C2E63A839BA3C4B01`, the pair binary with the
failed stack-capture experiment removed from the trace path) was booted once for 130 s with the stage-2 probe and the
same environment: ALIVE 130.4 s, 0 exceptions, 229,111 D3D12 calls, slot 15 at #100 +28.06 s / #1000 +35.52 s / #10000
+110.05 s (120.6/s and 120.8/s), `S_OK` throughout, `init 2`/`loading 1` from +24.0 s, machine `2/2` to the kill, holder
`109` with pending `0x69` at +31.94 s, no stage-3 request, 4,424 presents at +120 s, and the same three black
2564x1421 captures. The pair binary and the final binary differ only in that diagnostic path; the default path and the
switch are the same source.

## 6. Limits and the next falsifier

- **The title site above the thunk is not separated** (section 2.3). The slot, the answer, the frequency, the thread and
  the thunk-level caller are measured; which of the three short readers calls the thunk was not resolved at run time,
  and all three read the same two fields. The next falsifier is a backtrace at the poll's call site: a working title-frame
  stack capture (or a debugger breakpoint on the thunk) around +40 s names it.
- **One pair on one machine**, shared with other slices' builds and boots; the boot clock moves (the loading stage
  starts +16 to +24 s across the measured boots) and the poll starts with it. The durable state - stage words, holder,
  pending counter, population, black present - does not depend on it.
- **A 130 s window cannot exclude a stage after 130 s**, but the 600 s boot of [long_boot.md](long_boot.md) and the
  earlier 300 s traces show no later title-side work either.
- **The count is bounded by the trace schedule.** The measured 10,000 calls are the class's own request count, not a
  total from a per-call log; the frequency comes from the #100/#1000/#10000 lines.
- **The internet arm is fabricated.** It is not a console state; it exists to show the title's read and branch. What
  would make a *level* a default is a measured console session in which the level changes, and the title's own readers
  are the documented way it learns.
- **The registration path is unexercised in this boot**: the title never calls slots 16/17, so the initial notification
  the shim sends to a registered connectivity callback is measured only by the tests, not by the title, in these boots.
  Five-star's docs record the same uncertainty ("whether it runs a connectivity-change callback at registration").
- **The hint's cost and interface type are hints**, not measured from a console: `Unknown` (0) and 0 are the offline
  guesses, corroborated by VIctory's offline model; five-star reports `Unrestricted` cost in its offline mode. Nothing
  in the title's three readers reads either field.
- **No host network observation.** Unlike XCL's host-connectivity implementation, this runtime never reports the PC's
  own internet access, so a host with a network still presents the console offline hint. That is deliberate and matches
  the console state being modelled; it is also why our answer differs from XCL's by design.
- **The next falsifier:** a boot in which the title registers a connectivity-change callback and the runtime must
  deliver a *state change* (not only the initial notification), or a title path that reads the cost/interface fields.
  Neither is present in this boot: no slot 16/17 call and no read of cost or interface type in the three disassembled
  readers.
- No game data: this page carries API names, slot numbers, times, counts, HRESULTs, the title's own RVAs and image
  means only - no package path, executable byte, shader, table or save content.

## 7. Tests

`port/test/xgameruntime_test.cpp` extends the existing `XNetworkingImpl` section with the measured shapes and the seam:

- the measured offline default: slot 15 answers `S_OK` with level `None` (1), cost `Unknown` (0), interface type 0,
  `networkInitialized` true and no data-limit/roaming flag; the title's has-internet test (`level - 3 < 2`) is false;
- the internet experiment arm (`rt::shims::xgameruntime_set_network_hint(3, 1)`): level `InternetAccess`,
  cost `Unrestricted`, still initialized, and the title's has-internet test is true;
- level `Unknown` (0) is still an initialized stack answering `S_OK`;
- clearing the seam (`-1, -1`) restores the measured offline answer;
- the registered connectivity callback still receives the initial notification with the forced hint (the existing
  registration checks);
- result: **180 checks, 0 failures**.

The A/B and caller boots are measurement, not tests: they are not in the repository, and no test depends on game files.

## 8. Attribution

This slice was implemented from this project's own measurements: the call table, the boot state, the thunk and reader
disassembly are this tree's. The behaviour was cross-checked, at the mechanism level only, against the public projects
[Marvisak/VIctory](https://github.com/Marvisak/VIctory) (GPL-3.0-or-later, `crates/victory-hle/src/modules/xnetworking.rs`
and its offline hint and "titles wait for the stack" comment, commit `13f91ca`),
[xodus-gaming/XCL](https://github.com/xodus-gaming/xcl) (GPL-2.0-only, `crates/xcl/src/hle/xgameruntime/networking.rs`
and its xnetworking Rust review document, commit `6d12f58`) and
[ThatRecompDev/five-star-recomp](https://github.com/ThatRecompDev/five-star-recomp) (GPL-3.0-or-later,
its loader page "How the executable reads the hint" and wiki page "Network", commit `0bfa755`);
the connectivity-level and initialization contract is Microsoft's public GDK documentation
([network initialization and connectivity](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/networking/initialization-connectivity-networking),
[XNetworkingGetConnectivityHint](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/networking/xnetworking/functions/xnetworkinggetconnectivityhint),
[XNetworkingRegisterConnectivityHintChanged](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/networking/xnetworking/functions/xnetworkingregisterconnectivityhintchanged)).
No code from those projects was copied. The upstream projects remain the copyright of their contributors and their
licenses continue to apply to their own trees. The lead adds the `NOTICE.md` entry; this slice makes no git writes.

## 9. Evidence and reproduction

Outside the repository, under `D:\leonida-work\netgate`:

- `boot-m1` - the measurement boot of section 2.1 (binary `C294F789...`): `meta.txt` with the hash, `summary.txt`,
  `stderr.log` (the trace), `boot.jsonl`, and `capture-m1` (frames 1, 2,001 and 4,001).
- `boot-a3` and `boot-b2-internet` - the A/B pair of section 5, both on binary
  `CFEFFE245B2AE608D7777437F1D090B0CC7B1E261678F28C5E2A35714A2E3B95`; `capture-a3` and
  `capture-b2-internet` are their captures. `boot-m10` is the final 130 s boot on the gate binary
  `8C7AAC7529385795E2BCA42D470105066ADC27616D2CF59C2E63A839BA3C4B01`; `capture-m10` its captures.
- `analyze_arm.py` prints one arm's tables (networking lines, poll times, stage words, holder, timeline, captures);
  `mean_captures.py` prints the captures' mean RGB; `netdis.py` ... `netdis5.py` are the targeted capstone
  disassemblies of the thunk, the three readers and the slot-4 caller; `docs_check.py` runs the Markdown checks with
  this page included (the checks enumerate tracked Markdown, and this page is untracked until the lead commits it).
- `boot_m1.ps1`, `boot_pair.ps1`, `boot_pair2.ps1`, `boot_final.ps1`, `boot_caller.ps1` and `boot_short.ps1` run one
  arm each (they take `D:\leonida-work\.boot-lock`, the shared boot lock). No game bytes are read by the analysis.

Reproduce one arm (from the checkout root, after `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`):

```powershell
python -I tooling\boot_report.py --exe port\build\palmetto.exe --image <retail image> --game-dir <package> `
    --overlay <work>\overlay-b --out <work>\boot-b --timeout 130 `
    --env PALMETTO_SHADERS=<work>\shaders\spirv --env PALMETTO_D3D12=vulkan --env PALMETTO_SKIP_INT3=1 `
    --env PALMETTO_SEED_GLOBAL=75DF268:D8 --env PALMETTO_STATE_TRACE=2,130 --env PALMETTO_USER_TRACE=1 `
    --env PALMETTO_STAGE2_READY=20 --env PALMETTO_PIPELINE_TIMELINE=10 `
    --env PALMETTO_CAPTURE=<work>\capture-b --env PALMETTO_CAPTURE_EVERY=2000 --env PALMETTO_CAPTURE_LIMIT=8 `
    --env PALMETTO_NETWORK_HINT=internet
```

Drop the last `--env` for the control arm; hold the boot lock around each run.
