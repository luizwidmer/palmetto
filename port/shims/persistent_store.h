// The offline user's persistent profile: the one signed-in user's identity kept on disk, so every boot after the
// first serves the same user from the record instead of from a constant (docs/persistent_store.md).
//
// The record lives in the persistent-local-storage folder the title can ask for
// (`<overlay>\.palmetto-persistent\settings.bin`) and is written atomically: the new bytes go to
// `settings.bin~`, are flushed, and MoveFileEx replaces `settings.bin`, so a kill at any step leaves either the
// previous record or the new one, never a truncated one. A record that fails its magic/version/size/checksum is
// treated as absent - the process starts a fresh one instead of trusting it - and a `settings.bin~` left by a killed
// write is removed, not read.
//
// The identity is this runtime's own: user id 0x0009000000000001 and local id 1 unless a previous boot's record says
// otherwise. The store is on by default (it is the runtime's own state, not an answer the title sees change);
// PALMETTO_PERSIST_PROFILE=0 switches it off and the constexpr defaults are served, with no file read or written.
// Bounded: the record is read once per process, and one line names the create or the load.
//
// This header only declares the seam; the store is implemented in xgameruntime.cpp next to the persistent-storage
// class whose folder it uses, so the two files do not duplicate the overlay path handling.
#pragma once

#include <cstdint>

namespace rt::shims {

struct OfflineProfile {
    uint64_t user_id;
    uint64_t local_id;
    char gamertag[16];  // printable ASCII, NUL-terminated, at most 15 characters
    uint32_t boot_count;
    bool from_store;    // a valid record from an earlier boot was loaded (as opposed to one just created)
};

// Loads the record, or creates it from the defaults on the first boot, and serves it back. The defaults are this
// runtime's identity constants (the user shim passes its own); they are used only when there is no record yet.
// Bounded: the first successful call does the I/O, later calls return the loaded copy. Returns false when the store
// is switched off or there is no overlay to put it in; *out is untouched then and the caller serves the constants.
bool xgameruntime_offline_profile(uint64_t default_user_id, uint64_t default_local_id, const char* default_gamertag,
                                  OfflineProfile* out);

// Test seams. Forget makes the next call model another process boot (re-read and advance the record). The switch
// setter forces the store on or off and returns the previous state, like the other xgameruntime seams.
void xgameruntime_offline_profile_forget();
bool xgameruntime_set_persistent_profile(bool on);

}  // namespace rt::shims
