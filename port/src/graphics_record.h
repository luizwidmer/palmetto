// The console's deserialized graphics pipeline record (device slot 57), decoded into the pieces a PC
// D3D12_GRAPHICS_PIPELINE_STATE_DESC needs. Pure decoding over byte spans: this file creates no device object, links
// nothing and never reads through a pointer taken out of the record.
//
// The record is the expansion of a pipeline-library graphics record (docs/boot_frontier.md, "slot 57 is a deserialized
// graphics pipeline record"; docs/shader_library.md section 2):
//
//   +0x00  root signature pointer
//   +0x08  fixed-function state blob pointer       +0x10  size (380..520 bytes)
//   +0x18  input-layout blob pointer               +0x20  size (80..340 bytes)
//   +0x28  slot 0 (PS) native program pointer      +0x30  size
//   +0x38  slot 0 (PS) kind-2 stage blob pointer   +0x40  size
//   +0x48  slot 1 (VS) native program pointer      +0x50  size
//   +0x58  slot 1 (VS) kind-2 stage blob pointer   +0x60  size
//   +0x68  slot 2 (MS) native program pointer      +0x70  size
//   +0x78  slot 2 (MS) kind-2 stage blob pointer   +0x80  size
//
// Unused slots are all zero. Everything after the last used pair is zero to +0x108; the tail from +0x108 is
// unidentified and this decoder ignores it. The record slot names the stage - the stage blob itself carries no stage
// identity (docs/boot_frontier.md; docs/shader_library.md section 2, "The slot gives the stage").
//
// Decoded vs unknown (docs/graphics_record.md has the field table with the evidence for every row):
//   decoded: the six pointer/size pairs and their offsets, which slot/stage each pair is, the record's measured size
//            envelope (state and input-layout ranges, program size range, kind-2 blob record envelope), the state
//            blob's measured container (its two size families, the stage-block/common-block boundaries and the
//            varying tail, docs/state_blob.md), the input layout's 0x50-byte header, its element count and its
//            per-element records (a length-prefixed semantic name and a 24-byte attribute block of six words), and the
//            stage blob's kind word, size word, 0x40 head tag, 0x41/0x42/0x43 record tags, 16-byte source digest and
//            the 12 zero bytes after it.
//   unknown, kept raw: every fixed-function word of the state blob (its container decodes; no rasterizer, blend,
//            depth-stencil, topology, sample or attachment field is measured, docs/state_blob.md), the meaning of the
//            input layout's attribute words (this file keeps all six verbatim; graphics_pso::build decides word 1 is the
//            format and word 3 the aligned byte offset from the measured values, docs/input_layout.md), and the stage
//            blob's head words and 0x41/0x42 record bodies. They stay unknown on purpose: a guessed format or a
//            guessed state produces a wrong PSO, which is far harder to find than a refusal.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rt::graphics_record {

// A non-owning byte range. decode() copies these out of the record but never dereferences them: the pointer comes from
// a deserialized console record, so readability of the pointed-to bytes is the caller's guarantee, not this file's.
struct Span {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// Stage slots, in record order: slot 0 pixel, slot 1 vertex, slot 2 mesh (docs/shader_library.md section 2).
inline constexpr int kStageSlotPs = 0;
inline constexpr int kStageSlotVs = 1;
inline constexpr int kStageSlotMs = 2;
inline constexpr std::size_t kStageSlotCount = 3;

// "ps", "vs", "ms", or "unused" for a slot number.
const char* stage_slot_name(int slot);

// The measured shape of the record. All numbers are offsets into the record's own bytes or measured size ranges;
// they are checks, not decoded values.
inline constexpr std::size_t kRootSignatureOffset = 0x00;
inline constexpr std::size_t kStatePairOffset = 0x08;      // pointer at +0x08, size at +0x10
inline constexpr std::size_t kInputLayoutPairOffset = 0x18;  // pointer at +0x18, size at +0x20
inline constexpr std::size_t kStagePairOffset = 0x28;      // slot 0 program pair; +0x10 per pair; +0x20 per slot
inline constexpr std::size_t kProgramToBlobPairOffset = 0x10;  // a slot's stage-blob pair follows its program pair
inline constexpr std::size_t kStagePairStride = 0x20;
inline constexpr std::size_t kPairPointerBytes = 0x08;
inline constexpr std::size_t kDescriptorValidatedBytes = 0x108;  // zero-filled region ends here; tail is unknown
inline constexpr std::size_t kDescriptorTailOffset = kDescriptorValidatedBytes;

inline constexpr std::size_t kStateMinBytes = 380;  // docs/pso_cache.md, "stage record (sizes 380..520)"
inline constexpr std::size_t kStateMaxBytes = 520;
inline constexpr std::size_t kInputLayoutMinBytes = 80;   // docs/shader_library.md: a mesh pipeline's layout is 80 B
inline constexpr std::size_t kInputLayoutMaxBytes = 340;

// Library programs, docs/shader_library.md section 2: "52 to 239,464 bytes".
inline constexpr std::size_t kProgramMinBytes = 52;
inline constexpr std::size_t kProgramMaxBytes = 239464;

// Kind-2 stage blob envelope (docs/shader_library.md section 3; tooling/shader_library.py stage_digest).
inline constexpr std::uint32_t kStageBlobKind = 2;
inline constexpr std::size_t kStageBlobHeadBytes = 0x10;    // kind word + size word + 0x08 word + 0x40 tag
inline constexpr std::size_t kStageBlobRecordBytes = 32;
inline constexpr std::size_t kStageBlobMinBytes = kStageBlobHeadBytes + kStageBlobRecordBytes;
inline constexpr std::uint32_t kStageBlobHeadTag = 0x40;    // word at head-4, checked by tooling/shader_library.py
inline constexpr std::uint32_t kStageBlobSizeBias = 12;     // size word + this == blob size (measured; meaning open)
inline constexpr std::size_t kStageBlobDigestBytes = 16;
inline constexpr std::size_t kStageBlobTrailingZeroBytes = 12;
inline constexpr std::uint32_t kStageBlobRecordTagA = 0x41;
inline constexpr std::uint32_t kStageBlobRecordTagB = 0x42;
inline constexpr std::uint32_t kStageBlobDigestTag = 0x43;

// Input layout (docs/input_layout.md). Measured on the six boot shapes (80 B/count 0, 120/1, 152/2, 156/2, 188/3,
// 224/4): a 0x50-byte header with the element count at +0x4C, then one record per element, back to back from +0x50:
//
//   +0x50  u32 name length INCLUDING the NUL terminator, then that many name bytes, then a 24-byte attribute block
//
// and the blob's bytes are the records' bytes, zero-padded to a 32-bit boundary:
//
//   size == 0x50 + sum(4 + name_len + 24) rounded up to a multiple of 4
//
// The same sizes also fit "all names first, then count*28 attribute bytes" (4 + name_len + 24 == name_len + 28), but
// that reading needs the header to end at +0x54 while every measured first name starts at +0x54 from the +0x50 length
// word, so it leaves four bytes unexplained and the six measured sizes refute it. The 24-byte attribute block's six
// words are kept verbatim here; graphics_pso::build decides their field meaning from the measured values.
inline constexpr std::size_t kInputLayoutHeaderBytes = 0x50;
inline constexpr std::size_t kInputLayoutCountOffset = 0x4C;
inline constexpr std::size_t kInputLayoutNameLengthBytes = 4;  // u32 before every name
inline constexpr std::size_t kInputLayoutNameMinBytes = 2;     // shortest length this decoder accepts: 1 char + NUL
inline constexpr std::size_t kInputLayoutNameMaxBytes = 64;    // a bound; the measured names are 6 and 9 bytes
inline constexpr std::size_t kInputLayoutAttributeWords = 6;
inline constexpr std::size_t kInputLayoutAttributeBytes = kInputLayoutAttributeWords * 4;
inline constexpr std::size_t kInputLayoutMinElementBytes =
    kInputLayoutNameLengthBytes + kInputLayoutNameMinBytes + kInputLayoutAttributeBytes;
// The most elements the measured 80..340-byte envelope can hold. It bounds the count before anything is allocated.
inline constexpr std::size_t kInputLayoutMaxElements =
    (kInputLayoutMaxBytes - kInputLayoutHeaderBytes) / kInputLayoutMinElementBytes;

// ---- The decoded record ------------------------------------------------------------------------------------------

struct Stage {
    int slot = kStageSlotPs;
    Span program;     // native RDNA2 program; the shader index keys it by SHA-256 of these bytes as passed
    Span stage_blob;  // kind-2 stage blob; decode_stage_blob() validates its contents
};

struct Record {
    void* root_signature = nullptr;  // +0x00, the pointer value as passed; not validated and not dereferenced
    Span state;                      // +0x08/+0x10: fixed-function state blob (decode_state)
    Span input_layout;               // +0x18/+0x20: input-layout blob (decode_input_layout)
    Stage stages[kStageSlotCount];
    std::size_t stage_count = 0;
};

// Parses the record's own bytes. `available` must cover kDescriptorValidatedBytes; the record's tail beyond that is
// ignored. Never dereferences `root_signature`, the blob pointers or the program pointers: the blob decoders take the
// spans this fills in. Returns false on any inconsistency (short read, a size outside the measured range, a state size
// in a measured envelope that is neither measured family or that disagrees with the record's programmable stage - the
// vertex family is 380 + 8*k and the mesh family 464 + 8*k, docs/state_blob.md - a half-set pair, a non-zero unused
// pair, a non-zero byte where the measurement says zero, zero stages, or vertex and mesh stages together).
bool decode(const void* descriptor, std::size_t available, Record* out);

// ---- The input layout -------------------------------------------------------------------------------------------

// One input-layout element, in stored order (docs/input_layout.md). decode_input_layout() decodes the semantic name
// and keeps the six-word attribute block verbatim. The words' field meaning is a decision graphics_pso::build makes
// from the measured values: they are the D3D12_INPUT_ELEMENT_DESC fields in their D3D12 order with the name pointer
// replaced by the stored name - word 0 SemanticIndex (measured: 2 on a COLOR element, i.e. COLOR2), word 1 Format,
// word 2 InputSlot (0), word 3 AlignedByteOffset, word 4 InputSlotClass (0, per-vertex), word 5 InstanceDataStepRate
// (0). build() maps word 1 through the measured DXGI table and refuses any other value by name.
struct Element {
    std::string semantic;              // the printable name, without its NUL terminator
    std::size_t semantic_offset = 0;   // offset of the NUL-terminated name in the blob (a stable SemanticName)
    std::uint32_t name_bytes = 0;      // the stored length word: name bytes including the NUL terminator
    std::array<std::uint32_t, kInputLayoutAttributeWords> attribute{};  // the six attribute words, verbatim
};

// Decodes the input-layout blob: element count at +0x4C, then `count` records from +0x50, each a u32 name length
// including the NUL, the name bytes and a 24-byte attribute block; the records' bytes are zero-padded to a 4-byte
// boundary, so the blob must be filled exactly (no slack, no overflow). Returns false when the size is outside
// 80..340 bytes or not a whole number of words, when the count cannot fit the envelope, when a length word or a name
// is not the measured shape (length 2..64, NUL-terminated, printable ASCII), when the padding is not zero, or when the
// records do not fill the blob exactly. The attribute words are not interpreted here: this file is pure bytes.
bool decode_input_layout(Span blob, std::vector<Element>* out);

// ---- The fixed-function state blob ------------------------------------------------------------------------------

// The 380..520-byte blob is the console driver's serialized graphics-pipeline state record (docs/state_blob.md). This
// decoder proves its container and copies the words out; it decodes no rasterizer, blend, depth-stencil, sample,
// topology, strip-cut or attachment field, because no word's meaning is pinned by a measurement of this title's data.
//
// Measured on this title's whole library (5,680 distinct state blobs, referenced by 25,471 graphics records):
//
//   * two size families: a vertex family 380 + 8*k, k = 0..5, and a mesh family 464 + 8*k, k = 0..7. The family is the
//     record's programmable stage: every one of the 13,409 vertex-family records names a VS slot and every one of the
//     12,062 mesh-family records names an MS slot (and no record names the other).
//   * the fixed layout is 40 header bytes, then a stage block - 76 bytes for the vertex family, 160 for the mesh
//     family - then a 168-byte block shared by both families, then the variable region. So the variable region starts
//     at 0x11C for the vertex family and 0x170 for the mesh family.
//   * +0x28 holds (variable payload bytes << 16) | variable-region offset; the payload is size - offset - 28, so the
//     variable region is 28 header bytes plus 68 + 8*k payload bytes, and the whole blob is filled exactly.
//   * the header words are the same in every measured blob apart from the varying tail's count and three flag sets:
//     +0x00 is 0x4005D800 with only bits 3 and 4 varying, +0x04 is 4 (vertex family) or 6 (mesh family), +0x08 is
//     0x00100060, +0x0C is (the item count << 1) with flag bits {0, 0x20040, 0x400C0}, and +0x10 is 0xB50516A0.
//
// What the variable items and the flag bits mean is not measured, so they are kept raw; see docs/state_blob.md for the
// histogram of what this rule decodes and what it refuses.
enum class StateFamily {
    kVertex,
    kMesh,
};

// "vertex" or "mesh": the stage family the state record's size names.
const char* state_family_name(StateFamily family);

// The measured shape of the state record. All numbers are offsets into the blob or measured sizes; they are checks,
// not decoded state values.
inline constexpr std::size_t kStateVertexFamilyBaseBytes = 380;  // 380 + 8*k
inline constexpr std::size_t kStateVertexFamilyMaxItems = 5;
inline constexpr std::size_t kStateMeshFamilyBaseBytes = 464;  // 464 + 8*k
inline constexpr std::size_t kStateMeshFamilyMaxItems = 7;
inline constexpr std::size_t kStateFamilyStepBytes = 8;  // one varying-tail item
inline constexpr std::size_t kStateHeaderBytes = 40;    // +0x00..+0x28
inline constexpr std::size_t kStateVertexStageBlockBytes = 76;  // +0x28..+0x74
inline constexpr std::size_t kStateMeshStageBlockBytes = 160;   // +0x28..+0xC8
inline constexpr std::size_t kStateCommonBlockBytes = 168;      // then 0x74..0x11C (vertex) or 0xC8..0x170 (mesh)
inline constexpr std::size_t kStateVariableHeaderBytes = 28;    // the variable region's own header
inline constexpr std::size_t kStateVariablePayloadBaseBytes = 68;  // plus 8 per item
inline constexpr std::size_t kStateVariablePairOffset = 0x28;   // (payload << 16) | variable-region offset
inline constexpr std::size_t kStateItemCountOffset = 0x0C;      // item count << 1, plus flags
inline constexpr std::uint32_t kStateHeaderWord00Base = 0x4005D800u;   // +0x00: only bits 3 and 4 vary
inline constexpr std::uint32_t kStateHeaderWord00Flags = 0x18u;
inline constexpr std::uint32_t kStateHeaderWord08 = 0x00100060u;  // +0x08
inline constexpr std::uint32_t kStateHeaderWord10 = 0xB50516A0u;  // +0x10
inline constexpr std::uint32_t kStateStageWordVertex = 4;         // +0x04, vertex family
inline constexpr std::uint32_t kStateStageWordMesh = 6;           // +0x04, mesh family
inline constexpr std::uint32_t kStateItemCountMask = 0x0Fu;       // the measured item count is << 1
inline constexpr std::uint32_t kStateItemCountFlags[3] = {0x0u, 0x20040u, 0x400C0u};  // measured flag sets

// The measured 380..520-byte state blob, as its container is decoded (docs/state_blob.md). `raw` keeps every word
// verbatim: the decoded fields are the container's shape, not the fixed-function state. The converter must not infer
// rasterizer/blend/depth-stencil/sample/topology/attachment state from these words; graphics_pso::
// decode_fixed_function_state() refuses all eight fields by name.
struct State {
    static constexpr std::size_t kMinBytes = kStateMinBytes;
    static constexpr std::size_t kMaxBytes = kStateMaxBytes;
    std::size_t size = 0;                          // the blob's own size
    StateFamily family = StateFamily::kVertex;     // the size family, which names the record's programmable stage
    std::size_t stage_block_bytes = kStateVertexStageBlockBytes;  // +0x28..: 76 (vertex) or 160 (mesh)
    std::size_t common_block_bytes = kStateCommonBlockBytes;      // the block shared by both families
    std::size_t variable_offset = 0;               // where the variable region starts (0x11C or 0x170)
    std::size_t variable_bytes = 0;                // size - variable_offset (96 + 8*k)
    std::size_t variable_item_count = 0;           // k; what an item means is not decoded
    std::size_t variable_payload_bytes = 0;        // 68 + 8*k
    std::uint32_t item_count_word = 0;             // +0x0C verbatim: the item count << 1 with the measured flag bits
    std::uint32_t header_word00 = 0;               // +0x00 verbatim: 0x4005D800 with the measured flag bits
    std::vector<std::uint32_t> raw;                // every word of the blob; no fixed-function field is decoded
};

// Validates the measured container exactly - the size family and its step, the header words, the item-count word, the
// variable-region pair, and that the blob is filled to its end with 28 + 68 + 8*k variable bytes - then copies the
// words out. Returns false for any other shape (including a size in the measured 380..520 envelope that no measured
// state has), naming nothing: the caller refuses the record. The fixed-function fields stay undecoded.
bool decode_state(Span blob, State* out);

// ---- The kind-2 stage blob --------------------------------------------------------------------------------------

// A 0x41/0x42 record: the tag is decoded, the seven body words are not (the field meanings in docs/pso_cache.md are
// unproven there and are not adopted here).
struct ResourceRecord {
    std::uint32_t tag = 0;       // decoded: 0x41 or 0x42
    std::uint32_t body[7] = {};  // unknown, kept verbatim
};

// The 16 bytes after the 0x43 tag: the source container's own digest (docs/shader_library.md section 3). Kept as
// opaque identity bytes; this decoder does not verify it against any container.
struct SourceDigest {
    std::array<std::uint8_t, kStageBlobDigestBytes> bytes{};
};

struct StageBlob {
    std::uint32_t kind = 0;               // decoded: 2
    std::uint32_t declared_size_word = 0; // raw +0x04 word; decoded relation: + 12 == blob size
    std::uint32_t head_word_08 = 0;       // unknown, kept verbatim
    std::uint32_t head_tag = 0;           // decoded: +0x0C word == 0x40
    std::vector<ResourceRecord> records;  // 0x41 records then 0x42 records, in stored order
    SourceDigest source_digest;           // decoded: the 16 bytes after the 0x43 tag
};

// Decodes the kind-2 stage blob envelope: kind word 2, the measured size relation, the 0x40 head tag, 0x41 then 0x42
// records, a final 0x43 record whose digest is kept and whose 12 trailing bytes must be zero. Record bodies stay
// unknown. Returns false for any other kind, an unknown tag, a broken tag order, a size that disagrees with the size
// word, or a non-zero trailing byte.
bool decode_stage_blob(Span blob, StageBlob* out);

}  // namespace rt::graphics_record
