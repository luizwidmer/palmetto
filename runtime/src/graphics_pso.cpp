#include "graphics_pso.h"

#include <array>
#include <utility>

namespace rt::graphics_pso {
namespace {

// The shader index's stage names. A record names its stages by slot (0 PS, 1 VS, 2 MS) and nothing inside the stage
// blob does (docs/shader_library.md section 2). "gs" and "cs" are not record slots: they are scanned only when a
// program resolved under no graphics stage, so a slot/stage mismatch (a program published for another stage) is named
// instead of being reported as a missing translation.
constexpr const char* kStageNames[] = {"ps", "vs", "ms", "gs", "cs"};

// The D3D12_INPUT_ELEMENT_DESC array a built description points at. D3D12_GRAPHICS_PIPELINE_STATE_DESC carries the
// elements by pointer, not by value, so build() needs storage that outlives it: this per-thread buffer, valid until the
// next build() on the same thread (the lifetime contract documented in graphics_pso.h). Each SemanticName points into
// the record's own input-layout blob, which the caller keeps alive for the creation call.
thread_local std::array<D3D12_INPUT_ELEMENT_DESC, rt::graphics_record::kInputLayoutMaxElements> g_input_elements{};

bool fail(std::string* why, std::string message) {
    if (why) *why = std::move(message);
    return false;
}

// A record slot and its stage name: "1 (vs)".
std::string stage_slot(int slot) {
    return std::to_string(slot) + " (" + rt::graphics_record::stage_slot_name(slot) + ")";
}

// A stage's bytecode input name: "stages[1] (vs)".
std::string stage_input(int slot) {
    return "stages[" + std::to_string(slot) + "] (" + rt::graphics_record::stage_slot_name(slot) + ")";
}

// One why-message prefix for a record stage: "record.stages[1].slot: ...".
std::string record_stage(std::size_t index) {
    return "record.stages[" + std::to_string(index) + "].slot: ";
}

// The refusal field of one input-layout element: "input_layout.elements[2].format[7]". The slot-57 histogram keys a
// refusal by the text before its first ':', so an unmapped format word or a non-zero unproven word puts its numeric
// value in the field and the boot histogram then lists exactly which values remain unmapped.
std::string element_field(std::size_t index, const std::string& field) {
    return "input_layout.elements[" + std::to_string(index) + "]." + field;
}

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

}  // namespace

const InputFormat* measured_input_format(std::uint32_t console_word) {
    for (const InputFormat& format : kMeasuredInputFormats)
        if (format.console_word == console_word) return &format;
    return nullptr;
}

bool resolve_native_stage(std::span<const std::uint8_t> bytecode, const char* stage,
                          const palmetto::ShaderLibrary& library,
                          const std::function<bool(const palmetto::ShaderEntry&)>& fits, NativeStage* out,
                          std::string* why) {
    if (out) *out = NativeStage{};
    if (why) why->clear();
    if (!out || !stage || !*stage || bytecode.empty()) return false;

    const std::string stage_name(stage);
    palmetto::ComputeProgram program = palmetto::compute_program(bytecode);
    if (program.kind == palmetto::ComputeProgram::Kind::fssm) return false;  // the FSSM/DXBC path owns those bytes
    bool raw_key = false;
    if (program.kind == palmetto::ComputeProgram::Kind::none) {
        // Not a program compute_program() recognises. Its identity is still the SHA-256 of the bytes as passed - the
        // key the library's graphics programs were published under (docs/pipeline_program_source.md), which is the
        // same value whether or not the program carries the 36-byte FSPX prologue. Look that key up before calling
        // the bytes unreadable: a miss means this path has nothing to do with them.
        const std::string raw_hash = palmetto::sha256_hex(bytecode);
        if (library.fspx_candidates(stage_name, raw_hash).empty()) return false;
        program.kind = palmetto::ComputeProgram::Kind::fspx;
        program.bytes.assign(bytecode.begin(), bytecode.end());
        program.state.clear();
        raw_key = true;
    }

    bool ambiguous = false;
    const std::vector<const palmetto::ShaderEntry*> candidates =
        library.program_candidates(stage_name, program, &ambiguous);
    const palmetto::ShaderEntry* entry = pick_entry(candidates, fits);
    const std::string hash = palmetto::sha256_hex(program.bytes);
    out->program = std::move(program);
    out->entry = entry;
    out->raw_key = raw_key;
    if (entry || !why) return true;

    if (ambiguous) {
        *why = "ambiguous: " + std::string(raw_key ? "the native program" : "the FSPX kind-6 program") + " sha256 " +
               hash + " has translations that differ and came without its kind-0 state, so no single module can be "
                      "chosen (the record slot names the stage " +
               stage_name + ")";
        return true;
    }
    // A program published under another stage is a slot/stage mismatch, not a missing translation: say which stage,
    // because the caller's wiring would be the thing to fix.
    for (const char* other : kStageNames) {
        if (stage_name == other) continue;
        if (!library.program_candidates(other, out->program, nullptr).empty()) {
            *why = "stage mismatch: the program sha256 " + hash + " is published for stage " + other + ", not " +
                   stage_name + " (the record slot names the stage); no module was chosen";
            return true;
        }
    }
    *why = "no translation: no shader index entry for stage " + stage_name + " and " +
           (out->program.state.empty() ? std::string(raw_key ? "the SHA-256 of its program bytes"
                                                            : "the FSPX kind-6 program")
                                       : std::string("the FSPX kind-6 program looked up with its kind-0 state")) +
           " sha256 " + hash;
    return true;
}

FixedFunctionState documented_default_state() {
    FixedFunctionState state{};
    // Rasterizer: solid triangles, back faces culled, clockwise is front, depth clipping on, no depth bias.
    state.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    state.rasterizer.CullMode = D3D12_CULL_MODE_BACK;
    state.rasterizer.FrontCounterClockwise = FALSE;
    state.rasterizer.DepthBias = 0;
    state.rasterizer.DepthBiasClamp = 0.0f;
    state.rasterizer.SlopeScaledDepthBias = 0.0f;
    state.rasterizer.DepthClipEnable = TRUE;
    state.rasterizer.MultisampleEnable = FALSE;
    state.rasterizer.AntialiasedLineEnable = FALSE;
    state.rasterizer.ForcedSampleCount = 0;
    state.rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    // Blend: off, with the API's neutral factors and ops and every colour channel written on every target, so a later
    // blend enable changes only BlendEnable.
    state.blend.AlphaToCoverageEnable = FALSE;
    state.blend.IndependentBlendEnable = FALSE;
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : state.blend.RenderTarget) {
        target.BlendEnable = FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    // Depth-stencil: both off, every field still a valid API value (the neutral ops and comparison).
    state.depth_stencil.DepthEnable = FALSE;
    state.depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    state.depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    state.depth_stencil.StencilEnable = FALSE;
    state.depth_stencil.StencilReadMask = 0xff;
    state.depth_stencil.StencilWriteMask = 0xff;
    for (D3D12_DEPTH_STENCILOP_DESC* face : {&state.depth_stencil.FrontFace, &state.depth_stencil.BackFace}) {
        face->StencilFailOp = D3D12_STENCIL_OP_KEEP;
        face->StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        face->StencilPassOp = D3D12_STENCIL_OP_KEEP;
        face->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    state.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    state.sample_count = 1;
    state.sample_quality = 0;
    state.sample_mask = 0xffffffffu;
    return state;
}

bool decode_fixed_function_state(const rt::graphics_record::State& state, FixedFunctionState* out,
                                 FixedFunctionFieldStatus* fields, std::string* why) {
    if (fields) *fields = FixedFunctionFieldStatus{};
    (void)out;  // untouched on refusal, like build(): a guessed state is never written to the caller's struct
    if (state.size == 0) {
        if (why)
            *why = "state: the fixed-function state record was not decoded (its container was refused before this "
                   "call, docs/state_blob.md)";
        return false;
    }
    // The container is decoded; what it does not yield is any fixed-function field. Naming every field keeps the
    // slot-57 histogram honest: one row says the whole set refuses, and the eight names say exactly what a measurement
    // would have to pin before the stand-in can be replaced.
    if (why) {
        std::string refused;
        for (const char* name : kFixedFunctionFieldNames) {
            if (!refused.empty()) refused += ", ";
            refused += name;
        }
        *why = "state.fixed_function: no fixed-function field is pinned by a measurement of this title's data, so the "
               "documented stand-in stays; refused: " +
               refused + " (the state record is the console driver's serialized " +
               std::to_string(state.size) + "-byte pipeline state, docs/state_blob.md)";
    }
    return false;
}

bool build(const rt::graphics_record::Record& record, const GraphicsPsoInputs& inputs,
           D3D12_GRAPHICS_PIPELINE_STATE_DESC* out, std::string* why) {
    if (why) why->clear();
    if (!out) return fail(why, "build: the output description is null");

    // ---- The record's shape: which stages it names. decode() guarantees this, but build() is a public entry point and
    // a caller-built Record could be anything, so it is checked again instead of trusted.
    if (record.stage_count == 0 || record.stage_count > rt::graphics_record::kStageSlotCount)
        return fail(why, "record.stage_count: the record names " + std::to_string(record.stage_count) +
                             " stage(s); a graphics pipeline record has 1 to " +
                             std::to_string(rt::graphics_record::kStageSlotCount));
    bool used[rt::graphics_record::kStageSlotCount] = {};
    for (std::size_t i = 0; i < record.stage_count; ++i) {
        const int slot = record.stages[i].slot;
        if (slot < 0 || slot >= static_cast<int>(rt::graphics_record::kStageSlotCount))
            return fail(why, record_stage(i) + "slot " + std::to_string(slot) +
                                 " is not one of the record's stage slots");
        if (used[slot]) return fail(why, record_stage(i) + "stage slot " + stage_slot(slot) + " appears twice");
        used[slot] = true;
    }
    const bool vertex = used[rt::graphics_record::kStageSlotVs];
    const bool mesh = used[rt::graphics_record::kStageSlotMs];
    if (vertex && mesh)
        return fail(why, "record.stages: a vertex and a mesh stage together are not a stage combination D3D12 accepts");
    if (mesh)
        return fail(why, "record.stages[].slot: a mesh stage cannot be expressed in a "
                         "D3D12_GRAPHICS_PIPELINE_STATE_DESC (the PC description has no mesh-shader field); a record "
                         "whose slot 2 is used needs the D3D12_PIPELINE_STATE_STREAM_DESC path with its MS subobject");

    // ---- The two blobs the record always carries. The state is checked for its measured envelope and then not used:
    // no word of it is decoded (docs/graphics_record.md section 4), so the fixed-function state comes from
    // `inputs.defaults` instead. The input layout is decoded into its measured element table (docs/input_layout.md);
    // every element field is then proved against the measured values before a D3D12_INPUT_ELEMENT_DESC is built, and
    // anything not proved refuses by field.
    if (!record.root_signature)
        return fail(why, "record.root_signature: the record has no root signature at +0x00, so the pipeline's "
                         "bindings cannot be established");
    rt::graphics_record::State state;
    if (!rt::graphics_record::decode_state(record.state, &state))
        return fail(why, "record.state: the fixed-function state blob is missing or outside its measured envelope "
                         "(380..520 bytes, a whole number of words), so the record is not the shape this converter "
                         "knows");
    std::vector<rt::graphics_record::Element> elements;
    if (!rt::graphics_record::decode_input_layout(record.input_layout, &elements))
        return fail(why, "record.input_layout: the input-layout blob does not decode (its 0x50-byte header, its "
                         "length-prefixed printable names, its 24-byte attribute blocks and its zero padding do not "
                         "fill the measured 80..340 bytes; docs/input_layout.md)");
    if (!elements.empty() && !vertex)
        return fail(why, "record.stages: the input layout has " + std::to_string(elements.size()) +
                             " element(s) but the record names no vertex stage; D3D12 requires a vertex shader to "
                             "consume an input layout");
    // The decoded elements become D3D12_INPUT_ELEMENT_DESCs through the measured attribute field order
    // (docs/input_layout.md): the six words are the D3D12_INPUT_ELEMENT_DESC fields in their D3D12 order with the name
    // pointer replaced by the stored name, i.e. word 0 SemanticIndex, word 1 Format, word 2 InputSlot, word 3
    // AlignedByteOffset, word 4 InputSlotClass, word 5 InstanceDataStepRate. Word 1 and word 3 are pinned by the
    // measured values (the formats' sizes are exactly the byte offsets), and word 0 is pinned by the measured COLOR
    // element whose value is 2 - the COLOR2 semantic index. Every unmeasured value refuses by field, with the value in
    // the field name so the boot histogram lists it.
    std::uint32_t input_stride = 0;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const rt::graphics_record::Element& element = elements[index];
        const std::uint32_t semantic_index = element.attribute[0];
        const std::uint32_t format_word = element.attribute[1];
        const std::uint32_t input_slot = element.attribute[2];
        const std::uint32_t stored_offset = element.attribute[3];
        const std::uint32_t input_class = element.attribute[4];
        const std::uint32_t step_rate = element.attribute[5];
        const InputFormat* format = measured_input_format(format_word);
        if (!format)
            return fail(why, element_field(index, "format[" + std::to_string(format_word) + "]") + ": element " +
                                 std::to_string(index) + " (" + quoted(element.semantic) + ") stores format word " +
                                 std::to_string(format_word) +
                                 ", which is not one of the measured DXGI format values 2, 6, 16, 28 and 30 "
                                 "(docs/input_layout.md), so no D3D12_INPUT_ELEMENT_DESC can be built for it");
        if (input_slot != 0)
            return fail(why, element_field(index, "input_slot[" + std::to_string(input_slot) + "]") + ": element " +
                                 std::to_string(index) + " (" + quoted(element.semantic) + ") stores input slot " +
                                 std::to_string(input_slot) +
                                 "; every measured element is in slot 0, so the slot is not mapped");
        if (input_class != 0)
            return fail(why, element_field(index, "input_slot_class[" + std::to_string(input_class) + "]") +
                                 ": element " + std::to_string(index) + " (" + quoted(element.semantic) +
                                 ") stores input-slot class " + std::to_string(input_class) +
                                 "; every measured element is per-vertex data (class 0), so the class is not mapped");
        if (step_rate != 0)
            return fail(why, element_field(index, "instance_step_rate[" + std::to_string(step_rate) + "]") +
                                 ": element " + std::to_string(index) + " (" + quoted(element.semantic) +
                                 ") stores instance step rate " + std::to_string(step_rate) +
                                 "; every measured element has step rate 0, so it is not mapped");
        // D3D12 takes AlignedByteOffset verbatim and the description carries no stride, so a measured gap between two
        // elements is emitted as stored - the post-decode boot has an 188-byte shape with COLOR at offset 24 after a
        // 12-byte POSITION. Only an overlap with the preceding elements' measured sizes is refused.
        if (stored_offset == D3D12_APPEND_ALIGNED_ELEMENT)
            return fail(why, element_field(index, "aligned_byte_offset[" + std::to_string(stored_offset) + "]") +
                                 ": element " + std::to_string(index) + " (" + quoted(element.semantic) +
                                 ") stores the D3D12_APPEND_ALIGNED_ELEMENT sentinel as its offset; no measured "
                                 "element uses the sentinel, so the offset is not mapped");
        if (stored_offset < input_stride)
            return fail(why, element_field(index, "aligned_byte_offset[" + std::to_string(stored_offset) + "]") +
                                 ": element " + std::to_string(index) + " (" + quoted(element.semantic) +
                                 ") stores offset " + std::to_string(stored_offset) +
                                 ", which overlaps the elements before it: they occupy " +
                                 std::to_string(input_stride) +
                                 " bytes at the measured format sizes and every measured layout is non-overlapping, "
                                 "so this blob is not the measured shape and no D3D12_INPUT_ELEMENT_DESC is built "
                                 "from it");
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (elements[earlier].semantic != element.semantic || elements[earlier].attribute[0] != semantic_index)
                continue;
            return fail(why, element_field(index, "semantic_index[" + std::to_string(semantic_index) + "]") +
                                 ": element " + std::to_string(index) + " (" + quoted(element.semantic) +
                                 ") repeats the semantic index of element " + std::to_string(earlier) +
                                 "; D3D12 needs a unique semantic name and index per element, so the duplicate is "
                                 "refused rather than built");
        }
        D3D12_INPUT_ELEMENT_DESC& desc = g_input_elements[index];
        desc = {};
        desc.SemanticName = reinterpret_cast<const char*>(record.input_layout.data + element.semantic_offset);
        desc.SemanticIndex = semantic_index;
        desc.Format = format->dxgi;
        desc.InputSlot = input_slot;  // 0: the only measured slot
        desc.AlignedByteOffset = stored_offset;
        desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;  // measured class word 0
        desc.InstanceDataStepRate = 0;                                     // measured step-rate word 0
        input_stride += format->bytes;
    }
    const std::size_t input_element_count = elements.size();

    // ---- The caller's inputs. Each is checked by name; build() substitutes nothing.
    if (!inputs.root_signature)
        return fail(why, "pRootSignature: the caller supplied no PC root signature for the record's root signature");
    for (int slot = 0; slot < static_cast<int>(rt::graphics_record::kStageSlotCount); ++slot) {
        const StageBytecode& code = inputs.stages[slot];
        const bool present = code.data != nullptr && code.size != 0;
        const std::string name = stage_input(slot);
        if (used[slot] && !present)
            return fail(why, name + ": the record has a " + rt::graphics_record::stage_slot_name(slot) +
                                 " stage but the caller supplied no resolved program bytes for it");
        if (!used[slot] && (code.data || code.size))
            return fail(why, name + ": the record has no " + rt::graphics_record::stage_slot_name(slot) +
                                 " stage, so these program bytes have no stage to go in");
    }
    if (inputs.render_target_count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return fail(why, "render_target_count: " + std::to_string(inputs.render_target_count) + " is above D3D12's " +
                             std::to_string(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT));
    for (UINT target = 0; target < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++target) {
        const std::string name = "rtv_formats[" + std::to_string(target) + "]";
        if (target < inputs.render_target_count) {
            if (inputs.rtv_formats[target] == DXGI_FORMAT_UNKNOWN)
                return fail(why, name + ": a render target the PSO uses needs a format, not DXGI_FORMAT_UNKNOWN");
        } else if (inputs.rtv_formats[target] != DXGI_FORMAT_UNKNOWN) {
            return fail(why, name + ": D3D12 requires the slots at and above NumRenderTargets to be "
                                    "DXGI_FORMAT_UNKNOWN");
        }
    }
    const FixedFunctionState& ff = inputs.defaults;
    switch (ff.topology_type) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE: break;
        default:
            return fail(why, "defaults.topology_type: " + std::to_string(static_cast<int>(ff.topology_type)) +
                                 " is not a topology type this converter can build (point, line or triangle)");
    }
    if (ff.sample_count == 0 || ff.sample_count > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT ||
        (ff.sample_count & (ff.sample_count - 1u)) != 0)
        return fail(why, "defaults.sample_count: " + std::to_string(ff.sample_count) +
                             " is not a power of two from 1 to " +
                             std::to_string(D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT));
    if (ff.sample_quality != 0 && ff.sample_quality != DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN &&
        ff.sample_quality != DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN)
        return fail(why, "defaults.sample_quality: " + std::to_string(ff.sample_quality) +
                             " is not 0, the standard or the centre multisample pattern");
    switch (ff.strip_cut) {
        case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED:
        case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF:
        case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF: break;
        default:
            return fail(why, "defaults.strip_cut: " + std::to_string(static_cast<int>(ff.strip_cut)) +
                                 " is not a strip-cut value this converter can build");
    }
    switch (ff.rasterizer.FillMode) {
        case D3D12_FILL_MODE_SOLID:
        case D3D12_FILL_MODE_WIREFRAME: break;
        default:
            return fail(why, "defaults.rasterizer.FillMode: " +
                                 std::to_string(static_cast<int>(ff.rasterizer.FillMode)) + " is not a fill mode");
    }
    switch (ff.rasterizer.CullMode) {
        case D3D12_CULL_MODE_NONE:
        case D3D12_CULL_MODE_FRONT:
        case D3D12_CULL_MODE_BACK: break;
        default:
            return fail(why, "defaults.rasterizer.CullMode: " +
                                 std::to_string(static_cast<int>(ff.rasterizer.CullMode)) + " is not a cull mode");
    }
    switch (ff.rasterizer.ConservativeRaster) {
        case D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF:
        case D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON: break;
        default:
            return fail(why, "defaults.rasterizer.ConservativeRaster: " +
                                 std::to_string(static_cast<int>(ff.rasterizer.ConservativeRaster)) +
                                 " is not a conservative-rasterization mode");
    }

    // ---- The description. Every field is set here: nothing is left to the structure's zero value by accident.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = inputs.root_signature;
    desc.VS = {inputs.stages[rt::graphics_record::kStageSlotVs].data,
               inputs.stages[rt::graphics_record::kStageSlotVs].size};
    desc.PS = {inputs.stages[rt::graphics_record::kStageSlotPs].data,
               inputs.stages[rt::graphics_record::kStageSlotPs].size};
    desc.DS = {};
    desc.HS = {};
    desc.GS = {};
    // No stream output: the record has none, and D3D12's stream-output state has no console source here.
    desc.StreamOutput = {};
    desc.BlendState = ff.blend;
    desc.SampleMask = ff.sample_mask;
    desc.RasterizerState = ff.rasterizer;
    desc.DepthStencilState = ff.depth_stencil;
    // An empty input layout stays a null array with NumElements 0 (the 1,074 fullscreen-style records the
    // measurement built). A layout with elements points at the per-thread descriptions build() just filled; see the
    // lifetime note in graphics_pso.h.
    if (input_element_count == 0) {
        desc.InputLayout = {nullptr, 0};
    } else {
        desc.InputLayout = {g_input_elements.data(), static_cast<UINT>(input_element_count)};
    }
    desc.IBStripCutValue = ff.strip_cut;
    desc.PrimitiveTopologyType = ff.topology_type;
    desc.NumRenderTargets = inputs.render_target_count;
    for (UINT target = 0; target < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++target)
        desc.RTVFormats[target] = inputs.rtv_formats[target];
    desc.DSVFormat = inputs.dsv_format;
    desc.SampleDesc.Count = ff.sample_count;
    desc.SampleDesc.Quality = ff.sample_quality;
    // Node 0, no flags and no cached PSO: the console record carries none of them, and each is the neutral value the
    // PC runtime takes rather than a console value guessed from the blob.
    desc.NodeMask = 0;
    desc.CachedPSO = {};
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    *out = desc;
    return true;
}

}  // namespace rt::graphics_pso
