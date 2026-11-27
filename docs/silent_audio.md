# The title's WASAPI output path on this host: measured, and why there is no silent endpoint

This page records a measurement, not a port. The public upstream fork serves a **silent output endpoint** (its
commits `d4ce33ed4` "runtime: serve a silent output endpoint when the host has none" and `018145b33` "serve the
spatial stream on the silent output endpoint", files `silent_audio.cpp`/`.h`, its `com_shim.cpp` hook and
`docs/runtime_imports.md` section "Audio output off the console (WASAPI)") for a host that has no render endpoint: there
`GetDefaultAudioEndpoint(eRender, ...)` answers `0x80070490` (`ERROR_NOT_FOUND`), the title's device start takes an error
path that leaves its own output-device lock held by a job worker, its audio engine never mixes and the boot stops on an
`int3` about 47 s in (measured under Wine with its audio drivers disabled). Before porting that slice, the same question
was measured on this host. The answer is that this host's WASAPI path works: the title gets a usable spatial client,
creates and starts its spatial stream, and its mixer runs for the rest of the boot. **The silent endpoint is not
implemented, no `PALMETTO_AUDIO` switch is added, and the tree is unchanged** - the measurement instrumentation was
temporary and is not shipped.

Upstream here is a reference and an evidence source under GPL-3.0, not a code source; nothing from its files was copied,
and the instrumentation below was written for this measurement. The porting policy is in `docs/upstream_port.md`.

## 1. Why the question was worth a boot

The boot's loading state does not advance: the loader job goes dormant after about 13.5 s, a stationary two-pass frame
loop runs to the timeout and the presented image stays black (`docs/streamed_consumer.md` section 6e,
`docs/loading_gate.md`). A held output-device lock would fit that shape from the CPU side, so the audio path was a
plausible cause worth one bounded measurement. It is not the cause: the audio objects are created at +22.34 s, after the
loader's last enqueue at about +13.5 s, and every audio call then succeeded.

## 2. Static shape of the title's WASAPI path

A byte search of the retail image for Windows audio GUIDs finds exactly five, and no other WASAPI interface id or class
id:

| Symbol | Meaning |
|---|---|
| `CLSID_MMDeviceEnumerator` | the in-process device enumerator the title creates with `CoCreateInstance` |
| `IID_IMMDeviceEnumerator` | the interface it asks that class for |
| `IID_IAudioClient` | the shared-mode render client it activates on the endpoint |
| `IID_ISpatialAudioClient` | the spatial client it activates on the same endpoint |
| `IID_ISpatialAudioObjectRenderStream` | the stream interface it asks the spatial client for |

No `IAudioClient2/3`, no `ISpatialAudioClient2`, no `IAudioRenderClient`, no `IAudioClock` or volume interfaces, no
`IMMEndpoint` and no `IPropertyStore` appear in the image, so the path is exactly the one upstream describes. That also
bounds what instrumentation had to cover.

## 3. Method: bounded instrumentation, then one 130 s boot

`port/shims/com_shim.cpp` forwards `CoCreateInstance` to the host. For the measurement it was changed so that, with
`PALMETTO_AUDIO_TRACE=1` only, every guest `CoCreateInstance` was logged and `CLSID_MMDeviceEnumerator` was created with
the host and returned inside a forwarder. The forwarder wrapped `IMMDeviceEnumerator`, `IMMDevice`, `IAudioClient`,
`ISpatialAudioClient` and `ISpatialAudioObjectRenderStream`, forwarded every call unchanged, and logged each call's
`HRESULT`, thread id, caller module+RVA and duration: the first eight calls of each entry point and then every tenth
power, and always a call that took a second or longer (the runtime's own bounded rule, `port/shims/platform_trace.h`).
On the first enumerator the same switch ran one read-only probe of the host's own render path, using the host objects
directly: the default endpoint for each of the three roles, a spatial-client activation, a mono 48 kHz float object
format check, `GetMaxFrameCount`, an audio-client activation, `GetMixFormat` and `GetDevicePeriod`. The probe neither
initialised nor started an audio client, so it opened no device for itself. The title's own calls then went through the
wrapped interfaces: it activates `IAudioClient` only for `GetMixFormat`, never `Initialize`s or `Start`s it, and outputs
through `ISpatialAudioObjectRenderStream`, whose `Start` and 10 ms `BeginUpdatingAudioObjects` passes were logged.

With the switch unset, `CoCreateInstance` went straight to the host, as it still does: the temporary header was deleted
after the measurement and `com_shim.cpp` is identical to `HEAD`, so **no shipped code changed and no switch remains**.

One harness boot (`tooling/boot_report.py`), 130 s, standard environment: `PALMETTO_SHADERS` pointing at the translated
shader corpus, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, plus
`PALMETTO_AUDIO_TRACE=1` and `PALMETTO_PIPELINE_TIMELINE=5`; one boot at a time under `_setup/boot_lock.ps1`; out and
overlay folders under the work tree, outside the repository. The 70 audio lines it printed are the evidence for every
number below.

## 4. The measured host result

On the first enumerator the title creates (at +22.34 s), the host's own path for all three render roles was:

| Question | eConsole | eMultimedia | eCommunications |
|---|---|---|---|
| `GetDefaultAudioEndpoint(eRender, role)` | `S_OK`, 1.52 ms | `S_OK`, 1.20 ms | `S_OK`, 1.19 ms |
| `IMMDevice::GetState` | `DEVICE_STATE_ACTIVE` (1) | `DEVICE_STATE_ACTIVE` (1) | `DEVICE_STATE_ACTIVE` (1) |
| `Activate(IID_ISpatialAudioClient)` | `S_OK`, 2.15 ms | `S_OK`, 0.04 ms | `S_OK`, 0.04 ms |
| `ISpatialAudioClient::GetMaxDynamicObjectCount` | 0 | 0 | 0 |
| `ISpatialAudioClient::GetNativeStaticObjectTypeMask` | `0xFFFFE` | `0xFFFFE` | `0xFFFFE` |
| `IsAudioObjectFormatSupported` (mono, 48 kHz, float) | `S_OK` | `S_OK` | `S_OK` |
| `GetMaxFrameCount` (same format) | `S_OK`, 480 frames | `S_OK`, 480 frames | `S_OK`, 480 frames |
| `Activate(IID_IAudioClient)` | `S_OK` | `S_OK` | `S_OK` |
| `GetMixFormat` | tag 65534 (`WAVEFORMATEXTENSIBLE`), 8 ch, 48 kHz, 32-bit; subtype GUID not recorded | same | same |
| `GetDevicePeriod` | default 10 ms, minimum 3 ms | same | same |

So this host has an active default render endpoint for every role, an `ISpatialAudioClient` on each of them and the full
static-object layout (`0xFFFFE`), with no dynamic objects (ordinary endpoint, no spatial-sound format enabled). The
retained `GetMixFormat` trace records `WAVEFORMATEXTENSIBLE` tag 65534, 8 channels, 48 kHz and 32 bits, but not the
subtype GUID; it does not establish whether the shared mix is float or integer PCM. Separately, the title's own
spatial pre-mix request is `WAVE_FORMAT_PCM` (tag 3), mono, 48 kHz, 32-bit and succeeds (section 5); the read-only probe's
mono-float spatial-format check is a distinct query, not evidence about the shared-mix subtype. There is no
`0x80070490`, no null sink and no error path at the host boundary.

## 5. The title's own path

The title's own calls, through the same wrappers, around +22.36 s to +22.38 s:

| Step | Answer |
|---|---|
| `CoCreateInstance(CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator)` | `S_OK`, 0.02 ms |
| `IMMDeviceEnumerator::RegisterEndpointNotificationCallback` | `S_OK` |
| `IMMDeviceEnumerator::EnumAudioEndpoints(eRender, ACTIVE)` | `S_OK` |
| `IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)` | `S_OK`, 1.15 ms |
| `IMMDevice::GetId` | `S_OK`, a 55-character endpoint id |
| `IMMDevice::Activate(IID_IAudioClient)` | `S_OK` |
| `IAudioClient::GetMixFormat` | `S_OK`, WAVEFORMATEXTENSIBLE tag 65534, 8 ch, 48 kHz, 32-bit; subtype GUID not recorded |
| `IMMDevice::Activate(IID_ISpatialAudioClient)` | `S_OK` |
| `ISpatialAudioClient::GetSupportedAudioObjectFormatEnumerator` | `S_OK` |
| `ISpatialAudioClient::IsAudioObjectFormatSupported` (WAVE_FORMAT_PCM tag 3, mono, 48 kHz, 32-bit) | `S_OK` |
| `ISpatialAudioClient::ActivateSpatialAudioStream(IID_ISpatialAudioObjectRenderStream)` | `S_OK`, 5.19 ms |
| `ISpatialAudioClient::GetStaticObjectPosition` (front left, front right) | `S_OK`, `S_OK` |
| `ISpatialAudioObjectRenderStream::Start` | `S_OK` |

From +22.38 s a second thread of the title (one stable thread id) runs the mixer: `BeginUpdatingAudioObjects`
(`available=0`, `frames=480`), `ActivateSpatialAudioObject` (front left, then front right),
`EndUpdatingAudioObjects`, all `S_OK`. The bounded logging shows the opening eight passes between +22.38 s and +22.45 s,
the tenth at +22.47 s, the hundredth at +23.37 s, the thousandth at +32.37 s and the ten-thousandth at +122.37 s: a
continuous 480-frame pass, i.e. 10 ms at 48 kHz, at the real-time rate of about 100 passes per second for the whole
window. No call after `Start` failed.

Across all 70 logged calls: **0 non-zero `HRESULT`s, 0 calls of a second or longer, no `[slow]` line**. The slowest call
of the whole path is the 5.19 ms `ActivateSpatialAudioStream`; the generator was `S_OK` and the stream ran. There is no
error path and no device-lock-looking wait to observe.

## 6. Boot numbers

One 130 s boot, killed by its timeout as intended: **ALIVE 130.2 s, 0 exception events**, 542,829 D3D12 calls recorded
(max `#544969`), `replay: submission 24576 at 106.2 s`, and at the +125 s timeline line: 51,786 graphics pipelines
created, 10,241 slot-57 records converted with 0 refused, 25,493 submissions and 12,746 presents. The frame loop is the
same stationary two-pass blit loop as the earlier 130 s timeline boot (535,283 calls, 25,093 submissions, 12,546
presents at +125 s, submission 24,576 at 104.6 s in `docs/streamed_consumer.md` section 6e); the few-percent difference
between the two boots is the run-to-run spread that page already notes. The loading state did not advance in either
boot, and the audio path was fully working in this one, so a silent endpoint would not have changed it.

There is no A/B pair against a `PALMETTO_AUDIO=silent` build because that build does not exist: the measurement is the
decision, and it says there is nothing to port for this host.

## 7. Decision

**No silent endpoint, no switch, no code.** The precondition the upstream slice serves is absent here: this host
provides an active default render endpoint and a working `ISpatialAudioClient`, the title activates both, creates and
starts its spatial stream, and its mixer thread keeps running. The loading stall is not downstream of the audio path
(it starts before the audio objects are created and the audio path never fails). Implementing a discarding endpoint
would add a second, untested output path for a failure this host does not have, so the honest outcome is this negative.

A future host that does fail this path, for example a headless or Wine-style host with no render endpoint, is the case
the upstream slice covers, and this measurement fixes what such a re-derivation must answer: `GetDefaultAudioEndpoint`
returning `0x80070490`, `EnumAudioEndpoints` empty, `Activate(IID_ISpatialAudioClient)` failing or an
`ISpatialAudioClient` with no usable static objects, and an `ISpatialAudioObjectRenderStream` that cannot be created or
started. A silent device for that case would have to answer the same five interfaces measured here (see section 2) with
the same answers measured in section 5, which is the contract the title has already been seen to accept.

## 8. Limits

- Measured on one Windows 11 host with real, active render endpoints. The captured shared-mix answer was
  WAVEFORMATEXTENSIBLE tag 65534, 8 channels, 48 kHz, 32-bit; the retained trace omitted its subtype GUID, so this page
  makes no float-versus-PCM claim about that mix. A host whose default endpoint is absent, disabled or proprietary may
  answer differently; this page does not generalise to such a host.
- Nothing audible was produced, claimed or checked: the title's stream was started and driven, but the host's own
  endpoint was not monitored and the stream's samples were not inspected.
- The probe was deliberately read-only. It did not `Initialize` or `Start` an audio client; the title's own
  `IAudioClient` was activated for `GetMixFormat` only (the log has no `Initialize` or `Start` on it), and its output
  path is the spatial stream, whose `Start` and passes were logged.
- The title's path was observed for the whole 130 s window only at the bounded-logging samples (first eight calls, then
  tenth powers, plus any call of a second or longer); a failure between those samples would have been logged only if it
  also took a second. Every sampled call succeeded.
- No tests were added because no code ships. The measurement used the existing gates' tree (`build_gta6rt.cmd`,
  `build_tests.cmd`) and no new target, file or switch.

## 9. Credit

The silent-output-endpoint design, its measured stall signature and the console-audio context this page refers to are
the work of the **public upstream fork** (GPL-3.0): commits `d4ce33ed4` and `018145b33` (`runtime/shims/silent_audio.*`,
the `com_shim.cpp` hook, `runtime/test/silent_audio_test.cpp` and the "Audio output off the console (WASAPI)" section of
its `runtime_imports.md`). They were read as reference and evidence only; the measurement instrumentation behind this
page was written for this tree and removed afterwards. The upstream project and its authors are credited through
`NOTICE.md` by the porting slice that owns the attribution.
