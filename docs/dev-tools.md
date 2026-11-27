# Development tools

From the Palmetto checkout root, use Python 3 to list the development tools:

```powershell
python -I tooling/scripts/dev.py --help
python -I tooling/scripts/dev.py discover
python -I tooling/scripts/dev.py discover --json
```

From another directory, pass the path to `tooling/scripts/dev.py`, quoting it if needed.
The inventory always describes the checkout containing that script.

`boot-trace` points to the analyzer for startup JSONL and D3D12 stderr logs.
`guest-scene` points to the headless synthetic guest runner. Run a listed help
command from the checkout root to see the producer's options. The text
inventory displays commands for PowerShell on Windows and POSIX shells
on other platforms. The inventory
checks for each producer script and lists its prerequisites without checking
those prerequisites or running the producer. Live attach, pause, step, input,
and screenshots are unavailable through this entry point.

## JSON output

`discover --json` writes one object to stdout:

- `schema_version` identifies the format. The current version is `1`.
- `tools` lists entries by `id`. Each `path` uses forward slashes and is
  relative to the checkout root. `present` indicates whether the script is
  a file. `help_argv` is a list of arguments for a command run from the checkout
  root. Its first argument is the Python executable running discovery. It
  is `null` when the script is missing. `requirements` lists prerequisites
  for using the producer.
- `live_controls` contains an `available` boolean and a `reason`.

Exit code `0` indicates that the inventory was emitted, including when a
producer script is missing. Invalid CLI arguments exit with code `2`.

## Analyze a boot trace

Use the existing analyzer through the development CLI:

```powershell
python -I tooling/scripts/dev.py boot-trace --help
python -I tooling/scripts/dev.py boot-trace "logs/startup trace.jsonl"
python -I tooling/scripts/dev.py boot-trace "logs/startup trace.jsonl" --d3d12 "logs/d3d12 stderr.log" --json
```

Pass a startup JSONL from `palmetto --trace`, a D3D12 stderr log from
`palmetto --trace-d3d12`, or both. Relative input paths are interpreted from
the directory where you run the CLI. From outside the checkout, give the
path to `tooling/scripts/dev.py` and keep the trace paths relative to your directory.

The analyzer supplies the help, output, diagnostics and exit status. If one
input fails but the other can be read, `--json` can still emit a partial
summary on stdout with a diagnostic on stderr and exit code `2`. If neither
input can be analyzed, there is no JSON summary. The summary describes the
recorded events, and the exit status reports the analysis result.

You can also run `python -I tooling/boot_trace.py` with the same arguments.

## Run a synthetic guest scene

Use the existing headless runner with a loader and guest built on Windows
with Vulkan, static SDL3 and `PALMETTO_GUEST_SCENE=ON`. Supply the synthetic
M1.5 shader fixture root containing `spirv/`. See the
[runtime instructions](../port/README.md#a-600-frame-headless-guest-scene)
for the build, fixture and Wine setup.

```powershell
python -I tooling/scripts/dev.py guest-scene --help
python -I tooling/scripts/dev.py guest-scene --loader "build/palmetto.exe" --guest "build/tiny_guest_draw.exe" --shaders "m15 shaders" --out "scene run" --negative-controls
```

Pass `--out PATH` or `--out=PATH` with a new or empty directory. Relative
paths are interpreted from the directory where you run the CLI, including
when you invoke `tooling/scripts/dev.py` by its full path from another directory.

The CLI forwards the runner's exit code and diagnostics. Exit code `0`
means the scene checks passed. Exit code `1` can indicate a scene check
failure or a failure before the runner can write a report, such as an
output directory it cannot create. Exit code `2` indicates a usage or
prerequisite error.

When the runner writes `report.json`, it prints the path. Open that file
for the scene result and per-run details. For example, if the full run
starts and the loader exits with an error, `runs.full.returncode` records
the loader's exit code while the CLI returns the runner's exit code.
The output directory also holds any logs, frame records and PPM images
the runner produced. If the runner rejects an occupied directory, any
report already there belongs to an earlier run.

You can also invoke `python -I tooling/scripts/run_guest_scene.py` directly.


## Environment switches for measurement

These switches are opt-in and off by default unless stated; `PALMETTO_SHADERS`, `PALMETTO_D3D12`,
`PALMETTO_SKIP_INT3` and `PALMETTO_SEED_GLOBAL` are the ones a normal boot sets.

| Variable | Purpose |
|---|---|
| `PALMETTO_SHADERS` | Directory of the translated shader corpus the runtime resolves programs from. |
| `PALMETTO_D3D12` | Selects the D3D12 backend (`vulkan` for the real device). |
| `PALMETTO_SKIP_INT3` | Stepping policy for the loader's `int3` redirect sites. |
| `PALMETTO_SEED_GLOBAL` | `addr:size` of a stand-in range the loader zeroes before the title reads it. |
| `PALMETTO_CRASH_SAVE_DIR` | Where a crash report and its side files are written beside the faulting process. |
| `PALMETTO_DS_ORDER` | Microseconds of console-faithful DirectStorage completion ordering (diagnostic and opt-in fix). |
| `PALMETTO_DUMP_BYTES` | Hex RVAs whose image words are printed after the loader maps the image. |
| `PALMETTO_DUMP_PIPELINES` | Directory that receives dumped pipeline program bytes and state for offline translation. |
| `PALMETTO_PIPELINE_CACHE` | Opt-in persistent `VkPipelineCache` file: loaded at the first pipeline creation and written back while the boot compiles; unset keeps today's empty/build-seeded cache (docs/pipeline_cache.md). |
| `PALMETTO_PIPELINE_CACHE_MAX_MB` | Size bound of `PALMETTO_PIPELINE_CACHE`, 1 to 16384 MiB, default 512; a blob over it is neither read nor written. |
| `PALMETTO_PIPELINE_PROFILE` | Pipeline creation profile: per-kind calls, driver compile vs shader-module/layout time, wall-clock windows, cache feedback and the slowest recipes (docs/pipeline_cache.md). |
| `PALMETTO_PIPELINE_RECIPE_CACHE` | Opt-in per-process pipeline recipe cache: identical descriptions share one `VkPipeline`, `VkPipelineLayout` and shader modules, reference-counted until the last PSO using them is released (docs/pipeline_cache.md). |
| `PALMETTO_POKE_LEA` | One shaped instruction to poke, for diagnosting a specific loader path. |
| `PALMETTO_RESOLVE` | Hex address to describe (what lives there) for a call or store target outside the image. |
| `PALMETTO_RNG_SEED` | Deterministic RNG seed (decimal or 0x hex) for reproducible runs. |
| `PALMETTO_STOP_INT3` | Up to four `int3` sites that are not stepped over. |
| `PALMETTO_TITLE_VA` | `0` disables rebasing the title's shader-visible VAs into the arena. |
| `PALMETTO_TITLE_WATCH` | Hex RVAs of title memory to watch during a run (measurement only). |
| `PALMETTO_TRACE_TLS` | Traces the loader's TLS redirect resolution. |
| `PALMETTO_PIPELINE_LIVE_CAP=<n>` | The live graphics-pipeline cap (default 16, the measured-correct default; `0` keeps the exact old eager path for the A/B). A bind past the cap evicts the least-recently-used pipeline; the state keeps its recipe and rebuilds on the next bind, and the old pipeline is deleted after the GPU passes the submissions (docs/memory_growth.md). |
| `PALMETTO_PIPELINE_LAZY_BUILD=0` | Builds every graphics pipeline eagerly at creation instead of at its first bind (default on: lazy, under the live cap; the eager-build A/B arm). Diagnostics for comparing the driver's compiler memory (docs/memory_growth.md). |
| `PALMETTO_ALLOC_TRACE=1` | Rewrites every loaded module's allocator imports and records a 32-frame stack for each allocation of 1 MiB or more, with live bytes per stack; attribution only - it perturbs the driver's shader compiler enough to stall a boot, so do not use it for timing. Off by default. |
| `PALMETTO_MEM_TRACE=<seconds>` | Prints one bounded owner-counter line per interval: process private/working set, process heaps, the title XMem heap live/peak per caller, XMemVirtualAlloc and title VirtualAlloc counts, and physical-page live/mapped/committed. Off by default. |
| `PALMETTO_AGGREGATE_DEVICE_ID` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_ANALYTICS_FORM` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_ARENA_LOG` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_ARENA_MB` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_ARGV_SEQUENCE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_AUDIO_FRAMES` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_AUDIO_NO_SLEEP` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_AUDIO_TRACE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_AUDIO_TRACK` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BACK_IMAGES` | Keeps a back-image copy for the presentation path (diagnostic). |
| `PALMETTO_BASE_RESERVED` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BINDLESS_FUNCTIONS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BINDLESS_IDLE_WAIT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BINDLESS_LOAD` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BINDLESS_MEMBER` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BINK_INDEX` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BINK_LIMIT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BVH_DEVICE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BVH_DXIL` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BVH_FLAG` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BVH_HARDWARE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BVH_RECORD62` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_BVH_WARP` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CAPTURE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CAPTURE_EVERY` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CAPTURE_LIMIT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_COMPAT_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CONSERVATIVE_PS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CONSERVATIVE_TIER2_PS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CONSERVATIVE_TIER2_VS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CONSERVATIVE_TIER3_PS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CONSERVATIVE_VS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CONTENT_PACKAGES` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_CRASH_REPORT` | Writes a `.pdata`-walked stack report of the faulting thread; only for fault codes, not for handled C++ throws. |
| `PALMETTO_CRASH_REPORT_ALL` | The same walk for every other thread, once per process; only for fault codes. |
| `PALMETTO_D3D49_DUMP` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_D3D49_VERBOSE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DEPTH24_FUNCTIONS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DEPTH24_LOAD` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DEPTH24_MEMBER` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DEPTH_EDGE_VS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DEVICE_FN` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DEVICE_TYPE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DSTORAGE_TRACE` | Logs DirectStorage requests, completions and polls. |
| `PALMETTO_DS_ARRAYS` | Dumps every live status array (capacity, entry states, polls, threads). |
| `PALMETTO_DS_LIGHTBAR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DS_LOAD_TRACE` | Traces the load-window DirectStorage calls. |
| `PALMETTO_DS_STACK` | Captures the caller stack of DirectStorage status polls. |
| `PALMETTO_DS_TRIGGERS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DS_TRIGGER_FREQ` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DS_TRIGGER_GAIN` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_DUMP_REGS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FAKE_INPUT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FAULT_MEM` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FAULT_MEM_RVA` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FIRST_LIGHT_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP_AT_DRAW` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP_CONTROL` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP_CONTROL_PS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP_CONTROL_VS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP_PS_CONTROL` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP_SAMPLE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FRAME_DUMP_WINDOW` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FSR_DEVICE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FSR_NO_STANDIN` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_FSR_WARP` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_GAMEINPUT_THREAD` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_GAMERTAG` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_GAME_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_GOTO` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_GRAPHICS_CONSOLE_ROOT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_HAVE_ABI_PROBE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_HDR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_HOST_ADAPTER` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_INPUT_TRACE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_INSTANCE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_INSTANCE_FN` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_KEYBOARD_PAD` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_LIST_SLOT_TRACE` | Traces the console command-list recording slots the recorder calls. |
| `PALMETTO_LOADER_BEGINEND` | Logs the loader begin/end entry calls with thread and object state. |
| `PALMETTO_LOADER_FUZZ_ITERS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_LOADER_FUZZ_SECONDS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_LOADER_FUZZ_SEED` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_LOADER_HOOK` | Installs entry detours on the loader begin/end/re-init/completion entries. |
| `PALMETTO_LOADER_POLLBP` | Breakpoint control for the loader poll loop (diagnostic). |
| `PALMETTO_LOADER_WATCH` | Hardware-watch the loader object's armed byte and in-flight count. |
| `PALMETTO_LOAD_TRACE` | Logs device/queue/fence calls in a time window during the load. |
| `PALMETTO_LOG_GAME_EVENTS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_M15_GUEST` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_MOUSE_SENSITIVITY` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_MOVIE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_MOVIE_AUDIO` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_MOVIE_FRAME` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_NO_SUCH_ALG` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_ORACLE_DUMP` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PACKAGED` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PACKAGE_ID` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PACKAGE_READY` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PACKAGE_V4` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PAGE_PULL` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PC_TAIL_UBO` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PIPELINE_TIMELINE` | Prints a periodic summary of pipeline creation and submission counters. |
| `PALMETTO_POKE_HOST` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_POKE_LOG` | Logs the one shaped loader poke (diagnostic). |
| `PALMETTO_PRESENT_ADOPT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PRESENT_HEIGHT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PRESENT_NO_TEAR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PRESENT_STUB` | Answers present requests without presenting (measurement). |
| `PALMETTO_PRESENT_UNCAP` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PRESENT_WIDTH` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PRESENT_WINDOW` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PROMPTS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_PROMPTS_SWITCH_MS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_QUEUE_PROXY_ISOLATE_ALL` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_QUEUE_PROXY_JOBS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_READBACK_AT_DRAW` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_READBACK_CENTRE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_READBACK_FULL` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_REPLAY_MISS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RGB96_LARGE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RIP_HIST` | Samples every thread's instruction pointer into a histogram in named windows. |
| `PALMETTO_ROOT_DATA_TRACE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RT136_A` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RT136_B` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RT136_C` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RT_AS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RT_ENV_INIT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RUMBLE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_RUN_DLLMAIN` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SAVE_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SCENE_AUDIO` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SCENE_MODE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SCRATCH_FUNCTIONS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SCRATCH_LOAD` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SCRATCH_MEMBER` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SDR_EOTF` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SHADER_DXBC` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SIG_CLASS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SKIP_CPU_CHECK` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SLOT70_CBV` | Opt-in console slot-70 constant-buffer-view conversion (falsified). |
| `PALMETTO_SLOT70_CENSUS` | Census of console slot-70 call shapes. |
| `PALMETTO_SLOT70_IDENTITY_VA` | Resolves a console slot-70 destination address against the creation registry. |
| `PALMETTO_SLOT70_PAIR` | Pairs console slot-70 texture publications with the resource created before them. |
| `PALMETTO_SLOT70_PROBE` | Probes the console slot-70 call shape. |
| `PALMETTO_SLOT70_PUBLISH` | Publishes the slot-70 record block as a matrix view. |
| `PALMETTO_STANDIN_PIPELINES` | Builds stand-in graphics pipelines when a record cannot be converted. |
| `PALMETTO_STREAM_CONSUMER` | Guards and labels the streamed-block consumer's first touches. |
| `PALMETTO_SWAPCHAIN_MAINTENANCE1` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SWEEP_PS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_SWEEP_VS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEB_CHECK` | Validates the thread environment block offsets before the loader runs. |
| `PALMETTO_TEST_BUFFER_UAV_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_CS_R9G9B9E5_UAV_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_CS_RAYQUERY_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_CS_SAMPLE_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_CS_SET0_SAMPLE_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_CS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_FS_SOLID_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_GATHER_ALPHA_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_GS_DEPTH_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_GS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_INDIRECT_COUNT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_M15_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_M2F_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_M2_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_M3_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_M4_DIR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_MSAA_FRAG_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_MSAA_VERT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_MS_FLOAT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_MS_SINT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_MS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_MS_UINT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_PS_ATOMIC_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_PS_DEPTH_EMPTY_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_PS_DEPTH_WRITE_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_PS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_PS_UINT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_SUBGROUP_SHADERS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_VS_DEPTH_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_VS_FULLSCREEN_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_VS_INT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_VS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEST_VS_TRI_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEXTURE_BIND_FETCHED` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEXTURE_IMPORT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEXTURE_IMPORT_MAX` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TEXTURE_PAGE_WATCH` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_THREAD_STACKS` | Captures every thread's stack at named times. |
| `PALMETTO_TITLE_PEEK` | Reads named words of the title process on a timer. |
| `PALMETTO_TLS_GA_PATCH` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TYPELESS_PS_FLOAT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TYPELESS_PS_UINT_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_TYPELESS_VS_SPV` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_UNIMPLEMENTED` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_UPLOAD_SYSMEM` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_USER_DATA_SHIFT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_USER_TRACE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VERTEX_MATRIX_DESCRIPTOR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VERTEX_PATH` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VERTEX_STREAM_DESCRIPTOR` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VK_DEVICE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VK_DEVICE_FUNCTIONS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VK_GRAPHICS_FAMILY_ONLY` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VK_LOAD` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VK_MAX_QUEUES_PER_FAMILY` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VK_MEMBER` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VK_VALIDATION` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VULKAN_DEVICE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_VULKAN_LOADER` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_WAIT_SAMPLE` | Samples the title's wait states on a timer: per thread the wait API, handle or wait-on-address key, object type and state, the timeout, and the caller's short stack. |
| `PALMETTO_WAIT_TRACE` | Names every host wait a title thread enters - API, caller site, object, timeout, outcome - matches set/wake calls against those objects, and dumps the aggregate wait sites, object correlation and still-outstanding waits at the named times (docs/wait_forensics.md). |
| `PALMETTO_WATCH_GLOBAL` | Watches a title global and reports writes. |
| `PALMETTO_WATCH_RVA_WRITE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XBOX_PRESENT_HOOK_ONLY` | Keeps the PresentX hook without presenting. |
| `PALMETTO_XBOX_VIEW_SHAPE` | Probes the console view-creation call shapes. |
| `PALMETTO_XDXR_FUNCTIONS` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XDXR_LOAD` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XDXR_MEMBER` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XG_DUMP` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XG_LAYOUT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XG_TRACE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_BAD_PACKET_SIZE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_DECODER_FAILURE` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_DECODER_H` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_INVALID_ARGUMENT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_INVALID_FORMAT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_OK` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_OUT_OF_MEMORY` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_PACKET_BYTES` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XMA2_PANIC` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XS_CALLERS` | Logs the caller and argument shape of every console call (first call per site). |
| `PALMETTO_XS_CALLERS_PROFILE` | Adds per-site call counts to the console caller probe. |
| `PALMETTO_XS_CLEAR_BARRIER_PROBE` | Probes the console clear and barrier recording calls. |
| `PALMETTO_XS_FRAME_INTERVAL_SLOT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XS_LIST_DUMP` | Dumps the arguments of named console list slots. |
| `PALMETTO_XS_LIST_ID` | Logs which console list proxy a recording call lands on. |
| `PALMETTO_XS_LIST_MUTE` | Suppresses named console list recording calls (measurement). |
| `PALMETTO_XS_LIST_X_SLOT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XS_QUEUE_X_SLOT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XS_SCHEDULE_EVENT_SLOT` | Runtime diagnostic switch; opt-in and off by default. |
| `PALMETTO_XS_SLOT57_LAYOUT_DUMP` | Dumps the slot-57 input layout of the first records. |
| `PALMETTO_XS_WAIT_EVENT_SLOT` | Runtime diagnostic switch; opt-in and off by default. |

## Guaranteed pull-request checks

Two workflows start on every pull request: `CI` (`.github/workflows/ci.yml`) and `No Game Data`
(`.github/workflows/no-game-data.yml`). The `Runtime` workflow builds and tests the port when a pull request
touches the paths it lists (`port/`, `vendor/`, `tooling/scripts/`, `.gitattributes` and the `tooling/` files its
jobs read), and `jactionlint` parses the workflow files when `.github/` changes. None of the checks needs a GPU,
game data or the retail package.

- **Every pull request** - `CI` compiles the Python tree, runs the standard-library test loop (one `python -I` run
  per test file without a pytest or package dependency), the pytest files, the translator semantic suite and the
  viewer streaming tests.
- **Documentation only** - when the only changes are `README.md`, `CONTRIBUTING.md`, `AGENTS.md` or a Markdown
  page under `docs/`, `CI` skips the build and test jobs and runs the tests that read those documents instead:
  every `tests/test_docs_*.py`, `tests/test_design_contract_rows.py` (it reads `docs/design.md`) and
  `tests/test_i18n_index.py`.
- **No game data** - `No Game Data` rejects a tracked game or binary extension and runs
  `tooling/scripts/check_no_game_data.py` over the tree, which reads the first bytes so a renamed file is caught.
- **Runtime paths** - `Runtime` configures and builds the port, then runs `ctest`, the GPU-free contract targets
  among them, on Windows, on Linux with Mesa's llvmpipe and on macOS. The build step runs before ctest because a
  test target whose link fails would otherwise be silently absent from a green ctest ([testing.md](testing.md)
  covers that trap).

Two registration tests keep those guarantees from drifting. `tests/test_ci_registration.py` fails when a test
file no job selects, when a document test is not run for a documentation-only pull request, and when `Runtime`
would stop building and running `ctest` for a pull request that changes the port. `tests/test_runtime_paths.py`
recomputes the `tooling/` files the `Runtime` jobs read and fails when the path filter lists one less. Run them,
with the checks they guard, from the checkout root:

```powershell
python -I tests/test_ci_registration.py
python -I tests/test_runtime_paths.py
python -I tests/test_ctest_registration.py
foreach ($t in Get-ChildItem tests/test_docs_*.py) { python -I $t.FullName }
python -I tests/test_design_contract_rows.py
python -I tests/test_i18n_index.py
```

The GPU-free runtime contract targets need the port built. On Linux or macOS configure the portable subset and
run ctest:

```sh
cmake -S port -B port/build
cmake --build port/build --parallel
ctest --test-dir port/build --output-on-failure
```

## Console vtable slot maps

[console_slots.md](console_slots.md) keeps the measured map of the console D3D12 vtable slots. The
reverse-engineering workflow that extends it (a Ghidra installation plus the scripts that turn a boot's
caller census into decompiled functions and compare console slots with the PC SDK interfaces) is
maintained outside this repository: those are analysis aids and third-party installations, not build
inputs. The page records the evidence and the confidence of every row, so the map can be extended
without the tooling living here.
