// Crash-time stack walk over the mapped image's own .pdata. See crash_unwind.h for the contract and why every read
// below is checked before it is made: the caller may be an exception handler running on a stack that the fault
// corrupted, and a reporter that faults (or allocates, or blocks) while reporting is worse than no reporter.

#include "crash_unwind.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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

        CONTEXT next = current;
        const UnwindStatus status = unwind_frame(image, *entry, control_pc, next);
        if (status == UnwindStatus::BadInfo) {
            stop = Stop::BadUnwindInfo;
            break;
        }
        if (status == UnwindStatus::Faulted) {
            stop = Stop::UnwindFault;
            break;
        }
        if (next.Rsp <= current.Rsp) {
            stop = Stop::StackNotAdvancing;
            break;
        }
        current = next;
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

void write_bytes(HANDLE file, const char* text, size_t bytes) noexcept {
    if (file == nullptr || file == INVALID_HANDLE_VALUE || bytes == 0) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(bytes), &written, nullptr);
}

struct DumpSink {
    HANDLE file;
    size_t frames;
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
    write_bytes(sink.file, line, used);

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
    write_bytes(sink.file, line, used);
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

size_t dump(const CONTEXT* context, const wchar_t* path, const void* user_data) noexcept {
    (void)user_data;  // reserved for the integrator; see the header
    const bool owns_file = path != nullptr;
    HANDLE file = owns_file
                      ? CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr)
                      : GetStdHandle(STD_ERROR_HANDLE);
    if (file == INVALID_HANDLE_VALUE) return 0;

    const Image image = load_image();
    char header[256];
    size_t used = 0;
    used = appendf(header, sizeof header, used, "[rt] crash report");
    if (context != nullptr) {
        used = appendf(header, sizeof header, used, ": rip 0x%016llx rsp 0x%016llx",
                       static_cast<unsigned long long>(context->Rip), static_cast<unsigned long long>(context->Rsp));
    }
    used = appendf(header, sizeof header, used, "\n");
    write_bytes(file, header, used);

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
    write_bytes(file, header, used);

    DumpSink sink{file, 0};
    const Stop stop = walk(context, &sink, &write_frame);

    used = appendf(header, sizeof header, 0, "[rt] crash report: %zu frame%s; walk stopped: %s\n", sink.frames,
                   sink.frames == 1 ? "" : "s", stop_text(stop));
    write_bytes(file, header, used);

    if (owns_file) CloseHandle(file);
    return sink.frames;
}

}  // namespace rt::crash_report
