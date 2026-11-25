// Synthetic records and a synthetic shader index for port/src/graphics_pso.*: every byte below is built here. The
// slot-57 record uses the decoder's measured layout (docs/graphics_record.md), the native programs are the 36-byte FSPX
// prologue plus filler words this test chose, and the shader index is written into a scratch folder as
// fspx/index.json plus one module per entry. Nothing in this file is game data, and no device, GPU or game file is
// involved.
//
// Covered:
//   - resolve_native_stage(): a bare kind-6 program found by its SHA-256 key; a prologue-less program found by the same
//     raw key; an FSPX payload matched on its kind-0 state; a program with no translation; an ambiguous one (refused,
//     not guessed); a program published for another stage (named); and false for bytes that are not a native program
//     (an FSSM blob, arbitrary bytes, an unknown program's bytes).
//   - pick_entry(): the binding-free preference and the fits fallback.
//   - build(): every D3D12_GRAPHICS_PIPELINE_STATE_DESC field from the caller's inputs and the record's proved pieces;
//     an empty input layout (the mesh shape) is accepted; the measured input-layout element table is decoded into
//     D3D12_INPUT_ELEMENT_DESC entries (formats, semantic index, packed offsets, per-vertex data) and every unmeasured
//     format word, input slot, class, step rate, offset or duplicate semantic/index is refused with its field and value
//     named; a missing state blob, a missing root signature (the record's and the caller's), a stage without bytes,
//     bytes for a stage the record does not name, bad attachment lists, a zeroed fixed-function state and a mesh stage
//     are refused, each with its field named; the fixed-function state is used verbatim (a second, different state
//     changes the description field for field), so there is no silent default.
//   - console_graphics_user_data(): the console user-SGPR placement of a root signature (docs/root_arguments.md):
//     vertex/pixel base s8 with one dword per descriptor table and two per root view, mesh base s16, and a
//     signature that would spill refused with its parameter named.
//   - decode_fixed_function_state(): the decoded state record's container passes, but all eight fixed-function fields
//     refuse by name with the histogram key `state.fixed_function` and the output untouched; an undecoded container
//     refuses as `state` (docs/state_blob.md).
//   - run_probe(): the behavioural probe behind that refusal. Every fixed-function field a defined rendering result
//     depends on has a scene whose observation changes with the field (cull/front face, depth enable/func/write, blend
//     enable/factors/op/write mask, topology type, strip cut, sample count, sample mask); sample quality has none, and
//     the test says so instead of inventing a scene. The probe is the falsifier a decode would have to survive; the
//     record supplies no observation, so the decode still refuses.
// It prints the input and refusal tables so the boundary is visible without reading the header.

#include "../src/graphics_pso.h"
#include "test_temp_dir.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace pso = rt::graphics_pso;
using rt::graphics_record::kStageSlotMs;
using rt::graphics_record::kStageSlotPs;
using rt::graphics_record::kStageSlotVs;
using rt::graphics_record::Record;
using rt::graphics_record::Span;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

using Bytes = std::vector<std::uint8_t>;

void put_u32(Bytes* bytes, std::size_t at, std::uint32_t value) {
    std::memcpy(bytes->data() + at, &value, sizeof value);
}

void put_u64(Bytes* bytes, std::size_t at, std::uint64_t value) {
    std::memcpy(bytes->data() + at, &value, sizeof value);
}

void append_u32(Bytes* bytes, std::uint32_t value) {
    const std::size_t at = bytes->size();
    bytes->resize(at + sizeof value);
    put_u32(bytes, at, value);
}

std::uint64_t pointer_word(const Bytes& blob) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(blob.data()));
}

// A pointer/size pair as the record stores it: pointer at `at`, size at `at + 8`.
void put_span_pair(Bytes* bytes, std::size_t at, const Bytes& blob) {
    put_u64(bytes, at, pointer_word(blob));
    put_u64(bytes, at + 8, blob.size());
}

// A state blob in the measured container shape (docs/state_blob.md): 388 B is the vertex family at k = 1 (the header
// words, the +0x28 variable-region pair and a later raw word). Its container decodes; its fixed-function fields do
// not, which is the point of the state tests. Not game data: the container words only.
Bytes make_state_blob() {
    Bytes blob(388, 0);
    put_u32(&blob, 0x00, rt::graphics_record::kStateHeaderWord00Base | 0x08u);
    put_u32(&blob, 0x04, rt::graphics_record::kStateStageWordVertex);
    put_u32(&blob, 0x08, rt::graphics_record::kStateHeaderWord08);
    put_u32(&blob, 0x0c, 0x00020042u);
    put_u32(&blob, 0x10, rt::graphics_record::kStateHeaderWord10);
    put_u32(&blob, 0x150, 0x00000040u);
    const std::size_t variable_at =
        rt::graphics_record::kStateHeaderBytes + rt::graphics_record::kStateVertexStageBlockBytes +
        rt::graphics_record::kStateCommonBlockBytes;
    put_u32(&blob, rt::graphics_record::kStateVariablePairOffset,
            ((388 - variable_at - rt::graphics_record::kStateVariableHeaderBytes) << 16) |
                static_cast<std::uint32_t>(variable_at));
    return blob;
}

// A state blob in the measured mesh-family container shape (docs/state_blob.md): 464 B, k = 0. A record that names the
// mesh stage has to carry this family (the state size names the record's programmable stage).
Bytes make_mesh_state_blob() {
    Bytes blob(464, 0);
    put_u32(&blob, 0x00, rt::graphics_record::kStateHeaderWord00Base);
    put_u32(&blob, 0x04, rt::graphics_record::kStateStageWordMesh);
    put_u32(&blob, 0x08, rt::graphics_record::kStateHeaderWord08);
    put_u32(&blob, 0x0c, 0x00000000u);
    put_u32(&blob, 0x10, rt::graphics_record::kStateHeaderWord10);
    const std::size_t variable_at =
        rt::graphics_record::kStateHeaderBytes + rt::graphics_record::kStateMeshStageBlockBytes +
        rt::graphics_record::kStateCommonBlockBytes;
    put_u32(&blob, rt::graphics_record::kStateVariablePairOffset,
            ((464 - variable_at - rt::graphics_record::kStateVariableHeaderBytes) << 16) |
                static_cast<std::uint32_t>(variable_at));
    return blob;
}

// An 80-byte input layout with no elements: the measured mesh-pipeline shape (docs/shader_library.md section 2), and
// the only input layout build() can turn into a D3D12_INPUT_LAYOUT_DESC today.
Bytes make_empty_layout() { return Bytes(80, 0); }

// One element of the measured input-layout table: its printable name and its six attribute words (docs/input_layout.md).
// Attribute word 1 is the format and word 3 the aligned byte offset in the measured samples; every other word is 0.
struct LayoutElement {
    const char* name;
    std::array<std::uint32_t, rt::graphics_record::kInputLayoutAttributeWords> words;
};

std::array<std::uint32_t, rt::graphics_record::kInputLayoutAttributeWords> layout_words(std::uint32_t format,
                                                                                        std::uint32_t offset,
                                                                                        std::uint32_t semantic_index = 0) {
    std::array<std::uint32_t, rt::graphics_record::kInputLayoutAttributeWords> words{};
    words[0] = semantic_index;
    words[1] = format;
    words[3] = offset;
    return words;
}

// A blob in the measured shape: element count at +0x4C, then per element a u32 name length including the NUL, the name
// bytes and a 24-byte attribute block, then zero padding to a 4-byte boundary. Sizes are 80 (count 0), 120/1, 152/2,
// 156/2, 188/3 and 224/4.
Bytes make_input_layout(std::initializer_list<LayoutElement> elements) {
    std::size_t size = rt::graphics_record::kInputLayoutHeaderBytes;
    for (const LayoutElement& element : elements)
        size += rt::graphics_record::kInputLayoutNameLengthBytes + std::strlen(element.name) + 1 +
                rt::graphics_record::kInputLayoutAttributeBytes;
    size = (size + 3) & ~std::size_t{3};
    Bytes blob(size, 0);
    put_u32(&blob, rt::graphics_record::kInputLayoutCountOffset, static_cast<std::uint32_t>(elements.size()));
    std::size_t at = rt::graphics_record::kInputLayoutHeaderBytes;
    for (const LayoutElement& element : elements) {
        const std::uint32_t name_bytes = static_cast<std::uint32_t>(std::strlen(element.name)) + 1;
        put_u32(&blob, at, name_bytes);
        at += rt::graphics_record::kInputLayoutNameLengthBytes;
        std::memcpy(blob.data() + at, element.name, name_bytes);
        at += name_bytes;
        for (std::size_t word = 0; word < element.words.size(); ++word) put_u32(&blob, at + 4 * word, element.words[word]);
        at += rt::graphics_record::kInputLayoutAttributeBytes;
    }
    return blob;
}

// The measured shapes, with the measured format words and the packed offsets they imply:
Bytes make_layout_120() { return make_input_layout({{"POSITION", layout_words(6, 0)}}); }
Bytes make_layout_152() {
    return make_input_layout({{"POSITION", layout_words(6, 0)}, {"COLOR", layout_words(28, 12)}});
}
Bytes make_layout_156() {
    return make_input_layout({{"TEXCOORD", layout_words(16, 0)}, {"POSITION", layout_words(16, 8)}});
}
Bytes make_layout_188() {
    return make_input_layout(
        {{"POSITION", layout_words(6, 0)}, {"COLOR", layout_words(28, 12)}, {"TEXCOORD", layout_words(16, 16)}});
}
// The 224-byte shape: COLOR0 U8x4 at 0, TEXCOORD float2 at 4, POSITION float2 at 12, COLOR2 U8x4 at 20. The last
// element is the measured one that pins the attribute field order: its word 0 is 2 (the COLOR2 semantic index), its
// word 1 is 30 (DXGI_FORMAT_R8G8B8A8_UINT) and its word 3 is 20 (the packed offset).
Bytes make_layout_224() {
    return make_input_layout({{"COLOR", layout_words(28, 0)},
                              {"TEXCOORD", layout_words(16, 4)},
                              {"POSITION", layout_words(16, 12)},
                              {"COLOR", layout_words(30, 20, 2)}});
}

// A kind-2 stage blob: head (kind 2, size word, unknown word, 0x40 tag) and 32-byte records whose tags end with 0x43.
// build() never reads it; decode() only needs it to be the measured shape.
Bytes make_stage_blob(const std::vector<std::uint32_t>& tags) {
    Bytes blob(0x10 + 32 * tags.size(), 0);
    put_u32(&blob, 0x00, 2);
    put_u32(&blob, 0x04, static_cast<std::uint32_t>(blob.size() - 12));
    put_u32(&blob, 0x08, 0x0000abcdu);
    put_u32(&blob, 0x0c, 0x40);
    for (std::size_t index = 0; index < tags.size(); ++index) {
        const std::size_t at = 0x10 + 32 * index;
        put_u32(&blob, at, tags[index]);
        if (tags[index] == 0x43) {
            for (std::size_t byte = 0; byte < 16; ++byte)
                blob[at + 4 + byte] = static_cast<std::uint8_t>((0xa0 + byte) & 0xff);
        } else {
            for (std::size_t word = 0; word < 7; ++word)
                put_u32(&blob, at + 4 + 4 * word, 0x11000000u | static_cast<std::uint32_t>(word));
        }
    }
    return blob;
}

// A synthetic native program (docs/fspx.md, "Kind 6"): the 36-byte prologue of three VOP3 words with opcode 0x180
// (bits 31..16 = 0xD580) and their literals, then a filler word and s_endpgm, padded to the 52-byte minimum the record's
// measured program range allows.
Bytes fspx_code(std::uint32_t seed) {
    Bytes code;
    for (int word = 0; word < 3; ++word) {
        append_u32(&code, 0xd5800000u | static_cast<std::uint32_t>(word));
        append_u32(&code, 0x000000ffu);
        append_u32(&code, seed + static_cast<std::uint32_t>(word));
    }
    append_u32(&code, 0x7e000280u);  // v_mov_b32 v0, 0
    append_u32(&code, 0xbf810000u);  // s_endpgm
    append_u32(&code, 0);            // padding to 52 bytes
    append_u32(&code, 0);
    return code;
}

// The same bytes with word 3 no longer carrying the prologue: compute_program() classifies it as no program, so only
// the SHA-256 of the bytes themselves can name it.
Bytes without_prologue(Bytes code) {
    code[14] ^= 0x01;
    return code;
}

// An FSPX payload with the usual four kinds, laid out after a four-entry directory (body at 80): kind 0 is 28 bytes of
// 0x11, kind 1 is 20 bytes of 0x22, kind 6 is `code`, kind 7 is 8 bytes of 0x77 (docs/fspx.md).
Bytes fspx_payload(const Bytes& code) {
    Bytes directory;
    const std::uint32_t size = static_cast<std::uint32_t>(16 + 16 * 4 + 28 + 20 + code.size() + 8);
    append_u32(&directory, 1);  // version
    append_u32(&directory, 4);  // entries
    append_u32(&directory, size);
    const std::uint32_t n = static_cast<std::uint32_t>(code.size());
    for (const std::array<std::uint32_t, 4>& entry :
         {std::array<std::uint32_t, 4>{0, 80, 28, 0}, std::array<std::uint32_t, 4>{1, 108, 20, 0},
          std::array<std::uint32_t, 4>{6, 128, n, 0}, std::array<std::uint32_t, 4>{7, 128 + n, 8, 0}}) {
        for (const std::uint32_t word : entry) append_u32(&directory, word);
    }
    Bytes payload = {'F', 'S', 'P', 'X'};
    payload.insert(payload.end(), directory.begin(), directory.end());
    payload.insert(payload.end(), 28, 0x11);
    payload.insert(payload.end(), 20, 0x22);
    payload.insert(payload.end(), code.begin(), code.end());
    payload.insert(payload.end(), 8, 0x77);
    return payload;
}

// The slot-57 record: 0x108 validated bytes (the same shape port/test/graphics_record_test.cpp builds) plus an
// unidentified tail decode() must ignore. `slots` names the stage slots used, in record order.
Bytes make_descriptor(const Bytes& state, const Bytes& layout, const std::vector<int>& slots, const Bytes& ps_program,
                      const Bytes& ps_blob, const Bytes& vs_program, const Bytes& vs_blob, const Bytes& ms_program = {},
                      const Bytes& ms_blob = {}) {
    Bytes descriptor(0x120, 0);
    put_u64(&descriptor, 0x00, 0x1122334455667788ull);  // the root signature pointer value; never dereferenced
    put_span_pair(&descriptor, 0x08, state);
    put_span_pair(&descriptor, 0x18, layout);
    for (const int slot : slots) {
        const std::size_t at = 0x28 + 0x20 * static_cast<std::size_t>(slot);
        const Bytes* program = slot == kStageSlotPs ? &ps_program : slot == kStageSlotVs ? &vs_program : &ms_program;
        const Bytes* blob = slot == kStageSlotPs ? &ps_blob : slot == kStageSlotVs ? &vs_blob : &ms_blob;
        put_span_pair(&descriptor, at, *program);
        put_span_pair(&descriptor, at + 0x10, *blob);
    }
    for (std::size_t at = 0x108; at < descriptor.size(); ++at) descriptor[at] = 0xee;  // the unknown tail
    return descriptor;
}

// A decoded record plus the bytes its spans point at. The record carries pointers into these vectors, so the fixture
// has to outlive every build() call made with it (the record is never decoded into dangling storage).
struct RecordFixture {
    Bytes state = make_state_blob();
    Bytes layout = make_empty_layout();
    Bytes ps_program = fspx_code(0x101);
    Bytes vs_program = fspx_code(0x202);
    Bytes ms_program = fspx_code(0x303);
    Bytes ps_blob = make_stage_blob({0x41, 0x43});
    Bytes vs_blob = make_stage_blob({0x41, 0x43});
    Bytes ms_blob = make_stage_blob({0x42, 0x43});
    Bytes descriptor;
    Record record;
};

// A decoded record from the named slots (PS+VS when omitted) with the given input layout (the 80-byte empty layout when
// omitted).
RecordFixture make_record(const std::vector<int>& slots = {kStageSlotPs, kStageSlotVs}, const Bytes& layout = Bytes{}) {
    RecordFixture fixture;
    // A record that names the mesh stage carries the mesh-family state record (docs/state_blob.md): decode() checks
    // that the state size family names the record's programmable stage.
    for (const int slot : slots)
        if (slot == kStageSlotMs) fixture.state = make_mesh_state_blob();
    if (!layout.empty()) fixture.layout = layout;
    fixture.descriptor = make_descriptor(fixture.state, fixture.layout, slots, fixture.ps_program, fixture.ps_blob,
                                         fixture.vs_program, fixture.vs_blob, fixture.ms_program, fixture.ms_blob);
    if (!rt::graphics_record::decode(fixture.descriptor.data(), fixture.descriptor.size(), &fixture.record))
        fixture.record.stage_count = 0;
    return fixture;
}

// ---- The synthetic shader index -----------------------------------------------------------------------------------

struct LibraryEntry {
    std::string name;
    std::string stage;
    std::string code_hash;
    std::string state_hash;  // empty: no kind-0 key
    std::uint32_t module = 1;  // the module's last word, so two entries can differ in their SPIR-V
};

// <dir>/index.json (empty: every program here is an FSPX one) and <dir>/fspx/index.json with one entry per program,
// plus <dir>/fspx/<name>.spv for each (docs/fspx.md: FSPX entries are found by fspx_code_sha256 in that subfolder and
// their modules live beside it). No game data: the hashes are of the synthetic programs above.
bool write_library(const std::filesystem::path& dir, const std::vector<LibraryEntry>& entries) {
    std::error_code ec;
    std::filesystem::create_directories(dir / "fspx", ec);
    if (ec) return false;
    {
        std::ofstream index(dir / "index.json", std::ios::binary);
        index << "{}";
        if (!index) return false;
    }
    {
        std::ofstream index(dir / "fspx" / "index.json", std::ios::binary);
        index << "{";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const LibraryEntry& entry = entries[i];
            if (i) index << ",";
            index << "\"" << entry.name << "\": {\"stage\": \"" << entry.stage << "\", \"fspx_code_sha256\": \""
                  << entry.code_hash << "\"";
            if (!entry.state_hash.empty()) index << ", \"fspx_state_sha256\": \"" << entry.state_hash << "\"";
            index << "}";
        }
        index << "}";
        if (!index) return false;
    }
    for (const LibraryEntry& entry : entries) {
        std::ofstream module(dir / "fspx" / (entry.name + ".spv"), std::ios::binary);
        const std::uint32_t words[5] = {0x07230203, 0x00010500, 0, 1, entry.module};
        module.write(reinterpret_cast<const char*>(words), sizeof words);
        if (!module) return false;
    }
    return true;
}

bool load_library(testfiles::ScratchDir& scratch, const std::vector<LibraryEntry>& entries,
                  palmetto::ShaderLibrary* library) {
    if (!write_library(scratch.path(), entries)) return false;
    std::string error;
    if (!library->load(scratch.path().string(), &error)) {
        std::fprintf(stderr, "graphics_pso_test: the synthetic shader index did not load: %s\n", error.c_str());
        return false;
    }
    return true;
}

const auto kFitsAll = [](const palmetto::ShaderEntry&) { return true; };

std::string hash_of(const Bytes& bytes) { return palmetto::sha256_hex(bytes); }

std::string state_hash_of_0x11() { return hash_of(Bytes(28, 0x11)); }

// ---- Tests -------------------------------------------------------------------------------------------------------

void test_resolve_native_stage() {
    testfiles::ScratchDir scratch("palmetto_graphics_pso_resolve");
    palmetto::ShaderLibrary library;

    const Bytes ps_code = fspx_code(0x101);
    const Bytes vs_code = fspx_code(0x202);
    const Bytes untranslated = fspx_code(0x909);
    const Bytes raw_only = without_prologue(fspx_code(0x404));
    const Bytes payload_code = fspx_code(0x505);
    const Bytes ambiguous_code = fspx_code(0x606);
    const Bytes duplicated_code = fspx_code(0x707);
    const bool loaded = load_library(scratch,
                                     {
                                         {"lib_ps_00001", "ps", hash_of(ps_code)},
                                         {"lib_vs_00002", "vs", hash_of(vs_code)},
                                         {"lib_ps_00003", "ps", hash_of(raw_only)},
                                         {"lib_ps_00004", "ps", hash_of(payload_code), state_hash_of_0x11()},
                                         {"lib_ps_00005", "ps", hash_of(ambiguous_code), "", 11},
                                         {"lib_ps_00006", "ps", hash_of(ambiguous_code), "", 22},
                                         {"lib_ps_00007", "ps", hash_of(duplicated_code), "", 33},
                                         {"lib_ps_00008", "ps", hash_of(duplicated_code), "", 33},
                                     },
                                     &library);
    check(loaded, "resolve: the synthetic shader index loads");
    if (!loaded) return;

    // Bytes that are not a native program are not this path's: the FSSM/DXBC and stand-in paths, and their refusal,
    // stay in charge.
    const Bytes fssm = {'F', 'S', 'S', 'M', ' ', 'b', 'l', 'o', 'b'};
    pso::NativeStage native;
    std::string why;
    check(!pso::resolve_native_stage(fssm, "ps", library, kFitsAll, &native, &why),
          "resolve: an FSSM blob is not a native stage");
    check(!pso::resolve_native_stage(Bytes(64, 0xaa), "ps", library, kFitsAll, &native, &why),
          "resolve: arbitrary bytes with no index entry are not a native stage");
    check(pso::resolve_native_stage(untranslated, "ps", library, kFitsAll, &native, &why),
          "resolve: a classified program with no translation is still a native stage (reported, not discarded)");

    // A bare kind-6 program found by its SHA-256 key, the stage named by the caller.
    why.clear();
    check(pso::resolve_native_stage(ps_code, "ps", library, kFitsAll, &native, &why),
          "resolve: a bare kind-6 program is a native stage");
    check(native.entry && native.entry->name == "fspx/lib_ps_00001", "resolve: the ps entry is found");
    check(native.program.bytes == ps_code, "resolve: the kind-6 bytes are the program, as create_compute takes them");
    check(!native.raw_key, "resolve: a classified program is not the raw-key path");
    check(why.empty(), "resolve: no refusal message when the entry is found");

    // The record slot gives the stage: the same bytes looked up as the vertex stage are not there.
    why.clear();
    check(pso::resolve_native_stage(ps_code, "vs", library, kFitsAll, &native, &why),
          "resolve: the same program under another stage is still a native stage");
    check(!native.entry, "resolve: it has no entry under the wrong stage");
    check(why.find("stage mismatch") != std::string::npos && why.find(" published for stage ps") != std::string::npos,
          "resolve: the refusal names the stage the program is published for");

    // No translation: classified, but the index holds nothing under its key.
    why.clear();
    check(pso::resolve_native_stage(untranslated, "ps", library, kFitsAll, &native, &why),
          "resolve: an untranslated native program is classified");
    check(!native.entry && why.find("no translation") != std::string::npos,
          "resolve: an untranslated native program is refused with 'no translation'");

    // A program without the FSPX prologue: compute_program cannot classify it, but the publication's key is the SHA-256
    // of the bytes as passed, so it resolves by that key.
    const palmetto::ComputeProgram unclassified = palmetto::compute_program(raw_only);
    check(unclassified.kind == palmetto::ComputeProgram::Kind::none,
          "resolve: the prologue-less program is not FSPX-classifiable");
    why.clear();
    check(pso::resolve_native_stage(raw_only, "ps", library, kFitsAll, &native, &why),
          "resolve: a prologue-less program with an index entry is a native stage");
    check(native.entry && native.entry->name == "fspx/lib_ps_00003", "resolve: the raw-key entry is found");
    check(native.raw_key, "resolve: the raw-key path is recorded");
    check(native.program.bytes == raw_only, "resolve: the raw-key program bytes are the bytecode itself");

    // An FSPX payload: the kind-0 state picks among translations of the same kind-6 bytes.
    const Bytes payload = fspx_payload(payload_code);
    why.clear();
    check(pso::resolve_native_stage(payload, "ps", library, kFitsAll, &native, &why),
          "resolve: an FSPX payload is a native stage");
    check(native.entry && native.entry->name == "fspx/lib_ps_00004", "resolve: the payload resolves by its kind-6 key");
    check(native.program.bytes == payload_code && native.program.state == Bytes(28, 0x11),
          "resolve: the payload's kind-6 bytes and kind-0 state are kept");
    Bytes wrong_state = payload;
    wrong_state[80] = 0x12;  // the first kind-0 byte
    why.clear();
    check(pso::resolve_native_stage(wrong_state, "ps", library, kFitsAll, &native, &why),
          "resolve: a payload with a different kind-0 state is still classified");
    check(!native.entry && why.find("kind-0 state") != std::string::npos,
          "resolve: the refusal names the kind-0 lookup");

    // Two translations of the same kind-6 bytes with different modules and no kind-0 state: ambiguous, so refused
    // rather than picking one.
    why.clear();
    check(pso::resolve_native_stage(ambiguous_code, "ps", library, kFitsAll, &native, &why),
          "resolve: an ambiguous program is classified");
    check(!native.entry, "resolve: an ambiguous program has no entry");
    check(why.find("ambiguous") != std::string::npos,
          "resolve: an ambiguous program is refused with 'ambiguous', not guessed");

    // Two identical translations (same module, same metadata) are not ambiguous: the first is built.
    why.clear();
    check(pso::resolve_native_stage(duplicated_code, "ps", library, kFitsAll, &native, &why),
          "resolve: two identical translations are a native stage");
    check(native.entry && native.entry->name == "fspx/lib_ps_00007",
          "resolve: identical translations resolve to the first entry");

    // A vertex stage's entry must be one whose inputs this PSO's layout feeds: `fits` is the caller's test.
    why.clear();
    check(pso::resolve_native_stage(vs_code, "vs", library, [](const palmetto::ShaderEntry&) { return false; }, &native,
                                    &why),
          "resolve: a stage whose entries do not fit is still classified");
    check(native.entry && native.entry->name == "fspx/lib_vs_00002",
          "resolve: with no fitting entry the first one is still chosen (as pick_entry has always done)");

    check(!pso::resolve_native_stage(ps_code, "ps", library, kFitsAll, nullptr, &why),
          "resolve: a null output is refused rather than written");
    check(!pso::resolve_native_stage(std::span<const std::uint8_t>{}, "ps", library, kFitsAll, &native, &why),
          "resolve: empty bytecode is not a native stage");
}

void test_pick_entry() {
    palmetto::ShaderEntry unbound;
    palmetto::ShaderEntry bound;
    bound.binding_count = 2;
    palmetto::ShaderEntry bound2;
    bound2.binding_count = 3;
    const std::vector<const palmetto::ShaderEntry*> both{&bound, &unbound};
    check(pso::pick_entry(both, kFitsAll) == &unbound,
          "pick_entry: an entry with no set-0 bindings wins over an earlier one with bindings");
    const std::vector<const palmetto::ShaderEntry*> fits_first{&bound, &bound2};
    check(pso::pick_entry(fits_first, [&bound2](const palmetto::ShaderEntry& e) { return &e == &bound2; }) == &bound2,
          "pick_entry: without an unbound entry, the first that fits wins");
    check(pso::pick_entry(fits_first, [&bound](const palmetto::ShaderEntry& e) { return &e == &bound; }) == &bound,
          "pick_entry: the first fit wins when several do (only one fits here)");
    const std::vector<const palmetto::ShaderEntry*> none{};
    check(pso::pick_entry(none, kFitsAll) == nullptr, "pick_entry: no candidates gives no entry");
}

// Every field of the documented default set, spelled out so docs/graphics_pso.md and the code cannot drift apart.
void test_documented_default_state() {
    const pso::FixedFunctionState state = pso::documented_default_state();
    check(state.rasterizer.FillMode == D3D12_FILL_MODE_SOLID, "defaults: rasterizer fill is solid");
    check(state.rasterizer.CullMode == D3D12_CULL_MODE_BACK, "defaults: back faces are culled");
    check(state.rasterizer.FrontCounterClockwise == FALSE, "defaults: clockwise is front");
    check(state.rasterizer.DepthBias == 0 && state.rasterizer.DepthBiasClamp == 0.0f &&
              state.rasterizer.SlopeScaledDepthBias == 0.0f,
          "defaults: no depth bias");
    check(state.rasterizer.DepthClipEnable == TRUE, "defaults: depth clipping is on");
    check(state.rasterizer.MultisampleEnable == FALSE && state.rasterizer.AntialiasedLineEnable == FALSE,
          "defaults: multisample rasterization and antialiased lines are off");
    check(state.rasterizer.ForcedSampleCount == 0 &&
              state.rasterizer.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
          "defaults: no forced samples and no conservative rasterization");
    check(state.blend.AlphaToCoverageEnable == FALSE && state.blend.IndependentBlendEnable == FALSE,
          "defaults: blending off, one blend state for every target");
    bool blend_off = true, masks_all = true;
    for (const D3D12_RENDER_TARGET_BLEND_DESC& target : state.blend.RenderTarget) {
        blend_off = blend_off && target.BlendEnable == FALSE && target.LogicOpEnable == FALSE;
        masks_all = masks_all && target.RenderTargetWriteMask == D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    check(blend_off, "defaults: no target enables blending or a logic op");
    check(masks_all, "defaults: every target writes every colour channel");
    check(state.depth_stencil.DepthEnable == FALSE && state.depth_stencil.StencilEnable == FALSE,
          "defaults: depth and stencil are off");
    check(state.depth_stencil.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL &&
              state.depth_stencil.DepthFunc == D3D12_COMPARISON_FUNC_LESS,
          "defaults: the depth fields are still valid API values");
    check(state.depth_stencil.StencilReadMask == 0xff && state.depth_stencil.StencilWriteMask == 0xff,
          "defaults: the stencil masks are the API's full masks");
    const auto neutral_face = [](const D3D12_DEPTH_STENCILOP_DESC& face) {
        return face.StencilFailOp == D3D12_STENCIL_OP_KEEP && face.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP &&
               face.StencilPassOp == D3D12_STENCIL_OP_KEEP && face.StencilFunc == D3D12_COMPARISON_FUNC_ALWAYS;
    };
    check(neutral_face(state.depth_stencil.FrontFace) && neutral_face(state.depth_stencil.BackFace),
          "defaults: the stencil ops are the neutral ones");
    check(state.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, "defaults: triangles");
    check(state.strip_cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED, "defaults: no strip cut");
    check(state.sample_count == 1 && state.sample_quality == 0 && state.sample_mask == 0xffffffffu,
          "defaults: one sample, full mask");
}

// The state record's container decodes but its fixed-function fields do not: decode_fixed_function_state() must
// refuse all eight by name, key the refusal state.fixed_function, report every field false, and leave the output
// untouched; an undecoded container is refused as `state`. This is the measurement behind the documented stand-in
// (docs/state_blob.md): the container is decoded, no field is pinned.
void test_fixed_function_state_decode() {
    const Bytes blob = make_state_blob();
    rt::graphics_record::State state;
    check(rt::graphics_record::decode_state(Span{blob.data(), blob.size()}, &state),
          "state: the fixture's measured container decodes");
    check(state.family == rt::graphics_record::StateFamily::kVertex && state.variable_item_count == 1,
          "state: the fixture is the vertex family at k = 1");

    pso::FixedFunctionState out{};
    out.sample_count = 77;  // a sentinel: a refusal must not write the output
    pso::FixedFunctionFieldStatus fields;
    fields.rasterizer = true;  // a sentinel: the refusal resets every field status
    std::string why;
    check(!pso::decode_fixed_function_state(state, &out, &fields, &why), "state fields: all eight fields refuse");
    check(out.sample_count == 77, "state fields: the output is untouched on refusal");
    const bool none = !fields.rasterizer && !fields.blend && !fields.depth_stencil && !fields.topology_type &&
                      !fields.strip_cut && !fields.sample_count && !fields.sample_quality && !fields.sample_mask;
    check(none, "state fields: every field status is false");
    check(why.rfind("state.fixed_function:", 0) == 0, "state fields: the refusal is keyed state.fixed_function");
    bool named = true;
    for (const char* name : pso::kFixedFunctionFieldNames) named = named && why.find(name) != std::string::npos;
    check(named, "state fields: the refusal names all eight refused fields");
    check(why.find("documented stand-in") != std::string::npos,
          "state fields: the refusal says the documented stand-in stays");
    check(why.find("388-byte") != std::string::npos, "state fields: the refusal names the decoded container's size");

    // A container the decoder refused (State::size 0) is refused as `state`, not as a fixed-function field.
    const rt::graphics_record::State undecoded;
    why.clear();
    check(!pso::decode_fixed_function_state(undecoded, &out, &fields, &why),
          "state fields: an undecoded container is refused");
    check(why.rfind("state:", 0) == 0 && why.find("state.fixed_function") == std::string::npos,
          "state fields: an undecoded container is keyed state, not state.fixed_function");
}

pso::GraphicsPsoInputs base_inputs(const void* ps, SIZE_T ps_size, const void* vs, SIZE_T vs_size) {
    pso::GraphicsPsoInputs inputs{};
    inputs.root_signature = reinterpret_cast<ID3D12RootSignature*>(static_cast<std::uintptr_t>(0x1000));
    inputs.rtv_formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    inputs.render_target_count = 1;
    inputs.dsv_format = DXGI_FORMAT_UNKNOWN;
    inputs.defaults = pso::documented_default_state();
    inputs.stages[kStageSlotPs] = {ps, ps_size};
    inputs.stages[kStageSlotVs] = {vs, vs_size};
    return inputs;
}

// The behavioural probe: every field that can be observed has a scene whose observation changes with it, and the one
// field that cannot be observed is named as such instead of being given a made-up probe. These checks are the
// falsifier behind decode_fixed_function_state()'s refusal: the state record's words are only a candidate place for a
// field, and a decode is only a pin when the observation the probe models is available from the title's own draw -
// it is not (docs/frame_capture.md sections 6-8), so the refusal below stands while the probe stays usable.
void test_fixed_function_probe() {
    const pso::FixedFunctionState defaults = pso::documented_default_state();
    const auto run = [](const pso::FixedFunctionState& state, pso::ProbeScene scene) {
        return pso::run_probe(state, scene);
    };

    // The contract: eight rows in the field-name order, each saying what it observes, and exactly one field (sample
    // quality) that no rendering result the API defines depends on.
    bool order = true;
    bool named = true;
    int observable = 0;
    for (int field = 0; field < 8; ++field) {
        order = order && pso::kProbeContract[field].field == field;
        named = named && pso::kProbeContract[field].distinguishes != nullptr &&
                pso::kProbeContract[field].distinguishes[0] != '\0';
        if (pso::kProbeContract[field].observable) ++observable;
    }
    check(order, "probe: the contract rows are the eight field names in kFixedFunctionFieldNames order");
    check(named, "probe: every contract row says what it observes");
    check(observable == 7, "probe: seven of the eight fields have an observable scene");
    check(!pso::kProbeContract[6].observable, "probe: sample quality has no observable scene");

    // Determinism: the same state and scene give the same outcome.
    check(run(defaults, pso::ProbeScene::kTwoDepthQuads) == run(defaults, pso::ProbeScene::kTwoDepthQuads),
          "probe: a probe run is deterministic");

    // ---- Rasterizer: the winding pair separates cull mode and front-face flag.
    pso::FixedFunctionState cull_none = defaults;
    cull_none.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    pso::FixedFunctionState cull_front = defaults;
    cull_front.rasterizer.CullMode = D3D12_CULL_MODE_FRONT;
    pso::FixedFunctionState front_ccw = defaults;
    front_ccw.rasterizer.FrontCounterClockwise = TRUE;
    const pso::ProbeOutcome back_culled = run(defaults, pso::ProbeScene::kWindingPair);
    const pso::ProbeOutcome nothing_culled = run(cull_none, pso::ProbeScene::kWindingPair);
    const pso::ProbeOutcome front_culled = run(cull_front, pso::ProbeScene::kWindingPair);
    const pso::ProbeOutcome flipped_face = run(front_ccw, pso::ProbeScene::kWindingPair);
    check(back_culled.primitives == 2 && back_culled.culled == 1 && back_culled.fragments == 4,
          "probe: back-face culling keeps the clockwise half only (4 of the 8 rasterized sample fragments)");
    check(nothing_culled.culled == 0 && nothing_culled.fragments == 8,
          "probe: culling off rasterizes both halves (8 sample fragments)");
    check(front_culled.fragments == 4 && flipped_face.fragments == 4,
          "probe: culling the front, or flipping the front-face flag, keeps the other half");
    check(!(back_culled == nothing_culled), "probe: cull mode is behaviourally observable");
    check(!(back_culled == front_culled), "probe: a cull-mode change is observable even though the fragment count is equal");
    check(!(back_culled == flipped_face), "probe: the front-face flag is behaviourally observable");
    check(front_culled == flipped_face,
          "probe: culling the front under a clockwise front is the same observation as culling the back counter-clockwise");
    // Depth clipping is the rasterizer's other observable part in the model.
    pso::FixedFunctionState clip_off = defaults;
    clip_off.rasterizer.DepthClipEnable = FALSE;
    const pso::ProbeOutcome clipped = run(defaults, pso::ProbeScene::kDepthClip);
    const pso::ProbeOutcome clamped = run(clip_off, pso::ProbeScene::kDepthClip);
    check(clipped.primitives == 4 && clipped.clipped == 4 && clipped.fragments == 0,
          "probe: depth clipping removes every triangle of a layer outside the depth range");
    check(clamped.clipped == 0 && clamped.fragments == 16, "probe: with clipping off the layer is depth-clamped and drawn");
    check(!(clipped == clamped), "probe: depth clipping is behaviourally observable");
    // A fill mode the model does not express is counted, not silently treated as solid.
    pso::FixedFunctionState wireframe = defaults;
    wireframe.rasterizer.FillMode = D3D12_FILL_MODE_WIREFRAME;
    const pso::ProbeOutcome wired = run(wireframe, pso::ProbeScene::kTopologyList);
    check(wired.unmodelled == run(defaults, pso::ProbeScene::kTopologyList).unmodelled + 1 &&
              wired.colour == run(defaults, pso::ProbeScene::kTopologyList).colour,
          "probe: wireframe fill is counted unmodelled, not mistaken for solid");

    // ---- Blend: the overlap scene separates enable, factors, operation and write mask.
    pso::FixedFunctionState blend_on = defaults;
    blend_on.blend.RenderTarget[0].BlendEnable = TRUE;
    blend_on.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend_on.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend_on.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend_on.blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    const pso::ProbeOutcome blend_off = run(defaults, pso::ProbeScene::kOverlap);
    const pso::ProbeOutcome blended = run(blend_on, pso::ProbeScene::kOverlap);
    check(blend_off.written == 32 && blended.written == 32, "probe: both overlapping quads reach the colour write");
    check(blend_off.colour[0] == 191 && blend_off.colour[1] == 64 && blend_off.colour[2] == 64,
          "probe: blending off leaves the second quad's own colour");
    check(!(blend_off == blended), "probe: blend enable is behaviourally observable");
    pso::FixedFunctionState blend_one = blend_on;
    blend_one.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend_one.blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso::FixedFunctionState blend_subtract = blend_on;
    blend_subtract.blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_SUBTRACT;
    pso::FixedFunctionState blend_no_red = blend_on;
    blend_no_red.blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    check(!(run(blend_one, pso::ProbeScene::kOverlap) == blended), "probe: the blend factors are observable");
    check(!(run(blend_subtract, pso::ProbeScene::kOverlap) == blended), "probe: the blend operation is observable");
    check(!(run(blend_no_red, pso::ProbeScene::kOverlap) == blended), "probe: the render-target write mask is observable");

    // ---- Depth: the two-quad scene separates enable, func and write mask.
    pso::FixedFunctionState depth_on = defaults;
    depth_on.depth_stencil.DepthEnable = TRUE;
    pso::FixedFunctionState depth_no_write = depth_on;
    depth_no_write.depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso::FixedFunctionState depth_greater = depth_on;
    depth_greater.depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    const pso::ProbeOutcome depth_off = run(defaults, pso::ProbeScene::kTwoDepthQuads);
    const pso::ProbeOutcome depth_tested = run(depth_on, pso::ProbeScene::kTwoDepthQuads);
    const pso::ProbeOutcome depth_unwritten = run(depth_no_write, pso::ProbeScene::kTwoDepthQuads);
    const pso::ProbeOutcome depth_greater_out = run(depth_greater, pso::ProbeScene::kTwoDepthQuads);
    check(depth_off.colour[0] == 0 && depth_off.colour[1] == 255,
          "probe: with depth off the far green quad overwrites the near one");
    check(depth_tested.colour[0] == 255 && depth_tested.colour[1] == 0 && depth_tested.depth_failed == 16,
          "probe: with depth on the near red quad writes and the far quad fails (16 rejected fragments)");
    check(depth_unwritten.colour[1] == 255, "probe: with the depth write mask zero the far quad still passes");
    check(depth_greater_out.written == 0, "probe: a greater-than depth test rejects both quads against the cleared depth");
    check(!(depth_off == depth_tested), "probe: depth enable is behaviourally observable");
    check(!(depth_tested == depth_unwritten), "probe: the depth write mask is behaviourally observable");
    check(!(depth_tested == depth_greater_out), "probe: the depth comparison function is behaviourally observable");

    // ---- Topology type: a PSO type outside the scene's list class assembles nothing.
    pso::FixedFunctionState point_type = defaults;
    point_type.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    pso::FixedFunctionState line_type = defaults;
    line_type.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    const pso::ProbeOutcome triangles = run(defaults, pso::ProbeScene::kTopologyList);
    const pso::ProbeOutcome points = run(point_type, pso::ProbeScene::kTopologyList);
    check(triangles.mismatch == 0 && triangles.primitives == 4 && triangles.fragments == 16,
          "probe: a triangle topology type matches the triangle list and draws its four triangles");
    check(points.mismatch == 1 && points.primitives == 0 && points.fragments == 0,
          "probe: a point topology type cannot express a triangle list (the API rejects the pair)");
    check(points == run(line_type, pso::ProbeScene::kTopologyList),
          "probe: two non-matching types are not told apart by one draw - the draw's own list class names its type");

    // ---- Strip cut: the sentinel either restarts the strip or is read as an out-of-range index.
    pso::FixedFunctionState cut_ffff = defaults;
    cut_ffff.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    pso::FixedFunctionState cut_ffffffff = defaults;
    cut_ffffffff.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    const pso::ProbeOutcome strip_disabled = run(defaults, pso::ProbeScene::kStripCut);
    const pso::ProbeOutcome strip_cut = run(cut_ffff, pso::ProbeScene::kStripCut);
    check(strip_disabled.out_of_range == 3 && strip_cut.out_of_range == 0,
          "probe: the 0xFFFF sentinel restarts the strip; a cut value that does not match reads it as a bad index");
    check(strip_disabled.primitives == 1 && strip_cut.primitives == 1,
          "probe: both cut values leave the one lead triangle of the strip (the fault count is the observable)");
    check(!(strip_disabled == strip_cut), "probe: the strip-cut value is behaviourally observable");
    check(strip_disabled == run(cut_ffffffff, pso::ProbeScene::kStripCut),
          "probe: cut disabled and 0xFFFFFFFF are not told apart by a strip with no 0xFFFFFFFF index (a scene limit)");

    // ---- Sample count and sample mask: the partial pixel and the masked samples.
    pso::FixedFunctionState four_samples = defaults;
    four_samples.sample_count = 4;
    const pso::ProbeOutcome one_sample = run(defaults, pso::ProbeScene::kSampleGrid);
    const pso::ProbeOutcome msaa = run(four_samples, pso::ProbeScene::kSampleGrid);
    check(one_sample.fragments == 0, "probe: the partial pixel's centre is outside the triangle at one sample");
    check(msaa.fragments == 1 && msaa.coverage[5] == 1,
          "probe: at four samples the same triangle covers the pixel's first sample");
    check(!(one_sample == msaa), "probe: the sample count is behaviourally observable");
    pso::FixedFunctionState masked = four_samples;
    masked.sample_mask = 0xfffffffeu;
    const pso::ProbeOutcome masked_out = run(masked, pso::ProbeScene::kSampleGrid);
    check(masked_out.fragments == 0 && masked_out.coverage[5] == 0,
          "probe: a sample mask that drops the covered sample removes the fragment");
    check(!(masked_out == msaa), "probe: the sample mask is behaviourally observable");

    // ---- Sample quality: the one field no probe can observe. A non-zero quality selects a driver-defined pattern and
    // changes no rendering result the API defines, so every scene gives the same outcome.
    pso::FixedFunctionState quality_one = four_samples;
    quality_one.sample_quality = 1;
    bool quality_visible = false;
    for (const pso::ProbeScene scene : {pso::ProbeScene::kWindingPair, pso::ProbeScene::kTwoDepthQuads,
                                        pso::ProbeScene::kOverlap, pso::ProbeScene::kTopologyList,
                                        pso::ProbeScene::kStripCut, pso::ProbeScene::kSampleGrid,
                                        pso::ProbeScene::kDepthClip})
        quality_visible = quality_visible || !(run(four_samples, scene) == run(quality_one, scene));
    check(!quality_visible, "probe: no scene observes a sample-quality change (only the container's layout could)");

    // The refusal the probe cannot lift: the record yields no observation, so every field stays refused.
    const Bytes blob = make_state_blob();
    rt::graphics_record::State state;
    check(rt::graphics_record::decode_state(Span{blob.data(), blob.size()}, &state),
          "probe: the fixture's measured container still decodes");
    pso::FixedFunctionState out{};
    pso::FixedFunctionFieldStatus fields;
    std::string why;
    check(!pso::decode_fixed_function_state(state, &out, &fields, &why),
          "probe: a decodable container still yields no fixed-function field");
    bool every_field_named = true;
    for (int field = 0; field < 8; ++field) every_field_named = every_field_named && why.find(pso::kFixedFunctionFieldNames[field]) != std::string::npos;
    check(every_field_named, "probe: the refusal names every field the probe contract covers");

    const auto row = [](const char* scene, const char* state, const pso::ProbeOutcome& o) {
        std::printf("  %-18s %-20s prim %u cull %u clip %u frag %3u fail %3u writ %3u oob %u colour %3u %3u %3u %3u\n",
                    scene, state, o.primitives, o.culled, o.clipped, o.fragments, o.depth_failed, o.written,
                    o.out_of_range, o.colour[0], o.colour[1], o.colour[2], o.colour[3]);
    };
    std::printf("graphics_pso_test: behavioural probe observations (4x4 target, one sample unless named)\n");
    row("winding pair", "defaults (cull back)", back_culled);
    row("winding pair", "cull none", nothing_culled);
    row("winding pair", "cull front", front_culled);
    row("depth quads", "defaults (depth off)", depth_off);
    row("depth quads", "depth on, less", depth_tested);
    row("depth quads", "depth write zero", depth_unwritten);
    row("depth quads", "less -> greater", depth_greater_out);
    row("overlap", "blend off", blend_off);
    row("overlap", "src/one-minus-src", blended);
    row("topology list", "triangle type", triangles);
    row("topology list", "point type", points);
    row("strip cut", "disabled", strip_disabled);
    row("strip cut", "0xFFFF", strip_cut);
    row("sample grid", "one sample", one_sample);
    row("sample grid", "four samples", msaa);
    row("sample grid", "four, mask 0x...FE", masked_out);
    row("depth clip", "clip on", clipped);
    row("depth clip", "clip off", clamped);
    std::printf("\n");
}

void test_build_success() {
    const RecordFixture fixture = make_record();
    const Record& record = fixture.record;
    check(record.stage_count == 2, "build: the synthetic PS+VS record decodes");
    const Bytes ps = fspx_code(0x101);
    const Bytes vs = fspx_code(0x202);
    const pso::GraphicsPsoInputs inputs = base_inputs(ps.data(), ps.size(), vs.data(), vs.size());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.SampleDesc.Count = 77;  // a sentinel: build() must overwrite it, so it is not relying on a zeroed structure
    std::string why;
    check(pso::build(record, inputs, &desc, &why), "build: a PS+VS record with an empty input layout builds");
    check(why.empty(), "build: no refusal message on success");
    check(desc.pRootSignature == inputs.root_signature, "build: pRootSignature is the caller's root signature");
    check(desc.PS.pShaderBytecode == ps.data() && desc.PS.BytecodeLength == ps.size(),
          "build: the pixel stage's bytecode is the caller's resolved PS program");
    check(desc.VS.pShaderBytecode == vs.data() && desc.VS.BytecodeLength == vs.size(),
          "build: the vertex stage's bytecode is the caller's resolved VS program");
    check(!desc.DS.pShaderBytecode && !desc.DS.BytecodeLength && !desc.HS.pShaderBytecode &&
              !desc.HS.BytecodeLength && !desc.GS.pShaderBytecode && !desc.GS.BytecodeLength,
          "build: hull, domain and geometry stages are empty (a record has no such slots)");
    check(desc.StreamOutput.NumEntries == 0 && desc.StreamOutput.pSODeclaration == nullptr &&
              desc.StreamOutput.NumStrides == 0 && desc.StreamOutput.pBufferStrides == nullptr &&
              desc.StreamOutput.RasterizedStream == 0,
          "build: no stream output");
    check(std::memcmp(&desc.BlendState, &inputs.defaults.blend, sizeof desc.BlendState) == 0,
          "build: the blend state is the caller's, field for field");
    check(desc.SampleMask == inputs.defaults.sample_mask, "build: the sample mask is the caller's");
    check(std::memcmp(&desc.RasterizerState, &inputs.defaults.rasterizer, sizeof desc.RasterizerState) == 0,
          "build: the rasterizer state is the caller's, field for field");
    check(std::memcmp(&desc.DepthStencilState, &inputs.defaults.depth_stencil, sizeof desc.DepthStencilState) == 0,
          "build: the depth-stencil state is the caller's, field for field");
    check(desc.InputLayout.NumElements == 0 && desc.InputLayout.pInputElementDescs == nullptr,
          "build: an empty input layout is what is proved");
    check(desc.IBStripCutValue == inputs.defaults.strip_cut, "build: the strip-cut value is the caller's");
    check(desc.PrimitiveTopologyType == inputs.defaults.topology_type, "build: the topology type is the caller's");
    check(desc.NumRenderTargets == 1 && desc.RTVFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM,
          "build: the render-target count and format are the caller's");
    bool unused_rt_unknown = true;
    for (UINT target = 1; target < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++target)
        unused_rt_unknown = unused_rt_unknown && desc.RTVFormats[target] == DXGI_FORMAT_UNKNOWN;
    check(unused_rt_unknown, "build: the unused render targets stay DXGI_FORMAT_UNKNOWN");
    check(desc.DSVFormat == DXGI_FORMAT_UNKNOWN, "build: no depth-stencil format when the caller gave none");
    check(desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0, "build: the sample description is the caller's");
    check(desc.NodeMask == 0 && desc.Flags == D3D12_PIPELINE_STATE_FLAG_NONE,
          "build: node 0 and no pipeline flags (the record carries neither)");
    check(desc.CachedPSO.pCachedBlob == nullptr && desc.CachedPSO.CachedBlobSizeInBytes == 0,
          "build: no cached PSO blob");

    // A second, deliberately different state must reach the description unchanged: there is no silent default.
    pso::GraphicsPsoInputs custom = inputs;
    custom.defaults.rasterizer.FillMode = D3D12_FILL_MODE_WIREFRAME;
    custom.defaults.rasterizer.CullMode = D3D12_CULL_MODE_FRONT;
    custom.defaults.rasterizer.FrontCounterClockwise = TRUE;
    custom.defaults.rasterizer.DepthBias = 7;
    custom.defaults.rasterizer.DepthBiasClamp = 0.25f;
    custom.defaults.rasterizer.SlopeScaledDepthBias = 1.5f;
    custom.defaults.rasterizer.DepthClipEnable = FALSE;
    custom.defaults.rasterizer.MultisampleEnable = TRUE;
    custom.defaults.rasterizer.AntialiasedLineEnable = TRUE;
    custom.defaults.blend.AlphaToCoverageEnable = TRUE;
    custom.defaults.blend.IndependentBlendEnable = TRUE;
    custom.defaults.blend.RenderTarget[0].BlendEnable = TRUE;
    custom.defaults.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    custom.defaults.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    custom.defaults.blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
    custom.defaults.depth_stencil.DepthEnable = TRUE;
    custom.defaults.depth_stencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    custom.defaults.depth_stencil.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    custom.defaults.depth_stencil.StencilEnable = TRUE;
    custom.defaults.depth_stencil.StencilReadMask = 0x0f;
    custom.defaults.depth_stencil.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    custom.defaults.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    custom.defaults.strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    custom.defaults.sample_count = 4;
    custom.defaults.sample_mask = 0x0f0f0f0f;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC custom_desc{};
    check(pso::build(record, custom, &custom_desc, &why), "build: a caller-chosen state builds");
    check(std::memcmp(&custom_desc.RasterizerState, &custom.defaults.rasterizer, sizeof custom_desc.RasterizerState) ==
              0,
          "build: the caller's rasterizer state is used verbatim, not a default");
    check(std::memcmp(&custom_desc.BlendState, &custom.defaults.blend, sizeof custom_desc.BlendState) == 0,
          "build: the caller's blend state is used verbatim");
    check(std::memcmp(&custom_desc.DepthStencilState, &custom.defaults.depth_stencil,
                      sizeof custom_desc.DepthStencilState) == 0,
          "build: the caller's depth-stencil state is used verbatim");
    check(custom_desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE &&
              custom_desc.IBStripCutValue == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF &&
              custom_desc.SampleDesc.Count == 4 && custom_desc.SampleMask == 0x0f0f0f0f,
          "build: topology, strip cut, sample count and mask are the caller's");
    // An FSPX payload is what the caller resolved when it has the program's kind-0 state: build() writes the bytes it
    // was given and create_graphics resolves them again.
    const Bytes payload = fspx_payload(fspx_code(0x101));
    pso::GraphicsPsoInputs with_payload = inputs;
    with_payload.stages[kStageSlotPs] = {payload.data(), payload.size()};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC payload_desc{};
    check(pso::build(record, with_payload, &payload_desc, &why), "build: an FSPX payload as the PS bytecode builds");
    check(payload_desc.PS.pShaderBytecode == payload.data() && payload_desc.PS.BytecodeLength == payload.size(),
          "build: the payload bytes go into the description as given");
}

void test_build_ps_only() {
    const RecordFixture fixture = make_record({kStageSlotPs});
    const Record& record = fixture.record;
    check(record.stage_count == 1 && record.stages[0].slot == kStageSlotPs, "build: the PS-only record decodes");
    const Bytes ps = fspx_code(0x101);
    const pso::GraphicsPsoInputs inputs = base_inputs(ps.data(), ps.size(), nullptr, 0);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::string why;
    check(pso::build(record, inputs, &desc, &why), "build: a PS-only record builds");
    check(desc.PS.pShaderBytecode == ps.data() && !desc.VS.pShaderBytecode,
          "build: a PS-only record has a pixel stage and no vertex stage");
}

// The input-layout element table wired into D3D12_INPUT_ELEMENT_DESC entries (docs/input_layout.md).
void test_build_input_layout() {
    const Bytes ps = fspx_code(0x101);
    const Bytes vs = fspx_code(0x202);
    const pso::GraphicsPsoInputs inputs = base_inputs(ps.data(), ps.size(), vs.data(), vs.size());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::string why;

    // The measured format words are the documented DXGI enum values, which is the mapping build() applies.
    check(pso::measured_input_format(2) && pso::measured_input_format(2)->dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT &&
              DXGI_FORMAT_R32G32B32A32_FLOAT == 2 && pso::measured_input_format(2)->bytes == 16,
          "layout build: format word 2 is DXGI_FORMAT_R32G32B32A32_FLOAT (16 bytes)");
    check(pso::measured_input_format(6) && pso::measured_input_format(6)->dxgi == DXGI_FORMAT_R32G32B32_FLOAT &&
              DXGI_FORMAT_R32G32B32_FLOAT == 6 && pso::measured_input_format(6)->bytes == 12,
          "layout build: format word 6 is DXGI_FORMAT_R32G32B32_FLOAT (12 bytes)");
    check(pso::measured_input_format(16) && pso::measured_input_format(16)->dxgi == DXGI_FORMAT_R32G32_FLOAT &&
              DXGI_FORMAT_R32G32_FLOAT == 16 && pso::measured_input_format(16)->bytes == 8,
          "layout build: format word 16 is DXGI_FORMAT_R32G32_FLOAT (8 bytes)");
    check(pso::measured_input_format(28) && pso::measured_input_format(28)->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM &&
              DXGI_FORMAT_R8G8B8A8_UNORM == 28 && pso::measured_input_format(28)->bytes == 4,
          "layout build: format word 28 is DXGI_FORMAT_R8G8B8A8_UNORM (4 bytes)");
    check(pso::measured_input_format(30) && pso::measured_input_format(30)->dxgi == DXGI_FORMAT_R8G8B8A8_UINT &&
              DXGI_FORMAT_R8G8B8A8_UINT == 30 && pso::measured_input_format(30)->bytes == 4,
          "layout build: format word 30 is DXGI_FORMAT_R8G8B8A8_UINT (4 bytes)");
    check(!pso::measured_input_format(0) && !pso::measured_input_format(7),
          "layout build: an unmeasured format word has no mapping");

    // The measured 188-byte shape: three elements become three D3D12_INPUT_ELEMENT_DESC entries with the measured
    // formats, packed offsets, slot 0, semantic index 0, per-vertex data and step rate 0.
    const RecordFixture fixture_188 = make_record({kStageSlotPs, kStageSlotVs}, make_layout_188());
    const Record& record_188 = fixture_188.record;
    check(record_188.stage_count == 2 && pso::build(record_188, inputs, &desc, &why),
          "layout build: a record with the measured 188-byte layout builds");
    check(desc.InputLayout.NumElements == 3 && desc.InputLayout.pInputElementDescs != nullptr,
          "layout build: the description carries the three decoded elements");
    if (desc.InputLayout.NumElements == 3 && desc.InputLayout.pInputElementDescs) {
        const D3D12_INPUT_ELEMENT_DESC* e = desc.InputLayout.pInputElementDescs;
        check(std::strcmp(e[0].SemanticName, "POSITION") == 0 && e[0].SemanticIndex == 0 &&
                  e[0].Format == DXGI_FORMAT_R32G32B32_FLOAT && e[0].InputSlot == 0 && e[0].AlignedByteOffset == 0 &&
                  e[0].InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA && e[0].InstanceDataStepRate == 0,
              "layout build: POSITION -> R32G32B32_FLOAT, slot 0, offset 0, per-vertex");
        check(std::strcmp(e[1].SemanticName, "COLOR") == 0 && e[1].Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                  e[1].AlignedByteOffset == 12,
              "layout build: COLOR -> R8G8B8A8_UNORM at offset 12");
        check(std::strcmp(e[2].SemanticName, "TEXCOORD") == 0 && e[2].Format == DXGI_FORMAT_R32G32_FLOAT &&
                  e[2].AlignedByteOffset == 16,
              "layout build: TEXCOORD -> R32G32_FLOAT at offset 16");
        // The semantic pointers are the record's own validated blob bytes, not a copy that could dangle.
        check(e[0].SemanticName == reinterpret_cast<const char*>(record_188.input_layout.data + 0x54) &&
                  e[1].SemanticName == reinterpret_cast<const char*>(record_188.input_layout.data + 0x79),
              "layout build: each SemanticName points at the record's validated name bytes");
    }

    // The measured 224-byte shape is the one that pins the attribute field order: its last element is COLOR with word 0
    // 2 (the COLOR2 semantic index) and word 1 30 (R8G8B8A8_UINT), so the six words are the D3D12 element fields in
    // order and the duplicate semantic name is not ambiguous.
    const RecordFixture fixture_224 = make_record({kStageSlotPs, kStageSlotVs}, make_layout_224());
    check(pso::build(fixture_224.record, inputs, &desc, &why) && desc.InputLayout.NumElements == 4,
          "layout build: the measured 224-byte four-element layout builds");
    if (desc.InputLayout.NumElements == 4 && desc.InputLayout.pInputElementDescs) {
        const D3D12_INPUT_ELEMENT_DESC* e = desc.InputLayout.pInputElementDescs;
        check(std::strcmp(e[0].SemanticName, "COLOR") == 0 && e[0].SemanticIndex == 0 &&
                  e[0].Format == DXGI_FORMAT_R8G8B8A8_UNORM && e[0].AlignedByteOffset == 0,
              "layout build: COLOR0 -> R8G8B8A8_UNORM at offset 0 with semantic index 0");
        check(std::strcmp(e[3].SemanticName, "COLOR") == 0 && e[3].SemanticIndex == 2 &&
                  e[3].Format == DXGI_FORMAT_R8G8B8A8_UINT && e[3].AlignedByteOffset == 20,
              "layout build: COLOR2 -> R8G8B8A8_UINT at offset 20 with semantic index 2");
    }

    // Every other measured shape builds with its own formats and packed offsets.
    const RecordFixture fixture_120 = make_record({kStageSlotPs, kStageSlotVs}, make_layout_120());
    check(pso::build(fixture_120.record, inputs, &desc, &why) && desc.InputLayout.NumElements == 1 &&
              desc.InputLayout.pInputElementDescs[0].Format == DXGI_FORMAT_R32G32B32_FLOAT,
          "layout build: the measured 120-byte shape builds one POSITION element");
    const RecordFixture fixture_152 = make_record({kStageSlotPs, kStageSlotVs}, make_layout_152());
    check(pso::build(fixture_152.record, inputs, &desc, &why) && desc.InputLayout.NumElements == 2 &&
              desc.InputLayout.pInputElementDescs[1].Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
              desc.InputLayout.pInputElementDescs[1].AlignedByteOffset == 12,
          "layout build: the measured 152-byte shape builds POSITION + COLOR at offset 12");
    const RecordFixture fixture_156 = make_record({kStageSlotPs, kStageSlotVs}, make_layout_156());
    check(pso::build(fixture_156.record, inputs, &desc, &why) && desc.InputLayout.NumElements == 2 &&
              desc.InputLayout.pInputElementDescs[0].Format == DXGI_FORMAT_R32G32_FLOAT &&
              desc.InputLayout.pInputElementDescs[0].AlignedByteOffset == 0 &&
              desc.InputLayout.pInputElementDescs[1].AlignedByteOffset == 8,
          "layout build: the measured 156-byte shape builds packed float2 elements at 0 and 8");

    // A measured gap between elements is emitted as stored: the post-decode boot carries an 188-byte shape with
    // COLOR at offset 24 after a 12-byte POSITION, and D3D12 takes AlignedByteOffset verbatim (the description has no
    // stride), so gaps build and the stored offsets reach the description unchanged.
    const RecordFixture gapped = make_record(
        {kStageSlotPs, kStageSlotVs},
        make_input_layout({{"POSITION", layout_words(6, 0)}, {"COLOR", layout_words(28, 24)},
                           {"TEXCOORD", layout_words(16, 28)}}));
    check(pso::build(gapped.record, inputs, &desc, &why) && desc.InputLayout.NumElements == 3,
          "layout build: a measured gap between elements builds");
    if (desc.InputLayout.NumElements == 3 && desc.InputLayout.pInputElementDescs) {
        const D3D12_INPUT_ELEMENT_DESC* e = desc.InputLayout.pInputElementDescs;
        check(e[1].AlignedByteOffset == 24 && e[2].AlignedByteOffset == 28,
              "layout build: the stored gap offsets 24 and 28 reach the description verbatim");
    }
    std::array<std::uint32_t, rt::graphics_record::kInputLayoutAttributeWords> append_words = layout_words(6, 0);
    append_words[3] = D3D12_APPEND_ALIGNED_ELEMENT;
    const RecordFixture append = make_record({kStageSlotPs, kStageSlotVs},
                                             make_input_layout({{"POSITION", append_words}}));
    why.clear();
    check(!pso::build(append.record, inputs, &desc, &why) &&
              why.find("input_layout.elements[0].aligned_byte_offset[4294967295]") != std::string::npos,
          "layout build: the unmeasured D3D12_APPEND_ALIGNED_ELEMENT sentinel is refused with its value");

    // Format word 2's 16-byte size is not independently offset-confirmed, so the offset check is its
    // falsifier: a following element at 16 bytes is accepted, one at 12 is refused and names the offset field.
    const RecordFixture float4 = make_record(
        {kStageSlotPs, kStageSlotVs},
        make_input_layout({{"POSITION", layout_words(2, 0)}, {"COLOR", layout_words(28, 16)}}));
    check(pso::build(float4.record, inputs, &desc, &why) && desc.InputLayout.NumElements == 2,
          "layout build: format word 2 advances the packed offset by 16 bytes");
    const RecordFixture float3 = make_record(
        {kStageSlotPs, kStageSlotVs},
        make_input_layout({{"POSITION", layout_words(2, 0)}, {"COLOR", layout_words(28, 12)}}));
    check(!pso::build(float3.record, inputs, &desc, &why) &&
              why.find("input_layout.elements[1].aligned_byte_offset[12]") != std::string::npos,
          "layout build: a 12-byte stride after format word 2 is refused as an overlap, making a wrong size "
          "hypothesis visible");

    // Refusals. Each field carries the numeric value so the slot-57 boot histogram lists exactly which values remain
    // unmeasured (the histogram keys a reason by the text before its first ':').
    const RecordFixture unknown_last = make_record(
        {kStageSlotPs, kStageSlotVs},
        make_input_layout({{"POSITION", layout_words(6, 0)}, {"COLOR", layout_words(28, 12)},
                           {"TEXCOORD", layout_words(7, 16)}}));
    why.clear();
    check(!pso::build(unknown_last.record, inputs, &desc, &why),
          "layout build: an unmeasured format word is refused");
    check(why.find("input_layout.elements[2].format[7]") != std::string::npos &&
              why.find("TEXCOORD") != std::string::npos,
          "layout build: the refusal field carries the element index and the format word value");
    const RecordFixture unknown_first =
        make_record({kStageSlotPs, kStageSlotVs}, make_input_layout({{"POSITION", layout_words(7, 0)}}));
    why.clear();
    check(!pso::build(unknown_first.record, inputs, &desc, &why) &&
              why.find("input_layout.elements[0].format[7]") != std::string::npos,
          "layout build: an unmeasured format word on the first element names elements[0].format[7]");

    std::array<std::uint32_t, rt::graphics_record::kInputLayoutAttributeWords> words = layout_words(6, 0);
    words[2] = 1;  // word 2 is InputSlot under the measured field order; 0 in every measured element
    const RecordFixture bad_slot = make_record({kStageSlotPs, kStageSlotVs}, make_input_layout({{"POSITION", words}}));
    why.clear();
    check(!pso::build(bad_slot.record, inputs, &desc, &why) &&
              why.find("input_layout.elements[0].input_slot[1]") != std::string::npos,
          "layout build: a non-zero input slot is refused with the slot value in the field");
    words = layout_words(6, 0);
    words[4] = 1;  // word 4 is InputSlotClass; 0 (per-vertex) in every measured element
    const RecordFixture bad_class =
        make_record({kStageSlotPs, kStageSlotVs}, make_input_layout({{"POSITION", words}}));
    why.clear();
    check(!pso::build(bad_class.record, inputs, &desc, &why) &&
              why.find("input_layout.elements[0].input_slot_class[1]") != std::string::npos,
          "layout build: a non-zero input-slot class is refused with the class value in the field");
    words = layout_words(6, 0);
    words[5] = 1;  // word 5 is InstanceDataStepRate; 0 in every measured element
    const RecordFixture bad_step =
        make_record({kStageSlotPs, kStageSlotVs}, make_input_layout({{"POSITION", words}}));
    why.clear();
    check(!pso::build(bad_step.record, inputs, &desc, &why) &&
              why.find("input_layout.elements[0].instance_step_rate[1]") != std::string::npos,
          "layout build: a non-zero instance step rate is refused with the rate value in the field");

    const RecordFixture wrong_offset = make_record(
        {kStageSlotPs, kStageSlotVs},
        make_input_layout({{"POSITION", layout_words(6, 0)}, {"COLOR", layout_words(28, 8)}}));
    why.clear();
    check(!pso::build(wrong_offset.record, inputs, &desc, &why) &&
              why.find("input_layout.elements[1].aligned_byte_offset[8]") != std::string::npos,
          "layout build: an aligned offset that overlaps the preceding element is refused with its value");

    // Two elements with the same name and the same index would be one D3D12 semantic; that is refused, while the
    // measured 224-byte shape's COLOR 0 / COLOR 2 above is not a duplicate.
    const RecordFixture duplicate = make_record(
        {kStageSlotPs, kStageSlotVs},
        make_input_layout({{"POSITION", layout_words(6, 0)}, {"POSITION", layout_words(6, 12)}}));
    why.clear();
    check(!pso::build(duplicate.record, inputs, &desc, &why),
          "layout build: a repeated semantic name and index is refused");
    check(why.find("input_layout.elements[1].semantic_index[0]") != std::string::npos,
          "layout build: the duplicate-semantic refusal names the element and the index");

    const RecordFixture no_vertex = make_record({kStageSlotPs}, make_layout_120());
    why.clear();
    check(!pso::build(no_vertex.record, inputs, &desc, &why),
          "layout build: an input layout with elements but no vertex stage is refused");
    check(why.find("record.stages") != std::string::npos && why.find("no vertex stage") != std::string::npos,
          "layout build: the refusal says the input layout needs a vertex stage");

    // A refusal leaves the output description untouched.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC untouched{};
    untouched.SampleDesc.Count = 77;
    why.clear();
    check(!pso::build(unknown_last.record, inputs, &untouched, &why) && untouched.SampleDesc.Count == 77,
          "layout build: the output description is untouched on a layout refusal");
}

void test_build_refusals() {
    const RecordFixture fixture = make_record();
    const Record& record = fixture.record;
    const Bytes ps = fspx_code(0x101);
    const Bytes vs = fspx_code(0x202);
    const pso::GraphicsPsoInputs good = base_inputs(ps.data(), ps.size(), vs.data(), vs.size());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::string why;

    // The fixed-function state blob.
    Record no_state = record;
    no_state.state = Span{};
    why.clear();
    check(!pso::build(no_state, good, &desc, &why), "build: a record without its state blob is refused");
    check(why.find("record.state") != std::string::npos, "build: the refusal names record.state");

    // The root signature: the record's, and the caller's mapping of it.
    Record no_root = record;
    no_root.root_signature = nullptr;
    why.clear();
    check(!pso::build(no_root, good, &desc, &why), "build: a record without a root signature is refused");
    check(why.find("record.root_signature") != std::string::npos, "build: the refusal names record.root_signature");
    pso::GraphicsPsoInputs no_pc_root = good;
    no_pc_root.root_signature = nullptr;
    why.clear();
    check(!pso::build(record, no_pc_root, &desc, &why), "build: a missing PC root signature is refused");
    check(why.find("pRootSignature") != std::string::npos, "build: the refusal names pRootSignature");

    // The per-stage bytecode: a stage the record names without bytes, and bytes for a stage it does not name.
    pso::GraphicsPsoInputs no_vs = good;
    no_vs.stages[kStageSlotVs] = {};
    why.clear();
    check(!pso::build(record, no_vs, &desc, &why), "build: a record stage without resolved program bytes is refused");
    check(why.find("stages[1] (vs)") != std::string::npos, "build: the refusal names the stage slot");
    const RecordFixture ps_only_fixture = make_record({kStageSlotPs});
    const Record& ps_only = ps_only_fixture.record;
    why.clear();
    check(!pso::build(ps_only, good, &desc, &why), "build: bytes for a stage the record does not name are refused");
    check(why.find("stages[1] (vs)") != std::string::npos && why.find("no vs stage") != std::string::npos,
          "build: the refusal says the record has no vs stage");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC untouched{};
    untouched.SampleDesc.Count = 77;
    check(!pso::build(record, no_vs, &untouched, &why) && untouched.SampleDesc.Count == 77,
          "build: the output description is untouched on a refusal");

    // The attachment lists.
    pso::GraphicsPsoInputs too_many = good;
    too_many.render_target_count = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1;
    why.clear();
    check(!pso::build(record, too_many, &desc, &why), "build: more render targets than D3D12 allows is refused");
    check(why.find("render_target_count") != std::string::npos, "build: the refusal names render_target_count");
    pso::GraphicsPsoInputs unknown_format = good;
    unknown_format.render_target_count = 2;  // rtv_formats[1] stays DXGI_FORMAT_UNKNOWN
    why.clear();
    check(!pso::build(record, unknown_format, &desc, &why), "build: a used render target with no format is refused");
    check(why.find("rtv_formats[1]") != std::string::npos, "build: the refusal names the render-target slot");
    pso::GraphicsPsoInputs stray_format = good;
    stray_format.rtv_formats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;  // unused, so D3D12 requires UNKNOWN
    why.clear();
    check(!pso::build(record, stray_format, &desc, &why), "build: a format on an unused render target is refused");
    check(why.find("rtv_formats[1]") != std::string::npos, "build: the refusal names the unused render-target slot");

    // The fixed-function state: a zeroed one is not "the console's", it is refused field by field.
    pso::GraphicsPsoInputs zeroed = good;
    zeroed.defaults = {};
    why.clear();
    check(!pso::build(record, zeroed, &desc, &why), "build: a zeroed fixed-function state is refused");
    check(why.find("defaults.topology_type") != std::string::npos,
          "build: the refusal names defaults.topology_type");
    pso::GraphicsPsoInputs bad_samples = good;
    bad_samples.defaults.sample_count = 3;
    why.clear();
    check(!pso::build(record, bad_samples, &desc, &why), "build: a non-power-of-two sample count is refused");
    check(why.find("defaults.sample_count") != std::string::npos, "build: the refusal names defaults.sample_count");
    pso::GraphicsPsoInputs bad_quality = good;
    bad_quality.defaults.sample_quality = 5;
    why.clear();
    check(!pso::build(record, bad_quality, &desc, &why), "build: an unknown sample quality is refused");
    check(why.find("defaults.sample_quality") != std::string::npos, "build: the refusal names defaults.sample_quality");
    pso::GraphicsPsoInputs bad_fill = good;
    bad_fill.defaults.rasterizer.FillMode = static_cast<D3D12_FILL_MODE>(4);
    why.clear();
    check(!pso::build(record, bad_fill, &desc, &why), "build: an unknown fill mode is refused");
    check(why.find("defaults.rasterizer.FillMode") != std::string::npos,
          "build: the refusal names defaults.rasterizer.FillMode");
    pso::GraphicsPsoInputs bad_cull = good;
    bad_cull.defaults.rasterizer.CullMode = static_cast<D3D12_CULL_MODE>(4);
    why.clear();
    check(!pso::build(record, bad_cull, &desc, &why), "build: an unknown cull mode is refused");
    check(why.find("defaults.rasterizer.CullMode") != std::string::npos,
          "build: the refusal names defaults.rasterizer.CullMode");
    pso::GraphicsPsoInputs bad_strip = good;
    bad_strip.defaults.strip_cut = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(7);
    why.clear();
    check(!pso::build(record, bad_strip, &desc, &why), "build: an unknown strip-cut value is refused");
    check(why.find("defaults.strip_cut") != std::string::npos, "build: the refusal names defaults.strip_cut");

    // A mesh stage cannot be expressed in a D3D12_GRAPHICS_PIPELINE_STATE_DESC.
    const RecordFixture ps_ms_fixture = make_record({kStageSlotPs, kStageSlotMs});
    const Record& ps_ms = ps_ms_fixture.record;
    check(ps_ms.stage_count == 2, "build: a PS+MS record decodes");
    pso::GraphicsPsoInputs with_ms = base_inputs(ps.data(), ps.size(), nullptr, 0);
    with_ms.stages[kStageSlotMs] = {ps.data(), ps.size()};
    why.clear();
    check(!pso::build(ps_ms, with_ms, &desc, &why), "build: a record with a mesh stage is refused");
    check(why.find("mesh stage") != std::string::npos && why.find("PIPELINE_STATE_STREAM") != std::string::npos,
          "build: the refusal says a mesh stage needs the pipeline-state-stream path");

    // Caller-built records that decode() would never produce are still checked.
    Record no_stages;
    no_stages.stage_count = 0;
    why.clear();
    check(!pso::build(no_stages, good, &desc, &why), "build: a record with no stages is refused");
    check(why.find("record.stage_count") != std::string::npos, "build: the refusal names record.stage_count");
    Record bad_slot = record;
    bad_slot.stages[0].slot = 5;
    why.clear();
    check(!pso::build(bad_slot, good, &desc, &why), "build: a stage slot outside the record's three is refused");
    check(why.find("slot 5") != std::string::npos, "build: the refusal names the slot");
    Record twice = record;
    twice.stage_count = 2;
    twice.stages[0].slot = kStageSlotPs;
    twice.stages[1].slot = kStageSlotPs;
    why.clear();
    check(!pso::build(twice, good, &desc, &why), "build: the same stage slot twice is refused");
    check(why.find("appears twice") != std::string::npos, "build: the refusal says the slot appears twice");

    check(!pso::build(record, good, nullptr, &why), "build: a null output description is refused");
}

// The input and refusal surface, printed for the next reader.
// The console graphics user-SGPR layout (docs/root_arguments.md): a translated vertex/pixel shader reads root
// parameters from the stage base (s8; mesh s16), one dword per descriptor table, not from the PC packing at s0. The
// helper is a pure placement (rt::rootsig::pack_stage) with the stage chosen by the pipeline's shape; a signature that
// would spill is refused with the parameter named, never truncated.
void test_console_graphics_user_data() {
    using rt::rootsig::ParamType;
    using rt::rootsig::RootParameter;
    using rt::rootsig::RootSignature;

    RootSignature signature;
    RootParameter constants;
    constants.type = ParamType::Constants;
    constants.num_values = 2;
    signature.params.push_back(constants);
    RootParameter table;
    table.type = ParamType::Table;
    table.ranges.resize(3);
    signature.params.push_back(table);
    RootParameter view;
    view.type = ParamType::Descriptor;
    signature.params.push_back(view);

    pso::ConsoleUserDataPlacement placement;
    check(pso::console_graphics_user_data(signature, false, &placement), "console layout: the vs/ps signature places");
    check(placement.ok && placement.base == 8, "console layout: the vertex/pixel base is s8");
    check(placement.first_sgpr.size() == 3, "console layout: one entry per root parameter");
    check(placement.first_sgpr[0] == 8, "console layout: the first constants take s8");
    check(placement.first_sgpr[1] == 10, "console layout: a descriptor table takes one dword at s10");
    check(placement.first_sgpr[2] == 11, "console layout: a root view takes two dwords at s11");

    pso::ConsoleUserDataPlacement mesh;
    check(pso::console_graphics_user_data(signature, true, &mesh), "console layout: the same signature places for mesh");
    check(mesh.base == 16 && mesh.first_sgpr[0] == 16, "console layout: the mesh base is s16");

    // A signature that would run past s31 must be refused by name (the console spills; that layout is not modelled).
    RootSignature big;
    RootParameter many;
    many.type = ParamType::Constants;
    many.num_values = 28;
    big.params.push_back(many);
    big.params.push_back(table);
    pso::ConsoleUserDataPlacement refused;
    check(!pso::console_graphics_user_data(big, false, &refused), "console layout: a spilling signature is refused");
    check(!refused.ok && refused.error.find("parameter") != std::string::npos,
          "console layout: the refusal names the parameter that does not fit");

    std::printf("graphics_pso_test: console graphics user-data placement (docs/root_arguments.md)\n");
    std::printf("  %-10s %-6s %s\n", "pipeline", "base", "first user SGPR per parameter");
    std::printf("  %-10s s%-5u", "vs/ps", placement.base);
    for (std::uint32_t first : placement.first_sgpr) std::printf(" s%u", first);
    std::printf("\n  %-10s s%-5u", "mesh", mesh.base);
    for (std::uint32_t first : mesh.first_sgpr) std::printf(" s%u", first);
    std::printf("\n");
}

void print_findings() {
    struct Row {
        const char* item;
        const char* source;
    };
    const Row rows[] = {
        {"pRootSignature", "caller, mapped from record.root_signature (decoder copies only the pointer)"},
        {"VS/PS bytecode", "caller's resolved native program per record slot (resolve_native_stage)"},
        {"DS/HS/GS", "empty: a record has no such slot"},
        {"InputLayout", "decoded element table; formats, semantic index and packed offsets from the measured words"},
        {"Rasterizer/Blend/DepthStencil/Sample/Topology", "caller's FixedFunctionState (docs/graphics_pso.md)"},
        {"RTVFormats/DSVFormat/NumRenderTargets", "caller"},
        {"NodeMask/Flags/CachedPSO/StreamOutput", "neutral values: the record carries none of them"},
        {"record.state", "container decoded (two size families, stage blocks, variable region); no field is used"},
        {"decode_fixed_function_state", "refuses all eight fields: no word is pinned by a measurement (docs/state_blob.md)"},
        {"run_probe", "the behavioural falsifier: 7 of 8 fixed-function fields have a scene whose observation changes"},
        {"kProbeContract[6]", "sample quality is not observable by any defined rendering result; only the container's layout could pin it"},
        {"mesh record (slot 2)", "refused: D3D12_GRAPHICS_PIPELINE_STATE_DESC has no mesh field"},
    };
    std::printf("graphics_pso_test: where each D3D12_GRAPHICS_PIPELINE_STATE_DESC field comes from\n");
    std::printf("%-44s %s\n", "field", "source");
    for (const Row& row : rows) std::printf("%-44s %s\n", row.item, row.source);
    std::printf("\n");
    const Row refusals[] = {
        {"build", "record.stage_count / record.stages[i].slot: not a graphics record's shape"},
        {"build", "record.root_signature: no console root signature at +0x00"},
        {"build", "record.state: the fixed-function blob is missing or outside 380..520 B"},
        {"build", "record.input_layout: the layout blob does not decode"},
        {"build", "input_layout.elements[i].format[word]: an element's format word is not one of the measured DXGI values"},
        {"build", "input_layout.elements[i].input_slot[n] / input_slot_class[n] / instance_step_rate[n]: a non-zero unmeasured word"},
        {"build", "input_layout.elements[i].aligned_byte_offset: an offset that is not the measured packed one"},
        {"build", "input_layout.elements[i].semantic_index[n]: a repeated semantic name and index"},
        {"build", "pRootSignature: the caller mapped no PC root signature"},
        {"build", "stages[i] (stage): the record stage has no resolved bytes, or gets bytes it has no slot for"},
        {"build", "render_target_count / rtv_formats[i]: an attachment list D3D12 would refuse"},
        {"build", "defaults.*: the caller's fixed-function state is not a valid value"},
        {"build", "mesh stage: needs D3D12_PIPELINE_STATE_STREAM_DESC's MS subobject"},
        {"resolve_native_stage", "no translation / stage mismatch / ambiguous: no single module can be chosen"},
        {"resolve_native_stage", "returns false: the bytes are not a native program (FSSM/stand-in path applies)"},
    };
    std::printf("graphics_pso_test: refusal surface\n");
    std::printf("%-24s %s\n", "function", "refusal");
    for (const Row& row : refusals) std::printf("%-24s %s\n", row.item, row.source);
    std::printf("\n");
}

}  // namespace

int main() {
    print_findings();

    test_resolve_native_stage();
    test_pick_entry();
    test_documented_default_state();
    test_console_graphics_user_data();
    test_fixed_function_state_decode();
    test_fixed_function_probe();
    test_build_success();
    test_build_ps_only();
    test_build_input_layout();
    test_build_refusals();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s) of %d check(s)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("graphics_pso_test: all %d checks passed\n", g_checks);
    return 0;
}
