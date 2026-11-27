// xgameruntime: XGameEventImpl, XGameUiImpl, XGameStreamingImpl, XDisplayImpl, XUserDeviceImpl, XAccessibilityImpl and
// XLauncherImpl, offline: small classes a title calls early.
//
// Each XGameEvent*, XGameUi* and XGameStreaming* function in xgameruntime.lib asks QueryApiImpl for its class and
// calls one slot with its own arguments after `this`. The class and interface ids and the method order are
// xodus-gaming/xgameruntime-docs' COM pages (GPL-3.0; facts only, nothing copied); IUnknown takes slots 0-2, so each
// class's first method is slot 3. Signatures follow Microsoft's public GDK reference (learn.microsoft.com/gaming/gdk).
// No GDK header or library code is used.
//
//   XGameEventImpl      XGameEventWrite (telemetry) succeeds and is dropped; the first event's name is logged, and
//                       PALMETTO_LOG_GAME_EVENTS=1 logs every one.
//   XGameUiImpl         a message dialog or error dialog has no screen: its title and text go to the log and it
//                       completes at once with the dialog's default button (error dialogs: dismissed). Every other UI
//                       (profile cards, pickers, text entry, web authentication, invites) fails before starting.
//   XGameStreamingImpl  the game is never streamed: initialising succeeds, IsStreaming is false, there are no
//                       clients, the connection-state registration hands out a token that never fires.
//   XDisplayImpl        XDisplayTryEnableHdrMode: by default the host desktop's own answer, measured from the primary
//                       DXGI output: Enabled with the output's luminance when the desktop runs HDR10, Disabled when it
//                       does not (or when no output answers). PALMETTO_HDR=1 forces Enabled with the documented
//                       defaults, =0/off/false/no forces Disabled (the pre-slice answer); any other value is refused by
//                       name and auto applies. See docs/peer_port_first_ui.md section 14.
//   XUserDeviceImpl     every device belongs to the offline user; associations and audio endpoints never change, so
//                       their change registrations are kept until unregistered and never called back; the user has
//                       no audio endpoint of its own; finding a controller shows no UI (as on desktop).
//   XAccessibilityImpl  high contrast is Off, closed captions can't be configured. The speech surface is a console
//                       answer by default: one silent voice installed, XSpeechSynthesizerCreate hands out a
//                       synthesizer handle, and text/SSML streams are a valid empty WAVE container (see the section
//                       below and docs/first_run_path.md). PALMETTO_SILENT_SPEECH=0 refuses the create with E_FAIL
//                       and a null handle, the answer this tree measured before the default change (the opt-out kept
//                       for the fallback and the tests). Speech-to-text display calls fail (E_FAIL).
//   XLauncherImpl       XLaunchUri fails (E_FAIL): nothing is launched. The two display timeout deferral calls are
//                       served with a Windows "display required" power request, the console behaviour the peers
//                       implement; PALMETTO_DISPLAY_DEFERRAL=0/off/false/no restores the pre-slice E_FAIL answer.
//
// Slots whose signature the public pages don't give answer with a value that is safe whichever way the caller reads
// it: see `client_not_connected` (XGameStreaming) and `offline_failure`.
#include <windows.h>

// The host display's HDR state is read through the real dxgi.dll, loaded at run time: the runtime links no DXGI import
// (the guest's DXGI calls are this tree's own proxy, port/shims/dxgi_com.cpp) and no build file changes for this slice.
// Only the COM interfaces and the DXGI_OUTPUT_DESC1 fields are used; CreateDXGIFactory1 is resolved by name.
#include <dxgi1_6.h>

#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include "platform_trace.h"

namespace rt::shims {
void* xgameruntime_user_new_handle();   // xgameruntime_user.cpp
}

namespace {

using XUserHandle = void*;

// XGameErr.h values, from the GDK "Error codes" page.
constexpr HRESULT E_GAMESTREAMING_NOT_INITIALIZED = static_cast<HRESULT>(0x89245400);

constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

#define FN(f) reinterpret_cast<const void*>(&f)

// A process-wide singleton per class, as the other runtime classes: AddRef and Release don't free.
ULONG STDMETHODCALLTYPE object_add_ref(void*) { return 2; }
ULONG STDMETHODCALLTYPE object_release(void*) { return 1; }

// One class: its object (a pointer to its vtable) and the interface ids it answers with that vtable.
struct ClassObject {
    const void* const* vtable;
};

HRESULT answer(ClassObject* object, const GUID* begin, const GUID* end, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == kIUnknown) {
        *out = object;
        return S_OK;
    }
    for (const GUID* id = begin; id != end; ++id)
        if (iid == *id) {
            *out = object;
            return S_OK;
        }
    return E_NOINTERFACE;
}

// A slot the docs don't name: E_NOTIMPL, so nothing reads outputs it didn't write.
template <const char* Class, int Slot> HRESULT STDMETHODCALLTYPE unknown_slot(void*) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        std::fprintf(stderr, "[rt] xgameruntime: %s slot %d (unknown in the docs) called; E_NOTIMPL\n", Class, Slot);
    return E_NOTIMPL;
}

// A UI the offline runtime doesn't have. The *Async function fails before starting, so the XAsyncBlock is never used
// and no callback runs; its Result partners return the same error. Arguments don't matter (x64: the caller owns the
// argument area).
template <const char* Class, int Slot> HRESULT STDMETHODCALLTYPE offline_failure(void*) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        std::fprintf(stderr, "[rt] xgameruntime: %s slot %d is not available offline (E_FAIL)\n", Class, Slot);
    return E_FAIL;
}

// ======================= XGameEventImpl =======================

constexpr char kEventName[] = "XGameEventImpl";
constexpr GUID kEventClass = {0xbbfbdcc7, 0xbfe7, 0x409b, {0xa5, 0xca, 0xed, 0xf0, 0x54, 0x96, 0x0b, 0x4d}};
std::atomic<long> g_events{0};

bool log_every_event() {
    static const bool on = [] {
        char value[4] = {};
        size_t length = 0;
        return getenv_s(&length, value, "PALMETTO_LOG_GAME_EVENTS") == 0 && length == 2 && value[0] == '1';
    }();
    return on;
}

// 3: HRESULT XGameEventWrite(XUserHandle user, const char* serviceConfigId, const char* playSessionId,
//                            const char* eventName, const char* dimensionsJson, const char* measurementsJson)
HRESULT STDMETHODCALLTYPE event_write(void*, XUserHandle, const char*, const char*, const char* name,
                                      const char* dimensions, const char* measurements) {
    if (!name) return E_INVALIDARG;
    const long n = g_events.fetch_add(1);
    if (log_every_event())
        std::fprintf(stderr, "[rt] game event %s dimensions=%s measurements=%s\n", name, dimensions ? dimensions : "",
                     measurements ? measurements : "");
    else if (n == 0)
        std::fprintf(stderr, "[rt] xgameruntime: game events are dropped offline (first: %s; "
                             "PALMETTO_LOG_GAME_EVENTS=1 logs them)\n", name);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE event_query_interface(void*, REFIID iid, void** out);
const void* const kEventVtable[] = {
    FN(event_query_interface), FN(object_add_ref), FN(object_release),   // 0-2 IUnknown
    FN(event_write),                                                     // 3 XGameEventWrite
};
ClassObject g_event_object{kEventVtable};
const GUID kEventInterfaces[] = {kEventClass};
HRESULT STDMETHODCALLTYPE event_query_interface(void*, REFIID iid, void** out) {
    return answer(&g_event_object, std::begin(kEventInterfaces), std::end(kEventInterfaces), iid, out);
}

// ======================= XGameUiImpl =======================

constexpr char kUiName[] = "XGameUiImpl";
constexpr GUID kUiClass = {0xdfcd4649, 0x4ff8, 0x4043, {0xba, 0x07, 0x35, 0xd6, 0x07, 0xdf, 0x98, 0xb0}};
const GUID kUiInterfaces[] = {
    kUiClass,   // the class id, which a thunk may pass for its implemented interface (IXGameUiImpl4)
    {0x6eeaa73e, 0x9669, 0x43ad, {0xa2, 0xc7, 0xd0, 0xda, 0x4e, 0x1f, 0x50, 0xa1}},   // IXGameUiImpl
    {0x36a03122, 0x9ea3, 0x4a3a, {0xa8, 0xa4, 0x89, 0x9c, 0xfd, 0x85, 0xd7, 0xdb}},   // IXGameUiImpl2
    {0xade7eba1, 0x2093, 0x42ce, {0xa5, 0x44, 0xa5, 0x23, 0xa6, 0x67, 0x90, 0xe0}},   // IXGameUiImpl3
    {0xeaf669df, 0x5542, 0x4590, {0x99, 0xa3, 0x8d, 0xc0, 0x61, 0xf8, 0x37, 0xcc}},   // IXGameUiImpl4
};

enum class XGameUiMessageDialogButton : uint32_t { First = 0, Second = 1, Third = 2 };

const char kDialogIdentity = 0, kErrorDialogIdentity = 0;

// The answer of a message dialog rides in the async context: the button, which never needs more.
HRESULT CALLBACK dialog_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin:
        return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork:
        XAsyncComplete(data->async, S_OK, sizeof(XGameUiMessageDialogButton));
        return S_OK;
    case XAsyncOp::GetResult:
        if (!data->buffer || data->bufferSize < sizeof(XGameUiMessageDialogButton)) return E_INVALIDARG;
        *static_cast<XGameUiMessageDialogButton*>(data->buffer) =
            static_cast<XGameUiMessageDialogButton>(reinterpret_cast<uintptr_t>(data->context));
        return S_OK;
    default:
        return S_OK;
    }
}

HRESULT CALLBACK error_dialog_provider(XAsyncOp op, const XAsyncProviderData* data) {
    if (op == XAsyncOp::Begin) return XAsyncSchedule(data->async, 0);
    if (op == XAsyncOp::DoWork) XAsyncComplete(data->async, S_OK, 0);
    return S_OK;
}

// 3: HRESULT XGameUiShowMessageDialogAsync(XAsyncBlock* async, const char* titleText, const char* contentText,
//        const char* firstButtonText, const char* secondButtonText, const char* thirdButtonText,
//        XGameUiMessageDialogButton defaultButton, XGameUiMessageDialogButton cancelButton)
// The parameter list is the one the GDK reference gives; only the result-page and the enum were checked for this
// file. No caller confirms it: tooling/gdk_runtime_calls.py finds no thunk for this slot in the executable
// (docs/runtime_imports.md). The dialog is "answered" with its default button.
HRESULT STDMETHODCALLTYPE ui_message_dialog_async(void*, XAsyncBlock* async, const char* title, const char* content,
                                                  const char* first, const char* second, const char* third,
                                                  XGameUiMessageDialogButton default_button, XGameUiMessageDialogButton) {
    if (!async) return E_POINTER;
    const auto button = static_cast<uint32_t>(default_button);
    if (button > 2) return E_INVALIDARG;
    const char* labels[] = {first, second, third};
    std::fprintf(stderr, "[rt] game dialog: \"%s\": %s [buttons: %s%s%s%s%s; answered: %s]\n", title ? title : "",
                 content ? content : "", first ? first : "", second ? " / " : "", second ? second : "",
                 third ? " / " : "", third ? third : "", labels[button] ? labels[button] : "(default)");
    return XAsyncBegin(async, reinterpret_cast<void*>(static_cast<uintptr_t>(button)), &kDialogIdentity,
                       "XGameUiShowMessageDialogAsync", dialog_provider);
}

// 4: HRESULT XGameUiShowMessageDialogResult(XAsyncBlock* async, XGameUiMessageDialogButton* resultButton)
HRESULT STDMETHODCALLTYPE ui_message_dialog_result(void*, XAsyncBlock* async, XGameUiMessageDialogButton* button) {
    if (!button) return E_POINTER;
    return XAsyncGetResult(async, &kDialogIdentity, sizeof(*button), button, nullptr);
}

// 14: HRESULT XGameUiShowErrorDialogAsync(XAsyncBlock* async, HRESULT errorCode, const char* context)
HRESULT STDMETHODCALLTYPE ui_error_dialog_async(void*, XAsyncBlock* async, HRESULT code, const char* context) {
    if (!async) return E_POINTER;
    std::fprintf(stderr, "[rt] game error dialog: HRESULT 0x%08lX%s%s [dismissed]\n", static_cast<unsigned long>(code),
                 context ? " context " : "", context ? context : "");
    return XAsyncBegin(async, nullptr, &kErrorDialogIdentity, "XGameUiShowErrorDialogAsync", error_dialog_provider);
}

// 15: HRESULT XGameUiShowErrorDialogResult(XAsyncBlock* async)
HRESULT STDMETHODCALLTYPE ui_error_dialog_result(void*, XAsyncBlock* async) {
    return XAsyncGetResult(async, &kErrorDialogIdentity, 0, nullptr, nullptr);
}

// 16: XGameUiSetNotificationPositionHint(position): there are no notifications to place.
HRESULT STDMETHODCALLTYPE ui_set_notification_hint(void*) { return S_OK; }

template <int Slot> HRESULT STDMETHODCALLTYPE ui_offline(void* self) { return offline_failure<kUiName, Slot>(self); }
template <int Slot> HRESULT STDMETHODCALLTYPE ui_unknown(void* self) { return unknown_slot<kUiName, Slot>(self); }

HRESULT STDMETHODCALLTYPE ui_query_interface(void*, REFIID iid, void** out);
const void* const kUiVtable[] = {
    FN(ui_query_interface), FN(object_add_ref), FN(object_release),   // 0-2 IUnknown
    FN(ui_message_dialog_async),       // 3  XGameUiShowMessageDialogAsync
    FN(ui_message_dialog_result),      // 4  XGameUiShowMessageDialogResult
    FN(ui_offline<5>),                 // 5  XGameUiShowSendGameInviteAsync
    FN(ui_offline<6>),                 // 6  XGameUiShowSendGameInviteResult
    FN(ui_offline<7>),                 // 7  XGameUiShowPlayerProfileCardAsync
    FN(ui_offline<8>),                 // 8  XGameUiShowPlayerProfileCardResult
    FN(ui_offline<9>),                 // 9  XGameUiShowAchievementsAsync
    FN(ui_offline<10>),                // 10 XGameUiShowAchievementsResult
    FN(ui_offline<11>),                // 11 XGameUiShowPlayerPickerAsync
    FN(ui_offline<12>),                // 12 XGameUiShowPlayerPickerResultCount
    FN(ui_offline<13>),                // 13 XGameUiShowPlayerPickerResult
    FN(ui_error_dialog_async),         // 14 XGameUiShowErrorDialogAsync
    FN(ui_error_dialog_result),        // 15 XGameUiShowErrorDialogResult
    FN(ui_set_notification_hint),      // 16 XGameUiSetNotificationPositionHint
    FN(ui_offline<17>),                // 17 XGameUiShowTextEntryAsync
    FN(ui_offline<18>),                // 18 XGameUiShowTextEntryResultSize
    FN(ui_offline<19>),                // 19 XGameUiShowTextEntryResult
    FN(ui_unknown<20>),                // 20 (unknown in the docs)
    FN(ui_unknown<21>),                // 21 (unknown in the docs)
    FN(ui_unknown<22>),                // 22 (unknown in the docs)
    FN(ui_unknown<23>),                // 23 (unknown in the docs)
    FN(ui_offline<24>),                // 24 XGameUiShowWebAuthenticationAsync
    FN(ui_offline<25>),                // 25 XGameUiShowWebAuthenticationResultSize
    FN(ui_offline<26>),                // 26 XGameUiShowWebAuthenticationResult
    FN(ui_offline<27>),                // 27 XGameUiShowWebAuthenticationWithOptionsAsync
    FN(ui_unknown<28>),                // 28 (unknown in the docs)
    FN(ui_unknown<29>),                // 29 (unknown in the docs)
    FN(ui_offline<30>),                // 30 XGameUiShowMultiplayerActivityGameInviteAsync (2)
    FN(ui_offline<31>),                // 31 XGameUiShowMultiplayerActivityGameInviteResult (2)
    FN(ui_unknown<32>),                // 32 (unknown in the docs) (2)
    FN(ui_unknown<33>),                // 33 (unknown in the docs) (2)
    FN(ui_offline<34>),                // 34 XGameUiTextEntryOpen (2)
    FN(ui_offline<35>),                // 35 XGameUiTextEntryClose (2)
    FN(ui_offline<36>),                // 36 XGameUiTextEntryGetState (2)
    FN(ui_offline<37>),                // 37 XGameUiTextEntryGetExtents (2)
    FN(ui_offline<38>),                // 38 XGameUiTextEntryUpdatePositionHint (2)
    FN(ui_offline<39>),                // 39 XGameUiTextEntryUpdateVisibility (2)
    FN(ui_offline<40>),                // 40 XGameUiShowStateShareAsync (3)
    FN(ui_offline<41>),                // 41 XGameUiShowStateShareResult (3)
    FN(ui_unknown<42>),                // 42 XGameUiSetUiCallbacks (4, PC-hosted UI; signature not given)
    FN(ui_unknown<43>),                // 43 XGameUiSetMessageDialogUiResponse (4)
    FN(ui_unknown<44>),                // 44 XGameUiSetPlayerPickerUiResponse (4)
    FN(ui_unknown<45>),                // 45 XGameUiSetTextEntryUiResponse (4)
    FN(ui_unknown<46>),                // 46 XGameUiSetPlayerProfileCardUiResponse (4)
    FN(ui_unknown<47>),                // 47 XGameUiSetSendGameInviteUiResponse (4)
    FN(ui_unknown<48>),                // 48 XGameUiSetAchievementsUiResponse (4)
    FN(ui_unknown<49>),                // 49 XGameUiSetMultiplayerActivityGameInviteUiResponse (4)
    FN(ui_unknown<50>),                // 50 XGameUiSetErrorDialogUiResponse (4)
};
static_assert(std::size(kUiVtable) == 51, "IUnknown + IXGameUiImpl's 27 methods + 10 + 2 + 9");
ClassObject g_ui_object{kUiVtable};
HRESULT STDMETHODCALLTYPE ui_query_interface(void*, REFIID iid, void** out) {
    return answer(&g_ui_object, std::begin(kUiInterfaces), std::end(kUiInterfaces), iid, out);
}

// ======================= XGameStreamingImpl =======================

constexpr GUID kStreamingClass = {0x0a2192aa, 0xb2d5, 0x4d58, {0x83, 0xbe, 0x38, 0x3b, 0x6d, 0x80, 0x79, 0x9e}};
const GUID kStreamingInterfaces[] = {
    kStreamingClass,   // the class id, for its implemented interface (IXGameStreamingImpl3)
    {0x8aff07f5, 0xa1bf, 0x4db8, {0x80, 0xa5, 0x31, 0xcc, 0xa0, 0xde, 0x51, 0xb7}},   // IXGameStreamingImpl
    {0x5f5e5169, 0x746c, 0x4001, {0xad, 0x1c, 0xda, 0x72, 0x8d, 0x01, 0xc9, 0xeb}},   // IXGameStreamingImpl2
    {0x57786622, 0x6605, 0x46d0, {0xb9, 0x17, 0x0f, 0x22, 0xbb, 0xcd, 0x9c, 0x52}},   // IXGameStreamingImpl3
};

std::mutex g_streaming_mutex;
uint64_t g_next_token = 1;

// 3: HRESULT XGameStreamingInitialize()
HRESULT STDMETHODCALLTYPE streaming_initialize(void*) { return S_OK; }
// 4: void XGameStreamingUninitialize()
void STDMETHODCALLTYPE streaming_uninitialize(void*) {}
// 5: bool XGameStreamingIsStreaming(): no client is ever connected.
bool STDMETHODCALLTYPE streaming_is_streaming(void*) { return false; }
// 9: uint32_t XGameStreamingGetClientCount()
uint32_t STDMETHODCALLTYPE streaming_client_count(void*) { return 0; }

// 12: HRESULT XGameStreamingRegisterConnectionStateChanged(XTaskQueueHandle queue, void* context,
//        XGameStreamingConnectionStateChangedCallback* callback, XTaskQueueRegistrationToken* token)
// That parameter list is the GDK reference's, not re-checked for this file, and no caller confirms it: the executable
// links no XGameStreamingImpl thunk (docs/runtime_imports.md). No client connects, so the callback never runs.
HRESULT STDMETHODCALLTYPE streaming_register_connection(void*, XTaskQueueHandle, void*, void* callback,
                                                        XTaskQueueRegistrationToken* token) {
    if (!callback || !token) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_streaming_mutex);
    token->token = g_next_token++;
    return S_OK;
}

// Unregistering (7, 13): nothing is ever pending. Returns true, in case the caller reads a bool; a void caller
// ignores it.
bool STDMETHODCALLTYPE streaming_unregister(void*) { return true; }

// Every other slot asks about a client, its touch controls or its stream, and there is none. The answer is
// E_GAMESTREAMING_NOT_INITIALIZED (0x89245400), chosen for its shape: a caller reading an HRESULT sees a failure, and
// one reading a bool (al) sees false, since its low byte is 0. The slots returning an enum or a count are the ones
// above, answered explicitly.
template <int Slot> HRESULT STDMETHODCALLTYPE client_not_connected(void*) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        std::fprintf(stderr, "[rt] xgameruntime: XGameStreamingImpl slot %d: no streaming client offline\n", Slot);
    return E_GAMESTREAMING_NOT_INITIALIZED;
}
static_assert((static_cast<uint32_t>(E_GAMESTREAMING_NOT_INITIALIZED) & 0xFF) == 0, "reads as false in al");

// 11: XGameStreamingConnectionState XGameStreamingGetConnectionState(XGameStreamingClientId client): 0, the first
// enumerator, taken to be Disconnected; there is no client to ask about either way, and the executable has no caller.
uint32_t STDMETHODCALLTYPE streaming_connection_state(void*) { return 0; }

HRESULT STDMETHODCALLTYPE streaming_query_interface(void*, REFIID iid, void** out);
const void* const kStreamingVtable[] = {
    FN(streaming_query_interface), FN(object_add_ref), FN(object_release),   // 0-2 IUnknown
    FN(streaming_initialize),              // 3  XGameStreamingInitialize
    FN(streaming_uninitialize),            // 4  XGameStreamingUninitialize
    FN(streaming_is_streaming),            // 5  XGameStreamingIsStreaming
    FN(client_not_connected<6>),           // 6  XGameStreamingRegisterClientPropertiesChanged (per client)
    FN(streaming_unregister),              // 7  XGameStreamingUnregisterClientPropertiesChanged
    FN(client_not_connected<8>),           // 8  XGameStreamingGetStreamPhysicalDimensions
    FN(streaming_client_count),            // 9  XGameStreamingGetClientCount
    FN(client_not_connected<10>),          // 10 XGameStreamingGetClients
    FN(streaming_connection_state),        // 11 XGameStreamingGetConnectionState
    FN(streaming_register_connection),     // 12 XGameStreamingRegisterConnectionStateChanged
    FN(streaming_unregister),              // 13 XGameStreamingUnregisterConnectionStateChanged
    FN(client_not_connected<14>),          // 14 XGameStreamingGetStreamAddedLatency
    FN(client_not_connected<15>),          // 15 XGameStreamingGetServerLocationNameSize
    FN(client_not_connected<16>),          // 16 XGameStreamingGetServerLocationName
    FN(client_not_connected<17>),          // 17 XGameStreamingHideTouchControls
    FN(client_not_connected<18>),          // 18 XGameStreamingShowTouchControlLayout
    FN(client_not_connected<19>),          // 19 XGameStreamingHideTouchControlsOnClient
    FN(client_not_connected<20>),          // 20 XGameStreamingShowTouchControlLayoutOnClient
    FN(client_not_connected<21>),          // 21 XGameStreamingIsTouchInputEnabled (reads as false)
    FN(client_not_connected<22>),          // 22 XGameStreamingGetLastFrameDisplayed
    FN(client_not_connected<23>),          // 23 XGameStreamingGetAssociatedFrame
    FN(client_not_connected<24>),          // 24 XGameStreamingGetGamepadPhysicality
    FN(client_not_connected<25>),          // 25 XGameStreamingUpdateTouchControlsState
    FN(client_not_connected<26>),          // 26 XGameStreamingUpdateTouchControlsStateOnClient
    FN(client_not_connected<27>),          // 27 XGameStreamingShowTouchControlsWithStateUpdate
    FN(client_not_connected<28>),          // 28 XGameStreamingShowTouchControlsWithStateUpdateOnClient
    FN(client_not_connected<29>),          // 29 XGameStreamingGetTouchBundleVersionNameSize
    FN(client_not_connected<30>),          // 30 XGameStreamingGetTouchBundleVersion
    FN(client_not_connected<31>),          // 31 XGameStreamingGetClientIPAddress
    FN(client_not_connected<32>),          // 32 XGameStreamingGetSessionId (2)
    FN(client_not_connected<33>),          // 33 XGameStreamingGetDisplayDetails (3)
    FN(client_not_connected<34>),          // 34 XGameStreamingSetResolution (3)
};
static_assert(std::size(kStreamingVtable) == 35, "IUnknown + IXGameStreamingImpl's 29 methods + 1 + 2");
ClassObject g_streaming_object{kStreamingVtable};
HRESULT STDMETHODCALLTYPE streaming_query_interface(void*, REFIID iid, void** out) {
    return answer(&g_streaming_object, std::begin(kStreamingInterfaces), std::end(kStreamingInterfaces), iid, out);
}


// ======================= XDisplayImpl =======================

constexpr char kDisplayName[] = "XDisplayImpl";
constexpr GUID kDisplayClass = {0x03f0fe74, 0xfdd9, 0x4e5c, {0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac, 0xc5}};
const GUID kDisplayInterfaces[] = {
    kDisplayClass,
    {0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}},   // IXDisplayImpl
};

enum class XDisplayHdrModePreference : uint32_t { PreferHdr = 0, PreferRefreshRate = 1 };
enum class XDisplayHdrModeResult : uint32_t { Unknown = 0, Enabled = 1, Disabled = 2 };
struct XDisplayHdrModeInfo {
    float minToneMapLuminance;            // nits
    float maxToneMapLuminance;
    float maxFullFrameToneMapLuminance;
};

// ---- The measured display answer (docs/peer_port_first_ui.md section 14) ----
//
// Measurement: the retail title calls only IXDisplayImpl slot 4 (one XDisplayImpl thunk in the image), from two
// wrappers, and the answer is consumed: image+0x5A38910 reads the three XDisplayHdrModeInfo floats into its display
// settings and sets its HDR-enabled word when the result is Enabled, while image+0x2481CA0 asks with a null info
// pointer. Our pre-slice answer was always Disabled unless a switch forced Enabled; the peers answer from the host:
// five-star's display service reports what the Windows desktop runs (DXGI_OUTPUT_DESC1's colour space and luminance,
// Enabled when it is HDR10), and the VIctory family answers Disabled only because its swap chain is SDR and says it
// would report the host's luminance once it can present HDR. This tree's presenter can compose and present HDR10
// planes (port/shims/xbox_present.cpp and its pixel tests), so the host answer is the honest one here.
//
// The answer rule, per call (no cached environment read, so a test can exercise every spelling):
//   * PALMETTO_HDR unset, empty or "auto": the host desktop's own answer. The primary output is read through the real
//     dxgi.dll: Enabled with its measured min/max/max-full-frame luminance when its colour space is HDR10 (PQ,
//     Rec.2020), Disabled when it is SDR; Disabled when no output answers (the pre-slice answer is the safe fallback).
//   * PALMETTO_HDR=1/on/true/yes: Enabled with the documented default luminances 0.01 / 1000 / 1000 nits (this is the
//     pre-slice forcing switch, kept so an HDR arm does not depend on the desktop).
//   * PALMETTO_HDR=0/off/false/no: Disabled, whatever the host runs (the pre-slice default).
//   * any other value: refused by name once, auto applies (a typo never silently forces either answer).
// The info pointer is optional. Every call is logged (first 8, then every 16th) with the preference, whether the caller
// passed an info pointer, and the answer's source, so an arm shows every display query the title makes.

enum class HdrSwitch { Auto = 0, ForcedOn = 1, ForcedOff = 2, RefusedByValue = 3 };

// Reads an environment variable into a bounded buffer through the Win32 environment (the same per-call read the speech
// switch uses, so a test's SetEnvironmentVariableA is seen). A value longer than the buffer is truncated, which can
// never be one of the accepted spellings, so the parser below refuses it by name.
bool environment_string(const char* name, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) return false;
    out[0] = '\0';
    const DWORD length = GetEnvironmentVariableA(name, out, static_cast<DWORD>(capacity));
    if (length == 0) return false;
    if (length >= capacity) out[capacity - 1] = '\0';
    return true;
}

// The three measured spellings plus auto, with the blank-trimmed value the project's other switches accept.
bool trimmed_equals_ignore_case(const char* value, const char* lower_word) {
    while (*value == ' ' || *value == '\t') ++value;
    std::size_t length = std::strlen(value);
    while (length && (value[length - 1] == ' ' || value[length - 1] == '\t')) --length;
    bool equal = true;
    std::size_t i = 0;
    for (; i < length && lower_word[i]; ++i) {
        char c = value[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != lower_word[i]) equal = false;
    }
    return equal && i == length && lower_word[i] == '\0';
}

HdrSwitch parse_hdr_switch(const char* value) {
    if (value == nullptr || *value == '\0') return HdrSwitch::Auto;
    if (trimmed_equals_ignore_case(value, "auto")) return HdrSwitch::Auto;
    if (trimmed_equals_ignore_case(value, "1") || trimmed_equals_ignore_case(value, "on") ||
        trimmed_equals_ignore_case(value, "true") || trimmed_equals_ignore_case(value, "yes"))
        return HdrSwitch::ForcedOn;
    if (trimmed_equals_ignore_case(value, "0") || trimmed_equals_ignore_case(value, "off") ||
        trimmed_equals_ignore_case(value, "false") || trimmed_equals_ignore_case(value, "no"))
        return HdrSwitch::ForcedOff;
    return HdrSwitch::RefusedByValue;
}

std::atomic<long> g_hdr_calls{0};
std::atomic<long> g_hdr_enabled_answers{0};
std::atomic<long> g_host_hdr_probes{0};

// The host display state. `available` is false when the real dxgi.dll could not be loaded or no output answered.
struct HostDisplayHdr {
    bool available = false;
    bool hdr10 = false;
    float min_luminance = 0.0f;
    float max_luminance = 0.0f;
    float max_full_frame_luminance = 0.0f;
};

// A test-only override of the host probe: mode -1 = probe the host, 0 = available SDR, 1 = available HDR10. The runtime
// never writes it; the unit test does, so the answer rule is exercised without depending on the machine's display.
std::atomic<int> g_test_host_mode{-1};
std::atomic<float> g_test_host_min{0.0f};
std::atomic<float> g_test_host_max{0.0f};
std::atomic<float> g_test_host_full{0.0f};

using CreateDxgiFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);

CreateDxgiFactory1Fn create_dxgi_factory1() {
    static const CreateDxgiFactory1Fn resolved = [] {
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (dxgi == nullptr) return static_cast<CreateDxgiFactory1Fn>(nullptr);
        return reinterpret_cast<CreateDxgiFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    }();
    return resolved;
}

// The primary output's HDR state through the host DXGI: the first adapter with an output, its output 0, its
// DXGI_OUTPUT_DESC1. The same source and fields five-star's display service reads. The window has no say here: the
// colour space of the desktop's output is what a console's XDisplayTryEnableHdrMode answer describes.
HostDisplayHdr probe_host_display_hdr() {
    HostDisplayHdr state;
    const CreateDxgiFactory1Fn create = create_dxgi_factory1();
    if (create == nullptr) return state;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr) return state;
    for (UINT adapter_index = 0; !state.available; ++adapter_index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (adapter == nullptr) continue;
        for (UINT output_index = 0; !state.available; ++output_index) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (output == nullptr) continue;
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
                output6 != nullptr) {
                DXGI_OUTPUT_DESC1 desc{};
                if (SUCCEEDED(output6->GetDesc1(&desc))) {
                    state.available = true;
                    state.hdr10 = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    state.min_luminance = desc.MinLuminance;
                    state.max_luminance = desc.MaxLuminance;
                    state.max_full_frame_luminance = desc.MaxFullFrameLuminance;
                }
                output6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return state;
}

HostDisplayHdr host_display_hdr() {
    ++g_host_hdr_probes;
    HostDisplayHdr state;
    const int override_mode = g_test_host_mode.load(std::memory_order_relaxed);
    if (override_mode >= 0) {
        if (override_mode == 2) return state;   // the test's "no output answered" case
        state.available = true;
        state.hdr10 = override_mode == 1;
        state.min_luminance = g_test_host_min.load(std::memory_order_relaxed);
        state.max_luminance = g_test_host_max.load(std::memory_order_relaxed);
        state.max_full_frame_luminance = g_test_host_full.load(std::memory_order_relaxed);
        return state;
    }
    return probe_host_display_hdr();
}

// What the info block is filled with when Enabled: the host's measured luminance range, or the documented defaults when
// the caller forced HDR on (the pre-slice forcing switch's values).
XDisplayHdrModeInfo enabled_hdr_info(const HostDisplayHdr& host) {
    if (host.available && host.hdr10) return {host.min_luminance, host.max_luminance, host.max_full_frame_luminance};
    return {0.01f, 1000.0f, 1000.0f};
}

// 4: XDisplayHdrModeResult XDisplayTryEnableHdrMode(XDisplayHdrModePreference preference, XDisplayHdrModeInfo* info)
uint32_t STDMETHODCALLTYPE display_try_enable_hdr(void*, XDisplayHdrModePreference preference, XDisplayHdrModeInfo* info) {
    char value[16] = {};
    const bool have_value = environment_string("PALMETTO_HDR", value, sizeof value);
    const HdrSwitch mode = parse_hdr_switch(have_value ? value : nullptr);
    const long call = g_hdr_calls.fetch_add(1) + 1;
    static std::atomic<bool> s_refused_logged{false};
    const bool log_this_call = call <= 8 || call % 16 == 0;
    if (mode == HdrSwitch::RefusedByValue && !s_refused_logged.exchange(true))
        std::fprintf(stderr,
                     "[rt] xgameruntime: PALMETTO_HDR value \"%s\" is not auto, 1/on/true/yes or 0/off/false/no; the "
                     "host's answer (auto) applies\n",
                     value);
    HostDisplayHdr host;
    bool enabled = false;
    const char* source = "";
    if (mode == HdrSwitch::ForcedOn) {
        enabled = true;
        source = "forced by PALMETTO_HDR";
    } else if (mode == HdrSwitch::ForcedOff) {
        enabled = false;
        source = "forced off by PALMETTO_HDR";
    } else {
        host = host_display_hdr();
        enabled = host.available && host.hdr10;
        source = !host.available      ? "auto: no host output answered"
                 : host.hdr10         ? "auto: host desktop HDR10"
                                      : "auto: host desktop SDR";
    }
    if (info != nullptr) *info = enabled ? enabled_hdr_info(host) : XDisplayHdrModeInfo{};
    if (enabled) ++g_hdr_enabled_answers;
    if (log_this_call) {
        if (enabled) {
            const XDisplayHdrModeInfo shown = enabled_hdr_info(host);
            std::fprintf(stderr,
                         "[rt] xgameruntime: XDisplayTryEnableHdrMode call %ld (preference %u, info %s): Enabled "
                         "(%.4f/%.3f/%.3f nits; %s)\n",
                         call, static_cast<unsigned>(preference), info != nullptr ? "given" : "null",
                         shown.minToneMapLuminance, shown.maxToneMapLuminance, shown.maxFullFrameToneMapLuminance,
                         source);
        } else {
            std::fprintf(stderr,
                         "[rt] xgameruntime: XDisplayTryEnableHdrMode call %ld (preference %u, info %s): Disabled "
                         "(%s)\n",
                         call, static_cast<unsigned>(preference), info != nullptr ? "given" : "null", source);
        }
    }
    return static_cast<uint32_t>(enabled ? XDisplayHdrModeResult::Enabled : XDisplayHdrModeResult::Disabled);
}

template <int Slot> HRESULT STDMETHODCALLTYPE display_unknown(void* self) { return unknown_slot<kDisplayName, Slot>(self); }

HRESULT STDMETHODCALLTYPE display_query_interface(void*, REFIID iid, void** out);
const void* const kDisplayVtable[] = {
    FN(display_query_interface), FN(object_add_ref), FN(object_release),   // 0-2 IUnknown
    FN(display_unknown<3>),                 // 3 (unknown in the docs)
    FN(display_try_enable_hdr),             // 4 XDisplayTryEnableHdrMode
};
ClassObject g_display_object{kDisplayVtable};
HRESULT STDMETHODCALLTYPE display_query_interface(void*, REFIID iid, void** out) {
    return answer(&g_display_object, std::begin(kDisplayInterfaces), std::end(kDisplayInterfaces), iid, out);
}

// ======================= XUserDeviceImpl =======================

constexpr GUID kUserDeviceClass = {0x7d824997, 0x10dc, 0x45ab, {0x86, 0xb7, 0x27, 0x37, 0x76, 0x7c, 0x0b, 0xf1}};
const GUID kUserDeviceInterfaces[] = {
    kUserDeviceClass,                                                                  // IXUserDeviceImpl
    {0x0cc6a956, 0xe7e1, 0x4fdf, {0x93, 0x41, 0x9d, 0x5d, 0xa6, 0x49, 0xeb, 0xc8}},    // IXUserDeviceImpl2 (no methods)
};

// APP_LOCAL_DEVICE_ID is taken to be 32 bytes (APP_LOCAL_DEVICE_ID_SIZE), not on the pages read for this file. No
// caller confirms it: the executable links no XUserDeviceImpl thunk (docs/runtime_imports.md).
struct APP_LOCAL_DEVICE_ID {
    uint8_t value[32];
};

const char kFindControllerIdentity = 0;

// 3: HRESULT XUserFindForDevice(const APP_LOCAL_DEVICE_ID* deviceId, XUserHandle* handle): every device belongs to
// the one offline user.
HRESULT STDMETHODCALLTYPE device_find_user(void*, const APP_LOCAL_DEVICE_ID* device, XUserHandle* out) {
    if (!device || !out) return E_INVALIDARG;
    *out = rt::shims::xgameruntime_user_new_handle();
    return S_OK;
}

// 4: HRESULT XUserRegisterForDeviceAssociationChanged(XTaskQueueHandle queue, void* context,
//        XUserDeviceAssociationChangedCallback* callback, XTaskQueueRegistrationToken* token)
// 5: bool XUserUnregisterForDeviceAssociationChanged(XTaskQueueRegistrationToken token, bool wait)
// 7: HRESULT XUserRegisterForDefaultAudioEndpointUtf16Changed(XTaskQueueHandle queue, void* context,
//        XUserDefaultAudioEndpointUtf16ChangedCallback* callback, XTaskQueueRegistrationToken* token)
// 8: bool XUserUnregisterForDefaultAudioEndpointUtf16Changed(XTaskQueueRegistrationToken token, bool wait)
//
// Each registration is kept (its kind, a duplicate of its queue, its context and callback) under a token unique
// across both kinds and never 0, until the title unregisters it with the matching function. Offline, the device
// association and the default audio endpoint never change, so no event is ever raised: no callback is queued or
// run, and nothing here calls one. Unregistering therefore never has to wait, whatever `wait` says, and removes the
// registration at once. It returns true for every token, as XUserUnregisterForChangeEvent does: a token already
// unregistered, never handed out or of the other kind is left alone (a title polling with wait = false would spin
// on false). queue and context are _In_opt_; a null queue stays null (the process queue, if an event ever came);
// callback and token are required (E_INVALIDARG).
//
// The state is namespace-scope and the lock a constinit SRW lock, not function-local statics: these calls run on
// the title's threads.
enum class DeviceEvent : uint32_t { Association = 0, AudioEndpoint = 1 };

struct DeviceRegistration {
    uint64_t token;
    DeviceEvent kind;
    XTaskQueueHandle queue;   // our duplicate, closed on unregister; null for the process queue
    void* context;
    void* callback;
};

constinit SRWLOCK g_device_lock = SRWLOCK_INIT;
std::vector<DeviceRegistration> g_device_registrations;   // guarded by g_device_lock
constinit uint64_t g_device_next_token = 1;               // guarded by g_device_lock

template <DeviceEvent Kind>
HRESULT STDMETHODCALLTYPE device_register(void*, XTaskQueueHandle queue, void* context, void* callback,
                                          XTaskQueueRegistrationToken* token) {
    if (!callback || !token) return E_INVALIDARG;
    XTaskQueueHandle kept = nullptr;
    if (queue) {
        const HRESULT hr = XTaskQueueDuplicateHandle(queue, &kept);
        if (FAILED(hr)) return hr;
    }
    uint64_t issued = 0;
    AcquireSRWLockExclusive(&g_device_lock);
    try {
        g_device_registrations.push_back({g_device_next_token, Kind, kept, context, callback});
        issued = g_device_next_token++;
    } catch (...) {   // out of memory: no exception reaches the title
    }
    ReleaseSRWLockExclusive(&g_device_lock);
    if (!issued) {
        if (kept) XTaskQueueCloseHandle(kept);
        return E_OUTOFMEMORY;
    }
    token->token = issued;
    return S_OK;
}

template <DeviceEvent Kind> bool STDMETHODCALLTYPE device_unregister(void*, XTaskQueueRegistrationToken token, bool) {
    XTaskQueueHandle queue = nullptr;
    AcquireSRWLockExclusive(&g_device_lock);
    for (auto it = g_device_registrations.begin(); it != g_device_registrations.end(); ++it)
        if (it->token == token.token && it->kind == Kind) {
            queue = it->queue;
            g_device_registrations.erase(it);
            break;
        }
    ReleaseSRWLockExclusive(&g_device_lock);
    if (queue) XTaskQueueCloseHandle(queue);
    return true;
}

// 6: HRESULT XUserGetDefaultAudioEndpointUtf16(XUserLocalId user, XUserDefaultAudioEndpointKind kind,
//        size_t endpointIdUtf16Count, wchar_t* endpointIdUtf16, size_t* endpointIdUtf16Used)
// The user has no audio endpoint of its own (no headset): ERROR_NOT_FOUND, with an empty string written.
HRESULT STDMETHODCALLTYPE device_audio_endpoint(void*, uint64_t, uint32_t, size_t count, wchar_t* id, size_t* used) {
    if (id && count) id[0] = L'\0';
    if (used) *used = 0;
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT CALLBACK find_controller_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin:
        return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork:
        XAsyncComplete(data->async, S_OK, sizeof(APP_LOCAL_DEVICE_ID));
        return S_OK;
    case XAsyncOp::GetResult:
        if (!data->buffer || data->bufferSize < sizeof(APP_LOCAL_DEVICE_ID)) return E_INVALIDARG;
        *static_cast<APP_LOCAL_DEVICE_ID*>(data->buffer) = APP_LOCAL_DEVICE_ID{};
        return S_OK;
    default:
        return S_OK;
    }
}

// 9: HRESULT XUserFindControllerForUserWithUiAsync(XUserHandle user, XAsyncBlock* async): no UI, as on desktop.
HRESULT STDMETHODCALLTYPE device_find_controller_async(void*, XUserHandle user, XAsyncBlock* async) {
    if (!user) return E_INVALIDARG;
    if (!async) return E_POINTER;
    return XAsyncBegin(async, nullptr, &kFindControllerIdentity, "XUserFindControllerForUserWithUiAsync",
                       find_controller_provider);
}

// 10: HRESULT XUserFindControllerForUserWithUiResult(XAsyncBlock* async, APP_LOCAL_DEVICE_ID* deviceId): all zero,
// as the GDK documents for desktop.
HRESULT STDMETHODCALLTYPE device_find_controller_result(void*, XAsyncBlock* async, APP_LOCAL_DEVICE_ID* device) {
    if (!device) return E_POINTER;
    return XAsyncGetResult(async, &kFindControllerIdentity, sizeof(*device), device, nullptr);
}

HRESULT STDMETHODCALLTYPE device_query_interface(void*, REFIID iid, void** out);
const void* const kUserDeviceVtable[] = {
    FN(device_query_interface), FN(object_add_ref), FN(object_release),   // 0-2 IUnknown
    FN(device_find_user),                   // 3  XUserFindForDevice
    FN(device_register<DeviceEvent::Association>),     // 4  XUserRegisterForDeviceAssociationChanged
    FN(device_unregister<DeviceEvent::Association>),   // 5  XUserUnregisterForDeviceAssociationChanged
    FN(device_audio_endpoint),                         // 6  XUserGetDefaultAudioEndpointUtf16
    FN(device_register<DeviceEvent::AudioEndpoint>),   // 7  XUserRegisterForDefaultAudioEndpointUtf16Changed
    FN(device_unregister<DeviceEvent::AudioEndpoint>), // 8  XUserUnregisterForDefaultAudioEndpointUtf16Changed
    FN(device_find_controller_async),       // 9  XUserFindControllerForUserWithUiAsync
    FN(device_find_controller_result),      // 10 XUserFindControllerForUserWithUiResult
};
static_assert(std::size(kUserDeviceVtable) == 11, "IUnknown + IXUserDeviceImpl's 8 methods");
ClassObject g_user_device_object{kUserDeviceVtable};
HRESULT STDMETHODCALLTYPE device_query_interface(void*, REFIID iid, void** out) {
    return answer(&g_user_device_object, std::begin(kUserDeviceInterfaces), std::end(kUserDeviceInterfaces), iid, out);
}

// ======================= XAccessibilityImpl =======================

constexpr char kAccessibilityName[] = "XAccessibilityImpl";
constexpr GUID kAccessibilityClass = {0x3e241977, 0x6237, 0x41e9, {0x85, 0x59, 0x10, 0x2c, 0x2d, 0x99, 0x83, 0xf1}};
const GUID kAccessibilityInterfaces[] = {
    kAccessibilityClass,   // IXAccessibilityImpl: the class id
    {0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}},   // IXAccessibilityImpl2
};

// 5: HRESULT XHighContrastGetMode(XHighContrastMode* mode): Off, the first enumerator. ASSUMPTION: the signature and
// that Off is 0 (GDK reference, not re-checked for this file).
HRESULT STDMETHODCALLTYPE accessibility_high_contrast(void*, uint32_t* mode) {
    if (!mode) return E_POINTER;
    *mode = 0;
    return S_OK;
}

// 4: HRESULT XClosedCaptionSetEnabled(bool enabled): there are no captions to show; accepted and dropped.
HRESULT STDMETHODCALLTYPE accessibility_captions_set_enabled(void*, bool) { return S_OK; }

// ---- the speech surface (XSpeechSynthesizer.h, Microsoft GDK reference) ----
//
// The retail executable links these XAccessibilityImpl slots (tooling/gdk_runtime_calls.py: 8-13, 15, 16 and 21 are
// referenced, 14 is not), and the argument counts its thunks pass after `this` are 2, 1, 1, 1, 2, 3, 2, 4 and 3: the
// documented parameter list of each call.
//    8  XSpeechSynthesizerEnumerateInstalledVoices(void* context, callback)
//    9  XSpeechSynthesizerCreate(XSpeechSynthesizerHandle* synthesizer)
//   10  XSpeechSynthesizerCloseHandle(XSpeechSynthesizerHandle)
//   11  XSpeechSynthesizerSetDefaultVoice(XSpeechSynthesizerHandle)
//   12  XSpeechSynthesizerSetCustomVoice(XSpeechSynthesizerHandle, const char* voiceId)
//   13  XSpeechSynthesizerCreateStreamFromText(XSpeechSynthesizerHandle, const char* text, stream*)
//   14  XSpeechSynthesizerCloseStreamHandle(XSpeechSynthesizerStreamHandle)
//   15  XSpeechSynthesizerGetStreamDataSize(XSpeechSynthesizerStreamHandle, size_t* size)
//   16  XSpeechSynthesizerGetStreamData(XSpeechSynthesizerStreamHandle, size_t bufferSize, void* buffer,
//                                       size_t* bufferUsed)
//   21  XSpeechSynthesizerCreateStreamFromSsml(XSpeechSynthesizerHandle, const char* ssml, stream*)
// The boot calls one of them: slot 9, once, at +23.74 s and +25.35 s in the two 130 s boots measured for
// docs/first_run_path.md, and the slot-8 enumerate follows it when the create succeeds.
//
// The default is the console answer: Microsoft's pages list Windows as a supported platform, so a host answers these
// calls, and this runtime has no voice to synthesize with. It therefore installs one silent voice and a stream that is
// a valid WAVE container with no sample data, so a title that narrates plays silence and goes on. The measured A/B that
// justified making it the default is docs/decomp_first_ui.md section 12 (the switch-on arm added the enumerate and
// nothing else; the create is refused no more, which is the console's answer to a create).
//
// PALMETTO_SILENT_SPEECH=0|off is the fallback: the pre-default answer of this tree (create E_FAIL with a null handle,
// no voice enumerated), kept because a caller that wants the exact old behaviour must be able to ask for it.

// PALMETTO_SILENT_SPEECH, read per call: the speech surface is not polled (the boot calls one slot once), and a test
// flips the variable between calls. Unset means the surface exists; "0" and "off" restore the pre-default refusal.
bool silent_speech_enabled() {
    char value[8] = {};
    const DWORD n = GetEnvironmentVariableA("PALMETTO_SILENT_SPEECH", value, sizeof value);
    return !(n > 0 && n < sizeof value && (std::strcmp(value, "0") == 0 || _stricmp(value, "off") == 0));
}

// A caller-supplied output pointer is only written after this proves the range committed and writable, the same rule
// as xgameruntime.cpp's writable_range: these slots receive console-ABI pointers, and writing one the caller did not
// map would be corruption, not an offline answer.
bool writable_range(const void* pointer, size_t size) {
    if (!pointer || size == 0) return false;
    const auto page_writable = [](const void* address) {
        MEMORY_BASIC_INFORMATION page{};
        if (VirtualQuery(address, &page, sizeof page) != sizeof page) return false;
        constexpr DWORD kProtected = PAGE_NOACCESS | PAGE_GUARD;
        constexpr DWORD kWrite = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return page.State == MEM_COMMIT && (page.Protect & kProtected) == 0 && (page.Protect & kWrite) != 0;
    };
    return page_writable(pointer) && page_writable(static_cast<const unsigned char*>(pointer) + size - 1);
}

bool readable_byte(const void* pointer) {
    MEMORY_BASIC_INFORMATION page{};
    return pointer && VirtualQuery(pointer, &page, sizeof page) == sizeof page && page.State == MEM_COMMIT &&
           (page.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

bool callable_pointer(const void* pointer) {
    MEMORY_BASIC_INFORMATION page{};
    if (!readable_byte(pointer) || VirtualQuery(pointer, &page, sizeof page) != sizeof page) return false;
    constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (page.Protect & kExecute) != 0;
}

// Length of a NUL-terminated caller string, reading only committed pages, stopping at `cap`: a text or voice id the
// caller points at is measured (a length, never the content) but never dereferenced past what VirtualQuery proved.
size_t readable_string_length(const char* text, size_t cap) {
    size_t done = 0;
    while (text && done < cap) {
        MEMORY_BASIC_INFORMATION page{};
        if (VirtualQuery(text + done, &page, sizeof page) != sizeof page || page.State != MEM_COMMIT ||
            (page.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            return done;
        const auto begin = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
        const auto at = reinterpret_cast<std::uintptr_t>(text + done);
        if (at < begin || at >= begin + page.RegionSize) return done;
        const size_t room = static_cast<size_t>(begin + page.RegionSize - at);
        const size_t count = room < cap - done ? room : cap - done;
        const void* zero = std::memchr(text + done, '\0', count);
        if (zero) return done + static_cast<size_t>(static_cast<const char*>(zero) - (text + done));
        done += count;
    }
    return done;
}

// XSpeechSynthesizerVoiceInformation (XSpeechSynthesizer.h, the GDK reference's member order and 40-byte x64 layout).
// The gender is XSpeechSynthesizerVoiceGender, a uint32_t enum: Female = 0, Male = 1.
struct SpeechVoiceInformation {
    const char* description;
    const char* display_name;
    std::uint32_t gender;
    const char* voice_id;
    const char* language;
};
static_assert(offsetof(SpeechVoiceInformation, voice_id) == 24 && sizeof(SpeechVoiceInformation) == 40,
              "XSpeechSynthesizerVoiceInformation, as the title's callback reads it");
using InstalledVoicesCallback = bool(CALLBACK*)(const SpeechVoiceInformation*, void*);

// The one voice the silent answer installs. The strings are the runtime's own (no game data); the id is what the
// title compares against when it selects a voice.
constexpr char kSilentVoiceId[] = "palmetto-silent";
constexpr SpeechVoiceInformation kSilentVoice = {"Palmetto silent voice (en-US)", "Palmetto silent voice", 0,
                                                 kSilentVoiceId, "en-US"};

// The two handles: addresses of process-lifetime objects, so the title passes back exactly what it was given. One of
// each is enough offline: every stream is the same silent WAVE, so two open streams would behave alike anyway.
struct SilentSpeechHandle {
    std::uint32_t magic;
};
SilentSpeechHandle g_silent_synthesizer{0x53594e54};   // 'SYNT'
SilentSpeechHandle g_silent_stream{0x5354524d};        // 'STRM'
bool is_silent_synthesizer(const void* handle) { return handle == &g_silent_synthesizer; }
bool is_silent_stream(const void* handle) { return handle == &g_silent_stream; }

// A valid 44-byte RIFF/WAVE container for 16 kHz, 16-bit mono PCM with no sample data. GetStreamData's page returns
// the audio in the WAVE container (`audio/wav`); a title that narrates plays this and goes on. The bytes are the WAVE
// format's, not a game's.
constexpr unsigned char kSilentWave[] = {
    'R',  'I',  'F',  'F',  0x24, 0x00, 0x00, 0x00, 'W',  'A',  'V',  'E',  // RIFF, 36 bytes after this field
    'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00,                          // fmt , 16-byte format block
    0x01, 0x00, 0x01, 0x00, 0x80, 0x3e, 0x00, 0x00,                          // PCM, 1 channel, 16000 Hz
    0x00, 0x7d, 0x00, 0x00, 0x02, 0x00, 0x10, 0x00,                          // 32000 B/s, align 2, 16-bit
    'd',  'a',  't',  'a',  0x00, 0x00, 0x00, 0x00,                          // data, no samples
};
static_assert(sizeof kSilentWave == 44, "the WAVE header and its empty data chunk");

// One trace line per call under PALMETTO_USER_TRACE (platform_trace.h), with the answered HRESULT: the switch-on boot
// shows whether the title goes past the create and which speech call it makes next.
void trace_speech(const char* what, HRESULT hr, void* caller) {
    rt::shims::platform_trace(what, caller, static_cast<std::uint64_t>(hr));
}

// 8: HRESULT XSpeechSynthesizerEnumerateInstalledVoices(void* context, callback). The console surface has a voice
// installed, so the default enumerates the one silent voice; the PALMETTO_SILENT_SPEECH=0 fallback has none and calls
// back zero times.
HRESULT STDMETHODCALLTYPE speech_enumerate_voices(void*, void* context, InstalledVoicesCallback callback) {
    if (!callable_pointer(reinterpret_cast<const void*>(callback))) {
        trace_speech("XSpeechSynthesizerEnumerateInstalledVoices", E_INVALIDARG, _ReturnAddress());
        return E_INVALIDARG;
    }
    if (silent_speech_enabled()) callback(&kSilentVoice, context);
    trace_speech("XSpeechSynthesizerEnumerateInstalledVoices", S_OK, _ReturnAddress());
    return S_OK;
}

// 9: HRESULT XSpeechSynthesizerCreate(XSpeechSynthesizerHandle* synthesizer). The console answer creates the
// synthesizer; the PALMETTO_SILENT_SPEECH=0 fallback refuses it with E_FAIL and a null handle.
HRESULT STDMETHODCALLTYPE speech_create(void* self, void** synthesizer) {
    const bool writable = synthesizer && writable_range(synthesizer, sizeof *synthesizer);
    if (!silent_speech_enabled()) {
        if (writable) *synthesizer = nullptr;
        const HRESULT hr = offline_failure<kAccessibilityName, 9>(self);
        trace_speech("XSpeechSynthesizerCreate", hr, _ReturnAddress());
        return hr;
    }
    if (!synthesizer) {
        trace_speech("XSpeechSynthesizerCreate", E_POINTER, _ReturnAddress());
        return E_POINTER;
    }
    if (!writable) {
        trace_speech("XSpeechSynthesizerCreate", E_POINTER, _ReturnAddress());
        return E_POINTER;
    }
    *synthesizer = &g_silent_synthesizer;
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        std::fprintf(stderr, "[rt] xgameruntime: speech synthesis is silent offline: one silent voice, empty WAVE "
                             "streams (PALMETTO_SILENT_SPEECH=0 restores the refusal)\n");
    trace_speech("XSpeechSynthesizerCreate", S_OK, _ReturnAddress());
    return S_OK;
}

// 10: HRESULT XSpeechSynthesizerCloseHandle(XSpeechSynthesizerHandle)
HRESULT STDMETHODCALLTYPE speech_close_synth(void*, void* synthesizer) {
    const HRESULT hr = is_silent_synthesizer(synthesizer) ? S_OK : E_INVALIDARG;
    trace_speech("XSpeechSynthesizerCloseHandle", hr, _ReturnAddress());
    return hr;
}

// 11: HRESULT XSpeechSynthesizerSetDefaultVoice(XSpeechSynthesizerHandle)
HRESULT STDMETHODCALLTYPE speech_set_default_voice(void*, void* synthesizer) {
    const HRESULT hr = is_silent_synthesizer(synthesizer) ? S_OK : E_INVALIDARG;
    trace_speech("XSpeechSynthesizerSetDefaultVoice", hr, _ReturnAddress());
    return hr;
}

// 12: HRESULT XSpeechSynthesizerSetCustomVoice(XSpeechSynthesizerHandle, const char* voiceId): only the enumerated
// voice exists, so any other id is refused.
HRESULT STDMETHODCALLTYPE speech_set_custom_voice(void*, void* synthesizer, const char* voice_id) {
    HRESULT hr = E_INVALIDARG;
    if (is_silent_synthesizer(synthesizer) && voice_id) {
        const size_t length = readable_string_length(voice_id, sizeof kSilentVoiceId);
        if (length == sizeof kSilentVoiceId - 1 && std::memcmp(voice_id, kSilentVoiceId, length) == 0) hr = S_OK;
    }
    trace_speech("XSpeechSynthesizerSetCustomVoice", hr, _ReturnAddress());
    return hr;
}

// The shared body of 13 and 21: the synthesizer handle, a text/SSML string and the stream out pointer. With the silent
// voice the stream is handed out; the text's length is logged once (the content is never read into a log).
HRESULT create_silent_stream(void* synthesizer, const char* text, void** stream, const char* what) {
    if (!stream) {
        trace_speech(what, E_POINTER, _ReturnAddress());
        return E_POINTER;
    }
    const bool writable = writable_range(stream, sizeof *stream);
    if (writable) *stream = nullptr;
    if (!is_silent_synthesizer(synthesizer) || !text || !readable_byte(text)) {
        trace_speech(what, E_INVALIDARG, _ReturnAddress());
        return E_INVALIDARG;
    }
    if (!writable) {
        trace_speech(what, E_POINTER, _ReturnAddress());
        return E_POINTER;
    }
    *stream = &g_silent_stream;
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true)) {
        const size_t length = readable_string_length(text, 4096);
        std::fprintf(stderr, "[rt] xgameruntime: silent speech: a %zu-byte text stream, %zu bytes of WAVE\n", length,
                     sizeof kSilentWave);
    }
    trace_speech(what, S_OK, _ReturnAddress());
    return S_OK;
}

// 13: HRESULT XSpeechSynthesizerCreateStreamFromText(synthesizer, const char* text, stream*)
HRESULT STDMETHODCALLTYPE speech_create_stream_from_text(void*, void* synthesizer, const char* text, void** stream) {
    return create_silent_stream(synthesizer, text, stream, "XSpeechSynthesizerCreateStreamFromText");
}

// 21: HRESULT XSpeechSynthesizerCreateStreamFromSsml(synthesizer, const char* ssml, stream*)
HRESULT STDMETHODCALLTYPE speech_create_stream_from_ssml(void*, void* synthesizer, const char* ssml, void** stream) {
    return create_silent_stream(synthesizer, ssml, stream, "XSpeechSynthesizerCreateStreamFromSsml");
}

// 14: HRESULT XSpeechSynthesizerCloseStreamHandle(XSpeechSynthesizerStreamHandle)
HRESULT STDMETHODCALLTYPE speech_close_stream(void*, void* stream) {
    const HRESULT hr = is_silent_stream(stream) ? S_OK : E_INVALIDARG;
    trace_speech("XSpeechSynthesizerCloseStreamHandle", hr, _ReturnAddress());
    return hr;
}

// 15: HRESULT XSpeechSynthesizerGetStreamDataSize(XSpeechSynthesizerStreamHandle, size_t* size)
HRESULT STDMETHODCALLTYPE speech_stream_size(void*, void* stream, size_t* size) {
    const bool writable = size && writable_range(size, sizeof *size);
    HRESULT hr = E_INVALIDARG;
    if (is_silent_stream(stream)) {
        if (writable) {
            *size = sizeof kSilentWave;
            hr = S_OK;
        } else {
            hr = E_POINTER;
        }
    } else if (writable) {
        *size = 0;
    }
    trace_speech("XSpeechSynthesizerGetStreamDataSize", hr, _ReturnAddress());
    return hr;
}

// 16: HRESULT XSpeechSynthesizerGetStreamData(XSpeechSynthesizerStreamHandle, size_t bufferSize, void* buffer,
//                                              size_t* bufferUsed). The published error for an invalid stream is
// HRESULT_FROM_WIN32(ERROR_NOT_FOUND); the offline answer keeps this file's E_INVALIDARG convention instead.
HRESULT STDMETHODCALLTYPE speech_stream_data(void*, void* stream, size_t buffer_size, void* buffer,
                                             size_t* buffer_used) {
    const bool used_writable = buffer_used && writable_range(buffer_used, sizeof *buffer_used);
    if (used_writable) *buffer_used = 0;
    HRESULT hr = E_INVALIDARG;
    if (is_silent_stream(stream)) {
        if (!buffer || !writable_range(buffer, sizeof kSilentWave)) {
            hr = E_POINTER;
        } else if (buffer_size < sizeof kSilentWave) {
            hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        } else {
            std::memcpy(buffer, kSilentWave, sizeof kSilentWave);
            if (used_writable) *buffer_used = sizeof kSilentWave;
            hr = S_OK;
        }
    }
    trace_speech("XSpeechSynthesizerGetStreamData", hr, _ReturnAddress());
    return hr;
}

// The slots with no offline counterpart whose arguments the layout doesn't give: the caption properties (3) and
// speech-to-text display (6, 7, 17-20). Nothing is written.
template <int Slot> HRESULT STDMETHODCALLTYPE accessibility_offline(void* self) {
    return offline_failure<kAccessibilityName, Slot>(self);
}

HRESULT STDMETHODCALLTYPE accessibility_query_interface(void*, REFIID iid, void** out);
const void* const kAccessibilityVtable[] = {
    FN(accessibility_query_interface), FN(object_add_ref), FN(object_release),   // 0-2 IUnknown
    FN(accessibility_offline<3>),            // 3  XClosedCaptionGetProperties
    FN(accessibility_captions_set_enabled),  // 4  XClosedCaptionSetEnabled
    FN(accessibility_high_contrast),         // 5  XHighContrastGetMode
    FN(accessibility_offline<6>),            // 6  XSpeechToTextSetPositionHint
    FN(accessibility_offline<7>),            // 7  XSpeechToTextSendString
    FN(speech_enumerate_voices),             // 8  XSpeechSynthesizerEnumerateInstalledVoices
    FN(speech_create),                       // 9  XSpeechSynthesizerCreate
    FN(speech_close_synth),                  // 10 XSpeechSynthesizerCloseHandle
    FN(speech_set_default_voice),            // 11 XSpeechSynthesizerSetDefaultVoice
    FN(speech_set_custom_voice),             // 12 XSpeechSynthesizerSetCustomVoice
    FN(speech_create_stream_from_text),      // 13 XSpeechSynthesizerCreateStreamFromText
    FN(speech_close_stream),                 // 14 XSpeechSynthesizerCloseStreamHandle
    FN(speech_stream_size),                  // 15 XSpeechSynthesizerGetStreamDataSize
    FN(speech_stream_data),                  // 16 XSpeechSynthesizerGetStreamData
    FN(accessibility_offline<17>),           // 17 XSpeechToTextBeginHypothesisString
    FN(accessibility_offline<18>),           // 18 XSpeechToTextUpdateHypothesisString
    FN(accessibility_offline<19>),           // 19 XSpeechToTextFinalizeHypothesisString
    FN(accessibility_offline<20>),           // 20 XSpeechToTextCancelHypothesisString
    FN(speech_create_stream_from_ssml),      // 21 XSpeechSynthesizerCreateStreamFromSsml
};
static_assert(std::size(kAccessibilityVtable) == 22, "IUnknown + IXAccessibilityImpl's 19 methods");
ClassObject g_accessibility_object{kAccessibilityVtable};
HRESULT STDMETHODCALLTYPE accessibility_query_interface(void*, REFIID iid, void** out) {
    return answer(&g_accessibility_object, std::begin(kAccessibilityInterfaces), std::end(kAccessibilityInterfaces),
                  iid, out);
}

// ======================= XLauncherImpl =======================

constexpr char kLauncherName[] = "XLauncherImpl";
constexpr GUID kLauncherClass = {0x1b339674, 0x328d, 0x4283, {0xa2, 0x00, 0x31, 0x71, 0xf1, 0x8d, 0x36, 0x39}};

// 3: HRESULT XLaunchUri(XUserHandle user, const char* uri): nothing is launched; E_FAIL.
// 4: HRESULT XDisplayAcquireTimeoutDeferral(XDisplayTimeoutDeferralHandle* handle)
// 5: XDisplayCloseTimeoutDeferralHandle(handle)
//
// The display timeout deferral is a console answer, not a UI the offline runtime lacks: the title asks the system to
// keep the display on while it is doing something the player is watching, and every measured peer that serves it maps
// it to a Windows power request (five-star's DisplayService, VIctory's and PercyScarlett's display modules). Doing the
// same is the console-faithful answer: one PowerCreateRequest with a display-required reason per deferral, set on
// acquire and cleared on close, the handle kept in this file's table so a close only clears a request this runtime
// handed out. Failure of the Windows call is returned as its own error (HRESULT_FROM_WIN32(GetLastError())) and the
// handle is null; a null handle argument is E_INVALIDARG; closing null or an unknown handle does nothing.
//
// The measured boot calls neither slot (the port's display probe arms show no call in the windows), so the switch is
// for a caller that wants the pre-slice refusal: PALMETTO_DISPLAY_DEFERRAL=0/off/false/no restores E_FAIL for both
// slots. Any other value enables the console answer; a value that is neither an off spelling nor on/1/true/yes is
// refused by name once and the console answer applies.
constinit SRWLOCK g_deferral_lock = SRWLOCK_INIT;
std::vector<HANDLE> g_deferrals;   // guarded by g_deferral_lock

bool display_deferral_switch_off() {
    char value[8] = {};
    if (!environment_string("PALMETTO_DISPLAY_DEFERRAL", value, sizeof value)) return false;
    const bool off = trimmed_equals_ignore_case(value, "0") || trimmed_equals_ignore_case(value, "off") ||
                     trimmed_equals_ignore_case(value, "false") || trimmed_equals_ignore_case(value, "no");
    if (off) return true;
    static std::atomic<bool> s_refused_logged{false};
    const bool on = trimmed_equals_ignore_case(value, "1") || trimmed_equals_ignore_case(value, "on") ||
                    trimmed_equals_ignore_case(value, "true") || trimmed_equals_ignore_case(value, "yes");
    if (!on && !s_refused_logged.exchange(true))
        std::fprintf(stderr,
                     "[rt] xgameruntime: PALMETTO_DISPLAY_DEFERRAL value \"%s\" is not 0/off/false/no or "
                     "1/on/true/yes; the console answer (deferral served) applies\n",
                     value);
    return false;
}

HRESULT STDMETHODCALLTYPE launcher_acquire_deferral(void*, void** handle) {
    if (handle == nullptr) return E_INVALIDARG;
    *handle = nullptr;
    if (display_deferral_switch_off()) return offline_failure<kLauncherName, 4>(nullptr);
    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString = const_cast<LPWSTR>(L"XDisplayAcquireTimeoutDeferral");
    HANDLE request = PowerCreateRequest(&reason);
    if (request == INVALID_HANDLE_VALUE) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        std::fprintf(stderr, "[rt] xgameruntime: XDisplayAcquireTimeoutDeferral: PowerCreateRequest failed (0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        return hr;
    }
    if (!PowerSetRequest(request, PowerRequestDisplayRequired)) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(request);
        std::fprintf(stderr, "[rt] xgameruntime: XDisplayAcquireTimeoutDeferral: PowerSetRequest failed (0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        return hr;
    }
    AcquireSRWLockExclusive(&g_deferral_lock);
    try {
        g_deferrals.push_back(request);
    } catch (...) {   // out of memory: the request is not handed out
        ReleaseSRWLockExclusive(&g_deferral_lock);
        PowerClearRequest(request, PowerRequestDisplayRequired);
        CloseHandle(request);
        return E_OUTOFMEMORY;
    }
    ReleaseSRWLockExclusive(&g_deferral_lock);
    *handle = request;
    return S_OK;
}

void STDMETHODCALLTYPE launcher_close_deferral(void*, void* handle) {
    if (handle == nullptr) return;
    bool owned = false;
    AcquireSRWLockExclusive(&g_deferral_lock);
    for (auto it = g_deferrals.begin(); it != g_deferrals.end(); ++it) {
        if (*it == static_cast<HANDLE>(handle)) {
            g_deferrals.erase(it);
            owned = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_deferral_lock);
    if (!owned) return;   // not a request this runtime handed out: nothing to clear, nothing to close
    PowerClearRequest(static_cast<HANDLE>(handle), PowerRequestDisplayRequired);
    CloseHandle(static_cast<HANDLE>(handle));
}

HRESULT STDMETHODCALLTYPE launcher_query_interface(void*, REFIID iid, void** out);
const void* const kLauncherVtable[] = {
    FN(launcher_query_interface), FN(object_add_ref), FN(object_release),   // 0-2 IUnknown
    FN((offline_failure<kLauncherName, 3>)),   // 3 XLaunchUri
    FN(launcher_acquire_deferral),             // 4 XDisplayAcquireTimeoutDeferral
    FN(launcher_close_deferral),               // 5 XDisplayCloseTimeoutDeferralHandle
};
static_assert(std::size(kLauncherVtable) == 6, "IUnknown + IXLauncherImpl's 3 methods");
ClassObject g_launcher_object{kLauncherVtable};
HRESULT STDMETHODCALLTYPE launcher_query_interface(void*, REFIID iid, void** out) {
    // IXLauncherImpl's id is the class id.
    return answer(&g_launcher_object, &kLauncherClass, &kLauncherClass + 1, iid, out);
}

#undef FN

}  // namespace

namespace rt::shims {

// Called by xgameruntime.cpp's QueryApiImpl for every class: true when the class is one of this file's, with *hr set.
bool xgameruntime_offline_query(const GUID& class_id, REFIID interface_id, void** out, HRESULT* hr) {
    if (class_id == kEventClass) {
        *hr = event_query_interface(nullptr, interface_id, out);
        return true;
    }
    if (class_id == kUiClass) {
        *hr = ui_query_interface(nullptr, interface_id, out);
        return true;
    }
    if (class_id == kStreamingClass) {
        *hr = streaming_query_interface(nullptr, interface_id, out);
        return true;
    }
    if (class_id == kDisplayClass) {
        *hr = display_query_interface(nullptr, interface_id, out);
        return true;
    }
    if (class_id == kUserDeviceClass) {
        *hr = device_query_interface(nullptr, interface_id, out);
        return true;
    }
    if (class_id == kAccessibilityClass) {
        *hr = accessibility_query_interface(nullptr, interface_id, out);
        return true;
    }
    if (class_id == kLauncherClass) {
        *hr = launcher_query_interface(nullptr, interface_id, out);
        return true;
    }
    return false;
}

// For tests: how many game events were written.
long xgameruntime_offline_game_events() { return g_events.load(); }

// For tests: how many XUserDeviceImpl registrations of one kind (0 device association, 1 default audio endpoint)
// are kept.
long xgameruntime_offline_device_registrations(uint32_t kind) {
    long n = 0;
    AcquireSRWLockShared(&g_device_lock);
    for (const auto& registration : g_device_registrations)
        if (static_cast<uint32_t>(registration.kind) == kind) ++n;
    ReleaseSRWLockShared(&g_device_lock);
    return n;
}

// ---- The display answer's test hooks (docs/peer_port_first_ui.md section 14's tests) --------------------------------
//
// The HDR answer depends on the host desktop, which a unit test must not read: the test sets the host state it wants
// here. These are process-local setters used only by the test target; the runtime never calls them.

// How many XDisplayTryEnableHdrMode calls the shim answered, and how many of them were Enabled. The port's arm reads
// the same two counters through its log lines; the test uses these.
long xgameruntime_offline_hdr_calls() { return g_hdr_calls.load(); }
long xgameruntime_offline_hdr_enabled_answers() { return g_hdr_enabled_answers.load(); }

// The switch parser's decision as the test sees it: 0 auto, 1 forced on, 2 forced off, 3 refused by value.
int xgameruntime_offline_parse_hdr_switch(const char* value) { return static_cast<int>(parse_hdr_switch(value)); }

// Sets the host display the answer rule sees: mode -1 probes the host, 0 is an available SDR output, 1 is an available
// HDR10 output with the given luminance range, 2 is "no output answered".
void xgameruntime_offline_set_host_hdr_for_tests(int mode, float min_luminance, float max_luminance,
                                                 float max_full_frame_luminance) {
    g_test_host_min.store(min_luminance, std::memory_order_relaxed);
    g_test_host_max.store(max_luminance, std::memory_order_relaxed);
    g_test_host_full.store(max_full_frame_luminance, std::memory_order_relaxed);
    g_test_host_mode.store(mode < 0 ? -1 : (mode > 2 ? 2 : mode), std::memory_order_relaxed);
}

void xgameruntime_offline_clear_host_hdr_for_tests() { g_test_host_mode.store(-1, std::memory_order_relaxed); }

long xgameruntime_offline_host_hdr_probes() { return g_host_hdr_probes.load(); }

// How many display timeout deferrals are open (a request this runtime created and has not closed yet).
long xgameruntime_offline_display_deferrals_open() {
    long n = 0;
    AcquireSRWLockShared(&g_deferral_lock);
    n = static_cast<long>(g_deferrals.size());
    ReleaseSRWLockShared(&g_deferral_lock);
    return n;
}

}  // namespace rt::shims
