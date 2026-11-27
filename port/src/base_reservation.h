#pragma once

// Keeping a relocation-less game image's preferred range free until the loader maps it there.
//
// An EXE without a .reloc directory can only run at its ImageBase (0x140000000 by default). Windows and x86-64 Wine
// leave that range alone, but arm64 Wine on macOS (CrossOver's arm64 bottles, whose x86-64 code runs under FEX) puts
// its own start-up allocations at 0x100000000 and up, over that range, before palmetto's first line runs. So palmetto
// starts itself again, suspended, reserves [ImageBase, ImageBase + SizeOfImage) in that child before any of its code
// runs, and resumes it; the child's loader then commits the image inside the reservation (commit_reserved), so
// nothing else can take the range in between. The child is told by PALMETTO_BASE_RESERVED=<base>:<size> (hex), which
// also stops it from starting itself again. Measured on CrossOver Preview 20260821 (arm64 bottle, M2 Max): in a
// suspended child 0x140000000-0x170000000 is free and reservable; macOS maps its own regions above that (the arm64
// shared region starts at 0x180000000), so a range there can't be had either way.
//
// Nothing changes where the range is free at start-up (Windows, x86-64 Wine) or where the image has relocations.
// Measured on package 1.0.2.1: the title image is relocatable after all - a 0x12a350-byte .reloc directory (2,763
// blocks, 599,676 entries, 598,282 of them DIR64) - so needed() returns nullopt for it, the loader maps it at
// 0x140000000 while that range is free and moves it with relocations where it is not, and this reservation applies
// to fixed-base images such as the tiny_game_fixed fixture (/FIXED, ImageBase 0x140000000). That fixture's header has
// a base-relocation directory of {RVA 0, Size 0xd}: a directory is present only when its RVA and its size are both
// non-zero, the rule rt::pe::Directory::present() (pe_image.h) and the Windows loader follow. Reading only the size
// treated that fixed image as relocatable and skipped the reservation.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace rt::base_reservation {

inline constexpr wchar_t kVariable[] = L"PALMETTO_BASE_RESERVED";

struct Range {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
};

// The range this process was started with (PALMETTO_BASE_RESERVED), if any.
inline std::optional<Range> inherited() {
    wchar_t value[64] = {};
    const DWORD length = GetEnvironmentVariableW(kVariable, value, 64);
    if (!length || length >= 64) return std::nullopt;
    Range r{};
    // swscanf_s, not swscanf (MSVC's C4996): integer conversions take no buffer sizes, so the parse is the same.
    if (swscanf_s(value, L"%llx:%llx", reinterpret_cast<unsigned long long*>(&r.base),
                  reinterpret_cast<unsigned long long*>(&r.size)) != 2 ||
        !r.size)
        return std::nullopt;
    return r;
}

// Commits [base, base + size) for the loader when it lies inside the reservation this process was started with and
// that reservation is still in place; returns null otherwise (the caller then allocates as usual). The reservation
// is the allocation, so VirtualFree(base, 0, MEM_RELEASE) frees it as it frees any image.
inline void* commit_reserved(std::uint64_t base, std::uint64_t size) {
    const auto r = inherited();
    if (!r || base != r->base || size > r->size) return nullptr;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(base), &info, sizeof info) || info.State != MEM_RESERVE ||
        reinterpret_cast<std::uint64_t>(info.AllocationBase) != base) {
        // The marker is inherited but this range is no longer its reservation: a stale or hand-set environment
        // variable, or a reservation something gave back. Map as usual rather than commit memory that is not ours.
        std::fprintf(stderr,
                     "[rt] base reservation: %ls names %#llx-%#llx, but this process has no reservation there; "
                     "allocating as usual\n",
                     kVariable, static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(base + size));
        return nullptr;
    }
    void* const committed =
        VirtualAlloc(reinterpret_cast<void*>(base), static_cast<SIZE_T>(size), MEM_COMMIT, PAGE_READWRITE);
    if (committed)
        std::fprintf(stderr, "[rt] base reservation: committed %#llx-%#llx from the inherited reservation\n",
                     static_cast<unsigned long long>(base), static_cast<unsigned long long>(base + size));
    return committed;
}

// The preferred range of `image` when it has no relocations and that range is not free in this process; nullopt
// when the image can be mapped as it is (or is not a readable PE32+, which the loader reports itself).
//
// The header checks mirror the ones pe_loader.cpp's map_image() makes through the same fields: a file the loader will
// reject must not make palmetto start itself again first, and a base or a size that VirtualAllocEx cannot take must
// fall through to the loader's own named error instead of a relaunch that can only fail. The relocation decision uses
// the directory's RVA and its size, not the size alone (see the measured fixed fixture note above).
inline std::optional<Range> needed(const std::filesystem::path& image) {
    std::ifstream file(image, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    if (!file.read(reinterpret_cast<char*>(&dos), sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        return std::nullopt;
    IMAGE_NT_HEADERS64 nt{};
    if (!file.seekg(dos.e_lfanew) || !file.read(reinterpret_cast<char*>(&nt), sizeof nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        static_cast<std::size_t>(nt.FileHeader.SizeOfOptionalHeader) < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !nt.OptionalHeader.SizeOfImage)
        return std::nullopt;
    const auto& oh = nt.OptionalHeader;
    if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
        const auto& relocs = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        // A directory with RVA 0 is not a relocation directory, whatever its size field holds (as the /FIXED fixture
        // above shows), so only a present directory makes the image relocatable.
        if (relocs.VirtualAddress && relocs.Size)
            return std::nullopt;  // relocatable: the loader moves it if the base is taken
    }
    const Range r{oh.ImageBase, (static_cast<std::uint64_t>(oh.SizeOfImage) + 0xffff) & ~std::uint64_t{0xffff}};
    // VirtualAlloc and VirtualAllocEx need an allocation-granularity-aligned address that no valid range follows, and
    // the reservation loop below needs base + size not to wrap the address space.
    if (!r.base || (r.base & 0xffffull) || r.base > ~std::uint64_t{0} - r.size) return std::nullopt;
    for (std::uint64_t at = r.base; at < r.base + r.size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof info)) return r;
        if (info.State != MEM_FREE) return r;
        at = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    return std::nullopt;
}

// Runs this program again with the same command line, suspended, reserves `r` in the new process, resumes it and
// returns its exit code. On failure prints why and returns 1 (the child, if any, is ended unrun).
inline int relaunch(const Range& r) {
    wchar_t self[MAX_PATH * 4] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH * 4)) {
        std::fprintf(stderr, "error: GetModuleFileName failed (%lu)\n", GetLastError());
        return 1;
    }
    wchar_t marker[64];
    std::swprintf(marker, 64, L"%llx:%llx", static_cast<unsigned long long>(r.base),
                  static_cast<unsigned long long>(r.size));
    // Inherited by the child: it tells it to commit inside the reservation and not to relaunch again.
    if (!SetEnvironmentVariableW(kVariable, marker)) {
        std::fprintf(stderr, "error: setting %ls failed (%lu)\n", kVariable, GetLastError());
        return 1;
    }
    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup{sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    for (HANDLE h : {startup.hStdInput, startup.hStdOutput, startup.hStdError})
        if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    PROCESS_INFORMATION process{};
    std::fflush(stdout);
    std::fflush(stderr);
    if (!CreateProcessW(self, command_line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                        &startup, &process)) {
        std::fprintf(stderr, "error: starting palmetto again to reserve the image range failed (%lu)\n", GetLastError());
        return 1;
    }
    const auto abandon = [&process] {  // the child never ran: end it and release its handles
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    };
    if (!VirtualAllocEx(process.hProcess, reinterpret_cast<void*>(r.base), static_cast<SIZE_T>(r.size), MEM_RESERVE,
                        PAGE_NOACCESS)) {
        const DWORD error = GetLastError();
        abandon();
        std::fprintf(stderr,
                     "error: the image has no relocations and its range %#llx-%#llx is taken, also in a new "
                     "process (VirtualAllocEx %lu)\n",
                     static_cast<unsigned long long>(r.base), static_cast<unsigned long long>(r.base + r.size), error);
        return 1;
    }
    // The one line that says a second process is running: everything after it comes from that child.
    std::fprintf(stderr, "[rt] base reservation: reserved %#llx-%#llx in the relaunched process (pid %lu)\n",
                 static_cast<unsigned long long>(r.base), static_cast<unsigned long long>(r.base + r.size),
                 static_cast<unsigned long>(process.dwProcessId));
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {  // still suspended: the wait would never end
        const DWORD error = GetLastError();
        abandon();
        std::fprintf(stderr, "error: resuming the relaunched palmetto failed (%lu)\n", error);
        return 1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

}  // namespace rt::base_reservation
