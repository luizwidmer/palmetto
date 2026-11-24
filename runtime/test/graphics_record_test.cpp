// Synthetic blobs for runtime/src/graphics_record.*: the slot-57 record's six pointer/size pairs, its fixed-function
// state blob, its input-layout blob and two kind-2 stage blobs, all built byte by byte here. Nothing in this file is
// game data: the state blob's container words are the structural words the measurements quote (docs/state_blob.md),
// and every other byte is a pattern this test chose.
//
// Covered: a PS+VS record that decodes to the expected spans/offsets; the decoder promises not to read through the
// record's pointers (a record full of fake pointers still decodes); an 80-byte mesh input layout with no elements; the
// measured input-layout element table (count at +0x4C, then per element a u32 name length incl NUL, the name and a
// 24-byte attribute block, zero-padded to a 4-byte boundary) on all six measured shapes; the state blob's measured
// container on all fourteen size shapes (two families, the variable-region pair, the header constants) and the
// state-size/stage-family cross-check; every measured bound (380..520 B state, 80..340 B layout, 52..239,464 B
// program, 0x10 + 32*k stage blob) on both sides; and the malformed cases: a short descriptor, zero stages, half-set
// pairs, a non-zero reserved byte, a vertex and mesh stage together, a state size no measured family has or one that
// disagrees with the record's stages, a state header word outside the measurement, an input layout whose count, names,
// lengths, padding or fill are not the measured shape (including the refuted contiguous-names form), a stage blob whose
// kind, size word, 0x40 head tag, tag order, final 0x43 record or 12 zero bytes disagree. It prints the
// decoded/unknown field table so the boundary is visible without reading the header.

#include "../src/graphics_record.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using rt::graphics_record::Element;
using rt::graphics_record::Record;
using rt::graphics_record::Span;
using rt::graphics_record::StageBlob;
using rt::graphics_record::State;

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

std::uint64_t pointer_word(const Bytes& blob) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(blob.data()));
}

// A pointer/size pair as the record stores it: pointer at `at`, size at `at + 8`.
void put_span_pair(Bytes* bytes, std::size_t at, const Bytes& blob) {
    put_u64(bytes, at, pointer_word(blob));
    put_u64(bytes, at + 8, blob.size());
}

// A state blob in the measured container shape (docs/state_blob.md): the vertex family is 380 + 8*k (k 0..5), the
// mesh family 464 + 8*k (k 0..7); a shape carries the measured header words, the variable-region pair and zeros
// elsewhere. The words are structural, not this title's state: the decoder keeps them all raw.
Bytes make_state_shape(std::size_t size) {
    const std::size_t vertex_max =
        rt::graphics_record::kStateVertexFamilyBaseBytes +
        rt::graphics_record::kStateFamilyStepBytes * rt::graphics_record::kStateVertexFamilyMaxItems;
    const bool vertex_family = size <= vertex_max;
    const std::size_t base = vertex_family ? rt::graphics_record::kStateVertexFamilyBaseBytes
                                           : rt::graphics_record::kStateMeshFamilyBaseBytes;
    const std::size_t stage_bytes = vertex_family ? rt::graphics_record::kStateVertexStageBlockBytes
                                                  : rt::graphics_record::kStateMeshStageBlockBytes;
    const std::size_t variable_at =
        rt::graphics_record::kStateHeaderBytes + stage_bytes + rt::graphics_record::kStateCommonBlockBytes;
    const std::size_t items = (size - base) / rt::graphics_record::kStateFamilyStepBytes;
    Bytes blob(size, 0);
    put_u32(&blob, 0x00, rt::graphics_record::kStateHeaderWord00Base | (items != 0 ? 0x08u : 0u));
    put_u32(&blob, 0x04, vertex_family ? rt::graphics_record::kStateStageWordVertex
                                       : rt::graphics_record::kStateStageWordMesh);
    put_u32(&blob, 0x08, rt::graphics_record::kStateHeaderWord08);
    put_u32(&blob, 0x0c, static_cast<std::uint32_t>(items * 2) | (items != 0 ? 0x20040u : 0u));
    put_u32(&blob, 0x10, rt::graphics_record::kStateHeaderWord10);
    const std::uint32_t payload =
        static_cast<std::uint32_t>(size - variable_at - rt::graphics_record::kStateVariableHeaderBytes);
    put_u32(&blob, rt::graphics_record::kStateVariablePairOffset,
            (payload << 16) | static_cast<std::uint32_t>(variable_at));
    put_u32(&blob, 0x150, 0x00000040u);  // a state word the measurements quote: kept raw, never interpreted
    return blob;
}

// The measured 388-byte state sample's shape (the vertex family, k = 1).
Bytes make_state_blob() { return make_state_shape(388); }

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

// A blob in the measured shape: the element count at +0x4C, then one record per element (a u32 name length including
// the NUL, the name bytes and a 24-byte attribute block), then zero padding to a 4-byte boundary. The measured sizes
// are 80 (count 0), 120/1, 152/2, 156/2, 188/3 and 224/4.
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
Bytes make_layout_120() {  // one element: POSITION, float3 at 0
    return make_input_layout({{"POSITION", layout_words(6, 0)}});
}
Bytes make_layout_152() {  // POSITION float3 at 0, COLOR U8x4 at 12
    return make_input_layout({{"POSITION", layout_words(6, 0)}, {"COLOR", layout_words(28, 12)}});
}
Bytes make_layout_156() {  // TEXCOORD float2 at 0, POSITION float2 at 8
    return make_input_layout({{"TEXCOORD", layout_words(16, 0)}, {"POSITION", layout_words(16, 8)}});
}
Bytes make_layout_188() {  // POSITION float3 at 0, COLOR U8x4 at 12, TEXCOORD float2 at 16
    return make_input_layout(
        {{"POSITION", layout_words(6, 0)}, {"COLOR", layout_words(28, 12)}, {"TEXCOORD", layout_words(16, 16)}});
}
Bytes make_layout_224() {  // COLOR0 U8x4 at 0, TEXCOORD float2 at 4, POSITION float2 at 12, COLOR2 U8x4 at 20
    return make_input_layout({{"COLOR", layout_words(28, 0)},
                              {"TEXCOORD", layout_words(16, 4)},
                              {"POSITION", layout_words(16, 12)},
                              {"COLOR", layout_words(30, 20, 2)}});
}

// Synthetic native-program bytes (not a shader): a repeating pattern of the requested size.
Bytes make_program(std::size_t size, std::uint8_t seed) {
    Bytes blob(size);
    for (std::size_t i = 0; i < size; ++i) blob[i] = static_cast<std::uint8_t>((i * 37 + seed) & 0xff);
    return blob;
}

// A kind-2 stage blob: head (kind 2, size word, unknown word, 0x40 tag) and 32-byte records. `tags` must end with the
// 0x43 digest record; the digest bytes are a pattern this test chose.
Bytes make_stage_blob(const std::vector<std::uint32_t>& tags, std::uint8_t digest_seed) {
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
                blob[at + 4 + byte] = static_cast<std::uint8_t>((0xa0 + digest_seed + byte) & 0xff);
        } else {
            for (std::size_t word = 0; word < 7; ++word)
                put_u32(&blob, at + 4 + 4 * word, 0x11000000u | (static_cast<std::uint32_t>(index) << 8) |
                                                       static_cast<std::uint32_t>(word));
        }
    }
    return blob;
}

struct Fixture {
    Bytes state = make_state_blob();
    Bytes layout = make_layout_188();
    Bytes ps_program = make_program(96, 0x11);
    Bytes ps_blob = make_stage_blob({0x41, 0x42, 0x43}, 1);
    Bytes vs_program = make_program(52, 0x22);
    Bytes vs_blob = make_stage_blob({0x41, 0x43}, 2);
    Bytes descriptor = make_descriptor();

    // 0x108 validated bytes plus an unidentified tail that decode() must ignore.
    Bytes make_descriptor() const {
        Bytes desc(0x120, 0);
        put_u64(&desc, 0x00, 0x1122334455667788ull);  // a pointer value; nothing dereferences it
        put_span_pair(&desc, 0x08, state);
        put_span_pair(&desc, 0x18, layout);
        put_span_pair(&desc, 0x28, ps_program);
        put_span_pair(&desc, 0x38, ps_blob);
        put_span_pair(&desc, 0x48, vs_program);
        put_span_pair(&desc, 0x58, vs_blob);
        for (std::size_t at = 0x108; at < desc.size(); ++at) desc[at] = 0xee;  // the unknown tail
        return desc;
    }
};

// ----------------------------------------------------------------------------------------------------------------

void test_decode_success() {
    const Fixture fixture;
    Record record;
    check(rt::graphics_record::decode(fixture.descriptor.data(), fixture.descriptor.size(), &record),
          "decode: the synthetic PS+VS record decodes");
    check(record.stage_count == 2, "decode: two stages are present");
    check(record.stages[0].slot == rt::graphics_record::kStageSlotPs, "decode: slot 0 is the pixel shader");
    check(record.stages[1].slot == rt::graphics_record::kStageSlotVs, "decode: slot 1 is the vertex shader");
    check(record.stages[2].program.size == 0 && record.stages[2].stage_blob.size == 0,
          "decode: slot 2 (mesh) stays empty");
    check(record.root_signature == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1122334455667788ull)),
          "decode: the root signature pointer value is copied at +0x00");
    check(record.state.data == fixture.state.data() && record.state.size == fixture.state.size(),
          "decode: the state span is +0x08/+0x10");
    check(record.input_layout.data == fixture.layout.data() && record.input_layout.size == fixture.layout.size(),
          "decode: the input-layout span is +0x18/+0x20");
    check(record.stages[0].program.data == fixture.ps_program.data() &&
              record.stages[0].program.size == fixture.ps_program.size(),
          "decode: the PS program span is +0x28/+0x30");
    check(record.stages[0].stage_blob.data == fixture.ps_blob.data() &&
              record.stages[0].stage_blob.size == fixture.ps_blob.size(),
          "decode: the PS stage blob span is +0x38/+0x40");
    check(record.stages[1].program.data == fixture.vs_program.data() &&
              record.stages[1].program.size == fixture.vs_program.size(),
          "decode: the VS program span is +0x48/+0x50");
    check(record.stages[1].stage_blob.data == fixture.vs_blob.data() &&
              record.stages[1].stage_blob.size == fixture.vs_blob.size(),
          "decode: the VS stage blob span is +0x58/+0x60");

    // The unidentified tail beyond +0x108 is ignored, so exactly the validated region is enough.
    check(rt::graphics_record::decode(fixture.descriptor.data(), rt::graphics_record::kDescriptorValidatedBytes,
                                      &record),
          "decode: a +0x108-byte read is enough; the tail is not required");
}

void test_decode_does_not_dereference() {
    // Every blob pair gets a pointer value that is not mapped in this process. decode() only copies the pairs out; if
    // it ever reads through one of them this test faults instead of the shipped decoder crashing on a bad record.
    const Fixture fixture;
    Bytes desc = fixture.descriptor;
    const std::uint64_t fake_pointers[6] = {0x10000, 0x20000, 0x30000, 0x40000, 0x50000, 0x60000};
    const std::size_t pair_offsets[6] = {0x08, 0x18, 0x28, 0x38, 0x48, 0x58};
    for (std::size_t pair = 0; pair < 6; ++pair) put_u64(&desc, pair_offsets[pair], fake_pointers[pair]);
    Record record;
    check(rt::graphics_record::decode(desc.data(), desc.size(), &record),
          "decode: a record of fake pointers decodes without being dereferenced");
    check(record.state.data == reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(0x10000)),
          "decode: the fake state pointer is copied verbatim");
}

void test_empty_input_layout() {
    // docs/shader_library.md: a mesh pipeline's input layout is 80 bytes with no elements.
    Bytes layout(80, 0);
    std::vector<Element> elements;
    check(rt::graphics_record::decode_input_layout(Span{layout.data(), layout.size()}, &elements),
          "layout: an 80-byte mesh layout decodes");
    check(elements.empty(), "layout: the mesh layout has no elements");

    const Fixture fixture;
    Bytes desc = fixture.descriptor;
    put_span_pair(&desc, 0x18, layout);
    Record record;
    check(rt::graphics_record::decode(desc.data(), desc.size(), &record), "decode: an 80-byte input layout is accepted");
    check(record.input_layout.size == 80, "decode: the mesh layout's size is kept");
}

void test_state() {
    const Fixture fixture;
    State state;
    check(rt::graphics_record::decode_state(Span{fixture.state.data(), fixture.state.size()}, &state),
          "state: the 388-byte measured-container sample decodes");
    check(state.size == 388 && state.raw.size() == 388 / 4,
          "state: the size is kept and every word is copied");
    check(state.family == rt::graphics_record::StateFamily::kVertex, "state: 388 B is the vertex family");
    check(state.stage_block_bytes == rt::graphics_record::kStateVertexStageBlockBytes &&
              state.common_block_bytes == rt::graphics_record::kStateCommonBlockBytes,
          "state: the vertex stage block is 76 B and the shared block 168 B");
    check(state.variable_offset == 0x11cu && state.variable_bytes == 104 && state.variable_item_count == 1 &&
              state.variable_payload_bytes == 76,
          "state: k = 1 puts a 104-byte variable region at 0x11C (28 header + 76 payload)");
    check(state.header_word00 == 0x4005d808u, "state: +0x00 is kept with its measured flag bits");
    check(state.item_count_word == 0x00020042u, "state: +0x0C is the item count << 1 with its measured flags");

    // The words are kept verbatim and are not interpreted: the measured sample words and a later state word.
    const std::uint32_t measured[5] = {0x4005d808u, 0x00000004u, 0x00100060u, 0x00020042u, 0xb50516a0u};
    bool measured_kept = true;
    for (std::size_t word = 0; word < 5; ++word) measured_kept = measured_kept && state.raw[word] == measured[word];
    check(measured_kept, "state: the measured sample words are kept verbatim");
    check(state.raw[0x150 / 4] == 0x40, "state: the 0x40 word at +0x150 is kept raw, not interpreted");

    // All fourteen measured size shapes decode: the vertex family 380 + 8*k (k 0..5) and the mesh family
    // 464 + 8*k (k 0..7). The item count, the variable-region offset and the payload follow the size.
    const std::size_t shapes[14] = {380, 388, 396, 404, 412, 420, 464, 472, 480, 488, 496, 504, 512, 520};
    for (const std::size_t size : shapes) {
        const Bytes blob = make_state_shape(size);
        State decoded;
        check(rt::graphics_record::decode_state(Span{blob.data(), blob.size()}, &decoded),
              "state: a measured size shape decodes");
        const bool mesh = size >= rt::graphics_record::kStateMeshFamilyBaseBytes;
        const std::size_t base = mesh ? rt::graphics_record::kStateMeshFamilyBaseBytes
                                      : rt::graphics_record::kStateVertexFamilyBaseBytes;
        const std::size_t stage_bytes = mesh ? rt::graphics_record::kStateMeshStageBlockBytes
                                             : rt::graphics_record::kStateVertexStageBlockBytes;
        const std::size_t items = (size - base) / rt::graphics_record::kStateFamilyStepBytes;
        check(decoded.size == size && decoded.family == (mesh ? rt::graphics_record::StateFamily::kMesh
                                                              : rt::graphics_record::StateFamily::kVertex) &&
                  decoded.stage_block_bytes == stage_bytes && decoded.variable_item_count == items &&
                  decoded.variable_offset == rt::graphics_record::kStateHeaderBytes + stage_bytes +
                                                 rt::graphics_record::kStateCommonBlockBytes &&
                  decoded.variable_payload_bytes ==
                      rt::graphics_record::kStateVariablePayloadBaseBytes +
                          rt::graphics_record::kStateFamilyStepBytes * items &&
                  decoded.variable_bytes == rt::graphics_record::kStateVariableHeaderBytes + decoded.variable_payload_bytes,
              "state: the shape's family, stage block, item count, variable region and payload follow the measurement");
    }

    // Sizes inside the measured 380..520 envelope that no measured state has: the 421..463 gap between the families,
    // and a size off a family's 8-byte step.
    const std::size_t no_family[4] = {424, 436, 452, 460};
    for (const std::size_t size : no_family) {
        const Bytes blob(size, 0);
        check(!rt::graphics_record::decode_state(Span{blob.data(), blob.size()}, &state),
              "state: a size in the 421..463 gap between the two measured families is refused");
    }
    const std::size_t outside[3] = {376, 379, 524};
    for (const std::size_t size : outside) {
        const Bytes blob(size, 0);
        check(!rt::graphics_record::decode_state(Span{blob.data(), blob.size()}, &state),
              "state: a size outside 380..520 is refused");
    }
    Bytes not_words(382, 0);
    check(!rt::graphics_record::decode_state(Span{not_words.data(), not_words.size()}, &state),
          "state: a size that is not a whole number of words is refused");

    // Each measured header word is a check: one word away from the measurement is a shape the decoder was not
    // measured on, so it refuses instead of keeping a word it cannot explain.
    struct Broken {
        std::size_t at;
        std::uint32_t value;
        const char* what;
    };
    const Broken broken[] = {
        {0x00, 0x4005dc00u, "state: +0x00 outside 0x4005D800's measured flag bits is refused"},
        {0x04, rt::graphics_record::kStateStageWordMesh,
         "state: the mesh stage word on a vertex-family blob is refused"},
        {0x08, 0x00100061u, "state: +0x08 off the measured constant is refused"},
        {0x0c, 0x00020044u, "state: +0x0C whose count nibble disagrees with the size step is refused"},
        {0x0c, 0x00030042u, "state: +0x0C with an unmeasured flag set is refused"},
        {0x10, 0xb50516a1u, "state: +0x10 off the measured constant is refused"},
        {0x28, 0x004c011du, "state: +0x28 off the measured variable-region pair is refused"},
    };
    for (const Broken& bad : broken) {
        Bytes blob = make_state_shape(388);
        put_u32(&blob, bad.at, bad.value);
        check(!rt::graphics_record::decode_state(Span{blob.data(), blob.size()}, &state), bad.what);
    }

    check(!rt::graphics_record::decode_state(Span{nullptr, 388}, &state), "state: a null blob is refused");
    check(!rt::graphics_record::decode_state(Span{fixture.state.data(), fixture.state.size()}, nullptr),
          "state: a null output is refused");
}

void test_input_layout() {
    // The measured 188-byte shape: the count at +0x4C, elements from +0x50, each a u32 name length including the NUL,
    // the name bytes and a 24-byte attribute block; no padding (80 + 37 + 34 + 37 = 188).
    const Fixture fixture;
    std::vector<Element> elements;
    check(rt::graphics_record::decode_input_layout(Span{fixture.layout.data(), fixture.layout.size()}, &elements),
          "layout: the synthetic 188-byte measured shape decodes");
    check(elements.size() == 3, "layout: three elements are decoded");
    check(elements.size() == 3 && elements[0].semantic == "POSITION" && elements[1].semantic == "COLOR" &&
              elements[2].semantic == "TEXCOORD",
          "layout: the length-prefixed semantics are decoded in element order");
    check(elements.size() == 3 && elements[0].semantic_offset == 0x54 && elements[1].semantic_offset == 0x79 &&
              elements[2].semantic_offset == 0x9B,
          "layout: the name offsets are the measured +0x54/+0x79/+0x9B");
    check(elements.size() == 3 && elements[0].name_bytes == 9 && elements[1].name_bytes == 6 &&
              elements[2].name_bytes == 9,
          "layout: the stored length words include the NUL terminator");
    const std::uint32_t formats[3] = {6, 28, 16};
    const std::uint32_t offsets[3] = {0, 12, 16};
    bool attributes = elements.size() == 3;
    for (std::size_t index = 0; attributes && index < elements.size(); ++index) {
        for (std::size_t word = 0; word < elements[index].attribute.size(); ++word) {
            const std::uint32_t expected = word == 1 ? formats[index] : word == 3 ? offsets[index] : 0;
            attributes = attributes && elements[index].attribute[word] == expected;
        }
    }
    check(attributes, "layout: the six attribute words are kept verbatim (word 1 the format, word 3 the offset)");

    // Every measured shape decodes, and its size is the element records plus zero padding to a 32-bit boundary.
    const Bytes shapes[] = {make_layout_120(), make_layout_152(), make_layout_156(), make_layout_188(),
                            make_layout_224()};
    const std::size_t shape_elements[] = {1, 2, 2, 3, 4};
    const std::size_t shape_bytes[] = {120, 152, 156, 188, 224};
    for (std::size_t shape = 0; shape < 5; ++shape) {
        std::vector<Element> decoded;
        check(rt::graphics_record::decode_input_layout(Span{shapes[shape].data(), shapes[shape].size()}, &decoded),
              "layout: a measured shape decodes");
        check(decoded.size() == shape_elements[shape] && shapes[shape].size() == shape_bytes[shape],
              "layout: the measured element count and size are kept");
    }

    // An 80-byte blob is the measured empty layout; it has no records to pad.
    Bytes empty(80, 0);
    check(rt::graphics_record::decode_input_layout(Span{empty.data(), empty.size()}, &elements),
          "layout: an 80-byte empty layout decodes");
    check(elements.empty(), "layout: the empty layout has no elements");

    // The contiguous-names form the decoder used to accept is not this title's blob: from +0x50 the decoder reads a
    // length word, and the name bytes are not a valid length-prefixed record, so it refuses rather than pairing a name
    // with bytes whose encoding is not measured.
    Bytes contiguous(188, 0);
    put_u32(&contiguous, 0x4c, 3);
    std::memcpy(contiguous.data() + 0x50, "POSITION\0COLOR\0TEXCOORD\0", 24);
    check(!rt::graphics_record::decode_input_layout(Span{contiguous.data(), contiguous.size()}, &elements),
          "layout: the refuted contiguous-names form is refused");

    // Size envelope and the 32-bit word shape.
    const std::size_t rejected_sizes[3] = {79, 187, 341};
    for (const std::size_t size : rejected_sizes) {
        Bytes blob(size, 0);
        check(!rt::graphics_record::decode_input_layout(Span{blob.data(), blob.size()}, &elements),
              "layout: a size outside 80..340 or not a whole number of words is refused");
    }

    // The count against the bytes: too high runs past the blob, too low leaves the records unfilled.
    Bytes four = fixture.layout;
    put_u32(&four, 0x4c, 4);
    check(!rt::graphics_record::decode_input_layout(Span{four.data(), four.size()}, &elements),
          "layout: a count above the records the bytes hold is refused");
    Bytes two = fixture.layout;
    put_u32(&two, 0x4c, 2);
    check(!rt::graphics_record::decode_input_layout(Span{two.data(), two.size()}, &elements),
          "layout: a count below the records the bytes hold is refused");

    // A name without its NUL: POSITION's terminator at +0x5C becomes a printable byte.
    Bytes unterminated = fixture.layout;
    unterminated[0x5c] = 'X';
    check(!rt::graphics_record::decode_input_layout(Span{unterminated.data(), unterminated.size()}, &elements),
          "layout: a name without its NUL is refused");

    // An unprintable byte inside a name, and a length word outside the measured bound.
    Bytes binary_name = fixture.layout;
    binary_name[0x55] = 0x01;
    check(!rt::graphics_record::decode_input_layout(Span{binary_name.data(), binary_name.size()}, &elements),
          "layout: an unprintable name byte is refused");
    Bytes zero_length = fixture.layout;
    put_u32(&zero_length, 0x50, 0);
    check(!rt::graphics_record::decode_input_layout(Span{zero_length.data(), zero_length.size()}, &elements),
          "layout: a zero name length is refused");
    Bytes long_name = fixture.layout;
    put_u32(&long_name, 0x50, 0xffffffffu);
    check(!rt::graphics_record::decode_input_layout(Span{long_name.data(), long_name.size()}, &elements),
          "layout: a name length past the measured bound is refused");

    // The padding is part of the shape: it must be inside the 4-byte boundary and zero.
    Bytes dirty_pad = make_layout_120();
    dirty_pad.back() = 0x01;
    check(!rt::graphics_record::decode_input_layout(Span{dirty_pad.data(), dirty_pad.size()}, &elements),
          "layout: a non-zero padding byte is refused");
    Bytes slack = fixture.layout;
    slack.resize(slack.size() + 4);
    check(!rt::graphics_record::decode_input_layout(Span{slack.data(), slack.size()}, &elements),
          "layout: bytes after the padding (slack) are refused");

    // Count 0 with bytes after the header: those bytes have no element to belong to.
    Bytes stray(84, 0);
    check(!rt::graphics_record::decode_input_layout(Span{stray.data(), stray.size()}, &elements),
          "layout: an empty count with trailing bytes is refused");

    // A count that would need more element records than the envelope can hold is refused before it is used.
    Bytes huge = fixture.layout;
    put_u32(&huge, 0x4c, 0xffffffffu);
    check(!rt::graphics_record::decode_input_layout(Span{huge.data(), huge.size()}, &elements),
          "layout: an absurd element count is refused");

    check(!rt::graphics_record::decode_input_layout(Span{nullptr, 188}, &elements), "layout: a null blob is refused");
    check(!rt::graphics_record::decode_input_layout(Span{fixture.layout.data(), fixture.layout.size()}, nullptr),
          "layout: a null output is refused");
}

void test_stage_blob() {
    const Fixture fixture;
    StageBlob blob;
    check(rt::graphics_record::decode_stage_blob(Span{fixture.ps_blob.data(), fixture.ps_blob.size()}, &blob),
          "stage blob: the synthetic kind-2 blob decodes");
    check(blob.kind == 2, "stage blob: the kind word is 2");
    check(blob.head_tag == 0x40, "stage blob: the +0x0C head word is the measured 0x40 tag");
    check(blob.head_word_08 == 0x0000abcdu, "stage blob: the +0x08 head word is kept raw");
    check(blob.declared_size_word + rt::graphics_record::kStageBlobSizeBias == fixture.ps_blob.size(),
          "stage blob: the size word keeps the measured +12 relation");
    check(blob.records.size() == 2, "stage blob: the two 0x41/0x42 records are decoded");
    check(blob.records[0].tag == 0x41 && blob.records[1].tag == 0x42, "stage blob: record tags are kept in order");
    check(blob.records[0].body[0] == 0x11000000u && blob.records[1].body[6] == 0x11000106u,
          "stage blob: record bodies are kept verbatim");
    check(blob.source_digest.bytes[0] == 0xa1 && blob.source_digest.bytes[15] == 0xb0,
          "stage blob: the 16-byte source digest after 0x43 is kept");

    StageBlob vs;
    check(rt::graphics_record::decode_stage_blob(Span{fixture.vs_blob.data(), fixture.vs_blob.size()}, &vs),
          "stage blob: a single-record stage blob decodes");
    check(vs.records.size() == 1 && vs.records[0].tag == 0x41, "stage blob: the single 0x41 record is decoded");

    Bytes too_small(0x20, 0);
    put_u32(&too_small, 0x00, 2);
    check(!rt::graphics_record::decode_stage_blob(Span{too_small.data(), too_small.size()}, &blob),
          "stage blob: a blob shorter than head + one record is refused");

    Bytes misaligned = fixture.ps_blob;
    misaligned.push_back(0);
    put_u32(&misaligned, 0x04, static_cast<std::uint32_t>(misaligned.size() - 12));
    check(!rt::graphics_record::decode_stage_blob(Span{misaligned.data(), misaligned.size()}, &blob),
          "stage blob: a size that is not 0x10 + 32*k is refused");

    Bytes kind3 = fixture.ps_blob;
    put_u32(&kind3, 0x00, 3);
    check(!rt::graphics_record::decode_stage_blob(Span{kind3.data(), kind3.size()}, &blob),
          "stage blob: kind 3 (the compute layout blob) is refused");

    Bytes bad_size = fixture.ps_blob;
    put_u32(&bad_size, 0x04, static_cast<std::uint32_t>(bad_size.size() - 16));
    check(!rt::graphics_record::decode_stage_blob(Span{bad_size.data(), bad_size.size()}, &blob),
          "stage blob: a declared size that disagrees with the span is refused");

    Bytes bad_head = fixture.ps_blob;
    put_u32(&bad_head, 0x0c, 0x41);
    check(!rt::graphics_record::decode_stage_blob(Span{bad_head.data(), bad_head.size()}, &blob),
          "stage blob: a head word that is not the 0x40 tag is refused");

    const Bytes bad_order = make_stage_blob({0x42, 0x41, 0x43}, 3);
    check(!rt::graphics_record::decode_stage_blob(Span{bad_order.data(), bad_order.size()}, &blob),
          "stage blob: a 0x41 record after a 0x42 record is refused");

    const Bytes no_digest = make_stage_blob({0x41, 0x42}, 4);
    check(!rt::graphics_record::decode_stage_blob(Span{no_digest.data(), no_digest.size()}, &blob),
          "stage blob: a blob without the final 0x43 record is refused");

    const Bytes digest_not_last = make_stage_blob({0x43, 0x41}, 5);
    check(!rt::graphics_record::decode_stage_blob(Span{digest_not_last.data(), digest_not_last.size()}, &blob),
          "stage blob: a 0x43 record that is not last is refused");

    const Bytes unknown_tag = make_stage_blob({0x44, 0x43}, 6);
    check(!rt::graphics_record::decode_stage_blob(Span{unknown_tag.data(), unknown_tag.size()}, &blob),
          "stage blob: an unknown record tag is refused");

    Bytes dirty_tail = fixture.ps_blob;
    dirty_tail.back() = 0x01;
    check(!rt::graphics_record::decode_stage_blob(Span{dirty_tail.data(), dirty_tail.size()}, &blob),
          "stage blob: a non-zero byte in the final 12 zero bytes is refused");

    check(!rt::graphics_record::decode_stage_blob(Span{nullptr, 0x30}, &blob), "stage blob: a null blob is refused");
    check(!rt::graphics_record::decode_stage_blob(Span{fixture.ps_blob.data(), fixture.ps_blob.size()}, nullptr),
          "stage blob: a null output is refused");
}

void test_record_malformed() {
    const Fixture fixture;
    Record record;

    check(!rt::graphics_record::decode(nullptr, 0x120, &record), "decode: a null record is refused");
    check(!rt::graphics_record::decode(fixture.descriptor.data(), fixture.descriptor.size(), nullptr),
          "decode: a null output is refused");
    check(!rt::graphics_record::decode(fixture.descriptor.data(), 0x107, &record),
          "decode: a read shorter than +0x108 is refused");

    // No stage at all.
    Bytes no_stages = fixture.descriptor;
    for (std::size_t at = 0x28; at < 0x88; ++at) no_stages[at] = 0;
    check(!rt::graphics_record::decode(no_stages.data(), no_stages.size(), &record),
          "decode: a record without stages is refused");

    // The state span.
    Bytes bad_state = fixture.descriptor;
    put_u64(&bad_state, 0x10, 379);
    check(!rt::graphics_record::decode(bad_state.data(), bad_state.size(), &record),
          "decode: a state size below the measured range is refused");
    bad_state = fixture.descriptor;
    put_u64(&bad_state, 0x10, 521);
    check(!rt::graphics_record::decode(bad_state.data(), bad_state.size(), &record),
          "decode: a state size above the measured range is refused");
    bad_state = fixture.descriptor;
    put_u64(&bad_state, 0x08, 0);
    check(!rt::graphics_record::decode(bad_state.data(), bad_state.size(), &record),
          "decode: a null state pointer is refused");
    bad_state = fixture.descriptor;
    put_u64(&bad_state, 0x08, 0);
    put_u64(&bad_state, 0x10, 0);
    check(!rt::graphics_record::decode(bad_state.data(), bad_state.size(), &record),
          "decode: a missing state pair is refused");

    // The input-layout span.
    Bytes bad_layout = fixture.descriptor;
    put_u64(&bad_layout, 0x20, 79);
    check(!rt::graphics_record::decode(bad_layout.data(), bad_layout.size(), &record),
          "decode: an input-layout size below the measured range is refused");
    bad_layout = fixture.descriptor;
    put_u64(&bad_layout, 0x20, 341);
    check(!rt::graphics_record::decode(bad_layout.data(), bad_layout.size(), &record),
          "decode: an input-layout size above the measured range is refused");

    // Half-set stage pairs.
    Bytes half_pair = fixture.descriptor;
    put_u64(&half_pair, 0x30, 0);
    check(!rt::graphics_record::decode(half_pair.data(), half_pair.size(), &record),
          "decode: a program pointer without a size is refused");
    half_pair = fixture.descriptor;
    put_u64(&half_pair, 0x28, 0);
    check(!rt::graphics_record::decode(half_pair.data(), half_pair.size(), &record),
          "decode: a program size without a pointer is refused");
    half_pair = fixture.descriptor;
    put_u64(&half_pair, 0x40, 0);
    check(!rt::graphics_record::decode(half_pair.data(), half_pair.size(), &record),
          "decode: a stage-blob pointer without a size is refused");

    // Program and stage-blob sizes outside the measured shape.
    Bytes bad_program = fixture.descriptor;
    put_u64(&bad_program, 0x30, 51);
    check(!rt::graphics_record::decode(bad_program.data(), bad_program.size(), &record),
          "decode: a program size below 52 bytes is refused");
    bad_program = fixture.descriptor;
    put_u64(&bad_program, 0x30, 239465);
    check(!rt::graphics_record::decode(bad_program.data(), bad_program.size(), &record),
          "decode: a program size above 239,464 bytes is refused");
    Bytes bad_blob = fixture.descriptor;
    put_u64(&bad_blob, 0x40, 0x2f);
    check(!rt::graphics_record::decode(bad_blob.data(), bad_blob.size(), &record),
          "decode: a stage blob shorter than 0x30 is refused");
    bad_blob = fixture.descriptor;
    put_u64(&bad_blob, 0x40, 0x31);
    check(!rt::graphics_record::decode(bad_blob.data(), bad_blob.size(), &record),
          "decode: a stage blob that is not 0x10 + 32*k is refused");

    // The measured zero region after the slot pairs.
    Bytes dirty_tail = fixture.descriptor;
    dirty_tail[0x90] = 0x01;
    check(!rt::graphics_record::decode(dirty_tail.data(), dirty_tail.size(), &record),
          "decode: a non-zero byte in the +0x88..+0x108 reserved region is refused");

    // Measured stage combinations: no record has a vertex and a mesh stage together; mesh + pixel is measured.
    Bytes vs_and_ms = fixture.descriptor;
    put_span_pair(&vs_and_ms, 0x68, fixture.ps_program);  // slot 2 program
    put_span_pair(&vs_and_ms, 0x78, fixture.vs_blob);     // slot 2 stage blob
    check(!rt::graphics_record::decode(vs_and_ms.data(), vs_and_ms.size(), &record),
          "decode: a vertex and a mesh stage together are refused");

    Bytes ms_and_ps = vs_and_ms;
    for (std::size_t at = 0x48; at < 0x68; ++at) ms_and_ps[at] = 0;  // drop the VS slot
    const Bytes mesh_state = make_state_shape(464);  // the measured mesh family
    put_span_pair(&ms_and_ps, 0x08, mesh_state);
    check(rt::graphics_record::decode(ms_and_ps.data(), ms_and_ps.size(), &record),
          "decode: a pixel and a mesh stage pair is accepted");
    check(record.stage_count == 2 && record.stages[1].slot == rt::graphics_record::kStageSlotMs,
          "decode: the mesh stage is slot 2");

    // The state blob's size family names the record's programmable stage (docs/state_blob.md): a mesh record whose
    // state is the vertex family, and a vertex record whose state is the mesh family, are both refused.
    Bytes mesh_with_vertex_state = ms_and_ps;
    put_span_pair(&mesh_with_vertex_state, 0x08, make_state_shape(388));
    check(!rt::graphics_record::decode(mesh_with_vertex_state.data(), mesh_with_vertex_state.size(), &record),
          "decode: a mesh record with a vertex-family state size is refused");
    Bytes vertex_with_mesh_state = fixture.descriptor;
    put_span_pair(&vertex_with_mesh_state, 0x08, mesh_state);
    check(!rt::graphics_record::decode(vertex_with_mesh_state.data(), vertex_with_mesh_state.size(), &record),
          "decode: a vertex record with a mesh-family state size is refused");
    // A state size inside the measured envelope that is neither family (the 421..463 gap) is refused at the record.
    Bytes gap_state = fixture.descriptor;
    put_span_pair(&gap_state, 0x08, Bytes(424, 0));
    check(!rt::graphics_record::decode(gap_state.data(), gap_state.size(), &record),
          "decode: a state size in the 421..463 gap is refused at the record");

    // A pixel-only record is a subset the measurements do not forbid.
    Bytes ps_only = fixture.descriptor;
    for (std::size_t at = 0x48; at < 0x68; ++at) ps_only[at] = 0;
    check(rt::graphics_record::decode(ps_only.data(), ps_only.size(), &record),
          "decode: a pixel-only record is accepted");
    check(record.stage_count == 1, "decode: the pixel-only record has one stage");
}

// The decoded/unknown boundary, printed for the next reader.
void print_findings() {
    struct Finding {
        const char* field;
        const char* status;
        const char* evidence;
    };
    const Finding findings[] = {
        {"Record.root_signature", "decoded (opaque pointer)",
         "+0x00 pointer value; not validated and not dereferenced"},
        {"Record.state ptr/size", "decoded (range-checked)",
         "+0x08/+0x10; 380..520 B (docs/pso_cache.md); words kept raw"},
        {"Record.input_layout ptr/size", "decoded (range-checked)",
         "+0x18/+0x20; 80..340 B; a mesh layout is 80 B (docs/shader_library.md)"},
        {"Record.stages[i].slot", "decoded",
         "record order: 0 PS, 1 VS, 2 MS (docs/shader_library.md section 2)"},
        {"Record.stages[i].program", "decoded (span only)",
         "native program bytes; key = SHA-256 as passed (docs/boot_frontier.md)"},
        {"Record.stages[i].stage_blob", "decoded (span only)",
         "kind-2 blob envelope, contents via decode_stage_blob"},
        {"record +0x88..+0x108", "decoded (must be zero)",
         "every measured sample is zero to +0x108; a non-zero byte is refused"},
        {"record tail +0x108..", "unknown",
         "unidentified tail; not represented, ignored by decode()"},
        {"State container", "decoded", "the measured state-record container: two size families, stage/common blocks, "
                                       "the variable region and its pair (docs/state_blob.md)"},
        {"State family", "decoded (cross-checked)", "380 + 8*k = vertex family, 464 + 8*k = mesh family; the record's "
                                                    "VS/MS slots must agree (all 25,471 measured records do)"},
        {"State.variable_item_count", "decoded (meaning unknown)", "k = 0..5 (vertex) / 0..7 (mesh); the item count "
                                                                   "word at +0x0C is k << 1 with measured flags"},
        {"State.raw words", "unknown (kept raw)",
         "every word of the blob, the 0x40 word at +0x150 included; no fixed-function meaning measured"},
        {"fixed-function state", "unknown (refused by name)",
         "rasterizer/blend/depth_stencil/topology_type/strip_cut/sample_count/sample_quality/sample_mask: "
         "graphics_pso::decode_fixed_function_state refuses all eight"},
        {"InputLayout.element count", "decoded", "u32 at +0x4C (all six measured shapes, docs/input_layout.md)"},
        {"InputLayout element table", "decoded",
         "from +0x50: u32 name length incl NUL, the name, a 24-byte attribute block; zero pad to a 4-byte boundary"},
        {"InputLayout.semantic", "decoded", "length-prefixed, NUL-terminated printable names in element order"},
        {"InputLayout attribute block", "decoded, kept verbatim (6 u32 words)",
         "the D3D12 element fields in order: word 0 SemanticIndex, 1 Format, 2 InputSlot, 3 AlignedByteOffset, "
         "4 InputSlotClass, 5 InstanceDataStepRate"},
        {"InputLayout format word", "decoded raw (word 1); DXGI mapping in graphics_pso",
         "measured 2/6/16/28/30 = R32G32B32A32_FLOAT/R32G32B32_FLOAT/R32G32_FLOAT/R8G8B8A8_UNORM/R8G8B8A8_UINT"},
        {"InputLayout semantic index", "decoded raw (word 0)",
         "0 on every one- or two-element sample; 2 on the 224-byte shape's second COLOR (COLOR2)"},
        {"InputLayout aligned byte offset", "decoded raw (word 3)",
         "the measured layouts are packed, so it equals the bytes the preceding elements occupy"},
        {"InputLayout input slot / class / step rate", "decoded raw (words 2, 4, 5)",
         "0 in every measured element; graphics_pso::build refuses any other value by name"},
        {"StageBlob.kind", "decoded", "u32 at +0x00 == 2 (docs/shader_library.md section 3)"},
        {"StageBlob.declared_size_word", "decoded (relation)", "u32 at +0x04 + 12 == blob size (measured)"},
        {"StageBlob.head_word_08", "unknown (kept raw)", "u32 at +0x08; not decoded"},
        {"StageBlob.head_tag", "decoded",
         "u32 at +0x0C == 0x40 (20,318 blobs; tools/shader_library.py stage_digest)"},
        {"StageBlob records", "decoded tags / unknown bodies",
         "0x41 then 0x42 32-byte records; seven body words kept raw"},
        {"StageBlob.source_digest", "decoded (opaque)",
         "16 bytes after the final 0x43 tag: the source container's digest"},
        {"StageBlob trailing 12 bytes", "decoded (must be zero)", "measured zero in all 20,318 blobs"},
    };
    std::printf("graphics_record_test: decoded/unknown boundary for the slot-57 record\n");
    std::printf("%-32s %-28s %s\n", "field", "status", "evidence");
    for (const Finding& finding : findings)
        std::printf("%-32s %-28s %s\n", finding.field, finding.status, finding.evidence);
    std::printf("\n");
}

}  // namespace

int main() {
    print_findings();

    test_decode_success();
    test_decode_does_not_dereference();
    test_empty_input_layout();
    test_state();
    test_input_layout();
    test_stage_blob();
    test_record_malformed();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s) of %d check(s)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("graphics_record_test: all %d checks passed\n", g_checks);
    return 0;
}
