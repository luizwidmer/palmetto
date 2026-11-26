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
#include <string_view>
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
//
// The one exception is the measured cull axis below: with PALMETTO_RASTER_CULL_MEASURED=1 this function returns the
// same set with the rasterizer cull pair replaced, because a measurement of the title's own draw decides that field
// (docs/raster_state.md). Off by default, the returned set is the documented one above, field for field.
FixedFunctionState documented_default_state();

// ---- The measured cull axis of the undecoded rasterizer state ---------------------------------------------------
//
// The 380..520-byte state record's rasterizer state is not decodable: no word of it is pinned to a D3D12 field
// (docs/state_blob.md), and docs/raster_state.md re-derives the cull census over the library's 5,680 distinct blobs -
// the only aligned word whose value stays inside the cull enum on every blob is the record's stage-set word at +0x2C,
// not a per-pipeline rasterizer value. The cull axis alone is decidable by observation of the title's own draw: with
// the console driver-block feed in place (docs/vertex_constant_feed.md), the constant pass's full-screen triangle,
// fetched from the title's own vertex stream, is drawn when the pipeline's rasterizer cull mode is NONE and is
// discarded under the documented stand-in's CULL_BACK / clockwise-front, because the pass's recorded triangle is
// counter-clockwise in D3D screen space (docs/raster_state.md sections 2-3).
//
// That observation is a measurement of the pass, not of the record's words, so it is applied as an explicit, logged,
// opt-in substitution; the record's eight fields stay refused and every other stand-in field stays the documented one:
//
//   PALMETTO_RASTER_CULL_MEASURED=1   build with CullMode NONE and FrontCounterClockwise FALSE
//
// The observation cannot tell NONE from (CULL_BACK, counter-clockwise front) or (CULL_FRONT, clockwise front) - the
// same triangle survives all three - so the value applied is NONE, the only one of the three that adds no winding rule
// the record has not yielded. What would pin a different value is the serialized driver layout (docs/raster_state.md
// section 6); until then the switch stays off by default and the documented stand-in is what every pipeline carries.
bool measured_cull_axis_enabled();

// Applies the measured cull axis to `state` (CullMode NONE, FrontCounterClockwise FALSE) and returns whether it
// changed it. The second overload takes the decision explicitly so a caller or a test can drive both sides without an
// environment variable; the first reads PALMETTO_RASTER_CULL_MEASURED once.
bool apply_measured_cull_axis(FixedFunctionState* state, bool enabled);
inline bool apply_measured_cull_axis(FixedFunctionState* state) {
    return apply_measured_cull_axis(state, measured_cull_axis_enabled());
}

// ---- The per-record measured rasterizer state: a measured record selection --------------------------------------
//
struct FixedFunctionFieldStatus;  // defined below with the fixed-function decode (this section only points at it)

// The cull axis above is a *pass-level* observation: it was decided on draws of the title's own constant and compose
// passes, and the picture slice measured that applying it to every pipeline changes measured title behaviour before
// stage 3 - the global arm's boots stay in stage 2 where the default path reaches stage 3 (docs/vertex_buffer_va.md
// section 21, docs/raster_state.md section 5). The per-record decode is measured here, and its result is a negative
// on the record side plus a measured rule on the observation side:
//
//   * The state record's rasterizer field is still not readable. Over the library's 5,680 distinct state records the
//     only aligned word whose value stays inside the D3D12 cull enum is the record's stage-set word at +0x2C, and
//     that word is 2 on every PS+VS record - all three content draws included - so it names the stage family, not a
//     per-pipeline cull. A word/byte/nibble extraction over the smallest container yields no second place: the record
//     stores the console driver's state, not the D3D12 enum (docs/state_blob.md section 9, docs/raster_state.md
//     section 3).
//   * The cull *value* is pinned per measured record by the behavioural observation the probe contract requires
//     (docs/state_blob.md section 9): under the documented stand-in's back-face cull the constant pass and the compose
//     pass cover 0 of 8,294,400 pixels, and with the cull pair replaced they cover 4,147,200 - the winding-pair probe
//     tells the two states apart (`run_probe`, ProbeScene::kWindingPair). The observation cannot tell cull NONE from
//     (BACK, counter-clockwise front) or (FRONT, clockwise front), so NONE stays the minimal value (docs/
//     raster_state.md section 4).
//
// The rule this enables is a **measured record selection**, explicit and off by default: the caller names the records
// the measurement covered by their measured *shape* - the state bytes, the input-layout bytes and the program bytes of
// each stage, i.e. the interface sizes a slot-57 build carries (the boot log prints them as the record's `pairs`) -
// and only a record of one of those shapes gets the measured cull pair. Every other record keeps the caller's state
// (the documented stand-in is the fallback) byte for byte, and an unparsable selector, an unmeasured shape, a mesh
// pipeline, a record whose state container did not decode and a refused stage placement are refusals by name. The
// *place* of the field stays refused: the selection says which measured records the observed value belongs to, not
// which word of the record encoded it.
//
//   PALMETTO_RASTER_RECORD_MEASURED=state:388,layout:188,ps:384,vs:320,ms:0;state:396,layout:188,ps:384,vs:512,ms:0
//
// A shape is five sizes: the state blob's bytes, the input layout's bytes and the PS/VS/MS program bytes (0 for a
// stage the record does not use). The measured shapes are the content window's three draws (docs/vertex_buffer_va.md
// section 21); nothing here is game data - a shape is a set of sizes and the selector is a run-time measurement input.
struct MeasuredRecordShape {
    std::uint32_t state_bytes = 0;   // the decoded state container's size (380..520)
    std::uint32_t layout_bytes = 0;  // the input-layout blob's size (80..340)
    std::uint32_t stage_bytes[3] = {};  // by record slot: PS(0), VS(1), MS(2); 0 for a slot the record does not use
};

// Parses the selector text (the environment's value). An empty text is the switch off (`enabled` false, `ok` true).
// A non-empty text must name one to eight shapes, each exactly `state:<n>,layout:<n>,ps:<n>,vs:<n>,ms:<n>` with `;`
// between shapes; a missing/unknown/duplicate key, a non-decimal value, a size outside the measured ranges, a shape
// with no programmable stage or with both VS and MS, and a duplicate shape are refused by name. `out` is untouched on
// a refusal and `why` names the refused field with the offending text in it.
struct MeasuredRecordSelection {
    bool enabled = false;  // the switch is set (text non-empty)
    bool ok = true;        // the text parsed; false names the refusal in `error`
    std::string error;
    std::vector<MeasuredRecordShape> shapes;
};
bool parse_measured_record_selection(std::string_view text, MeasuredRecordSelection* out, std::string* why);

// The environment decision, read once from PALMETTO_RASTER_RECORD_MEASURED (the explicit overload of the parse above
// is the testable one). Off by default: every record keeps the documented stand-in and nothing is logged.
const MeasuredRecordSelection& measured_record_selection();

// The measured shape of one record: the decoded state container's size, the input layout's size and each stage's
// program bytes by record slot. The record must have decoded; a slot the record does not use is 0.
MeasuredRecordShape measured_record_shape(const rt::graphics_record::Record& record);

// The per-record decode of the rasterizer state. Returns true only when the record's measured shape is one the
// selection names: `cull_mode`/`front_counter_clockwise` are then the measured pair (NONE, clockwise front), and
// `fields` (when given) reports only `rasterizer` true - the other seven fixed-function fields stay refused. Returns
// false with a named refusal in `why` for: the switch off (the documented stand-in stays), a malformed selector, an
// unmeasured record shape, a record whose state container did not decode, a mesh pipeline (the measurement covered no
// mesh draw), and a record whose stage program sizes disagree with the state family. The refusal prefix is
// `state.record_rasterizer`; the place of the cull field is refused by name (`rasterizer.cull_mode.place`).
struct RecordRasterState {
    D3D12_CULL_MODE cull_mode = D3D12_CULL_MODE_NONE;
    BOOL front_counter_clockwise = FALSE;
};
bool decode_record_rasterizer_state(const rt::graphics_record::State& state,
                                    const rt::graphics_record::Record& record,
                                    const MeasuredRecordSelection& selection, RecordRasterState* out,
                                    FixedFunctionFieldStatus* fields, std::string* why);
inline bool decode_record_rasterizer_state(const rt::graphics_record::State& state,
                                           const rt::graphics_record::Record& record, RecordRasterState* out,
                                           FixedFunctionFieldStatus* fields, std::string* why) {
    return decode_record_rasterizer_state(state, record, measured_record_selection(), out, fields, why);
}

// Applies the per-record decode to `state`: on a matched record exactly the caller's rasterizer cull pair is replaced
// with the measured one and every other field is left byte-identical; on a refusal `state` is untouched (the documented
// stand-in stays) and `why` names the refusal. The environment overload is what build() calls; the explicit overload is
// the testable decision.
bool apply_measured_record_rasterizer_state(const rt::graphics_record::State& state,
                                            const rt::graphics_record::Record& record,
                                            const MeasuredRecordSelection& selection, FixedFunctionState* state_inout,
                                            FixedFunctionFieldStatus* fields, std::string* why);
inline bool apply_measured_record_rasterizer_state(const rt::graphics_record::State& state,
                                                   const rt::graphics_record::Record& record,
                                                   FixedFunctionState* state_inout,
                                                   FixedFunctionFieldStatus* fields, std::string* why) {
    return apply_measured_record_rasterizer_state(state, record, measured_record_selection(), state_inout, fields, why);
}

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

// The value a console-layout descriptor-table slot carries (docs/root_arguments.md, pinned from the translated
// module): ONE dword holding the arena-relative VA of the table's first descriptor. The shader forms
// `heap_base + (dword & ~3)`, so a value that does not fit 32 bits cannot be expressed and must be refused rather than
// truncated - `fits` is false then and `dword` 0.
struct ConsoleTableValue {
    bool fits = false;
    std::uint32_t dword = 0;
};
ConsoleTableValue console_table_value(std::uint64_t arena_relative_va);

// ---- Classifying a pipeline's root-signature parameters in the console layout ---------------------------------
//
// The sibling pass's black was traced to its pixel stage's user data, and the published program metadata narrows what
// that stage can read: `fspx/lib_ps_00820` reads exactly `s8` (a 32-bit pointer) and `s9` (a 32-bit constant), and the
// compose `fspx/lib_ps_00822` reads the same pair (docs/root_arguments.md section 9). On the console pixel base `s8`, a
// descriptor table takes ONE dword, so the dword after a table parameter is `s9` - the constant only if the root
// signature declares a 32-bit constant parameter *after* the table. This classification answers that question from the
// signature alone, without a boot: it lists every parameter (type, console first SGPR, console dword width) and finds
// the first 32-bit-constant parameter that follows a descriptor table.
//
// The two overloads exist because the pipeline path carries different models: the pure signature model
// (`rt::rootsig::RootSignature`) is available when the signature is created and in the tests, while the pipeline
// objects carry the already-placed parameters (`RootArgumentLayout`); both reduce to `ConsoleParameter`.
struct ConsoleParameter {
    rt::rootsig::ParamType type = rt::rootsig::ParamType::Constants;
    std::uint32_t values = 0;   // Constants: number of 32-bit values; other types: unused (0)
    std::uint32_t first_sgpr = rt::rootsig::kNoSgpr;  // console user SGPR, kNoSgpr when the layout refused it
    std::uint32_t dwords = 0;   // console width: a constant one dword per value, a root view two, a table one
};

struct ConsoleSignatureClass {
    bool ok = false;
    std::string error;                     // set when the console layout refuses the signature (spill)
    std::vector<ConsoleParameter> parameters;
    int table_index = -1;                  // index of the first descriptor-table parameter, -1 when none
    int constant_after_table = -1;         // first 32-bit-constant parameter after the first table, -1 when none
    std::uint32_t constant_after_table_slot = rt::rootsig::kNoSgpr;     // its console user SGPR (the sibling's s9)
    std::uint32_t constant_after_table_dwords = 0;
};

// Classifies already-placed parameters. `parameters` are in root-signature order.
void classify_console_parameters(std::span<const ConsoleParameter> parameters, ConsoleSignatureClass* out);

// Places `signature` in the console graphics layout (mesh base s16 when `mesh_pipeline`, else the vertex/pixel base
// s8) and classifies the result. False, with `error` set, when the console layout refuses the signature.
bool classify_console_signature(const rt::rootsig::RootSignature& signature, bool mesh_pipeline,
                                ConsoleSignatureClass* out);

// ---- Whether the console layout can be fed beside the PC packing --------------------------------------------------
//
// The port feeds a graphics draw's 32 user-data dwords with the PC packing (rt::rootsig::pack from s0, two dwords per
// table) and, on the measured graphics path, also with the console layout (rt::rootsig::pack_stage from the stage base
// s8, one dword per table) that a translated vertex/pixel shader reads (docs/root_arguments.md,
// docs/vertex_constant_feed.md). Both layouts share one 32-dword block, so on a signature whose PC packing reaches the
// console base the console feed would overwrite a dword a shader reads at its PC slot. This plan answers that from the
// two placements alone: every console parameter placeable inside the 32 dwords, and no console dword inside a
// PC-packed parameter's dword range. A refused plan is logged with `why` and leaves every dword at the previous
// answer - it is never a silent partial feed. Pure.
struct ConsoleFeedParameter {
    std::uint32_t pc_first = 0;                           // first dword of the parameter's PC packing (pack, from s0)
    std::uint32_t pc_dwords = 0;                          // dwords that packing gives it (2 table/root view, NumValues)
    std::uint32_t console_first = rt::rootsig::kNoSgpr;   // first user SGPR in the console layout, kNoSgpr when unplaced
    std::uint32_t console_dwords = 0;
};

struct ConsoleFeedPlan {
    bool feed = false;
    std::string why;  // set when !feed: the parameter whose placement makes the dual feed unsafe (bounded text)
};

// Fills `out` with whether the console layout can be fed beside the PC packing for these parameters. Returns
// `out->feed`; false with `out->why` naming the first parameter that spills past the 32 dwords or whose console dwords
// overlap a PC parameter's dwords.
bool console_feed_plan(std::span<const ConsoleFeedParameter> parameters, ConsoleFeedPlan* out);

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
// pipeline-state-stream path instead (build_mesh_stream() below; docs/mesh_stage.md).
//
// The input-element descriptions a built description points at (`InputLayout.pInputElementDescs`) live in storage
// owned by this backend, not by the caller: the description carries a pointer, not the array. That storage is valid
// until the next build() on the same thread, and the slot-57 wiring uses the description with
// CreateGraphicsPipelineState immediately after building it (port/shims/d3d12_xs_device.cpp), which is the
// contract. Each SemanticName points at the record's own blob bytes (they are NUL-terminated and validated), so it
// has the blob's lifetime.
bool build(const rt::graphics_record::Record& record, const GraphicsPsoInputs& inputs,
           D3D12_GRAPHICS_PIPELINE_STATE_DESC* out, std::string* why);

// ---- The mesh pipeline's description: the pipeline-state-stream form (docs/mesh_stage.md) -------------------------
//
// A D3D12_GRAPHICS_PIPELINE_STATE_DESC has no mesh-shader field, so a record whose slot 2 (MS) is used is built through
// a D3D12_PIPELINE_STATE_STREAM_DESC instead. build_mesh_stream() is the GPU-free creation side of that: it lays out
// the subobject bytes a PC device's CreatePipelineState parses (port/shims/d3d12_vulkan_device.cpp's
// parse_pipeline_stream -> PipelineState::create_graphics(desc, &ms)), and calls no device. The stream carries
//
//   ROOT_SIGNATURE, MS, PS (when the record has one), RENDER_TARGET_FORMATS, DEPTH_STENCIL_FORMAT, RASTERIZER, BLEND,
//   DEPTH_STENCIL, SAMPLE_MASK, SAMPLE_DESC, NODE_MASK, FLAGS
//
// in the d3dx12.h layout (a type word, the payload at its alignment, each subobject pointer-aligned). There is no VS,
// GS, HS or DS subobject: D3D12 forbids them beside MS, and the record's slot set has no such slot. There is no
// INPUT_LAYOUT, PRIMITIVE_TOPOLOGY or IB_STRIP_CUT: D3D12 ignores a mesh pipeline's input-assembly state, and the
// measured mesh records carry the 80-byte empty input layout (docs/shader_library.md section 2), so a mesh record with
// elements is refused as an unmeasured shape instead of having its layout silently dropped. The fixed-function state
// is `inputs.defaults`, exactly as build() writes it.
//
// Where a host has no mesh stage (a device without VK_EXT_mesh_shader), the PC device refuses the stream with E_NOTIMPL
// and its own named log; there is deliberately no VS/PS stand-in for mesh work - a stand-in would silently drop the
// geometry the record asked for. The named refusals below are for the conversion, not the device.
struct MeshPipelineStream {
    // The subobject bytes. desc() points into this buffer, so the stream must outlive the CreatePipelineState call.
    std::vector<std::uint8_t> bytes;
    std::size_t subobject_count = 0;
    std::uint32_t ms_bytes = 0;  // the MS program's bytecode length
    std::uint32_t ps_bytes = 0;  // the PS program's, or 0 when the record is MS only
    // The stream description for the device. The buffer owns it; the const_cast only meets the member's void* type.
    D3D12_PIPELINE_STATE_STREAM_DESC desc() const {
        D3D12_PIPELINE_STATE_STREAM_DESC stream{};
        stream.SizeInBytes = bytes.size();
        stream.pPipelineStateSubobjectStream = const_cast<std::uint8_t*>(bytes.data());
        return stream;
    }
    // A bounded one-line description for the caller's once-per-shape log (at most 160 characters).
    std::string describe() const;
};

// A record whose slot 2 (MS) is used -> the pipeline-state-stream description, or false with `why` naming the field
// that stops it; `out` is untouched on false. The measured shapes are PS+MS and MS only (10,726 + 1,336 of the
// library's 25,471 graphics records, docs/mesh_stage.md). Refused by name: a record with no mesh stage (the
// D3D12_GRAPHICS_PIPELINE_STATE_DESC path is build()'s), a vertex and a mesh stage together (D3D12 forbids the pair),
// the unidentified slot-6 program (no measured stage), a mesh record whose input layout has elements (unmeasured), a
// missing root signature/PC root signature/attachment list/bad fixed-function state (the same checks as build()), a
// mesh stage without resolved program bytes, vertex bytes supplied for a mesh pipeline, or PS bytes for a record that
// has no PS slot.
bool build_mesh_stream(const rt::graphics_record::Record& record, const GraphicsPsoInputs& inputs,
                       MeshPipelineStream* out, std::string* why);

// The counters a slot-57 wiring keeps for the mesh/merged-geometry path (docs/mesh_stage.md): records that named a
// mesh stage, streams built, and conversions refused. Pure data so the tally is testable without a device; the wiring
// owns one instance and prints summary() once per histogram checkpoint. The merged-geometry half has no runtime path
// yet (the 195 GS modules are translator-keyed only and no record names a geometry slot), so a program resolved only
// under stage "gs" is already named by resolve_native_stage's "stage mismatch" refusal; it is not counted here.
struct MeshStageCounters {
    std::uint64_t records_with_mesh_stage = 0;  // slot-57-shaped records that named slot 2
    std::uint64_t streams_built = 0;            // build_mesh_stream() succeeded
    std::uint64_t refusals = 0;                 // build_mesh_stream() refused (the reason is the caller's log)
    void note_record() { ++records_with_mesh_stage; }
    void note_built() { ++streams_built; }
    void note_refused() { ++refusals; }
    // A bounded one-line summary for the boot histogram ("mesh stages: records 0, streams 0, refusals 0"), at most
    // 120 characters.
    std::string summary() const;
};

}  // namespace rt::graphics_pso
