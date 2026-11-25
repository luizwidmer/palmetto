#include "graphics_record.h"

#include <cstring>
#include <utility>

namespace rt::graphics_record {
namespace {

std::uint32_t load_u32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof value);
    return value;
}

std::uint64_t load_u64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof value);
    return value;
}

const std::uint8_t* pointer_from(std::uint64_t value) {
    return reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(value));
}

bool in_range(std::uint64_t value, std::size_t low, std::size_t high) {
    return value >= low && value <= high;
}

struct Pair {
    std::uint64_t pointer = 0;
    std::uint64_t size = 0;
};

Pair read_pair(const std::uint8_t* record, std::size_t at) {
    return Pair{load_u64(record + at), load_u64(record + at + kPairPointerBytes)};
}

Span span_of(Pair pair) {
    return Span{pointer_from(pair.pointer), static_cast<std::size_t>(pair.size)};
}

bool pair_is_empty(Pair pair) {
    return (pair.pointer | pair.size) == 0;
}

}  // namespace

const char* stage_slot_name(int slot) {
    switch (slot) {
        case kStageSlotPs: return "ps";
        case kStageSlotVs: return "vs";
        case kStageSlotMs: return "ms";
        default: return "unused";
    }
}

bool has_mesh_stage(const Record& record) {
    for (std::size_t i = 0; i < record.stage_count && i < kStageSlotCount; ++i)
        if (record.stages[i].slot == kStageSlotMs) return true;
    return false;
}

RecordShape record_shape(const Record& record) {
    bool ps = false, vs = false;
    for (std::size_t i = 0; i < record.stage_count && i < kStageSlotCount; ++i) {
        if (record.stages[i].slot == kStageSlotPs) ps = true;
        if (record.stages[i].slot == kStageSlotVs) vs = true;
    }
    if (has_mesh_stage(record)) return ps ? RecordShape::kPixelMesh : RecordShape::kMesh;
    if (record.has_unidentified_stage) return RecordShape::kPixelVertexSlot6;
    return vs ? (ps ? RecordShape::kPixelVertex : RecordShape::kVertex) : RecordShape::kPixelVertex;
}

const char* record_shape_name(RecordShape shape) {
    switch (shape) {
        case RecordShape::kPixelVertex: return "ps+vs";
        case RecordShape::kVertex: return "vs";
        case RecordShape::kPixelMesh: return "ps+ms";
        case RecordShape::kMesh: return "ms";
        case RecordShape::kPixelVertexSlot6: return "ps+vs+slot6";
    }
    return "ps+vs";
}

const char* state_family_name(StateFamily family) {
    return family == StateFamily::kVertex ? "vertex" : "mesh";
}

bool decode(const void* descriptor, std::size_t available, Record* out) {
    if (!out) return false;
    if (!descriptor) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(descriptor);
    if (available < kDescriptorValidatedBytes) return false;

    Record record;
    // +0x00: the root signature pointer. Kept as the pointer value that was passed; nothing dereferences or validates
    // it, and a zero here is not interpreted (the measurements do not prove a null root signature impossible).
    record.root_signature =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(load_u64(bytes + kRootSignatureOffset)));

    // The two measured blobs the record always carries. Size ranges are the measured ones: a record outside them is
    // not the shape this decoder understands, so it is refused rather than passed on with a size it was not given.
    const Pair state = read_pair(bytes, kStatePairOffset);
    const Pair layout = read_pair(bytes, kInputLayoutPairOffset);
    if (state.pointer == 0 || state.size == 0 || !in_range(state.size, kStateMinBytes, kStateMaxBytes)) return false;
    if (state.size % 4 != 0) return false;
    if (layout.pointer == 0 || layout.size == 0 || !in_range(layout.size, kInputLayoutMinBytes, kInputLayoutMaxBytes))
        return false;
    record.state = span_of(state);
    record.input_layout = span_of(layout);

    // The three slot pairs, in record order. A slot is absent when both of its pairs are zero, present when both are
    // fully set; anything in between is inconsistent. `stages` is filled in record order and each entry keeps the
    // record slot number, which is its stage identity.
    for (std::size_t slot = 0; slot < kStageSlotCount; ++slot) {
        const std::size_t at = kStagePairOffset + slot * kStagePairStride;
        const Pair program = read_pair(bytes, at);
        const Pair blob = read_pair(bytes, at + kProgramToBlobPairOffset);
        const bool program_empty = pair_is_empty(program);
        const bool blob_empty = pair_is_empty(blob);
        if (program_empty && blob_empty) continue;  // slot unused
        if (program_empty || blob_empty) return false;
        if (program.pointer == 0 || program.size == 0) return false;
        if (!in_range(program.size, kProgramMinBytes, kProgramMaxBytes)) return false;
        if (blob.pointer == 0 || blob.size == 0) return false;
        // The stage blob's measured envelope: 16 head bytes plus whole 32-byte records, one of them the 0x43 digest
        // record, so its size is 0x10 + 32*k and at least 0x30. A size that does not fit is refused here, before any
        // byte of the blob is read.
        if (blob.size < kStageBlobMinBytes) return false;
        if ((blob.size - kStageBlobHeadBytes) % kStageBlobRecordBytes != 0) return false;
        Stage& stage = record.stages[record.stage_count];
        stage.slot = static_cast<int>(slot);
        stage.program = span_of(program);
        stage.stage_blob = span_of(blob);
        ++record.stage_count;
    }
    if (record.stage_count == 0) return false;

    // Measured stage combinations of a library graphics record (docs/shader_library.md section 2: VS+PS, MS+PS, MS
    // only, VS only): no record has a vertex stage and a mesh stage together, and D3D12 forbids that pair as well.
    auto has_stage = [&record](int slot) {
        for (std::size_t i = 0; i < record.stage_count; ++i)
            if (record.stages[i].slot == slot) return true;
        return false;
    };
    if (has_stage(kStageSlotVs) && has_stage(kStageSlotMs)) return false;

    // The state blob's size family names the record's programmable stage, and the measurements agree on all 25,471
    // records: the vertex family (380 + 8*k) only on records with a VS slot, the mesh family (464 + 8*k) only on
    // records with an MS slot. A size in the envelope that is neither family, or one that disagrees with the slots, is
    // a record shape this decoder was not measured on (docs/state_blob.md).
    const bool state_vertex_family =
        state.size >= kStateVertexFamilyBaseBytes &&
        state.size <= kStateVertexFamilyBaseBytes + kStateFamilyStepBytes * kStateVertexFamilyMaxItems &&
        (state.size - kStateVertexFamilyBaseBytes) % kStateFamilyStepBytes == 0;
    const bool state_mesh_family =
        state.size >= kStateMeshFamilyBaseBytes &&
        state.size <= kStateMeshFamilyBaseBytes + kStateFamilyStepBytes * kStateMeshFamilyMaxItems &&
        (state.size - kStateMeshFamilyBaseBytes) % kStateFamilyStepBytes == 0;
    if (!state_vertex_family && !state_mesh_family) return false;
    if (has_stage(kStageSlotVs) && !state_vertex_family) return false;
    if (has_stage(kStageSlotMs) && !state_mesh_family) return false;

    // Everything after the three slot pairs is zero to +0x108 in every measured sample, with one measured exception:
    // the slot-6 program pair of 2 library records (one program, no stage blob; PS+VS+slot6, docs/mesh_stage.md).
    // Slots 3 to 5 and the slot-6 stage-blob pair are zero in every measured sample, so any other non-zero byte is a
    // shape this decoder was not measured on and is refused rather than silently ignored.
    constexpr std::size_t reserved_begin = kStagePairOffset + kStageSlotCount * kStagePairStride;
    constexpr std::size_t unidentified_blob_pair = kUnidentifiedProgramPairOffset + kProgramToBlobPairOffset;
    for (std::size_t at = reserved_begin; at < kUnidentifiedProgramPairOffset; ++at)
        if (bytes[at] != 0) return false;
    const Pair unidentified = read_pair(bytes, kUnidentifiedProgramPairOffset);
    if (!pair_is_empty(unidentified)) {
        // The measured slot-6 shape: a fully set program pair with a size in the measured program range, no stage
        // blob, and a record that also names the vertex stage (both measured slot-6 records are PS+VS+slot6). The
        // stage is not identified by any measurement, so it is kept as an opaque span and is not a Stage here; the
        // converter refuses it by name. A partial pair, a blob, a size outside the range or a slot set the
        // measurement never shows is refused.
        if (unidentified.pointer == 0 || unidentified.size == 0) return false;
        if (!in_range(unidentified.size, kProgramMinBytes, kProgramMaxBytes)) return false;
        if (!has_stage(kStageSlotVs) || has_stage(kStageSlotMs)) return false;
        record.has_unidentified_stage = true;
        record.unidentified_program = span_of(unidentified);
    }
    for (std::size_t at = unidentified_blob_pair; at < kDescriptorValidatedBytes; ++at)
        if (bytes[at] != 0) return false;

    *out = record;
    return true;
}

bool decode_input_layout(Span blob, std::vector<Element>* out) {
    if (!out) return false;
    out->clear();
    if (!blob.data) return false;
    if (blob.size < kInputLayoutMinBytes || blob.size > kInputLayoutMaxBytes) return false;
    if (blob.size % 4 != 0) return false;  // every measured blob is a whole number of 32-bit words

    const std::uint32_t count = load_u32(blob.data + kInputLayoutCountOffset);
    // Each element needs its length word, at least one printable name byte plus the NUL and its attribute block. The
    // bound comes from the blob that is actually there, before anything is allocated from the count.
    if (count > kInputLayoutMaxElements) return false;

    std::vector<Element> elements(count);
    std::size_t position = kInputLayoutHeaderBytes;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (position + kInputLayoutNameLengthBytes > blob.size) return false;
        const std::uint32_t name_bytes = load_u32(blob.data + position);
        position += kInputLayoutNameLengthBytes;
        // The measured length words are 6 (COLOR) and 9 (POSITION, TEXCOORD). A shorter one cannot carry a name; a
        // longer one is outside the measured shape, so the element boundary this decoder assumes is not the blob's.
        if (name_bytes < kInputLayoutNameMinBytes || name_bytes > kInputLayoutNameMaxBytes) return false;
        if (position + name_bytes + kInputLayoutAttributeBytes > blob.size) return false;
        if (blob.data[position + name_bytes - 1] != 0) return false;  // the name must end in its own NUL
        for (std::uint32_t byte = 0; byte + 1 < name_bytes; ++byte) {
            const std::uint8_t value = blob.data[position + byte];
            if (value < 0x20 || value > 0x7e) return false;  // a measured name is printable ASCII
        }
        Element& element = elements[index];
        element.semantic.assign(reinterpret_cast<const char*>(blob.data + position), name_bytes - 1);
        element.semantic_offset = position;
        element.name_bytes = name_bytes;
        position += name_bytes;
        for (std::size_t word = 0; word < kInputLayoutAttributeWords; ++word)
            element.attribute[word] = load_u32(blob.data + position + 4 * word);
        position += kInputLayoutAttributeBytes;
    }

    // Exact fill: the element records end at `position` and the blob continues only with zero padding to the next
    // 32-bit boundary. A remainder of any other size, a non-zero padding byte, or slack after the padding means the
    // element boundary is not the measured one, so nothing is returned rather than pairing names with bytes whose
    // encoding is not known.
    const std::size_t padded = (position + 3) & ~static_cast<std::size_t>(3);
    if (padded != blob.size) return false;
    for (std::size_t at = position; at < blob.size; ++at)
        if (blob.data[at] != 0) return false;

    *out = std::move(elements);
    return true;
}

bool decode_state(Span blob, State* out) {
    if (!out) return false;
    *out = State{};
    if (!blob.data) return false;
    if (blob.size < kStateMinBytes || blob.size > kStateMaxBytes) return false;
    if (blob.size % 4 != 0) return false;  // the blob is read as 32-bit words in the measurements

    // The size family, and the item count its size step gives. 421..463 bytes is inside the measured envelope but is
    // no measured state shape (the two families are 380 + 8*k, k = 0..5, and 464 + 8*k, k = 0..7), so it is refused
    // rather than fitted to whichever family it is nearer.
    State state;
    std::size_t base = 0;
    if (blob.size <= kStateVertexFamilyBaseBytes + kStateFamilyStepBytes * kStateVertexFamilyMaxItems) {
        state.family = StateFamily::kVertex;
        state.stage_block_bytes = kStateVertexStageBlockBytes;
        base = kStateVertexFamilyBaseBytes;
    } else if (blob.size >= kStateMeshFamilyBaseBytes) {
        state.family = StateFamily::kMesh;
        state.stage_block_bytes = kStateMeshStageBlockBytes;
        base = kStateMeshFamilyBaseBytes;
    } else {
        return false;
    }
    if ((blob.size - base) % kStateFamilyStepBytes != 0) return false;
    const std::size_t items = (blob.size - base) / kStateFamilyStepBytes;
    const std::size_t max_items =
        state.family == StateFamily::kVertex ? kStateVertexFamilyMaxItems : kStateMeshFamilyMaxItems;
    if (items > max_items) return false;

    // The variable region follows the same layout in both families: 40 header bytes, the family's stage block, the
    // 168-byte shared block, then 28 + 68 + 8*k bytes, which is the blob's end exactly.
    const std::size_t variable_offset =
        kStateHeaderBytes + state.stage_block_bytes + kStateCommonBlockBytes;
    const std::size_t payload_bytes =
        blob.size - variable_offset - kStateVariableHeaderBytes;  // 68 + 8*k when filled exactly
    if (payload_bytes != kStateVariablePayloadBaseBytes + kStateFamilyStepBytes * items) return false;

    // The header words every measured blob carries. A deviating word is a shape this decoder was not measured on, so
    // it refuses instead of keeping a word the measurement does not explain.
    const std::uint32_t word00 = load_u32(blob.data + 0x00);
    if ((word00 & ~kStateHeaderWord00Flags) != kStateHeaderWord00Base) return false;
    const std::uint32_t stage_word = load_u32(blob.data + 0x04);
    const std::uint32_t expected_stage_word =
        state.family == StateFamily::kVertex ? kStateStageWordVertex : kStateStageWordMesh;
    if (stage_word != expected_stage_word) return false;
    if (load_u32(blob.data + 0x08) != kStateHeaderWord08) return false;
    const std::uint32_t count_word = load_u32(blob.data + kStateItemCountOffset);
    // The measured item-count word is (count * 2) in its low nibble plus one of three measured flag sets. 2*k fits the
    // nibble for k = 0..7, which is both families' bound.
    if ((count_word & kStateItemCountMask) != static_cast<std::uint32_t>(items * 2)) return false;
    const std::uint32_t count_flags = count_word & ~kStateItemCountMask;
    bool known_flags = false;
    for (const std::uint32_t flags : kStateItemCountFlags) known_flags = known_flags || count_flags == flags;
    if (!known_flags) return false;
    if (load_u32(blob.data + 0x10) != kStateHeaderWord10) return false;

    // +0x28: the variable region's offset in the high 16 bits and its payload length in the low ones, as the measured
    // pair spells it: (payload << 16) | offset.
    const std::uint32_t pair = load_u32(blob.data + kStateVariablePairOffset);
    const std::uint32_t expected_pair =
        (static_cast<std::uint32_t>(payload_bytes) << 16) | static_cast<std::uint32_t>(variable_offset);
    if (pair != expected_pair) return false;

    state.size = blob.size;
    state.variable_offset = variable_offset;
    state.variable_bytes = blob.size - variable_offset;
    state.variable_item_count = items;
    state.variable_payload_bytes = payload_bytes;
    state.item_count_word = count_word;
    state.header_word00 = word00;
    state.raw.resize(blob.size / 4);
    std::memcpy(state.raw.data(), blob.data, blob.size);
    *out = std::move(state);
    return true;
}

bool decode_stage_blob(Span blob, StageBlob* out) {
    if (!out) return false;
    *out = StageBlob{};
    if (!blob.data) return false;
    if (blob.size < kStageBlobMinBytes) return false;
    if ((blob.size - kStageBlobHeadBytes) % kStageBlobRecordBytes != 0) return false;

    const std::uint32_t kind = load_u32(blob.data);
    if (kind != kStageBlobKind) return false;  // kind 3 is the compute layout blob, not this record's stage blob
    const std::uint32_t declared_size = load_u32(blob.data + 4);
    // Measured relation: the word at +0x04 plus 12 is the blob's own size. The 12 is measured, not explained, so it is
    // used only as a consistency check.
    if (static_cast<std::uint64_t>(declared_size) + kStageBlobSizeBias != blob.size) return false;
    const std::uint32_t head_tag = load_u32(blob.data + kStageBlobHeadBytes - 4);
    if (head_tag != kStageBlobHeadTag) return false;

    StageBlob stage;
    stage.kind = kind;
    stage.declared_size_word = declared_size;
    stage.head_word_08 = load_u32(blob.data + 8);  // unknown; kept verbatim
    stage.head_tag = head_tag;

    const std::size_t record_count = (blob.size - kStageBlobHeadBytes) / kStageBlobRecordBytes;
    bool second_kind_seen = false;
    bool digest_seen = false;
    for (std::size_t index = 0; index < record_count; ++index) {
        const std::size_t at = kStageBlobHeadBytes + index * kStageBlobRecordBytes;
        const std::uint32_t tag = load_u32(blob.data + at);
        if (tag == kStageBlobRecordTagA) {
            if (second_kind_seen) return false;  // measured order: all 0x41, then all 0x42, then one 0x43
        } else if (tag == kStageBlobRecordTagB) {
            second_kind_seen = true;
        } else if (tag == kStageBlobDigestTag) {
            // The digest record is the last one and is 32 bytes: tag, 16 digest bytes, 12 zero bytes.
            if (index + 1 != record_count) return false;
            std::memcpy(stage.source_digest.bytes.data(), blob.data + at + 4, kStageBlobDigestBytes);
            for (std::size_t zero = at + 4 + kStageBlobDigestBytes; zero < at + kStageBlobRecordBytes; ++zero)
                if (blob.data[zero] != 0) return false;
            digest_seen = true;
            continue;
        } else {
            return false;  // a tag no measurement covers: refuse rather than guess what follows
        }
        ResourceRecord record;
        record.tag = tag;
        for (std::size_t word = 0; word < 7; ++word) record.body[word] = load_u32(blob.data + at + 4 + 4 * word);
        stage.records.push_back(record);
    }
    if (!digest_seen) return false;

    *out = std::move(stage);
    return true;
}

}  // namespace rt::graphics_record
