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

using Handler = void (*)(void* context, const AppStateChange* change);

bool switch_on() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_TITLE_APPSTATE_CALLBACK");
        return value && std::strcmp(value, "1") == 0;
    }();
    return on;
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
            // 20 s: 500 ms and 3 s both raced the boot-media construction in some arms (crash at game+0x2217DF0, wild read
            // within ~1.5 s of the callback); by 20 s the module, its dispatch list and the stage-2 handshake are built.
            std::this_thread::sleep_for(std::chrono::milliseconds(20000));
            const AppStateChange change{0, 1};  // reason unknown, state running
            std::fprintf(stderr,
                         "[rt] appnotify: synthetic app-state change delivered (reason %u state %u context %p) to the "
                         "boot-media handler\n",
                         change.reason, change.state, context);
            std::fflush(stderr);
            callback(context, &change);
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