#include "graphics_pso.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
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
    // The measured cull axis (docs/raster_state.md): the one fixed-function field a measurement of the title's own draw
    // decides while the state record's words stay refused. Off by default; the substitution is logged once.
    if (apply_measured_cull_axis(&state)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] graphics_pso: measured cull axis: the state record's rasterizer state is not decoded "
                         "(docs/raster_state.md); building with cull NONE and clockwise front, the one value under "
                         "which the title's own full-screen triangle is drawn beside the stand-in's back-face cull. "
                         "Every other stand-in field is unchanged.\n");
    }
    return state;
}

bool measured_cull_axis_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_RASTER_CULL_MEASURED");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

bool apply_measured_cull_axis(FixedFunctionState* state, bool enabled) {
    if (!state || !enabled) return false;
    state->rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    state->rasterizer.FrontCounterClockwise = FALSE;
    return true;
}

// ---- The per-record measured rasterizer state (docs/vertex_buffer_va.md section 21) -------------------------------

namespace {

// One parsed size field of a selector shape. The measured ranges are the record's own interface ranges (the state
// container and the input layout) and the program byte range; a value outside them is a refusal, not a clamp.
bool parse_shape_size(std::string_view text, std::uint32_t minimum, std::uint32_t maximum, std::uint32_t* out) {
    if (!out || text.empty()) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
        if (value > 0xFFFFFFFFull) return false;
    }
    if (value < minimum || value > maximum) return false;
    *out = static_cast<std::uint32_t>(value);
    return true;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool same_shape(const MeasuredRecordShape& a, const MeasuredRecordShape& b) {
    if (a.state_bytes != b.state_bytes || a.layout_bytes != b.layout_bytes) return false;
    for (int slot = 0; slot < 3; ++slot)
        if (a.stage_bytes[slot] != b.stage_bytes[slot]) return false;
    return true;
}

// The one refusal text of a malformed selector: the offending shape, the refused key and why.
bool shape_refusal(std::string* why, std::size_t index, std::string_view what) {
    if (why)
        *why = "state.record_rasterizer.selector[" + std::to_string(index) + "]: " + std::string(what) +
               "; the per-record measured cull rule stays off and every record keeps the documented stand-in";
    return false;
}

// Logs the measured rule's application/fallback once per distinct event. build() runs on many threads, so the
// bookkeeping is under a mutex; the log lines are the boot's measurement of what the rule reached.
void log_measured_record_event(const MeasuredRecordShape* shape, bool matched, const std::string& why) {
    static std::mutex mutex;
    static std::set<std::uint64_t> logged_shapes;
    static bool logged_fallback = false;
    static bool logged_off = false;
    const std::lock_guard lock(mutex);
    if (!matched) {
        if (why.rfind("state.record_rasterizer.off", 0) == 0) {
            if (!logged_off) {
                logged_off = true;
                std::fprintf(stderr, "[rt] graphics_pso: per-record rasterizer state: %s\n", why.c_str());
            }
            return;
        }
        if (!logged_fallback) {
            logged_fallback = true;
            std::fprintf(stderr, "[rt] graphics_pso: per-record rasterizer state: fallback to the documented stand-in: %s\n",
                         why.c_str());
        }
        return;
    }
    if (!shape) return;
    std::uint64_t key = static_cast<std::uint64_t>(shape->state_bytes) << 32 | shape->layout_bytes;
    key = key * 0x100000001b3ull ^ shape->stage_bytes[0];
    key = key * 0x100000001b3ull ^ shape->stage_bytes[1];
    key = key * 0x100000001b3ull ^ shape->stage_bytes[2];
    if (!logged_shapes.insert(key).second) return;
    std::fprintf(stderr,
                 "[rt] graphics_pso: per-record rasterizer state: measured record shape state %u, layout %u, "
                 "ps/vs/ms %u/%u/%u: the measured cull pair (NONE, clockwise front) replaces only the caller's cull "
                 "fields; every other fixed-function field and every other record keeps the documented stand-in "
                 "(docs/vertex_buffer_va.md section 21)\n",
                 shape->state_bytes, shape->layout_bytes, shape->stage_bytes[0], shape->stage_bytes[1],
                 shape->stage_bytes[2]);
}

}  // namespace

bool parse_measured_record_selection(std::string_view text, MeasuredRecordSelection* out, std::string* why) {
    if (why) why->clear();
    if (!out) return false;
    MeasuredRecordSelection selection;
    const bool was_empty = trimmed(text).empty();
    if (was_empty) {
        selection.enabled = false;
        selection.ok = true;
        *out = std::move(selection);
        return true;
    }
    selection.enabled = true;
    // The selector is a ';' list of five-key shapes. Exactly the five measured keys, once each; at most eight records
    // (a measurement covers a handful of draws, not a corpus).
    std::size_t index = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find(';', at);
        const std::string_view shape_text = trimmed(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (shape_text.empty()) {
            selection.ok = false;
            if (why)
                *why = "state.record_rasterizer.selector[" + std::to_string(index) +
                       "]: an empty record shape; the per-record measured cull rule stays off and every record keeps "
                       "the documented stand-in";
            *out = std::move(selection);
            return false;
        }
        if (index >= 8) {
            selection.ok = false;
            if (why)
                *why = "state.record_rasterizer.selector: more than eight measured record shapes; the per-record "
                       "measured cull rule stays off and every record keeps the documented stand-in";
            *out = std::move(selection);
            return false;
        }
        MeasuredRecordShape shape;
        bool seen[5] = {};  // state, layout, ps, vs, ms
        std::size_t key_at = 0;
        while (key_at <= shape_text.size()) {
            const std::size_t key_end = shape_text.find(',', key_at);
            const std::string_view pair =
                trimmed(shape_text.substr(key_at, key_end == std::string_view::npos ? std::string_view::npos
                                                                                    : key_end - key_at));
            const std::size_t colon = pair.find(':');
            if (colon == std::string_view::npos) {
                selection.ok = false;
                *out = std::move(selection);
                return shape_refusal(why, index, std::string("expected <key>:<size>, read \"") + std::string(pair) + "\"");
            }
            const std::string_view key = trimmed(pair.substr(0, colon));
            const std::string_view value = trimmed(pair.substr(colon + 1));
            int slot = -1;
            if (key == "state") slot = 0;
            else if (key == "layout") slot = 1;
            else if (key == "ps") slot = 2;
            else if (key == "vs") slot = 3;
            else if (key == "ms") slot = 4;
            if (slot < 0) {
                selection.ok = false;
                *out = std::move(selection);
                return shape_refusal(why, index, std::string("unknown key \"") + std::string(key) +
                                                          "\" (the measured keys are state, layout, ps, vs, ms)");
            }
            if (seen[slot]) {
                selection.ok = false;
                *out = std::move(selection);
                return shape_refusal(why, index, std::string("duplicate key \"") + std::string(key) + "\"");
            }
            seen[slot] = true;
            std::uint32_t parsed = 0;
            const bool ok = slot == 0
                                ? parse_shape_size(value, 380, 520, &parsed)
                                : slot == 1
                                      ? parse_shape_size(value, 80, 340, &parsed)
                                      : parse_shape_size(value, 0, 239464, &parsed);
            if (!ok) {
                selection.ok = false;
                *out = std::move(selection);
                return shape_refusal(why, index, std::string("refused ") + std::string(key) + " value \"" +
                                                          std::string(value) + "\" (decimal, measured range)");
            }
            if (slot == 0) shape.state_bytes = parsed;
            else if (slot == 1) shape.layout_bytes = parsed;
            else shape.stage_bytes[slot - 2] = parsed;
            if (key_end == std::string_view::npos) break;
            key_at = key_end + 1;
        }
        for (int slot = 0; slot < 5; ++slot)
            if (!seen[slot]) {
                selection.ok = false;
                *out = std::move(selection);
                return shape_refusal(why, index, "a size key is missing (all five of state, layout, ps, vs, ms must "
                                                 "be present)");
            }
        // A shape must name a programmable stage set a record can have: one of PS/VS/PS+MS, and never VS and MS
        // together (D3D12 refuses that pair, and the record decoder refuses it too). Program sizes are 0 or above the
        // measured program minimum; a smaller non-zero value cannot be a program.
        if (shape.stage_bytes[0] != 0 && shape.stage_bytes[0] < 52) {
            selection.ok = false;
            *out = std::move(selection);
            return shape_refusal(why, index, "ps is a non-zero size below the measured 52-byte program minimum");
        }
        if (shape.stage_bytes[1] != 0 && shape.stage_bytes[1] < 52) {
            selection.ok = false;
            *out = std::move(selection);
            return shape_refusal(why, index, "vs is a non-zero size below the measured 52-byte program minimum");
        }
        if (shape.stage_bytes[2] != 0 && shape.stage_bytes[2] < 52) {
            selection.ok = false;
            *out = std::move(selection);
            return shape_refusal(why, index, "ms is a non-zero size below the measured 52-byte program minimum");
        }
        if (shape.stage_bytes[1] != 0 && shape.stage_bytes[2] != 0) {
            selection.ok = false;
            *out = std::move(selection);
            return shape_refusal(why, index, "both vs and ms are named; a record has one programmable vertex stage");
        }
        if (shape.stage_bytes[0] == 0) {
            selection.ok = false;
            *out = std::move(selection);
            return shape_refusal(why, index, "ps is 0; the measured records carry a pixel stage");
        }
        if (shape.stage_bytes[1] == 0 && shape.stage_bytes[2] == 0) {
            selection.ok = false;
            *out = std::move(selection);
            return shape_refusal(why, index, "no vertex and no mesh stage; a record's geometry stage is missing");
        }
        for (const MeasuredRecordShape& earlier : selection.shapes)
            if (same_shape(earlier, shape)) {
                selection.ok = false;
                *out = std::move(selection);
                return shape_refusal(why, index, "the same record shape appears twice");
            }
        selection.shapes.push_back(shape);
        ++index;
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    *out = std::move(selection);
    return true;
}

const MeasuredRecordSelection& measured_record_selection() {
    static const MeasuredRecordSelection selection = [] {
        const char* value = std::getenv("PALMETTO_RASTER_RECORD_MEASURED");
        MeasuredRecordSelection parsed;
        if (value) {
            std::string why;
            if (!parse_measured_record_selection(value, &parsed, &why)) {
                if (!why.empty()) std::fprintf(stderr, "[rt] graphics_pso: measured record selection refused: %s\n",
                                               why.c_str());
                parsed.enabled = true;
                parsed.ok = false;
                parsed.error = why;
            }
        }
        return parsed;
    }();
    return selection;
}

MeasuredRecordShape measured_record_shape(const rt::graphics_record::Record& record) {
    MeasuredRecordShape shape;
    shape.state_bytes = static_cast<std::uint32_t>(record.state.size);
    shape.layout_bytes = static_cast<std::uint32_t>(record.input_layout.size);
    for (std::size_t i = 0; i < record.stage_count; ++i) {
        const int slot = record.stages[i].slot;
        if (slot >= 0 && slot < static_cast<int>(rt::graphics_record::kStageSlotCount))
            shape.stage_bytes[slot] = static_cast<std::uint32_t>(record.stages[i].program.size);
    }
    return shape;
}

bool decode_record_rasterizer_state(const rt::graphics_record::State& state,
                                    const rt::graphics_record::Record& record,
                                    const MeasuredRecordSelection& selection, RecordRasterState* out,
                                    FixedFunctionFieldStatus* fields, std::string* why) {
    if (fields) *fields = FixedFunctionFieldStatus{};
    if (!out) {
        if (why) *why = "state.record_rasterizer: the output is null";
        return false;
    }
    const char* const kPlace =
        "rasterizer.cull_mode.place: the state record's own words pin no rasterizer field (the only cull-enum word is "
        "the +0x2C stage-set word, docs/vertex_buffer_va.md section 21), so the measured cull value is applied by the "
        "measured record selection, not read from a word";
    if (!selection.enabled) {
        if (why) *why = std::string("state.record_rasterizer.off: PALMETTO_RASTER_RECORD_MEASURED is unset, so no "
                                    "record's cull is measured and the documented stand-in stays; refused: ") + kPlace;
        return false;
    }
    if (!selection.ok) {
        if (why) *why = "state.record_rasterizer.selector: " + selection.error;
        return false;
    }
    if (state.size == 0) {
        if (why) *why = "state.record_rasterizer.state: the state container did not decode, so the record cannot be "
                        "measured; the documented stand-in stays";
        return false;
    }
    // The measured records are vertex-family graphics pipelines. A mesh pipeline has no measured draw, and a record
    // whose state family disagrees with its stage slots is not the shape the record decoder accepts.
    bool mesh = false;
    for (std::size_t i = 0; i < record.stage_count; ++i)
        if (record.stages[i].slot == rt::graphics_record::kStageSlotMs) mesh = true;
    if (mesh) {
        if (why) *why = "state.record_rasterizer.record: the record names a mesh stage, and no mesh draw was measured "
                        "for the cull axis (docs/vertex_buffer_va.md section 21); the documented stand-in stays";
        return false;
    }
    if (state.family != rt::graphics_record::StateFamily::kVertex) {
        if (why) *why = "state.record_rasterizer.state: the state record is the mesh family, which is not the family "
                        "the measured records carry; the documented stand-in stays";
        return false;
    }
    const MeasuredRecordShape shape = measured_record_shape(record);
    for (const MeasuredRecordShape& measured : selection.shapes) {
        if (!same_shape(measured, shape)) continue;
        RecordRasterState result;
        result.cull_mode = D3D12_CULL_MODE_NONE;
        result.front_counter_clockwise = FALSE;
        *out = result;
        if (fields) fields->rasterizer = true;
        if (why) why->clear();
        return true;
    }
    if (why)
        *why = "state.record_rasterizer.record: the record's measured shape (state " +
               std::to_string(shape.state_bytes) + ", layout " + std::to_string(shape.layout_bytes) + ", ps/vs/ms " +
               std::to_string(shape.stage_bytes[0]) + "/" + std::to_string(shape.stage_bytes[1]) + "/" +
               std::to_string(shape.stage_bytes[2]) +
               ") is not one the measurement covers, so the documented stand-in stays; refused: " + kPlace;
    return false;
}

bool apply_measured_record_rasterizer_state(const rt::graphics_record::State& state,
                                            const rt::graphics_record::Record& record,
                                            const MeasuredRecordSelection& selection, FixedFunctionState* state_inout,
                                            FixedFunctionFieldStatus* fields, std::string* why) {
    if (!state_inout) {
        if (why) *why = "state.record_rasterizer: the state output is null";
        return false;
    }
    RecordRasterState raster;
    if (!decode_record_rasterizer_state(state, record, selection, &raster, fields, why)) return false;
    state_inout->rasterizer.CullMode = raster.cull_mode;
    state_inout->rasterizer.FrontCounterClockwise = raster.front_counter_clockwise;
    return true;
}

bool console_graphics_user_data(const rt::rootsig::RootSignature& signature, bool mesh_pipeline,
                                ConsoleUserDataPlacement* out) {
    if (!out) return false;
    ConsoleUserDataPlacement placement;
    placement.stage = mesh_pipeline ? rt::rootsig::Stage::Mesh : rt::rootsig::Stage::Vertex;
    placement.base = rt::rootsig::stage_base(placement.stage);
    // The same placement rule the compute bind point measured (D3D-448): root order from the stage base, each
    // parameter whole; a parameter that does not fit would spill through the console's root-signature spill pointer,
    // a layout this tree does not model, so the signature is refused with that parameter named.
    const rt::rootsig::StagePackResult packed = rt::rootsig::pack_stage(signature, placement.stage);
    if (!packed.ok) {
        placement.error = packed.error;
        *out = std::move(placement);
        return false;
    }
    placement.first_sgpr = packed.first_sgpr;
    placement.ok = true;
    *out = std::move(placement);
    return true;
}

ConsoleTableValue console_table_value(std::uint64_t arena_relative_va) {
    // The translated vertex/pixel shaders form `heap_base + (s8 & ~3)` from the console table slot (pinned from the
    // SPIR-V of lib_vs_00823: the slot's high half is forced to zero and the low two bits masked), so only a value
    // that fits one dword can be expressed. A larger arena would need a different encoding; refuse instead of
    // truncating, because a truncated address reads somewhere else.
    ConsoleTableValue value;
    if (arena_relative_va >> 32) return value;
    value.fits = true;
    value.dword = static_cast<std::uint32_t>(arena_relative_va);
    return value;
}

void classify_console_parameters(std::span<const ConsoleParameter> parameters, ConsoleSignatureClass* out) {
    if (!out) return;
    ConsoleSignatureClass result;
    result.parameters.assign(parameters.begin(), parameters.end());
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (result.table_index < 0 && parameters[i].type == rt::rootsig::ParamType::Table) result.table_index = int(i);
        if (result.table_index >= 0 && int(i) > result.table_index &&
            parameters[i].type == rt::rootsig::ParamType::Constants) {
            result.constant_after_table = int(i);
            result.constant_after_table_slot = parameters[i].first_sgpr;
            result.constant_after_table_dwords = parameters[i].dwords;
            break;
        }
    }
    result.ok = true;
    *out = std::move(result);
}

bool classify_console_signature(const rt::rootsig::RootSignature& signature, bool mesh_pipeline,
                                ConsoleSignatureClass* out) {
    if (!out) return false;
    ConsoleUserDataPlacement placement;
    const bool placed = console_graphics_user_data(signature, mesh_pipeline, &placement);
    std::vector<ConsoleParameter> parameters;
    parameters.reserve(signature.params.size());
    for (std::size_t i = 0; i < signature.params.size(); ++i) {
        const rt::rootsig::RootParameter& parameter = signature.params[i];
        ConsoleParameter entry;
        entry.type = parameter.type;
        entry.values = parameter.type == rt::rootsig::ParamType::Constants ? parameter.num_values : 0;
        entry.dwords = rt::rootsig::stage_dwords(parameter.type, parameter.num_values);
        entry.first_sgpr = placed && i < placement.first_sgpr.size() ? placement.first_sgpr[i]
                                                                    : rt::rootsig::kNoSgpr;
        parameters.push_back(entry);
    }
    if (!placed) {
        ConsoleSignatureClass refused;
        refused.error = placement.error;
        refused.parameters = std::move(parameters);
        *out = std::move(refused);
        return false;
    }
    classify_console_parameters(parameters, out);
    return true;
}

bool console_feed_plan(std::span<const ConsoleFeedParameter> parameters, ConsoleFeedPlan* out) {
    if (!out) return false;
    ConsoleFeedPlan plan;
    // The two feeds are written into one block, so a console dword may not land on a dword the PC packing gives a
    // parameter: another shader reads that dword at its PC slot. Console offsets that were never placed (kNoSgpr) are
    // skipped, and a signature with no console placement feeds nothing and is trivially safe.
    plan.feed = true;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const ConsoleFeedParameter& p = parameters[i];
        if (p.console_first == rt::rootsig::kNoSgpr || !p.console_dwords) continue;
        const std::uint64_t console_end = std::uint64_t(p.console_first) + p.console_dwords;
        if (console_end > rt::rootsig::kMaxUserSgprs) {
            plan.feed = false;
            plan.why = "parameter " + std::to_string(i) + " would spill past the " +
                       std::to_string(rt::rootsig::kMaxUserSgprs) + " user-data dwords";
            *out = std::move(plan);
            return false;
        }
        for (std::size_t j = 0; j < parameters.size(); ++j) {
            const ConsoleFeedParameter& q = parameters[j];
            if (!q.pc_dwords) continue;
            const std::uint64_t pc_begin = q.pc_first;
            const std::uint64_t pc_end = pc_begin + q.pc_dwords;
            if (std::uint64_t(p.console_first) >= pc_end || pc_begin >= console_end) continue;
            plan.feed = false;
            plan.why = "parameter " + std::to_string(i) + "'s console slot s" + std::to_string(p.console_first) +
                       " overlaps parameter " + std::to_string(j) + "'s PC dword(s) from s" +
                       std::to_string(pc_begin);
            *out = std::move(plan);
            return false;
        }
    }
    *out = std::move(plan);
    return out->feed;
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
    // The per-record measured rasterizer state (docs/vertex_buffer_va.md section 21): the measured record selection
    // replaces only the cull pair of a record the measurement covers; every other record and every other field keeps
    // the caller's state. Off by default, so the default path is byte-identical to the caller's description.
    FixedFunctionState measured_defaults = inputs.defaults;
    {
        const MeasuredRecordSelection& selection = measured_record_selection();
        if (selection.enabled) {
            FixedFunctionFieldStatus record_fields;
            std::string record_why;
            if (apply_measured_record_rasterizer_state(state, record, selection, &measured_defaults, &record_fields,
                                                       &record_why)) {
                const MeasuredRecordShape shape = measured_record_shape(record);
                log_measured_record_event(&shape, true, record_why);
            } else {
                log_measured_record_event(nullptr, false, record_why);
            }
        }
    }
    const FixedFunctionState& ff = measured_defaults;
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

// ---- The behavioural probe: a GPU-free model of what one fixed-function field changes on screen ------------------
//
// The contract (graphics_pso.h, kProbeContract) pairs each FixedFunctionState field group with one minimal scene and
// the observable that changes with it. The model below is deliberately small - a 4x4 RGBA8 target, a per-sample depth
// buffer, D3D12's standard sample positions and one render target - so every answer is exact and reviewable rather
// than approximate: each scene was placed so the pixels it touches have one unambiguous answer, and
// port/test/graphics_pso_test.cpp checks the pair of values that must differ for every observable field.
//
// What the model does not express is named where it applies (the contract's `distinguishes` text says which
// sub-fields a scene observes) and is counted in ProbeOutcome::unmodelled instead of silently treated as a value.
namespace {

constexpr int kProbeSize = 4;
constexpr int kProbePixels = kProbeSize * kProbeSize;
constexpr int kProbeMaxSamples = 16;
// The blend factor the probe's scenes would use for D3D12_BLEND_BLEND_FACTOR: the API's blend factor is dynamic
// state, not a FixedFunctionState field, so the probe fixes one value and the factors that read it stay distinct.
constexpr float kProbeBlendFactor = 0.25f;

struct ProbeVertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
    float a;
};

struct SamplePosition {
    float x;
    float y;
};

// D3D12's standard sample positions per sample count. Only kSampleGrid asks a question about a partial pixel, and it
// asks it about the first sample position, so the exact pattern matters only as a fixed, documented set.
constexpr SamplePosition kSamples1[] = {{0.5f, 0.5f}};
constexpr SamplePosition kSamples2[] = {{0.75f, 0.25f}, {0.25f, 0.75f}};
constexpr SamplePosition kSamples4[] = {{0.375f, 0.125f}, {0.875f, 0.375f}, {0.125f, 0.625f}, {0.625f, 0.875f}};
constexpr SamplePosition kSamples8[] = {{0.5625f, 0.3125f}, {0.4375f, 0.6875f}, {0.8125f, 0.5625f},
                                        {0.3125f, 0.1875f}, {0.1875f, 0.8125f}, {0.0625f, 0.4375f},
                                        {0.6875f, 0.9375f}, {0.9375f, 0.0625f}};
constexpr SamplePosition kSamples16[] = {{0.125f, 0.125f}, {0.375f, 0.125f}, {0.625f, 0.125f}, {0.875f, 0.125f},
                                         {0.125f, 0.375f}, {0.375f, 0.375f}, {0.625f, 0.375f}, {0.875f, 0.375f},
                                         {0.125f, 0.625f}, {0.375f, 0.625f}, {0.625f, 0.625f}, {0.875f, 0.625f},
                                         {0.125f, 0.875f}, {0.375f, 0.875f}, {0.625f, 0.875f}, {0.875f, 0.875f}};

// The probe scenes, in clip space (w = 1). The winding of every full-quad triangle is clockwise on the D3D screen, so
// the documented default rasterizer (back-face culling, clockwise front) draws them; the winding pair is the one scene
// that carries both windings on purpose.
constexpr ProbeVertex kWindingPair[] = {
    // left half, counter-clockwise on screen (back-facing under a clockwise front face)
    {-1.0f, -1.0f, 0.5f, 1, 1, 1, 1}, {0.0f, -1.0f, 0.5f, 1, 1, 1, 1}, {-1.0f, 1.0f, 0.5f, 1, 1, 1, 1},
    // right half, clockwise on screen (front-facing under a clockwise front face)
    {0.0f, -1.0f, 0.5f, 1, 1, 1, 1}, {1.0f, 1.0f, 0.5f, 1, 1, 1, 1}, {1.0f, -1.0f, 0.5f, 1, 1, 1, 1},
};
// The full-target layers of the depth, blend, topology and clip scenes are split into four triangles with horizontal
// and vertical edges at whole target coordinates (never through a sample position), so no sample lies on a shared
// edge and the per-triangle coverage is unambiguous.
constexpr ProbeVertex kTwoDepthQuads[] = {
    // the near layer, red at z = 0.25, drawn first
    {-1, 1, 0.25f, 1, 0, 0, 1}, {1, 1, 0.25f, 1, 0, 0, 1}, {-1, 0, 0.25f, 1, 0, 0, 1},
    {-1, 0, 0.25f, 1, 0, 0, 1}, {1, 1, 0.25f, 1, 0, 0, 1}, {1, 0, 0.25f, 1, 0, 0, 1},
    {-1, 0, 0.25f, 1, 0, 0, 1}, {1, 0, 0.25f, 1, 0, 0, 1}, {-1, -1, 0.25f, 1, 0, 0, 1},
    {-1, -1, 0.25f, 1, 0, 0, 1}, {1, 0, 0.25f, 1, 0, 0, 1}, {1, -1, 0.25f, 1, 0, 0, 1},
    // the far layer, green at z = 0.75, drawn second
    {-1, 1, 0.75f, 0, 1, 0, 1}, {1, 1, 0.75f, 0, 1, 0, 1}, {-1, 0, 0.75f, 0, 1, 0, 1},
    {-1, 0, 0.75f, 0, 1, 0, 1}, {1, 1, 0.75f, 0, 1, 0, 1}, {1, 0, 0.75f, 0, 1, 0, 1},
    {-1, 0, 0.75f, 0, 1, 0, 1}, {1, 0, 0.75f, 0, 1, 0, 1}, {-1, -1, 0.75f, 0, 1, 0, 1},
    {-1, -1, 0.75f, 0, 1, 0, 1}, {1, 0, 0.75f, 0, 1, 0, 1}, {1, -1, 0.75f, 0, 1, 0, 1},
};
constexpr ProbeVertex kOverlap[] = {
    // the first layer, drawn first
    {-1, 1, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f}, {1, 1, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f},
    {-1, 0, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f}, {-1, 0, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f},
    {1, 1, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f}, {1, 0, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f},
    {-1, 0, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f}, {1, 0, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f},
    {-1, -1, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f}, {-1, -1, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f},
    {1, 0, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f}, {1, -1, 0.5f, 0.25f, 0.5f, 1.0f, 0.5f},
    // the second layer, drawn second: alpha 0.25, so a source-alpha blend tells the two apart
    {-1, 1, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f}, {1, 1, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f},
    {-1, 0, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f}, {-1, 0, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f},
    {1, 1, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f}, {1, 0, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f},
    {-1, 0, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f}, {1, 0, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f},
    {-1, -1, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f}, {-1, -1, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f},
    {1, 0, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f}, {1, -1, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f},
};
constexpr ProbeVertex kTopologyList[] = {
    {-1, 1, 0.5f, 1, 1, 1, 1}, {1, 1, 0.5f, 1, 1, 1, 1}, {-1, 0, 0.5f, 1, 1, 1, 1},
    {-1, 0, 0.5f, 1, 1, 1, 1}, {1, 1, 0.5f, 1, 1, 1, 1}, {1, 0, 0.5f, 1, 1, 1, 1},
    {-1, 0, 0.5f, 1, 1, 1, 1}, {1, 0, 0.5f, 1, 1, 1, 1}, {-1, -1, 0.5f, 1, 1, 1, 1},
    {-1, -1, 0.5f, 1, 1, 1, 1}, {1, 0, 0.5f, 1, 1, 1, 1}, {1, -1, 0.5f, 1, 1, 1, 1},
};
constexpr ProbeVertex kStripCut[] = {
    {-1.0f, -1.0f, 0.5f, 1, 1, 1, 1}, {1.0f, 1.0f, 0.5f, 1, 1, 1, 1}, {1.0f, -1.0f, 0.5f, 1, 1, 1, 1},
    {-1.0f, 1.0f, 0.5f, 1, 1, 1, 1}, {-1.0f, -1.0f, 0.5f, 1, 1, 1, 1}, {1.0f, 1.0f, 0.5f, 1, 1, 1, 1},
};
// 0xFFFF is the strip's cut sentinel, and it is not a valid index into the six vertices: a cut value equal to it
// restarts the strip, any other cut value lets the strip read it as a vertex index.
constexpr std::uint32_t kStripCutIndices[] = {0u, 1u, 2u, 0xFFFFu, 3u, 4u};
// Screen (1.3,1.0) (1.6,1.0) (1.3,1.3) on the 4x4 target, converted to clip space: it covers the first of the four
// standard sample positions inside pixel (1,1) and not the pixel centre.
constexpr ProbeVertex kSampleGrid[] = {
    {-0.35f, 0.5f, 0.5f, 1, 1, 1, 1}, {-0.2f, 0.5f, 0.5f, 1, 1, 1, 1}, {-0.35f, 0.35f, 0.5f, 1, 1, 1, 1},
};
constexpr ProbeVertex kDepthClip[] = {
    {-1, 1, 1.5f, 1, 1, 1, 1}, {1, 1, 1.5f, 1, 1, 1, 1}, {-1, 0, 1.5f, 1, 1, 1, 1},
    {-1, 0, 1.5f, 1, 1, 1, 1}, {1, 1, 1.5f, 1, 1, 1, 1}, {1, 0, 1.5f, 1, 1, 1, 1},
    {-1, 0, 1.5f, 1, 1, 1, 1}, {1, 0, 1.5f, 1, 1, 1, 1}, {-1, -1, 1.5f, 1, 1, 1, 1},
    {-1, -1, 1.5f, 1, 1, 1, 1}, {1, 0, 1.5f, 1, 1, 1, 1}, {1, -1, 1.5f, 1, 1, 1, 1},
};

enum class ProbeTopology { kPointList, kLineList, kLineStrip, kTriangleList, kTriangleStrip };

struct ProbeSceneData {
    ProbeTopology topology = ProbeTopology::kTriangleList;
    bool indexed = false;
    const ProbeVertex* vertices = nullptr;
    std::size_t vertex_count = 0;
    const std::uint32_t* indices = nullptr;
    std::size_t index_count = 0;
};

ProbeSceneData probe_scene(ProbeScene scene) {
    switch (scene) {
        case ProbeScene::kWindingPair:
            return {ProbeTopology::kTriangleList, false, kWindingPair, std::size(kWindingPair), nullptr, 0};
        case ProbeScene::kTwoDepthQuads:
            return {ProbeTopology::kTriangleList, false, kTwoDepthQuads, std::size(kTwoDepthQuads), nullptr, 0};
        case ProbeScene::kOverlap:
            return {ProbeTopology::kTriangleList, false, kOverlap, std::size(kOverlap), nullptr, 0};
        case ProbeScene::kTopologyList:
            return {ProbeTopology::kTriangleList, false, kTopologyList, std::size(kTopologyList), nullptr, 0};
        case ProbeScene::kStripCut:
            return {ProbeTopology::kTriangleStrip, true, kStripCut, std::size(kStripCut), kStripCutIndices,
                    std::size(kStripCutIndices)};
        case ProbeScene::kSampleGrid:
            return {ProbeTopology::kTriangleList, false, kSampleGrid, std::size(kSampleGrid), nullptr, 0};
        case ProbeScene::kDepthClip:
            return {ProbeTopology::kTriangleList, false, kDepthClip, std::size(kDepthClip), nullptr, 0};
    }
    return {};
}

float clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

void to_screen(const ProbeVertex& vertex, float* x, float* y) {
    *x = (vertex.x + 1.0f) * 0.5f * static_cast<float>(kProbeSize);
    *y = (1.0f - vertex.y) * 0.5f * static_cast<float>(kProbeSize);
}

bool depth_test(D3D12_COMPARISON_FUNC func, float lhs, float rhs) {
    switch (func) {
        case D3D12_COMPARISON_FUNC_NEVER: return false;
        case D3D12_COMPARISON_FUNC_LESS: return lhs < rhs;
        case D3D12_COMPARISON_FUNC_EQUAL: return lhs == rhs;
        case D3D12_COMPARISON_FUNC_LESS_EQUAL: return lhs <= rhs;
        case D3D12_COMPARISON_FUNC_GREATER: return lhs > rhs;
        case D3D12_COMPARISON_FUNC_NOT_EQUAL: return lhs != rhs;
        case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return lhs >= rhs;
        case D3D12_COMPARISON_FUNC_ALWAYS: return true;
        default: return true;  // an invalid enum is counted as unmodelled before the test
    }
}

float blend_op(D3D12_BLEND_OP op, float src, float dst) {
    switch (op) {
        case D3D12_BLEND_OP_ADD: return src + dst;
        case D3D12_BLEND_OP_SUBTRACT: return src - dst;
        case D3D12_BLEND_OP_REV_SUBTRACT: return dst - src;
        case D3D12_BLEND_OP_MIN: return src < dst ? src : dst;
        case D3D12_BLEND_OP_MAX: return src > dst ? src : dst;
        default: return src + dst;
    }
}

// The four factor channels one D3D12_BLEND value contributes to the blend, or false when the probe cannot express it
// (the dual-source SRC1_* factors need two pixel-shader outputs, which the probe does not carry).
bool blend_factor(D3D12_BLEND factor, const float src[4], const float dst[4], float out[4]) {
    const auto set = [&out](float r, float g, float b, float a) {
        out[0] = r;
        out[1] = g;
        out[2] = b;
        out[3] = a;
    };
    switch (factor) {
        case D3D12_BLEND_ZERO: set(0.0f, 0.0f, 0.0f, 0.0f); return true;
        case D3D12_BLEND_ONE: set(1.0f, 1.0f, 1.0f, 1.0f); return true;
        case D3D12_BLEND_SRC_COLOR: set(src[0], src[1], src[2], src[3]); return true;
        case D3D12_BLEND_INV_SRC_COLOR: set(1 - src[0], 1 - src[1], 1 - src[2], 1 - src[3]); return true;
        case D3D12_BLEND_SRC_ALPHA: set(src[3], src[3], src[3], src[3]); return true;
        case D3D12_BLEND_INV_SRC_ALPHA: set(1 - src[3], 1 - src[3], 1 - src[3], 1 - src[3]); return true;
        case D3D12_BLEND_DEST_ALPHA: set(dst[3], dst[3], dst[3], dst[3]); return true;
        case D3D12_BLEND_INV_DEST_ALPHA: set(1 - dst[3], 1 - dst[3], 1 - dst[3], 1 - dst[3]); return true;
        case D3D12_BLEND_DEST_COLOR: set(dst[0], dst[1], dst[2], dst[3]); return true;
        case D3D12_BLEND_INV_DEST_COLOR: set(1 - dst[0], 1 - dst[1], 1 - dst[2], 1 - dst[3]); return true;
        case D3D12_BLEND_SRC_ALPHA_SAT: {
            const float saturated = src[3] < 1 - dst[3] ? src[3] : 1 - dst[3];
            set(saturated, saturated, saturated, 1.0f);
            return true;
        }
        case D3D12_BLEND_BLEND_FACTOR:
            set(kProbeBlendFactor, kProbeBlendFactor, kProbeBlendFactor, kProbeBlendFactor);
            return true;
        case D3D12_BLEND_INV_BLEND_FACTOR:
            set(1 - kProbeBlendFactor, 1 - kProbeBlendFactor, 1 - kProbeBlendFactor, 1 - kProbeBlendFactor);
            return true;
        default: set(1.0f, 1.0f, 1.0f, 1.0f); return false;
    }
}

bool topology_type_matches(D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type, ProbeTopology topology) {
    switch (topology_type) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: return topology == ProbeTopology::kPointList;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
            return topology == ProbeTopology::kLineList || topology == ProbeTopology::kLineStrip;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
            return topology == ProbeTopology::kTriangleList || topology == ProbeTopology::kTriangleStrip;
        default: return false;  // UNDEFINED and PATCH: the probe has no patch/tessellation model
    }
}

struct ProbeTarget {
    const FixedFunctionState& state;
    int sample_count = 1;
    const SamplePosition* samples = kSamples1;
    std::array<float, kProbePixels * kProbeMaxSamples> depth{};
    std::array<float, kProbePixels * kProbeMaxSamples * 4> colour{};
    std::array<std::uint32_t, kProbePixels> covered{};
    ProbeOutcome outcome;
};

void shade_fragment(ProbeTarget& target, std::size_t pixel, int sample, float z, const float fragment[4]) {
    if ((target.state.sample_mask & (1u << static_cast<unsigned>(sample))) == 0) return;
    target.covered[pixel] |= 1u << static_cast<unsigned>(sample);
    ++target.outcome.fragments;
    const std::size_t at = pixel * kProbeMaxSamples + static_cast<std::size_t>(sample);
    if (target.state.depth_stencil.DepthEnable != FALSE) {
        if (!depth_test(target.state.depth_stencil.DepthFunc, z, target.depth[at])) {
            ++target.outcome.depth_failed;
            return;
        }
        if (target.state.depth_stencil.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO) target.depth[at] = z;
    }
    const D3D12_RENDER_TARGET_BLEND_DESC& blend = target.state.blend.RenderTarget[0];
    float source[4] = {fragment[0], fragment[1], fragment[2], fragment[3]};
    if (blend.BlendEnable != FALSE) {
        float dst[4] = {target.colour[at * 4 + 0], target.colour[at * 4 + 1], target.colour[at * 4 + 2],
                        target.colour[at * 4 + 3]};
        float src_factor[4] = {1, 1, 1, 1};
        float dst_factor[4] = {0, 0, 0, 0};
        float src_alpha_factor[4] = {1, 1, 1, 1};
        float dst_alpha_factor[4] = {0, 0, 0, 0};
        if (!blend_factor(blend.SrcBlend, source, dst, src_factor) ||
            !blend_factor(blend.DestBlend, source, dst, dst_factor) ||
            !blend_factor(blend.SrcBlendAlpha, source, dst, src_alpha_factor) ||
            !blend_factor(blend.DestBlendAlpha, source, dst, dst_alpha_factor))
            ++target.outcome.unmodelled;
        for (int channel = 0; channel < 3; ++channel)
            source[channel] =
                blend_op(blend.BlendOp, source[channel] * src_factor[channel], dst[channel] * dst_factor[channel]);
        source[3] = blend_op(blend.BlendOpAlpha, source[3] * src_alpha_factor[3], dst[3] * dst_alpha_factor[3]);
    }
    for (int channel = 0; channel < 4; ++channel) {
        const UINT mask = 1u << static_cast<unsigned>(channel);
        if ((blend.RenderTargetWriteMask & mask) != 0)
            target.colour[at * 4 + static_cast<std::size_t>(channel)] = clamp01(source[channel]);
    }
    ++target.outcome.written;
}

// Rasterizes one clipped triangle: front-face/cull first, then the per-sample point-in-triangle test. Colours and depth
// are interpolated linearly (every probe vertex has w = 1, so screen-space linear interpolation is exact).
void rasterize_triangle(ProbeTarget& target, const ProbeVertex& a, const ProbeVertex& b, const ProbeVertex& c) {
    float ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
    to_screen(a, &ax, &ay);
    to_screen(b, &bx, &by);
    to_screen(c, &cx, &cy);
    const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0.0f) {
        ++target.outcome.culled;  // a zero-area triangle covers no sample
        return;
    }
    const bool clockwise = area > 0.0f;
    const bool front = clockwise != (target.state.rasterizer.FrontCounterClockwise != FALSE);
    switch (target.state.rasterizer.CullMode) {
        case D3D12_CULL_MODE_FRONT:
            if (front) {
                ++target.outcome.culled;
                return;
            }
            break;
        case D3D12_CULL_MODE_BACK:
            if (!front) {
                ++target.outcome.culled;
                return;
            }
            break;
        case D3D12_CULL_MODE_NONE: break;
        default: ++target.outcome.unmodelled; break;
    }
    float za = a.z, zb = b.z, zc = c.z;
    if (target.state.rasterizer.DepthClipEnable != FALSE) {
        if (za < 0.0f || za > 1.0f || zb < 0.0f || zb > 1.0f || zc < 0.0f || zc > 1.0f) {
            ++target.outcome.clipped;
            return;
        }
    } else {
        za = clamp01(za);
        zb = clamp01(zb);
        zc = clamp01(zc);
    }
    for (int row = 0; row < kProbeSize; ++row) {
        for (int column = 0; column < kProbeSize; ++column) {
            const std::size_t pixel = static_cast<std::size_t>(row * kProbeSize + column);
            for (int sample = 0; sample < target.sample_count; ++sample) {
                const float px = static_cast<float>(column) + target.samples[sample].x;
                const float py = static_cast<float>(row) + target.samples[sample].y;
                const float w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / area;
                const float w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float fragment[4] = {
                    w0 * a.r + w1 * b.r + w2 * c.r, w0 * a.g + w1 * b.g + w2 * c.g,
                    w0 * a.b + w1 * b.b + w2 * c.b, w0 * a.a + w1 * b.a + w2 * c.a};
                shade_fragment(target, pixel, sample, w0 * za + w1 * zb + w2 * zc, fragment);
            }
        }
    }
}

void rasterize_line(ProbeTarget& target, const ProbeVertex& a, const ProbeVertex& b) {
    float ax = 0, ay = 0, bx = 0, by = 0;
    to_screen(a, &ax, &ay);
    to_screen(b, &bx, &by);
    if (target.state.rasterizer.DepthClipEnable != FALSE &&
        (a.z < 0.0f || a.z > 1.0f || b.z < 0.0f || b.z > 1.0f)) {
        ++target.outcome.clipped;
        return;
    }
    const float dx = bx - ax;
    const float dy = by - ay;
    const float length_squared = dx * dx + dy * dy;
    for (int row = 0; row < kProbeSize; ++row) {
        for (int column = 0; column < kProbeSize; ++column) {
            const std::size_t pixel = static_cast<std::size_t>(row * kProbeSize + column);
            for (int sample = 0; sample < target.sample_count; ++sample) {
                const float px = static_cast<float>(column) + target.samples[sample].x;
                const float py = static_cast<float>(row) + target.samples[sample].y;
                float t = length_squared > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / length_squared : 0.0f;
                t = clamp01(t);
                const float ex = ax + t * dx - px;
                const float ey = ay + t * dy - py;
                if (ex * ex + ey * ey > 0.25f) continue;  // a sample within half a pixel of the segment
                const float fragment[4] = {a.r + t * (b.r - a.r), a.g + t * (b.g - a.g), a.b + t * (b.b - a.b),
                                           a.a + t * (b.a - a.a)};
                shade_fragment(target, pixel, sample, a.z + t * (b.z - a.z), fragment);
            }
        }
    }
}

void rasterize_point(ProbeTarget& target, const ProbeVertex& vertex) {
    float x = 0, y = 0;
    to_screen(vertex, &x, &y);
    if (target.state.rasterizer.DepthClipEnable != FALSE && (vertex.z < 0.0f || vertex.z > 1.0f)) {
        ++target.outcome.clipped;
        return;
    }
    const float fragment[4] = {vertex.r, vertex.g, vertex.b, vertex.a};
    for (int row = 0; row < kProbeSize; ++row) {
        for (int column = 0; column < kProbeSize; ++column) {
            const std::size_t pixel = static_cast<std::size_t>(row * kProbeSize + column);
            for (int sample = 0; sample < target.sample_count; ++sample) {
                const float px = static_cast<float>(column) + target.samples[sample].x;
                const float py = static_cast<float>(row) + target.samples[sample].y;
                const float ex = px - x;
                const float ey = py - y;
                if (ex * ex + ey * ey > 0.25f) continue;  // a sample within half a pixel of the point
                shade_fragment(target, pixel, sample, vertex.z, fragment);
            }
        }
    }
}

std::uint8_t quantize(float value) {
    const float clamped = clamp01(value);
    return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
}

}  // namespace

ProbeOutcome run_probe(const FixedFunctionState& state, ProbeScene scene) {
    const ProbeSceneData data = probe_scene(scene);
    ProbeOutcome outcome;
    if (data.vertices == nullptr) return outcome;
    if (!topology_type_matches(state.topology_type, data.topology)) {
        outcome.mismatch = 1;
        return outcome;
    }

    ProbeTarget target{state};
    switch (state.sample_count) {
        case 1: target.sample_count = 1; target.samples = kSamples1; break;
        case 2: target.sample_count = 2; target.samples = kSamples2; break;
        case 4: target.sample_count = 4; target.samples = kSamples4; break;
        case 8: target.sample_count = 8; target.samples = kSamples8; break;
        case 16: target.sample_count = 16; target.samples = kSamples16; break;
        default: ++target.outcome.unmodelled; target.sample_count = 1; target.samples = kSamples1; break;
    }
    if (state.rasterizer.FillMode != D3D12_FILL_MODE_SOLID) ++target.outcome.unmodelled;  // no wireframe model
    target.depth.fill(1.0f);
    target.colour.fill(0.0f);

    // Assemble the primitives from the scene's topology, cut value and index/vertex sequence.
    const std::uint32_t cut_value =
        state.strip_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
            ? 0xFFFFu
            : (state.strip_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF ? 0xFFFFFFFFu : 0u);
    const bool cut_enabled = data.indexed && state.strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    constexpr std::size_t kProbeMaxVertices = 32;  // every probe scene's vertex/index list fits
    std::array<std::size_t, kProbeMaxVertices> sequence{};
    std::size_t sequence_count = 0;
    for (std::size_t i = 0; i < data.vertex_count && sequence_count < sequence.size(); ++i)
        sequence[sequence_count++] = i;
    if (data.indexed) {
        sequence_count = 0;
        for (std::size_t i = 0; i < data.index_count && sequence_count < sequence.size(); ++i)
            sequence[sequence_count++] = data.indices[i];
    }

    switch (data.topology) {
        case ProbeTopology::kPointList:            for (std::size_t i = 0; i < sequence_count; ++i) {
                if (sequence[i] >= data.vertex_count) {
                    ++target.outcome.out_of_range;
                    continue;
                }
                ++target.outcome.primitives;
                rasterize_point(target, data.vertices[sequence[i]]);
            }
            break;
        case ProbeTopology::kLineList:
            for (std::size_t i = 0; i + 1 < sequence_count; i += 2) {
                if (sequence[i] >= data.vertex_count || sequence[i + 1] >= data.vertex_count) {
                    ++target.outcome.out_of_range;
                    continue;
                }
                ++target.outcome.primitives;
                rasterize_line(target, data.vertices[sequence[i]], data.vertices[sequence[i + 1]]);
            }
            break;
        case ProbeTopology::kLineStrip: {
            const ProbeVertex* previous = nullptr;
            for (std::size_t i = 0; i < sequence_count; ++i) {
                if (cut_enabled && data.indices[i] == cut_value) {
                    previous = nullptr;
                    continue;
                }
                if (sequence[i] >= data.vertex_count) {
                    ++target.outcome.out_of_range;
                    previous = nullptr;
                    continue;
                }
                const ProbeVertex* current = &data.vertices[sequence[i]];
                if (previous != nullptr) {
                    ++target.outcome.primitives;
                    rasterize_line(target, *previous, *current);
                }
                previous = current;
            }
            break;
        }
        case ProbeTopology::kTriangleList:
            for (std::size_t i = 0; i + 2 < sequence_count; i += 3) {
                if (sequence[i] >= data.vertex_count || sequence[i + 1] >= data.vertex_count ||
                    sequence[i + 2] >= data.vertex_count) {
                    ++target.outcome.out_of_range;
                    continue;
                }
                ++target.outcome.primitives;
                rasterize_triangle(target, data.vertices[sequence[i]], data.vertices[sequence[i + 1]],
                                   data.vertices[sequence[i + 2]]);
            }
            break;
        case ProbeTopology::kTriangleStrip: {
            std::array<std::size_t, 3> window{};
            std::size_t window_count = 0;
            std::size_t run_position = 0;
            for (std::size_t i = 0; i < sequence_count; ++i) {
                if (cut_enabled && data.indices[i] == cut_value) {
                    window_count = 0;
                    run_position = 0;
                    continue;
                }
                window[0] = window[1];
                window[1] = window[2];
                window[2] = sequence[i];
                ++run_position;
                ++window_count;
                if (window_count < 3) continue;
                // Strip winding alternates: triangle t is (t, t+1, t+2) for even t and (t+1, t, t+2) for odd t.
                const bool even_triangle = ((run_position - 3) % 2) == 0;
                if (window[0] >= data.vertex_count || window[1] >= data.vertex_count ||
                    window[2] >= data.vertex_count) {
                    ++target.outcome.out_of_range;
                    continue;
                }
                const std::size_t first = even_triangle ? window[0] : window[1];
                const std::size_t second = even_triangle ? window[1] : window[0];
                ++target.outcome.primitives;
                rasterize_triangle(target, data.vertices[first], data.vertices[second], data.vertices[window[2]]);
            }
            break;
        }
    }

    // Resolve: the average of every sample in the pixel, with uncovered samples left at the cleared colour.
    for (std::size_t pixel = 0; pixel < kProbePixels; ++pixel) {
        std::uint32_t covered = target.covered[pixel];
        std::uint8_t covered_count = 0;
        while (covered != 0) {
            covered_count = static_cast<std::uint8_t>(covered_count + (covered & 1u));
            covered >>= 1u;
        }
        target.outcome.coverage[pixel] = covered_count;
        float sum[4] = {0, 0, 0, 0};
        for (int sample = 0; sample < target.sample_count; ++sample) {
            const std::size_t at = pixel * kProbeMaxSamples + static_cast<std::size_t>(sample);
            for (int channel = 0; channel < 4; ++channel) sum[channel] += target.colour[at * 4 + channel];
        }
        for (int channel = 0; channel < 4; ++channel)
            target.outcome.colour[pixel * 4 + static_cast<std::size_t>(channel)] =
                quantize(sum[channel] / static_cast<float>(target.sample_count));
    }
    return target.outcome;
}

// ---- The mesh pipeline's description: the pipeline-state-stream form (docs/mesh_stage.md) -------------------------
namespace {

std::size_t round_up_to(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// A bounded line: never more than `limit` characters, so a boot log line is bounded by construction.
std::string bounded_line(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit - 3);
        text += "...";
    }
    return text;
}

// The subobject runner of a D3D12_PIPELINE_STATE_STREAM_DESC: a subobject is its type word, its payload at that
// payload's alignment, and the next subobject starts pointer-aligned - the d3dx12.h layout, which is exactly the rule
// the PC device's parse_pipeline_stream() (port/shims/d3d12_vulkan_device.cpp) applies. The buffer is zero-filled
// between subobjects: the parser skips those bytes, they are not payload.
class SubobjectWriter {
public:
    template<typename T>
    void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& value) {
        const std::size_t type_at = bytes_.size();
        const std::size_t payload = round_up_to(type_at + sizeof(type), alignof(T));
        bytes_.resize(payload + sizeof(T));
        std::memcpy(bytes_.data() + type_at, &type, sizeof(type));
        std::memcpy(bytes_.data() + payload, &value, sizeof(T));
        bytes_.resize(round_up_to(bytes_.size(), alignof(void*)));
        ++count_;
    }
    std::vector<std::uint8_t> take() { return std::move(bytes_); }
    std::size_t count() const { return count_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t count_ = 0;
};

}  // namespace

std::string MeshPipelineStream::describe() const {
    char line[200] = {};
    std::snprintf(line, sizeof line, "mesh stream: %zu subobject(s), %zu B, ms %u B, ps %u B", subobject_count,
                  bytes.size(), ms_bytes, ps_bytes);
    return bounded_line(line, 160);
}

std::string MeshStageCounters::summary() const {
    char line[160] = {};
    std::snprintf(line, sizeof line, "mesh stages: records %llu, streams %llu, refusals %llu",
                  static_cast<unsigned long long>(records_with_mesh_stage),
                  static_cast<unsigned long long>(streams_built), static_cast<unsigned long long>(refusals));
    return bounded_line(line, 120);
}

bool build_mesh_stream(const rt::graphics_record::Record& record, const GraphicsPsoInputs& inputs,
                       MeshPipelineStream* out, std::string* why) {
    if (why) why->clear();
    if (!out) return fail(why, "build_mesh_stream: the output stream is null");

    // ---- The record's shape, checked again rather than trusted (this is a public entry point). A mesh pipeline is
    // built from a record whose slot 2 is used: the measured shapes are PS+MS and MS only (docs/mesh_stage.md).
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
    const bool pixel = used[rt::graphics_record::kStageSlotPs];
    if (vertex && mesh)
        return fail(why, "record.stages: a vertex and a mesh stage together are not a stage combination D3D12 accepts");
    if (!mesh)
        return fail(why, "record.stages: the record names no mesh stage; a mesh pipeline needs the record's slot 2 "
                          "(the stream's MS subobject), and the vertex-family shapes are build()'s "
                          "D3D12_GRAPHICS_PIPELINE_STATE_DESC path");
    if (record.has_unidentified_stage)
        return fail(why, "record.stages: the record names the unidentified slot-6 program (" +
                             std::to_string(record.unidentified_program.size) +
                             " B; 2 measured library records, no measured stage), which cannot be carried into a "
                             "pipeline, so the record is refused instead of dropping the program");

    // ---- The record's blobs. The state is the measured mesh family (its size names the record's programmable stage,
    // docs/state_blob.md); the input layout is the measured 80-byte empty one (docs/shader_library.md section 2).
    if (!record.root_signature)
        return fail(why, "record.root_signature: the record has no root signature at +0x00, so the pipeline's "
                         "bindings cannot be established");
    rt::graphics_record::State state;
    if (!rt::graphics_record::decode_state(record.state, &state))
        return fail(why, "record.state: the fixed-function state blob is missing or outside its measured envelope "
                         "(380..520 bytes, a whole number of words), so the record is not the shape this converter "
                         "knows");
    if (state.family != rt::graphics_record::StateFamily::kMesh)
        return fail(why, "record.state: a record with a mesh stage carries the measured mesh-family state record "
                         "(464 + 8*k bytes, docs/state_blob.md); this one is the vertex family, so its shape is not "
                         "the measured mesh one");
    std::vector<rt::graphics_record::Element> elements;
    if (!rt::graphics_record::decode_input_layout(record.input_layout, &elements))
        return fail(why, "record.input_layout: the input-layout blob does not decode (its 0x50-byte header, its "
                         "length-prefixed printable names, its 24-byte attribute blocks and its zero padding do not "
                         "fill the measured 80..340 bytes; docs/input_layout.md)");
    if (!elements.empty())
        return fail(why, "record.input_layout: the measured mesh records carry the 80-byte empty layout, with no "
                         "elements (docs/shader_library.md section 2); this one has " +
                             std::to_string(elements.size()) +
                             " element(s), a shape no mesh record was measured with, and D3D12 ignores a mesh PSO's "
                             "input layout, so it is refused rather than silently dropped");

    // ---- The caller's inputs, by name. The mesh stage is required; the pixel stage is optional; a vertex stage is
    // forbidden beside MS (D3D12: a mesh PSO has no vertex stage).
    if (!inputs.root_signature)
        return fail(why, "pRootSignature: the caller supplied no PC root signature for the record's root signature");
    const StageBytecode& ms_code = inputs.stages[rt::graphics_record::kStageSlotMs];
    const StageBytecode& ps_code = inputs.stages[rt::graphics_record::kStageSlotPs];
    const StageBytecode& vs_code = inputs.stages[rt::graphics_record::kStageSlotVs];
    if (ms_code.data == nullptr || ms_code.size == 0)
        return fail(why, stage_input(rt::graphics_record::kStageSlotMs) +
                             ": the record names a mesh stage but the caller supplied no resolved program bytes for it");
    if (pixel && (ps_code.data == nullptr || ps_code.size == 0))
        return fail(why, stage_input(rt::graphics_record::kStageSlotPs) +
                             ": the record names a pixel stage but the caller supplied no resolved program bytes for it");
    if (!pixel && (ps_code.data || ps_code.size))
        return fail(why, stage_input(rt::graphics_record::kStageSlotPs) + ": the record names no pixel stage, so "
                                                                          "these program bytes have no stage to go in");
    if (vs_code.data || vs_code.size)
        return fail(why, stage_input(rt::graphics_record::kStageSlotVs) +
                             ": a mesh pipeline has no vertex stage (D3D12 forbids VS beside MS), so these program "
                             "bytes have no stage to go in");
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
    // The fixed-function fields a mesh pipeline carries are the same API values build() checks; the input-assembly
    // fields (topology type, index-buffer strip cut) are not part of a mesh pipeline, so they are not checked or
    // written here (D3D12 ignores them for a PSO with an MS stage).
    const FixedFunctionState& ff = inputs.defaults;
    if (ff.sample_count == 0 || ff.sample_count > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT ||
        (ff.sample_count & (ff.sample_count - 1u)) != 0)
        return fail(why, "defaults.sample_count: " + std::to_string(ff.sample_count) +
                             " is not a power of two from 1 to " +
                             std::to_string(D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT));
    if (ff.sample_quality != 0 && ff.sample_quality != DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN &&
        ff.sample_quality != DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN)
        return fail(why, "defaults.sample_quality: " + std::to_string(ff.sample_quality) +
                             " is not 0, the standard or the centre multisample pattern");
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

    // ---- The stream: every subobject is set here; nothing is left to the runtime's default.
    SubobjectWriter writer;
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, inputs.root_signature);
    const D3D12_SHADER_BYTECODE ms{ms_code.data, ms_code.size};
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, ms);
    D3D12_SHADER_BYTECODE ps{};
    if (pixel) {
        ps = {ps_code.data, ps_code.size};
        writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, ps);
    }
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.NumRenderTargets = inputs.render_target_count;
    for (UINT target = 0; target < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++target)
        formats.RTFormats[target] = inputs.rtv_formats[target];
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, inputs.dsv_format);
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, ff.rasterizer);
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, ff.blend);
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, ff.depth_stencil);
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, ff.sample_mask);
    const DXGI_SAMPLE_DESC sample{ff.sample_count, ff.sample_quality};
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, sample);
    const UINT node_mask = 0;  // the console record carries none; the neutral value, as build() uses
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, node_mask);
    const D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    writer.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, flags);

    MeshPipelineStream stream;
    stream.subobject_count = writer.count();
    stream.bytes = writer.take();
    stream.ms_bytes = static_cast<std::uint32_t>(ms_code.size);
    stream.ps_bytes = pixel ? static_cast<std::uint32_t>(ps_code.size) : 0;
    *out = std::move(stream);
    return true;
}

}  // namespace rt::graphics_pso
