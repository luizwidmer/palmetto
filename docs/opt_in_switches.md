# Opt-in switches: the PALMETTO_* environment registry

Every behaviour change in this port is opt-in: a switch is read once from the environment, and with it unset the
runtime takes the legacy path. This page is the round-21 audit of that surface: the switch set is derived from the
source (not from prose), each switch is classified by the parser idiom its call site actually uses, and the wiring
is measured as *include reachability* from a compiled port translation unit. It also names every place where the
wiring is absent or inconsistent, with the exact fix.

Measured against `D:\leonida` at round-21 start (HEAD `2785e429e`, tree clean): **294 switches** in `port/src` and
`port/shims` plus one non-switch `PALMETTO_*` literal, **292 include-reachable** and **2 test-only** (the census header became wired when round-21 T1 applied its `#include` to `pe_loader.cpp`, and query-heap readiness was wired at `CreateQueryHeap` by T15; this table is measured against the round-21 integration tree).
`port/test/opt_in_switches_test.cpp` re-derives the same set, sites and reachability from the source and fails when
this page's table drifts; it is registered as CTest `opt_in_switches` in `port/CMakeLists.txt` (stanza under
"Registered target" below).

## Method

1. **Name set.** Scan every `.h`/`.cpp`/`.c` under `port/src` and `port/shims` for string literals that are exactly
   `PALMETTO_[A-Z0-9_]+`. A literal with other characters (a message such as `"PALMETTO_X=1 ..."`) is not a switch.
   Exactly one literal is excluded as not read from the environment: `PALMETTO_AGGREGATE_DEVICE_ID`
   (`port/shims/gameinput.cpp:2799`, a synthetic device-id payload copied with `memcpy`).
2. **Parser kind.** The first read site of each switch is classified by the expression that consumes it. The scan is
   mechanical (helper map plus the comparison/parse idiom in the following lines) and the sixteen rows that spell the
   name through an intermediate constant were read by hand. `kind` is what the site does, not a recommendation.
3. **Wiring.** Build the `#include` graph of `port/src` + `port/shims` and mark a site file *wired* when a `.cpp`/`.c`
   in either root reaches it transitively. Four headers are reached only from tests, so their parser is compiled and
   tested but no runtime call site exists.

Limits: include reachability is a static property. It does not prove the containing function executes on a boot, and
it cannot see a switch whose value is parsed and then dropped inside a function that is reached. The 4 unwired rows
are the complete static answer; execution-time coverage needs a boot with coverage instrumentation and is the next
falsifier (see "Falsifiers").

## Status by parser idiom

| Kind | Rows | Enabled when | Unset default |
| --- | ---: | --- | --- |
| `exact_one` | 26 | the value is exactly `1` (`true`, `01`, ` 1` stay off) | off |
| `switch_on` | 89 | the value is non-empty and its first character is not `0` (`01` and `0x` are off) | off |
| `text_switch` | 6 | the value is not empty and not `0`/`off`/`no` (case-insensitive) | off |
| `any_set` | 82 | the variable is present at all; a present-but-empty value counts as ON | off |
| `value_int` | 49 | n/a: a decimal value with a caller fallback; unset or invalid keeps the legacy value | not consulted |
| `value_string` | 39 | n/a: a non-empty string value with a caller fallback; unset or empty keeps the legacy value | not consulted |
| `disable_value` | 3 | the value is not `0`/`off`/`no` (documented default-on switch) | **on** (default-on) |

All seven kinds are implemented as the pure reference parser `rt::opt_in_switches::flag_parse` in
`port/src/opt_in_switches.h` and exercised for unset, empty, `0`, `1`, `01`, `off`, `no`, `true`, whitespace and
invalid text. Default behaviour is unchanged: unset is off (or the caller's fallback) for every row except the three
documented default-on switches.

## Findings: dead or inconsistent wiring

### F1. Two switches are parsed and tested but never reached by a runtime call site (round-20/21 pure slices)

| Switch | Site | State |
| --- | --- | --- |
| `PALMETTO_D3D12_RESIDENCY_BUDGET` | `port/src/d3d12_residency_budget.h:22` | include-reachable from tests only |
| `PALMETTO_UI_INPUT_SCRIPT` | `port/src/ui_input_script.h:38` | include-reachable from tests only |

These are deliberate pure slices: `d3d12_residency_budget.h` (P-58 contract) and
`ui_input_script.h` (P-02 input contract). Two rows have left this list in round 21: `loader_census.h` (P-57 census)
when T1 added its include to `pe_loader.cpp`, and `query_heap_readiness.h` when T15 wired its preflight into
`d3d12_vulkan_device.cpp` `CreateQueryHeap` (F1-follow-up below). Their parser is correct and tested; what is missing
is the insertion hunk in the owner file. Fix (needs the file owner's approval, so not done in this audit): include the
header and call its `enabled()`/policy entry point from the named owner:

- `PALMETTO_D3D12_RESIDENCY_BUDGET`: construct the `BudgetSet` where the layer registers pageable residency.
- `PALMETTO_UI_INPUT_SCRIPT`: call the script loader from the `PALMETTO_FIRST_RUN_SCRIPT` path in
  `port/shims/gameinput.cpp` (the script parser is wired; this exact-value gate is not).

None of these changes the default path: each parser returns off/legacy when unset.

### F1-follow-up: PALMETTO_D3D12X_QUERY_HEAP_READINESS is wired (T15)

The preflight now runs in the layer's real creation path, `port/shims/d3d12_vulkan_device.cpp` `CreateQueryHeap`
(just before the `new QueryHeap` allocation). It is reached only through the exact `1` switch; unset performs no
environment read and runs the original code byte-identically. When set, the measured contract in
`port/src/query_heap_readiness.h` is applied with the live capabilities (`timestamp_period()`, queue-family
`timestamp_valid_bits`, `pipeline_statistics_query`, `occlusion_query_precise`, and `stream_output_available=false`):

- `Action::refuse_invalid_argument` -> the existing host-order `E_INVALIDARG` (count/node/type), after the standard
  checks; the stderr line is `[rt] d3d12: query heap readiness refused type <t> count <n> node <m>: <reason>
  (PALMETTO_D3D12X_QUERY_HEAP_READINESS)`.
- `Action::refuse_not_implemented` -> `E_NOTIMPL` **before** any `new QueryHeap` allocation, with the same named
  reason: video-decode statistics, unnamed type 6, PIPELINE_STATISTICS1, and any type whose measured capability is
  absent (timestamp, pipeline statistics, precise occlusion). This is the same refusal the existing
  `QueryHeap::create` returns for those types, now named and issued earlier; stream-output statistics stays
  `ready_zero` and is created as before.
- `Action::proceed` -> the existing path, unchanged.

The pure mapping (`action_of`, `is_refusal`, `action_name`) is covered by `query_heap_readiness_test`
(8 synthetic cases; the new case walks every disposition and requires a named reason for each refusal). No GPU, no
game data, no default-path change.
### F2. A present-but-EMPTY value turns on the 82 `any_set` switches, but not the other flag families

The tree has four flag idioms that disagree at the empty string. `any_set` sites test only that the variable exists
(`_dupenv_s(...) == 0 && value`), so `PALMETTO_X=` **enables** them while `PALMETTO_X=` leaves `exact_one`,
`switch_on` and `text_switch` rows off. Effects: an empty variable is indistinguishable from a set one for those 82
rows; the default path is still byte-identical because an unset variable is still off.

Rows: all 82 `any_set` rows in the table below. Example sites: `port/shims/xbox_present.cpp:57`
(`PALMETTO_PRESENT_PER_ORIGIN`), `port/src/main.cpp:1492` (`PALMETTO_EXECUTOR_WATCH`),
`port/src/main.cpp:9955` (`PALMETTO_SHADERS`). Exact fix (no behaviour change until approved): in the shared reader,
treat an empty value as unset, e.g. `_dupenv_s(...) == 0 && value && value[0]` instead of `&& value`. Applying it
would change what an empty variable does, so it is reported, not applied.

### F3. `switch_on` rows treat `01`, `0x` and every spelling beginning with `0` as OFF

`switch_on`/`read_flag` test only the first character (`value[0] != '0'`), so `PALMETTO_X=01` and `PALMETTO_X=0x1`
are off while `PALMETTO_X=1 ` (trailing space) is on. 89 rows. Example: `port/src/queue_holds.h:31`,
`port/shims/xg_xs.cpp:213`. This is the documented measured behaviour of those sites; recorded as a spelling
contract in the table rather than changed.

### F4. Three documented DEFAULT-ON switches are not opt-in

`PALMETTO_RUMBLE`, `PALMETTO_GAMEINPUT_THREAD` and `PALMETTO_PAD_PRESENT` are disabled only by `0`/`off`/`no`; unset
keeps the feature on. That is their documented default (`port/shims/gameinput.cpp:176-178`, `gameinput_nosdl.h:876`)
and not a defect, but they are the only rows whose unset state is not the legacy-disabled state: list them separately
when auditing "default off" claims. Sites: `port/shims/gameinput.cpp:176`, `:177`, `gameinput_nosdl.h:876`.

### F5. Numeric switches parse with `atoi`/`strtoul` without a shared range check

The 49 `value_int` rows each validate their own range; e.g. `PALMETTO_CAPTURE_EVERY`/`PALMETTO_CAPTURE_LIMIT` use
`std::atoi` and clamp with `std::max(1, ...)` (`port/src/vulkan_present.h:199-200`), while
`PALMETTO_XS_*_SLOT` uses `strtoul` and keeps the default outside 3..127 (`port/shims/d3d12_xs_device.cpp:632`).
Not a wiring bug (each site names its fallback) but a portability difference: `atoi` accepts `12x` as 12 where
`strtoul`+end checks reject it. The reference parser's `integer_or` uses the strict form; sites keep their own.

## Falsifiers

```bat
:: isolated build with VS18 (no CMakeLists change needed), from the repo root:
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /I port\src port\test\opt_in_switches_test.cpp /Fe:%TEMP%\opt_in_switches_test.exe
%TEMP%\opt_in_switches_test.exe

:: the registered target through the shared build tree, from the repo root:
ctest --test-dir port\build -C RelWithDebInfo -R "^opt_in_switches$" --output-on-failure
```

Negative control: point the test at a tree with one extra `PALMETTO_*` literal and it fails `source scan`
(`scanned.size() == table.size()`); delete one table row and it fails `table shape`. Evidence of the isolated run and
the negative control: `D:\leonida-work\scratch\r21\optswitch\` (`opt_in_switches_test.exe`, `fakerepo\`, this
page's generator `audit.py`).

## The registry (294 switches)

The table is generated from the source scan and ordered by name. `site` is a read site (the value consumed by the
parser; a switch can have several); `wired` is include reachability from a compiled port translation unit.

| Switch | Kind | Site | Wired |
| --- | --- | --- | --- |
| `PALMETTO_ALLOC_TRACE` | any_set | `port/shims/xmem.cpp:211` | yes |
| `PALMETTO_ALLOC_WATCH` | switch_on | `port/src/main.cpp:3275` | yes |
| `PALMETTO_ALLOC_WATCH_FREE` | switch_on | `port/src/main.cpp:3288` | yes |
| `PALMETTO_ANALYTICS_FORM` | any_set | `port/shims/xgameruntime.cpp:1382` | yes |
| `PALMETTO_ARENA_LOG` | switch_on | `port/shims/d3d12_vulkan_device.cpp:9844` | yes |
| `PALMETTO_ARENA_MB` | any_set | `port/shims/d3d12_vulkan_device.cpp:9774` | yes |
| `PALMETTO_AUDIO_DECODE` | switch_on | `port/shims/audio_decode.h:148` | yes |
| `PALMETTO_AUDIO_EFFECTS` | switch_on | `port/shims/xapu_effects.h:15` | yes |
| `PALMETTO_AUDIO_SUBMIT_PROBE` | exact_one | `port/src/main.cpp:10434` | yes |
| `PALMETTO_BACK_IMAGES` | any_set | `port/src/main.cpp:10401` | yes |
| `PALMETTO_BASE_RESERVED` | value_int | `port/src/base_reservation.h:38` | yes |
| `PALMETTO_BINDLESS_DEGRADATION` | any_set | `port/shims/d3d12_vulkan_device.cpp:9622` | yes |
| `PALMETTO_BINDLESS_IDLE_WAIT` | switch_on | `port/shims/d3d12_vulkan_device.cpp:9746` | yes |
| `PALMETTO_BINK_FASTFORWARD` | any_set | `port/src/main.cpp:9807` | yes |
| `PALMETTO_BINK_PROBE` | switch_on | `port/src/main.cpp:10592` | yes |
| `PALMETTO_BINK_WITNESS` | any_set | `port/src/main.cpp:9806` | yes |
| `PALMETTO_BVH_ROUTE` | value_string | `port/src/xdxr_route.h:177` | yes |
| `PALMETTO_CAPTURE` | value_string | `port/src/vulkan_present.h:192` | yes |
| `PALMETTO_CAPTURE_EVERY` | value_int | `port/src/vulkan_present.h:199` | yes |
| `PALMETTO_CAPTURE_LIMIT` | value_int | `port/src/vulkan_present.h:200` | yes |
| `PALMETTO_CARD_COPY_GATE` | switch_on | `port/src/vulkan_backend.h:1106` | yes |
| `PALMETTO_CARD_COPY_GATE_REFRESH` | switch_on | `port/src/vulkan_backend.h:1107` | yes |
| `PALMETTO_CARD_COPY_SKIP_NO_SHADER` | switch_on | `port/src/vulkan_backend.h:1104` | yes |
| `PALMETTO_CARD_COPY_TRACE` | switch_on | `port/src/vulkan_backend.h:1103` | yes |
| `PALMETTO_CARD_COPY_WATCH` | switch_on | `port/src/vulkan_backend.h:1105` | yes |
| `PALMETTO_COLOUR_PATH` | switch_on | `port/src/vulkan_replay.h:2014` | yes |
| `PALMETTO_COMPAT_DIR` | value_string | `port/src/pe_loader.cpp:1166` | yes |
| `PALMETTO_CONSOLE_TYPE` | any_set | `port/src/pe_loader.cpp:2292` | yes |
| `PALMETTO_CONTENT_CULL_VARIANT` | any_set | `port/src/vulkan_replay.h:1593` | yes |
| `PALMETTO_CONTENT_PACKAGES` | any_set | `port/shims/xgameruntime.cpp:1436` | yes |
| `PALMETTO_CONTENT_PROBE` | any_set | `port/src/vulkan_replay.h:1527` | yes |
| `PALMETTO_CONTENT_PROBE_ARM` | any_set | `port/src/vulkan_replay.h:1560` | yes |
| `PALMETTO_CONTENT_PROBE_DRAWS` | any_set | `port/src/vulkan_replay.h:1537` | yes |
| `PALMETTO_CRASH_REPORT` | any_set | `port/shims/startup_trace.h:355` | yes |
| `PALMETTO_CRASH_REPORT_ALL` | any_set | `port/shims/startup_trace.h:356` | yes |
| `PALMETTO_CRASH_SAVE` | any_set | `port/shims/startup_trace.h:643` | yes |
| `PALMETTO_CRASH_SAVE_DIR` | value_string | `port/shims/startup_trace.h:646` | yes |
| `PALMETTO_D3D12` | value_string | `port/shims/d3d12_xs.cpp:165` | yes |
| `PALMETTO_D3D12X_QUERY_HEAP_READINESS` | exact_one | `port/src/query_heap_readiness.h:13` | yes |
| `PALMETTO_D3D12X_STRICT_VIEWPORTS` | exact_one | `port/src/viewport_replay_readiness.h:13` | yes |
| `PALMETTO_D3D12_RESIDENCY_BUDGET` | exact_one | `port/src/d3d12_residency_budget.h:22` | **no (test-only)** |
| `PALMETTO_DEVICE_HEALTH` | any_set | `port/shims/d3d12_vulkan_device.cpp:1285` | yes |
| `PALMETTO_DEVICE_TYPE` | value_int | `port/shims/xgameruntime.cpp:1233` | yes |
| `PALMETTO_DIRECT_HEAP_STRICT` | exact_one | `port/shims/d3d12_com.cpp:426` | yes |
| `PALMETTO_DISPLAY_DEFERRAL` | value_string | `port/shims/xgameruntime_offline.cpp:1154` | yes |
| `PALMETTO_DSTORAGE_TRACE` | switch_on | `port/shims/dstorage.cpp:105` | yes |
| `PALMETTO_DS_ARRAYS` | switch_on | `port/shims/dstorage.cpp:1419` | yes |
| `PALMETTO_DS_BCPACK` | text_switch | `port/shims/dstorage.cpp:188` | yes |
| `PALMETTO_DS_BCPACK_PREDICTOR` | any_set | `port/shims/dstorage.cpp:191` | yes |
| `PALMETTO_DS_LIGHTBAR` | value_string | `port/shims/gameinput.cpp:163` | yes |
| `PALMETTO_DS_LOAD_TRACE` | value_int | `port/shims/dstorage.cpp:317` | yes |
| `PALMETTO_DS_ORDER` | value_int | `port/shims/dstorage.cpp:507` | yes |
| `PALMETTO_DS_PAGES` | text_switch | `port/shims/dstorage.cpp:185` | yes |
| `PALMETTO_DS_PAGES_ARM` | switch_on | `port/shims/dstorage.cpp:3697` | yes |
| `PALMETTO_DS_PAGES_VIEW` | text_switch | `port/shims/dstorage.cpp:186` | yes |
| `PALMETTO_DS_PIN` | text_switch | `port/shims/dstorage.cpp:183` | yes |
| `PALMETTO_DS_PIN_RESIDENT` | text_switch | `port/shims/dstorage.cpp:184` | yes |
| `PALMETTO_DS_REAP_ORDER` | value_int | `port/shims/dstorage.cpp:1829` | yes |
| `PALMETTO_DS_RECORD` | value_int | `port/shims/dstorage.cpp:625` | yes |
| `PALMETTO_DS_STACK` | switch_on | `port/shims/dstorage.cpp:400` | yes |
| `PALMETTO_DS_STAGING` | value_int | `port/shims/dstorage.cpp:182` | yes |
| `PALMETTO_DS_TRIGGERS` | value_string | `port/shims/gameinput.cpp:153` | yes |
| `PALMETTO_DS_TRIGGER_FREQ` | value_string | `port/shims/gameinput.cpp:161` | yes |
| `PALMETTO_DS_TRIGGER_GAIN` | value_string | `port/shims/gameinput.cpp:157` | yes |
| `PALMETTO_DS_UNSHUFFLE` | text_switch | `port/shims/dstorage.cpp:187` | yes |
| `PALMETTO_DUMP_BYTES` | exact_one | `port/src/main.cpp:10245` | yes |
| `PALMETTO_DUMP_PIPELINES` | any_set | `port/shims/d3d12_vulkan_device.cpp:7593` | yes |
| `PALMETTO_DUMP_REGS` | any_set | `port/shims/startup_trace.h:249` | yes |
| `PALMETTO_DXGI_MODE_ANSWER` | value_string | `port/shims/dxgi_com.cpp:825` | yes |
| `PALMETTO_EXECUTOR_WATCH` | exact_one | `port/src/main.cpp:1492` | yes |
| `PALMETTO_FAKE_INPUT` | any_set | `port/shims/gameinput.cpp:1981` | yes |
| `PALMETTO_FAULT_MEM` | any_set | `port/shims/startup_trace.h:268` | yes |
| `PALMETTO_FAULT_MEM_RVA` | value_int | `port/shims/startup_trace.h:308` | yes |
| `PALMETTO_FILE_TRACE` | any_set | `port/shims/file_system.cpp:718` | yes |
| `PALMETTO_FIRST_RUN_SCRIPT` | any_set | `port/shims/gameinput.cpp:2070` | yes |
| `PALMETTO_FRAME_DUMP` | switch_on | `port/src/vulkan_replay.h:5585` | yes |
| `PALMETTO_FRAME_DUMP_AT_DRAW` | value_int | `port/src/vulkan_replay.h:5595` | yes |
| `PALMETTO_FRAME_DUMP_CONTROL` | switch_on | `port/src/vulkan_replay.h:5617` | yes |
| `PALMETTO_FRAME_DUMP_CONTROL_PS` | value_int | `port/src/vulkan_backend.h:2267` | yes |
| `PALMETTO_FRAME_DUMP_CONTROL_VS` | value_int | `port/src/vulkan_backend.h:2266` | yes |
| `PALMETTO_FRAME_DUMP_PS_CONTROL` | switch_on | `port/src/vulkan_replay.h:5633` | yes |
| `PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE` | switch_on | `port/src/vulkan_replay.h:5647` | yes |
| `PALMETTO_FRAME_DUMP_SAMPLE` | switch_on | `port/src/vulkan_replay.h:1451` | yes |
| `PALMETTO_FRAME_DUMP_WINDOW` | value_int | `port/src/vulkan_replay.h:5605` | yes |
| `PALMETTO_GAMEINPUT_THREAD` | disable_value | `port/shims/gameinput.cpp:177` | yes |
| `PALMETTO_GAMERTAG` | any_set | `port/shims/xgameruntime_user.cpp:82` | yes |
| `PALMETTO_GAME_DIR` | value_string | `port/shims/xgameruntime.cpp:1800` | yes |
| `PALMETTO_GPU_VIEW_PAIR` | any_set | `port/shims/xmem.cpp:730` | yes |
| `PALMETTO_GPU_VIEW_PAIR_TRACE` | any_set | `port/shims/xmem.cpp:731` | yes |
| `PALMETTO_GRAPHICS_CONSOLE_ROOT` | switch_on | `port/src/vulkan_replay.h:1822` | yes |
| `PALMETTO_HDR` | value_string | `port/shims/xgameruntime_offline.cpp:575` | yes |
| `PALMETTO_IMAGE_DEFERRED` | switch_on | `port/shims/d3d12_vulkan_device.cpp:1261` | yes |
| `PALMETTO_IMAGE_MEM_POOL` | switch_on | `port/shims/d3d12_vulkan_device.cpp:1250` | yes |
| `PALMETTO_IMAGE_MEM_TRACE` | switch_on | `port/shims/d3d12_vulkan_device.cpp:1239` | yes |
| `PALMETTO_INPUT_TRACE` | value_string | `port/shims/gameinput.cpp:210` | yes |
| `PALMETTO_KEYBOARD_PAD` | value_string | `port/shims/gameinput.cpp:178` | yes |
| `PALMETTO_LIST_SLOT_TRACE` | switch_on | `port/shims/d3d12_xs_device.cpp:1885` | yes |
| `PALMETTO_LOADER_BEGINEND` | switch_on | `port/src/main.cpp:815` | yes |
| `PALMETTO_LOADER_CENSUS` | exact_one | `port/src/loader_census.h:38` | yes |
| `PALMETTO_LOADER_HOOK` | switch_on | `port/src/main.cpp:10489` | yes |
| `PALMETTO_LOADER_POLLBP` | switch_on | `port/src/main.cpp:835` | yes |
| `PALMETTO_LOADER_WATCH` | switch_on | `port/src/main.cpp:10565` | yes |
| `PALMETTO_LOAD_TRACE` | value_int | `port/shims/d3d12_xs_device.cpp:3453` | yes |
| `PALMETTO_LOG_GAME_EVENTS` | exact_one | `port/shims/xgameruntime_offline.cpp:129` | yes |
| `PALMETTO_MEM_TRACE` | value_int | `port/shims/xmem.cpp:202` | yes |
| `PALMETTO_MESH_STREAM` | any_set | `port/shims/d3d12_xs_device.cpp:7307` | yes |
| `PALMETTO_MOUSE_SENSITIVITY` | value_string | `port/shims/gameinput.cpp:159` | yes |
| `PALMETTO_MOVIE_PROBE` | exact_one | `port/src/main.cpp:10339` | yes |
| `PALMETTO_NETWORK_HINT` | value_string | `port/shims/xgameruntime.cpp:481` | yes |
| `PALMETTO_OBJECT_WRITE_WATCH` | exact_one | `port/src/main.cpp:1807` | yes |
| `PALMETTO_PACE_CONTENT` | any_set | `port/src/vulkan_backend.h:3505` | yes |
| `PALMETTO_PACE_DELAY_US` | any_set | `port/src/vulkan_backend.h:3504` | yes |
| `PALMETTO_PACE_IDLE` | any_set | `port/src/vulkan_backend.h:3504` | yes |
| `PALMETTO_PACKAGED` | value_int | `port/shims/xgameruntime.cpp:1536` | yes |
| `PALMETTO_PACKAGE_ID` | any_set | `port/shims/xgameruntime.cpp:1402` | yes |
| `PALMETTO_PACKAGE_READY` | value_string | `port/shims/xgameruntime.cpp:1591` | yes |
| `PALMETTO_PACKAGE_V4` | value_string | `port/shims/xgameruntime.cpp:1573` | yes |
| `PALMETTO_PAD_PRESENT` | disable_value | `port/shims/gameinput_nosdl.h:876` | yes |
| `PALMETTO_PAGE_PULL` | switch_on | `port/shims/d3d12_vulkan_device.cpp:3572` | yes |
| `PALMETTO_PERSIST_PROFILE` | value_string | `port/shims/xgameruntime.cpp:913` | yes |
| `PALMETTO_PE_LOADER_SHADOW_PARSE` | exact_one | `port/src/pe_loader.cpp:1518` | yes |
| `PALMETTO_PE_LOADER_SHADOW_STRICT` | exact_one | `port/src/pe_loader.cpp:1518` | yes |
| `PALMETTO_PIPELINE_CACHE` | any_set | `port/shims/d3d12_vulkan_device.cpp:495` | yes |
| `PALMETTO_PIPELINE_CACHE_MAX_MB` | value_int | `port/shims/d3d12_vulkan_device.cpp:512` | yes |
| `PALMETTO_PIPELINE_CALL_STATS` | switch_on | `port/shims/d3d12_vulkan_device.cpp:1541` | yes |
| `PALMETTO_PIPELINE_COMPILE_BOUND` | value_int | `port/shims/d3d12_vulkan_device.cpp:1512` | yes |
| `PALMETTO_PIPELINE_COMPILE_WORKERS` | value_int | `port/shims/d3d12_vulkan_device.cpp:1501` | yes |
| `PALMETTO_PIPELINE_DEFER_COMPILE` | switch_on | `port/shims/d3d12_vulkan_device.cpp:1491` | yes |
| `PALMETTO_PIPELINE_HOLD` | any_set | `port/shims/d3d12_vulkan_device.cpp:1570` | yes |
| `PALMETTO_PIPELINE_HOLD_BOUND` | any_set | `port/shims/d3d12_vulkan_device.cpp:1580` | yes |
| `PALMETTO_PIPELINE_HOLD_BUDGET_MS` | any_set | `port/shims/d3d12_vulkan_device.cpp:1591` | yes |
| `PALMETTO_PIPELINE_LAZY_BUILD` | switch_on | `port/shims/d3d12_vulkan_device.cpp:1474` | yes |
| `PALMETTO_PIPELINE_LIVE_CAP` | value_int | `port/shims/d3d12_vulkan_device.cpp:1461` | yes |
| `PALMETTO_PIPELINE_PREWARM` | switch_on | `port/shims/d3d12_vulkan_device.cpp:1528` | yes |
| `PALMETTO_PIPELINE_PROFILE` | switch_on | `port/shims/d3d12_vulkan_device.cpp:241` | yes |
| `PALMETTO_PIPELINE_RECIPE_CACHE` | switch_on | `port/shims/d3d12_vulkan_device.cpp:1141` | yes |
| `PALMETTO_PIPELINE_TIMELINE` | value_int | `port/shims/d3d12_xs_device.cpp:276` | yes |
| `PALMETTO_PLANE_REFRESH` | any_set | `port/shims/d3d12_vulkan_device.cpp:1735` | yes |
| `PALMETTO_PLANE_REFRESH_TRACE` | any_set | `port/shims/d3d12_vulkan_device.cpp:1736` | yes |
| `PALMETTO_POKE_HOST` | any_set | `port/shims/startup_trace.h:557` | yes |
| `PALMETTO_POKE_LEA` | exact_one | `port/src/main.cpp:10266` | yes |
| `PALMETTO_POKE_LOG` | exact_one | `port/src/main.cpp:10321` | yes |
| `PALMETTO_PRESENT_ADOPT` | exact_one | `port/shims/xbox_present.cpp:1017` | yes |
| `PALMETTO_PRESENT_ATTEST` | any_set | `port/src/main.cpp:10067` | yes |
| `PALMETTO_PRESENT_DRIVEN` | value_string | `port/shims/xbox_present.cpp:461` | yes |
| `PALMETTO_PRESENT_FIT` | any_set | `port/src/vulkan_present.h:207` | yes |
| `PALMETTO_PRESENT_GEOMETRY` | value_int | `port/src/vulkan_present.h:222` | yes |
| `PALMETTO_PRESENT_HDR10` | any_set | `port/src/vulkan_present.h:240` | yes |
| `PALMETTO_PRESENT_HEIGHT` | value_int | `port/shims/xbox_present.cpp:1171` | yes |
| `PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY` | value_string | `port/shims/xbox_present.cpp:372` | yes |
| `PALMETTO_PRESENT_NO_TEAR` | value_string | `port/shims/d3d12_vulkan_device.cpp:14372` | yes |
| `PALMETTO_PRESENT_PACING` | value_string | `port/shims/d3d12_xs_device.cpp:5133` | yes |
| `PALMETTO_PRESENT_PER_ORIGIN` | any_set | `port/shims/xbox_present.cpp:57` | yes |
| `PALMETTO_PRESENT_PLANE_DUMP` | switch_on | `port/shims/xbox_present.cpp:408` | yes |
| `PALMETTO_PRESENT_PLANE_RULE` | any_set | `port/shims/xbox_present.cpp:358` | yes |
| `PALMETTO_PRESENT_STUB` | any_set | `port/shims/d3d12_xs_device.cpp:4788` | yes |
| `PALMETTO_PRESENT_SYNTHETIC` | value_int | `port/src/vulkan_present.h:224` | yes |
| `PALMETTO_PRESENT_UNCAP` | value_string | `port/shims/xbox_present.cpp:538` | yes |
| `PALMETTO_PRESENT_WIDTH` | value_int | `port/shims/xbox_present.cpp:1170` | yes |
| `PALMETTO_PRESENT_WINDOW` | value_string | `port/shims/xbox_present.cpp:987` | yes |
| `PALMETTO_PROMPTS` | value_string | `port/shims/gameinput.cpp:185` | yes |
| `PALMETTO_PROMPTS_SWITCH_MS` | value_string | `port/shims/gameinput.cpp:195` | yes |
| `PALMETTO_RASTER_CULL_MEASURED` | switch_on | `port/src/graphics_pso.cpp:189` | yes |
| `PALMETTO_RASTER_RECORD_MEASURED` | any_set | `port/src/graphics_pso.cpp:427` | yes |
| `PALMETTO_READBACK_AFTER` | any_set | `port/src/vulkan_backend.h:1078` | yes |
| `PALMETTO_READBACK_AT_DRAW` | value_int | `port/src/vulkan_replay.h:1477` | yes |
| `PALMETTO_READBACK_CENTRE` | switch_on | `port/src/vulkan_backend.h:5017` | yes |
| `PALMETTO_READBACK_FULL` | any_set | `port/src/vulkan_replay.h:1494` | yes |
| `PALMETTO_READBACK_PPM` | value_int | `port/src/vulkan_backend.h:5402` | yes |
| `PALMETTO_READBACK_PPM_LIMIT` | value_int | `port/src/vulkan_backend.h:5405` | yes |
| `PALMETTO_READ_TERMINAL` | any_set | `port/shims/dstorage.cpp:1705` | yes |
| `PALMETTO_REPLAY_MISS` | switch_on | `port/src/vulkan_replay.h:1081` | yes |
| `PALMETTO_RESOLVE` | exact_one | `port/src/main.cpp:10184` | yes |
| `PALMETTO_RIP_HIST` | any_set | `port/src/main.cpp:10422` | yes |
| `PALMETTO_RNG_SEED` | any_set | `port/shims/bcrypt_rng.cpp:80` | yes |
| `PALMETTO_ROOT_DATA_TRACE` | switch_on | `port/shims/d3d12_com.cpp:2008` | yes |
| `PALMETTO_ROOT_PACK_MEASURE` | switch_on | `port/shims/d3d12_com.cpp:415` | yes |
| `PALMETTO_RT_AS` | value_string | `port/shims/d3d12_vulkan_device.cpp:14688` | yes |
| `PALMETTO_RT_ENV_INIT` | any_set | `port/shims/process.cpp:315` | yes |
| `PALMETTO_RUMBLE` | disable_value | `port/shims/gameinput.cpp:176` | yes |
| `PALMETTO_S3DISPATCH` | switch_on | `port/src/main.cpp:10539` | yes |
| `PALMETTO_SAVE_DIR` | value_string | `port/shims/xgameruntime_gamesave.cpp:180` | yes |
| `PALMETTO_SDR_EOTF` | value_string | `port/shims/xbox_present.cpp:523` | yes |
| `PALMETTO_SEED_GLOBAL` | exact_one | `port/src/main.cpp:10204` | yes |
| `PALMETTO_SHADERS` | value_string | `port/src/main.cpp:9955` | yes |
| `PALMETTO_SIG_CLASS` | switch_on | `port/shims/d3d12_vulkan_device.cpp:6914` | yes |
| `PALMETTO_SILENT_SPEECH` | value_string | `port/shims/xgameruntime_offline.cpp:840` | yes |
| `PALMETTO_SKIP_CPU_CHECK` | exact_one | `port/src/host_cpu.cpp:73` | yes |
| `PALMETTO_SKIP_INT3` | any_set | `port/shims/startup_trace.h:782` | yes |
| `PALMETTO_SLOT67_PROGRAM_LOG` | switch_on | `port/shims/d3d12_xs_device.cpp:7819` | yes |
| `PALMETTO_SLOT70_CBV` | switch_on | `port/shims/d3d12_xs_device.cpp:5483` | yes |
| `PALMETTO_SLOT70_CENSUS` | switch_on | `port/shims/d3d12_xs_device.cpp:5484` | yes |
| `PALMETTO_SLOT70_IDENTITY_VA` | value_int | `port/shims/d3d12_xs_device.cpp:5875` | yes |
| `PALMETTO_SLOT70_PAIR` | switch_on | `port/shims/d3d12_xs_device.cpp:5485` | yes |
| `PALMETTO_SLOT70_PROBE` | switch_on | `port/shims/d3d12_xs_device.cpp:5483` | yes |
| `PALMETTO_SLOT70_PUBLISH` | switch_on | `port/shims/d3d12_xs_device.cpp:5484` | yes |
| `PALMETTO_SLOT_WATCH` | switch_on | `port/src/main.cpp:10531` | yes |
| `PALMETTO_STAGE2_DISPATCH` | value_int | `port/src/main.cpp:10471` | yes |
| `PALMETTO_STAGE2_READY` | value_int | `port/src/main.cpp:10481` | yes |
| `PALMETTO_STAGE2_TRACKER` | value_int | `port/src/main.cpp:10461` | yes |
| `PALMETTO_STAGE2_WAKE` | value_int | `port/src/main.cpp:10452` | yes |
| `PALMETTO_STAGE3_PUMP` | any_set | `port/src/main.cpp:10448` | yes |
| `PALMETTO_STANDIN_PIPELINES` | switch_on | `port/shims/d3d12_vulkan_device.cpp:3599` | yes |
| `PALMETTO_STATE_TRACE` | exact_one | `port/src/main.cpp:10431` | yes |
| `PALMETTO_STOP_INT3` | any_set | `port/shims/startup_trace.h:765` | yes |
| `PALMETTO_STORE_ASYNC_COMPLETE` | value_string | `port/shims/xgameruntime_store.cpp:147` | yes |
| `PALMETTO_STREAMOPEN` | switch_on | `port/src/main.cpp:10557` | yes |
| `PALMETTO_STREAMOPEN_WATCH` | switch_on | `port/src/main.cpp:10558` | yes |
| `PALMETTO_STREAMREAP` | switch_on | `port/src/main.cpp:10546` | yes |
| `PALMETTO_STREAMREAP_WATCH` | switch_on | `port/src/main.cpp:10547` | yes |
| `PALMETTO_STREAM_CONSUMER` | switch_on | `port/shims/dstorage.cpp:753` | yes |
| `PALMETTO_SUBMIT_OVERLAP` | switch_on | `port/src/queue_holds.h:54` | yes |
| `PALMETTO_SUBMIT_OVERLAP_COPY_BOUND` | any_set | `port/src/queue_holds.h:57` | yes |
| `PALMETTO_SUBMIT_OVERLAP_DIRECT_BOUND` | switch_on | `port/src/queue_holds.h:56` | yes |
| `PALMETTO_SUBMIT_TRACE` | switch_on | `port/src/vulkan_backend.h:3930` | yes |
| `PALMETTO_TEB_CHECK` | any_set | `port/src/main.cpp:10226` | yes |
| `PALMETTO_TEXTURE_BIND_FETCHED` | any_set | `port/src/vulkan_replay.h:1760` | yes |
| `PALMETTO_TEXTURE_IMPORT` | switch_on | `port/shims/d3d12_vulkan_device.cpp:3586` | yes |
| `PALMETTO_TEXTURE_IMPORT_MAX` | value_int | `port/shims/d3d12_vulkan_device.cpp:4184` | yes |
| `PALMETTO_TEXTURE_PAGE_WATCH` | switch_on | `port/shims/d3d12_vulkan_device.cpp:2848` | yes |
| `PALMETTO_THREAD_STACKS` | any_set | `port/src/main.cpp:10426` | yes |
| `PALMETTO_TIMELINE` | switch_on | `port/src/main.cpp:10496` | yes |
| `PALMETTO_TITLE_CLASS0A_SIZE` | exact_one | `port/src/main.cpp:10047` | yes |
| `PALMETTO_TITLE_PEEK` | any_set | `port/shims/d3d12_xs_device.cpp:3580` | yes |
| `PALMETTO_TITLE_PULL_COALESCE` | switch_on | `port/src/vulkan_backend.h:1112` | yes |
| `PALMETTO_TITLE_PULL_TRACE` | switch_on | `port/src/vulkan_backend.h:1111` | yes |
| `PALMETTO_TITLE_VA` | switch_on | `port/shims/d3d12_vulkan_device.cpp:3560` | yes |
| `PALMETTO_TITLE_VA_WINDOWS` | any_set | `port/shims/d3d12_vulkan_device.cpp:2003` | yes |
| `PALMETTO_TITLE_WATCH` | any_set | `port/shims/d3d12_xs_device.cpp:3511` | yes |
| `PALMETTO_TITLE_WATCHDOG_HOLD` | any_set | `port/src/main.cpp:10091` | yes |
| `PALMETTO_TLS_GA_PATCH` | any_set | `port/src/pe_loader.cpp:601` | yes |
| `PALMETTO_TRACE_TLS` | any_set | `port/src/pe_loader.cpp:552` | yes |
| `PALMETTO_UI_INPUT_SCRIPT` | exact_one | `port/src/ui_input_script.h:38` | **no (test-only)** |
| `PALMETTO_UI_PROBE` | exact_one | `port/src/main.cpp:10371` | yes |
| `PALMETTO_UNIMPLEMENTED` | any_set | `port/shims/registry.cpp:309` | yes |
| `PALMETTO_UPLOAD_SYSMEM` | switch_on | `port/shims/d3d12_vulkan_device.cpp:9241` | yes |
| `PALMETTO_UPSCALE_KIND7_CONSUMER` | exact_one | `port/src/upscale_kind7.h:20` | yes |
| `PALMETTO_UPSCALE_STANDIN` | switch_on | `port/src/upscale_standin.h:193` | yes |
| `PALMETTO_UPSCALE_STANDIN_CONTROL` | switch_on | `port/src/upscale_standin.h:203` | yes |
| `PALMETTO_USER_CHANGE_EVENT` | value_string | `port/shims/xgameruntime_user.cpp:383` | yes |
| `PALMETTO_USER_TRACE` | value_string | `port/shims/platform_trace.h:31` | yes |
| `PALMETTO_VERTEX_MATRIX_DESCRIPTOR` | switch_on | `port/src/vulkan_replay.h:1768` | yes |
| `PALMETTO_VERTEX_PATH` | switch_on | `port/src/vulkan_replay.h:1731` | yes |
| `PALMETTO_VERTEX_STREAM_DESCRIPTOR` | switch_on | `port/src/vulkan_replay.h:1747` | yes |
| `PALMETTO_VK_DEVICE` | any_set | `port/src/vulkan_device_filter.h:22` | yes |
| `PALMETTO_VK_GRAPHICS_FAMILY_ONLY` | value_int | `port/shims/d3d12_vulkan_device.cpp:132` | yes |
| `PALMETTO_VK_MAX_QUEUES_PER_FAMILY` | value_int | `port/shims/d3d12_vulkan_device.cpp:141` | yes |
| `PALMETTO_VK_VALIDATION` | switch_on | `port/shims/d3d12_vulkan_device.cpp:14504` | yes |
| `PALMETTO_WAIT_SAMPLE` | any_set | `port/src/main.cpp:10413` | yes |
| `PALMETTO_WAIT_TRACE` | any_set | `port/src/main.cpp:10418` | yes |
| `PALMETTO_WATCH_GLOBAL` | exact_one | `port/src/main.cpp:10506` | yes |
| `PALMETTO_WATCH_RVA_WRITE` | value_int | `port/shims/startup_trace.h:447` | yes |
| `PALMETTO_WINDOW_FIT` | any_set | `port/shims/xbox_present.cpp:251` | yes |
| `PALMETTO_WRITER_STACK_LOG` | any_set | `port/src/main.cpp:2127` | yes |
| `PALMETTO_WRITER_SWEEP` | any_set | `port/src/main.cpp:2537` | yes |
| `PALMETTO_WRITER_SWEEP_DELAY_MS` | value_int | `port/src/main.cpp:2526` | yes |
| `PALMETTO_WRITER_WATCH` | exact_one | `port/src/main.cpp:2708` | yes |
| `PALMETTO_XBOX_VIEW_SHAPE` | switch_on | `port/shims/d3d12_xs_device.cpp:3237` | yes |
| `PALMETTO_XG_DUMP` | switch_on | `port/shims/xg_xs.cpp:227` | yes |
| `PALMETTO_XG_LAYOUT` | value_string | `port/shims/xg_xs.cpp:208` | yes |
| `PALMETTO_XG_LINEAR_ARRAYS` | any_set | `port/shims/xg_xs.cpp:132` | yes |
| `PALMETTO_XG_MOVIE_PLANES` | value_string | `port/shims/xg_xs.cpp:111` | yes |
| `PALMETTO_XG_TRACE` | switch_on | `port/shims/xg_xs.cpp:226` | yes |
| `PALMETTO_XMA2_DECODER_DLL` | value_string | `port/shims/audio_decode.h:177` | yes |
| `PALMETTO_XMEM_ALIAS_WATCH` | any_set | `port/shims/d3d12_vulkan_device.cpp:1717` | yes |
| `PALMETTO_XMEM_CALLERS` | any_set | `port/shims/xmem.cpp:83` | yes |
| `PALMETTO_XMEM_LAYOUT` | any_set | `port/shims/xmem.cpp:2518` | yes |
| `PALMETTO_XMEM_LAYOUT_CALLER` | any_set | `port/shims/xmem.cpp:2519` | yes |
| `PALMETTO_XMEM_NATIVE_WRITE_WATCH` | any_set | `port/shims/xmem.cpp:2559` | yes |
| `PALMETTO_XS_ALLOCATOR_CALLBACK` | switch_on | `port/shims/d3d12_xs_device.cpp:1441` | yes |
| `PALMETTO_XS_ALLOCATOR_CALLBACK_CLASS` | value_int | `port/shims/d3d12_xs_device.cpp:1468` | yes |
| `PALMETTO_XS_ALLOCATOR_CALLBACK_PROBE` | value_int | `port/shims/d3d12_xs_device.cpp:1479` | yes |
| `PALMETTO_XS_ALLOCATOR_CALLBACK_SIZE` | value_int | `port/shims/d3d12_xs_device.cpp:1456` | yes |
| `PALMETTO_XS_ALLOCATOR_CALLBACK_TRACE` | switch_on | `port/shims/d3d12_xs_device.cpp:1446` | yes |
| `PALMETTO_XS_BOTTOM_OF_PIPE_GPU` | value_int | `port/shims/d3d12_xs_device.cpp:1967` | yes |
| `PALMETTO_XS_CALLERS` | switch_on | `port/shims/d3d12_xs_device.cpp:531` | yes |
| `PALMETTO_XS_CALLERS_PROFILE` | switch_on | `port/shims/d3d12_xs_device.cpp:542` | yes |
| `PALMETTO_XS_CLEAR_BARRIER_PROBE` | switch_on | `port/shims/d3d12_xs_device.cpp:3652` | yes |
| `PALMETTO_XS_CONSOLE_LIST_EXTRAS` | switch_on | `port/shims/d3d12_xs_device.cpp:2824` | yes |
| `PALMETTO_XS_CONSOLE_LIST_TRACE` | switch_on | `port/shims/d3d12_xs_device.cpp:2829` | yes |
| `PALMETTO_XS_FRAME_INTERVAL_SLOT` | value_int | `port/shims/d3d12_xs_device.cpp:654` | yes |
| `PALMETTO_XS_LIST_DUMP` | any_set | `port/shims/d3d12_xs_device.cpp:4204` | yes |
| `PALMETTO_XS_LIST_ID` | switch_on | `port/shims/d3d12_xs_device.cpp:4288` | yes |
| `PALMETTO_XS_LIST_MUTE` | value_int | `port/shims/d3d12_xs_device.cpp:4179` | yes |
| `PALMETTO_XS_LIST_X_SLOT` | value_int | `port/shims/d3d12_xs_device.cpp:658` | yes |
| `PALMETTO_XS_MEMORY_WAIT_BOUND_MS` | any_set | `port/shims/d3d12_xs_device.cpp:1965` | yes |
| `PALMETTO_XS_MEMORY_WAIT_HOLD` | any_set | `port/shims/d3d12_xs_device.cpp:1964` | yes |
| `PALMETTO_XS_MEMORY_WAIT_MAX` | any_set | `port/shims/d3d12_xs_device.cpp:1966` | yes |
| `PALMETTO_XS_MEMORY_WAIT_TRACE` | switch_on | `port/shims/d3d12_xs_device.cpp:1987` | yes |
| `PALMETTO_XS_QUEUE_X_SLOT` | value_int | `port/shims/d3d12_xs_device.cpp:657` | yes |
| `PALMETTO_XS_SCHEDULE_EVENT_SLOT` | value_int | `port/shims/d3d12_xs_device.cpp:655` | yes |
| `PALMETTO_XS_SIG41` | switch_on | `port/shims/d3d12_xs_device.cpp:748` | yes |
| `PALMETTO_XS_SLOT57_LAYOUT_DUMP` | switch_on | `port/shims/d3d12_xs_device.cpp:6941` | yes |
| `PALMETTO_XS_WAIT_EVENT_SLOT` | value_int | `port/shims/d3d12_xs_device.cpp:656` | yes |

## Registered target (port/CMakeLists.txt, applied by the lead in round 21)

```cmake
add_executable(opt_in_switches_test test/opt_in_switches_test.cpp)
target_include_directories(opt_in_switches_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(opt_in_switches_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  PALMETTO_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(opt_in_switches_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
add_test(NAME opt_in_switches COMMAND opt_in_switches_test)
```

The macro points at the checkout root so the scan finds `port/src` even when CTest runs from a build directory
outside it. The test also accepts `PALMETTO_SOURCE_ROOT` and walks up from the working directory when the macro is
absent, so a hand build with plain `cl` works too (see Falsifiers).

## Evidence

- `port/src/opt_in_switches.h` - the 294-row table, kinds, the pure parser and the non-switch exclusion.
- `port/src/query_heap_readiness.h` - the F1-follow-up wiring: pure `assess()` + `action_of()`/`is_refusal()`/
  `action_name()` mapping; the shim call is `port/shims/d3d12_vulkan_device.cpp` `CreateQueryHeap`.
- `port/test/query_heap_readiness_test.cpp` - 8 synthetic cases, including the new shim action-mapping case;
  `ctest -R '^query_heap_readiness$'` -> 1/1 passed in 0.06 s on the round-21 tree.
- `port/test/opt_in_switches_test.cpp` - 3,010 checks: parser semantics, table shape, kind census, default path,
  source scan, recorded sites and include reachability; `PASS (3010 checks, 294 switches)`. Built through the
  registered target and `ctest -R '^opt_in_switches$'` on the round-21 tree: 1/1 passed in 0.86 s. `D:\leonida-work\scratch\r21\optswitch\RESULTS.txt` has both logs and the negative control.
- `D:\leonida-work\scratch\r21\optswitch\` - `audit.py` (the scan generator), `switches.json` (per-row data),
  `literal_names.txt` (name evidence) and the compiled isolated test.
- Round-20/21 pages that define the three unwired slices: [d3d12_vulkan.md](d3d12_vulkan.md) (query/residency)
  and [peer_port_first_ui.md](peer_port_first_ui.md) (first-run input script); the census slice in
  [pe_loader_rewrite.md](pe_loader_rewrite.md) is wired by round-21 T1.
