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
//
// The second half of this header is the terminal-outcome rule (docs/peer_port_streaming.md section 12): the title's
// own load reader retries a read until it returns one of three errors, so a read that fails with any other error is
// silent retry material. These helpers name the three errors and the one the rule converts to, so the file and the
// storage read paths cannot disagree about them; the switch itself is read by each shim.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

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

// ---- Terminal read outcomes (PALMETTO_READ_TERMINAL) --------------------------------------------------------------
//
// docs/wait_forensics.md section 5 measured the retail title's own reader (game+0x314200) against the disassembly: it
// calls ReadFile in a loop and ends the loop only when the call fails with ERROR_INVALID_HANDLE (6), ERROR_HANDLE_EOF
// (0x26) or ERROR_INVALID_PARAMETER (0x57); every other error is treated as transient, the loop sleeps 1 ms and the
// same read is issued again. A read that cannot be made to succeed but reports anything else is therefore a silent
// retry: the measured pre-repair boot issued one such read 68,608 times and the loader stayed parked
// (docs/readfile_first_touch.md). The rule below closes that shape: a synchronous read that the one repair retry does
// not make succeed is converted once into one of those three errors, so the title handles a failed load instead of
// sleeping in its retry loop.
//
// PALMETTO_READ_TERMINAL is the opt-in switch; unset (or 0/off/false/no, case-insensitive, surrounding blanks ignored)
// is the previous path exactly. Both shims read it on their error paths only.

// The three errors the measured reader ends its loop on. Any other error is retried by the title.
inline bool is_terminal_read_error(DWORD error) {
    return error == ERROR_INVALID_HANDLE || error == ERROR_HANDLE_EOF || error == ERROR_INVALID_PARAMETER;
}

// The error a survivor of the one repair retry is converted to. All three end the measured loop; ERROR_INVALID_PARAMETER
// (0x57) is chosen because the argument that could not be satisfied is the destination range the kernel cannot write
// (0x26 would read in the title's own reader as end-of-file, i.e. a truncated load, and 6 would name the file handle).
// Which of the three the title's loader handles best is not measured at the current content population - no measured
// read survives the repair - and is the slice's named next falsifier, not a claim of this rule.
inline constexpr DWORD kTerminalReadError = ERROR_INVALID_PARAMETER;

// Whether a failed read is one the rule converts. Refused (false, with the reason named at the call site):
//   * an asynchronous read that reports ERROR_IO_PENDING - the host's overlapped contract owns that outcome, and the
//     title is not retrying a completed read;
//   * an error already in the terminal set - the reader ends on it as it is;
//   * a null or empty destination - the caller's own argument, and the host's answer for it is already terminal.
inline bool converts_to_terminal(DWORD error, const void* buffer, DWORD size) {
    return error != ERROR_IO_PENDING && buffer != nullptr && size != 0 && !is_terminal_read_error(error);
}

// PALMETTO_READ_TERMINAL's value: set and not 0/off/false/no (case-insensitive, surrounding blanks ignored) enables the
// rule; unset or any of those spellings leaves the default path, which is every current arm's path.
inline bool terminal_reads_enabled(const char* value) {
    if (value == nullptr) return false;
    while (*value == ' ' || *value == '\t') ++value;
    const char* end = value;
    while (*end != '\0') ++end;
    while (end != value && (end[-1] == ' ' || end[-1] == '\t')) --end;
    const std::size_t size = static_cast<std::size_t>(end - value);
    if (size == 0) return false;
    if (size == 1 && value[0] == '0') return false;
    const auto is = [&](const char* word) {
        const std::size_t length = std::strlen(word);
        if (length != size) return false;
        for (std::size_t i = 0; i < size; ++i) {
            char c = value[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != word[i]) return false;
        }
        return true;
    };
    return !(is("off") || is("false") || is("no"));
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
