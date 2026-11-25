// Crash-time stack walk over the mapped image's own .pdata. See crash_unwind.h for the contract and why every read
// below is checked before it is made: the caller may be an exception handler running on a stack that the fault
// corrupted, and a reporter that faults (or allocates, or blocks) while reporting is worse than no reporter.

#include "crash_unwind.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <tlhelp32.h>

namespace rt::crash_report {
namespace {

// The image registration is written once after the loader maps the title and read from the fault path. The atomics
// have constant initialisers, so there is nothing to construct (or destroy) if the report runs late in teardown.
std::atomic<void*> g_base{nullptr};
std::atomic<uint64_t> g_size{0};
std::atomic<const IMAGE_RUNTIME_FUNCTION_ENTRY*> g_functions{nullptr};
std::atomic<size_t> g_count{0};
std::atomic<bool> g_registered{false};

// A real .pdata has one 12-byte entry per function; the title's is a few hundred KiB at most. Anything past this is
// not a table the walker can trust, and multiplying it out would only hide a bad count behind an overflow.
constexpr size_t kMaxFunctionEntries = 16u * 1024u * 1024u;

// UNW_FLAG_CHAININFO links another UNWIND_INFO. RtlVirtualUnwind follows that link itself, so a chain that loops
// would make the OS call spin forever inside the handler. Validate the chain with this depth bound first.
constexpr unsigned kMaxChainDepth = 8;

struct Image {
    uint64_t base;
    uint64_t size;
    const IMAGE_RUNTIME_FUNCTION_ENTRY* functions;
    size_t count;
};

uint64_t to_u64(const void* pointer) noexcept { return reinterpret_cast<uint64_t>(pointer); }

Image load_image() noexcept {
    Image image{};
    if (!g_registered.load(std::memory_order_acquire)) return image;
    image.base = to_u64(g_base.load(std::memory_order_relaxed));
    image.size = g_size.load(std::memory_order_relaxed);
    image.functions = g_functions.load(std::memory_order_relaxed);
    image.count = g_count.load(std::memory_order_relaxed);
    return image;
}

// True when [address, address + bytes) is committed, readable memory: every page the range touches is checked, so a
// range that starts in a committed page and runs into an unmapped one is rejected rather than read.
bool readable_range(uint64_t address, size_t bytes) noexcept {
    uint64_t at = address;
    while (bytes != 0) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(at)), &memory, sizeof memory) !=
            sizeof memory)
            return false;
        if (memory.State != MEM_COMMIT) return false;
        if ((memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        const uint64_t region_start = to_u64(memory.BaseAddress);
        const uint64_t region_end = region_start + memory.RegionSize;
        if (at < region_start || at >= region_end) return false;
        uint64_t step = region_end - at;
        if (step > bytes) step = bytes;
        at += step;
        bytes -= static_cast<size_t>(step);
    }
    return true;
}

// A copy from an address that may be unmapped, non-canonical or protected. The SEH frame here is tiny and has no
// objects that need unwinding (C2712); the caller only sees false.
bool safe_copy(void* destination, const void* source, size_t bytes) noexcept {
    if (bytes == 0) return true;
    __try {
        std::memcpy(destination, source, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// The qwords at RSP. A frame's eight words can straddle a page boundary, so only what the first committed page
// holds is returned; the unwinder only needs the first word (the return address) and the walker reports the rest.
unsigned read_stack_words(uint64_t rsp, uint64_t (&words)[kMaxStackWords]) noexcept {
    if (rsp == 0) return 0;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(rsp)), &memory, sizeof memory) != sizeof memory)
        return 0;
    if (memory.State != MEM_COMMIT) return 0;
    if ((memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return 0;
    const uint64_t region_start = to_u64(memory.BaseAddress);
    const uint64_t region_end = region_start + memory.RegionSize;
    if (rsp < region_start || rsp >= region_end) return 0;
    size_t count = static_cast<size_t>((region_end - rsp) / sizeof(uint64_t));
    if (count > kMaxStackWords) count = kMaxStackWords;
    size_t read = 0;
    __try {
        for (; read < count; ++read)
            words[read] = reinterpret_cast<const volatile uint64_t*>(static_cast<uintptr_t>(rsp))[read];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Keep the words read before the fault: "up to 8" is part of the result, not an all-or-nothing promise.
    }
    return static_cast<unsigned>(read);
}

void fill_callee_saved(Frame& frame, const CONTEXT& context) noexcept {
    frame.callee_saved[0] = context.Rbx;
    frame.callee_saved[1] = context.Rbp;
    frame.callee_saved[2] = context.Rsi;
    frame.callee_saved[3] = context.Rdi;
    frame.callee_saved[4] = context.R12;
    frame.callee_saved[5] = context.R13;
    frame.callee_saved[6] = context.R14;
    frame.callee_saved[7] = context.R15;
}

// .pdata entries are sorted by BeginAddress in a PE32+ image, so binary search is exact and cheap. The table is
// re-checked for readability first: a stale count or a pointer that is no longer mapped must read as "no entry",
// not as a fault.
const IMAGE_RUNTIME_FUNCTION_ENTRY* search_table(const Image& image, uint32_t rva) noexcept {
    if (image.functions == nullptr || image.count == 0 || image.count > kMaxFunctionEntries) return nullptr;
    const size_t bytes = image.count * sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    if (!readable_range(to_u64(image.functions), bytes)) return nullptr;
    size_t low = 0;
    size_t high = image.count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const IMAGE_RUNTIME_FUNCTION_ENTRY& entry = image.functions[middle];
        if (rva < entry.BeginAddress) {
            high = middle;
        } else if (rva >= entry.EndAddress) {
            low = middle + 1;
        } else {
            return &entry;
        }
    }
    return nullptr;
}

bool usable_entry(const Image& image, const IMAGE_RUNTIME_FUNCTION_ENTRY& entry) noexcept {
    return entry.BeginAddress < entry.EndAddress && static_cast<uint64_t>(entry.EndAddress) <= image.size;
}

// Finds the routine holding pc. is_return_address selects the debugger behaviour for a call that is the last
// instruction of a routine: the return address is then exactly EndAddress, so the byte before it is looked up and
// that (still in-range) control PC is what RtlVirtualUnwind is given.
const IMAGE_RUNTIME_FUNCTION_ENTRY* find_entry(const Image& image, uint64_t pc, bool is_return_address,
                                               uint64_t& control_pc) noexcept {
    if (pc < image.base) return nullptr;
    const uint64_t offset = pc - image.base;
    if (offset >= image.size) return nullptr;
    const uint32_t rva = static_cast<uint32_t>(offset);
    if (const IMAGE_RUNTIME_FUNCTION_ENTRY* entry = search_table(image, rva)) {
        if (!usable_entry(image, *entry)) return nullptr;
        control_pc = pc;
        return entry;
    }
    if (is_return_address && rva != 0) {
        if (const IMAGE_RUNTIME_FUNCTION_ENTRY* entry = search_table(image, rva - 1)) {
            if (!usable_entry(image, *entry)) return nullptr;
            control_pc = pc - 1;
            return entry;
        }
    }
    return nullptr;
}

enum class InfoStatus { Ok, BadVersion, OutOfBounds, ChainTooDeep };

// Checks one UNWIND_INFO (and the chain it links) against the mapped image before RtlVirtualUnwind reads it. The
// format is documented in Microsoft's x64 exception-handling notes: version in bits 0-2, flags 3-7, prolog size,
// code count, then CodeCount 2-byte unwind codes, an optional handler RVA, and - for UNW_FLAG_CHAININFO - one
// RUNTIME_FUNCTION.
InfoStatus validate_unwind_info(const Image& image, uint32_t unwind_rva, unsigned depth) noexcept {
    if (unwind_rva == 0 || static_cast<uint64_t>(unwind_rva) >= image.size) return InfoStatus::OutOfBounds;
    uint8_t header[4] = {};
    const uint8_t* const info = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(image.base + unwind_rva));
    if (!safe_copy(header, info, sizeof header)) return InfoStatus::OutOfBounds;
    const unsigned version = header[0] & 7u;
    const unsigned flags = header[0] >> 3u;
    if (version != 1 && version != 2) return InfoStatus::BadVersion;
    uint64_t size = 4ull + 2ull * header[2];
    if ((flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) != 0) size += 4;
    if (unwind_rva + size > image.size) return InfoStatus::OutOfBounds;
    if ((flags & UNW_FLAG_CHAININFO) == 0) return InfoStatus::Ok;
    if (depth >= kMaxChainDepth) return InfoStatus::ChainTooDeep;
    if (unwind_rva + size + sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) > image.size) return InfoStatus::OutOfBounds;
    IMAGE_RUNTIME_FUNCTION_ENTRY chained{};
    if (!safe_copy(&chained, info + size, sizeof chained)) return InfoStatus::OutOfBounds;
    return validate_unwind_info(image, chained.UnwindInfoAddress, depth + 1);
}

enum class UnwindStatus { Ok, BadInfo, Faulted };

UnwindStatus unwind_frame(const Image& image, const IMAGE_RUNTIME_FUNCTION_ENTRY& entry, uint64_t control_pc,
                          CONTEXT& context) noexcept {
    if (validate_unwind_info(image, entry.UnwindInfoAddress, 0) != InfoStatus::Ok) return UnwindStatus::BadInfo;
    const auto* function = reinterpret_cast<const RUNTIME_FUNCTION*>(&entry);
    bool faulted = false;
    __try {
        void* handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, static_cast<DWORD64>(image.base), static_cast<DWORD64>(control_pc),
                         const_cast<PRUNTIME_FUNCTION>(function), &context, &handler_data, &establisher_frame,
                         nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    return faulted ? UnwindStatus::Faulted : UnwindStatus::Ok;
}

}  // namespace

const char* stop_text(Stop stop) noexcept {
    switch (stop) {
        case Stop::ContextNull: return "no CONTEXT was passed";
        case Stop::NoSink: return "no frame sink was passed";
        case Stop::ImageNotSet: return "no image was registered with set_image";
        case Stop::FaultOutsideImage: return "the faulting PC is outside the image";
        case Stop::LeftImage: return "the next return address is outside the image";
        case Stop::NoFunctionEntry: return "the image's .pdata has no entry for the frame";
        case Stop::BadUnwindInfo: return "the .pdata entry's unwind info is not usable";
        case Stop::UnwindFault: return "RtlVirtualUnwind faulted while reading the unwind info";
        case Stop::InvalidStack: return "the stack pointer is not readable";
        case Stop::StackNotAdvancing: return "unwinding did not move the stack pointer up";
        case Stop::RepeatedFrame: return "a frame repeats an earlier control PC";
        case Stop::FrameLimit: return "the frame limit was reached";
    }
    return "unknown";
}

Stop walk(const CONTEXT* context, void* user, FrameSink sink) noexcept {
    if (context == nullptr) return Stop::ContextNull;
    if (sink == nullptr) return Stop::NoSink;
    const Image image = load_image();
    if (image.base == 0 || image.size == 0) return Stop::ImageNotSet;

    uint64_t seen[kMaxFrames] = {};
    CONTEXT current = *context;  // a copy: the caller's context (and the fault's record) is never written to
    Stop stop = Stop::FrameLimit;
    size_t frames = 0;

    for (;;) {
        if (frames >= kMaxFrames) {
            stop = Stop::FrameLimit;
            break;
        }
        // Everything below that is read from `current` (Rip for the repeat test, Rsp for the stack words, the
        // callee-saved registers) is taken before the unwind writes to it, which is what lets the unwind land in
        // `current` itself: a second CONTEXT local would be another 1 232 bytes of this frame, and the only thing the
        // previous state is still needed for afterwards is "did RSP move up", which an 8-byte local answers.
        const uint64_t pc = current.Rip;
        bool repeated = false;
        for (size_t i = 0; i < frames; ++i) repeated = repeated || seen[i] == pc;
        if (repeated) {
            stop = Stop::RepeatedFrame;
            break;
        }

        Frame frame{};
        frame.rip = pc;
        frame.rsp = current.Rsp;
        const bool in_image = pc >= image.base && pc - image.base < image.size;
        frame.in_image = in_image;
        frame.stack_word_count = read_stack_words(current.Rsp, frame.stack_words);
        fill_callee_saved(frame, current);

        const IMAGE_RUNTIME_FUNCTION_ENTRY* entry = nullptr;
        uint64_t control_pc = pc;
        if (in_image) {
            frame.rva = static_cast<uint32_t>(pc - image.base);
            entry = find_entry(image, pc, frames != 0, control_pc);
            if (entry != nullptr) {
                frame.has_unwind_info = true;
                frame.routine_rva = entry->BeginAddress;
            }
        }

        sink(user, frame);
        seen[frames] = pc;
        ++frames;

        if (!in_image) {
            // The first frame outside the mapping is host code (a shim, the CRT, kernel32) that this walker does not
            // name; the frames above it are beyond the chain it was asked to report, so print where the chain left
            // the image and stop rather than walk host frames with a title-oriented table.
            stop = frames == 1 ? Stop::FaultOutsideImage : Stop::LeftImage;
            break;
        }
        if (entry == nullptr) {
            stop = Stop::NoFunctionEntry;
            break;
        }
        if (frame.stack_word_count == 0) {
            stop = Stop::InvalidStack;
            break;
        }

        const uint64_t previous_rsp = current.Rsp;
        const UnwindStatus status = unwind_frame(image, *entry, control_pc, current);
        if (status == UnwindStatus::BadInfo) {
            stop = Stop::BadUnwindInfo;
            break;
        }
        if (status == UnwindStatus::Faulted) {
            stop = Stop::UnwindFault;
            break;
        }
        if (current.Rsp <= previous_rsp) {
            stop = Stop::StackNotAdvancing;
            break;
        }
    }
    return stop;
}

namespace {

// Fixed-buffer formatting. vsnprintf does not allocate and stops at the buffer end; the returned cursor is the
// reported number of characters, clamped to the buffer (a truncated line is still written, and still one line).
size_t appendf(char* buffer, size_t size, size_t used, const char* format, ...) noexcept {
    if (used + 1 >= size) return size - 1;
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer + used, size - used, format, arguments);
    va_end(arguments);
    if (written <= 0) return used;
    used += static_cast<size_t>(written);
    return used < size ? used : size - 1;
}

// Returns what the handle did with the bytes, because dump_all() has to answer "was this thread's report written"
// without asking the filesystem: a stale report at the same name says yes for a write that never happened.
[[nodiscard]] bool write_bytes(HANDLE file, const char* text, size_t bytes) noexcept {
    if (file == nullptr || file == INVALID_HANDLE_VALUE) return false;
    if (bytes == 0) return true;
    DWORD written = 0;
    return WriteFile(file, text, static_cast<DWORD>(bytes), &written, nullptr) != 0 && written == bytes;
}

struct DumpSink {
    HANDLE file;
    size_t frames;
    bool failed;  // the handle rejected a line; the walk continues so the frame count stays honest
};

// One frame is two lines: the RVA/routine/stack words, then the callee-saved registers as that frame saw them. The
// second line is what attributes a clobbered register: the walker restores the nonvolatiles while unwinding, so the
// first frame where a value changes names the frame whose call did not preserve it.
void write_frame(void* user, const Frame& frame) noexcept {
    auto& sink = *static_cast<DumpSink*>(user);
    char line[512];
    size_t used = 0;
    if (frame.in_image) {
        used = appendf(line, sizeof line, used, "frame %zu: image+0x%x", sink.frames, static_cast<unsigned>(frame.rva));
        if (frame.has_unwind_info) {
            used = appendf(line, sizeof line, used, " (routine image+0x%x)",
                           static_cast<unsigned>(frame.routine_rva));
        } else {
            used = appendf(line, sizeof line, used, " (no .pdata entry)");
        }
    } else {
        used = appendf(line, sizeof line, used, "frame %zu: 0x%016llx", sink.frames,
                       static_cast<unsigned long long>(frame.rip));
        used = appendf(line, sizeof line, used, " (outside the image)");
    }
    used = appendf(line, sizeof line, used, " stack:");
    if (frame.stack_word_count == 0) {
        used = appendf(line, sizeof line, used, " (unreadable)");
    } else {
        for (unsigned i = 0; i < frame.stack_word_count; ++i) {
            used = appendf(line, sizeof line, used, " %016llx",
                           static_cast<unsigned long long>(frame.stack_words[i]));
        }
    }
    used = appendf(line, sizeof line, used, "\n");
    if (!write_bytes(sink.file, line, used)) sink.failed = true;

    used = appendf(line, sizeof line, 0,
                   "         rbx=%016llx rbp=%016llx rsi=%016llx rdi=%016llx "
                   "r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
                   static_cast<unsigned long long>(frame.callee_saved[0]),
                   static_cast<unsigned long long>(frame.callee_saved[1]),
                   static_cast<unsigned long long>(frame.callee_saved[2]),
                   static_cast<unsigned long long>(frame.callee_saved[3]),
                   static_cast<unsigned long long>(frame.callee_saved[4]),
                   static_cast<unsigned long long>(frame.callee_saved[5]),
                   static_cast<unsigned long long>(frame.callee_saved[6]),
                   static_cast<unsigned long long>(frame.callee_saved[7]));
    if (!write_bytes(sink.file, line, used)) sink.failed = true;
    ++sink.frames;
}

}  // namespace

void set_image(void* base, size_t size, const IMAGE_RUNTIME_FUNCTION_ENTRY* functions, size_t count) noexcept {
    g_registered.store(false, std::memory_order_release);
    g_base.store(base, std::memory_order_relaxed);
    g_size.store(size, std::memory_order_relaxed);
    g_functions.store(functions, std::memory_order_relaxed);
    g_count.store(count, std::memory_order_relaxed);
    g_registered.store(base != nullptr && size != 0, std::memory_order_release);
}

ReportWrite dump_to_handle(const CONTEXT* context, HANDLE file, const char* tag) noexcept {
    ReportWrite result{0, false};
    if (file == nullptr || file == INVALID_HANDLE_VALUE) return result;

    const Image image = load_image();
    char header[256];
    size_t used = 0;
    bool ok = true;
    used = appendf(header, sizeof header, used, "[rt] crash report");
    if (context != nullptr) {
        used = appendf(header, sizeof header, used, ": rip 0x%016llx rsp 0x%016llx",
                       static_cast<unsigned long long>(context->Rip), static_cast<unsigned long long>(context->Rsp));
    }
    used = appendf(header, sizeof header, used, "\n");
    ok = write_bytes(file, header, used);

    // Which fault produced this file, before the frames: a directory of reports otherwise cannot be split into passes.
    if (tag != nullptr && tag[0] != '\0') {
        used = appendf(header, sizeof header, 0, "pass: %s\n", tag);
        ok = write_bytes(file, header, used) && ok;
    }

    used = 0;
    if (image.base != 0 && image.size != 0) {
        used = appendf(header, sizeof header, used,
                       "image base 0x%016llx size 0x%llx, %zu .pdata entr%s\n",
                       static_cast<unsigned long long>(image.base),
                       static_cast<unsigned long long>(image.size), image.count,
                       image.count == 1 ? "y" : "ies");
    } else {
        used = appendf(header, sizeof header, used, "no image registered\n");
    }
    ok = write_bytes(file, header, used) && ok;

    DumpSink sink{file, 0, false};
    const Stop stop = walk(context, &sink, &write_frame);

    used = appendf(header, sizeof header, 0, "[rt] crash report: %zu frame%s; walk stopped: %s\n", sink.frames,
                   sink.frames == 1 ? "" : "s", stop_text(stop));
    ok = write_bytes(file, header, used) && ok;

    result.frames = sink.frames;
    result.written = ok && !sink.failed;
    return result;
}

size_t dump(const CONTEXT* context, const wchar_t* path, const char* tag) noexcept {
    if (path == nullptr) return dump_to_handle(context, GetStdHandle(STD_ERROR_HANDLE), tag).frames;
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    const ReportWrite written = dump_to_handle(context, file, tag);
    CloseHandle(file);
    return written.frames;
}

namespace {

constexpr size_t kThreadPathChars = 512;
constexpr size_t kSummaryChars = 256;

// How a slot in the handle array answers "why is there no report file for this thread". The handle doubles as the state, so a
// second array (256 bytes) is not needed: CreateFileW never returns nullptr and never returns a usable handle equal to
// INVALID_HANDLE_VALUE, so nullptr = "the path did not fit, no call was made" and INVALID_HANDLE_VALUE = "CreateFileW
// refused it (missing directory, read-only report, ACL, share violation)".
constexpr HANDLE kPathTooLong = nullptr;

// "<directory>\crash_report.<tid>.txt". swprintf writes into the caller's buffer and reports truncation instead of
// allocating, so a long directory costs a short file name, never heap. A path that would be truncated is refused: a
// report written under a clipped name could be mistaken for another thread's.
bool thread_report_path(const wchar_t* directory, uint32_t tid, wchar_t (&path)[kThreadPathChars]) noexcept {
    const int written = std::swprintf(path, kThreadPathChars, L"%ls\\crash_report.%lu.txt", directory,
                                      static_cast<unsigned long>(tid));
    return written > 0 && static_cast<size_t>(written) < kThreadPathChars;
}

// The summary is ASCII like the per-thread reports (write_bytes on narrow text), so the same `grep`/`fread` that reads
// one reads the other. It is not UTF-16: a report nobody can grep on a build server is a report nobody reads.
void summary_captured(HANDLE file, uint32_t tid, size_t frames) noexcept {
    char line[kSummaryChars];
    const size_t used = appendf(line, sizeof line, 0, "thread %lu: %zu frame%s\n", static_cast<unsigned long>(tid),
                                frames, frames == 1 ? "" : "s");
    (void)write_bytes(file, line, used);
}

void summary_skipped(HANDLE file, uint32_t tid, const char* why) noexcept {
    char line[kSummaryChars];
    const size_t used = appendf(line, sizeof line, 0, "thread %lu: not captured (%s)\n",
                                static_cast<unsigned long>(tid), why);
    (void)write_bytes(file, line, used);
}

// Said once, on stderr, when the index name does not fit: a pass whose index was silently dropped looks exactly like a
// pass that found nothing to report.
constexpr char kNoIndexNote[] =
    "[rt] crash report: the index path is longer than the buffer; no crash_report.threads.txt for this pass\n";

}  // namespace

AllThreadsResult dump_all(const wchar_t* directory, const wchar_t* summary_path, const char* reason) noexcept {
    AllThreadsResult result{};
    if (directory == nullptr || directory[0] == L'\0') return result;

    // Headroom for the chain this call starts. SetThreadStackGuarantee is a guarantee, not a reserve: it makes no stack
    // bigger, it decides how much of the stack stays usable to the handler when a stack overflow happens - the one
    // fault this reporter exists for, and the one where the frame below would otherwise be its own second fault. Three
    // limits stated where it is asked for: it is per thread (every other thread needs its own, which is why
    // startup_trace.h asks for the same amount when it installs the handler); it only covers a fault raised after it
    // returns; and it cannot enlarge a reserve that is already too small, in which case the call fails and the pass
    // still runs - hence the ignored result. Measured peak chain: see the numbers in docs/crash_report.md.
    ULONG guaranteed = kStackGuaranteeBytes;
    (void)SetThreadStackGuarantee(&guaranteed);

    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    DWORD ids[kMaxDumpedThreads] = {};
    size_t count = 0;
    size_t past_cap = 0;

    // Enumerate first, act second, and suspend nothing while enumerating. The snapshot is closed before any thread is
    // suspended, so a failed suspend cannot leave a toolhelp handle open on the fault path, and the list is a fixed
    // stack array because the caller is an exception handler (see the header's allocation note).
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid) continue;
            if (entry.th32ThreadID == self) continue;
            if (count >= kMaxDumpedThreads) {
                ++past_cap;
                continue;
            }
            ids[count++] = entry.th32ThreadID;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);

    // `considered` is the number the caller's log line reads as "threads besides mine", so the truncated ones belong in
    // it: a 300-thread process used to print "256 besides the reporting one" and count the other 44 only in
    // `truncated`. written + skipped + truncated == considered holds for every pass.
    result.considered = count + past_cap;
    result.truncated = past_cap;

    // One path buffer for the whole pass: the index path and every per-thread path are built into it in turn, and the
    // index path is dead once the index handle exists. Two live wchar_t[512] buffers (the old default_summary plus the
    // loop's path) cost 1 KiB of this frame and bought nothing.
    wchar_t path[kThreadPathChars] = {};

    HANDLE summary = nullptr;
    if (summary_path != nullptr) {
        if (summary_path[0] != L'\0')
            summary = CreateFileW(summary_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (std::swprintf(path, kThreadPathChars, L"%ls\\crash_report.threads.txt", directory) > 0) {
        // No name given: the directory's own index. A caller that knows only where the reports go still gets the file
        // that says how many there are and what each one holds. A name that does not fit used to be dropped in silence,
        // which reads exactly like a pass that found nothing.
        summary = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    } else {
        (void)write_bytes(GetStdHandle(STD_ERROR_HANDLE), kNoIndexNote, sizeof kNoIndexNote - 1);
    }
    if (summary != nullptr && summary != INVALID_HANDLE_VALUE) {
        char head[kSummaryChars];
        size_t used = appendf(head, sizeof head, 0,
                              "[rt] crash report: all threads of process %lu, %zu besides the reporting one\n",
                              static_cast<unsigned long>(pid), result.considered);
        if (reason != nullptr && reason[0] != '\0') used = appendf(head, sizeof head, used, "pass: %s\n", reason);
        if (past_cap != 0)
            used = appendf(head, sizeof head, used, "%zu thread(s) past the cap of %zu are not in this pass\n",
                           past_cap, kMaxDumpedThreads);
        (void)write_bytes(summary, head, used);
    }

    // Every per-thread report file is opened while nothing is suspended. This is the fix for the deadlock, not tidying:
    // CreateFileW takes the process heap lock, so a pass that opens its files from inside its own suspend window can
    // park the thread holding that lock and then wait for it forever (measured: the pass stopped dead 29 files in).
    // Inside the window below there is no CreateFileW left - only writes to an already-open handle, which needs no
    // process heap. A file that ends up empty belongs to a thread that could not be captured, and the index says why.
    //
    // All of them in one burst, and that is not a stack decision open to revisiting. An earlier revision opened them in
    // batches of 32 to keep the handle array small (256 HANDLEs are 2 KiB of this frame) and argued that the batches
    // are serialised, so "the file is opened while nothing of this pass is parked" still held for every single open. The
    // argument is true per open and false for the pass: a later burst is a later chance to meet a heap lock nobody will
    // release, and it is issued only after the first 32 threads have been through the suspend window - which is exactly
    // when a lock taker gets its chance. Measured with 60 parked threads and a thread that takes
    // HeapLock(GetProcessHeap()) 15 ms into the pass and never lets go: one burst finished 3/3 runs with all 61
    // per-thread reports, while 32-file batching timed out
    // 3/3 runs, stopping inside the second batch's CreateFileW at 33/36/37/39/47/49/54 files with suspended == 0 - no
    // thread of this pass was parked when it hung, so it was not the R3 case. The frame is what kStackGuaranteeBytes
    // buys; a hang is not buyable at any frame size.
    HANDLE files[kMaxDumpedThreads] = {};
    for (size_t index = 0; index < count; ++index) {
        if (!thread_report_path(directory, ids[index], path)) {
            files[index] = kPathTooLong;
            continue;
        }
        files[index] = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    for (size_t index = 0; index < count; ++index) {
        const DWORD tid = ids[index];
        if (files[index] == kPathTooLong || files[index] == INVALID_HANDLE_VALUE) {
            ++result.skipped;
            summary_skipped(summary, tid, files[index] == kPathTooLong
                                             ? "the report path would be longer than 511 characters"
                                             : "report file could not be created");
            continue;
        }

        // OpenThread stays outside the window: nothing is suspended at this point, so a lock it takes can still be
        // released by whoever holds it. Its failure gets its own reason.
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
        if (thread == nullptr) {
            ++result.skipped;
            summary_skipped(summary, tid, "OpenThread failed");
            CloseHandle(files[index]);
            files[index] = INVALID_HANDLE_VALUE;
            continue;
        }

        // CONTROL gives Rip/Rsp (where the walk starts) and INTEGER gives the callee-saved registers the report
        // exists for. Without INTEGER every frame's rbx..r15 column would read zero and the attribution it carries
        // is lost.
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        ReportWrite written{0, false};
        bool suspended = false;
        bool captured = false;
        const char* failure = nullptr;

        __try {
            // The suspend window. Everything in it is a kernel call on a handle somebody else already opened:
            // SuspendThread, GetThreadContext, WriteFile. No file creation, no path joining, no C++ allocation,
            // and no wait on a lock whose holder this pass could be holding suspended.
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                failure = "SuspendThread failed";
            } else {
                suspended = true;
                captured = GetThreadContext(thread, &context) != 0;
                if (captured) {
                    written = dump_to_handle(&context, files[index], reason);
                } else {
                    failure = "GetThreadContext failed";
                }
            }
        } __finally {
            // The one action that has to happen on every way out. The caller's __except(EXCEPTION_EXECUTE_HANDLER)
            // does not unwind the stack, so an SEH fault on the reporting path would skip a ResumeThread() written
            // as the next statement - and the process keeps running, which leaves the thread parked forever. One
            // ResumeThread and nothing else in here: it cannot fail with an exception and it does not block.
            if (suspended) ResumeThread(thread);
        }

        CloseHandle(thread);
        CloseHandle(files[index]);
        files[index] = INVALID_HANDLE_VALUE;

        if (!captured) {
            ++result.skipped;
            summary_skipped(summary, tid, failure != nullptr ? failure : "the thread's CONTEXT could not be read");
            continue;
        }
        // The writer answers "did this report get written", not the filesystem: a stale report at the same name
        // says yes for a write that never happened, and the index then contradicts the file next to it.
        if (!written.written) {
            ++result.skipped;
            summary_skipped(summary, tid, "the report file rejected the write");
            continue;
        }
        ++result.written;
        summary_captured(summary, tid, written.frames);
    }

    if (summary != nullptr && summary != INVALID_HANDLE_VALUE) {
        char tail[kSummaryChars];
        const size_t used = appendf(tail, sizeof tail, 0, "considered %zu, written %zu, skipped %zu, truncated %zu\n",
                                    result.considered, result.written, result.skipped, result.truncated);
        (void)write_bytes(summary, tail, used);
        CloseHandle(summary);
    }
    return result;
}

}  // namespace rt::crash_report
