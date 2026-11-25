// xgameruntime: XUserImpl, the class xgameruntime.lib's XUser* functions reach through QueryApiImpl, offline.
//
// A GDK title links XUserAddAsync, XUserGetId, XUserGetGamertag and the rest statically (xgameruntime.lib); each of
// those thunks asks QueryApiImpl for XUserImpl and calls one slot with the function's own arguments after `this`
// (docs/runtime_imports.md, "Run-time loaded modules"; xgameruntime.cpp serves the other classes the same way).
// The class and interface ids and the slot order are xodus-gaming/xgameruntime-docs' "Layout" lists (GPL-3.0; facts
// only, nothing copied). Signatures, enum values and error codes follow Microsoft's public GDK reference
// (learn.microsoft.com/gaming/gdk, XUser and Error Codes pages). No GDK header or library code is used.
//
// Offline there is one local user, always signed in: the default user XUserAddAsync returns (with or without
// AddDefaultUserSilently), with a fixed Xbox user id and local id, and a gamertag from PALMETTO_GAMERTAG (15 ASCII
// characters at most, "Player" otherwise). Nothing goes online: token and gamer-picture requests fail as they would
// without a network, and the user never changes, so registered change callbacks are never called by default. The opt-in
// probe PALMETTO_USER_CHANGE_EVENT=1 delivers one SignedInAgain event per registration instead, for the measured
// question this file's trace answers: does the title's registered callback at game+0x2211C80 change the boot when it
// runs? (docs/user_events.md; the default path is unchanged.)
//
// The executable links thunks for IXUserImpl slots 3-5, 7-14, 19-25 and 33-34 and IXUserGamertagImpl slot 3
// (tooling/gdk_runtime_calls.py, from the file alone; docs/runtime_imports.md). None of them is a private or PC-only
// slot; those still log once when called.
#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include "persistent_store.h"
#include "platform_trace.h"

namespace {

// ---- Public GDK types (XUser.h, as the GDK reference documents them) ----

using XUserHandle = struct XUserLocal*;              // opaque to the title
using XUserSignOutDeferralHandle = struct XUserSignOutDeferral*;
struct XUserLocalId {
    uint64_t value;
};
enum class XUserAddOptions : uint32_t { None = 0x00, AddDefaultUserSilently = 0x01, AllowGuests = 0x02, AddDefaultUserAllowingUI = 0x04 };
enum class XUserState : uint32_t { SignedIn = 0, SigningOut = 1, SignedOut = 2 };
enum class XUserAgeGroup : uint32_t { Unknown = 0, Child = 1, Teen = 2, Adult = 3 };
enum class XUserGamertagComponent : uint32_t { Classic = 0, Modern = 1, ModernSuffix = 2, UniqueModern = 3 };
enum class XUserPrivilegeDenyReason : uint32_t { None = 0 };
// XUser.h's enum, from the GDK "XUserChangeEvent" page. The title's registered callback switches on 0, 2 and 5
// (game+0x2211C80 -> game+0x2212190); the probe delivers 0 (docs/user_events.md).
enum class XUserChangeEvent : uint32_t { SignedInAgain = 0, SigningOut = 1, SignedOut = 2, Gamertag = 3, GamerPicture = 4, Privileges = 5 };
using XUserChangeEventCallback = void(CALLBACK*)(void* context, XUserLocalId user, XUserChangeEvent event);

// XGameErr.h values, from the GDK "Error codes" page.
constexpr HRESULT E_GAMEUSER_USER_NOT_FOUND = static_cast<HRESULT>(0x89245104);
constexpr HRESULT E_GAMEUSER_DEFERRAL_NOT_AVAILABLE = static_cast<HRESULT>(0x89245103);
constexpr HRESULT E_GAMEUSER_FAILED_TO_RESOLVE = static_cast<HRESULT>(0x89245107);
constexpr HRESULT E_GAMEUSER_FAILED_TO_GET_TOKEN = static_cast<HRESULT>(0x89245111);
constexpr HRESULT kInsufficientBuffer = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

// ---- The offline user ----

// Fixed, non-zero ids. The Xbox user id has the shape of a real XUID (a 64-bit number); nothing checks it offline.
constexpr uint64_t kOfflineXuid = 0x0009000000000001ull;
constexpr uint64_t kOfflineLocalId = 1;

const char* gamertag() {
    static char tag[16] = "Player";
    static std::once_flag once;
    std::call_once(once, [] {
        size_t length = 0;   // with the terminator; 0 when unset
        if (getenv_s(&length, nullptr, 0, "PALMETTO_GAMERTAG") != 0 || length <= 1) return;
        if (length - 1 > 15) {
            std::fprintf(stderr, "[rt] xgameruntime: PALMETTO_GAMERTAG is longer than 15 characters; using \"Player\"\n");
            return;
        }
        char value[16] = {};
        if (getenv_s(&length, value, "PALMETTO_GAMERTAG") != 0) return;
        for (size_t i = 0; i + 1 < length; ++i)
            if (static_cast<unsigned char>(value[i]) < 0x20 || static_cast<unsigned char>(value[i]) > 0x7e) {
                std::fprintf(stderr, "[rt] xgameruntime: PALMETTO_GAMERTAG is not printable ASCII; using \"Player\"\n");
                return;
            }
        std::memcpy(tag, value, length);
    });
    return tag;
}

// ---- The persistent profile (port/shims/persistent_store.h, docs/persistent_store.md) ----
//
// The identity this file serves is the record the runtime keeps under the persistent-local-storage folder, when
// there is one: the record is created on the first boot and served back on every later one, so the same offline
// user id, local id and gamertag survive a kill. Without a record (no overlay, or PALMETTO_PERSIST_PROFILE=0) the
// fixed constants below are served, which is the default path every measurement before this slice saw. Bounded: the
// record is resolved once per process; the store prints the one create/load line.
std::mutex g_identity_mutex;
std::unique_ptr<rt::shims::OfflineProfile> g_identity;

rt::shims::OfflineProfile offline_profile() {
    std::lock_guard<std::mutex> lock(g_identity_mutex);
    if (!g_identity) {
        rt::shims::OfflineProfile stored{};
        auto profile = std::make_unique<rt::shims::OfflineProfile>();
        if (rt::shims::xgameruntime_offline_profile(kOfflineXuid, kOfflineLocalId, gamertag(), &stored)) {
            *profile = stored;
        } else {
            profile->user_id = kOfflineXuid;
            profile->local_id = kOfflineLocalId;
            std::snprintf(profile->gamertag, sizeof profile->gamertag, "%s", gamertag());
            profile->boot_count = 0;
            profile->from_store = false;
        }
        g_identity = std::move(profile);
    }
    return *g_identity;
}

// A title must close every XUserHandle it gets exactly once, so each one is its own allocation; they all name the
// one local user. A handle is valid while it is in the live set: it's looked up, never read, so a closed or foreign
// pointer is refused without touching its memory. The block is freed on close, so the heap may hand its address out
// again; a stale handle then names a newer live one, which is harmless here since every handle names the same user.
struct XUserLocal {
    uint32_t unused;
};
std::mutex g_handles_mutex;
std::unordered_set<const void*> g_handles;

XUserHandle new_handle() {
    auto* h = new XUserLocal{};
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    g_handles.insert(h);
    return h;
}
bool valid(XUserHandle h) {
    if (!h) return false;
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    return g_handles.count(h) != 0;
}

// ---- IXUserImpl (slots 3-36) ----

// 3: HRESULT XUserDuplicateHandle(XUserHandle handle, XUserHandle* duplicatedHandle)
HRESULT STDMETHODCALLTYPE user_duplicate(void*, XUserHandle handle, XUserHandle* dup) {
    if (!dup) return E_POINTER;
    *dup = nullptr;
    if (!valid(handle)) return E_INVALIDARG;
    *dup = new_handle();
    return S_OK;
}

// 4: void XUserCloseHandle(XUserHandle user)
void STDMETHODCALLTYPE user_close(void*, XUserHandle user) {
    {
        std::lock_guard<std::mutex> lock(g_handles_mutex);
        if (!user || g_handles.erase(user) == 0) return;   // closed already, or not ours
    }
    delete user;
}

// 5: int32_t XUserCompare(XUserHandle user1, XUserHandle user2): 0 when both name the same user. Every valid handle
// names the one local user.
int32_t STDMETHODCALLTYPE user_compare(void*, XUserHandle a, XUserHandle b) {
    const bool va = valid(a), vb = valid(b);
    if (va && vb) return 0;
    if (va != vb) return va ? 1 : -1;
    return a == b ? 0 : (a < b ? -1 : 1);
}

// 6: HRESULT XUserGetMaxUsers(uint32_t* maxUsers). One local user offline.
HRESULT STDMETHODCALLTYPE user_get_max_users(void*, uint32_t* max_users) {
    if (!max_users) return E_POINTER;
    *max_users = 1;
    return S_OK;
}

// XAsync providers. The result of an add is a new handle, created when the title collects it, so a result the
// title never collects leaks nothing.
const char kAddIdentity = 0;
const char kAddByIdIdentity = 0;
const char kResolveIdentity = 0;

HRESULT CALLBACK add_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin:
        return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork:
        XAsyncComplete(data->async, S_OK, sizeof(XUserHandle));
        return S_OK;
    case XAsyncOp::GetResult:
        if (data->bufferSize < sizeof(XUserHandle) || !data->buffer) return E_INVALIDARG;
        *static_cast<XUserHandle*>(data->buffer) = new_handle();
        return S_OK;
    default:
        return S_OK;
    }
}

// The add-by-id flavour: the context is the requested id; an id other than the local user's completes with
// E_GAMEUSER_USER_NOT_FOUND (no other user can sign in offline).
HRESULT CALLBACK add_by_id_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin:
        return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork: {
        const bool known = reinterpret_cast<uintptr_t>(data->context) == static_cast<uintptr_t>(kOfflineXuid);
        XAsyncComplete(data->async, known ? S_OK : E_GAMEUSER_USER_NOT_FOUND, known ? sizeof(XUserHandle) : 0);
        return S_OK;
    }
    case XAsyncOp::GetResult:
        if (data->bufferSize < sizeof(XUserHandle) || !data->buffer) return E_INVALIDARG;
        *static_cast<XUserHandle*>(data->buffer) = new_handle();
        return S_OK;
    default:
        return S_OK;
    }
}

// Resolving a privilege: every privilege is granted offline, so there is nothing to show; completes with S_OK.
HRESULT CALLBACK resolve_provider(XAsyncOp op, const XAsyncProviderData* data) {
    if (op == XAsyncOp::Begin) return XAsyncSchedule(data->async, 0);
    if (op == XAsyncOp::DoWork) XAsyncComplete(data->async, S_OK, 0);
    return S_OK;
}

// 7: HRESULT XUserAddAsync(XUserAddOptions options, XAsyncBlock* async). AllowGuests with AddDefaultUserSilently is
// refused, as documented ("A guest cannot be the default user"). Without a silent option the console would show the
// account picker; offline the one user is the answer either way.
HRESULT STDMETHODCALLTYPE user_add_async(void*, XUserAddOptions options, XAsyncBlock* async) {
    if (!async) return E_POINTER;
    const auto bits = static_cast<uint32_t>(options);
    if ((bits & static_cast<uint32_t>(XUserAddOptions::AddDefaultUserSilently)) &&
        (bits & static_cast<uint32_t>(XUserAddOptions::AllowGuests)))
        return E_INVALIDARG;
    return XAsyncBegin(async, nullptr, &kAddIdentity, "XUserAddAsync", add_provider);
}

// 8: HRESULT XUserAddResult(XAsyncBlock* async, XUserHandle* newUser)
HRESULT STDMETHODCALLTYPE user_add_result(void*, XAsyncBlock* async, XUserHandle* user) {
    if (!user) return E_POINTER;
    *user = nullptr;
    return XAsyncGetResult(async, &kAddIdentity, sizeof(*user), user, nullptr);
}

// 9: HRESULT XUserGetLocalId(XUserHandle user, XUserLocalId* userLocalId)
HRESULT STDMETHODCALLTYPE user_get_local_id(void*, XUserHandle user, XUserLocalId* id) {
    if (!id) return E_POINTER;
    if (!valid(user)) return E_INVALIDARG;
    id->value = offline_profile().local_id;
    return S_OK;
}

// 10: HRESULT XUserFindUserByLocalId(XUserLocalId userLocalId, XUserHandle* handle)
HRESULT STDMETHODCALLTYPE user_find_by_local_id(void*, XUserLocalId id, XUserHandle* handle) {
    if (!handle) return E_POINTER;
    *handle = nullptr;
    if (id.value != offline_profile().local_id) return E_GAMEUSER_USER_NOT_FOUND;
    *handle = new_handle();
    return S_OK;
}

// 11: HRESULT XUserGetId(XUserHandle user, uint64_t* userId)
HRESULT STDMETHODCALLTYPE user_get_id(void*, XUserHandle user, uint64_t* id) {
    if (!id) return E_POINTER;
    if (!valid(user)) return E_INVALIDARG;
    *id = offline_profile().user_id;
    return S_OK;
}

// 12: HRESULT XUserFindUserById(uint64_t userId, XUserHandle* handle)
HRESULT STDMETHODCALLTYPE user_find_by_id(void*, uint64_t id, XUserHandle* handle) {
    if (!handle) return E_POINTER;
    *handle = nullptr;
    if (id != offline_profile().user_id) return E_GAMEUSER_USER_NOT_FOUND;
    *handle = new_handle();
    return S_OK;
}

// 13: HRESULT XUserGetIsGuest(XUserHandle user, bool* isGuest)
HRESULT STDMETHODCALLTYPE user_get_is_guest(void*, XUserHandle user, bool* guest) {
    if (!guest) return E_POINTER;
    if (!valid(user)) return E_INVALIDARG;
    *guest = false;
    return S_OK;
}

// 14: HRESULT XUserGetState(XUserHandle user, XUserState* state)
HRESULT STDMETHODCALLTYPE user_get_state(void*, XUserHandle user, XUserState* state) {
    if (!state) return E_POINTER;
    if (!valid(user)) return E_INVALIDARG;
    *state = XUserState::SignedIn;
    return S_OK;
}

// A slot with no public signature, logged once. Slot 15 (the docs' "unknown") answers E_NOTIMPL, so a caller fails
// instead of reading outputs nothing wrote. Slot 47, IXUserImpl6's undocumented XUserIsSignOutPresent, is read as a
// bool, where E_NOTIMPL's low byte (1) would mean true, so it answers 0 (false: nothing signs out offline).
template <int Slot> HRESULT STDMETHODCALLTYPE unknown_slot(void*) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        std::fprintf(stderr, "[rt] xgameruntime: XUserImpl slot %d has no public signature; answering E_NOTIMPL\n", Slot);
    return E_NOTIMPL;
}
template <int Slot> uint64_t STDMETHODCALLTYPE private_slot(void*) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        std::fprintf(stderr, "[rt] xgameruntime: XUserImpl private slot %d called; answering 0\n", Slot);
    return 0;
}

// An operation that needs the network or a system UI the offline runtime doesn't have. The *Async function fails
// before starting, so the XAsyncBlock is never used and no callback runs; its Result/ResultSize partners return the
// same error. Their arguments don't matter, so the functions take none (x64: the caller owns the argument area).
template <int Slot, HRESULT Error> HRESULT STDMETHODCALLTYPE offline_failure(void*) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        std::fprintf(stderr, "[rt] xgameruntime: XUserImpl slot %d is not available offline (HRESULT 0x%08lx)\n", Slot,
                     static_cast<unsigned long>(Error));
    return Error;
}

// 19: HRESULT XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* ageGroup)
HRESULT STDMETHODCALLTYPE user_get_age_group(void*, XUserHandle user, XUserAgeGroup* group) {
    if (!group) return E_POINTER;
    if (!valid(user)) return E_INVALIDARG;
    *group = XUserAgeGroup::Adult;
    return S_OK;
}

// 20: HRESULT XUserCheckPrivilege(XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege,
//                                 bool* hasPrivilege, XUserPrivilegeDenyReason* reason). Everything is allowed offline
// (multiplayer and the like then fail at the network, as on a console without one).
HRESULT STDMETHODCALLTYPE user_check_privilege(void*, XUserHandle user, uint32_t, uint32_t, bool* has,
                                               XUserPrivilegeDenyReason* reason) {
    if (!has) return E_POINTER;
    if (!valid(user)) return E_INVALIDARG;
    *has = true;
    if (reason) *reason = XUserPrivilegeDenyReason::None;
    return S_OK;
}

// 21: HRESULT XUserResolvePrivilegeWithUiAsync(XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege,
//                                              XAsyncBlock* async)
HRESULT STDMETHODCALLTYPE user_resolve_privilege_async(void*, XUserHandle user, uint32_t, uint32_t, XAsyncBlock* async) {
    if (!async) return E_POINTER;
    if (!valid(user)) return E_INVALIDARG;
    return XAsyncBegin(async, nullptr, &kResolveIdentity, "XUserResolvePrivilegeWithUiAsync", resolve_provider);
}

// 22: HRESULT XUserResolvePrivilegeWithUiResult(XAsyncBlock* async)
HRESULT STDMETHODCALLTYPE user_resolve_privilege_result(void*, XAsyncBlock* async) {
    return XAsyncGetResult(async, &kResolveIdentity, 0, nullptr, nullptr);
}

// 33: HRESULT XUserRegisterForChangeEvent(XTaskQueueHandle queue, void* context, XUserChangeEventCallback* callback,
//                                         XTaskQueueRegistrationToken* token)
// 34: bool XUserUnregisterForChangeEvent(XTaskQueueRegistrationToken token, bool wait)
//
// The user never changes offline, so the registered callback is never invoked by default: the registration is kept
// (with the callback and its context) and unregistering is never pending. PALMETTO_USER_CHANGE_EVENT=1 (OFF by
// default) delivers one XUserChangeEvent::SignedInAgain to each registration, on the same task queue the title
// registered with (the queue's completion port, as the networking registrations in xgameruntime.cpp deliver their
// documented initial notification), and invokes the title's callback once. It is the falsifier of the "a frontend
// waits for a user change event instead of polling" hypothesis, not a claim that the offline user changed: the
// measured registration is the only user call in the boot and nothing re-reads user state (docs/user_events.md).
// Delivering a transition that did not happen can change a title's state, so it is opt-in, one delivery per
// registration, and capped per process. The switch's seam for tests is xgameruntime_set_user_change_event.
std::atomic<int> g_change_event_override{-1};   // test seam: -1 follows the environment, 0/1 force OFF/ON
bool change_event_enabled() {
    const int forced = g_change_event_override.load();
    if (forced >= 0) return forced != 0;
    char value[8] = {};
    const DWORD n = GetEnvironmentVariableA("PALMETTO_USER_CHANGE_EVENT", value, sizeof value);
    return n > 0 && n < sizeof value && std::strcmp(value, "0") != 0 && _stricmp(value, "off") != 0;
}

struct ChangeRegistration {
    XUserChangeEventCallback callback;
    void* context;
    bool running;       // the callback is executing now
    bool unregistered;  // XUserUnregisterForChangeEvent named this token while the callback ran
};
std::mutex g_tokens_mutex;
std::condition_variable g_tokens_cv;
std::unordered_map<uint64_t, ChangeRegistration> g_tokens;
uint64_t g_next_token = 1;
std::atomic<int> g_change_deliveries{0};
constexpr int kMaxChangeEventDeliveries = 8;   // bounded: a title that re-registers cannot stream events

struct ChangeDelivery {
    uint64_t token;
};

// Runs on the title's queue's completion port. The delivery is dropped if the registration went away first, so an
// XUserUnregisterForChangeEvent(wait = false) that returned true is never followed by the callback.
void CALLBACK run_change_delivery(void* self, bool canceled) {
    std::unique_ptr<ChangeDelivery> delivery(static_cast<ChangeDelivery*>(self));
    ChangeRegistration registration;
    {
        std::lock_guard<std::mutex> lock(g_tokens_mutex);
        const auto it = g_tokens.find(delivery->token);
        if (canceled || it == g_tokens.end() || it->second.unregistered) return;
        it->second.running = true;
        registration = it->second;
    }
    std::fprintf(stderr, "[rt] xgameruntime: +%.2fs XUser change event SignedInAgain (0) delivered to the registered callback\n",
                 rt::shims::platform_trace_seconds());
    registration.callback(registration.context, XUserLocalId{offline_profile().local_id},
                          XUserChangeEvent::SignedInAgain);
    {
        std::lock_guard<std::mutex> lock(g_tokens_mutex);
        const auto it = g_tokens.find(delivery->token);
        if (it == g_tokens.end()) return;
        if (it->second.unregistered) g_tokens.erase(it);
        else it->second.running = false;
    }
    g_tokens_cv.notify_all();
}

HRESULT STDMETHODCALLTYPE user_register_change(void*, XTaskQueueHandle queue, void* context,
                                               XUserChangeEventCallback callback, XTaskQueueRegistrationToken* token) {
    if (!callback || !token) return E_POINTER;
    // The measured point where the title first touches the user: resolve the persistent profile here (creating the
    // record on the first boot), so this boot's identity is on disk for the next one even though the title never
    // asks XUserGetId or XUserGetGamertag (docs/persistent_store.md).
    offline_profile();
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(g_tokens_mutex);
        id = g_next_token++;
        g_tokens.emplace(id, ChangeRegistration{callback, context, false, false});
    }
    token->token = id;
    if (!change_event_enabled()) return S_OK;
    if (g_change_deliveries.fetch_add(1) >= kMaxChangeEventDeliveries) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true))
            std::fprintf(stderr, "[rt] xgameruntime: XUser change event cap (%d) reached; further probes are not scheduled\n",
                         kMaxChangeEventDeliveries);
        return S_OK;
    }
    XTaskQueueHandle process_queue = nullptr;
    if (!queue) {
        if (!XTaskQueueGetCurrentProcessTaskQueue(&process_queue)) {
            std::fprintf(stderr, "[rt] xgameruntime: XUser change event probe: no process task queue\n");
            return S_OK;
        }
        queue = process_queue;
    }
    auto* delivery = new ChangeDelivery{id};
    const HRESULT hr = XTaskQueueSubmitCallback(queue, XTaskQueuePort::Completion, delivery, run_change_delivery);
    if (process_queue) XTaskQueueCloseHandle(process_queue);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[rt] xgameruntime: XUser change event probe could not be scheduled (0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        delete delivery;
    }
    return S_OK;
}

// false means "wait was false and the unregistration is still pending": only a delivery that is already running can be
// pending. Nothing else is ever pending, so a token that was never issued (or was already removed) is true.
bool STDMETHODCALLTYPE user_unregister_change(void*, XTaskQueueRegistrationToken token, bool wait) {
    std::unique_lock<std::mutex> lock(g_tokens_mutex);
    const auto it = g_tokens.find(token.token);
    if (it == g_tokens.end()) return true;
    if (!it->second.running) {
        g_tokens.erase(it);
        return true;
    }
    it->second.unregistered = true;
    if (wait) g_tokens_cv.wait(lock, [&] { return g_tokens.find(token.token) == g_tokens.end(); });
    return wait;
}

// 35: HRESULT XUserGetSignOutDeferral(XUserSignOutDeferralHandle* deferral): nothing signs out offline, so there is
// no sign-out to defer (the documented E_GAMEUSER_DEFERRAL_NOT_AVAILABLE).
HRESULT STDMETHODCALLTYPE user_get_signout_deferral(void*, XUserSignOutDeferralHandle* deferral) {
    if (deferral) *deferral = nullptr;
    return E_GAMEUSER_DEFERRAL_NOT_AVAILABLE;
}

// 36: void XUserCloseSignOutDeferralHandle(XUserSignOutDeferralHandle deferral)
void STDMETHODCALLTYPE user_close_signout_deferral(void*, XUserSignOutDeferralHandle) {}

// ---- IXUserImpl2 (37-38) ----

// 37: HRESULT XUserAddByIdWithUiAsync(uint64_t userId, XAsyncBlock* async)
HRESULT STDMETHODCALLTYPE user_add_by_id_async(void*, uint64_t id, XAsyncBlock* async) {
    if (!async) return E_POINTER;
    return XAsyncBegin(async, reinterpret_cast<void*>(static_cast<uintptr_t>(id)), &kAddByIdIdentity,
                       "XUserAddByIdWithUiAsync", add_by_id_provider);
}

// 38: HRESULT XUserAddByIdWithUiResult(XAsyncBlock* async, XUserHandle* newUser)
HRESULT STDMETHODCALLTYPE user_add_by_id_result(void*, XAsyncBlock* async, XUserHandle* user) {
    if (!user) return E_POINTER;
    *user = nullptr;
    return XAsyncGetResult(async, &kAddByIdIdentity, sizeof(*user), user, nullptr);
}

// ---- IXUserImpl4 (42) ----

// 42: bool XUserIsStoreUser(XUserHandle user): the handle is _In_opt_, and the GDK reference says the function
// always returns true on console.
bool STDMETHODCALLTYPE user_is_store_user(void*, XUserHandle) { return true; }

// ---- IXUserGamertagImpl (its slot 3) ----

// HRESULT XUserGetGamertag(XUserHandle user, XUserGamertagComponent component, size_t gamertagSize, char* gamertag,
//                          size_t* gamertagUsed). The offline user has a classic gamertag: Modern and UniqueModern are
// the same text (a classic tag has no '#' and no suffix) and ModernSuffix is empty. gamertagUsed counts the
// terminator.
HRESULT STDMETHODCALLTYPE user_get_gamertag(void*, XUserHandle user, XUserGamertagComponent component, size_t size,
                                            char* out, size_t* used) {
    if (used) *used = 0;
    if (!valid(user)) return E_INVALIDARG;
    const char* text = nullptr;
    switch (component) {
    case XUserGamertagComponent::Classic:
    case XUserGamertagComponent::Modern:
    case XUserGamertagComponent::UniqueModern:
        text = offline_profile().gamertag;
        break;
    case XUserGamertagComponent::ModernSuffix:
        text = "";
        break;
    default:
        return E_INVALIDARG;
    }
    const size_t need = std::strlen(text) + 1;
    if (!out || size < need) return kInsufficientBuffer;
    std::memcpy(out, text, need);
    if (used) *used = need;
    return S_OK;
}

// ---- PALMETTO_USER_TRACE: per-slot trace ----
//
// The permanent vtable entries stay what they are; with PALMETTO_USER_TRACE=1 the vtable of the traced build's user
// object points at these one-line wrappers instead. Each wrapper captures the title's return address (the thunk for
// that function), calls the real slot, and hands its value to the bounded trace: first call and every tenth-power
// call, with the caller as module+RVA. This is the measurement of what the loading loop polls: which XUser* function
// it calls continuously and what the shim answers. Off by default, and it never changes an answer.
//
// The wrapped slots are exactly the IXUserImpl/IXUserGamertagImpl slots the retail executable links thunks for
// (tooling/gdk_runtime_calls.py: IXUserImpl 3-5, 7-14, 19-25, 33-34 and IXUserGamertagImpl 3).
HRESULT STDMETHODCALLTYPE trace_user_duplicate(void* self, XUserHandle handle, XUserHandle* dup) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_duplicate(self, handle, dup);
    rt::shims::platform_trace("XUserDuplicateHandle", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
void STDMETHODCALLTYPE trace_user_close(void* self, XUserHandle user) {
    void* const caller = _ReturnAddress();
    user_close(self, user);
    rt::shims::platform_trace("XUserCloseHandle", caller, 0);
}
int32_t STDMETHODCALLTYPE trace_user_compare(void* self, XUserHandle a, XUserHandle b) {
    void* const caller = _ReturnAddress();
    const int32_t result = user_compare(self, a, b);
    rt::shims::platform_trace("XUserCompare", caller, static_cast<std::uint64_t>(static_cast<int64_t>(result)));
    return result;
}
HRESULT STDMETHODCALLTYPE trace_user_add_async(void* self, XUserAddOptions options, XAsyncBlock* async) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_add_async(self, options, async);
    rt::shims::platform_trace("XUserAddAsync", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_add_result(void* self, XAsyncBlock* async, XUserHandle* user) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_add_result(self, async, user);
    rt::shims::platform_trace("XUserAddResult", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_get_local_id(void* self, XUserHandle user, XUserLocalId* id) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_get_local_id(self, user, id);
    rt::shims::platform_trace("XUserGetLocalId", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_find_by_local_id(void* self, XUserLocalId id, XUserHandle* handle) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_find_by_local_id(self, id, handle);
    rt::shims::platform_trace("XUserFindUserByLocalId", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_get_id(void* self, XUserHandle user, uint64_t* id) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_get_id(self, user, id);
    rt::shims::platform_trace("XUserGetId", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_find_by_id(void* self, uint64_t id, XUserHandle* handle) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_find_by_id(self, id, handle);
    rt::shims::platform_trace("XUserFindUserById", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_get_is_guest(void* self, XUserHandle user, bool* guest) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_get_is_guest(self, user, guest);
    rt::shims::platform_trace("XUserGetIsGuest", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_get_state(void* self, XUserHandle user, XUserState* state) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_get_state(self, user, state);
    rt::shims::platform_trace("XUserGetState", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_get_age_group(void* self, XUserHandle user, XUserAgeGroup* group) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_get_age_group(self, user, group);
    rt::shims::platform_trace("XUserGetAgeGroup", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_check_privilege(void* self, XUserHandle user, uint32_t options, uint32_t privilege,
                                                     bool* has, XUserPrivilegeDenyReason* reason) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_check_privilege(self, user, options, privilege, has, reason);
    rt::shims::platform_trace("XUserCheckPrivilege", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_resolve_privilege_async(void* self, XUserHandle user, uint32_t options,
                                                             uint32_t privilege, XAsyncBlock* async) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_resolve_privilege_async(self, user, options, privilege, async);
    rt::shims::platform_trace("XUserResolvePrivilegeWithUiAsync", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_resolve_privilege_result(void* self, XAsyncBlock* async) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_resolve_privilege_result(self, async);
    rt::shims::platform_trace("XUserResolvePrivilegeWithUiResult", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_get_token(void* self) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = offline_failure<23, E_GAMEUSER_FAILED_TO_GET_TOKEN>(self);
    rt::shims::platform_trace("XUserGetTokenAndSignatureAsync", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_get_token_size(void* self) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = offline_failure<24, E_GAMEUSER_FAILED_TO_GET_TOKEN>(self);
    rt::shims::platform_trace("XUserGetTokenAndSignatureResultSize", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_get_token_result(void* self) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = offline_failure<25, E_GAMEUSER_FAILED_TO_GET_TOKEN>(self);
    rt::shims::platform_trace("XUserGetTokenAndSignatureResult", caller, static_cast<std::uint64_t>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE trace_user_register_change(void* self, XTaskQueueHandle queue, void* context,
                                                     XUserChangeEventCallback callback,
                                                     XTaskQueueRegistrationToken* token) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_register_change(self, queue, context, callback, token);
    // The registration's shape is what the change-event probe needs: whether the title gave a queue and a context,
    // and which function it registered (as module+RVA).
    char what[192];
    std::snprintf(what, sizeof what, "XUserRegisterForChangeEvent(queue=%s context=%s callback=%s)",
                  queue ? "given" : "null", context ? "given" : "null",
                  callback ? rt::shims::module_offset(reinterpret_cast<std::uint64_t>(callback)).c_str() : "null");
    rt::shims::platform_trace_keyed(reinterpret_cast<std::uint64_t>(&trace_user_register_change), what, caller,
                                    static_cast<std::uint64_t>(hr));
    return hr;
}
bool STDMETHODCALLTYPE trace_user_unregister_change(void* self, XTaskQueueRegistrationToken token, bool wait) {
    void* const caller = _ReturnAddress();
    const bool done = user_unregister_change(self, token, wait);
    char what[96];
    std::snprintf(what, sizeof what, "XUserUnregisterForChangeEvent(token=%llu wait=%d)",
                  static_cast<unsigned long long>(token.token), wait ? 1 : 0);
    rt::shims::platform_trace_keyed(reinterpret_cast<std::uint64_t>(&trace_user_unregister_change), what, caller,
                                    done ? 1 : 0);
    return done;
}
HRESULT STDMETHODCALLTYPE trace_user_get_gamertag(void* self, XUserHandle user, XUserGamertagComponent component,
                                                  size_t size, char* out, size_t* used) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = user_get_gamertag(self, user, component, size, out, used);
    rt::shims::platform_trace("XUserGetGamertag", caller, static_cast<std::uint64_t>(hr));
    return hr;
}

// ---- The object: one vtable for the IXUserImpl..IXUserImpl6 chain, one for IXUserGamertagImpl ----

constexpr GUID kUserClass = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
// IXUserImpl's id is the class id; IXUserImpl2..6 extend it, so each of them answers with the same vtable.
constexpr GUID kUserInterfaces[] = {
    kUserClass,
    {0xeb9bf948, 0x18dc, 0x4d82, {0xbb, 0xcc, 0x40, 0xe0, 0xa8, 0x09, 0xc4, 0xc0}},   // IXUserImpl2
    {0x1bf2f8c5, 0xd507, 0x4e52, {0xbb, 0x05, 0xf7, 0x26, 0xd0, 0xe7, 0x11, 0x61}},   // IXUserImpl3
    {0x079415e3, 0x6727, 0x437f, {0x8e, 0x9d, 0x8f, 0x8f, 0x9b, 0x24, 0x39, 0xf7}},   // IXUserImpl4
    {0x26f3c674, 0xa2fe, 0x44fa, {0xb6, 0xc4, 0xa3, 0x23, 0xbc, 0x94, 0xff, 0x53}},   // IXUserImpl5
    {0x5131d685, 0x4394, 0x4ee6, {0x8c, 0x18, 0xbf, 0xb5, 0xd4, 0xae, 0xf1, 0xff}},   // IXUserImpl6
};
constexpr GUID kGamertagInterface = {0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};
constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct UserObject {
    const void* const* vtable;            // IXUserImpl6
    const void* const* gamertag_vtable;   // IXUserGamertagImpl
};
UserObject* user_object();

HRESULT query(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    UserObject* o = user_object();
    if (iid == kIUnknown) {
        *out = &o->vtable;
        return S_OK;
    }
    for (const GUID& id : kUserInterfaces)
        if (iid == id) {
            *out = &o->vtable;
            return S_OK;
        }
    if (iid == kGamertagInterface) {
        *out = &o->gamertag_vtable;
        return S_OK;
    }
    return E_NOINTERFACE;
}

// A process-wide singleton, as the other runtime classes: AddRef and Release don't free.
HRESULT STDMETHODCALLTYPE object_query_interface(void*, REFIID iid, void** out) { return query(iid, out); }
ULONG STDMETHODCALLTYPE object_add_ref(void*) { return 2; }
ULONG STDMETHODCALLTYPE object_release(void*) { return 1; }

#define FN(f) reinterpret_cast<const void*>(&f)

const void* const kUserVtable[] = {
    FN(object_query_interface), FN(object_add_ref), FN(object_release),          // 0-2  IUnknown
    FN(user_duplicate),                                                         // 3  XUserDuplicateHandle
    FN(user_close),                                                             // 4  XUserCloseHandle
    FN(user_compare),                                                           // 5  XUserCompare
    FN(user_get_max_users),                                                     // 6  XUserGetMaxUsers
    FN(user_add_async),                                                         // 7  XUserAddAsync
    FN(user_add_result),                                                        // 8  XUserAddResult
    FN(user_get_local_id),                                                      // 9  XUserGetLocalId
    FN(user_find_by_local_id),                                                  // 10 XUserFindUserByLocalId
    FN(user_get_id),                                                            // 11 XUserGetId
    FN(user_find_by_id),                                                        // 12 XUserFindUserById
    FN(user_get_is_guest),                                                      // 13 XUserGetIsGuest
    FN(user_get_state),                                                         // 14 XUserGetState
    FN(unknown_slot<15>),                                                       // 15 (unknown in the docs)
    FN((offline_failure<16, E_FAIL>)),                                          // 16 XUserGetGamerPictureAsync
    FN((offline_failure<17, E_FAIL>)),                                          // 17 XUserGetGamerPictureResultSize
    FN((offline_failure<18, E_FAIL>)),                                          // 18 XUserGetGamerPictureResult
    FN(user_get_age_group),                                                     // 19 XUserGetAgeGroup
    FN(user_check_privilege),                                                   // 20 XUserCheckPrivilege
    FN(user_resolve_privilege_async),                                           // 21 XUserResolvePrivilegeWithUiAsync
    FN(user_resolve_privilege_result),                                          // 22 XUserResolvePrivilegeWithUiResult
    FN((offline_failure<23, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 23 XUserGetTokenAndSignatureAsync
    FN((offline_failure<24, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 24 ...ResultSize
    FN((offline_failure<25, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 25 ...Result
    FN((offline_failure<26, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 26 XUserGetTokenAndSignatureUtf16Async
    FN((offline_failure<27, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 27 ...Utf16ResultSize
    FN((offline_failure<28, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 28 ...Utf16Result
    FN((offline_failure<29, E_GAMEUSER_FAILED_TO_RESOLVE>)),                    // 29 XUserResolveIssueWithUiAsync
    FN((offline_failure<30, E_GAMEUSER_FAILED_TO_RESOLVE>)),                    // 30 XUserResolveIssueWithUiResult
    FN((offline_failure<31, E_GAMEUSER_FAILED_TO_RESOLVE>)),                    // 31 XUserResolveIssueWithUiUtf16Async
    FN((offline_failure<32, E_GAMEUSER_FAILED_TO_RESOLVE>)),                    // 32 XUserResolveIssueWithUiUtf16Result
    FN(user_register_change),                                                   // 33 XUserRegisterForChangeEvent
    FN(user_unregister_change),                                                 // 34 XUserUnregisterForChangeEvent
    FN(user_get_signout_deferral),                                              // 35 XUserGetSignOutDeferral
    FN(user_close_signout_deferral),                                            // 36 XUserCloseSignOutDeferralHandle
    FN(user_add_by_id_async),                                                   // 37 XUserAddByIdWithUiAsync (2)
    FN(user_add_by_id_result),                                                  // 38 XUserAddByIdWithUiResult (2)
    FN((offline_failure<39, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 39 XUserGetMsaTokenSilentlyAsync (3)
    FN((offline_failure<40, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 40 ...Result (3)
    FN((offline_failure<41, E_GAMEUSER_FAILED_TO_GET_TOKEN>)),                  // 41 ...ResultSize (3)
    FN(user_is_store_user),                                                     // 42 XUserIsStoreUser (4)
    FN((offline_failure<43, E_NOTIMPL>)),                                       // 43 XUserPlatformRemoteConnectSetEventHandlers (5, PC)
    FN((offline_failure<44, E_NOTIMPL>)),                                       // 44 XUserPlatformRemoteConnectCancelPrompt (5, PC)
    FN((offline_failure<45, E_NOTIMPL>)),                                       // 45 XUserPlatformSpopPromptSetEventHandlers (5, PC)
    FN((offline_failure<46, E_NOTIMPL>)),                                       // 46 XUserPlatformSpopPromptComplete (5, PC)
    FN(private_slot<47>),                                                       // 47 XUserIsSignOutPresent (6): false
    FN((offline_failure<48, E_NOTIMPL>)),                                       // 48 XUserSignOutAsync (6)
    FN((offline_failure<49, E_NOTIMPL>)),                                       // 49 XUserSignOutResult (6)
};
static_assert(std::size(kUserVtable) == 50, "IUnknown + IXUserImpl's 34 methods + 2 + 3 + 1 + 4 + 3");

// The gamertag face's IUnknown slots get `this` = &gamertag_vtable; the object is a singleton, so they ignore it.
const void* const kGamertagVtable[] = {
    FN(object_query_interface), FN(object_add_ref), FN(object_release),
    FN(user_get_gamertag),                                                      // 3 XUserGetGamertag
};

#undef FN

// The traced face of the same vtables, for PALMETTO_USER_TRACE: a copy of the permanent vtable with only the slots the
// executable imports replaced by their wrapper (the wrappers above). The object picks one face once, when it is first
// asked for, so with the switch unset the object is the untouched vtable - the default path changes nothing at all.
const void* const* traced_user_vtable() {
    static const std::vector<const void*> table = [] {
        std::vector<const void*> t(std::begin(kUserVtable), std::end(kUserVtable));
        t[3] = reinterpret_cast<const void*>(&trace_user_duplicate);
        t[4] = reinterpret_cast<const void*>(&trace_user_close);
        t[5] = reinterpret_cast<const void*>(&trace_user_compare);
        t[7] = reinterpret_cast<const void*>(&trace_user_add_async);
        t[8] = reinterpret_cast<const void*>(&trace_user_add_result);
        t[9] = reinterpret_cast<const void*>(&trace_user_get_local_id);
        t[10] = reinterpret_cast<const void*>(&trace_user_find_by_local_id);
        t[11] = reinterpret_cast<const void*>(&trace_user_get_id);
        t[12] = reinterpret_cast<const void*>(&trace_user_find_by_id);
        t[13] = reinterpret_cast<const void*>(&trace_user_get_is_guest);
        t[14] = reinterpret_cast<const void*>(&trace_user_get_state);
        t[19] = reinterpret_cast<const void*>(&trace_user_get_age_group);
        t[20] = reinterpret_cast<const void*>(&trace_user_check_privilege);
        t[21] = reinterpret_cast<const void*>(&trace_user_resolve_privilege_async);
        t[22] = reinterpret_cast<const void*>(&trace_user_resolve_privilege_result);
        t[23] = reinterpret_cast<const void*>(&trace_user_get_token);
        t[24] = reinterpret_cast<const void*>(&trace_user_get_token_size);
        t[25] = reinterpret_cast<const void*>(&trace_user_get_token_result);
        t[33] = reinterpret_cast<const void*>(&trace_user_register_change);
        t[34] = reinterpret_cast<const void*>(&trace_user_unregister_change);
        return t;
    }();
    return table.data();
}
const void* const* traced_gamertag_vtable() {
    static const std::vector<const void*> table = [] {
        std::vector<const void*> t(std::begin(kGamertagVtable), std::end(kGamertagVtable));
        t[3] = reinterpret_cast<const void*>(&trace_user_get_gamertag);
        return t;
    }();
    return table.data();
}

UserObject* user_object() {
    static UserObject o = [] {
        UserObject object{kUserVtable, kGamertagVtable};
        if (rt::shims::platform_trace_enabled()) {
            object.vtable = traced_user_vtable();
            object.gamertag_vtable = traced_gamertag_vtable();
        }
        return object;
    }();
    return &o;
}

}  // namespace

namespace rt::shims {

// Called by xgameruntime.cpp's QueryApiImpl for every class: true when the class is XUserImpl, with *hr set.
bool xgameruntime_user_query(const GUID& class_id, REFIID interface_id, void** out, HRESULT* hr) {
    if (class_id != kUserClass) return false;
    *hr = query(interface_id, out);
    return true;
}

// A new handle to the offline user, for the classes that hand one out too (XUserFindForDevice, in
// xgameruntime_offline.cpp). The title closes it with XUserCloseHandle like any other.
void* xgameruntime_user_new_handle() { return new_handle(); }

// For tests: the XUserHandles a title holds open.
long xgameruntime_user_open_handles() {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    return static_cast<long>(g_handles.size());
}

// Test seam for PALMETTO_USER_CHANGE_EVENT (docs/user_events.md): forces the change-event probe OFF/ON and returns the
// previous state, so one test process can check both faces without an environment variable changing mid-run.
bool xgameruntime_set_user_change_event(bool on) {
    const bool previous = change_event_enabled();
    g_change_event_override.store(on ? 1 : 0);
    return previous;
}

// Test seam for the persistent identity (docs/persistent_store.md): drops the resolved profile so the next user call
// resolves it again, which models the same process starting as a later boot.
void xgameruntime_user_forget_profile() {
    std::lock_guard<std::mutex> lock(g_identity_mutex);
    g_identity.reset();
}

}  // namespace rt::shims
