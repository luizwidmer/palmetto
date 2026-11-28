// Opt-in startup trace: palmetto --trace FILE.
//
// One JSON object per line, flushed per event so the file survives a crash. Off by default; every entry point is a
// single relaxed load when it is off. Events:
//   {"seq":1,"t":0.000,"kind":"trace_open"}
//   {"seq":2,"t":0.001,"kind":"map","module":"a.dll","base":"0x...","size":"0x2000","relocated":false}
//   {"seq":3,"t":0.002,"kind":"tls_callback","module":"a.dll","at":"a.dll+0x1200","reason":1}
//   {"seq":4,"t":0.002,"kind":"dllmain","module":"a.dll","result":1}
//   {"seq":5,"t":0.010,"kind":"unimplemented_call","import":"x.dll!Foo","caller":"a.dll+0x3f10"}   (first call only)
//   {"seq":6,"t":0.011,"kind":"debug_string","text":"..."}
//   {"seq":7,"t":0.020,"kind":"exception","code":"0xc0000005","at":"a.dll+0x4a00","access":"read","address":"0x0",
//        "stack":["a.dll+0x4a00","a.dll+0x2210"]}
//   {"seq":8,"t":0.030,"kind":"exit","code":0}
//
// The exception handler is vectored, logs and returns EXCEPTION_CONTINUE_SEARCH: it never changes what the process
// does (except PALMETTO_SKIP_INT3, below).
// OutputDebugStringA/W raise DBG_PRINTEXCEPTION_C / DBG_PRINTEXCEPTION_WIDE_C when no debugger is attached, so the
// same handler records their text without a shim.
//
// The file can quote whatever the game prints: it stays on the player's machine (AGENTS.md, "No game data").
#pragma once

#include <windows.h>
#include <intrin.h>
#include <set>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include "../src/crash_unwind.h"
#include "../src/sse4a_emulation.h"
#include <cstdlib>
#include <mutex>
#include <string>

#include "shims.h"

namespace rt::startup_trace {

namespace detail {
// The mapped image's .pdata, registered once so a fault can be reported as a chain of image+RVA frames. The walker is
// given the mapped copy because unwind data is RVA-relative, and the pointers live as long as the process does.
inline void*& crash_image_base() {
    static void* base = nullptr;
    return base;
}
inline std::size_t& crash_image_size() {
    static std::size_t size = 0;
    return size;
}
inline const void*& crash_image_functions() {
    static const void* functions = nullptr;
    return functions;
}
inline std::size_t& crash_image_function_count() {
    static std::size_t count = 0;
    return count;
}

inline std::atomic<bool> on{false};
// Intentionally immortal: vectored exception handlers or late teardown paths can still run after normal statics would
// have been destroyed.
inline std::mutex& mutex() {
    static auto* m = new std::mutex;
    return *m;
}
inline FILE* file = nullptr;
inline unsigned long long seq = 0;
inline std::chrono::steady_clock::time_point start;
inline void* veh = nullptr;
inline thread_local bool in_handler = false;
// With nothing to handle it, OutputDebugStringW's DBG_PRINTEXCEPTION_WIDE_C falls back to OutputDebugStringA on its
// own ANSI copy (ExceptionInformation[3]), a second, lossy exception for the same text. Every debug string on the
// thread resets this, so only the exception right after the wide one, with that same pointer, is skipped.
inline thread_local ULONG_PTR wide_fallback = 0;

inline void append_escaped(std::string& out, const char* s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += static_cast<char>(c);  // UTF-8 passes through
                }
        }
    }
}
}  // namespace detail

inline bool enabled() noexcept { return detail::on.load(std::memory_order_acquire); }

// A JSON string literal, quotes included.
inline std::string quote(const std::string& s) {
    std::string out = "\"";
    detail::append_escaped(out, s.data(), s.size());
    out += '"';
    return out;
}

inline std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// "module+0xRVA" for a mapped image or a host module, else the bare address.
inline std::string where(uint64_t address) { return shims::module_offset(address); }

// `fields` is the rest of the object, already JSON: `"module":"a.dll","result":1`. Empty is allowed.
inline void event(const char* kind, const std::string& fields = {}) {
    if (!enabled()) return;
    const std::lock_guard lock(detail::mutex());
    if (!detail::file) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - detail::start).count();
    std::fprintf(detail::file, "{\"seq\":%llu,\"t\":%.6f,\"kind\":\"%s\"%s%s}\n", ++detail::seq, t, kind,
                 fields.empty() ? "" : ",", fields.c_str());
    std::fflush(detail::file);
}

namespace detail {
constexpr DWORD kDbgPrint = 0x40010006;      // DBG_PRINTEXCEPTION_C
constexpr DWORD kDbgPrintWide = 0x4001000A;  // DBG_PRINTEXCEPTION_WIDE_C
constexpr DWORD kCppException = 0xE06D7363;  // MSVC throw: logged with its address, never its payload
constexpr int kMaxFrames = 16;

// Whether an exception code deserves a stack report. This handler is first-chance, so the filter is the only thing that
// separates "the process is dying" from "the program threw, handled it, and carried on". A report costs one walk of the
// faulting thread; the every-thread pass costs a suspend/context/write per thread - 45-62 ms on a 60-82 thread process,
// measured - and it is paid on the thread that threw. Without this gate three handled C++ throws produced three passes,
// each overwriting the last, so the directory described the last throw rather than the fault.
// Excluded by name: the two DBG_PRINT codes, a breakpoint, a single step, the SetThreadName convention, and a C++
// throw - 0xE06D7363 carries the SEVERE bit like a real fault, so it has to be named explicitly rather than filtered.
inline bool worth_crash_report(DWORD code) noexcept {
    switch (code) {
        case kCppException:
        case kDbgPrint:
        case kDbgPrintWide:
        case EXCEPTION_BREAKPOINT:
        case EXCEPTION_SINGLE_STEP:
        case 0x406D1388:  // SetThreadName: RaiseException(0x406D1388, ...) from a debug helper
            return false;
        default:
            break;
    }
    return (code & 0xC0000000ul) == 0xC0000000ul;
}

// One every-thread pass per process. The first fatal code wins; later ones are still traced (as
// crash_report_all_suppressed) but do not overwrite the reports of the pass that caught the process breaking, which is
// the only pass whose file set is self-consistent.
inline std::atomic<bool>& all_threads_taken() noexcept {
    static std::atomic<bool> taken{false};
    return taken;
}
inline std::atomic<unsigned long>& all_threads_code() noexcept {
    static std::atomic<unsigned long> code{0};
    return code;
}

inline std::string stack_from(const CONTEXT& start_context) {
    CONTEXT c = start_context;
    std::string out = "[";
    for (int i = 0; i < kMaxFrames && c.Rip; ++i) {
        if (i) out += ',';
        out += quote(where(c.Rip));
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!fn) {  // a leaf function: the return address is at [rsp]
            if (!c.Rsp || IsBadReadPtr(reinterpret_cast<void*>(c.Rsp), 8)) break;
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &frame, nullptr);
    }
    return out + "]";
}

inline std::string utf8_from_wide(const wchar_t* w, int len) {
    const int bytes = len > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr) : 0;
    std::string s(bytes > 0 ? bytes : 0, '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), bytes, nullptr, nullptr);
    return s;
}

// OutputDebugStringA text is in the ANSI code page, not UTF-8: convert it, or a byte such as 0xE9 would land in the
// file as is and break the JSON lines for a strict UTF-8 reader.
inline std::string utf8_from_ansi(const char* a, int len) {
    const int chars = len > 0 ? MultiByteToWideChar(CP_ACP, 0, a, len, nullptr, 0) : 0;
    if (chars <= 0) return {};
    std::wstring w(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_ACP, 0, a, len, w.data(), chars);
    return utf8_from_wide(w.data(), chars);
}

inline std::string debug_text(const EXCEPTION_RECORD& r) {
    // Parameters: [0] length in characters including the terminator, [1] the string.
    if (r.NumberParameters < 2 || !r.ExceptionInformation[1]) return {};
    const size_t n = static_cast<size_t>(r.ExceptionInformation[0]);
    if (r.ExceptionCode == kDbgPrintWide) {
        const auto* w = reinterpret_cast<const wchar_t*>(r.ExceptionInformation[1]);
        return utf8_from_wide(w, static_cast<int>(n ? n - 1 : wcslen(w)));
    }
    const auto* a = reinterpret_cast<const char*>(r.ExceptionInformation[1]);
    return utf8_from_ansi(a, static_cast<int>(n ? n - 1 : strlen(a)));
}

// Defined below in this namespace; declared here so the fault path can call it before its definition.
inline void save_crash_ranges(const EXCEPTION_POINTERS&);

inline void log_exception(const EXCEPTION_POINTERS& info) {
    save_crash_ranges(info);   // self-gated by PALMETTO_CRASH_SAVE; independent of the crash report switch
    const EXCEPTION_RECORD& r = *info.ExceptionRecord;
    try {
        if (r.ExceptionCode == kDbgPrint || r.ExceptionCode == kDbgPrintWide) {
            const ULONG_PTR fallback = wide_fallback;
            const bool wide = r.ExceptionCode == kDbgPrintWide;
            wide_fallback = wide && r.NumberParameters >= 4 ? r.ExceptionInformation[3] : 0;
            if (!wide && fallback && r.NumberParameters >= 2 && r.ExceptionInformation[1] == fallback) return;
            std::string text = debug_text(r);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            event("debug_string", "\"text\":" + quote(text));
        } else {
            std::string f = "\"code\":" + quote(hex(r.ExceptionCode)) +
                            ",\"at\":" + quote(where(reinterpret_cast<uint64_t>(r.ExceptionAddress)));
            if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2) {
                const auto op = r.ExceptionInformation[0];
                f += ",\"access\":" + quote(op == 0 ? "read" : op == 1 ? "write" : "execute") +
                     ",\"address\":" + quote(hex(r.ExceptionInformation[1]));
            }
            if (r.ExceptionCode == kCppException) f += ",\"cpp\":true";
            if (info.ContextRecord) {
                f += ",\"stack\":" + stack_from(*info.ContextRecord);                // PALMETTO_DUMP_REGS=1: the argument registers at the fault. A stop whose stack frame looks impossible
                // (a null test that should have branched) is usually a jump into a shared tail, and the registers say
                // which values the failing instruction actually saw.
                static const bool dump_regs = GetEnvironmentVariableW(L"PALMETTO_DUMP_REGS", nullptr, 0) != 0;
                if (dump_regs) {
                    const CONTEXT& c = *info.ContextRecord;
                    f += ",\"regs\":{";
                    f += "\"rcx\":" + quote(hex(c.Rcx)) + ",\"rdx\":" + quote(hex(c.Rdx)) +
                         ",\"r8\":" + quote(hex(c.R8)) + ",\"r9\":" + quote(hex(c.R9)) +
                         ",\"rdi\":" + quote(hex(c.Rdi)) + ",\"rsi\":" + quote(hex(c.Rsi)) +
                         ",\"rsp\":" + quote(hex(c.Rsp)) + ",\"rbp\":" + quote(hex(c.Rbp)) +
                         ",\"rbx\":" + quote(hex(c.Rbx)) + ",\"r12\":" + quote(hex(c.R12)) +
                         ",\"r13\":" + quote(hex(c.R13)) + ",\"r14\":" + quote(hex(c.R14)) +
                         ",\"r15\":" + quote(hex(c.R15)) +
                         ",\"rax\":" + quote(hex(c.Rax)) + ",\"r10\":" + quote(hex(c.R10)) +
                         ",\"r11\":" + quote(hex(c.R11));
                    f += "}";
                    // PALMETTO_FAULT_MEM=1: the memory the faulting instruction was using, not just the registers. A
                    // value that looks wrong in a register is either an object field or the raw argument; reading the
                    // surrounding bytes tells those apart (a clobbered first qword with intact neighbours is an
                    // object, a run of size-like words is a buffer). rcx and the other argument registers are dumped;
                    // unmapped or guarded ranges are skipped instead of faulting inside the handler.
                    static const bool dump_mem = GetEnvironmentVariableW(L"PALMETTO_FAULT_MEM", nullptr, 0) != 0;
                    if (dump_mem) {
                        struct Region {
                            const char* name;
                            DWORD64 address;
                            int bytes;
                        };
                        const Region regions[] = {
                            {"rcx", c.Rcx, 0x100}, {"rcx_before", c.Rcx - 0x40, 0x40},
                            {"rdx", c.Rdx, 0x40},  {"r8", c.R8, 0x40},     {"r9", c.R9, 0x40},
                            {"rdi", c.Rdi, 0x40},  {"rsi", c.Rsi, 0x40},   {"r14", c.R14, 0x40},
                            {"r15", c.R15, 0x40},  {"rsp", c.Rsp, 0x80},   {"rax", c.Rax, 0x40},
                        };
                        static const char digits[] = "0123456789abcdef";
                        bool first_region = true;
                        auto append_region = [&](const char* name, DWORD64 address, int requested) {
                            MEMORY_BASIC_INFORMATION mbi{};
                            if (!address ||
                                VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                                mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                                return;
                            const DWORD64 region_end = reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize;
                            int n = requested;
                            if (address + static_cast<DWORD64>(n) > region_end)
                                n = static_cast<int>(region_end - address);
                            std::string bytes;
                            bytes.reserve(static_cast<size_t>(n) * 2);
                            const auto* p = reinterpret_cast<const unsigned char*>(address);
                            for (int b = 0; b < n; ++b) {
                                bytes += digits[p[b] >> 4];
                                bytes += digits[p[b] & 15];
                            }
                            f += first_region ? ",\"mem\":{" : ",";
                            first_region = false;
                            f += quote(name) + ":{\"addr\":" + quote(hex(address)) + ",\"hex\":" + quote(bytes) + "}";
                        };
                        for (const Region& region : regions) append_region(region.name, region.address, region.bytes);
                        // PALMETTO_FAULT_MEM_RVA=<hex rva>[,<hex rva>...]: image-relative windows, for a structure the
                        // fault depends on but no register points at (a singleton whose field holds the bad object).
                        if (detail::crash_image_base()) {
                            const char* list = std::getenv("PALMETTO_FAULT_MEM_RVA");
                            char name[24];
                            for (int slot = 0; list && *list && slot < 4; ++slot) {
                                char* end = nullptr;
                                const auto rva = std::strtoull(list, &end, 16);
                                if (end == list) break;
                                std::snprintf(name, sizeof(name), "rva_%llx", static_cast<unsigned long long>(rva));
                                append_region(name, reinterpret_cast<DWORD64>(detail::crash_image_base()) + rva, 0x100);
                                list = (*end == ',') ? end + 1 : "";
                            }
                        }
                        if (!first_region) f += "}";
                    }
                    // The first eight TlsAlloc cells of this thread and the first qword each points at: a mapped image's
                    // index-0 TLS reads go through one of them (the loader's view), so a wrong view or view[0] shows up
                    // here next to the faulting registers.
                    if (std::getenv("PALMETTO_TRACE_TLS")) {
                        f += ",\"tls\":[";
                        for (int cell = 0; cell < 8; ++cell) {
                            const auto address = static_cast<std::uintptr_t>(__readgsqword(0x1480 + 8 * cell));
                            std::uint64_t first = 0;
                            bool readable = false;
                            if (address) {
                                MEMORY_BASIC_INFORMATION mbi{};
                                readable = VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) ==
                                               sizeof(mbi) &&
                                           mbi.State == MEM_COMMIT &&
                                           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
                                if (readable) first = *reinterpret_cast<const std::uint64_t*>(address);
                            }
                            if (cell) f += ",";
                            f += "{\"cell\":" + std::to_string(cell) + ",\"ptr\":" + quote(hex(address)) +
                                 ",\"first\":" + quote(hex(first)) + "}";
                        }
                        f += "]";
                    }
                }
            }
            event("exception", f);
    // PALMETTO_CRASH_REPORT=<file>: a .pdata-walked report of the faulting thread.
    // PALMETTO_CRASH_REPORT_ALL=<directory>: the same walk for every other thread of the process, one file each, plus
    // that directory's crash_report.threads.txt index. Both stay behind switches because they touch the filesystem on
    // the fault path; the walks themselves never allocate. Both are also gated on the exception code
    // (worth_crash_report): a first-chance handler that reports every throw describes the last throw, not the fault.
    if (detail::crash_image_base() && detail::worth_crash_report(r.ExceptionCode)) {
        wchar_t path[512] = {};
        wchar_t directory[512] = {};
        const DWORD own = GetEnvironmentVariableW(L"PALMETTO_CRASH_REPORT", path, 512);
        const DWORD all = GetEnvironmentVariableW(L"PALMETTO_CRASH_REPORT_ALL", directory, 512);
        const bool wants_own = own > 0 && own < 512;
        const bool wants_all = all > 0 && all < 512;
        if (wants_own || wants_all) {
            rt::crash_report::set_image(detail::crash_image_base(), detail::crash_image_size(),
                                        static_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(
                                            detail::crash_image_functions()),
                                        detail::crash_image_function_count());
        }
        // Which fault produced a file, written into every report and into the index: without it a directory of reports
        // is a set of files that cannot be attributed to a pass.
        char pass[64] = {};
        std::snprintf(pass, sizeof pass, "exception 0x%08lx", static_cast<unsigned long>(r.ExceptionCode));
        // The faulting thread first: it is the one the caller asked about, and the all-threads pass below suspends and
        // resumes the others one at a time.
        if (wants_own && info.ContextRecord) rt::crash_report::dump(info.ContextRecord, path, pass);
        if (wants_all) {
            // The latch: the first fatal code takes the pass. Later ones are traced, not dumped, so they cannot
            // overwrite the reports of the pass that caught the process breaking.
            bool expected = false;
            if (detail::all_threads_taken().compare_exchange_strong(expected, true)) {
                detail::all_threads_code().store(static_cast<unsigned long>(r.ExceptionCode),
                                                 std::memory_order_relaxed);
                const rt::crash_report::AllThreadsResult result =
                    rt::crash_report::dump_all(directory, nullptr, pass);
                // The four counts are the only evidence the pass ran at all: when the directory cannot be written they
                // are all zero, and dropping them (as this call used to) leaves the whole pass silent.
                event("crash_report_all", "\"code\":" + quote(hex(r.ExceptionCode)) + ",\"pass\":1" +
                                              ",\"considered\":" + std::to_string(result.considered) +
                                              ",\"written\":" + std::to_string(result.written) +
                                              ",\"skipped\":" + std::to_string(result.skipped) +
                                              ",\"truncated\":" + std::to_string(result.truncated));
            } else {
                event("crash_report_all_suppressed", "\"code\":" + quote(hex(r.ExceptionCode)) + ",\"already\":" +
                                                         quote(hex(detail::all_threads_code().load(
                                                             std::memory_order_relaxed))));
            }
        }
    }
        }
    } catch (...) {
        // Out of memory while formatting: drop the event rather than disturb the process.
    }
}

// PALMETTO_SKIP_INT3 (diagnostic, with --trace): an int3 in guest code is logged once per address and stepped over,
// so a boot can be followed past the title's soft asserts (an int3 followed by code that carries on). Off by default:
// a console title never reaches one in a correct run, so each skipped address is a finding, not a fix.
// The console's GPU is AMD, and the title uses AMD-only instructions that an Intel host does not implement. CLZERO
// (0F 01 FC) zeroes the 64-byte cache line holding RAX; the title uses it as a fast block clear (measured: a loop of
// clzero / add rax,0x40 / cmp / jb zeroing a structure at startup). Emulate exactly that when the host raises
// STATUS_ILLEGAL_INSTRUCTION, and report anything else once so an unexpected opcode is visible rather than silently
// skipped.
// A poke that must fire on every pass: when the int3 is hit the original byte is restored, the thread is set to
// single-step that instruction, and the int3 is written back on the step. Without this a probe only ever sees the first
// pass, which is useless when the interesting pass is a later one.
inline std::uintptr_t& repoke_address() {
    static thread_local std::uintptr_t address = 0;
    return address;
}

// PALMETTO_WATCH_RVA_WRITE=<hex rva>: name the writer of an image global a later fault depends on. The mapped image's
// 4 KiB page that holds that rva is made read-only; every store into it faults, is logged with the store instruction
// (module+rva) and registers, is single-stepped with the page writable, and the page is protected again on the step.
// This is how the store that put a heap pointer into a channel field was named; it stays on the image's own pages, which
// belong to nobody but the title, so the trap cannot re-enter the runtime's allocator.
inline std::uintptr_t& watch_a_page() {
    static std::uintptr_t page = 0;
    return page;
}
inline std::uintptr_t& watch_a_cell() {
    static std::uintptr_t cell = 0;
    return cell;
}
// The three pending values are per-thread: the watch traps on any guest thread, and a shared copy would let two
// threads' faults overwrite each other's single-step bookkeeping (a stray trap flag then surfaces as an unhandled
// single step in host code).
inline std::uintptr_t& watch_pending_page() {
    static thread_local std::uintptr_t page = 0;
    return page;
}
inline std::uintptr_t& watch_pending_address() {
    static thread_local std::uintptr_t address = 0;
    return address;
}
inline std::uintptr_t& watch_pending_rip() {
    static thread_local std::uintptr_t rip = 0;
    return rip;
}

inline void setup_watch_write() {
    const char* spec = std::getenv("PALMETTO_WATCH_RVA_WRITE");
    if (!spec || !crash_image_base()) return;
    const auto rva = std::strtoull(spec, nullptr, 16);
    const auto cell = reinterpret_cast<std::uintptr_t>(crash_image_base()) + rva;
    const std::uintptr_t page = cell & ~static_cast<std::uintptr_t>(0xFFF);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READONLY, &old)) {
        std::fprintf(stderr, "[rt] watch write: image+0x%llx page is not writable (%lu)\n",
                     static_cast<unsigned long long>(rva), GetLastError());
        return;
    }
    watch_a_page() = page;
    watch_a_cell() = cell;
    std::fprintf(stderr, "[rt] watching writes to image+0x%llx..0x%llx (cell image+0x%llx at %p)\n",
                 static_cast<unsigned long long>(page - reinterpret_cast<std::uintptr_t>(crash_image_base())),
                 static_cast<unsigned long long>(page + 0xFFF - reinterpret_cast<std::uintptr_t>(crash_image_base())),
                 static_cast<unsigned long long>(rva), reinterpret_cast<void*>(cell));
}

inline std::string hex_bytes(const void* address, int bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    const auto* p = static_cast<const unsigned char*>(address);
    out.reserve(static_cast<size_t>(bytes) * 2);
    for (int i = 0; i < bytes; ++i) {
        out += digits[p[i] >> 4];
        out += digits[p[i] & 15];
    }
    return out;
}

inline bool readable_range(std::uintptr_t address, int bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return address + static_cast<std::uintptr_t>(bytes) <= end;
}

inline std::string all_regs(const CONTEXT& c) {
    return "\"rip\":" + quote(where(c.Rip)) + ",\"rcx\":" + quote(hex(c.Rcx)) + ",\"rdx\":" + quote(hex(c.Rdx)) +
           ",\"r8\":" + quote(hex(c.R8)) + ",\"r9\":" + quote(hex(c.R9)) + ",\"rax\":" + quote(hex(c.Rax)) +
           ",\"rbx\":" + quote(hex(c.Rbx)) + ",\"rsi\":" + quote(hex(c.Rsi)) + ",\"rdi\":" + quote(hex(c.Rdi)) +
           ",\"r10\":" + quote(hex(c.R10)) + ",\"r11\":" + quote(hex(c.R11)) + ",\"r12\":" + quote(hex(c.R12)) +
           ",\"r13\":" + quote(hex(c.R13)) + ",\"r14\":" + quote(hex(c.R14)) + ",\"r15\":" + quote(hex(c.R15));
}

// Returns true when it handled the exception (a write fault in the watched page, or the single-step that follows one).
inline bool handle_watch_write(EXCEPTION_POINTERS* info) {
    if (!info->ExceptionRecord || !info->ContextRecord) return false;
    if (!watch_a_page()) return false;
    CONTEXT& c = *info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2 && info->ExceptionRecord->ExceptionInformation[0] == 1) {
        const auto address = static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        if (address < watch_a_page() || address >= watch_a_page() + 0x1000) return false;
        watch_pending_page() = watch_a_page();
        watch_pending_address() = address;
        watch_pending_rip() = static_cast<std::uintptr_t>(c.Rip);
        if (address == watch_a_cell())
            std::fprintf(stderr, "[rt] watch write cell: store at %s\n", where(c.Rip).c_str());
        event("watch_write", "\"address\":" + quote(hex(address)) + "," + all_regs(c));
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(watch_a_page()), 0x1000, PAGE_READWRITE, &old);
        c.EFlags |= 0x100;   // trap after the store, so the page can be protected again
        return true;
    }
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && watch_pending_page()) {
        const std::uintptr_t page = watch_pending_page();
        const std::uintptr_t address = watch_pending_address();
        const std::uintptr_t rip = watch_pending_rip();
        watch_pending_page() = 0;
        watch_pending_address() = 0;
        watch_pending_rip() = 0;
        c.EFlags &= ~static_cast<DWORD>(0x100);
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READONLY, &old);
        if (address == watch_a_cell()) {
            const auto pointed = *reinterpret_cast<std::uint64_t*>(address);
            std::string f = "\"at\":" + quote(where(address)) + ",\"rip\":" + quote(where(rip)) +
                            ",\"value\":" + quote(hex(pointed));
            if (readable_range(static_cast<std::uintptr_t>(pointed), 0x40))
                f += ",\"memory\":" + quote(
                         hex_bytes(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(pointed)), 0x40));
            event("watch_write_cell", f);
        }
        return true;
    }
    // A single step with no per-thread pending record is one of ours whose record a sibling thread could not see
    // (or a replayed step): clear the trap flag instead of reporting it as a guest fault.
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (c.EFlags & 0x100)) {
        c.EFlags &= ~static_cast<DWORD>(0x100);
        return true;
    }
    return false;
}

// PALMETTO_POKE_HOST=<hex rva>[,<hex rva>...]: the same one-shot probe, but in the runtime's own image rather than the
// guest's (rva from palmetto.exe's base). A host function that writes a value the guest later dereferences can be caught
// exactly: the registers say what it was about to write and the guest's stack words above it name the guest call site.
inline std::uintptr_t* host_poke_addresses() {
    static std::uintptr_t addresses[4] = {};
    return addresses;
}
inline unsigned char* host_poke_originals() {
    static unsigned char originals[4] = {};
    return originals;
}
inline void setup_host_pokes() {
    const char* spec = std::getenv("PALMETTO_POKE_HOST");
    if (!spec) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (unsigned slot = 0; slot < 4 && *spec; ++slot) {
        char* end = nullptr;
        const auto rva = std::strtoull(spec, &end, 16);
        if (end == spec) break;
        const auto address = base + rva;
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old)) {
            host_poke_addresses()[slot] = address;
            host_poke_originals()[slot] = *reinterpret_cast<unsigned char*>(address);
            *reinterpret_cast<unsigned char*>(address) = 0xCC;
            VirtualProtect(reinterpret_cast<void*>(address), 1, old, &old);
            std::fprintf(stderr, "[rt] host poke at palmetto.exe+0x%llx (%p)\n",
                         static_cast<unsigned long long>(rva), reinterpret_cast<void*>(address));
        } else {
            std::fprintf(stderr, "[rt] host poke at palmetto.exe+0x%llx failed (%lu)\n",
                         static_cast<unsigned long long>(rva), GetLastError());
        }
        spec = (*end == ',') ? end + 1 : "";
    }
}

inline bool handle_host_poke(EXCEPTION_POINTERS* info) {
    if (!info->ExceptionRecord || !info->ContextRecord) return false;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return false;
    const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (host_poke_addresses()[slot] != at) continue;
        CONTEXT& c = *info->ContextRecord;
        std::fprintf(stderr, "[rt] host poke hit: palmetto.exe+0x%llx rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx\n",
                     static_cast<unsigned long long>(at - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))),
                     static_cast<unsigned long long>(c.Rax), static_cast<unsigned long long>(c.Rcx),
                     static_cast<unsigned long long>(c.Rdx), static_cast<unsigned long long>(c.R8),
                     static_cast<unsigned long long>(c.R9));
        // The guest stack above this host frame: the return address of the guest's indirect call through the proxy
        // vtable is one of these words. Image words are named, the rest are raw, so the caller can be disassembled.
        const auto image = reinterpret_cast<std::uintptr_t>(crash_image_base());
        const auto image_end = image + crash_image_size();
        std::string stack = "\"stack\":[";
        const auto* words = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(c.Rsp));
        for (int i = 0; i < 192; ++i) {
            const std::uint64_t word = words[i];
            if (i) stack += ',';
            if (word >= image && word < image_end)
                stack += quote(where(word));
            else
                stack += quote(hex(word));
        }
        stack += "]";
        event("host_poke", "\"at\":" + quote(where(at)) + "," + all_regs(c) + "," + stack);
        // Restore this instruction byte and single-step it, then put the probe back (the repoke path does that).
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<unsigned char*>(at) = host_poke_originals()[slot];
            VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
        }
        repoke_address() = at;
        c.EFlags |= 0x100;
        return true;
    }
    return false;
}

inline bool handle_repoke(EXCEPTION_POINTERS* info) {
    if (!info->ExceptionRecord || !info->ContextRecord) return false;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return false;
    const std::uintptr_t address = repoke_address();
    if (!address) return false;
    CONTEXT& c = *info->ContextRecord;
    c.EFlags &= ~static_cast<DWORD>(0x100);
    DWORD old = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old)) {
        *reinterpret_cast<unsigned char*>(address) = 0xCC;
        VirtualProtect(reinterpret_cast<void*>(address), 1, old, &old);
    }
    repoke_address() = 0;
    return true;
}

// PALMETTO_CRASH_SAVE=<hex rva>:<hex length>[,<hex rva>:<hex length>...] writes those parts of the mapped image to files
// beside the crash report (or to PALMETTO_CRASH_SAVE_DIR) when the title faults. The crash report names the frame; this
// keeps the memory that frame was reading, so a wrong pointer can be told apart from a wrong object after the process is
// gone. Runs on the fault path, so it allocates nothing: a stack buffer, CreateFileW and WriteFile.
inline void save_crash_ranges(const EXCEPTION_POINTERS& info) {
    const char* spec = std::getenv("PALMETTO_CRASH_SAVE");
    if (!spec) return;
    char dir[MAX_PATH] = {};
    const DWORD dir_length = GetEnvironmentVariableA("PALMETTO_CRASH_SAVE_DIR", dir, MAX_PATH);
    if (dir_length == 0 || dir_length >= MAX_PATH) {
        if (GetTempPathA(MAX_PATH, dir) == 0) return;
    }
    const auto image = reinterpret_cast<std::uintptr_t>(crash_image_base());
    const char* at = spec;
    while (*at) {
        char* end = nullptr;
        const auto rva = std::strtoull(at, &end, 16);
        if (end == at) break;
        if (*end != ':') break;
        at = end + 1;
        const auto length = std::strtoull(at, &end, 16);
        at = (*end == ',') ? end + 1 : end;
        if (length == 0 || length > (64ull << 20)) continue;
        const auto address = image + rva;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(bytes, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            continue;   // unreadable page: skip it rather than fault inside the handler
        }
        char path[MAX_PATH] = {};
        std::snprintf(path, sizeof(path), "%s\\crash_image_%llx_%llx.bin", dir,
                      static_cast<unsigned long long>(rva), static_cast<unsigned long long>(length));
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        std::uint8_t buffer[64 << 10];
        std::size_t left = static_cast<std::size_t>(length);
        const std::uint8_t* cursor = bytes;
        while (left) {
            const std::size_t chunk = left < sizeof(buffer) ? left : sizeof(buffer);
            DWORD written = 0;
            if (!WriteFile(file, cursor, static_cast<DWORD>(chunk), &written, nullptr) || written != chunk) break;
            cursor += chunk;
            left -= chunk;
        }
        CloseHandle(file);
    }
}

inline bool emulate_amd_instruction(EXCEPTION_POINTERS* info) {
    if (!info->ExceptionRecord || info->ExceptionRecord->ExceptionCode != STATUS_ILLEGAL_INSTRUCTION ||
        !info->ContextRecord)
        return false;
    auto& c = *info->ContextRecord;
    const auto* at = reinterpret_cast<const unsigned char*>(c.Rip);
    if (!at) return false;
    if (at[0] == 0x0F && at[1] == 0x01 && at[2] == 0xFC) {   // clzero
        const auto address = static_cast<std::uintptr_t>(c.Rax) & ~static_cast<std::uintptr_t>(63);
        auto* line = reinterpret_cast<volatile unsigned char*>(address);
        for (int i = 0; i < 64; ++i) line[i] = 0;
        c.Rip += 3;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] emulated the AMD-only clzero instruction (zeroed a 64-byte line)\n");
        return true;
    }
    if (at[0] == 0x0F && at[1] == 0x01 && (at[2] == 0xFA || at[2] == 0xFB)) {   // monitorx / mwaitx
        // MONITORX arms a monitor on the address in RAX for MWAITX; the pair is how the title's fence wait loop polls a
        // completed-value cell (docs/fence_slot12.md). Neither instruction exists on an Intel host, so the monitor is a
        // no-op and the wait becomes a pause that yields: the loop's own memory comparison still decides readiness.
        if (at[2] == 0xFB) {
            static std::atomic<unsigned> yields{0};
            if ((yields.fetch_add(1, std::memory_order_relaxed) & 0xFF) == 0) SwitchToThread();
            _mm_pause();
        }
        c.Rip += 3;
        static std::atomic<bool> logged_mwait{false};
        if (!logged_mwait.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] emulated the AMD-only monitorx/mwaitx pair (the host has neither)\n");
        return true;
    }
    // AMD SSE4a bit-field instructions (EXTRQ / INSERTQ). The title's half-float packing on the session-13 path uses
    // INSERTQ xmm1, xmm2, length, index and an Intel host raises #UD at image+0x2E7FF1 (measured in round 22). Decode the
    // instruction from the faulting bytes and apply it to the CONTEXT's XMM registers; a field crossing bit 64 is refused
    // so the generic illegal-instruction report below still fires.
    if (at[0] == 0xF2 || at[0] == 0x66) {
        std::size_t available = 3;
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(at, &region, sizeof(region)) == sizeof(region)) {
            const auto region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            const auto here = reinterpret_cast<std::uintptr_t>(at);
            if (region_end > here)
                available = static_cast<std::size_t>(region_end - here) < 7u
                                ? static_cast<std::size_t>(region_end - here)
                                : 7u;
        }
        palmetto::sse4a::Instruction sse4a_instruction;
        if (palmetto::sse4a::decode(at, available, sse4a_instruction)) {
            auto& source = c.FltSave.XmmRegisters[sse4a_instruction.source & 15u];
            auto& destination = c.FltSave.XmmRegisters[sse4a_instruction.destination & 15u];
            const std::uint64_t source_low = source.Low;
            const std::uint64_t source_high = source.High;
            const palmetto::sse4a::Field field =
                palmetto::sse4a::resolve(sse4a_instruction, source_low, source_high);
            std::uint64_t destination_low = destination.Low;
            if (palmetto::sse4a::apply(sse4a_instruction, field, source_low, destination_low)) {
                destination.Low = destination_low;
                c.Rip += sse4a_instruction.size;
                static std::atomic<unsigned> sse4a_emulated{0};
                if ((sse4a_emulated.fetch_add(1, std::memory_order_relaxed) & 0xFFu) == 0)
                    std::fprintf(stderr,
                                 "[rt] emulated an AMD-only SSE4a %s (%u bit-field instruction(s) so far; host has no SSE4a)\n",
                                 sse4a_instruction.op == palmetto::sse4a::Op::insert ? "INSERTQ" : "EXTRQ",
                                 sse4a_emulated.load(std::memory_order_relaxed));
                return true;
            }
        }
    }
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 4)
        std::fprintf(stderr, "[rt] illegal instruction on the host: %02x %02x %02x at %p\n", at[0], at[1], at[2],
                     reinterpret_cast<const void*>(c.Rip));
    return false;
}

// PALMETTO_POKE_LEA=<rva>: a diagnostic for one shaped instruction. The loader writes an int3 over the guest's
// `lea rbx,[rax+0x12a8]` at that RVA; when it fires the registers are logged and the lea is performed by hand before
// execution continues, so the guest sees exactly the result it would have computed.
// PALMETTO_POKE_LOG=<rva>[,<rva>...]: up to four one-shot probes. Each address gets an int3 over its first byte; when it
// fires the registers are logged, the original byte is restored and the instruction re-executed, so the guest's behaviour
// is unchanged. This is how a register that is correct at one point and wrong at the next is localised to the call
// between them, without a debugger.
inline std::uintptr_t* poke_log_addresses() {
    static std::uintptr_t addresses[4] = {};
    return addresses;
}
inline unsigned char* poke_log_originals() {
    static unsigned char originals[4] = {};
    return originals;
}
inline void set_poke_log_addresses(const std::uintptr_t* addresses, const unsigned char* originals, unsigned count) {
    for (unsigned i = 0; i < 4; ++i) {
        poke_log_addresses()[i] = i < count ? addresses[i] : 0;
        poke_log_originals()[i] = i < count ? originals[i] : 0;
    }
}

inline std::uintptr_t& poke_lea_address() {
    static std::uintptr_t address = 0;
    return address;
}
inline void set_poke_lea_address(void* address) { poke_lea_address() = reinterpret_cast<std::uintptr_t>(address); }

// PALMETTO_STOP_INT3=<hex rva>[,<hex rva>...]: names up to four int3 sites that are NOT stepped over. A trap that follows
// a null check is the title's own statement that something was passed wrong; stepping over it turns that statement into a
// dereference somewhere else, with a much less useful fault. Stopping here instead lets the exception and the crash
// report show the arguments and the chain of the code that handed the bad value in.
inline std::uintptr_t* stop_int3_addresses() {
    static std::uintptr_t addresses[4] = {};
    return addresses;
}

inline bool is_named_stop_int3(std::uintptr_t address) {
    if (!crash_image_base()) return false;
    const char* list = std::getenv("PALMETTO_STOP_INT3");
    if (!list) return false;
    for (int i = 0; i < 4; ++i) {
        auto* const slots = stop_int3_addresses();
        if (slots[i] || !*list) break;
        char* end = nullptr;
        const auto rva = std::strtoull(list, &end, 16);
        if (end == list) break;
        slots[i] = reinterpret_cast<std::uintptr_t>(crash_image_base()) + rva;
        list = (*end == ',') ? end + 1 : "";
    }
    for (int i = 0; i < 4; ++i)
        if (stop_int3_addresses()[i] == address) return true;
    return false;
}

inline bool skip_int3(EXCEPTION_POINTERS* info) {
    static const bool skip = GetEnvironmentVariableW(L"PALMETTO_SKIP_INT3", nullptr, 0) != 0;
    if (!skip || info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT || !info->ContextRecord) return false;
    const auto* at = static_cast<const unsigned char*>(info->ExceptionRecord->ExceptionAddress);
    if (is_named_stop_int3(reinterpret_cast<std::uintptr_t>(at))) {
        std::fprintf(stderr, "[rt] int3 at %s is named by PALMETTO_STOP_INT3: not stepping over it\n",
                     where(reinterpret_cast<std::uint64_t>(at)).c_str());
        return false;   // let the exception be reported instead of skipped
    }
    for (unsigned i = 0; i < 4; ++i) {
        auto* const addresses = poke_log_addresses();
        if (!addresses[i] || reinterpret_cast<std::uintptr_t>(at) != addresses[i]) continue;
        CONTEXT& c = *info->ContextRecord;
        std::fprintf(stderr,
                     "[rt] poke log %u at %p: rax=%p rbx=%p rcx=%p rdx=%p r8=%p r9=%p rdi=%p rsi=%p r12=%p\n",
                     i, reinterpret_cast<void*>(c.Rip), reinterpret_cast<void*>(c.Rax),
                     reinterpret_cast<void*>(c.Rbx), reinterpret_cast<void*>(c.Rcx),
                     reinterpret_cast<void*>(c.Rdx), reinterpret_cast<void*>(c.R8), reinterpret_cast<void*>(c.R9),
                     reinterpret_cast<void*>(c.Rdi), reinterpret_cast<void*>(c.Rsi),
                     reinterpret_cast<void*>(c.R12));
        // The call chain, as raw words, in one atomic write so another thread's logging cannot split it.
        {
            char line[320];
            int used = std::snprintf(line, sizeof(line), "[rt] poke stack:");
            const auto stack = static_cast<std::uintptr_t>(c.Rsp);
            MEMORY_BASIC_INFORMATION mbi{};
            if (used > 0 && VirtualQuery(reinterpret_cast<const void*>(stack), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const auto* words = reinterpret_cast<const std::uint64_t*>(stack);
                for (int w = 0; w < 8 && used > 0 && used < static_cast<int>(sizeof(line)) - 32; ++w)
                    used += std::snprintf(line + used, sizeof(line) - used, " %llx",
                                          static_cast<unsigned long long>(words[w]));
            }
            if (used > 0 && used < static_cast<int>(sizeof(line)) - 2) {
                line[used++] = '\n';
                line[used] = '\0';
                std::fwrite(line, 1, static_cast<size_t>(used), stderr);
                std::fflush(stderr);
            }
        }
        // Qwords at the argument and object registers, when readable: a caller that loaded a null out of an object
        // usually holds the object in one of these, and the neighbours say which structure it is. rsi gets a longer
        // window because the frontier objects carry their containers in the second half of a 0x60-byte layout.
        {
            static const char* const names[] = {"rax", "rsi", "rbx", "rdx", "r8", "rdi"};
            const std::uintptr_t values[] = {static_cast<std::uintptr_t>(c.Rax), static_cast<std::uintptr_t>(c.Rsi),
                                             static_cast<std::uintptr_t>(c.Rbx), static_cast<std::uintptr_t>(c.Rdx),
                                             static_cast<std::uintptr_t>(c.R8), static_cast<std::uintptr_t>(c.Rdi)};
            static const int counts[] = {4, 12, 4, 4, 4, 4};
            for (int which = 0; which < 6; ++which) {
                const auto address = values[which];
                MEMORY_BASIC_INFORMATION mbi{};
                char line[256];
                int used = std::snprintf(line, sizeof(line), "[rt] poke mem %s %p:", names[which],
                                         reinterpret_cast<void*>(address));
                if (address && used > 0 &&
                    VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                    mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                    const auto* words = reinterpret_cast<const std::uint64_t*>(address);
                    for (int w = 0; w < counts[which] && used > 0 && used < static_cast<int>(sizeof(line)) - 24; ++w)
                        used += std::snprintf(line + used, sizeof(line) - used, " %llx",
                                              static_cast<unsigned long long>(words[w]));
                }
                if (used > 0 && used < static_cast<int>(sizeof(line)) - 2) {
                    line[used++] = '\n';
                    line[used] = '\0';
                    std::fwrite(line, 1, static_cast<size_t>(used), stderr);
                    std::fflush(stderr);
                }
            }
        }
        auto* const originals = poke_log_originals();
        DWORD old_protect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(addresses[i]), 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
            *reinterpret_cast<unsigned char*>(addresses[i]) = originals[i];
            VirtualProtect(reinterpret_cast<void*>(addresses[i]), 1, old_protect, &old_protect);
        }
        repoke_address() = addresses[i];   // re-armed once this instruction has executed
        c.EFlags |= 0x100;                 // trap after the restored instruction
        return true;
    }
    if (poke_lea_address() && reinterpret_cast<std::uintptr_t>(at) == poke_lea_address()) {
        CONTEXT& c = *info->ContextRecord;
        std::fprintf(stderr, "[rt] poke: lea site hit: rax=%p rbx=%p (rbx := rax + 0x12a8)\n",
                     reinterpret_cast<void*>(c.Rax), reinterpret_cast<void*>(c.Rbx));
        c.Rbx = c.Rax + 0x12A8;
        c.Rip += 7;   // the patched instruction was seven bytes long
        return true;
    }
    if (!at || *at != 0xCC || info->ContextRecord->Rip != reinterpret_cast<DWORD64>(at)) return false;
    static std::mutex m;
    static std::set<const void*> seen;
    bool first = false;
    {
        const std::lock_guard lock(m);
        first = seen.insert(at).second;
    }
    if (first) event("int3_skipped", "\"at\":" + quote(where(reinterpret_cast<uint64_t>(at))));
    info->ContextRecord->Rip += 1;
    return true;
}

inline LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    if (handle_watch_write(info)) return EXCEPTION_CONTINUE_EXECUTION;
    if (handle_host_poke(info)) return EXCEPTION_CONTINUE_EXECUTION;
    if (handle_repoke(info)) return EXCEPTION_CONTINUE_EXECUTION;
    if (emulate_amd_instruction(info)) return EXCEPTION_CONTINUE_EXECUTION;
    if (!enabled() || in_handler || !info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (skip_int3(info)) return EXCEPTION_CONTINUE_EXECUTION;
    in_handler = true;
#ifdef _MSC_VER
    // Under /EHsc, catch(...) does not catch SEH faults (e.g. a bad DBG_PRINTEXCEPTION_C pointer or corrupt unwind).
    // Keep the __try frame free of C++ destructors so in_handler is always reset and the exception still propagates.
    __try {
        log_exception(*info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    struct Reset {
        ~Reset() { in_handler = false; }
    } reset;
    log_exception(*info);
#endif
    in_handler = false;
    return EXCEPTION_CONTINUE_SEARCH;
}
}

// Public entry point for the poke diagnostic (the machinery lives in detail).
inline void set_poke_lea_address(void* address) { detail::set_poke_lea_address(address); }

// Public entry point for the poke-log diagnostic (the machinery lives in detail).
inline void set_crash_image(void* base, std::size_t size, const void* functions, std::size_t count) {
    detail::crash_image_base() = base;
    detail::crash_image_size() = size;
    detail::crash_image_functions() = functions;
    detail::crash_image_function_count() = count;
    detail::setup_watch_write();
    detail::setup_host_pokes();
}

inline void set_poke_log(const std::uintptr_t* addresses, const unsigned char* originals, unsigned count) {
    detail::set_poke_log_addresses(addresses, originals, count);
}

inline void close();

// Creates the trace file and installs the exception handler. The file must not exist yet ("x" mode): the trace is
// opened before the loader reads anything, so overwriting would let `--trace` destroy an input, such as the image
// itself. Returns false if the file exists or cannot be created; `error`, when given, receives the errno.
inline bool open(const wchar_t* path, int* error = nullptr) {
    {
        const std::lock_guard lock(detail::mutex());
        if (detail::file) return true;
        const errno_t e = _wfopen_s(&detail::file, path, L"wxb");
        if (e != 0 || !detail::file) {
            detail::file = nullptr;
            if (error) *error = e ? e : EINVAL;
            return false;
        }
        detail::start = std::chrono::steady_clock::now();
        detail::seq = 0;
        static const int atexit_registered = std::atexit(&close);
        (void)atexit_registered;
    }
    detail::veh = AddVectoredExceptionHandler(1, &detail::handler);
    // The report walk and the every-thread pass both run inside this handler, on the stack of the thread that faulted -
    // and a stack-overflow fault leaves next to none of it. The guarantee is per thread, so it has to be asked for
    // before the fault: doing it here covers the thread that installs the trace, and dump_all() asks again for the
    // thread that actually raises a report. It is a guarantee and not a reserve (see kStackGuaranteeBytes): no stack
    // grows, it only fixes how much stays usable to the handler after an overflow. The result is ignored on purpose -
    // a thread with a small reserve accepts what it can and keeps running.
    ULONG guaranteed = rt::crash_report::kStackGuaranteeBytes;
    (void)SetThreadStackGuarantee(&guaranteed);
    detail::on.store(true, std::memory_order_release);
    event("trace_open", "\"pid\":" + std::to_string(GetCurrentProcessId()));
    return true;
}

// Removes the handler and closes the file (tests and process exit via atexit).
inline void close() {
    detail::on.store(false, std::memory_order_release);
    if (detail::veh) RemoveVectoredExceptionHandler(detail::veh);
    detail::veh = nullptr;
    const std::lock_guard lock(detail::mutex());
    if (detail::file) std::fclose(detail::file);
    detail::file = nullptr;
}

}  // namespace rt::startup_trace
