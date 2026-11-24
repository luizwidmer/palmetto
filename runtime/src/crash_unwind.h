// Crash-time frame walk for the manually mapped target console image.
//
// Why this exists: the runtime's exception trace (shims/startup_trace.h) names the faulting module+RVA and, with
// PALMETTO_DUMP_REGS, some registers, but not the call chain. A clobbered callee-saved register cannot be attributed
// to the frame that clobbered it from one PC; a stack walk that names every frame does. The mapped title is not an
// HMODULE, so this walker is handed the image's base, size and own .pdata (RUNTIME_FUNCTION array) with set_image()
// instead of looking them up in the process, and it searches that table itself before calling RtlVirtualUnwind.
//
// How: copy the faulting CONTEXT, look the control PC up in the image's .pdata, print the frame, RtlVirtualUnwind one
// frame, repeat. The frame line carries the image-relative PC (RVA), the containing routine's begin RVA, up to eight
// stack words and the callee-saved registers as that frame saw them.
//
// Robustness contract - this runs inside an exception handler, over a stack that may be corrupt:
//   * no heap allocation, no C++ exceptions, no std::string/std::vector on the fault path. The report is streamed one
//     frame at a time, so a 64-frame walk needs no 64-frame buffer; all buffers are fixed-size locals.
//   * the image's .pdata, every UNWIND_INFO it points at and every stack word are range-checked and VirtualQuery'd
//     before they are read, and the one read that cannot be bounds-checked (RtlVirtualUnwind, which follows chained
//     unwind info itself) is inside __try/__except.
//   * the walk is bounded by kMaxFrames and stops on a repeated control PC, a stack pointer that does not advance, an
//     unreadable stack, or the first frame outside the image. It never modifies the CONTEXT it is given.
//
// Microsoft's x64 exception-handling documentation describes the RUNTIME_FUNCTION/UNWIND_INFO format and the
// virtual-unwind contract this uses: https://learn.microsoft.com/cpp/build/exception-handling-x64 (documentation
// only; no code is copied). The unwind loop follows the same shape as the loader tests' short walks
// (test/unimplemented_policy_test.cpp, shims/startup_trace.h), with the explicit table lookup and the guards added
// here because a crash reporter cannot assume the table or the stack is intact.
#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace rt::crash_report {

// A synthetic call chain and the current boot both fit well inside this; the cap only exists so a corrupt stack
// cannot make the handler walk (and print) without bound. shims/startup_trace.h stops at 16 frames because it writes
// a JSON line per fault; a report is allowed more because it is written once and each line is short.
inline constexpr size_t kMaxFrames = 64;

// "up to 8 argument/stack words": the shadow space plus the first stack arguments of a frame.
inline constexpr size_t kMaxStackWords = 8;

// Why the walk ended. dump() names it in the report's last line; tests assert it.
enum class Stop : uint8_t {
    ContextNull,        // no CONTEXT was passed
    NoSink,             // no frame sink was passed to walk()
    ImageNotSet,        // set_image() was never called
    FaultOutsideImage,  // the first control PC is not in the registered image (e.g. a fault inside a host shim)
    LeftImage,          // a later return address is outside the image
    NoFunctionEntry,    // the image's .pdata has no entry for the control PC (leaf code, or unmapped code)
    BadUnwindInfo,      // the .pdata entry is there but its UNWIND_INFO is not usable (bounds, version, chain)
    UnwindFault,        // RtlVirtualUnwind raised an exception while reading the unwind data
    InvalidStack,       // the stack pointer is not readable, so there is no return address to unwind to
    StackNotAdvancing,  // unwinding produced an RSP that is not above the previous one
    RepeatedFrame,      // a control PC repeats an earlier frame (a saved return address that points at itself)
    FrameLimit,         // kMaxFrames was reached
};

// A short static phrase for the stop; safe to call from the fault path (no formatting, no allocation).
const char* stop_text(Stop stop) noexcept;

// One walked frame. POD on purpose: the fault path zero-initialises it and fills it with plain stores.
struct Frame {
    uint64_t rip;            // absolute control PC of this frame (the faulting RIP for the first one)
    uint64_t rsp;            // stack pointer at this frame; stack_words were read from here
    uint32_t rva;            // rip - image base; 0 when the frame is outside the image
    uint32_t routine_rva;    // begin RVA of the routine holding rip, 0 when there is no .pdata entry
    bool in_image;
    bool has_unwind_info;
    unsigned stack_word_count;                    // 0 when the stack page above RSP is not readable
    uint64_t stack_words[kMaxStackWords];         // qwords at RSP: shadow space and stack arguments of a frame
    uint64_t callee_saved[8];                     // rbx, rbp, rsi, rdi, r12, r13, r14, r15 as this frame saw them
};

// Delivered once per frame, in order, from walk(). Must not allocate or throw. Returning false would be ignored today
// (the sink is allowed to be a simple logger); the void return keeps the callback usable from the fault path.
using FrameSink = void (*)(void* user, const Frame& frame);

// Walks `context`'s stack through the registered image and hands each frame to `sink` (must not be null). `user` is
// passed back to the sink unchanged. Returns why the walk stopped. Exposed for tests and for a caller that wants the
// frames as data instead of a file; dump() is the entry point for the runtime.
Stop walk(const CONTEXT* context, void* user, FrameSink sink) noexcept;

// Registers the image whose .pdata describes guest frames, and clears any previous one. Call once after
// Loader::load(), before guest code runs, with the main module's mapped range and Module::function_table /
// Module::function_count. The pointers must stay valid for the process lifetime (they live in the mapped image).
// base == nullptr or size == 0 leaves the walker unregistered, which dump() reports instead of guessing.
void set_image(void* base, size_t size, const IMAGE_RUNTIME_FUNCTION_ENTRY* functions, size_t count) noexcept;

// Writes a crash report to `path` (UTF-16, created or overwritten), or to stderr when `path` is null, and returns the
// number of frames written (0 when there was nothing to walk or the file could not be created). Every frame is
// written as soon as it is known, so an unwind that faults halfway still leaves the frames before it on disk.
//
// user_data is carried for the integrator and is not read by this implementation: it is reserved so a caller can add
// context later without a new entry point. Pass nullptr.
size_t dump(const CONTEXT* context, const wchar_t* path, const void* user_data = nullptr) noexcept;

}  // namespace rt::crash_report
