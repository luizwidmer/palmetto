// XUserImpl through QueryApiImpl, called the way xgameruntime.lib's XUser* thunks call it: QueryApiImpl with the
// class id (or a later interface id), one vtable slot with `this` first. Slots follow xgameruntime-docs' layout;
// arguments and expected values follow the public GDK reference. No game data; nothing online.
//
//   xgameruntime_user_test                          the offline user, its handles, gamertag "Player"
//   xgameruntime_user_test --expect-gamertag TAG    the same, run with PALMETTO_GAMERTAG=TAG
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include "file_system.h"
#include "persistent_store.h"
#include "shims.h"

namespace rt::shims {
long xgameruntime_user_open_handles();
bool xgameruntime_set_package_interface4(bool on);
bool xgameruntime_set_package_ready(bool on);
bool xgameruntime_set_user_change_event(bool on);
void xgameruntime_user_forget_profile();
}

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

using XUserHandle = void*;
struct XUserLocalId {
    uint64_t value;
};

constexpr HRESULT E_GAMEUSER_USER_NOT_FOUND = static_cast<HRESULT>(0x89245104);
constexpr HRESULT E_GAMEUSER_DEFERRAL_NOT_AVAILABLE = static_cast<HRESULT>(0x89245103);
constexpr HRESULT E_GAMEUSER_FAILED_TO_GET_TOKEN = static_cast<HRESULT>(0x89245111);

constexpr GUID kUserClass = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
constexpr GUID kUserImpl3 = {0x1bf2f8c5, 0xd507, 0x4e52, {0xbb, 0x05, 0xf7, 0x26, 0xd0, 0xe7, 0x11, 0x61}};
constexpr GUID kUserImpl6 = {0x5131d685, 0x4394, 0x4ee6, {0x8c, 0x18, 0xbf, 0xb5, 0xd4, 0xae, 0xf1, 0xff}};
constexpr GUID kGamertag = {0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};
constexpr GUID kOther = {0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};  // XGameImpl

using Query = HRESULT(WINAPI*)(const GUID*, REFIID, void**);
using Fn = void*;
Fn slot(void* obj, int index) { return (*static_cast<Fn* const*>(obj))[index]; }
template <class F> F at(void* obj, int index) { return reinterpret_cast<F>(slot(obj, index)); }

// Waits for an XAsyncBlock on the process queue, as a title polling XAsyncGetStatus(wait = true) does.
bool finished(XAsyncBlock* async) { return XAsyncGetStatus(async, true) != E_PENDING; }

// XPackageInstallationProgressCallback's shape (GDK reference: void CALLBACK(void* context, monitor)).
void CALLBACK count_progress_changed(void* context, void* /*monitor*/) {
    static_cast<std::atomic<int>*>(context)->fetch_add(1);
}

// The change-event probe's callback (GDK: void CALLBACK(void* context, XUserLocalId, XUserChangeEvent)); the
// registration's context is the atomic count, and the other fields are recorded for the shape checks.
std::atomic<int> g_change_events{0};
std::atomic<uint64_t> g_change_local_id{0};
std::atomic<uint32_t> g_change_event_value{0xFFFFFFFF};
std::atomic<void*> g_change_context{nullptr};
void CALLBACK count_change_event(void* context, XUserLocalId user, uint32_t event) {
    g_change_events.fetch_add(1);
    g_change_local_id.store(user.value);
    g_change_event_value.store(event);
    g_change_context.store(context);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* expect_tag = "Player";
    if (argc == 3 && std::strcmp(argv[1], "--expect-gamertag") == 0) expect_tag = argv[2];
    std::printf("xgameruntime_user_test: XUserImpl through QueryApiImpl (gamertag \"%s\")\n", expect_tag);

    auto query = reinterpret_cast<Query>(rt::shims::find("xgameruntime.dll", "QueryApiImpl"));
    check(query != nullptr, "QueryApiImpl is registered");
    if (!query) return 1;

    // ---- the object and its interfaces ----
    void* user = nullptr;
    check(query(&kUserClass, kUserClass, &user) == S_OK && user, "QueryApiImpl hands out XUserImpl for its class id");
    if (!user) return 1;
    void* user6 = nullptr;
    void* user3 = nullptr;
    check(query(&kUserClass, kUserImpl6, &user6) == S_OK && user6 == user &&
              query(&kUserClass, kUserImpl3, &user3) == S_OK && user3 == user,
          "IXUserImpl3 and IXUserImpl6 (later versions of the same vtable) answer with the same object");
    void* tag_face = nullptr;
    check(query(&kUserClass, kGamertag, &tag_face) == S_OK && tag_face && tag_face != user,
          "IXUserGamertagImpl is a second interface of the object");
    void* none = reinterpret_cast<void*>(1);
    check(query(&kUserClass, kOther, &none) == E_NOINTERFACE && none == nullptr,
          "an interface the class doesn't have is E_NOINTERFACE with a cleared out pointer");

    // ---- XUserAddAsync / XUserAddResult (slots 7, 8) ----
    using AddAsync = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, XAsyncBlock*);
    using AddResult = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, XUserHandle*);
    XAsyncBlock add{};
    check(at<AddAsync>(user, 7)(user, 0x01, &add) == S_OK, "XUserAddAsync(AddDefaultUserSilently) starts");
    check(finished(&add) && XAsyncGetStatus(&add, false) == S_OK, "it completes with S_OK");
    XUserHandle h = nullptr;
    check(at<AddResult>(user, 8)(user, &add, &h) == S_OK && h != nullptr, "XUserAddResult hands out a user handle");
    XAsyncBlock bad{};
    check(at<AddAsync>(user, 7)(user, 0x01 | 0x02, &bad) == E_INVALIDARG,
          "AllowGuests with AddDefaultUserSilently is refused (a guest can't be the default user)");
    XAsyncBlock picker{};
    XUserHandle h_picker = nullptr;
    check(at<AddAsync>(user, 7)(user, 0x00, &picker) == S_OK && finished(&picker) &&
              at<AddResult>(user, 8)(user, &picker, &h_picker) == S_OK && h_picker,
          "XUserAddAsync without a silent option also adds the offline user (no account picker offline)");

    // ---- identity (slots 5, 6, 9-14, 19, 20) ----
    using Compare = int32_t(STDMETHODCALLTYPE*)(void*, XUserHandle, XUserHandle);
    using MaxUsers = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t*);
    using GetLocalId = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, XUserLocalId*);
    using FindLocal = HRESULT(STDMETHODCALLTYPE*)(void*, XUserLocalId, XUserHandle*);
    using GetId = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, uint64_t*);
    using FindId = HRESULT(STDMETHODCALLTYPE*)(void*, uint64_t, XUserHandle*);
    using GetBool = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, bool*);
    using GetU32 = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, uint32_t*);
    using CheckPrivilege = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, uint32_t, uint32_t, bool*, uint32_t*);
    uint32_t max_users = 0;
    check(at<MaxUsers>(user, 6)(user, &max_users) == S_OK && max_users == 1, "XUserGetMaxUsers: one local user");
    check(at<Compare>(user, 5)(user, h, h_picker) == 0, "XUserCompare: both handles name the same user");
    XUserLocalId local{};
    check(at<GetLocalId>(user, 9)(user, h, &local) == S_OK && local.value != 0, "XUserGetLocalId: a non-zero local id");
    uint64_t xuid = 0;
    check(at<GetId>(user, 11)(user, h, &xuid) == S_OK && xuid != 0, "XUserGetId: a non-zero user id");
    XUserHandle by_local = nullptr, by_id = nullptr, missing = reinterpret_cast<XUserHandle>(1);
    check(at<FindLocal>(user, 10)(user, local, &by_local) == S_OK && by_local &&
              at<Compare>(user, 5)(user, by_local, h) == 0,
          "XUserFindUserByLocalId finds it");
    check(at<FindLocal>(user, 10)(user, XUserLocalId{local.value + 1}, &missing) == E_GAMEUSER_USER_NOT_FOUND &&
              missing == nullptr,
          "another local id is E_GAMEUSER_USER_NOT_FOUND");
    check(at<FindId>(user, 12)(user, xuid, &by_id) == S_OK && by_id, "XUserFindUserById finds it");
    missing = reinterpret_cast<XUserHandle>(1);
    check(at<FindId>(user, 12)(user, xuid + 1, &missing) == E_GAMEUSER_USER_NOT_FOUND && missing == nullptr,
          "another user id is E_GAMEUSER_USER_NOT_FOUND");
    bool guest = true;
    check(at<GetBool>(user, 13)(user, h, &guest) == S_OK && !guest, "XUserGetIsGuest: not a guest");
    uint32_t state = 99, age = 99;
    check(at<GetU32>(user, 14)(user, h, &state) == S_OK && state == 0, "XUserGetState: SignedIn (0)");
    check(at<GetU32>(user, 19)(user, h, &age) == S_OK && age == 3, "XUserGetAgeGroup: Adult (3)");
    bool has = false;
    uint32_t reason = 99;
    check(at<CheckPrivilege>(user, 20)(user, h, 0, 254, &has, &reason) == S_OK && has && reason == 0,
          "XUserCheckPrivilege: granted, deny reason None (0)");
    check(at<GetBool>(user, 13)(user, nullptr, &guest) == E_INVALIDARG, "a null handle is E_INVALIDARG");

    // ---- XUserResolvePrivilegeWithUiAsync / Result (21, 22) ----
    using ResolveAsync = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, uint32_t, uint32_t, XAsyncBlock*);
    using ResolveResult = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*);
    XAsyncBlock resolve{};
    check(at<ResolveAsync>(user, 21)(user, h, 0, 254, &resolve) == S_OK && finished(&resolve) &&
              at<ResolveResult>(user, 22)(user, &resolve) == S_OK,
          "XUserResolvePrivilegeWithUiAsync completes with S_OK (nothing to resolve)");

    // ---- tokens: not available offline (23) ----
    using TokenAsync = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, uint32_t, const char*, const char*, size_t,
                                                   const void*, size_t, const void*, XAsyncBlock*);
    XAsyncBlock token{};
    check(at<TokenAsync>(user, 23)(user, h, 0, "GET", "https://example.invalid/", 0, nullptr, 0, nullptr, &token) ==
              E_GAMEUSER_FAILED_TO_GET_TOKEN,
          "XUserGetTokenAndSignatureAsync fails before starting: E_GAMEUSER_FAILED_TO_GET_TOKEN");

    // ---- change events, sign-out deferral (33-36) ----
    using Register = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, void*, void(CALLBACK*)(void*, XUserLocalId, uint32_t),
                                                 XTaskQueueRegistrationToken*);
    using Unregister = bool(STDMETHODCALLTYPE*)(void*, XTaskQueueRegistrationToken, bool);
    using Deferral = HRESULT(STDMETHODCALLTYPE*)(void*, void**);
    auto on_change = [](void*, XUserLocalId, uint32_t) {};
    XTaskQueueRegistrationToken reg{};
    check(at<Register>(user, 33)(user, nullptr, nullptr, on_change, &reg) == S_OK && reg.token != 0,
          "XUserRegisterForChangeEvent hands out a token");
    check(at<Unregister>(user, 34)(user, reg, false) && at<Unregister>(user, 34)(user, reg, false) &&
              at<Unregister>(user, 34)(user, XTaskQueueRegistrationToken{999}, false),
          "XUserUnregisterForChangeEvent: true, also with wait = false for a token already gone or never issued "
          "(false would mean still pending, which never happens offline)");
    void* deferral = reinterpret_cast<void*>(1);
    check(at<Deferral>(user, 35)(user, &deferral) == E_GAMEUSER_DEFERRAL_NOT_AVAILABLE && deferral == nullptr,
          "XUserGetSignOutDeferral: E_GAMEUSER_DEFERRAL_NOT_AVAILABLE (nothing signs out offline)");

    // ---- the opt-in change-event probe (docs/user_events.md) ----
    // Off, the registration above delivered nothing: the user never changes offline. The probe delivers one
    // SignedInAgain event per registration on the queue the title registered with, with its context and the offline
    // local id; the switch is the seam so both faces are checked in one process.
    check(g_change_events.load() == 0, "the registration made with the probe off delivered no change event");
    XTaskQueueHandle queue = nullptr;
    if (XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &queue) == S_OK &&
        queue) {
        int context = 7;
        check(!rt::shims::xgameruntime_set_user_change_event(true), "the change-event seam reports the switch was off");
        XTaskQueueRegistrationToken probed{};
        check(at<Register>(user, 33)(user, queue, &context, count_change_event, &probed) == S_OK && probed.token != 0,
              "a registration with the probe on hands out a token");
        for (int i = 0; i < 400 && g_change_events.load() == 0; ++i) Sleep(10);
        check(g_change_events.load() == 1 && g_change_local_id.load() == 1 && g_change_event_value.load() == 0 &&
                  g_change_context.load() == &context,
              "the registered callback ran once with its own context, the offline local id (1) and SignedInAgain (0)");
        check(at<Unregister>(user, 34)(user, probed, false), "unregistering a delivered registration is not pending");

        // A registration dropped before its queue delivers it must not reach the callback: a manual queue only
        // delivers when dispatched.
        XTaskQueueHandle manual = nullptr;
        if (XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &manual) == S_OK && manual) {
            XTaskQueueRegistrationToken dropped{};
            check(at<Register>(user, 33)(user, manual, &context, count_change_event, &dropped) == S_OK &&
                      dropped.token != 0 && at<Unregister>(user, 34)(user, dropped, false),
                  "unregistering a delivery that has not run yet is not pending");
            while (XTaskQueueDispatch(manual, XTaskQueuePort::Completion, 0)) {
            }
            check(g_change_events.load() == 1, "the unregistered delivery does not reach the callback");

            // The probe is bounded per process: a title that keeps re-registering cannot stream events forever.
            for (int i = 0; i < 20; ++i) {
                XTaskQueueRegistrationToken many{};
                check(at<Register>(user, 33)(user, manual, &context, count_change_event, &many) == S_OK,
                      "a registration past the probe's cap still succeeds");
                at<Unregister>(user, 34)(user, many, false);
            }
            while (XTaskQueueDispatch(manual, XTaskQueuePort::Completion, 0)) {
            }
            const int delivered = g_change_events.load();
            check(delivered >= 1 && delivered <= 8, "the probe delivers at most 8 events in one process");
            XTaskQueueRegistrationToken capped{};
            at<Register>(user, 33)(user, manual, &context, count_change_event, &capped);
            while (XTaskQueueDispatch(manual, XTaskQueuePort::Completion, 0)) {
            }
            check(g_change_events.load() == delivered, "a registration past the cap schedules no more events");
            at<Unregister>(user, 34)(user, capped, false);
            // A manual queue has no thread pumping it, so terminate without waiting and dispatch the termination
            // callbacks; waiting on the queue itself would wait for a callback only dispatch could run.
            XTaskQueueTerminate(manual, false, nullptr, nullptr);
            while (XTaskQueueDispatch(manual, XTaskQueuePort::Work, 0)) {
            }
            while (XTaskQueueDispatch(manual, XTaskQueuePort::Completion, 0)) {
            }
            XTaskQueueCloseHandle(manual);
        } else {
            check(false, "a manual task queue for the change-event probe could not be created");
        }

        check(rt::shims::xgameruntime_set_user_change_event(false), "the change-event seam reports the switch was on");
        XTaskQueueTerminate(queue, true, nullptr, nullptr);
        XTaskQueueCloseHandle(queue);
    } else {
        check(false, "a task queue for the change-event probe could not be created");
    }

    // ---- IXUserImpl2: XUserAddByIdWithUiAsync / Result (37, 38) ----
    using AddByIdAsync = HRESULT(STDMETHODCALLTYPE*)(void*, uint64_t, XAsyncBlock*);
    XAsyncBlock by_id_add{}, by_other{};
    XUserHandle h_by_id = nullptr, h_other = reinterpret_cast<XUserHandle>(1);
    check(at<AddByIdAsync>(user, 37)(user, xuid, &by_id_add) == S_OK && finished(&by_id_add) &&
              at<AddResult>(user, 38)(user, &by_id_add, &h_by_id) == S_OK && h_by_id,
          "XUserAddByIdWithUiAsync with the local user's id adds it");
    check(at<AddByIdAsync>(user, 37)(user, xuid + 1, &by_other) == S_OK && finished(&by_other) &&
              at<AddResult>(user, 38)(user, &by_other, &h_other) == E_GAMEUSER_USER_NOT_FOUND && h_other == nullptr,
          "with another id it completes with E_GAMEUSER_USER_NOT_FOUND");

    // ---- IXUserImpl4: XUserIsStoreUser (42) ----
    using IsStore = bool(STDMETHODCALLTYPE*)(void*, XUserHandle);
    check(at<IsStore>(user, 42)(user, h) && at<IsStore>(user, 42)(user, nullptr),
          "XUserIsStoreUser: true, also for a null handle (_In_opt_; always true on console)");
    using Unknown = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, size_t, char*, size_t*);
    char probe[16] = "untouched";
    size_t probe_used = 42;
    check(at<Unknown>(user, 15)(user, h, sizeof(probe), probe, &probe_used) == E_NOTIMPL,
          "slot 15 (no public signature) answers E_NOTIMPL rather than S_OK with nothing written");

    // ---- IXUserGamertagImpl: XUserGetGamertag (3) ----
    using GetTag = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, uint32_t, size_t, char*, size_t*);
    auto tag = at<GetTag>(tag_face, 3);
    char buf[101] = {};
    size_t used = 0;
    const size_t want = std::strlen(expect_tag) + 1;
    check(tag(tag_face, h, 0, sizeof(buf), buf, &used) == S_OK && std::strcmp(buf, expect_tag) == 0 && used == want,
          "XUserGetGamertag(Classic): the gamertag, gamertagUsed counting the terminator");
    check(tag(tag_face, h, 3, sizeof(buf), buf, &used) == S_OK && std::strcmp(buf, expect_tag) == 0,
          "UniqueModern of a classic gamertag: the same text, no '#' and no suffix");
    check(tag(tag_face, h, 1, sizeof(buf), buf, &used) == S_OK && std::strcmp(buf, expect_tag) == 0,
          "Modern: the classic gamertag when none is set");
    check(tag(tag_face, h, 2, sizeof(buf), buf, &used) == S_OK && buf[0] == 0 && used == 1,
          "ModernSuffix: empty (just the terminator)");
    check(tag(tag_face, h, 0, want - 1, buf, &used) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
          "a buffer one byte short is E_INSUFFICIENT_BUFFER");
    check(tag(tag_face, h, 7, sizeof(buf), buf, &used) == E_INVALIDARG, "an unknown component is E_INVALIDARG");

    // ---- handles: duplicate, close, all accounted for (3, 4) ----
    using Dup = HRESULT(STDMETHODCALLTYPE*)(void*, XUserHandle, XUserHandle*);
    using Close = void(STDMETHODCALLTYPE*)(void*, XUserHandle);
    XUserHandle dup = nullptr;
    check(at<Dup>(user, 3)(user, h, &dup) == S_OK && dup && dup != h && at<Compare>(user, 5)(user, dup, h) == 0,
          "XUserDuplicateHandle: a new handle for the same user");
    check(rt::shims::xgameruntime_user_open_handles() == 6, "six handles are open: two adds, two finds, an add by id and a duplicate");
    for (XUserHandle x : {h, h_picker, by_local, by_id, h_by_id, dup}) at<Close>(user, 4)(user, x);
    check(rt::shims::xgameruntime_user_open_handles() == 0, "XUserCloseHandle closes each of them");
    at<Close>(user, 4)(user, h);   // a double close: the handle is looked up, not read, so this is a no-op
    check(rt::shims::xgameruntime_user_open_handles() == 0 && at<GetBool>(user, 13)(user, h, &guest) == E_INVALIDARG,
          "a closed handle is refused and closing it again does nothing");

    // ---- XPackageImpl: the loading schedule's readiness calls and the two opt-in switches (docs/loading_gate.md) ----
    // The retail executable asks QueryApiImpl for XPackageImpl with IXPackageImpl4's id, which defaults to
    // E_NOINTERFACE; PALMETTO_PACKAGE_V4 answers it. The two package-readiness slots it then reaches
    // (XPackageCreateInstallationMonitor, IXPackageImpl3's XPackageEnumeratePackages) default to E_NOTIMPL;
    // PALMETTO_PACKAGE_READY answers them the console way. This test drives both faces through the same seams.
    constexpr GUID kPackageClass = {0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
    constexpr GUID kPackage3 = {0xe2a4734b, 0x2f4a, 0x456d, {0xaa, 0x8f, 0xd0, 0x65, 0xe0, 0x4f, 0xb2, 0x09}};
    constexpr GUID kPackage4 = {0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};
    using CreateMonitor = HRESULT(STDMETHODCALLTYPE*)(void*, const char*, uint32_t, const void*, uint32_t,
                                                      XTaskQueueHandle, void**);
    using EnumeratePackages = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, uint32_t, void*, void*);

    void* package = nullptr;
    check(query(&kPackageClass, kPackage4, &package) == E_NOINTERFACE && !package,
          "IXPackageImpl4 is E_NOINTERFACE by default (byte-identical default answer)");
    check(!rt::shims::xgameruntime_set_package_interface4(true), "the v4 seam reports the switch was off");
    check(query(&kPackageClass, kPackage4, &package) == S_OK && package,
          "with the v4 switch on, IXPackageImpl4 gets the package object");
    void* package3 = nullptr;
    check(query(&kPackageClass, kPackage3, &package3) == S_OK && package3 == package,
          "the same vtable still answers IXPackageImpl3");
    if (package) {
        void* monitor = reinterpret_cast<void*>(1);
        check(at<CreateMonitor>(package, 5)(package, "offline-package", 1, nullptr, 1000, nullptr, &monitor) ==
                      E_NOTIMPL &&
                  monitor == reinterpret_cast<void*>(1),
              "XPackageCreateInstallationMonitor is E_NOTIMPL and writes nothing by default");
        check(at<EnumeratePackages>(package, 39)(package, 0, 0, nullptr, nullptr) == E_NOTIMPL,
              "XPackageEnumeratePackages (IXPackageImpl3) is E_NOTIMPL by default");
        using RegisterDefault = HRESULT(STDMETHODCALLTYPE*)(void*, void*, void*, void*, XTaskQueueRegistrationToken*);
        check(at<RegisterDefault>(package, 9)(package, nullptr, nullptr, nullptr, nullptr) == E_NOTIMPL,
              "XPackageRegisterInstallationProgressChanged is E_NOTIMPL by default");

        check(!rt::shims::xgameruntime_set_package_ready(true), "the ready seam reports the switch was off");
        void* created = nullptr;
        check(at<CreateMonitor>(package, 5)(package, "offline-package", 1, nullptr, 1000, nullptr, &created) == S_OK &&
                  created != nullptr,
              "with the ready switch on, the installation monitor is created for the installed package");
        check(at<CreateMonitor>(package, 5)(package, "offline-package", 1, nullptr, 1000, nullptr, nullptr) ==
                  E_INVALIDARG,
              "a null monitor pointer is refused rather than written");
        // An output pointer into a read-only page must be refused, not written: the slot proves the range first.
        void* read_only_page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
        check(read_only_page != nullptr &&
                  at<CreateMonitor>(package, 5)(package, "offline-package", 1, nullptr, 1000, nullptr,
                                                reinterpret_cast<void**>(read_only_page)) == E_INVALIDARG,
              "a read-only monitor pointer is refused rather than written");
        if (read_only_page) VirtualFree(read_only_page, 0, MEM_RELEASE);
        check(at<EnumeratePackages>(package, 39)(package, 0, 0, nullptr, nullptr) == S_OK,
              "with the ready switch on, enumerating additional packages succeeds with none installed");

        // Slot 9 and slot 6: the progress-changed registration is delivered once through the queue the monitor was
        // created with, and the monitor closes.
        XTaskQueueHandle queue = nullptr;
        if (XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &queue) == S_OK &&
            queue) {
            void* monitored = nullptr;
            check(at<CreateMonitor>(package, 5)(package, "offline-package", 1, nullptr, 0, queue, &monitored) == S_OK &&
                      monitored,
                  "the installation monitor is created with the title's queue and cadence");
            std::atomic<int> notifications{0};
            XTaskQueueRegistrationToken progress_token{};
            using RegisterProgress = HRESULT(STDMETHODCALLTYPE*)(void*, void*, void*, void*, XTaskQueueRegistrationToken*);
            check(at<RegisterProgress>(package, 9)(package, monitored, &notifications,
                                           reinterpret_cast<void*>(&count_progress_changed), &progress_token) == S_OK &&
                      progress_token.token != 0,
                  "XPackageRegisterInstallationProgressChanged succeeds with a token on the monitor's queue");
            for (int i = 0; i < 200 && notifications.load() == 0; ++i) Sleep(10);
            check(notifications.load() == 1,
                  "the registered callback is invoked once with the installed package's complete snapshot");
            using CloseMonitor = HRESULT(STDMETHODCALLTYPE*)(void*, void*);
            check(at<CloseMonitor>(package, 6)(package, monitored) == S_OK,
                  "XPackageCloseInstallationMonitorHandle succeeds");
            XTaskQueueTerminate(queue, true, nullptr, nullptr);
            XTaskQueueCloseHandle(queue);
        } else {
            check(false, "a task queue for the progress-changed test could not be created");
        }

        rt::shims::xgameruntime_set_package_ready(false);
        rt::shims::xgameruntime_set_package_interface4(false);
    }

    // ---- the persisted offline user (persistent_store.h, docs/persistent_store.md) ----
    // With a persistent-storage folder the identity the user surface serves comes from the profile record: created
    // from the defaults on the first boot, re-served on the next one. The record itself is checked in the
    // xgameruntime test; here only what the user slots answer is.
    {
        const std::filesystem::path store_root = std::filesystem::temp_directory_path() /
                                                 (L"palmetto_user_store_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code store_ec;
        std::filesystem::remove_all(store_root, store_ec);
        rt::fs::configure({}, store_root / L"overlay");
        rt::shims::xgameruntime_set_persistent_profile(true);
        rt::shims::xgameruntime_offline_profile_forget();
        rt::shims::xgameruntime_user_forget_profile();

        XAsyncBlock add2{};
        XUserHandle h2 = nullptr;
        check(at<AddAsync>(user, 7)(user, 0x01, &add2) == S_OK && finished(&add2) &&
                  at<AddResult>(user, 8)(user, &add2, &h2) == S_OK && h2,
              "the offline user still adds with the store configured");
        uint64_t first_xuid = 0;
        XUserLocalId first_local{};
        char first_tag[16] = {};
        size_t first_tag_used = 0;
        check(at<GetId>(user, 11)(user, h2, &first_xuid) == S_OK && first_xuid == 0x0009000000000001ull &&
                  at<GetLocalId>(user, 9)(user, h2, &first_local) == S_OK && first_local.value == 1 &&
                  tag(tag_face, h2, 0, sizeof(first_tag), first_tag, &first_tag_used) == S_OK &&
                  std::strcmp(first_tag, expect_tag) == 0,
              "the first boot serves the runtime's default identity and records it");
        rt::shims::OfflineProfile record{};
        check(rt::shims::xgameruntime_offline_profile(0, 0, nullptr, &record) && record.boot_count == 1 &&
                  record.user_id == first_xuid && record.local_id == first_local.value,
              "the record holds exactly the identity the user surface served");

        rt::shims::xgameruntime_offline_profile_forget();
        rt::shims::xgameruntime_user_forget_profile();
        uint64_t second_xuid = 0;
        XUserLocalId second_local{};
        char second_tag[16] = {};
        size_t second_tag_used = 0;
        check(at<GetId>(user, 11)(user, h2, &second_xuid) == S_OK && second_xuid == first_xuid &&
                  at<GetLocalId>(user, 9)(user, h2, &second_local) == S_OK &&
                  second_local.value == first_local.value &&
                  tag(tag_face, h2, 0, sizeof(second_tag), second_tag, &second_tag_used) == S_OK &&
                  std::strcmp(second_tag, expect_tag) == 0,
              "the next boot serves the same identity from the record");
        rt::shims::OfflineProfile reloaded{};
        check(rt::shims::xgameruntime_offline_profile(0, 0, nullptr, &reloaded) && reloaded.from_store &&
                  reloaded.boot_count == 2,
              "and the record advanced once for that boot");
        at<Close>(user, 4)(user, h2);
        rt::fs::configure({}, {});
        std::filesystem::remove_all(store_root, store_ec);
    }

    std::printf("xgameruntime_user_test: %d of %d checks failed\n", g_failures, g_checks);
    return g_failures ? 1 : 0;
}
