// Clean-room emulation of AMD SSE4a EXTRQ / INSERTQ on an Intel host (round-22 menus push).
//
// The title's session-13 half-float packing path executes the AMD-only
//
//   INSERTQ xmm1, xmm2, imm8(length), imm8(index)      F2 [REX] 0F 78 /r ib ib
//   INSERTQ xmm1, xmm2                                 F2 [REX] 0F 79 /r
//   EXTRQ   xmm1, imm8(length), imm8(index)            66 [REX] 0F 78 /0 ib ib
//   EXTRQ   xmm1, xmm2                                 66 [REX] 0F 79 /r
//
// on its frame thread; an Intel host raises STATUS_ILLEGAL_INSTRUCTION at image+0x2E7FF1 (measured). This header decodes
// the instruction from its bytes and applies the documented operation to the 64-bit low halves of two XMM registers.
// The instruction fact sheet is the AMD64 Architecture Programmer's Manual vol. 4; the implementation below is ours.
// Only register-direct ModRM forms exist (there is no memory operand). Semantics:
//   * length and index are masked to 6 bits; a length of zero means 64;
//   * INSERTQ writes the low `length` bits of xmm2[63:0] into xmm1[63:0] at bit `index`, keeping the other bits;
//   * EXTRQ replaces xmm1[63:0] with (xmm1[63:0] >> index) & ((1<<length)-1);
//   * xmm1[127:64] is undefined by the ISA and is left untouched;
//   * a field that would cross bit 64 is refused (the ISA leaves it undefined) so the caller can report it;
//   * the register forms take length = control[5:0], index = control[13:8], where control is xmm2[63:0] for EXTRQ and
//     xmm2[127:64] (the high half) for INSERTQ.
#pragma once

#include <cstddef>
#include <cstdint>

namespace palmetto::sse4a {

enum class Op : std::uint8_t { extract, insert };

struct Instruction {
    Op op = Op::extract;
    bool register_form = false;  // 0F 79: length/index come from the source register
    unsigned destination = 0;    // xmm register number 0-15
    unsigned source = 0;         // xmm register number 0-15 (equals destination for EXTRQ imm)
    std::uint8_t length = 0;     // raw imm8 (immediate forms only)
    std::uint8_t index = 0;      // raw imm8 (immediate forms only)
    std::size_t size = 0;        // encoded byte length (4, 5, 6 or 7)
};

struct Field {
    unsigned length = 0;
    unsigned index = 0;
};

inline bool decode(const std::uint8_t* bytes, std::size_t available, Instruction& out) noexcept {
    if (!bytes || available < 4) return false;
    std::size_t at = 0;
    const std::uint8_t prefix = bytes[at++];
    if (prefix != 0x66 && prefix != 0xF2) return false;
    std::uint8_t rex = 0;
    if ((bytes[at] & 0xF0) == 0x40) {
        if (at + 1 >= available) return false;
        rex = bytes[at++];
    }
    if (at + 3 > available) return false;
    if (bytes[at++] != 0x0F) return false;
    const std::uint8_t opcode = bytes[at++];
    if (opcode != 0x78 && opcode != 0x79) return false;
    const std::uint8_t modrm = bytes[at++];
    if ((modrm >> 6) != 3) return false;  // register-direct only
    const unsigned reg_field = (modrm >> 3) & 7u;
    const unsigned rm_field = modrm & 7u;
    const unsigned reg = reg_field | ((rex & 0x4) ? 8u : 0u);
    const unsigned rm = rm_field | ((rex & 0x1) ? 8u : 0u);
    Instruction result;
    result.op = prefix == 0x66 ? Op::extract : Op::insert;
    result.register_form = opcode == 0x79;
    if (opcode == 0x78) {
        if (at + 2 > available) return false;
        result.length = bytes[at++];
        result.index = bytes[at++];
        if (result.op == Op::extract) {
            if (reg_field != 0) return false;  // /0: the reg field is an opcode extension
            result.destination = result.source = rm;
        } else {
            result.destination = reg;
            result.source = rm;
        }
    } else {
        result.destination = reg;
        result.source = rm;
    }
    result.size = at;
    out = result;
    return true;
}

inline Field resolve(const Instruction& instruction, std::uint64_t source_low,
                     std::uint64_t source_high) noexcept {
    Field field;
    if (instruction.register_form) {
        const std::uint64_t control = instruction.op == Op::extract ? source_low : source_high;
        field.length = static_cast<unsigned>(control & 0x3Fu);
        field.index = static_cast<unsigned>((control >> 8) & 0x3Fu);
    } else {
        field.length = instruction.length & 0x3Fu;
        field.index = instruction.index & 0x3Fu;
    }
    if (field.length == 0) field.length = 64;
    return field;
}

inline std::uint64_t field_mask(unsigned length) noexcept {
    return length >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << length) - 1);
}

// Applies the operation to the destination's low qword. Returns false when the field would cross bit 64.
inline bool apply(const Instruction& instruction, const Field& field, std::uint64_t source_low,
                  std::uint64_t& destination_low) noexcept {
    if (field.index + field.length > 64) return false;
    const std::uint64_t mask = field_mask(field.length);
    if (instruction.op == Op::extract) {
        destination_low = (destination_low >> field.index) & mask;
    } else {
        const std::uint64_t hole = mask << field.index;
        const std::uint64_t bits = (source_low & mask) << field.index;
        destination_low = (destination_low & ~hole) | bits;
    }
    return true;
}

}  // namespace palmetto::sse4a