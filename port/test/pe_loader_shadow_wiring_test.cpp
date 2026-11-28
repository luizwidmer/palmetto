// Synthetic integration checks for the optional shadow comparison through Loader::load/map_image.
// No game bytes are used and no mapped code is executed.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "pe_loader.h"

namespace {
namespace fs = std::filesystem;
int g_checks = 0;
int g_failures = 0;

void check(bool passed, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", what);
    if (!passed) ++g_failures;
}

struct SyntheticImage {
    static constexpr std::uint32_t kNt = 0x80;
    static constexpr std::uint32_t kOptional = 0x98;
    static constexpr std::uint32_t kSections = 0x188;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x800, 0);

    void u16(std::uint32_t offset, std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    void u32(std::uint32_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
    void u64(std::uint32_t offset, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
    void section(unsigned index, const char* name, std::uint32_t va, std::uint32_t raw_pointer, std::uint32_t characteristics) {
        const std::uint32_t offset = kSections + index * 40;
        std::memcpy(bytes.data() + offset, name, std::min<std::size_t>(std::strlen(name), 8));
        u32(offset + 8, 0x200);
        u32(offset + 12, va);
        u32(offset + 16, 0x200);
        u32(offset + 20, raw_pointer);
        u32(offset + 36, characteristics);
    }
    explicit SyntheticImage(bool invalid_resource_directory) {
        u16(0, 0x5A4D);
        u32(0x3C, kNt);
        u32(kNt, 0x00004550);
        u16(kNt + 4, 0x8664);
        u16(kNt + 6, 2);
        u16(kNt + 20, 0xF0);
        u16(kNt + 22, 0x0022);
        u16(kOptional, 0x020B);
        u32(kOptional + 16, 0x1000);
        u64(kOptional + 24, 0x140000000ull);
        u32(kOptional + 32, 0x1000);
        u32(kOptional + 36, 0x200);
        u32(kOptional + 56, 0x4000);
        u32(kOptional + 60, 0x400);
        u32(kOptional + 108, 16);
        section(0, ".text", 0x1000, 0x400, 0x60000020);
        section(1, ".rdata", 0x2000, 0x600, 0x40000040);
        const std::uint32_t resource = kOptional + 112 + 2 * 8;
        u32(resource, invalid_resource_directory ? 0x3FF0 : 0x2020);
        u32(resource + 4, invalid_resource_directory ? 0x20 : 0x10);
        bytes[0x400] = 0xC3;  // Return stub; never executed.
    }
};

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / (L"palmetto_pe_loader_shadow_wiring_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

fs::path write_image(const fs::path& path, const SyntheticImage& image) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create synthetic image");
    output.write(reinterpret_cast<const char*>(image.bytes.data()), static_cast<std::streamsize>(image.bytes.size()));
    if (!output) throw std::runtime_error("cannot write synthetic image");
    return path;
}

void set_switch(const wchar_t* name, const wchar_t* value) {
    if (!SetEnvironmentVariableW(name, value)) throw std::runtime_error("cannot set shadow test environment");
}

int run_child_case(const std::wstring& which, const fs::path& matching, const fs::path& mismatch) {
    const wchar_t* parse = nullptr;
    const wchar_t* strict = nullptr;
    if (which == L"strict-only") {
        strict = L"1";
    } else if (which == L"truthy-parse") {
        parse = L"true";
        strict = L"1";
    } else if (which == L"parse-only") {
        parse = L"1";
    } else if (which == L"match-strict" || which == L"strict-mismatch") {
        parse = L"1";
        strict = L"1";
    } else if (which != L"off") {
        return 80;
    }
    set_switch(L"PALMETTO_PE_LOADER_SHADOW_PARSE", parse);
    set_switch(L"PALMETTO_PE_LOADER_SHADOW_STRICT", strict);

    const bool expects_mismatch = which == L"strict-mismatch";
    const fs::path image_path = which == L"match-strict" ? matching : mismatch;

    try {
        {
            rt::Loader loader(image_path.parent_path());
            rt::Module& module = loader.load(image_path);
            if (expects_mismatch) return 82;
            if (!module.base || module.size != 0x4000 || module.entry_rva != 0x1000 || module.base[0x1000] != 0xC3) return 83;
        }
        return 0;
    } catch (const std::exception& error) {
        if (!expects_mismatch) {
            std::fprintf(stderr, "child %ls unexpectedly refused: %s\n", which.c_str(), error.what());
            return 85;
        }
        const std::string message = error.what();
        return message.find("PALMETTO_PE_LOADER_SHADOW_STRICT") != std::string::npos &&
                       message.find("invalid_image") != std::string::npos
                   ? 0
                   : 86;
    }
}

std::wstring quote_arg(const std::wstring& value) {
    std::wstring result(1, static_cast<wchar_t>(34));
    result += value;
    result += static_cast<wchar_t>(34);
    return result;
}

bool run_child(const std::wstring& which, const std::wstring& matching, const std::wstring& mismatch) {
    wchar_t executable[MAX_PATH * 4] = {};
    const DWORD capacity = static_cast<DWORD>(std::size(executable));
    const DWORD length = GetModuleFileNameW(nullptr, executable, capacity);
    if (!length || length >= capacity) return false;
    std::wstring command = quote_arg(executable) + L" --child " + which + L" " + quote_arg(matching) + L" " + quote_arg(mismatch);
    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    PROCESS_INFORMATION process{};
    HANDLE log_handle = nullptr;
    fs::path log_path;
    BOOL inherit_handles = FALSE;
    if (which == L"parse-only") {
        log_path = fs::path(mismatch).parent_path() / L"shadow-diagnostic.log";
        log_handle = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_handle == INVALID_HANDLE_VALUE) return false;
        if (!SetHandleInformation(log_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            CloseHandle(log_handle);
            return false;
        }
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = log_handle;
        startup.hStdOutput = log_handle;
        startup.hStdError = log_handle;
        inherit_handles = TRUE;
    }
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, inherit_handles, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        std::printf("CreateProcessW failed for %ls (error %lu)\n", which.c_str(), GetLastError());
        if (log_handle && log_handle != INVALID_HANDLE_VALUE) CloseHandle(log_handle);
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0xFFFFFFFFu;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (log_handle && log_handle != INVALID_HANDLE_VALUE) CloseHandle(log_handle);
    if (exit_code != 0) {
        std::printf("child %ls returned %lu\n", which.c_str(), exit_code);
        return false;
    }
    if (which == L"parse-only") {
        std::ifstream input(log_path, std::ios::binary);
        const std::string output((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (output.find("[rt] loader shadow parse:") == std::string::npos || output.find("invalid_image") == std::string::npos) {
            std::printf("parse-only diagnostic was not named: %s\n", output.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"--child")
        return run_child_case(argv[2], fs::path(argv[3]), fs::path(argv[4]));
    try {
        TempDir temp;
        const fs::path matching = write_image(temp.path / L"shadow_match.exe", SyntheticImage(false));
        const fs::path mismatch = write_image(temp.path / L"shadow_invalid_directory.exe", SyntheticImage(true));
        const std::wstring match_arg = matching.wstring();
        const std::wstring mismatch_arg = mismatch.wstring();
        std::printf("pe_loader_shadow_wiring_test: real map_image switch and compare paths\n");
        check(run_child(L"off", match_arg, mismatch_arg), "unset switches preserve legacy mapping of parser-refused synthetic bytes");
        check(run_child(L"strict-only", match_arg, mismatch_arg), "strict-only is a no-op without shadow parsing");
        check(run_child(L"truthy-parse", match_arg, mismatch_arg), "only exact parse value 1 enables the parser");
        check(run_child(L"parse-only", match_arg, mismatch_arg), "parse-only emits a named mismatch and keeps legacy mapping");
        check(run_child(L"match-strict", match_arg, mismatch_arg), "strict comparison accepts matching facts through map_image");
        check(run_child(L"strict-mismatch", match_arg, mismatch_arg), "strict comparison refuses a parser mismatch by name");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected test exception: %s\n", error.what());
        return 2;
    }
    std::printf("pe_loader_shadow_wiring_test: %s (%d checks, %d failures)\n", g_failures ? "FAILED" : "OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}