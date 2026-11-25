// xapu.dll and chad.dll shim ABI test, against the call shapes measured from the retail executable
// (docs/audio_shims.md; tooling/import_call_sites.py and the call-site disassembly).
//
// All ten exports are resolved through rt::shims::find, so this exercises the exact tables and calling convention the
// runtime installs. No GPU, no boot and no game data: the functions above are pure process-memory shims.
//
// The trace section at the end is what proves the per-call ("audio") events: it opens the startup trace, drives the
// shims, and reads the file back, so a shim that stops logging fails here instead of silently changing a measurement.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#include "shims.h"
#include "startup_trace.h"

namespace {

using XApuConnectFn = HRESULT(__stdcall*)(void*, void*, void**);
using XApuDisconnectFn = HRESULT(__stdcall*)(void*);
using XApuEnqueueFn = HRESULT(__stdcall*)(void*, const uint32_t*, uint64_t);
using XApuGetResultCountFn = uint32_t(__stdcall*)(void*, uint64_t);
using XApuDequeueFn = HRESULT(__stdcall*)(void*, void*);

// Measured: XDspConnectWithMaximumStreamLimit(buffer, length, aggregateSeconds, maxStreams, maxChannels, handle**).
using XDspConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void**);
using XDspActivateFn = HRESULT(__stdcall*)(void*, const void*, void**, void**);
using XDspDeactivateFn = HRESULT(__stdcall*)(void*, uint64_t, uint64_t, uint64_t);
using XDspDisconnectFn = HRESULT(__stdcall*)(void*);
using XDspSubmitFn = HRESULT(__stdcall*)(void*, void*, uint32_t*);

constexpr uint32_t kXApuBufferSize = 0x104000;      // 0x14000 input + 128 * 0x1E00 output (docs/audio_shims.md)
constexpr HRESULT kXApuEPendingResults = static_cast<HRESULT>(0x8004E801u);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

int count_occurrences(const std::string& text, const std::string& needle) {
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// ---- The trace: every shim call is one "audio" event with its caller ----
void test_trace(XApuConnectFn connect, XApuDisconnectFn disconnect, XApuEnqueueFn enqueue,
                XApuGetResultCountFn result_count, XApuDequeueFn dequeue, XDspConnectFn dsp_connect,
                XDspActivateFn activate, XDspDeactivateFn deactivate, XDspDisconnectFn dsp_disconnect,
                XDspSubmitFn submit) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xapu_chad_trace_test_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());

    check(rt::startup_trace::open(path.c_str()), "open the startup trace file");

    uint8_t block[24] = {};
    uint8_t out16[16] = {};
    void* handle = nullptr;
    check(connect(block, out16, &handle) == S_OK && handle != nullptr, "connect with the trace on succeeds");
    const uint32_t commands[] = {1, 2};
    for (uint32_t type : commands) {
        const uint32_t command[2] = {type, 0};
        enqueue(handle, command, 0);
    }
    result_count(handle, 0);
    result_count(handle, 0);
    uint32_t result[20] = {};
    dequeue(handle, result);
    check(disconnect(handle) == S_OK, "disconnect with the trace on succeeds");

    void* dsp = nullptr;
    uint8_t title_buffer[16] = {};
    check(dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, &dsp) == S_OK && dsp != nullptr,
          "chad connect with the trace on succeeds");
    void* status = nullptr;
    void* stream = nullptr;
    activate(dsp, nullptr, &status, &stream);
    uint32_t id = 0;
    submit(stream, title_buffer, &id);
    deactivate(dsp, 0, 0, 0);
    check(dsp_disconnect(dsp) == S_OK, "chad disconnect with the trace on succeeds");

    rt::startup_trace::close();

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    DeleteFileW(path.c_str());

    check(count_occurrences(log, "\"kind\":\"audio\"") == 12, "one trace event per call: 7 xapu + 5 chad");
    check(count_occurrences(log, "\"api\":\"XApuConnect\"") == 1, "XApuConnect is traced once");
    check(count_occurrences(log, "\"api\":\"XApuEnqueueCommand\",\"caller\"") == 2,
          "both XApuEnqueueCommand calls are traced");
    check(count_occurrences(log, "\"api\":\"XApuGetResultCount\"") == 2, "both result-count calls are traced");
    check(count_occurrences(log, "\"api\":\"XApuDequeueResult\"") == 1, "the dequeue is traced once");
    check(count_occurrences(log, "\"api\":\"XDspSubmitCommand\"") == 1, "XDspSubmitCommand is traced once");
    check(count_occurrences(log, "\"dll\":\"xapu.dll\"") == 7, "the xapu events name their DLL");
    check(count_occurrences(log, "\"dll\":\"chad.dll\"") == 5, "the chad events name their DLL");
    check(count_occurrences(log, "\"caller\":\"") == 12, "every event names its caller");
    check(count_occurrences(log, "\"type\":1") == 1 && count_occurrences(log, "\"type\":2") == 1,
          "the enqueue events carry the measured command types");
    check(count_occurrences(log, "\"dropped\":1}") == 1 && count_occurrences(log, "\"dropped\":2}") == 1,
          "the dropped-command counter increments across the traced enqueues (1, then 2)");
}

}  // namespace

int main() {
    std::printf("xapu_chad_test: registered xapu.dll and chad.dll shims against the measured call shapes\n");

    const auto connect = reinterpret_cast<XApuConnectFn>(rt::shims::find("xapu.dll", "XApuConnect"));
    const auto disconnect = reinterpret_cast<XApuDisconnectFn>(rt::shims::find("xapu.dll", "XApuDisconnect"));
    const auto enqueue = reinterpret_cast<XApuEnqueueFn>(rt::shims::find("xapu.dll", "XApuEnqueueCommand"));
    const auto result_count =
        reinterpret_cast<XApuGetResultCountFn>(rt::shims::find("xapu.dll", "XApuGetResultCount"));
    const auto dequeue = reinterpret_cast<XApuDequeueFn>(rt::shims::find("xapu.dll", "XApuDequeueResult"));
    const auto dsp_connect = reinterpret_cast<XDspConnectFn>(
        rt::shims::find("chad.dll", "XDspConnectWithMaximumStreamLimit"));
    const auto activate = reinterpret_cast<XDspActivateFn>(rt::shims::find("chad.dll", "XDspActivate"));
    const auto deactivate = reinterpret_cast<XDspDeactivateFn>(rt::shims::find("chad.dll", "XDspDeactivate"));
    const auto dsp_disconnect = reinterpret_cast<XDspDisconnectFn>(rt::shims::find("chad.dll", "XDspDisconnect"));
    const auto submit = reinterpret_cast<XDspSubmitFn>(rt::shims::find("chad.dll", "XDspSubmitCommand"));

    check(connect != nullptr && disconnect != nullptr && enqueue != nullptr && result_count != nullptr &&
              dequeue != nullptr,
          "all five xapu.dll exports are registered");
    check(dsp_connect != nullptr && activate != nullptr && deactivate != nullptr && dsp_disconnect != nullptr &&
              submit != nullptr,
          "all five chad.dll exports are registered");
    check(rt::shims::owns_dll("xapu.dll"), "the registry owns xapu.dll");
    check(rt::shims::owns_dll("chad.dll"), "the registry owns chad.dll");
    if (!connect || !disconnect || !enqueue || !result_count || !dequeue || !dsp_connect || !activate ||
        !deactivate || !dsp_disconnect || !submit) {
        std::printf("Fatal: missing export bindings\n");
        return 1;
    }

    // ---- XApuConnect: block24 is the title's, out16 is {base, size} ----
    uint8_t block[24];
    std::memset(block, 0xA5, sizeof block);
    uint8_t out16[16];
    std::memset(out16, 0x5A, sizeof out16);
    void* handle = nullptr;
    check(connect(block, out16, &handle) == S_OK && handle != nullptr,
          "XApuConnect returns S_OK with a non-null opaque handle");
    bool block_untouched = true;
    for (uint8_t byte : block) block_untouched = block_untouched && byte == 0xA5;
    check(block_untouched, "XApuConnect leaves the title's 24-byte input block untouched");
    uint64_t base = 0;
    uint64_t size = 0;
    std::memcpy(&base, out16, sizeof base);
    std::memcpy(&size, out16 + 8, sizeof size);
    check(base != 0, "the 16-byte output's first qword is the buffer base");
    check(size == kXApuBufferSize, "the 16-byte output's second qword is the measured 0x104000-byte buffer size");
    if (base && size == kXApuBufferSize) {
        auto* buffer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(base));
        check(buffer[0] == 0 && buffer[size - 1] == 0, "the shared buffer is zeroed when it is handed over");
        buffer[0] = 0x11;
        buffer[size - 1] = 0x22;
        check(buffer[0] == 0x11 && buffer[size - 1] == 0x22, "the shared buffer is committed and writable end to end");
    }

    check(connect(block, out16, nullptr) == E_POINTER, "XApuConnect(nullptr handle) returns E_POINTER");
    void* rejected = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    check(connect(block, nullptr, &rejected) == E_POINTER && rejected == nullptr,
          "XApuConnect(nullptr output block) returns E_POINTER and clears the handle");

    // ---- XApuEnqueueCommand: accepted and dropped; the result queue stays empty ----
    check(result_count(handle, 0) == 0, "XApuGetResultCount is 0 before any command");
    for (uint32_t type : {1u, 2u, 3u, 5u}) {
        const uint32_t command[2] = {type, 0};
        check(enqueue(handle, command, 0) == S_OK, "XApuEnqueueCommand accepts a measured command type");
    }
    check(result_count(handle, 0) == 0, "XApuGetResultCount stays 0: commands are accepted and dropped");
    uint32_t result[20] = {};  // the title's dequeue local: a 0x50-byte zeroed buffer of result records
    check(dequeue(handle, result) == kXApuEPendingResults,
          "XApuDequeueResult reports the pending-results HRESULT on the empty queue");
    check(enqueue(nullptr, result, 0) == E_INVALIDARG, "XApuEnqueueCommand(nullptr handle) returns E_INVALIDARG");
    check(enqueue(handle, nullptr, 0) == E_INVALIDARG, "XApuEnqueueCommand(nullptr command) returns E_INVALIDARG");
    check(dequeue(nullptr, result) == E_INVALIDARG, "XApuDequeueResult(nullptr handle) returns E_INVALIDARG");
    check(dequeue(handle, nullptr) == E_INVALIDARG, "XApuDequeueResult(nullptr output) returns E_INVALIDARG");

    // ---- Handle lifetime: two connections are distinct; a stale handle is refused, not freed twice ----
    uint8_t out16b[16] = {};
    void* other = nullptr;
    check(connect(block, out16b, &other) == S_OK && other != nullptr, "a second XApuConnect succeeds");
    check(other != handle, "each connection has its own handle");
    uint64_t base_b = 0;
    std::memcpy(&base_b, out16b, sizeof base_b);
    check(base_b != 0 && base_b != base, "each connection has its own shared buffer");
    check(disconnect(handle) == S_OK, "XApuDisconnect frees the first connection");
    check(disconnect(handle) == E_INVALIDARG, "XApuDisconnect refuses the same (stale) handle a second time");
    check(disconnect(nullptr) == E_INVALIDARG, "XApuDisconnect(nullptr) returns E_INVALIDARG");
    check(disconnect(other) == S_OK, "XApuDisconnect frees the second connection");
    check(result_count(nullptr, 0) == 0, "XApuGetResultCount(nullptr handle) is a safe 0");

    // ---- chad: connect, activate, submit, deactivate, disconnect ----
    uint8_t title_buffer[16];
    std::memset(title_buffer, 0x3C, sizeof title_buffer);
    void* dsp = nullptr;
    check(dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, &dsp) == S_OK && dsp != nullptr,
          "XDspConnectWithMaximumStreamLimit returns S_OK with a non-null handle");
    bool title_buffer_untouched = true;
    for (uint8_t byte : title_buffer) title_buffer_untouched = title_buffer_untouched && byte == 0x3C;
    check(title_buffer_untouched, "the chad base buffer stays the title's: the shim never touches it");
    check(dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, nullptr) == E_POINTER,
          "XDspConnectWithMaximumStreamLimit(nullptr handle) returns E_POINTER");

    void* status = nullptr;
    void* stream = nullptr;
    check(activate(dsp, nullptr, &status, &stream) == S_OK, "XDspActivate succeeds");
    check(status != nullptr && stream != nullptr, "XDspActivate writes both outputs");
    if (status && stream) {
        bool zeroed = true;
        for (size_t i = 0; i < 256; ++i) zeroed = zeroed && static_cast<uint8_t*>(status)[i] == 0 &&
                                                       static_cast<uint8_t*>(stream)[i] == 0;
        check(zeroed, "the status and stream blocks are zeroed");
        check(*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(status) + 4) == 0 &&
                  *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(status) + 0xC) == 0,
              "the status dwords the title waits on (at +4 and +0xC) read 0");
    }
    check(activate(nullptr, nullptr, &status, &stream) == E_INVALIDARG, "XDspActivate(nullptr handle) returns E_INVALIDARG");

    uint32_t command_id = 0xDEADBEEFu;
    check(submit(stream, title_buffer, &command_id) == S_OK, "XDspSubmitCommand returns S_OK");
    check(command_id == 0xDEADBEEFu, "XDspSubmitCommand leaves the caller's id as it wrote it (status reads complete)");
    if (status) {
        check(*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(status) + 4) == 0 &&
                  *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(status) + 0xC) == 0,
              "the status block still reports command done, no error after a submit");
    }

    check(deactivate(dsp, 1, 2, 3) == S_OK, "XDspDeactivate succeeds");
    check(deactivate(nullptr, 0, 0, 0) == E_INVALIDARG, "XDspDeactivate(nullptr handle) returns E_INVALIDARG");
    check(dsp_disconnect(dsp) == S_OK, "XDspDisconnect frees the connection");
    check(dsp_disconnect(dsp) == E_INVALIDARG, "XDspDisconnect refuses the same (stale) handle a second time");
    check(dsp_disconnect(nullptr) == E_INVALIDARG, "XDspDisconnect(nullptr) returns E_INVALIDARG");

    // Repeated connect/disconnect cycles must not fail and must not hand out the same live handle twice.
    bool cycles_ok = true;
    for (int i = 0; i < 32; ++i) {
        void* cycle_handle = nullptr;
        uint8_t cycle_out[16] = {};
        cycles_ok = cycles_ok && connect(block, cycle_out, &cycle_handle) == S_OK && cycle_handle &&
                    disconnect(cycle_handle) == S_OK;
    }
    check(cycles_ok, "32 connect/disconnect cycles all succeed");
    bool dsp_cycles_ok = true;
    for (int i = 0; i < 32; ++i) {
        void* cycle_handle = nullptr;
        dsp_cycles_ok = dsp_cycles_ok && dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, &cycle_handle) == S_OK &&
                        cycle_handle && dsp_disconnect(cycle_handle) == S_OK;
    }
    check(dsp_cycles_ok, "32 chad connect/disconnect cycles all succeed");

    // ---- The trace: one event per call, with the caller, and nothing before the trace is opened ----
    test_trace(connect, disconnect, enqueue, result_count, dequeue, dsp_connect, activate, deactivate, dsp_disconnect,
               submit);

    std::printf("xapu_chad_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
