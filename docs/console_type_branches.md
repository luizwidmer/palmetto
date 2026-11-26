# The console-type branch: the ConsoleMode map, the one cached Series-S flag, and the Series-X arm

**Status: measured static, plus boot arms.** This page answers the picture question that the class-0xa work left
open: is the class-0xa type-5 LUT command a PC / unknown-console-type fallback? It decodes the title's console-type
value map end to end, lists every consumer of the cached value, extends the opt-in override
(`PALMETTO_CONSOLE_TYPE`, added by [decomp_renderer.md](decomp_renderer.md) section 11) from "6 only" to the measured
values, and boots the Series-X arm against a same-binary control. The switch is off by default and the default path
does not change. No game string, asset byte or hash is carried in the tree, the logs or this page.

## 1. Why this question needs its own page

The type-5 constant-table family builds a 256x1 class-0xa record, asks the title's own allocator for `0` bytes and
then copies `0x1000` bytes into it; the overrun hits the live command object and the boot stops in the title's
executor ([vertex_buffer_va.md](vertex_buffer_va.md) sections 15-16, [decomp_renderer.md](decomp_renderer.md)
section 4.5). The upscaler selector takes the console FidelityFX branch (kind 7) only for console type 6, and every
other type - including the PC answer 1 - takes kind 6 ([decomp_renderer.md](decomp_renderer.md) section 5). A real
Series X runs the same image and the same kind-6 path, and it does not crash, so the question is whether the
class-0xa table is a fallback that only the PC / unknown answer selects. Section 2 decodes the map and its consumers
statically, section 3 extends the switch, and section 4 measures the Series-X arm and a same-binary control.

## 2. The measured console-type chain (static)

All rows are the retail image with the image base `0x140000000`; RVAs are relative to it. **M** marks a static
measurement (decompiled or disassembled instruction), **I** an inference the page does not resolve further.

### 2.1 The read and the value map

The title's console identity is one host registry read: `RegOpenKeyW` on `HKEY_LOCAL_MACHINE` with the image's own
subkey string, `RegQueryValueExW` of the image's own value name, `RegCloseKey`. The whole map is the body of
`game+0x55DEB88`; the three import call sites are `0x55DEBB2` (open), `0x55DEBF2` (value), `0x55DEBF9` (close), and
the compares are `0x55DEC07`..`0x55DEC74`.

| registry dword | console type | evidence |
|---|---:|---|
| absent key / failed open | 1 | `0x55DEBB2` tests the open result and `jne 0x55DEC76` returns 1 (**M**) |
| `0x60` | 6 | `0x55DEC46` compares, `0x55DEC61` returns 6 (**M**) |
| `0x64` | 7 | `0x55DEC48` compares, `0x55DEC5A` returns 7 (**M**) |
| `0x68` | 8 | `0x55DEC4D` compares, `0x55DEC50`/`0x55DEC55` returns 8 or 0 (**M**) |
| `0x58` | 5 | `0x55DEC3E` compares, `0x55DEC68` returns 5 (**M**) |
| `0x50` | 4 | `0x55DEC39` compares, `0x55DEC6F` returns 4 (**M**) |
| `0x40` | 3 | `0x55DEC07` compares, `0x55DEC32` returns 3 (**M**) |
| `0x10`, `0x20`, `0x21`, `0x30`, `0x31` | 2 | the subtract chain `0x55DEC0E`..`0x55DEC25` falls into `0x55DEC2B` (**M**) |
| any other present value | 0 | the default path at `0x55DEC27` returns 0 (**M**) |

The read is not cached itself: every call redoes the registry pair. What is cached is one boolean, described next.

### 2.2 The cached flag and the getter

`game+0x75140` (`.pdata` range `0x75140..0x751A2`) is the only cached form of the type. It is an MSVC thread-safe
function-local static:

- a guard dword at `game+0x71B63B4`, initialised through the two CRT-shaped helper calls at `0x75177`/`0x7519B`
  (`0x55DB8CC` and `0x55DB934`; that they are the static-initialisation header/footer pair is **I**);
- one cached byte at `game+0x71B63B0`, written as `sete` of `(registry read == 6)` at `0x75194` and returned at
  `0x75165`; the read itself is the call at `0x75185`;
- so the cached value is **"console type is 6", not the console type** (**M**).

The measured value words are in the image's `.data` zero-fill tail (`0x71B63B0` and `0x71B63B4`); the file bytes at
`0x7B63B0` are `.text` padding, so the RVA [decomp_renderer.md](decomp_renderer.md) section 5 quotes for this cache
(`0x7B63B0`) has a digit missing - the cache is at `0x71B63B0`. The Series-S slice's own work-folder arm script
already used the right address (`PALMETTO_WATCH_GLOBAL=71B63B0`); this page records the correction and section 4
reads the byte.

A whole-image scan of direct `call`/`jmp` encodings finds **70 sites in 61 distinct functions** that call the getter
(section 2.4). It is the title's only "is Series S" test.

### 2.3 The exact-type consumers: three sites

A whole-image scan finds exactly three direct calls of `game+0x55DEB88`; two of them are the getter and the XMem
initialiser, and the third is a debug-print path.

| site | function | measured behaviour |
|---|---|---|
| `0x75185` | `0x75140` | the cached flag (section 2.2) (**M**) |
| `0x1E1CF84` | `0x1E1CF60` | XMem initialiser: `cmp eax,8; ja 0x1E1D574` then `mov ecx,0x1C2; bt ecx,eax; jae 0x1E1D574` - the bitmask accepts exactly types **1, 6, 7 and 8** and traps `int3` at `0x1E1D574` for 0 and 2..5; the type register is then overwritten and not stored. The next call, the getter at `0x1E1CFB0`, selects the VA-window size `((is_not_series_s) OR 2) << 33` = **24 GiB for 1/7/8 and 16 GiB for 6** (**M**; the type-3 light boot of section 4.1 measures one `int3_skipped` at exactly `game+0x1E1D574`) |
| `0x55FEAF6` | `0x55FE642` | a CRT/diagnostic path that formats the read result into a string (the same function builds a message with `OutputDebugStringA` and the CRT string helpers); not in the renderer (**M** for the call and the string formatting, **I** for its purpose) |

The bitmask is a measured cross-check of the numbering itself: the build accepts the PC answer plus the three
Scarlett answers, and cannot represent the Xbox One family (2..5) as a supported configuration.

### 2.4 The consumer census of the cached flag

Each row is a distinct function among the 61 that call the getter; the right column is the value or decision that
changes between "is Series S" (true) and everything else (**M** unless marked **I**).

| consumer | measured change when the flag is true |
|---|---|
| `0x1E1CED0` | copies one of two `0x50`-byte records from the image table at `0x6186258` into the XMem initialiser's 9-slot size table `0x71BF9B0..0x71BF9F0`: the Series-S record is the first, the record at `+0x50` is the one 1/7/8 use. The shared record holds the four pool sizes the committed boot measured (`0x49400000`, `0x89700000`, `0x23300000`, `0x25400000`), so this is the writer of the pool-size table [decomp_memory.md](decomp_memory.md) section 6 could not find; with the flag true and a predicate set, one pool size loses `0xDC00000` (220 MiB) and one 16-byte value is adjusted |
| `0x1E1CF60` | the 16 GiB VA window and the membership trap of section 2.3 |
| `0x2479690` | five memory-budget globals: `0x73F00000`/`0xC800000`/`0x4000000`/`0xF600000`/`0x185C00000` become `0x40500000`/`0x6400000`/`0x1000000`/`0x6000000`/`0xF8900000` |
| `0x247AD10` | a scratch/budget block of `0x4000000` (non-S) or `0x5200000` (S), followed by an `int3` if the block is under `0x200000` |
| `0x75420` | `0x4000000` (non-S) or `0x2800000` (S) |
| `0x2216BF0` | `0` (non-S) or `0xDC00000` (S) |
| `0x5B52510` | job count `0x1E` (non-S) or `0x18` (S) and a budget of `0x1E00000` or `0xA00000` |
| `0x56FE410`, `0x57000E0` | worker counts `0x14`+`0x32` (non-S) or `0x0A`+`0x19` (S) and the per-worker footprints `0x1000`/`0x4000` |
| `0x1D488A0` | `0x2C60` (non-S) or `0x1290` (S), shifted left 16 and passed to a pool call |
| `0x5B83200` | `0x251C0` (non-S) or `100000` (S) |
| `0x554F7C0` | `0x8000` (non-S) or `0x1800` (S) |
| `0x5D425C0` | `0x1800` (non-S) or `0xC00` (S) |
| `0x4EE01B0` | `0x4000` (non-S) or `0x2000` (S) |
| `0x56F5D50` | `10000` (non-S) or `5000` (S) |
| `0x5B286D0` | `0x2000` (non-S) or `0x1400` (S) |
| `0x25A8750` | `0x1C7` (non-S) or `0xFF` (S) |
| `0x501AD20` | `2` (non-S) or `1` (S) |
| `0x1FB4300` | returns the float `1.0` (S) or `0.6666667` (non-S); it feeds the upscaler object's scale fields |
| `0x28F1E70` | kind-6 object slot `0x48`: sets `+0x90 = 0.6666667` and `+0x94 = max(getter-scaled float, 0.6666667)` |
| `0x28EF680` | kind-7 object slot `0x50`: scales the upscale input by `0.6666667` when the flag is true, else by `1.0` |
| `0x314E740` | builds a stack resource description whose packed words are `0xF00`/`0x870` (3840x2160) and whose byte field is `0x1C` or `0x15`, then compares a queued record against it on the non-S path (**M** for the words; the field names are **I**) |
| `0x15EEE50`, `0x3E571F0`, the other settings-default functions | per-console setting and quality defaults (**M** for the flag read and the differing tables; the setting names are **I**) |

The consumers not individually named above are `0x31FA50`, `0x41BC50`, `0x67E530`, `0x844AF0`, `0x89D7A0`,
`0x8DFE30`, `0x9617E0`, `0xCC38A0`, `0x1002700`, `0x13AF6E0`, `0x1943DA0`, `0x1A42F00`, `0x1A75CD0`, `0x1DF5040`,
`0x1E74390`, `0x1EDB000`, `0x1F67680`, `0x220D7D0`, `0x2228FD0`, `0x2240DA0`, `0x25E1600`, `0x27CC700`,
`0x2BC2F80`, `0x325D2A0`, `0x325E740`, `0x3774630`, `0x402A770`, `0x4879860`, `0x50992F0`, `0x53C7730`,
`0x546D670`, `0x577AFA0`, `0x580F960`, `0x5821AE0`, `0x5C0CAB0`, `0x5C8F630` - all **M** as flag call sites; their
individual decisions were not decoded past the read, and none of them is in the class-0xa producer chain below.

The upscaler side is the branch [decomp_renderer.md](decomp_renderer.md) section 5 maps: the selector `0x7E93F0`
returns 7 only when the flag is true and the capability word's `0x40` bit is set, the factory `0x7E93B0` returns the
kind-6 object `0x7B99040` or the kind-7 object `0x7B98E08`, and the dispatch `0x7E92F0` calls the object's slots
`0x48` (set the scale fields), `0x50` and `0x58` (compute two output sizes from input sizes). The kind-6 vtable
`0x6300BF0` has `0x28F1E70`/`0x7E9440`/`0x7E9380` at those three slots; the kind-7 vtable `0x6300B50` has a `ret`
stub, `0x28EF680` and `0x28EF6F0`. Kind 7 therefore always runs at 2/3 of its input size and copies the output size
(the Series-S FSR shape the Series-S slice measured), and kind 6 runs the title's own upscaler at its own two scales.

### 2.5 What the console type cannot change

Two measured negatives decide the picture question before any boot:

1. **No consumer compares the type against 1, 7 or 8.** The three exact-type sites of section 2.3 do not branch on
   the value beyond the `{1,6,7,8}` membership test; every other consumer reads the single boolean, which is false for
   1, 7 and 8 alike. PC (1), Series X (7) and the devkit (8) are therefore statically indistinguishable in the title.
2. **The class-0xa producers are not console-type consumers.** The direct-call census of the 61 getter functions
   contains none of the type-5 LUT chain - the factory `0x1851D0`, the table-set emitter `0x45629A0`, the 0x1000-byte
   copy `0x550E2D0`, the ramp converter `0x13368F0`, the constant-table helper `0x45594F0` or the row-stride switch
   `0x1555C10` - and an indirect call into that chain from a getter consumer was not found (**M** for the direct
   call graph; **I** for the absence of an indirect edge).

So the class-0xa command cannot be selected by the console type: its class is a compile-time word of the image's own
format table ([vertex_buffer_va.md](vertex_buffer_va.md) section 16), and the only title-wide consumer of the type is
the Series-S boolean. Section 4's prediction is therefore "the Series-X arm reproduces the control boot", and its
falsifier is a measured difference.

### 2.6 Which hardware the measured values are

The title's numbering is the GDK `XSystemDeviceType` numbering, measured by the membership mask and cross-checked
against the public reference port's measured `ConsoleMode` decoding (0x60 Series S, 0x64 Series X, 0x68 devkit on
the same retail package; see section 8):

| console type | hardware | ConsoleMode dword |
|---:|---|---:|
| 1 | PC (the absent key) | none |
| 2 | Xbox One family | `0x10`, `0x20`, `0x21`, `0x30`, `0x31` |
| 3 | Xbox One S | `0x40` |
| 4 | Xbox One X | `0x50` |
| 5 | Xbox One X devkit | `0x58` |
| 6 | Xbox Series S (Lockhart) | `0x60` |
| 7 | **Xbox Series X (Anaconda)** | `0x64` |
| 8 | Scarlett devkit | `0x68` |

The Series-X candidate is therefore **7**, not 8: 8 is the Scarlett devkit, and the title treats 7 and 8 as equal
everywhere (section 2.5). The Series-S answer 6 is the only one that changes a renderer branch.

## 3. What we implemented: the switch extension

`port/src/pe_loader.cpp` and `port/src/pe_loader.h` change only the switch and its answer. The registry hooks, the
two image string RVAs, the plausibility check, the forwarding of every other registry call and the default path are
unchanged; the hooks now answer a value stored in the armed answer object instead of the one Series-S constant.

| switch spelling | answered ConsoleMode | title console type |
|---|---:|---:|
| unset, empty, `0`, `off`, `1`, `pc` | none (the default path) | 1 |
| `2` | `0x10` | 2 |
| `3` | `0x40` | 3 |
| `4` | `0x50` | 4 |
| `5` | `0x58` | 5 |
| `6`, `seriess`, `series-s`, `series s` | `0x60` | 6 |
| `7`, `seriesx`, `series-x`, `series x` | `0x64` | 7 |
| `8`, `devkit`, `scarlett-devkit` | `0x68` | 8 |

The parse is strict: whitespace around the value is tolerated, the numeric spellings must be one digit `2`..`8` (so
`07` and `+7` are refused), and any other word prints one line and leaves the default path. Type 1 is accepted as
"off" because the only way the title's read answers 1 is a failed open, i.e. no answer; type 0 (a present but
unknown ConsoleMode) is not a value an arm can ask for, because no measured device produces it. Two new pure helpers
carry the map both ways - `console_type_registry_value(type)` and `console_type_from_registry_value(dword)` - so the
hooks, the log line and the tests agree on one table. The log line now states the answered dword and the type the
title's map derives from it.

## 4. The boots

One boot at a time under `_setup/boot_lock.ps1`, the lock released after every boot; logs and captures stay outside the
repository. Two probe sets are used, and the split is deliberate: the **poke arms** (50-60 s) put one
`PALMETTO_POKE_LOG` point on the upscaler selector's return (`image+0x7E9429`) and read the cached-flag qword with
`PALMETTO_WATCH_GLOBAL=71B63B0`, and carry **no** alloc-watch poll thread or captures; the **stage-3 arms** (180 s)
use the section-12 content environment with `PALMETTO_ALLOC_WATCH=1`, the `PALMETTO_DUMP_REGS`/`PALMETTO_FAULT_MEM`
diagnostics and presenter captures, exactly as [decomp_renderer.md](decomp_renderer.md) section 11.3. The selector
question does not need the heavy poll, and this page keeps the heavy arms to the class-0xa window only.

The shared tree was being rebuilt by the concurrent Series-S FSR slice while these arms ran, and `palmetto.exe`
changed three times between 11:36 and 12:04 (`63843B22...`, `B8F3F880...`, `56C937CF...`). To keep the comparison
honest, every arm after the first poke boot runs a frozen copy of `56C937CF...` (`port/build/palmetto-ct56c937.exe`,
next to its `palmetto_tls_host.dll`); the binary column names it. The light and the frozen stage-3 arms all ran that
one copy.

### 4.1 The poke and light arms: the answer is consumed, the kind does not move

| arm | binary (first 8) | switch | reads answered | selector RAX (hits) | cached flag byte | FidelityFX module | wall | D3D12 calls |
|---|---|---|---|---:|---:|---|---:|---:|
| `x7pk1` | `63843B22` | `7` | `0x64` x4 | **6** (19/19) | 0 | none | 60.4 s | 84,855 |
| `l7pk` | `56C937CF` | `7` | `0x64` x4 | **6** (19/19) | 0 | none | 50.2 s | 59,318 |
| `lctl1` | `56C937CF` | none | - | **6** (19/19) | 0 | none | 50.2 s | 65,946 |
| `l81` | `56C937CF` | `8` | `0x68` x4 | **6** (19/19) | 0 | none | 50.2 s | 72,987 |
| `l31` | `56C937CF` | `3` | `0x40` x4 | **6** (19/19) | 0 | none | 60.2 s | 83,389 |
| `l6pk` | `56C937CF` | `6` | `0x60` x4 | **7** (17/17) | **1** | not in 50 s | 50.2 s | 61,499 |

- **Every answer is consumed.** Each armed boot prints four answer lines: the title's read pair (open + query) twice
  per boot, both answered with the switch's dword - the same shape section 11.4 of [decomp_renderer.md](decomp_renderer.md)
  recorded for Series S.
- **The cached flag is the Series-S bit, measured both ways.** The qword at `0x71B63B0` reads
  `0x8000000100000000` in the control and in the type-7/8/3 arms - the low dword is the cached boolean 0, its `+4`
  dword the static-init guard `0x80000001` - and `0x8000000100000001` in the type-6 arm on the same binary, i.e.
  flag 1. Only the Series-S answer can set it.
- **The upscaler kind moves only for Series S.** The selector returned 6 on all 19 hits of the type-7 arm (and of
  the control, the devkit answer 8 and the unsupported answer 3), and 7 on all 17 hits of the type-6 arm on the same
  binary. No non-S boot maps `amd_fidelityfx_upscaler_xbox.dll`; the committed Series-S stage-3 arms do
  ([decomp_renderer.md](decomp_renderer.md) section 11.4).
- **Type 3 is an XMem assert, not a renderer branch.** The type-3 light boot logs one extra `int3_skipped` event at
  `game+0x1E1D574` - exactly the membership trap of section 2.3 - and then continues: under `PALMETTO_SKIP_INT3=1`
  the trap is stepped over and the boot stays alive with 83,389 D3D12 calls. The older-console codes are therefore a
  support assertion in the XMem initialiser, not an alternate renderer path.
- **Type 8 is the devkit, not Series X.** It is consumed as `0x68`, leaves the same false flag and the same kind 6 -
  indistinguishable from type 7, as section 2.5 predicts.

### 4.2 The stage-3 arms: the class-0xa sequence appears under the Series-X answer

The class-0xa sequence is the measured set `FORMAT KEY` (key 0xa) -> `BLOCK ALLOC` (`0x0` bytes, 256x1) ->
`ENTRY WRITTEN` (stride 0, size 0) from [vertex_buffer_va.md](vertex_buffer_va.md) sections 15-16, read through
`PALMETTO_ALLOC_WATCH`. The stage-3 window is the trace time from the probe's own "stage 3 read" to the stop.

| arm | binary (first 8) | switch | env | stage 3 | class-0xa sequence | stop | stage-3 window | presenter captures | device lost |
|---|---|---|---|---|---|---|---|---|---|
| committed control `def3`, cited | `D7B4D897` | none | content | yes | FORMAT KEY / BLOCK ALLOC / ENTRY WRITTEN | `game+0x13360A3` read -1 | - | 10, black | no |
| `x7fb1` | `B8F3F880` | none | content | yes | FORMAT KEY / BLOCK ALLOC / ENTRY WRITTEN | `game+0x13360A3` read -1 | 4.04 s | 12, black | no |
| `ctlf2` | `56C937CF` | none | content + fallbacks | yes | FORMAT KEY / BLOCK ALLOC / ENTRY WRITTEN | `game+0x55AA4E6` write `0x3F8000003F98` | 3.82 s | 10, black | no |
| `x7b1` | `56C937CF` | `7` | content | yes | FORMAT KEY / BLOCK ALLOC / ENTRY WRITTEN | `game+0x55AA4E6` write `0x3F8000003F98` | 3.89 s | 12, black | no |
| `x7f1`, `x7f2`, `x7f3` | `56C937CF` | `7` | content + fallbacks | no x3 | none in the window | `game+0x86F97E` / `game+0x43ADFAD` / `game+0x86F981` | - | 12 / 9 / 12, black | yes x3 |

- **The Series-X answer reproduces the class-0xa command.** `x7b1` creates the same FORMAT KEY (key 0xa, command
  vtable `game+0x66314B0`, creator vtable `game+0x658DB60`), the same `0x0`-byte BLOCK ALLOC for a 256x1 entry and
  the same ENTRY WRITTEN with stride 0 and size 0, 2.92 s / 2.95 s / 2.99 s after its stage-3 read.
- **The same-binary control stops at the same site.** `ctlf2` (no console-type answer, frozen binary) and `x7b1`
  (type 7) both stop at `game+0x55AA4E6` writing `0x3F8000003F98`, the known "structure filled with 1.0f used as a
  pointer" family [vertex_buffer_va.md](vertex_buffer_va.md) section 18.5 records for the console-feed arm. Their
  windows agree (3.82 s against 3.89 s) and the class-0xa step times agree within 0.2 s. `x7fb1`'s older
  `game+0x13360A3` stop is another member of the same class-0xa family on a different binary; the committed control
  row has it too.
- **The honest negative: three type-7 fallback arms did not reach stage 3.** All three were consumed (`0x64` x4),
  lost the Vulkan device and stopped in the early families (`0x86F97E`, `0x43ADFAD`, `0x86F981`); the stage-3 reach is
  the environmental race the class-0xa pages record (2 of 3 control boots in [decomp_renderer.md](decomp_renderer.md)
  section 11.5, and here 2 of 2 frozen control arms needed one retry). None of the three says anything about the
  class-0xa command, and the type-7 arm that did reach stage 3 created it.
- **Pixels unchanged.** Every presenter capture in every arm is mean RGB 0.0, black; the non-S arms count 10-12
  captures each. No arm lost the device in its stage-3 window.

## 5. Verdict and the next falsifier

**The Series-X candidate is console type 7, not 8**, and answering it changes no measured branch: the read is
consumed as `0x64`, the cached "is Series S" flag stays false (the type-6 answer on the same binary sets it),
the upscaler selector returns 6 on every hit (the type-6 answer on the same binary returns 7), the class-0xa
command is created in the same sequence and the same-binary control stops at the same site. Type 8 is the Scarlett
devkit and behaves identically; the only console type that moves a renderer branch is 6 (Series S), whose kind-7 FSR
branch this layer cannot yet run.

**The class-0xa crash is not a PC / unknown-console-type fallback, as far as this measurement goes.** Three results
carry the verdict: the only consumer of the console type is the Series-S boolean plus the XMem membership mask
(section 2.5), the type-5 LUT chain reads neither (section 2.5), and the Series-X arm - the answer a real Series X
gives - creates the same class-0xa command and stops exactly where the control does (section 4.2). The typecast of
the title's PC answer as 1 is real, but 1 and 7 share the same branch; there is no fallback branch behind it.

The console-hardware difference must then be elsewhere, and the candidate is the class-0xa block's *victim*, not the
type: the title asks its own allocator for 0 bytes and copies `0x1000` bytes into whatever follows, so the same
compile-time mismatch can survive on a console whose allocation history places the zero-sized block next to harmless
free-space metadata ([decomp_memory.md](decomp_memory.md) section 9). The next falsifiers, in order: (a) a boot whose
allocation history matches the console path (the Series-S FSR survival slice's goal) that reaches the same executor
pass - if the class-0xa command appears there and the copy does not hit the live job, the victim layout is the
differentiator; (b) the title-side row-stride switch patched to answer `0x100` for class 0xa
([decomp_memory.md](decomp_memory.md) section 10, H8) - it proves the mechanism end to end; (c) a hardware trace of
the same switch on a retail console, which this tree cannot provide. If any type-7 boot ever diverges from a
same-binary control in this window, this verdict re-opens.

## 6. Limits

- **One image, one machine.** The map is the retail build's; a re-import or a different build moves every RVA.
- **The controlled comparison is one frozen binary, not a clean tree.** The concurrent Series-S FSR slice's working
  tree changes were in every build these arms ran; the frozen copy keeps the *comparison* honest (control and
  Series-X arm share it), but it is not the committed tree's binary.
- **Single-boot samples for the class-0xa window.** One stage-3 type-7 boot, two frozen control stage-3 boots and the
  committed control row carry section 4.2; the stop families move between runs of the same arm (the committed row's
  three control boots stopped at three different sites), so the sequence - not the stop site - is the stable measured
  quantity.
- **The type-3 boot measures a skipped trap.** `PALMETTO_SKIP_INT3=1` steps over `0x1E1D574`; that a hardware boot
  would instead take the trap and stop is **I**, not measured.
- **The hardware names are a cross-check, not a measurement.** The type numbers and their hardware names come from
  the public reference's measured `ConsoleMode` decoding and the GDK enumeration; this page measures the title's own
  map into those numbers and the `{1,6,7,8}` membership mask, not the consoles themselves.
- **Indirect calls are not in the census.** The three exact-type sites and the 61 flag consumers are direct
  `call`/`jmp` encodings; a type-dependent behaviour reached only through a function pointer would be missed.
- **The boot arms are a race.** Stage-3 reach is the environmental race the class-0xa pages record; an arm without
  stage 3 says nothing about the command, and the three type-7 fallback arms that missed it are reported as such.
- **The arms do not prove console hardware behaviour.** They show what this port's title does with each answered type;
  a real console also differs in GPU memory, driver and page tables, which this page does not model.
- **No game data.** RVAs, registry dwords, console types, sizes, counts and outcomes only; the two registry strings are
  read from the user's own image at run time and never written to the tree.

## 7. Tests and gates

- `port/test/pe_loader_test.cpp` extends the console-type checks from 27 to 49 (128 checks in the target, up from
  106): every accepted spelling, the refused spellings (`9`, `07`, a word), the `1`/`pc` default-path answers, the
  ConsoleMode map in both directions, and the armed answer's dword/type agreement. No new target or source file.
- The default path is covered by the switch-off checks: with `PALMETTO_CONSOLE_TYPE` unset the IAT is never patched.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints
  `TESTS_BUILT`, `pe_loader_test` passes, and the `tests/test_docs_*.py` checks pass.

## 8. Attribution

This page was assembled from this project's own measurements: the disassembled and decompiled functions
`game+0x55DEB88`, `game+0x75140`, `game+0x1E1CF60`, `game+0x55FE642`, `game+0x7E93F0`, `game+0x7E92F0`,
`game+0x7E93B0`, `game+0x876110`, the two upscaler vtables `0x6300BF0`/`0x6300B50`, the consumer census and the boot
arms above. The hardware names of the console types were cross-checked against the public GPL-3.0 project
ubiquitarianism/leonida (its runtime-imports page, "Console identity in the registry", and its runtime README), which
measured the same retail package's `ConsoleMode` values; no code or text was copied from it and its licence continues
to apply to its own tree. The project-wide attribution row in [NOTICE.md](../NOTICE.md) is unchanged. No git writes
were made; the uncommitted files at the end of the slice are `port/src/pe_loader.h`, `port/src/pe_loader.cpp`,
`port/test/pe_loader_test.cpp` and this page.
