// Static XDspSubmitCommand argument roles and the one-shot opt-in submit-site probe.
// Evidence: docs/audio_decode.md section 11. The probe only classifies pointer shape; it never reads their contents.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace palmetto::audio_submit_probe {

inline constexpr std::uint32_t kSubmitCallRva = 0x4E5B20Du;
inline constexpr std::uint32_t kSubmitThunkRva = 0x5F3FF30u;
inline constexpr std::size_t kCallInstructionSize = 5;  // measured direct call with a rel32 target
inline constexpr std::uint8_t kCallRel32Opcode = 0xE8;  // x86-64 CALL rel32; not a copied instruction sequence
inline constexpr std::size_t kCommandDescriptorSize = 0x20;
inline constexpr std::size_t kCommandIdSize = sizeof(std::uint32_t);

// These are the three arguments as seen by chad.dll after the title wrapper swaps RCX and RDX.
struct Arguments {
    std::uintptr_t submit_context;
    std::uintptr_t command_descriptor;
    std::uintptr_t command_id_out;
    std::uintptr_t stack_limit;
    std::uintptr_t stack_base;
};

enum class ShapeResult {
    Measured,
    ContextNull,
    DescriptorNull,
    CommandIdNull,
    InvalidStackBounds,
    DescriptorUnaligned,
    CommandIdUnaligned,
    DescriptorOutsideStack,
    CommandIdOutsideStack,
};

inline const char* shape_name(ShapeResult result) noexcept {
    switch (result) {
        case ShapeResult::Measured: return "measured-context-descriptor-0x20-command-id-u32";
        case ShapeResult::ContextNull: return "submit-context-null";
        case ShapeResult::DescriptorNull: return "command-descriptor-null";
        case ShapeResult::CommandIdNull: return "command-id-out-null";
        case ShapeResult::InvalidStackBounds: return "current-thread-stack-bounds-invalid";
        case ShapeResult::DescriptorUnaligned: return "command-descriptor-not-8-aligned";
        case ShapeResult::CommandIdUnaligned: return "command-id-out-not-4-aligned";
        case ShapeResult::DescriptorOutsideStack: return "command-descriptor-not-stack-0x20";
        case ShapeResult::CommandIdOutsideStack: return "command-id-out-not-stack-u32";
    }
    return "unknown-shape-result";
}

inline bool range_is_on_stack(std::uintptr_t address, std::size_t length, std::uintptr_t low,
                              std::uintptr_t high) noexcept {
    return low < high && address >= low && address <= high && length <= high - address;
}

// Pure classifier seam. The callsite constructs a 0x20-byte descriptor and a separate stack dword for the id.
// Stack bounds are supplied by the caller; this function performs no memory reads or writes.
inline ShapeResult classify(const Arguments& args) noexcept {
    if (args.submit_context == 0) return ShapeResult::ContextNull;
    if (args.command_descriptor == 0) return ShapeResult::DescriptorNull;
    if (args.command_id_out == 0) return ShapeResult::CommandIdNull;
    if (args.stack_limit == 0 || args.stack_base <= args.stack_limit) return ShapeResult::InvalidStackBounds;
    if ((args.command_descriptor & 7u) != 0) return ShapeResult::DescriptorUnaligned;
    if ((args.command_id_out & 3u) != 0) return ShapeResult::CommandIdUnaligned;
    if (!range_is_on_stack(args.command_descriptor, kCommandDescriptorSize, args.stack_limit, args.stack_base))
        return ShapeResult::DescriptorOutsideStack;
    if (!range_is_on_stack(args.command_id_out, kCommandIdSize, args.stack_limit, args.stack_base))
        return ShapeResult::CommandIdOutsideStack;
    return ShapeResult::Measured;
}

enum class InstallResult {
    Installed,
    AlreadyStarted,
    ImageTooSmall,
    AddressOverflow,
    SiteNotExecutable,
    InstructionShapeMismatch,
    CallTargetMismatch,
    HandlerUnavailable,
    ProtectFailed,
    SiteChanged,
    ProtectionRestoreFailed,
};

inline const char* install_result_name(InstallResult result) noexcept {
    switch (result) {
        case InstallResult::Installed: return "armed-one-shot";
        case InstallResult::AlreadyStarted: return "already-started";
        case InstallResult::ImageTooSmall: return "image-too-small-for-measured-call";
        case InstallResult::AddressOverflow: return "image-base-overflow";
        case InstallResult::SiteNotExecutable: return "submit-site-not-committed-executable";
        case InstallResult::InstructionShapeMismatch: return "submit-site-not-call-rel32";
        case InstallResult::CallTargetMismatch: return "submit-call-target-not-measured-thunk";
        case InstallResult::HandlerUnavailable: return "vectored-handler-unavailable";
        case InstallResult::ProtectFailed: return "submit-site-protection-failed";
        case InstallResult::SiteChanged: return "submit-site-changed-during-install";
        case InstallResult::ProtectionRestoreFailed: return "submit-site-protection-restore-failed";
    }
    return "unknown-install-result";
}

enum class Phase : unsigned { Off, Armed, Restoring, Disarmed, Failed };

struct ProbeState {
    std::atomic<bool> started{false};
    std::atomic<Phase> phase{Phase::Off};
    std::uintptr_t site = 0;
    std::uint8_t original = 0;
    PVOID handler = nullptr;
};

inline ProbeState& probe_state() noexcept {
    static ProbeState state;
    return state;
}

inline bool readable_executable_protection(DWORD protection) noexcept {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protection & 0xFFu) {
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY: return true;
        default: return false;
    }
}

inline bool restore_instruction(ProbeState& state, bool& protection_restored) noexcept {
    DWORD old_protection = 0;
    auto* instruction = reinterpret_cast<std::uint8_t*>(state.site);
    if (!VirtualProtect(instruction, 1, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    *instruction = state.original;
    FlushInstructionCache(GetCurrentProcess(), instruction, 1);
    // The code byte is back before another trapped thread is allowed to resume at the callsite.
    state.phase.store(Phase::Disarmed, std::memory_order_release);
    DWORD ignored = 0;
    protection_restored = VirtualProtect(instruction, 1, old_protection, &ignored) != FALSE;
    return true;
}

inline LONG CALLBACK exception_handler(EXCEPTION_POINTERS* info) noexcept {
    ProbeState& state = probe_state();
    if (!info || !info->ExceptionRecord || !info->ContextRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;

    const std::uintptr_t site = state.site;
    auto* context = info->ContextRecord;
    if (site == 0 || reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress) != site ||
        context->Rip != site + 1)
        return EXCEPTION_CONTINUE_SEARCH;

    Phase expected = Phase::Armed;
    if (!state.phase.compare_exchange_strong(expected, Phase::Restoring, std::memory_order_acq_rel)) {
        // A second thread may have trapped at the same one-shot site before the first restored its call byte.
        // The critical section is only the one-byte restore; resume once that byte is back.
        if (expected == Phase::Restoring) {
            for (unsigned attempt = 0; attempt < 64 && state.phase.load(std::memory_order_acquire) == Phase::Restoring;
                 ++attempt)
                SwitchToThread();
        }
        if (state.phase.load(std::memory_order_acquire) == Phase::Disarmed) {
            context->Rip = site;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const Arguments args{static_cast<std::uintptr_t>(context->Rcx), static_cast<std::uintptr_t>(context->Rdx),
                         static_cast<std::uintptr_t>(context->R8),
                         reinterpret_cast<std::uintptr_t>(tib->StackLimit),
                         reinterpret_cast<std::uintptr_t>(tib->StackBase)};
    const ShapeResult shape = classify(args);

    bool protection_restored = true;
    if (!restore_instruction(state, protection_restored)) {
        state.phase.store(Phase::Failed, std::memory_order_release);
        std::fprintf(stderr, "[rt] audio submit probe: refusal=submit-site-restore-failed; no argument memory read\n");
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    context->Rip = site;  // execute the original call after the one-byte INT3 has been removed
    state.phase.store(Phase::Disarmed, std::memory_order_release);
    PVOID handler = state.handler;
    state.handler = nullptr;
    if (handler) RemoveVectoredExceptionHandler(handler);

    const char* context_shape = args.submit_context ? "non-null" : "null";
    const char* descriptor_shape =
        range_is_on_stack(args.command_descriptor, kCommandDescriptorSize, args.stack_limit, args.stack_base)
            ? "stack-0x20"
            : "not-stack-0x20";
    const char* id_shape = range_is_on_stack(args.command_id_out, kCommandIdSize, args.stack_limit, args.stack_base)
                               ? "stack-u32"
                               : "not-stack-u32";
    std::fprintf(stderr,
                 "[rt] audio submit probe: hit=1 RCX submit-context=%s RDX command-descriptor=%s "
                 "R8 command-id-out=%s shape=%s%s\n",
                 context_shape, descriptor_shape, id_shape, shape_name(shape),
                 protection_restored ? "" : " page-protection-restore-failed");
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// One callsite, one hit, no pointer contents or audio bytes read. The caller gates this behind the exact opt-in value.
inline InstallResult start(std::uintptr_t image_base, std::size_t image_size) noexcept {
    if (kSubmitCallRva > image_size || kCallInstructionSize > image_size - kSubmitCallRva)
        return InstallResult::ImageTooSmall;
    constexpr std::uintptr_t kSiteAndCallLength = kSubmitCallRva + kCallInstructionSize;
    if (image_base > std::numeric_limits<std::uintptr_t>::max() - kSiteAndCallLength)
        return InstallResult::AddressOverflow;

    const std::uintptr_t site = image_base + kSubmitCallRva;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(site), &mbi, sizeof mbi) != sizeof mbi ||
        mbi.State != MEM_COMMIT || !readable_executable_protection(mbi.Protect))
        return InstallResult::SiteNotExecutable;
    const std::uintptr_t region_base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    if (site < region_base) return InstallResult::SiteNotExecutable;
    const std::uintptr_t region_offset = site - region_base;
    if (region_offset > mbi.RegionSize || kCallInstructionSize > mbi.RegionSize - region_offset)
        return InstallResult::SiteNotExecutable;

    const auto* instruction = reinterpret_cast<const std::uint8_t*>(site);
    if (instruction[0] != kCallRel32Opcode) return InstallResult::InstructionShapeMismatch;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction + 1, sizeof displacement);
    const std::int64_t target_rva = static_cast<std::int64_t>(kSubmitCallRva) +
                                    static_cast<std::int64_t>(kCallInstructionSize) + displacement;
    if (target_rva != static_cast<std::int64_t>(kSubmitThunkRva)) return InstallResult::CallTargetMismatch;

    ProbeState& state = probe_state();
    bool not_started = false;
    if (!state.started.compare_exchange_strong(not_started, true, std::memory_order_acq_rel))
        return InstallResult::AlreadyStarted;
    state.site = site;
    state.original = instruction[0];
    state.handler = AddVectoredExceptionHandler(1, &exception_handler);
    if (!state.handler) {
        state.started.store(false, std::memory_order_release);
        return InstallResult::HandlerUnavailable;
    }

    DWORD old_protection = 0;
    auto* writable_site = reinterpret_cast<std::uint8_t*>(site);
    if (!VirtualProtect(writable_site, 1, PAGE_EXECUTE_READWRITE, &old_protection)) {
        RemoveVectoredExceptionHandler(state.handler);
        state.handler = nullptr;
        state.started.store(false, std::memory_order_release);
        return InstallResult::ProtectFailed;
    }
    if (*writable_site != kCallRel32Opcode) {
        DWORD ignored = 0;
        VirtualProtect(writable_site, 1, old_protection, &ignored);
        RemoveVectoredExceptionHandler(state.handler);
        state.handler = nullptr;
        state.started.store(false, std::memory_order_release);
        return InstallResult::SiteChanged;
    }

    state.phase.store(Phase::Armed, std::memory_order_release);
    *writable_site = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), writable_site, 1);
    DWORD ignored = 0;
    if (!VirtualProtect(writable_site, 1, old_protection, &ignored)) {
        // The page is still writable after a failed protection restore; remove the patch before refusing.
        *writable_site = state.original;
        FlushInstructionCache(GetCurrentProcess(), writable_site, 1);
        VirtualProtect(writable_site, 1, old_protection, &ignored);
        state.phase.store(Phase::Off, std::memory_order_release);
        RemoveVectoredExceptionHandler(state.handler);
        state.handler = nullptr;
        state.started.store(false, std::memory_order_release);
        return InstallResult::ProtectionRestoreFailed;
    }
    return InstallResult::Installed;
}

}  // namespace palmetto::audio_submit_probe
