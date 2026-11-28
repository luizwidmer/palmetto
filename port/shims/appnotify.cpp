// PLM app-state notification shim (round-24 first-menus push).
//
// The title registers an app-state handler with RegisterAppStateChangeNotification (imported from
// api-ms-win-core-psm-appnotify-l1-1-0.dll, with RegisterAppConstrainedChangeNotification from -l1-1-1.dll) during the
// boot-media object construction 0x5A91C40. On the console the OS delivers an initial notification for the current
// state; on this desktop host the api set exists but never delivers a notification for the process, so the boot-media
// producer route (callback selector 0x2217C70 -> producer 0x2218180, which queues the state-2 item that sets
// 0x71B90AD and dispatches the type-0x24 item that sets 0x75DF0D8 -> AC 0x71B90AC) never runs. Measured boots read both
// present-pass keys 0 (docs/decomp_first_ui.md sections 11/17) and forcing the consumer keys deadlocked (section 20.8).
//
// PALMETTO_TITLE_APPSTATE_CALLBACK=1 answers the registration with one synthetic app-state change on the registering
// thread (the documented immediate initial notification), with the current-state value Running. It is a diagnostic that
// invents exactly the notification the host does not deliver; off, every call forwards to the host api set unchanged.
#include "shims.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace rt::shims::appnotify {
namespace {

struct AppStateChange {
    std::uint32_t reason;  // APPSTATE_CHANGE_REASON
    std::uint32_t state;   // APPLICATION_STATE (1 = running)
};

// Windows' PAPPSTATE_CHANGE_HANDLER passes the change FIRST and the registration context SECOND. The title's
// selector at 0x2217C70 tests RDX (the context) for non-null, tests CL (a byte of the change pointer) and then moves
// RDX into RCX before jumping to producer 0x2218180 - so the producer receives the boot-media object from the
// registration context, not the notification. Calling it as (context, change) passed the notification struct as the
// module object and crashed at 0x2217DF0 (wild read of +0x3B8).
using Handler = void (*)(const AppStateChange* change, void* context);

bool switch_on() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_TITLE_APPSTATE_CALLBACK");
        return value && std::strcmp(value, "1") == 0;
    }();
    return on;
}

// PALMETTO_TITLE_APPSTATE_DELAY_MS: when the synthetic notification is delivered (default 60 s). Value switch: a
// negative or absent value keeps the default, and it is clamped to 10 minutes.
std::uint32_t delivery_delay_ms() {
    static const std::uint32_t value = [] {
        const char* text = std::getenv("PALMETTO_TITLE_APPSTATE_DELAY_MS");
        if (!text || !*text) return 60000u;
        const long parsed = std::strtol(text, nullptr, 10);
        if (parsed < 0) return 60000u;
        if (parsed > 600000) return 600000u;
        return static_cast<std::uint32_t>(parsed);
    }();
    return value;
}

// The host api set as the default path: forward the call exactly as before this shim existed.
template <class Fn>
Fn host_function(const char* dll, const char* name) {
    HMODULE module = ::GetModuleHandleA(dll);
    if (!module) module = ::LoadLibraryA(dll);
    return module ? reinterpret_cast<Fn>(::GetProcAddress(module, name)) : nullptr;
}

struct Registration {
    Handler callback = nullptr;
    void* context = nullptr;
};

Registration registrations[4];

std::size_t store(Handler callback, void* context) {
    for (std::size_t i = 0; i < 4; ++i) {
        if (!registrations[i].callback) {
            registrations[i] = {callback, context};
            return i;
        }
    }
    return 0;
}

}  // namespace

HRESULT __stdcall register_app_state_change(Handler callback, void* context, void** registration) {
    if (!callback || !registration) return E_INVALIDARG;
    if (!switch_on()) {
        using Real = HRESULT(__stdcall*)(Handler, void*, void**);
        if (auto* real = host_function<Real>("api-ms-win-core-psm-appnotify-l1-1-0.dll",
                                           "RegisterAppStateChangeNotification")) {
            return real(callback, context, registration);
        }
        return E_NOTIMPL;
    }
    const std::size_t slot = store(callback, context);
    *registration = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot + 1));
    // Delivered asynchronously: calling the handler synchronously from inside the registration re-entered the module
    // during construction and the title never reached the renderer (a 240 s arm with 0 D3D12 calls). The real OS
    // delivers the initial notification after registration returns; 500 ms gives the module time to finish.
    static std::atomic<bool> delivered{false};
    if (!delivered.exchange(true)) {
        std::thread([callback, context] {
            // 60 s: the selector/producer are reached at 20 s, but the type-0x24 subscriber (handler 0x247B500) is not
            // registered yet (no poke hit), so nothing downstream changes. By 60 s the title is in session 3 and the
            // boot-media module has its subscriber list and frame loop running.
            std::this_thread::sleep_for(std::chrono::milliseconds(delivery_delay_ms()));
            // The title's selector tests CL (the low byte of the change pointer) and takes the alternate path when it
            // is zero. A heap/stack buffer can sit at a page-aligned address, so hand it an odd byte offset: the selector
            // then enters the producer 0x2218180 with the registration context.
            alignas(16) static thread_local unsigned char storage[64];
            auto* change = reinterpret_cast<AppStateChange*>(storage + 1);
            *change = AppStateChange{0, 1};  // reason unknown, state running
            std::fprintf(stderr,
                         "[rt] appnotify: synthetic app-state change delivered (reason %u state %u change %p context %p, "
                         "CL 0x%02x) to the boot-media handler\n",
                         change->reason, change->state, static_cast<void*>(change), context,
                         static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(change) & 0xffu));
            std::fflush(stderr);
            // Diagnostic: the producer 0x2218180 enters FUN_142217DE0 (the item enqueue helper) before it dispatches.
            // In the measured arm the dispatcher 0x2217B80 is never reached, so the helper may be waiting for ring
            // space. The ring fields (the helper's own offsets) are sampled once a second while the callback runs.
            std::thread monitor([context] {
                for (int i = 0; i < 60; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    const auto* base = static_cast<const unsigned char*>(context);
                    std::uint32_t ring0 = 0, armed = 0, pending = 0;
                    std::uint64_t ring1 = 0;
                    std::memcpy(&ring0, base + 0x3b8, sizeof(ring0));
                    std::memcpy(&ring1, base + 0x3c0, sizeof(ring1));
                    std::memcpy(&armed, base + 0x3d0, 1);
                    std::memcpy(&pending, base + 0x3d4, sizeof(pending));
                    std::fprintf(stderr,
                                 "[rt] appnotify monitor +%ds: ring0 0x%08x ring1 0x%016llx armed 0x%02x pending 0x%08x\n",
                                 i + 1, ring0, static_cast<unsigned long long>(ring1),
                                 static_cast<unsigned>(armed & 0xffu), pending);
                    std::fflush(stderr);
                }
            });
            monitor.detach();
            // PALMETTO_TITLE_APPSTATE_ARM=1: the enqueue helper FUN_142217DE0 waits until the boot-media module's arm
            // mask (+0x3D0) has the item-class bits for the producer's calls (0x08, 0x10, 0x20). The monitor shows the
            // mask stuck at 0x02 in every measured arm, so the producer spins forever. Force those bits for the
            // diagnostic and see whether the dispatcher 0x2217B80 and handler 0x247B500 are then reached.
            static const bool force_arm = [] {
                const char* value = std::getenv("PALMETTO_TITLE_APPSTATE_ARM");
                return value && std::strcmp(value, "1") == 0;
            }();
            if (force_arm) {
                auto* armed_byte = static_cast<unsigned char*>(context) + 0x3d0;
                const unsigned before = *armed_byte;
                *armed_byte = static_cast<unsigned char>(before | 0x38);
                std::fprintf(stderr, "[rt] appnotify: arm mask forced 0x%02x -> 0x%02x\n", before,
                             static_cast<unsigned>(*armed_byte));
                std::fflush(stderr);
            }
            callback(change, context);
            std::fprintf(stderr, "[rt] appnotify: callback returned\n");
            std::fflush(stderr);
        }).detach();
    }
    return S_OK;
}

HRESULT __stdcall unregister_app_state_change(void* registration) {
    if (!switch_on()) {
        using Real = HRESULT(__stdcall*)(void*);
        if (auto* real = host_function<Real>("api-ms-win-core-psm-appnotify-l1-1-0.dll",
                                           "UnregisterAppStateChangeNotification")) {
            return real(registration);
        }
        return E_NOTIMPL;
    }
    const auto slot = reinterpret_cast<std::uintptr_t>(registration);
    if (slot >= 1 && slot <= 4) registrations[slot - 1] = {};
    return S_OK;
}

HRESULT __stdcall register_app_constrained_change(void* selector, void* context, void** registration) {
    if (!selector || !registration) return E_INVALIDARG;
    if (!switch_on()) {
        using Real = HRESULT(__stdcall*)(void*, void*, void**);
        if (auto* real = host_function<Real>("api-ms-win-core-psm-appnotify-l1-1-1.dll",
                                           "RegisterAppConstrainedChangeNotification")) {
            return real(selector, context, registration);
        }
        return E_NOTIMPL;
    }
    // The constrained-change selector does not branch to the boot-media producer (docs/decomp_first_ui.md 11.2), so
    // only a valid handle is answered here.
    *registration = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    return S_OK;
}

HRESULT __stdcall unregister_app_constrained_change(void* registration) {
    if (!switch_on()) {
        using Real = HRESULT(__stdcall*)(void*);
        if (auto* real = host_function<Real>("api-ms-win-core-psm-appnotify-l1-1-1.dll",
                                           "UnregisterAppConstrainedChangeNotification")) {
            return real(registration);
        }
        return E_NOTIMPL;
    }
    return S_OK;
}

}  // namespace rt::shims::appnotify

SHIM_TABLE(appnotify,
    {"api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification",
     reinterpret_cast<void*>(&rt::shims::appnotify::register_app_state_change)},
    {"api-ms-win-core-psm-appnotify-l1-1-0.dll", "UnregisterAppStateChangeNotification",
     reinterpret_cast<void*>(&rt::shims::appnotify::unregister_app_state_change)},
    {"api-ms-win-core-psm-appnotify-l1-1-1.dll", "RegisterAppConstrainedChangeNotification",
     reinterpret_cast<void*>(&rt::shims::appnotify::register_app_constrained_change)},
    {"api-ms-win-core-psm-appnotify-l1-1-1.dll", "UnregisterAppConstrainedChangeNotification",
     reinterpret_cast<void*>(&rt::shims::appnotify::unregister_app_constrained_change)},
);