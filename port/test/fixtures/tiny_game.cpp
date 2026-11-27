// A stand-in game executable for `palmetto --run`: what a real title does in its first second, run through the
// loader instead of the Windows one. Test code, not game content. Run as
//   palmetto --run tiny_game.exe -- hello 42 [tls] [eh] [stack] [game-file] [exit-code] [abort-mode] [file-diagnostic]
// It checks its arguments, a static initializer, the heap, a file round trip, a second thread, and the
// xgameruntime.dll imports (answered by the shim registry: the stub DLL it links against would fail them), then
// calls exit() with 0 (42 with `exit-code`) when everything held. Two checks are opt-in, by argument (CTest's
// run_tiny_game_tls and run_tiny_game_eh): `tls`, thread_local on the game's own threads, which needs the loader's
// per-thread TLS (palmetto starts main on a separate thread); and `eh`, a C++ exception caught inside the image.
// A third, `game-file` (run_tiny_game_dir), reads GAME:\tiny_game_probe.txt after moving the current directory.
// A fourth, `stack` (run_tiny_game_stack), checks that the entry thread and a thread created with dwStackSize 0
// both have 8 MB of stack: a driver compiles shaders on the game's threads (BUILD-1457). Only under palmetto: run
// natively, the second thread gets this program's own 1 MB.
// A `dstorage-arm` mode (docs/peer_port_streaming.md section 14) asks the dstorage shim for its factory and ends the run
// on that answer, which runs the shim's opt-in mapped physical-page destination arm and gates on its checks; it is only
// meaningful under palmetto with the arm's three PALMETTO_DS_PAGES* switches set.
// An `abort-*` mode installs a SIGABRT disposition and calls abort(); tiny_game_abort_test compares the exact exit
// codes with a native run of the same image. No game files.
// On MSVC, `eh` needs more than the unwind tables #176 registers: _CxxThrowException finds the throwing image
// with RtlPcToFileHeader, which the loader redirects in the host MSVC runtime. Arguments, not environment variables,
// so tooling/scripts/run_wine_tests.py (which passes a test's command line but not its ENVIRONMENT) runs them too. stdout is
// unbuffered, so a crash still shows how far the run got.
#include <windows.h>
#include <unknwn.h>  // IUnknown/__uuidof for the dstorage-arm mode's DStorageGetFactory call

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" __declspec(dllimport) HRESULT __stdcall XGameRuntimeInitialize();
extern "C" __declspec(dllimport) void __stdcall XGameRuntimeUninitialize();
extern "C" __declspec(dllimport) uint32_t __stdcall XSystemGetDeviceType();

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("tiny_game: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct StaticInit {
    int value;
    StaticInit() : value(1234) {}
};
StaticInit g_static;

bool has_arg(int argc, char** argv, const char* name) {
    for (int i = 3; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

thread_local int t_counter = 5;

void abort_exit_handler(int) {
    std::_Exit(43);  // a handler that ends the run itself, as a real title's crash reporter may
}
void abort_crt_exit_handler(int) {
    std::exit(43);  // the same through the shimmed exit() import
}
void abort_return_handler(int) {}

// The reserved stack of the calling thread, in bytes, written to *out (a size_t).
DWORD WINAPI stack_reserve(void* out) {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    *static_cast<size_t*>(out) = static_cast<size_t>(high - low);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("tiny_game: started, argc=%d\n", argc);
    check(argc >= 3 && std::strcmp(argv[1], "hello") == 0 && std::strcmp(argv[2], "42") == 0,
          "argv is the game's command line (\"hello\" \"42\"), not palmetto's");
    check(std::strstr(GetCommandLineA(), "hello 42") != nullptr, "GetCommandLineA is the game's command line");
    check(g_static.value == 1234, "a static initializer ran before main");

    if (has_arg(argc, argv, "dstorage-arm")) {
        // docs/peer_port_streaming.md section 14: the runtime's mapped-destination exercise arm runs inside the dstorage
        // shim's DStorageGetFactory when PALMETTO_DS_PAGES_ARM=1 is set (with PALMETTO_DS_PAGES=1 and
        // PALMETTO_DS_PAGES_VIEW=1). This mode asks for the factory through the shim's own console name and ends the run
        // on its answer, so the arm's PASS/FAIL lines gate the fixture's exit code:
        //   PALMETTO_DS_PAGES=1 PALMETTO_DS_PAGES_VIEW=1 PALMETTO_DS_PAGES_ARM=1 \
        //     palmetto --run tiny_game.exe -- hello 42 dstorage-arm
        // The arm needs no GPU, no shader corpus and no game data; it runs its own synthetic physical pages.
        using GetFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        HMODULE dstorage = GetModuleHandleA("dstorage_xs.dll");  // the shim registry answers an owned module as loaded
        auto get_factory = dstorage != nullptr
                               ? reinterpret_cast<GetFactoryFn>(GetProcAddress(dstorage, "DStorageGetFactory"))
                               : nullptr;
        void* factory_object = nullptr;
        const HRESULT hr = get_factory != nullptr ? get_factory(__uuidof(IUnknown), &factory_object) : E_NOTIMPL;
        check(hr == S_OK, "DStorageGetFactory answers S_OK after the mapped-destination arm");
        if (factory_object != nullptr) static_cast<IUnknown*>(factory_object)->Release();
        std::printf("tiny_game: dstorage arm %s\n", SUCCEEDED(hr) ? "PASS" : "FAIL");
        std::fflush(stdout);
        std::exit(SUCCEEDED(hr) ? 0 : 1);  // the arm's own PASS/FAIL lines are the evidence this mode runs for
    }

    std::vector<int> v(1 << 20);
    for (size_t i = 0; i < v.size(); ++i) v[i] = int(i);
    check(v[123456] == 123456, "a 4 MiB heap allocation works");

    char tmp[MAX_PATH] = {};
    const DWORD temp_length = GetTempPathA(MAX_PATH, tmp);
    const bool temp_ok = temp_length > 0 && temp_length < MAX_PATH && tmp[temp_length] == '\0';
    if (has_arg(argc, argv, "file-diagnostic"))
        std::printf("tiny_game: GetTempPathA=%lu chars \"%s\"\n", static_cast<unsigned long>(temp_length), tmp);

    // "palmetto" is eight bytes. Reading only seven made the comparison fail on a native run as well as under the
    // loader; use the payload size for both CRT operations and check that the temporary file is actually removed.
    constexpr char contents[] = "palmetto";
    constexpr size_t contents_size = sizeof(contents) - 1;
    const std::string path = temp_ok ? std::string(tmp) + "palmetto_tiny_game_" +
                                           std::to_string(GetCurrentProcessId()) + ".txt"
                                     : std::string{};
    size_t written = 0;
    size_t bytes_read = 0;
    int remove_result = -1;
    bool file_ok = false;
    if (temp_ok) {
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            written = std::fwrite(contents, 1, contents_size, f);
            const int write_close = std::fclose(f);
            char back[sizeof(contents)] = {};
            if (FILE* r = std::fopen(path.c_str(), "rb")) {
                bytes_read = std::fread(back, 1, contents_size, r);
                const bool read_error = std::ferror(r) != 0;
                const int read_close = std::fclose(r);
                file_ok = written == contents_size && write_close == 0 && bytes_read == contents_size && !read_error &&
                          read_close == 0 && std::memcmp(back, contents, contents_size) == 0;
            }
            remove_result = std::remove(path.c_str());
        }
    }
    if (has_arg(argc, argv, "file-diagnostic") || !file_ok || remove_result != 0)
        std::printf("tiny_game: CRT file path=\"%s\" write=%zu/%zu read=%zu/%zu remove_rc=%d\n", path.c_str(),
                    written, contents_size, bytes_read, contents_size, remove_result);
    check(file_ok && remove_result == 0, "a file round trip through the CRT works");

    long long sum = 0;
    std::thread worker([&] { for (int i = 1; i <= 1000; ++i) sum += i; });
    worker.join();
    check(sum == 500500, "a second thread ran and joined");

    check(XGameRuntimeInitialize() == S_OK, "XGameRuntimeInitialize is answered by the palmetto shim (S_OK)");
    check(XSystemGetDeviceType() == 7, "XSystemGetDeviceType is answered by the palmetto shim ");
    XGameRuntimeUninitialize();

    if (has_arg(argc, argv, "tls")) {
        int seen = -1;
        std::thread t([&] { seen = t_counter; t_counter = 9; });
        t.join();
        check(t_counter == 5 && seen == 5, "thread_local starts at its initializer on each thread");
    } else {
        std::printf("tiny_game: SKIP thread_local (argument tls)\n");
    }
    if (has_arg(argc, argv, "eh")) {
        bool caught = false;
        try {
            throw std::string("boom");
        } catch (const std::string& e) {
            caught = e == "boom";
        }
        check(caught, "a C++ exception is thrown and caught inside the image");
    } else {
        std::printf("tiny_game: SKIP C++ exception (argument eh)\n");
    }
    if (has_arg(argc, argv, "stack")) {
        // Windows gives a thread created with no size the reserve in the process's PE header, which is palmetto's,
        // not this image's. The entry thread gets its size from run_entry.
        const size_t want = size_t{8} << 20;
        size_t entry = 0, created = 0;
        stack_reserve(&entry);
        if (HANDLE thread = CreateThread(nullptr, 0, stack_reserve, &created, 0, nullptr)) {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        std::printf("tiny_game: stack reserve: entry thread %zu bytes, CreateThread with dwStackSize 0 %zu bytes\n",
                    entry, created);
        check(entry >= want, "the entry thread has at least 8 MB of stack");
        check(created >= want, "a thread created with dwStackSize 0 has at least 8 MB of stack");
    } else {
        std::printf("tiny_game: SKIP stack reserve (argument stack)\n");
    }

    if (has_arg(argc, argv, "game-file")) {
        // The probe file sits in the game folder (check_game_dir.cmake writes it). Move the current directory away
        // first, as a title may: GAME: must still name the game folder, whether palmetto was given a bare image name
        // or a relative --game-dir.
        SetCurrentDirectoryA(tmp);
        HANDLE file = CreateFileW(L"GAME:\\tiny_game_probe.txt", GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        char data[4] = {};
        DWORD count = 0;
        const bool read = file != INVALID_HANDLE_VALUE && ReadFile(file, data, sizeof(data), &count, nullptr) &&
                          count == sizeof(data) && std::memcmp(data, "game", sizeof(data)) == 0;
        if (file == INVALID_HANDLE_VALUE) std::printf("tiny_game: GAME: open error %lu\n", GetLastError());
        else CloseHandle(file);
        check(read, "GAME:\\tiny_game_probe.txt is read from the game folder after SetCurrentDirectory");
    } else {
        std::printf("tiny_game: SKIP GAME: file (argument game-file)\n");
    }

    if (has_arg(argc, argv, "abort-handler")) {
        std::signal(SIGABRT, abort_exit_handler);
        std::abort();
    }
    if (has_arg(argc, argv, "abort-crt-exit")) {
        std::signal(SIGABRT, abort_crt_exit_handler);
        std::abort();
    }
    if (has_arg(argc, argv, "abort-return")) {
        std::signal(SIGABRT, abort_return_handler);
        std::abort();
    }
    if (has_arg(argc, argv, "abort-ignore")) {
        std::signal(SIGABRT, SIG_IGN);
        std::abort();
    }
    if (has_arg(argc, argv, "abort-default")) {
        std::signal(SIGABRT, SIG_DFL);
        std::abort();
    }

    std::printf("tiny_game: %s\n", g_failures ? "FAILED" : "OK");
    std::fflush(stdout);
    // Reserve 42 for a successful guest test. The loader itself returns 1 on errors.
    std::exit(g_failures ? 1 : (has_arg(argc, argv, "exit-code") ? 42 : 0));
}
