// PALMETTO_USER_TRACE: is the title's stuck loading state a platform (user/session/storage/package) service wait?
//
// Off by default, and byte-identical when unset: no line, no counter, no state change. With PALMETTO_USER_TRACE=1 the
// platform shims log
//   - every QueryApiImpl request: the class it asks for, the interface id, and the HRESULT answered (so a class the
//     shim refuses - E_NOINTERFACE, or E_NOTIMPL from a slot - is visible, and so is the poll rate);
//   - every served XUserImpl/XUserGamertagImpl slot, with the value returned.
// Each entry point logs its first call and then every tenth-power call (1, 10, 100, ...): a title polling every frame
// cannot flood the log, and the rate is readable between the last two lines. Every line carries the seconds since the
// shim loaded, the caller as module+RVA (so a line names the title's thunk or call site), and the answered value.
//
// This is a diagnostic only. It never changes what a shim returns.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "shims.h"

namespace rt::shims {

inline bool platform_trace_enabled() {
    static const bool on = [] {
        char value[16] = {};
        const DWORD n = GetEnvironmentVariableA("PALMETTO_USER_TRACE", value, sizeof value);
        if (n == 0 || n >= sizeof value) return false;
        return std::strcmp(value, "0") != 0 && _stricmp(value, "off") != 0;
    }();
    return on;
}

// Seconds since the process was created (GetProcessTimes' creation time against the file-time clock), so a line can
// be tied to the loading loop's timeline: the runtime's own `[rt] load trace +N.Ns` lines use the same start.
inline double platform_trace_seconds() {
    static const double start = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
        ULARGE_INTEGER value{};
        value.LowPart = created.dwLowDateTime;
        value.HighPart = created.dwHighDateTime;
        return static_cast<double>(value.QuadPart) / 1e7;   // 100 ns ticks since 1601
    }();
    FILETIME now_filetime{};
    GetSystemTimeAsFileTime(&now_filetime);
    ULARGE_INTEGER now{};
    now.LowPart = now_filetime.dwLowDateTime;
    now.HighPart = now_filetime.dwHighDateTime;
    return static_cast<double>(now.QuadPart) / 1e7 - start;
}

// The ordinal of this call when it is one to log (1-8, then the tenth, hundredth, ...), 0 when it should stay quiet.
// The first calls of an entry point are logged one by one so a one-shot call can be told from a poll of a few calls;
// after that it is O(log calls) per entry point. A key is any stable address (a string literal, a slot wrapper's
// address, or a mixed class/interface hash).
inline std::uint64_t platform_trace_tick(std::uint64_t key) {
    static std::mutex mu;
    static std::unordered_map<std::uint64_t, std::uint64_t> counts;
    std::lock_guard<std::mutex> lock(mu);
    const std::uint64_t n = ++counts[key];
    if (n <= 8) return n;
    std::uint64_t power = 10;
    while (power < n) power *= 10;
    return power == n ? n : 0;
}

// One printed line. `what` names the entry point (or class + interface), `n` is the call's ordinal from
// platform_trace_tick, `caller` is the return address captured at the shim's entry (`_ReturnAddress()`), `value` is
// what the shim answered (an HRESULT, a bool, an enum, ...).
inline void platform_trace_emit(const char* what, std::uint64_t n, void* caller, std::uint64_t value) {
    const std::string where = module_offset(reinterpret_cast<std::uint64_t>(caller));
    std::fprintf(stderr, "[rt] user trace: +%.2fs %s #%llu from %s -> 0x%llx\n", platform_trace_seconds(), what,
                 static_cast<unsigned long long>(n), where.c_str(), static_cast<unsigned long long>(value));
}

// The common shape: one stable key (a literal's address), one name, one value.
inline void platform_trace(const char* what, void* caller, std::uint64_t value) {
    if (!platform_trace_enabled()) return;
    const std::uint64_t n = platform_trace_tick(reinterpret_cast<std::uint64_t>(what));
    if (n) platform_trace_emit(what, n, caller, value);
}

// Same, with an explicit key (a slot wrapper's address) and a name that may be a formatted buffer.
inline void platform_trace_keyed(std::uint64_t key, const char* what, void* caller, std::uint64_t value) {
    if (!platform_trace_enabled()) return;
    const std::uint64_t n = platform_trace_tick(key);
    if (n) platform_trace_emit(what, n, caller, value);
}

}  // namespace rt::shims
