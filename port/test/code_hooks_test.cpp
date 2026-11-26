// code_hooks.h: a remapping thunk and direct-call redirection, on small x64 functions written into memory. No game
// files: the archetype lookup they stand in for takes its model hash in ecx (docs/modding.md).
#include "../src/code_hooks.h"
#include "../src/mods.h"
#include "../src/title_class0a.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

uint32_t remap_lucia(uint32_t hash) {
    return hash == palmetto::mods::joaat("player_story000_00") ? palmetto::mods::joaat("cs_mandy") : hash;
}

void put(uint8_t* at, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) *at++ = b;
}

// `call rel32` (or `jmp` with e9) at `at` towards `to`.
void branch(uint8_t* at, uint8_t opcode, const uint8_t* to) {
    at[0] = opcode;
    const int32_t rel = static_cast<int32_t>(to - (at + 5));
    std::memcpy(at + 1, &rel, 4);
}

// sub rsp,0x28 / call to / add rsp,0x28 / ret
void caller(uint8_t* at, const uint8_t* to) {
    put(at, {0x48, 0x83, 0xEC, 0x28});
    branch(at + 4, 0xE8, to);
    put(at + 9, {0x48, 0x83, 0xC4, 0x28, 0xC3});
}

void init_pe64(uint8_t* img, uint32_t size_of_image, const std::vector<IMAGE_SECTION_HEADER>& sections) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = static_cast<WORD>(sections.size());
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = size_of_image;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (std::size_t i = 0; i < sections.size(); ++i) sec[i] = sections[i];
}

// Real functions for the unwinding checks: an exception leaves the hooked function, or the callback.
uint32_t throw_on_seven(uint32_t value) {
    if (value == 7) throw std::runtime_error("seven");
    return value + 1;
}

uint32_t throw_on_seven_result(uint32_t result, uint32_t argument) {
    if (argument == 8) throw std::runtime_error("eight");
    return result * 100 + argument;
}

// Leaves no return address of an earlier call where an unwinder that reads the wrong slot could find one.
void dirty_stack() {
    volatile unsigned char bytes[8192];
    for (auto& b : bytes) b = 0xAB;
}

bool caught_through(const uint8_t* thunk, uint32_t argument) {
    dirty_stack();
    try {
        reinterpret_cast<uint32_t (*)(uint32_t)>(const_cast<uint8_t*>(thunk))(argument);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

// Unwinds one frame from inside `thunk`, where its call returns to (`return_offset`, after a prolog that moved rsp by
// `frame` bytes), and says whether the caller's rip and rsp come out.
bool unwinds_to_caller(uint8_t* thunk, std::size_t return_offset, std::size_t frame) {
    alignas(16) uint64_t stack[64] = {};
    const uint64_t caller_rip = 0x1234567890ull;
    uint64_t* at_entry = stack + 40;  // rsp when the thunk is entered: the return address
    *at_entry = caller_rip;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    context.Rip = reinterpret_cast<DWORD64>(thunk + return_offset);
    context.Rsp = reinterpret_cast<DWORD64>(at_entry) - frame;
    DWORD64 base = 0, establisher = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
    if (!function || base != reinterpret_cast<DWORD64>(thunk)) return false;
    void* handler_data = nullptr;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
    return context.Rip == caller_rip && context.Rsp == reinterpret_cast<DWORD64>(at_entry + 1);
}

// The offset just after the first `call rax` (FF D0) of a thunk.
std::size_t after_first_call(const uint8_t* thunk) {
    for (std::size_t i = 0; i + 1 < 256; ++i)
        if (thunk[i] == 0xFF && thunk[i + 1] == 0xD0) return i + 2;
    return 0;
}

}  // namespace

int main() {
    using namespace palmetto::hooks;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    uint8_t* mem = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 4096);
    check(mem != nullptr, "executable memory near the module");
    if (!mem) return 1;
    std::memset(mem, 0xCC, 4096);

    uint8_t* target0 = mem;          // returns its first argument:  mov eax, ecx ; ret
    put(target0, {0x89, 0xC8, 0xC3});
    uint8_t* target1 = mem + 16;     // returns its second argument: mov eax, edx ; ret
    put(target1, {0x89, 0xD0, 0xC3});
    uint8_t* target2 = mem + 32;     // returns its third argument:  mov eax, r8d ; ret
    put(target2, {0x44, 0x89, 0xC0, 0xC3});

    uint8_t* call0 = mem + 64;
    caller(call0, target0);
    uint8_t* tail0 = mem + 96;       // jmp target0 (a tail call)
    branch(tail0, 0xE9, target0);
    uint8_t* decoy = mem + 112;      // a call to target0 + 1: not the hooked function, must stay as it is
    caller(decoy, target0 + 1);
    uint8_t* call1 = mem + 144;
    caller(call1, target1);
    uint8_t* call2 = mem + 176;
    caller(call2, target2);
    uint8_t decoy_before[16];
    std::memcpy(decoy_before, decoy, 16);

    uint8_t* thunk0 = make_remap_thunk(target0, 0, &remap_lucia, target0);
    uint8_t* thunk1 = make_remap_thunk(target1, 1, &remap_lucia, target1);
    uint8_t* thunk2 = make_remap_thunk(target2, 0, &remap_lucia, target2);
    check(thunk0 && thunk1 && thunk2, "thunks are allocated near their targets");
    if (!thunk0 || !thunk1 || !thunk2) return 1;

    const std::size_t n0 = redirect_direct_branches(mem + 64, 112, target0, thunk0);
    const std::size_t n1 = redirect_direct_branches(call1, 16, target1, thunk1);
    const std::size_t n2 = redirect_direct_branches(call2, 16, target2, thunk2);
    check(n0 == 2, "the call and the tail jump to the function are redirected, the decoy is not");
    check(n1 == 1 && n2 == 1, "one call each in the other callers");
    check(std::memcmp(decoy, decoy_before, 16) == 0, "a branch to another address is left byte for byte");
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);

    using Fn1 = uint32_t (*)(uint32_t);
    using Fn2 = uint32_t (*)(uint64_t, uint32_t);
    using Fn3 = uint32_t (*)(uint32_t, uint32_t, uint32_t);
    const uint32_t lucia = 0xE472B687u, mandy = 0xD7C0CDFFu;
    check(reinterpret_cast<Fn1>(call0)(lucia) == mandy, "through the call, Lucia's model id becomes cs_mandy's");
    check(reinterpret_cast<Fn1>(call0)(1234) == 1234, "any other id passes through unchanged");
    check(reinterpret_cast<Fn1>(tail0)(lucia) == mandy, "a tail jump is remapped as well");
    check(reinterpret_cast<Fn2>(call1)(0x1111, lucia) == mandy, "argument 1 (edx) is remapped when asked");
    check(reinterpret_cast<Fn3>(call2)(lucia, 2, 777) == 777, "r8 reaches the original unchanged");

    // hook_image_calls refuses what is not a mapped image or not code
    check(!hook_image_calls(mem, 0, &remap_lucia, 0).ok, "memory without a PE header is refused");
    auto* self = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    check(!hook_image_calls(self, 0x7FFFFFF0u, &remap_lucia, 0).ok, "an RVA outside the image is refused");
    check(!hook_image_calls(self, 0, &remap_lucia, 0).ok, "an RVA in the headers (not code) is refused");
    check(!make_remap_thunk(target0, 2, &remap_lucia, target0), "only argument 0 or 1 can be remapped");

    // The result-override thunk (a view-mode getter): the original returns 2 ("third person, far") for every
    // context; the override answers 4 ("first person") for context 0 only and leaves the others as they were.
    uint8_t* getter = mem + 512;
    put(getter, {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3});   // mov eax, 2 ; ret
    uint8_t* call_getter = mem + 544;
    caller(call_getter, getter);
    static int original_calls = 0;
    auto first_person_on_foot = [](uint32_t result, uint32_t context) -> uint32_t {
        ++original_calls;
        return context == 0 ? 4u : result;
    };
    uint8_t* rthunk = make_result_thunk(getter, first_person_on_foot, getter);
    check(rthunk != nullptr, "a result thunk is allocated");
    if (rthunk) {
        check(redirect_direct_branches(call_getter, 16, getter, rthunk) == 1, "the getter's caller is redirected");
        FlushInstructionCache(GetCurrentProcess(), mem, 4096);
        using Ctx = uint32_t (*)(uint32_t);
        check(reinterpret_cast<Ctx>(call_getter)(0) == 4, "context 0 gets the overridden result");
        check(reinterpret_cast<Ctx>(call_getter)(3) == 2, "another context keeps the original's result");
        check(original_calls == 2, "the override sees every call, after the original ran");
    }
    check(!hook_image_results(mem, 0, first_person_on_foot).ok, "hook_image_results refuses memory without a PE");

    // Test A: PE64 executable section with Misc.VirtualSize == 0 falls back to SizeOfRawData (matching pe_loader.cpp).
    uint8_t* img_a = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 0x2000);
    check(img_a != nullptr, "synthetic PE64 image A allocated near module");
    if (img_a) {
        std::memset(img_a, 0, 0x2000);
        IMAGE_SECTION_HEADER sec_a{};
        std::memcpy(sec_a.Name, ".text", 5);
        sec_a.Misc.VirtualSize = 0;
        sec_a.VirtualAddress = 0x1000;
        sec_a.SizeOfRawData = 0x200;
        sec_a.Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
        init_pe64(img_a, 0x2000, {sec_a});
        put(img_a + 0x1000, {0x89, 0xC8, 0xC3});  // mov eax, ecx ; ret
        caller(img_a + 0x1020, img_a + 0x1000);
        DWORD old_a = 0;
        check(VirtualProtect(img_a + 0x1000, 0x1000, PAGE_EXECUTE_READ, &old_a) != FALSE,
              "image A code page marked PAGE_EXECUTE_READ");
        FlushInstructionCache(GetCurrentProcess(), img_a + 0x1000, 0x1000);

        const HookResult ra = hook_image_calls(img_a, 0x1000, &remap_lucia, 0);
        check(ra.ok && ra.redirected == 1,
              "VirtualSize == 0 executable section uses SizeOfRawData and redirects its caller");
        if (ra.ok) {
            check(reinterpret_cast<Fn1>(img_a + 0x1020)(lucia) == mandy,
                  "hooked caller in VirtualSize == 0 section remaps Lucia to Mandy");
            check(reinterpret_cast<Fn1>(img_a + 0x1020)(1234) == 1234,
                  "hooked caller in VirtualSize == 0 section passes other hashes through");
        }
    }

    // Test B: a 0-byte executable section (VirtualSize == 0 && SizeOfRawData == 0) is skipped rather than failing VirtualProtect.
    uint8_t* img_b = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 0x2000);
    check(img_b != nullptr, "synthetic PE64 image B allocated near module");
    if (img_b) {
        std::memset(img_b, 0, 0x2000);
        IMAGE_SECTION_HEADER sec_empty{};
        std::memcpy(sec_empty.Name, ".empty", 6);
        sec_empty.Misc.VirtualSize = 0;
        sec_empty.VirtualAddress = 0x1000;
        sec_empty.SizeOfRawData = 0;
        sec_empty.Characteristics = IMAGE_SCN_MEM_EXECUTE;
        IMAGE_SECTION_HEADER sec_text{};
        std::memcpy(sec_text.Name, ".text", 5);
        sec_text.Misc.VirtualSize = 0x200;
        sec_text.VirtualAddress = 0x1000;
        sec_text.SizeOfRawData = 0x200;
        sec_text.Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
        init_pe64(img_b, 0x2000, {sec_empty, sec_text});
        put(img_b + 0x1040, {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3});  // mov eax, 2 ; ret
        caller(img_b + 0x1060, img_b + 0x1040);
        DWORD old_b = 0;
        check(VirtualProtect(img_b + 0x1000, 0x1000, PAGE_EXECUTE_READ, &old_b) != FALSE,
              "image B code page marked PAGE_EXECUTE_READ");
        FlushInstructionCache(GetCurrentProcess(), img_b + 0x1000, 0x1000);

        const int before_calls = original_calls;
        const HookResult rb = hook_image_results(img_b, 0x1040, first_person_on_foot);
        check(rb.ok && rb.redirected == 1,
              "0-byte executable section is skipped and the non-empty section is hooked");
        if (rb.ok) {
            using Ctx = uint32_t (*)(uint32_t);
            check(reinterpret_cast<Ctx>(img_b + 0x1060)(0) == 4,
                  "result hook in PE64 image overrides context 0");
            check(reinterpret_cast<Ctx>(img_b + 0x1060)(3) == 2,
                  "result hook in PE64 image preserves non-overridden contexts");
            check(original_calls == before_calls + 2,
                  "result hook invokes the original getter on each call");
        }
    }

    // Test C: sections extending past SizeOfImage are clamped and 32-bit VirtualAddress + VirtualSize wraparound is safe.
    uint8_t* img_c = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 0x2000);
    check(img_c != nullptr, "synthetic PE64 image C allocated near module");
    if (img_c) {
        std::memset(img_c, 0, 0x2000);
        IMAGE_SECTION_HEADER sec_wrap{};
        std::memcpy(sec_wrap.Name, ".wrap", 5);
        sec_wrap.Misc.VirtualSize = 0x200u;
        sec_wrap.VirtualAddress = 0xFFFFFF00u;
        sec_wrap.Characteristics = IMAGE_SCN_MEM_EXECUTE;
        IMAGE_SECTION_HEADER sec_over{};
        std::memcpy(sec_over.Name, ".text", 5);
        sec_over.Misc.VirtualSize = 0x1800u;  // spans 0x0800..0x2000, past SizeOfImage = 0x1000
        sec_over.VirtualAddress = 0x0800u;
        sec_over.Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
        init_pe64(img_c, 0x1000, {sec_wrap, sec_over});
        put(img_c + 0x0800, {0x89, 0xC8, 0xC3});  // mov eax, ecx ; ret
        caller(img_c + 0x0820, img_c + 0x0800);   // in-image caller
        caller(img_c + 0x1020, img_c + 0x0800);   // out-of-image bytes past SizeOfImage
        uint8_t out_before[16];
        std::memcpy(out_before, img_c + 0x1020, sizeof(out_before));
        FlushInstructionCache(GetCurrentProcess(), img_c, 0x2000);

        const HookResult rc = hook_image_calls(img_c, 0x0800, &remap_lucia, 0);
        check(rc.ok && rc.redirected == 1,
              "section past SizeOfImage is clamped and wraparound section is ignored");
        if (rc.ok) {
            check(reinterpret_cast<Fn1>(img_c + 0x0820)(lucia) == mandy,
                  "in-image caller inside clamped section is remapped");
            check(std::memcmp(img_c + 0x1020, out_before, sizeof(out_before)) == 0,
                  "bytes beyond SizeOfImage are left untouched");
        }
    }

    // Test D: malformed headers, PE32 optional header, out-of-image section table, and null arguments/callbacks.
    uint8_t* bad = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(bad != nullptr, "scratch buffer for malformed PE checks allocated");
    if (bad) {
        std::memset(bad, 0, 0x1000);
        auto* bad_dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bad);
        bad_dos->e_magic = IMAGE_DOS_SIGNATURE;
        bad_dos->e_lfanew = 0;
        check(!hook_image_calls(bad, 0x200, &remap_lucia, 0).ok, "MZ with e_lfanew == 0 is refused");
        bad_dos->e_lfanew = -16;
        check(!hook_image_calls(bad, 0x200, &remap_lucia, 0).ok, "MZ with negative e_lfanew is refused");

        std::memset(bad, 0, 0x1000);
        init_pe64(bad, 0x1000, {});
        auto* bad_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bad + 0x80);
        bad_nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        check(!hook_image_calls(bad, 0x200, &remap_lucia, 0).ok, "PE32 optional header magic is refused");

        std::memset(bad, 0, 0x1000);
        init_pe64(bad, 0x200, {});
        bad_nt->FileHeader.NumberOfSections = 100;
        check(!hook_image_calls(bad, 0x100, &remap_lucia, 0).ok,
              "section table extending past SizeOfImage is refused");
        VirtualFree(bad, 0, MEM_RELEASE);
    }

    if (img_a) {
        check(!hook_image_calls(img_a, 0x1000, nullptr, 0).ok, "hook_image_calls refuses null callback");
        VirtualFree(img_a, 0, MEM_RELEASE);
    }
    if (img_b) {
        check(!hook_image_results(img_b, 0x1040, nullptr).ok, "hook_image_results refuses null callback");
        VirtualFree(img_b, 0, MEM_RELEASE);
    }
    if (img_c) VirtualFree(img_c, 0, MEM_RELEASE);

    check(!hook_image_calls(nullptr, 0x1000, &remap_lucia, 0).ok, "hook_image_calls refuses null base");
    check(!hook_image_results(nullptr, 0x1000, first_person_on_foot).ok, "hook_image_results refuses null base");
    check(make_remap_thunk(target0, 0, nullptr, target0) == nullptr, "make_remap_thunk refuses null callback");
    check(make_remap_thunk(nullptr, 0, &remap_lucia, target0) == nullptr, "make_remap_thunk refuses null close_to");
    check(make_result_thunk(getter, nullptr, getter) == nullptr, "make_result_thunk refuses null callback");
    check(make_result_thunk(getter, first_person_on_foot, nullptr) == nullptr,
          "make_result_thunk refuses null original");
    check(redirect_direct_branches(nullptr, 16, target0, thunk0) == 0,
          "redirect_direct_branches returns 0 for null begin");
    VirtualFree(mem, 0, MEM_RELEASE);

    // Stack walks and exceptions pass through both thunks.
    const auto* throwing = reinterpret_cast<const void*>(&throw_on_seven);
    uint8_t* unwind_result = make_result_thunk(throwing, &throw_on_seven_result, throwing);
    uint8_t* unwind_remap = make_remap_thunk(throwing, 0, &throw_on_seven, throwing);
    check(unwind_result && unwind_remap, "thunks for the unwinding checks");
    if (unwind_result && unwind_remap) {
        check(reinterpret_cast<Fn1>(unwind_result)(1) == 201, "the result thunk still works");
        check(reinterpret_cast<Fn1>(unwind_remap)(1) == 3, "the remap thunk still works");
        check(unwinds_to_caller(unwind_result, after_first_call(unwind_result), 0x28),
              "a stack walk from the hooked function reaches the result thunk's caller");
        check(unwinds_to_caller(unwind_remap, after_first_call(unwind_remap), 0x88),
              "a stack walk from the callback reaches the remap thunk's caller");
        check(caught_through(unwind_result, 7), "an exception leaves the hooked function through the result thunk");
        check(caught_through(unwind_result, 8), "an exception leaves the callback through the result thunk");
        check(caught_through(unwind_remap, 7), "an exception leaves the callback through the remap thunk");
        const DWORD64 freed = reinterpret_cast<DWORD64>(unwind_result) + 1;
        DWORD64 base = 0;
        free_thunk(unwind_result);
        check(RtlLookupFunctionEntry(freed, &base, nullptr) == nullptr, "a freed thunk leaves no function table");
        free_thunk(unwind_remap);
    }
    free_thunk(nullptr);

    // ---- title_class0a.h: the class-0xa row-stride mitigation (docs/decomp_first_ui.md section 16) -------------------
    // The switch's value parser and the class decision are pure; the trampoline and the call-site redirection are
    // exercised on synthetic functions written into memory (the measured title switch's ABI, not its bytes).
    {
        using namespace palmetto::title_class0a;
        check(!switch_enabled(nullptr), "the class-0xa switch is off when unset");
        check(!switch_enabled(""), "the class-0xa switch is off when empty");
        check(!switch_enabled("0"), "the class-0xa switch is off for 0");
        check(!switch_enabled(" off "), "the class-0xa switch is off for off");
        check(!switch_enabled("FALSE"), "the class-0xa switch is off for FALSE");
        check(!switch_enabled("no"), "the class-0xa switch is off for no");
        check(switch_enabled("1"), "the class-0xa switch is on for 1");
        check(switch_enabled(" 1 "), "the class-0xa switch ignores surrounding blanks");
        check(switch_enabled("yes"), "the class-0xa switch is on for any other spelling");
        check(class_of(0xau) == 0xau, "class 0xa is read from the low 12 bits");
        check(class_of(0x1000u | 0xau) == 0xau, "flag bits above the low 12 are masked off");
        check(overrides(0xau) && overrides(0x1000u | 0xau), "0xa is the overridden class");
        check(!overrides(0x9u) && !overrides(0xbu) && !overrides(0u) && !overrides(0x1u),
              "every other class keeps the original answer");
        check(row_stride(0x100u) == 0x1000u, "the class-0xa row stride is width*16 (0x1000 at width 0x100)");
        check(row_stride(1u) == 0x10u, "the class-0xa row stride is 16 bytes per element");

        // The trampoline alone: class 0xa is answered without entering the original; other classes reach it.
        uint8_t* mem2 = alloc_near(reinterpret_cast<const void*>(&remap_lucia), 0x4000);
        check(mem2 != nullptr, "executable memory for the class-0xa trampoline");
        if (mem2) {
            std::memset(mem2, 0xCC, 0x4000);
            uint8_t* const original = mem2;  // the measured switch's shape: class 1 -> width*4, everything else -> 0
            put(original, {0x89, 0xC8});                                // mov eax, ecx
            put(original + 2, {0x25, 0xFF, 0x0F, 0x00, 0x00});          // and eax, 0xfff
            put(original + 7, {0x83, 0xF8, 0x01});                      // cmp eax, 1
            put(original + 10, {0x75, 0x06});                           // jne +6 (the zero path)
            put(original + 12, {0x89, 0xD0});                           // mov eax, edx
            put(original + 14, {0xC1, 0xE0, 0x02});                     // shl eax, 2
            put(original + 17, {0xC3});                                 // ret
            put(original + 18, {0x31, 0xC0, 0xC3});                     // xor eax, eax ; ret
            check(original[10] == 0x75 && original[11] == 0x06, "the synthetic original branches over the class-1 path");
            uint8_t* const thunk = make_row_stride_thunk(original);
            check(thunk != nullptr, "a class-0xa trampoline is allocated near the original");
            if (thunk) {
                using StrideFn = uint32_t (*)(uint32_t, uint32_t);
                const auto stride = reinterpret_cast<StrideFn>(thunk);
                check(stride(0xau, 0x100u) == 0x1000u, "the trampoline answers 0x1000 for class 0xa at width 0x100");
                check(stride(0xau, 0x40u) == 0x400u, "the trampoline scales the class-0xa stride with the width");
                check(stride(0x1000u | 0xau, 0x100u) == 0x1000u, "the trampoline masks the class like the original");
                check(stride(0x1u, 0x100u) == 0x400u, "class 1 reaches the original unchanged");
                check(stride(0x9u, 0x100u) == 0u, "a class the original answers 0 for still reaches the original");
                check(make_row_stride_thunk(nullptr) == nullptr, "a null original is refused");
                free_row_stride_thunk(thunk);
            }
            VirtualFree(mem2, 0, MEM_RELEASE);
        }

        // Install, call through, uninstall on a synthetic mapped image: the two direct branches are redirected and
        // restored byte for byte, the decoy branch is untouched, and class 0xa only changes while installed.
        uint8_t* const img = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        check(img != nullptr, "synthetic image for the class-0xa patch");
        if (img) {
            std::memset(img, 0xCC, 0x4000);
            constexpr std::uint32_t kStrideRva = 0x1000;
            uint8_t* const original = img + kStrideRva;
            put(original, {0x89, 0xC8});                                // mov eax, ecx
            put(original + 2, {0x25, 0xFF, 0x0F, 0x00, 0x00});          // and eax, 0xfff
            put(original + 7, {0x83, 0xF8, 0x01});                      // cmp eax, 1
            put(original + 10, {0x75, 0x06});                           // jne +6 (the zero path)
            put(original + 12, {0x89, 0xD0});                           // mov eax, edx
            put(original + 14, {0xC1, 0xE0, 0x02});                     // shl eax, 2
            put(original + 17, {0xC3});                                 // ret
            put(original + 18, {0x31, 0xC0, 0xC3});                     // xor eax, eax ; ret
            uint8_t* const caller_site = img + 0x1100;
            caller(caller_site, original);                              // call
            uint8_t* const tail_site = img + 0x1120;
            branch(tail_site, 0xE9, original);                          // tail jump
            uint8_t* const decoy_site = img + 0x1140;
            caller(decoy_site, original + 1);                           // a branch to another address
            uint8_t caller_before[16], tail_before[8], decoy_before[16], original_before[21];
            std::memcpy(caller_before, caller_site, sizeof(caller_before));
            std::memcpy(tail_before, tail_site, sizeof(tail_before));
            std::memcpy(decoy_before, decoy_site, sizeof(decoy_before));
            std::memcpy(original_before, original, sizeof(original_before));
            DWORD old_img = 0;
            check(VirtualProtect(img, 0x4000, PAGE_EXECUTE_READ, &old_img) != FALSE,
                  "the synthetic image is executable before install (the install scans executable regions)");
            FlushInstructionCache(GetCurrentProcess(), img, 0x4000);
            using StrideFn = uint32_t (*)(uint32_t, uint32_t);

            Patch patch = install_row_stride_patch(img, 0x4000, kStrideRva);
            check(patch.ok && patch.site_count == 2, "install redirects the call and the tail jump, not the decoy");
            if (patch.ok) {
                check(std::memcmp(decoy_site, decoy_before, sizeof(decoy_before)) == 0,
                      "the decoy branch to another address is left byte for byte");
                check(std::memcmp(original, original_before, sizeof(original_before)) == 0,
                      "the original switch's own bytes are not rewritten");
                check(reinterpret_cast<StrideFn>(caller_site)(0xau, 0x100u) == 0x1000u,
                      "through the redirected call, class 0xa answers 0x1000");
                check(reinterpret_cast<StrideFn>(tail_site)(0xau, 0x40u) == 0x400u,
                      "through the redirected tail jump, class 0xa answers width*16");
                check(reinterpret_cast<StrideFn>(caller_site)(0x1u, 0x100u) == 0x400u,
                      "class 1 still runs the original and answers width*4");
                check(reinterpret_cast<StrideFn>(caller_site)(0x9u, 0x100u) == 0u,
                      "a class the original answers 0 for still runs the original");
                check(reinterpret_cast<StrideFn>(caller_site)(0x1000u | 0xau, 0x20u) == 0x200u,
                      "the redirected call masks the class and scales the width");
                const bool removed = uninstall_row_stride_patch(patch);
                check(removed, "uninstall restores every site");
                check(std::memcmp(caller_site, caller_before, sizeof(caller_before)) == 0,
                      "the redirected call is restored byte for byte");
                check(std::memcmp(tail_site, tail_before, sizeof(tail_before)) == 0,
                      "the redirected tail jump is restored byte for byte");
                check(patch.site_count == 0 && !patch.ok, "an uninstalled patch is empty");
                check(reinterpret_cast<StrideFn>(caller_site)(0xau, 0x100u) == 0u,
                      "after uninstall class 0xa runs the original again (the default path)");
                check(uninstall_row_stride_patch(patch), "uninstalling an empty patch is a no-op");

                // Install a second time (proves the restored bytes are patchable again) and release the page once no
                // call site points at it.
                Patch again = install_row_stride_patch(img, 0x4000, kStrideRva);
                check(again.ok && again.site_count == 2, "the restored image can be patched again");
                if (again.ok) {
                    check(reinterpret_cast<StrideFn>(caller_site)(0xau, 0x10u) == 0x100u,
                          "the re-installed patch answers again");
                    check(uninstall_row_stride_patch(again, true), "the second uninstall restores and frees");
                }
            }

            // Refusals: no image, the RVA outside the image, and an image that is not executable (install must not
            // rewrite data pages).
            check(!install_row_stride_patch(nullptr, 0x4000, kStrideRva).ok, "a null image is refused");
            check(!install_row_stride_patch(img, 0x1000, kStrideRva).ok, "an RVA outside the image is refused");
            DWORD ignored = 0;
            VirtualProtect(img, 0x4000, PAGE_READWRITE, &ignored);
            const Patch read_only = install_row_stride_patch(img, 0x4000, kStrideRva);
            check(!read_only.ok && read_only.site_count == 0,
                  "a non-executable image is refused and nothing stays patched");
            check(std::memcmp(caller_site, caller_before, sizeof(caller_before)) == 0,
                  "a refused install leaves the callers byte for byte");
            VirtualFree(img, 0, MEM_RELEASE);
        }
    }

    std::printf("code_hooks_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
