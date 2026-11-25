#pragma once

// A kernel-mode copy into title memory that is committed only on first touch fails before it writes anything.
//
// The title's working buffers are views of a reserve-only section: a page becomes usable when user-mode code first
// touches it, and the runtime's vectored handler then commits it (port/shims/xmem.cpp, physical_page_fault). A
// kernel-mode copy - ReadFile, or the DirectStorage file-range read that is a ReadFile underneath - raises no such
// fault, so the copy fails with ERROR_NOACCESS (998) and the title's readers treat that as transient
// (docs/readfile_first_touch.md measures one such read retried 68,608 times). The repair is the same wherever the copy
// is made: touch one byte of every destination page that is not committed yet from user mode, which runs the handler
// that commits it, then re-issue the copy once. This header is that primitive, shared by the two shims that copy into
// title memory (port/shims/file_system.cpp, port/shims/dstorage.cpp) so their page walks and bounds cannot drift
// apart.
//
// Bounds, in the code below and in the callers:
//   * the walk stops after kPageLimit pages, so a wrong or enormous destination cannot spin here;
//   * one touch per page, and a page that cannot be committed is swallowed so the walk continues;
//   * the caller re-issues the copy exactly once - never a loop - and reports the host's own error if the retry fails.
//
// No game data, no GPU: the primitive reads bytes out of the process's own pages. No C++ objects live in
// commit_range(), because a function with __try cannot have any that need unwinding.
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace rt::first_touch {

inline constexpr DWORD kPageBytes = 0x1000;
inline constexpr DWORD kPageLimit = 0x40000;  // 262,144 pages = 1 GiB of destination walked per attempt

// The two errors a kernel-mode copy into first-touch memory reports: ERROR_NOACCESS (998) for a destination whose
// pages are not committed, and ERROR_INVALID_USER_BUFFER (1784) for a destination the kernel rejects as a user buffer.
// The DirectStorage read path maps both to E_DSTORAGE_ACCESS_VIOLATION, so it must repair both; the file shim's own
// ReadFile measures 998 and stays gated on that one error (its measured A/B is ERROR_NOACCESS only).
inline bool is_kernel_write_blocked(DWORD error) {
    return error == ERROR_NOACCESS || error == ERROR_INVALID_USER_BUFFER;
}

// Reads one byte of every not-yet-committed page of [buffer, buffer + size) from user mode. Returns the number of
// pages the reads committed (a page the handler refuses is not counted); the caller's retried copy reports any
// destination that still cannot be written.
inline DWORD commit_range(void* buffer, std::size_t size) {
    DWORD pages = 0;
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(buffer);
    if (!buffer || size == 0 || size > UINTPTR_MAX - start) return 0;
    const std::size_t total = (size + kPageBytes - 1) / kPageBytes;
    const DWORD limit = total < kPageLimit ? static_cast<DWORD>(total) : kPageLimit;
    for (DWORD i = 0; i < limit; ++i) {
        const std::uintptr_t at = start + static_cast<std::uintptr_t>(i) * kPageBytes;
        MEMORY_BASIC_INFORMATION info{};
        if (!::VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof info)) break;
        if (info.State == MEM_COMMIT) continue;  // already mapped; the retry reports any protection it cannot write
        __try {
            (void)*reinterpret_cast<volatile const unsigned char*>(at);
            ++pages;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // No user-mode commit for this page: the retry reports it. The others may still commit.
        }
    }
    return pages;
}

}  // namespace rt::first_touch
