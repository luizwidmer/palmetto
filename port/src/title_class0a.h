#pragma once

// Title-side mitigation for the class-0xa zero-byte constant-table upload (user-approved; docs/decomp_first_ui.md
// section 16 has the measurement and the A/B, docs/vertex_buffer_va.md sections 14-16 and docs/decomp_memory.md
// section 9 the mechanism).
//
// The measured ABI of the title's row-stride switch, image+0x1555C10:
//
//   uint32_t row_stride(uint32_t class_and_flags /*ecx*/, uint32_t width /*edx*/)
//
//   class = ecx & 0xfff; the body is a `switch (class)` answering bytes per row: classes 1,2 -> width*4; 3,4 ->
//   width*3 rounded up; 9 -> width; 0x15 -> width*2; the 0x32-0x42 and 100/101/200/201 families -> untiled/tiled
//   answers; every other class -> 0. It is a leaf: it reads only ecx and edx, writes eax, touches no stack and has
//   no .pdata entry. Its only references in the image are two direct branches: `call` at image+0x1555B85 inside the
//   per-plane size helper image+0x1555B60, and `call` at image+0x455965C, the per-plane stride write of the
//   constant-upload helper image+0x45594F0. The image holds no rip-relative `lea`/`mov` of its address and no
//   relocation entry pointing at it, so both references are direct branches.
//
// The class-0xa block (image+0x4559649 asks the title allocator for the size image+0x455A610 computes) is requested
// with zero bytes because that switch has no 0xa case, while the same job's copy (image+0x550E2D0) writes 16 bytes
// per element (width << 4 = 0x1000 for the measured 256-wide table). The image's own format table says class 0xa is
// format numbering R32G32B32A32_FLOAT, 16 bytes per element (docs/vertex_buffer_va.md section 16.3), so
// `width * 16` is the row stride the title's own format describes; with it the size becomes height*width*16 = 0x1000
// and the descriptor entry becomes {256, 1, 0x1000, 0x1000, block}, matching the copy. Only class 0xa changes.
//
// The mitigation is a trampoline (no int3, no VEH and no debug register, so it cannot collide with the int3-skip
// handler or the title-side probes) that is installed only after the image is mapped and before any image code runs,
// by pointing the direct branches at image+0x1555C10 at the trampoline and recording their original rel32 for
// uninstall. `make_row_stride_thunk` is the executable code itself and is testable on a synthetic buffer.
//
// Scope and risks are spelled out in the doc section: this is a title-side workaround for the experiment, not a
// console-faithful emulation; it is off unless PALMETTO_TITLE_CLASS0A_SIZE is set; and a different image build would
// need the RVA re-measured (the loader still refuses to redirect anything that is not a direct branch to it).

#include <cstdint>
#include <cstring>

#include <windows.h>

#include "code_hooks.h"

namespace palmetto::title_class0a {

// The row-stride switch's RVA in the measured retail image (relative to the image's preferred base 0x140000000).
inline constexpr std::uint32_t kRowStrideRva = 0x1555C10u;
// The class is `ecx & 0xfff` in the switch's own body.
inline constexpr std::uint32_t kClassMask = 0xfffu;
// The class the image's format table gives its 256x1 float4 constant-table entries.
inline constexpr std::uint32_t kClass0a = 0xau;
// R32G32B32A32_FLOAT: 16 bytes per element, which is also the copy's step (the upload copies `width << 4` bytes).
inline constexpr std::uint32_t kClass0aElementBytes = 0x10u;

// The class the switch sees for an argument register: `param_1 & 0xfff`.
inline std::uint32_t class_of(std::uint32_t class_and_flags) { return class_and_flags & kClassMask; }

// Whether this call is the class-0xa one, i.e. the only case the mitigation answers differently.
inline bool overrides(std::uint32_t class_and_flags) { return class_of(class_and_flags) == kClass0a; }

// The row stride the mitigation answers for class 0xa: bytes per element times the row width.
inline std::uint32_t row_stride(std::uint32_t width) { return width * kClass0aElementBytes; }

// PALMETTO_TITLE_CLASS0A_SIZE uses the opt-in registry's exact_one contract: only the literal value "1" enables
// the mitigation. Unset, empty, or any other value leaves the default path byte-identical.
inline bool switch_enabled(const char* value) { return value != nullptr && std::strcmp(value, "1") == 0; }

// The executable trampoline. Entry: ecx = class_and_flags, edx = width, as the original's callers leave them.
//
//   mov eax, ecx
//   and eax, kClassMask
//   cmp eax, kClass0a
//   jne  original
//   mov eax, edx
//   shl eax, log2(kClass0aElementBytes)
//   ret
// original:
//   mov rax, <original>
//   jmp rax
//
// The non-0xa path is the original function byte for byte: the two argument registers are untouched and the jump
// carries the caller's return address on the stack. The 0xa path returns in eax without touching the stack. The
// function has no prolog, so the Windows unwinder treats it as a leaf (the return address is at [rsp]), which is
// correct for both paths. The page is readable+executable after `make_row_stride_thunk`; the caller owns it and
// frees it through `free_row_stride_thunk`. Returns nullptr when no page can be found within 2 GiB of `original`.
inline std::uint8_t* make_row_stride_thunk(const void* original) {
    if (original == nullptr) return nullptr;
    static_assert((kClass0aElementBytes & (kClass0aElementBytes - 1)) == 0,
                  "the class-0xa element size must be a power of two for the shl encoding");
    std::uint8_t* const thunk = palmetto::hooks::alloc_near(original, 0x1000);
    if (thunk == nullptr) return nullptr;
    std::uint8_t* p = thunk;
    *p++ = 0x89;  // mov eax, ecx
    *p++ = 0xC8;
    *p++ = 0x25;  // and eax, imm32
    const std::uint32_t mask = kClassMask;
    std::memcpy(p, &mask, sizeof(mask));
    p += sizeof(mask);
    *p++ = 0x83;  // cmp eax, imm8
    *p++ = 0xF8;
    *p++ = static_cast<std::uint8_t>(kClass0a);
    std::uint8_t* const branch = p;
    *p++ = 0x75;  // jne original (the displacement is filled in after the 0xa path)
    *p++ = 0x00;
    *p++ = 0x89;  // mov eax, edx
    *p++ = 0xD0;
    *p++ = 0xC1;  // shl eax, shamt
    *p++ = 0xE0;
    unsigned shift = 0;
    for (std::uint32_t bytes = kClass0aElementBytes; bytes > 1; bytes >>= 1) ++shift;
    *p++ = static_cast<std::uint8_t>(shift);
    *p++ = 0xC3;  // ret
    branch[1] = static_cast<std::uint8_t>(p - (branch + 2));
    *p++ = 0x48;  // mov rax, imm64 (the original function)
    *p++ = 0xB8;
    const std::uint64_t target = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(original));
    std::memcpy(p, &target, sizeof(target));
    p += sizeof(target);
    *p++ = 0xFF;  // jmp rax
    *p++ = 0xE0;
    DWORD old = 0;
    if (!VirtualProtect(thunk, 0x1000, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), thunk, static_cast<SIZE_T>(p - thunk));
    return thunk;
}

// Frees a trampoline that no call site points at any more (after `uninstall_row_stride_patch`).
inline void free_row_stride_thunk(std::uint8_t* thunk) {
    if (thunk != nullptr) VirtualFree(thunk, 0, MEM_RELEASE);
}

// One rewritten direct branch: its address and the original rel32, so uninstall restores the exact bytes.
struct Site {
    std::uint8_t* at = nullptr;
    std::int32_t original_rel32 = 0;
};

struct Patch {
    std::uint8_t* image = nullptr;
    std::uint32_t image_size = 0;
    std::uint8_t* thunk = nullptr;
    Site sites[32] = {};
    unsigned site_count = 0;
    bool ok = false;
};

namespace detail {

inline bool executes(DWORD protect) {
    if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD base = protect & 0xFF;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

// Points every E8/E9 rel32 in [begin, end) whose target is `original` at `patch.thunk`, recording what it changed.
inline bool redirect_region(std::uintptr_t begin, std::uintptr_t end, std::uint8_t* original, Patch& patch) {
    std::uint8_t* const bytes = reinterpret_cast<std::uint8_t*>(begin);
    const std::size_t size = static_cast<std::size_t>(end - begin);
    if (size < 5) return true;
    DWORD old = 0;
    if (!VirtualProtect(bytes, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = true;
    for (std::size_t i = 0; i + 5 <= size && ok; ++i) {
        if (bytes[i] != 0xE8 && bytes[i] != 0xE9) continue;
        std::int32_t rel = 0;
        std::memcpy(&rel, bytes + i + 1, sizeof(rel));
        const std::uintptr_t next = begin + i + 5;
        const std::uintptr_t dst = next + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
        if (dst != reinterpret_cast<std::uintptr_t>(original)) continue;
        if (patch.site_count >= sizeof(patch.sites) / sizeof(patch.sites[0])) {
            ok = false;
            break;
        }
        const std::intptr_t to =
            static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(patch.thunk) - next);
        if (to < INT32_MIN || to > INT32_MAX) {
            ok = false;
            break;
        }
        patch.sites[patch.site_count++] = Site{bytes + i, rel};
        const std::int32_t forwarded = static_cast<std::int32_t>(to);
        std::memcpy(bytes + i + 1, &forwarded, sizeof(forwarded));
        i += 4;
    }
    DWORD ignored = 0;
    VirtualProtect(bytes, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), bytes, size);
    return ok;
}

}  // namespace detail

inline bool uninstall_row_stride_patch(Patch& patch, bool release_thunk = false);

// Installs the mitigation on the mapped image: a trampoline for image+`stride_rva` (the measured switch unless a test
// names another) and the direct branches to it redirected. Runs before any image code, so no thread executes the
// rewritten bytes. On failure nothing stays patched and `ok` is false. `image_size` is the mapped image's SizeOfImage.
inline Patch install_row_stride_patch(std::uint8_t* image, std::uint32_t image_size,
                                      std::uint32_t stride_rva = kRowStrideRva) {
    Patch patch;
    if (image == nullptr || image_size == 0 || stride_rva >= image_size) return patch;
    patch.image = image;
    patch.image_size = image_size;
    std::uint8_t* const original = image + stride_rva;
    patch.thunk = make_row_stride_thunk(original);
    if (patch.thunk == nullptr) return patch;
    const std::uintptr_t image_begin = reinterpret_cast<std::uintptr_t>(image);
    const std::uintptr_t image_end = image_begin + image_size;
    std::uintptr_t at = image_begin;
    while (at < image_end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info)) break;
        const std::uintptr_t region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const std::uintptr_t region_end = region + info.RegionSize;
        const std::uintptr_t begin = region > image_begin ? region : image_begin;
        const std::uintptr_t end = region_end < image_end ? region_end : image_end;
        if (end > begin && info.State == MEM_COMMIT && detail::executes(info.Protect)) {
            if (!detail::redirect_region(begin, end, original, patch)) {
                // install runs before any image code, so no thread can be inside the trampoline yet: restore what was
                // rewritten and free the page.
                uninstall_row_stride_patch(patch, true);
                return Patch{};
            }
        }
        if (region_end <= at) break;
        at = region_end;
    }
    patch.ok = patch.site_count > 0;
    if (!patch.ok) {
        free_row_stride_thunk(patch.thunk);
        patch.thunk = nullptr;
    }
    return patch;
}

// Restores every rewritten branch byte for byte (the trampoline page is left mapped unless `release_thunk`).
// Returns whether every site was restored; the patch is unusable afterwards either way. The trampoline is only safe
// to free when no thread can be inside it, so the default keeps the page: a shutdown-time uninstall can be concurrent
// with a thread in the trampoline's few instructions.
inline bool uninstall_row_stride_patch(Patch& patch, bool release_thunk) {
    bool restored = true;
    for (unsigned i = 0; i < patch.site_count; ++i) {
        Site& site = patch.sites[i];
        DWORD old = 0;
        if (!VirtualProtect(site.at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            restored = false;
            continue;
        }
        std::memcpy(site.at + 1, &site.original_rel32, sizeof(site.original_rel32));
        DWORD ignored = 0;
        VirtualProtect(site.at + 1, 4, old, &ignored);
    }
    if (patch.image != nullptr)
        FlushInstructionCache(GetCurrentProcess(), patch.image, static_cast<SIZE_T>(patch.image_size));
    patch.site_count = 0;
    patch.ok = false;
    if (release_thunk) {
        free_row_stride_thunk(patch.thunk);
        patch.thunk = nullptr;
    }
    return restored;
}

// Restores the call sites when it goes out of scope, so every normal C++ exit from the run (including an exception)
// leaves the mapped image as it was. A title-initiated ExitProcess ends the process with the in-memory patch, which is
// process-local: the mapped image is this process's private copy, nothing on disk changes.
struct ScopedPatch {
    Patch patch;
    ScopedPatch() = default;
    ScopedPatch(const ScopedPatch&) = delete;
    ScopedPatch& operator=(const ScopedPatch&) = delete;
    ~ScopedPatch() {
        if (patch.site_count != 0) uninstall_row_stride_patch(patch);
    }
};

}  // namespace palmetto::title_class0a
