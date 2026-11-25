# Porting an upstream reference with our own standards

**Status: policy, not a measurement.** Every slice that applies advances from a public prior-art project to Palmetto
follows this page. The first slice under it is [audio_shims.md](audio_shims.md) (the `xapu.dll` and `chad.dll` shims).

The public reference today is [ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida), a GPL-3.0 fork
of this project that has carried parts of the console layer further in some areas (audio, story load, a few device
slots). It is an **evidence source**, never a code source: its commits, its measurements and its docs tell us what a
call shape or a failure means; the implementation in this repository is our own, in our structure, naming and test
style. Where our measurement disagrees with it, our measurement wins and the disagreement is recorded.

## 1. Why a policy at all

The two trees solve the same problem with different histories, and copying a slice would import its assumptions,
its bugs and its unmeasured guesses into a tree whose pages are careful about what is proven. It would also make the
next measurement ambiguous: a reader could no longer tell which claims are ours and which came along with the code.
A measured slice costs more up front and leaves the tree knowing what it knows.

The policy is also how the project keeps its own license and attribution honest. Palmetto is GPL-3.0; the reference is
GPL-3.0 too. We still credit the upstream project, the reference files and the commits/authors consulted, because
their work is what pointed us at the behaviour, and because a later reader must be able to check our cross-check.

## 2. The rule that outranks the rest

**A measurement or a policy conflict is resolved by the measurement, and the conflict is written down.** A slice
request may name the behaviour it expects from a reference. If our own call-site analysis shows something different,
the shim follows the measurement, the doc states the difference, and the report says so plainly. A negative result
(the slice fixes its target and the boot does not advance) is a valid slice; hiding it, or dressing it as progress,
is not.

Worked example from slice 1: the task named `XApuConnect`'s 24-byte block as an output to zero. Our disassembly
showed the title's own audio init writing that block (16-byte template plus the qword `0x0000000100000000`, with
`0x104000` at +0xC, the buffer size the title expects) and no title read of it after the call. The shim therefore
leaves it untouched; [audio_shims.md](audio_shims.md) records the evidence, and notes that the upstream fork's later
revision reached the same conclusion after starting with the same zeroing assumption.

## 3. What a slice must do

| step | requirement |
|---|---|
| Measure | Derive the semantics from our own static/dynamic measurement first: call-site counts and RVAs (`tooling/import_call_sites.py`), argument registers and the memory each site reads or writes (disassembly), the runtime behaviour (A/B boot through the harness). Public documentation is a second source; the reference is the third, used to cross-check. |
| Implement | Our own code, in our layout and style: `port/shims/`, a `SHIM_TABLE`, `startup_trace` events, the project's naming (`PALMETTO_*`, `rt::shims`), comments that state measured facts. No code, comment text, identifier or table copied from the reference; a shape that has to be identical (an ABI) is written from the measurement. |
| Cross-check | Say in the slice doc where the reference's semantics agree and where they differ, with the reference file and commit for each. A difference is a finding, not a nuisance. |
| Attribute | Add the project, license, reference paths and commits/authors consulted to [NOTICE.md](../NOTICE.md). The slice doc repeats the attribution in its own last section. |
| Keep game data out | No package path, shader byte, extracted table, save file, hash or address from the user's copy anywhere in code, docs, tests, logs or the report. Measured *shapes*, constants of the interface, counts and outcomes only. |
| State limits | In the code near the behaviour and in the doc: what the slice does not do, what is a hypothesis, what no call site measures. A limitation named now is a planned next slice; one left implicit becomes a silent assumption. |
| Measure the effect | One A/B pair of boots (standard environment, boot lock, one at a time, `tooling/boot_report.py`). Report the counters that would move if the slice worked (or would prove it did not), the presented/captured frames, and a plain verdict: advanced, unchanged, or regressed. |
| Keep the gates green | `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, the slice's own test passes, and the docs checks (`tests/test_docs_*.py`) pass. |
| Leave the commit to the lead | Agents make no git writes. The slice ends with the uncommitted file list and the exact measurement table; the lead commits it through the normal flow. |

## 4. How a slice is scoped

- One subsystem per slice. New files plus the smallest registration change; the default path keeps working if the
  slice is reverted.
- The slice does not edit files another slice or agent owns. A `writeScopes` overlap is handled by the lead before
  work starts; if it appears mid-slice, the narrower owner keeps the file.
- A slice that needs a switch (an opt-in path) starts with the switch off and says what would make it default. A
  slice that repairs an unimplemented import (serves it honestly) may be on by default only when the shim is the
  honest answer and the A/B shows no regression; the limits are still documented.
- Slices are ordered by measured value, not by what the reference did first. The next slice's doc section names the
  falsifier that would justify it.

## 5. How to write the slice document

The doc is what survives. It has these sections, in this order:

1. **Why these imports/calls are needed** - the boot evidence that makes the slice worth doing.
2. **Measured shapes** - a table per interface: call sites, RVAs (game+0x...), argument registers, the memory each
   site reads or writes, and how the result is used. RVAs are stable per executable version and are how the next
   reader re-checks a claim.
3. **What we implemented** - the design in our terms, including the cases where the console contract is unknown and
   the shim picks the conservative answer.
4. **Cross-check against the reference** - file/commit, what agrees, what differs, why.
5. **A/B measurement** - one table with the before and after boot numbers, the exact environment/switches, and the
   verdict. Note anything that did *not* move, including the loading state.
6. **Limits** - the named gaps, and which one is the next falsifier.
7. **Tests** - target name, what it covers, the check count.
8. **Attribution** - the upstream credit, repeated from `NOTICE.md`.

## 6. Attribution template

```text
This slice was implemented from this project's own measurements. The behaviour was cross-checked against the
public GPL-3.0 project ubiquitarianism/leonida (<license URL>), files <paths>, commits <hashes> (<authors>).
No code from that project was copied. The upstream project remains the copyright of its contributors and its
license continues to apply to its own tree.
```

Put the filled template in [NOTICE.md](../NOTICE.md) and a short pointer in the slice doc.

## 7. What this policy is not

- It is not a claim that the upstream project is wrong: it is a different codebase with its own measured state, and
  several of its hypotheses are the best current explanation (its later `XApu`/`XDsp` revision and the silent WASAPI
  endpoint are named in [audio_shims.md](audio_shims.md)).
- It is not permission to paste GPL code into this tree; both trees are GPL-3.0, but the policy is an independent
  implementation so the provenance of every line stays clear.
- It is not a promise that a slice advances the boot. A slice that removes five unimplemented calls and leaves the
  loading state exactly where it was is a good slice, and its doc says so.
