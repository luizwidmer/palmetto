// The backend that turns a console slot-57 graphics pipeline record (port/src/graphics_record.h) into what the PC
// layer builds a real graphics pipeline from:
//
//   - resolve_native_stage(): the native branch of PipelineState::create_graphics (port/shims/d3d12_vulkan_device.cpp).
//     The title's precompiled pipeline-library graphics programs are native RDNA2 programs, not FSSM/DXBC
//     (docs/fspx.md, docs/pipeline_program_source.md), and their identity is the SHA-256 of the program bytes as
//     passed - the shader index's fspx_code_sha256, the same key create_compute already resolves compute programs
//     with. This resolves one graphics stage the way create_compute resolves a compute shader, under the stage's own
//     name ("ps", "vs", "ms", "gs"), and refuses a program whose translations differ when no kind-0 state can pick
//     between them instead of choosing one.
//   - build(): a decoded record plus the caller's inputs -> D3D12_GRAPHICS_PIPELINE_STATE_DESC.
//
// Neither function creates a device object, needs a GPU or reads a game file: the tests build every byte and the
// shader index themselves (port/test/graphics_pso_test.cpp).
//
// What the console record does not yield is an explicit input, never a default: the root signature (the decoder copies
// only the pointer value) and the whole fixed-function state, because the 380..520-byte state blob's container decodes
// but no word of it is pinned to a D3D12 field (docs/state_blob.md) - decode_fixed_function_state() refuses all eight
// fields by name and returns false. GraphicsPsoInputs::defaults is that state, every field the caller's, and
// documented_default_state() is the one set the project documents as the measurement stand-in for the undecoded blob.
// A field is pinned only when a behavioural probe can tell its values apart; run_probe() below is that probe's
// GPU-free model, and docs/state_blob.md section 9 has the probe table and what a pin would still need.
// What the record cannot prove is a refusal with the field named in `why`, never a guess: a layout element whose format
// word is not one of the measured DXGI values, whose input slot, input-slot class or instance step rate is not the
// measured 0, whose aligned byte offset is not the measured packed one, or whose semantic name/index duplicates another
// is refused by name (docs/input_layout.md); nothing is mapped by analogy.
//
// docs/graphics_pso.md has the input contract, the refusal table, the documented default set and the wiring;
// docs/input_layout.md has the measured input-layout element table and the format-word decision; docs/state_blob.md has
// the state record's measured container and the histogram of what decodes and what refuses.
#pragma once

#include <windows.h>
#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "graphics_record.h"
#include "root_signature.h"
#include "shader_library.h"

namespace rt::graphics_pso {

// Among the index entries for one program (containers or translations of the same key), the one to build: the first
// that `fits` and declares no set-0 bindings (the cheaper pipeline); else the first that fits; else the first without
// bindings; else the first. Null when there is none. Shared by create_graphics and create_compute, so a translated
// FSSM program and a native graphics program are chosen by the same rule.
template<typename Fits>
const palmetto::ShaderEntry* pick_entry(const std::vector<const palmetto::ShaderEntry*>& candidates, Fits&& fits) {
    const palmetto::ShaderEntry* fitting = nullptr;
    const palmetto::ShaderEntry* unbound = nullptr;
    for (const palmetto::ShaderEntry* e : candidates) {
        const bool fit = fits(*e);
        if (fit && !e->binding_count) return e;
        if (fit && !fitting) fitting = e;
        if (!e->binding_count && !unbound) unbound = e;
    }
    if (fitting) return fitting;
    if (unbound) return unbound;
    return candidates.empty() ? nullptr : candidates.front();
}

// One graphics stage's native program, as the shader library resolves it.
struct NativeStage {
    // The program the bytecode named: its kind-6 bytes - which the pipeline copies to the GPU address arena as the
    // stage's code base, exactly as create_compute does - and its kind-0 dispatch state, when the bytecode carried one
    // (an FSPX payload or a DXBC container with an ACCC part). Empty when nothing was classified.
    palmetto::ComputeProgram program;
    // The library entry to build the stage from, or null when the program did not resolve (then `why` says why).
    const palmetto::ShaderEntry* entry = nullptr;
    // True when the bytes were not a program compute_program() recognises (no FSPX prologue) and were looked up by
    // the SHA-256 of their own bytes instead: the publication's key for the library's graphics programs
    // (docs/pipeline_program_source.md). Recorded so the caller's log says which identity matched.
    bool raw_key = false;
};

// The native branch of PipelineState::create_graphics for one graphics stage whose D3D12_SHADER_BYTECODE holds no
// FSSM blob. `stage` is the record slot's stage name ("ps", "vs", "ms", "gs"); the stage blob carries no identity, the
// slot does (docs/shader_library.md section 2). `fits` is create_graphics' per-entry test: a vertex shader's inputs
// must be fed by the PSO's input layout, the other stages pass true.
//
// Returns false when the bytes are not a native graphics program this tree can name (no FSSM blob, no usable FSPX
// program, and no shader-index entry under the SHA-256 of the bytes): the caller's FSSM/DXBC and stand-in paths, and
// their refusal, apply. Returns true when they are one; then NativeStage::entry is the entry to build, or null with
// `why` naming the refusal. `why` is cleared on entry and set only on a refusal; both may be null when unneeded.
bool resolve_native_stage(std::span<const std::uint8_t> bytecode, const char* stage,
                          const palmetto::ShaderLibrary& library,
                          const std::function<bool(const palmetto::ShaderEntry&)>& fits, NativeStage* out,
                          std::string* why);

// The fixed-function state a PC graphics pipeline needs and the console record's 380..520-byte state blob has not
// yielded: rasterizer, blend, depth-stencil, topology type, index-buffer strip cut and the sample description
// (docs/graphics_record.md section 4). Every field is the caller's: this struct has no defaults of its own and build()
// has no fallback, so a state substituted for the undecoded blob is always an explicit input. Fill every field - a
// zeroed one is refused with its field named, not treated as "the console's".
struct FixedFunctionState {
    D3D12_RASTERIZER_DESC rasterizer;
    D3D12_BLEND_DESC blend;
    D3D12_DEPTH_STENCIL_DESC depth_stencil;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut;
    UINT sample_count;
    UINT sample_quality;
    UINT sample_mask;
};

// The project's documented default set for FixedFunctionState (docs/graphics_pso.md), for a caller that has only the
// undecoded blob: triangle list, solid fill, back-face culling, depth clipping on, blending off, depth and stencil
// off, sample count 1 with the full mask. It is a measurement stand-in, not the console's state: passing it is a
// decision the caller makes and logs (the create_standin_graphics_pipeline defaults are the same set).
FixedFunctionState documented_default_state();

// ---- Where the graphics bind point's root arguments actually go: the console user-SGPR layout ---------------------
//
// A translated shader reads its root arguments from physical user SGPRs, and for a graphics stage the console base is
// s8 for vertex/pixel/geometry and s16 for mesh (docs/d3d12_vulkan.md section 3.5: `rt::rootsig::pack_stage`, measured
// for compute, a census hypothesis for the graphics stages). From that base a root signature is laid out in parameter
// order, each parameter whole: a 32-bit constant one dword per value, a root CBV/SRV/UAV two (its 64-bit VA) and a
// descriptor table **one** (the 32-bit VA of its first descriptor; the heaps live below 4 GiB).
//
// The port feeds the graphics bind point with the *PC* packing instead (`rt::rootsig::pack` from s0, two dwords per
// table; docs/d3d12_vulkan.md section 3.5 "D3D-448"), so a translated vertex or pixel shader that reads the parameter
// at its console slot reads zero. This is not a record decode: the slot-57 record carries no root-argument values at
// all (docs/root_arguments.md), and the one root argument the title does set - the descriptor table it sets before the
// draw - arrives but at the PC slot. `console_graphics_user_data()` exposes the console slots so the feed path can fill
// them from the same values it already feeds, which is the A/B the vertex-path measurement needs.
struct ConsoleUserDataPlacement {
    bool ok = false;
    std::string error;  // set when !ok; names the refusing parameter
    rt::rootsig::Stage stage = rt::rootsig::Stage::Vertex;  // the base this placement used
    std::uint32_t base = 0;                                 // the stage's first user SGPR
    // Per parameter, its first console user SGPR (rt::rootsig::kNoSgpr when the signature is refused).
    std::vector<std::uint32_t> first_sgpr;
};

// Places `signature`'s parameters in the console user-SGPR layout for a graphics pipeline: mesh base s16 when the
// pipeline carries a mesh stage, the vertex/pixel/geometry base s8 otherwise. Returns false (with `error` naming the
// parameter that does not fit) when the signature would spill, so a caller refuses instead of truncating; `out` is
// otherwise filled with the per-parameter first SGPR. No device and no game file: the tests build the signatures.
bool console_graphics_user_data(const rt::rootsig::RootSignature& signature, bool mesh_pipeline,
                                ConsoleUserDataPlacement* out);

// Which of FixedFunctionState's eight fields a decoded state record yielded. The state blob's container decodes
// (rt::graphics_record::decode_state, docs/state_blob.md) but no measurement of this title's data pins any of its
// words to a fixed-function field, so every member is false today; the struct exists so the refusal is per field and
// the slot-57 histogram can key on the field that stops a decode.
struct FixedFunctionFieldStatus {
    bool rasterizer = false;
    bool blend = false;
    bool depth_stencil = false;
    bool topology_type = false;
    bool strip_cut = false;
    bool sample_count = false;
    bool sample_quality = false;
    bool sample_mask = false;
};

// The eight field names FixedFunctionFieldStatus reports, in the order above (for logs and tests).
inline constexpr const char* kFixedFunctionFieldNames[8] = {
    "rasterizer", "blend", "depth_stencil", "topology_type", "strip_cut", "sample_count", "sample_quality",
    "sample_mask",
};

// Tries to fill `out` with the fixed-function state the decoded state record carries. Returns false, because the
// 380..520-byte state record is the console driver's serialized pipeline state (docs/state_blob.md): its container is
// decoded, but no word of it is pinned to a D3D12 rasterizer, blend, depth-stencil, topology, strip-cut or sample
// field by any measurement of this title's data, so filling `out` would be a guess. `why` names every refused field
// and the histogram field prefix is `state.fixed_function`; `fields` (when given) is set to all-false, and `out` is
// untouched. A caller that must still build uses documented_default_state() and logs the substitution, which is what
// the slot-57 wiring does (docs/slot57_wiring.md). A state record the container decoder refused (State::size 0) is
// refused as `state` instead, because nothing at all was decoded.
//
// What a pin would take is the behavioural probe below: the record's words are only a candidate *place* for a field,
// never a value, and the probe says which values a scene can tell apart. Decoding is refused until an observation of
// this title's own draw through the record exists to compare against; today it does not (the title's draws produce
// no coverage, docs/frame_capture.md sections 6-8), so every field stays refused.
bool decode_fixed_function_state(const rt::graphics_record::State& state, FixedFunctionState* out,
                                 FixedFunctionFieldStatus* fields, std::string* why);

// ---- The behavioural probe of a fixed-function field -------------------------------------------------------------
//
// A field is pinned to a word of the serialized state record only when a *behavioural probe* tells a value of that
// field apart: a minimal scene rendered through the state whose observable result differs between the candidate
// values. This is the falsifier the project requires before replacing the documented stand-in - a decode that cannot
// be told apart from a guess is not a decode. The probe is also what makes a refusal testable: the same scene under
// two candidate values must produce two different observations, or the candidate is not measurable at all.
//
// run_probe() is the GPU-free, deterministic model of that observation: a 4x4 RGBA8 colour target, a 4x4 depth
// buffer and one fixed minimal scene per probe. It creates no device and reads no game file, so the model is the
// *contract* for the measurement; the measurement itself is what the title's own draw through the record would have
// to produce, and that does not exist (docs/frame_capture.md sections 6-8: the title's vertices produce no coverage).
// docs/state_blob.md section 9 has the probe table, what each probe observes and what stays unobservable even in
// principle (sample quality).
enum class ProbeScene {
    // Two triangles placed over the left and the right half of the target with opposite screen winding. Observes the
    // rasterizer's cull mode and front-face flag: four (cull, front) combinations cover a different half.
    kWindingPair,
    // A red near layer then a green far layer over the same pixels. Observes depth enable, depth func and depth write:
    // the surviving colour is red only when the far layer fails the test after the near layer wrote.
    kTwoDepthQuads,
    // Two overlapping layers of known colour and alpha. Observes blend enable, the colour/alpha factors, the blend
    // operation and the render-target write mask, through the resolved colour.
    kOverlap,
    // Twelve vertices drawn as a triangle list (four triangles covering the target). Observes the topology type: a PSO
    // type that does not match the draw's list class assembles nothing (D3D12 rejects the pair), a matching type
    // assembles the four triangles.
    kTopologyList,
    // An indexed triangle strip whose index list holds a cut sentinel (0xFFFF) and a vertex list that does not.
    // Observes the index-buffer strip cut: a value equal to the sentinel restarts the strip; a value that does not
    // match reads it as an out-of-range vertex index, which the probe counts.
    kStripCut,
    // A small triangle over one pixel of the 4x4 target that covers one of the four sample positions and not the
    // pixel centre. Observes the sample count (covered samples and resolved colour) and the sample mask.
    kSampleGrid,
    // A full-screen quad whose vertices are outside the depth range. Observes depth clipping: with clipping on the
    // quad is removed, with clipping off the API clamps its depth and draws it.
    kDepthClip,
};

// What one probe run observed. Equality is the probe's own: two states are behaviourally distinguishable by a scene
// exactly when their outcomes differ.
struct ProbeOutcome {
    std::uint32_t primitives = 0;    // primitives assembled from the scene's vertices and indices
    std::uint32_t mismatch = 0;      // 1 when the state's topology type and the scene's list class disagree
    std::uint32_t culled = 0;        // primitives the rasterizer's cull mode removed
    std::uint32_t clipped = 0;       // primitives depth clipping removed
    std::uint32_t fragments = 0;     // sample fragments that reached the pixel stage (after cull, clip and sample mask)
    std::uint32_t depth_failed = 0;  // fragments the depth test rejected
    std::uint32_t written = 0;       // fragments that reached the colour write stage
    std::uint32_t out_of_range = 0;  // strip indices the cut did not cut and the vertex list does not hold
    std::uint32_t unmodelled = 0;    // state values the model cannot express (an invalid sample count, an unknown factor)
    std::array<std::uint8_t, 16> coverage{};  // covered samples per pixel, row-major 4x4
    std::array<std::uint8_t, 64> colour{};    // resolved RGBA8 per pixel, row-major 4x4
    bool operator==(const ProbeOutcome&) const = default;
};

// Runs one probe scene under `state` and returns the observation. GPU-free and deterministic; the state need not be
// a valid API state (an invalid value is counted in ProbeOutcome::unmodelled and treated as its neutral value), so the
// probe is safe in a test, but a caller compares outcomes of valid states.
ProbeOutcome run_probe(const FixedFunctionState& state, ProbeScene scene);

// One row of the probe contract: which FixedFunctionState field group a probe scene can decide, and what it observes.
// `observable` is false only when no rendering result the API defines depends on the field - D3D12 defines sample
// quality 0 as "the default pattern" and leaves other values device-dependent, so no scene can pin it; only the
// container's layout could. The rows are in kFixedFunctionFieldNames order (probe_contract[field].field == field).
struct ProbeContract {
    int field = 0;          // index into kFixedFunctionFieldNames
    ProbeScene scene = ProbeScene::kWindingPair;
    bool observable = false;
    const char* distinguishes = "";  // what changes in ProbeOutcome when the field's value changes
};

inline constexpr ProbeContract kProbeContract[8] = {
    {0, ProbeScene::kWindingPair, true,
     "the covered half: cull mode and front-face flag each select a half; the depth-clip scene adds whether an "
     "out-of-range layer is drawn (fill mode, bias, multisample and conservative rasterization are not modelled)"},
    {1, ProbeScene::kOverlap, true,
     "the resolved colour: blend enable, factors, operation and write mask each change it (one target modelled)"},
    {2, ProbeScene::kTwoDepthQuads, true,
     "the surviving colour: depth enable, depth func and depth write mask each change it (stencil not modelled)"},
    {3, ProbeScene::kTopologyList, true,
     "the assembled count: a topology type outside the scene's list class assembles nothing"},
    {4, ProbeScene::kStripCut, true,
     "the fault count: a cut value equal to the strip's sentinel restarts it, any other value reads it as a vertex index"},
    {5, ProbeScene::kSampleGrid, true,
     "the partial pixel's covered-sample count and resolved colour: 0 samples at count 1, 1 sample at count 4"},
    {6, ProbeScene::kSampleGrid, false,
     "nothing: D3D12 defines quality 0 as the default pattern and leaves other values device-dependent"},
    {7, ProbeScene::kSampleGrid, true,
     "the covered-sample count and resolved colour: a mask that drops samples changes both"},
};

// One input-layout format word this tree has measured, with the DXGI format it means and its bytes per vertex.
struct InputFormat {
    std::uint32_t console_word;  // the element's attribute word 1, as stored in the input-layout blob
    DXGI_FORMAT dxgi;
    UINT bytes;  // bytes per vertex the DXGI format occupies; the aligned-offset check advances by this
};

// The input-layout format words the measured boot shapes carry, tested against the documented DXGI enum
// (docs/input_layout.md): 2 = DXGI_FORMAT_R32G32B32A32_FLOAT (16 B), 6 = DXGI_FORMAT_R32G32B32_FLOAT (12 B),
// 16 = DXGI_FORMAT_R32G32_FLOAT (8 B), 28 = DXGI_FORMAT_R8G8B8A8_UNORM (4 B), 30 = DXGI_FORMAT_R8G8B8A8_UINT (4 B).
// 6, 16 and 28 have their byte sizes independently confirmed by the measured packed offsets; 2 and 30 are only seen as
// the last element of their layout, so their size is the DXGI enum reading and the packed-offset check is the
// falsifier. A word outside this table is refused by build() with the value in the field name, never mapped by analogy.
inline constexpr InputFormat kMeasuredInputFormats[] = {
    {2, DXGI_FORMAT_R32G32B32A32_FLOAT, 16},
    {6, DXGI_FORMAT_R32G32B32_FLOAT, 12},
    {16, DXGI_FORMAT_R32G32_FLOAT, 8},
    {28, DXGI_FORMAT_R8G8B8A8_UNORM, 4},
    {30, DXGI_FORMAT_R8G8B8A8_UINT, 4},
};

// The measured format for one stored format word, or null when this tree has not measured that word.
const InputFormat* measured_input_format(std::uint32_t console_word);

// One stage's bytecode, indexed by record slot (rt::graphics_record::kStageSlotPs, kStageSlotVs, kStageSlotMs): the
// bytes create_graphics will classify and resolve. For a slot-57 record this is the stage's native program as passed
// (record.stages[i].program), or an FSPX payload carrying its kind-0 state when the caller has one. A slot the record
// uses needs bytes here; a slot it does not use must have none.
struct StageBytecode {
    const void* data;
    SIZE_T size;
};

// What the caller knows and the record does not carry. Every member is required; build() refuses a missing one by
// name rather than inventing a value.
struct GraphicsPsoInputs {
    // The PC root signature for the record's +0x00 root signature (record.root_signature). The decoder copies only the
    // console pointer, so the caller validates/maps it and passes the PC object; null is refused as pRootSignature.
    ID3D12RootSignature* root_signature;
    // The render targets the PSO is created against, as the caller knows them from the bound views. Slots at and above
    // render_target_count must be DXGI_FORMAT_UNKNOWN, as D3D12 requires, and a used slot needs a format.
    DXGI_FORMAT rtv_formats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    UINT render_target_count;
    DXGI_FORMAT dsv_format;  // DXGI_FORMAT_UNKNOWN: no depth-stencil attachment
    // The state the record does not yield. build() writes exactly these values into the description.
    FixedFunctionState defaults;
    // The resolved native program bytes per record slot (see StageBytecode). The caller resolves them before calling
    // (by SHA-256, which is what resolve_native_stage does); build() cannot look a program up and does not pretend to.
    StageBytecode stages[rt::graphics_record::kStageSlotCount];
};

// The PC graphics-pipeline description for a decoded record, or false with `why` naming the field that stops it.
// `out` is untouched on false. On success every D3D12_GRAPHICS_PIPELINE_STATE_DESC field is filled: the stages from
// `inputs.stages`, the input layout as the measured element table proves it (an empty blob gives NumElements 0; a
// blob with elements gives one D3D12_INPUT_ELEMENT_DESC per element, each refused by field when it is not the measured
// shape, docs/input_layout.md), the fixed-function state from `inputs.defaults`, the attachments from the caller, and
// NodeMask 0, flags NONE, no cached PSO and no stream output (the console record carries none of those). A mesh stage
// is refused: a D3D12_GRAPHICS_PIPELINE_STATE_DESC has no mesh-shader field, so a record with slot 2 needs the
// pipeline-state-stream path instead (docs/graphics_pso.md).
//
// The input-element descriptions a built description points at (`InputLayout.pInputElementDescs`) live in storage
// owned by this backend, not by the caller: the description carries a pointer, not the array. That storage is valid
// until the next build() on the same thread, and the slot-57 wiring uses the description with
// CreateGraphicsPipelineState immediately after building it (port/shims/d3d12_xs_device.cpp), which is the
// contract. Each SemanticName points at the record's own blob bytes (they are NUL-terminated and validated), so it
// has the blob's lifetime.
bool build(const rt::graphics_record::Record& record, const GraphicsPsoInputs& inputs,
           D3D12_GRAPHICS_PIPELINE_STATE_DESC* out, std::string* why);

}  // namespace rt::graphics_pso
