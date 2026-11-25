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
// `tag`, when given, becomes a `pass:` line under the header: the trace passes the exception code of the pass that
// produced the file, so reports from two different faults can be told apart without trusting a timestamp. It is ASCII
// and is copied verbatim. (This parameter used to be a reserved `const void* user_data` that was never read; a
// parameter nobody reads is a parameter that invites the caller to believe it does something.)
size_t dump(const CONTEXT* context, const wchar_t* path, const char* tag = nullptr) noexcept;

// The same report, written to a handle the caller already opened: this creates nothing, opens nothing, closes nothing
// and joins no path. dump_all() uses it because a CreateFileW issued while another thread is suspended can deadlock
// (see the ordering note under dump_all). The result separates the two things `dump()`'s frame count cannot: a thread
// parked outside the image legitimately walks zero frames, while a handle that rejected the bytes is a failure. A
// caller that asks "did this pass write a report for this thread" must use this answer, not the filesystem: a stale
// report at the same name answers "yes" for a write that never happened.
struct ReportWrite {
    size_t frames;  // frames walked and written to the handle
    bool written;   // every byte reached the handle (false: the handle rejected a write)
};
ReportWrite dump_to_handle(const CONTEXT* context, HANDLE file, const char* tag = nullptr) noexcept;

// What one dump_all() pass did. POD, returned by value: the fault path cannot allocate a result vector, and the
// caller wants these four numbers for its one log line.
struct AllThreadsResult {
    size_t considered;   // threads of this process besides the caller, truncated ones included
    size_t written;      // report files created and written to
    size_t skipped;      // threads whose report could not be produced (exited, access denied, file refused)
    size_t truncated;    // threads past kMaxDumpedThreads that were left out
};

// The cap on threads one pass will consider. A crash report is not a census: a process with more threads than this
// has a different problem, and the array that holds the ids is a fixed stack local (see the note on allocation).
inline constexpr size_t kMaxDumpedThreads = 256;

// How much stack the reporter asks the loader to keep usable when a thread's stack overflows. dump_all() asks for it
// when it starts and startup_trace.h asks for it when the trace is installed, because the guarantee is per thread and
// only covers a fault raised after the call. A guarantee is not a reserve: it grows no stack, repairs no damage and
// cannot move the stack limit - it only decides how much of the stack remains usable to the handler once
// STATUS_STACK_OVERFLOW has happened, which is the one fault this reporter is most useful for and the one where a
// large frame would otherwise be its own second fault. 128 KiB covers the measured peak chain (8 664 B, 8 840 B
// counting the pushed registers) with room for the CRT formatting inside vsnprintf and for whatever the handler above
// has already spent. The smallest guarantee the pass survives depends on the binary and on how the stack was
// exhausted, so no threshold is claimed; see docs/crash_report.md. Asking for more than the thread's reserve fails;
// the caller ignores that result and runs the pass anyway.
inline constexpr unsigned kStackGuaranteeBytes = 128u * 1024u;

// Dumps every thread of this process except the caller: enumerates the threads, then for each one suspends it, reads
// its CONTEXT_CONTROL|CONTEXT_INTEGER and writes `<directory>\crash_report.<tid>.txt` with the same walker dump()
// uses. Returns what it managed, and writes one line per thread to `summary_path` - or, when that is null, to
// `<directory>\crash_report.threads.txt`, which is where a caller that knows only the directory gets its index:
//
//   [rt] crash report: all threads of process 1234, 3 besides the reporting one
//   thread 1234: 5 frames
//   thread 5678: not captured (OpenThread failed)
//   considered 3, written 2, skipped 1, truncated 0
//
// `reason`, when given, is copied into the index and into every per-thread report's `pass:` line (the trace passes the
// exception code that triggered the pass), so a directory of reports says which fault produced it.
//
// The per-thread frame count is the summary's only job; why a walk stopped is in that thread's own report, where
// dump() already wrote it. Summarising it here would mean walking twice.
//
// Why the caller's own thread is excluded: it is executing this function, so its own CONTEXT is the handler's and
// adding it here would duplicate the report dump() already writes for the faulting thread.
//
// Ordering - every file is opened before the thread it belongs to is suspended, and why: CreateFileW takes the
// process heap lock. If it is called inside the suspend window and the lock's holder happens to be one of the threads
// this pass has already parked, the wait never ends: measured with a thread that takes HeapLock and is then suspended
// in the loop, the pass stopped dead 29 files in. So the thread list, the index and the per-thread report files are
// opened while nothing of this pass is suspended; inside the window there is only SuspendThread, GetThreadContext and
// writes to an already-open handle, none of which needs the process heap. All of the report files are opened in one
// burst, not in batches: batching looks safe - the batches are serialised and each batch's threads are resumed before
// the next CreateFileW is issued - but it moves half of the pass's opens behind its own suspend windows, and a
// CreateFileW issued there can still wait forever on a heap lock somebody else took 15 ms ago and never releases.
// Measured: with 60 parked threads and such a lock taker, one burst finished every run and 32-file batching never
// finished (see the ordering note in crash_unwind.cpp and docs/crash_report.md). This is the same rule the wait-state
// sampler in src/main.cpp already follows ("allocated before any thread is suspended" / "everything below runs with
// every thread resumed"): collect raw data inside the window, do anything that can take a lock outside it.
// A report file that was created but never written belongs to a thread that could not be captured, and the index says
// which reason applied.
//
// Stack: this function's frame and the chain it starts are the largest thing on the crash path, and a stack-overflow
// fault is exactly the case where there is no stack left to give them. Two things keep that from being fatal: the chain
// fits in the guarantee this function asks for (see kStackGuaranteeBytes for the measured frame sizes and for the
// smallest guarantee that was measured to be enough), and the entrance asks for the guarantee itself, so that a fault
// raised after it still has room for the whole chain. What that does not cover: a thread that faults before it ever
// runs this function needs its own guarantee (startup_trace.h asks for one when the trace is installed), and no
// guarantee helps if the stack is already exhausted past it.
//
// Allocation: the ids, the handles and the per-thread open state live in fixed stack arrays, and every buffer is a
// fixed local, so this performs no C++ allocation. It does call CreateToolhelp32Snapshot / OpenThread / CreateFileW /
// WriteFile, whose kernel-side allocations are unavoidable for enumerating and writing; nothing on this path allocates
// from the C++ heap. That promise is about this function only: the trace that calls it (shims/startup_trace.h) builds
// its JSON event out of std::string before it gets here, so "the crash-report path does not allocate" is false once
// this is wired in.
//
// Robustness: the suspension is released in a __finally, so it is released on a normal exit, on a `continue`, and on
// an SEH fault alike. That is not decoration. The caller's __except (EXCEPTION_EXECUTE_HANDLER) does not unwind the
// stack, so a ResumeThread() at the end of the block would be skipped by exactly the fault it exists to survive, and
// the process - which continues running, because that handler is first-chance and returns CONTINUE_SEARCH - would be
// left with a suspended thread nobody can wake. The __finally body holds one ResumeThread and nothing else: it must
// neither fail nor block. A thread that cannot be captured is counted in `skipped` and the pass continues; the report
// of one unreadable thread is never a reason to lose the other nineteen. A suspended thread's user-mode stack is
// stable for the walk (the kernel may keep it in a wait, which the walk reports as the frame it stopped in, not as an
// error).
//
// Limits: a thread that is inside a kernel transition still reports its last user-mode frame; a thread created after
// the snapshot is taken is not in this report; and `considered` counts every thread the snapshot named, while only the
// first kMaxDumpedThreads get a report - the index names how many were left out, so `written + skipped + truncated ==
// considered` holds in every pass. None of these is a reason to retry: a crash report describes the moment it ran.
AllThreadsResult dump_all(const wchar_t* directory, const wchar_t* summary_path = nullptr,
                          const char* reason = nullptr) noexcept;

}  // namespace rt::crash_report
