// Synthetic tests for the clean-room SSE4a emulation (port/src/sse4a_emulation.h). No host state, no game data.
#include "../src/sse4a_emulation.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what);
    }
}

bool emulate(const std::uint8_t* bytes, std::size_t size, std::uint64_t src_low, std::uint64_t src_high,
             std::uint64_t& dest_low) {
    using namespace palmetto::sse4a;
    Instruction instruction;
    if (!decode(bytes, size, instruction)) return false;
    const Field field = resolve(instruction, src_low, src_high);
    return apply(instruction, field, src_low, dest_low);
}

}  // namespace

int main() {
    using namespace palmetto::sse4a;
    // Observed at the title fault site: INSERTQ xmm0, xmm1, length 0x10, index 0x10 -> F2 0F 78 C1 10 10.
    const std::uint8_t insert_imm[] = {0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x10};
    Instruction instruction;
    check(decode(insert_imm, sizeof(insert_imm), instruction), "decode insert imm");
    check(instruction.op == Op::insert && !instruction.register_form, "insert imm kind");
    check(instruction.destination == 0 && instruction.source == 1, "insert imm registers");
    check(instruction.length == 0x10 && instruction.index == 0x10, "insert imm fields");
    check(instruction.size == 6, "insert imm size");
    std::uint64_t dest = 0x1111111111111111ull;
    check(emulate(insert_imm, sizeof(insert_imm), 0xABCDull, 0, dest), "insert imm applies");
    check(dest == 0x11111111ABCD1111ull, "insert writes low 16 bits at bit 16");

    // Length zero means 64.
    const std::uint8_t insert_len0[] = {0xF2, 0x0F, 0x78, 0xC1, 0x00, 0x00};
    dest = 0x5555555555555555ull;
    check(emulate(insert_len0, sizeof(insert_len0), 0x0123456789ABCDEFull, 0, dest), "insert length 0 applies");
    check(dest == 0x0123456789ABCDEFull, "length 0 inserts 64 bits");

    // Register form: INSERTQ xmm0, xmm1 -> F2 0F 79 C1; length = high[5:0], index = high[13:8].
    const std::uint8_t insert_reg[] = {0xF2, 0x0F, 0x79, 0xC1};
    check(decode(insert_reg, sizeof(insert_reg), instruction), "decode insert reg");
    check(instruction.register_form && instruction.destination == 0 && instruction.source == 1, "insert reg operands");
    const std::uint64_t control = (static_cast<std::uint64_t>(0x10) << 8) | 0x10;
    dest = 0xFFFF00000000FFFFull;
    check(emulate(insert_reg, sizeof(insert_reg), 0, control, dest), "insert reg applies");
    check(dest == 0xFFFF00000000FFFFull, "insert reg writes the same 16 bits");

    // EXTRQ xmm1, length, index: 66 0F 78 C0 10 08 (reg field 0, rm 0) extracts bits 8..23 into the low qword.
    const std::uint8_t extract_imm[] = {0x66, 0x0F, 0x78, 0xC0, 0x10, 0x08};
    check(decode(extract_imm, sizeof(extract_imm), instruction), "decode extract imm");
    check(instruction.op == Op::extract && instruction.length == 0x10 && instruction.index == 0x08, "extract imm fields");
    dest = 0x00000000ABCDEF00ull;
    check(emulate(extract_imm, sizeof(extract_imm), 0, 0, dest), "extract imm applies");
    check(dest == 0x000000000000CDEFull, "extract keeps the field only");

    // Register form: 66 0F 79 C0 with control low = length | index<<8.
    const std::uint8_t extract_reg[] = {0x66, 0x0F, 0x79, 0xC0};
    check(decode(extract_reg, sizeof(extract_reg), instruction), "decode extract reg");
    dest = 0x00000000ABCDEF00ull;
    check(emulate(extract_reg, sizeof(extract_reg), control, 0, dest), "extract reg applies");
    check(dest == 0x000000000000ABCDull, "extract reg field");

    // Rejections: short buffer, wrong opcode, memory ModRM, reg field on EXTRQ imm, field crossing bit 64.
    check(!decode(insert_imm, 3, instruction), "short buffer refused");
    const std::uint8_t wrong_opcode[] = {0xF2, 0x0F, 0x77, 0xC1, 0x10, 0x10};
    check(!decode(wrong_opcode, sizeof(wrong_opcode), instruction), "wrong opcode refused");
    const std::uint8_t memory_modrm[] = {0xF2, 0x0F, 0x78, 0x01, 0x10, 0x10};
    check(!decode(memory_modrm, sizeof(memory_modrm), instruction), "memory ModRM refused");
    const std::uint8_t extract_bad_reg[] = {0x66, 0xF2, 0x0F, 0x78, 0xC8, 0x10, 0x10};
    check(!decode(extract_bad_reg, sizeof(extract_bad_reg), instruction), "EXTRQ imm with nonzero reg refused");
    const std::uint8_t insert_cross[] = {0xF2, 0x0F, 0x78, 0xC1, 0x30, 0x30};
    dest = 0;
    check(!emulate(insert_cross, sizeof(insert_cross), 1, 0, dest), "field crossing bit 64 refused");

    std::printf("sse4a_emulation_test: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}