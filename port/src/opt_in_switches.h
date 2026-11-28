// Generated audit data: the opt-in switch registry of port/src and port/shims (round 21, T14).
//
// Every row is one environment switch the runtime or a shim reads, with the parser idiom found at the
// recorded call site and whether that site is include-reachable from a compiled port translation unit.
// port/test/opt_in_switches_test.cpp re-derives the name set from the source and fails when this table and
// the source disagree; docs/opt_in_switches.md lists the same rows with the evidence.
//
// Kind is a classification of the recorded site, not a recommendation: the tree has four flag idioms
// (exact "1", first character not '0', text 0/off/no, any non-null value, plus default-on with "0" off)
// and numeric/string value switches. any_set is the one that treats an empty value as ON.
//
// Default behaviour: every flag kind is off when the variable is unset (flag_parse returns false), and a value
// switch falls back to the caller's legacy value. No row is consulted unless the caller passes it through the
// parser, so the default path is byte-identical.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define PALMETTO_STRICMP _stricmp
#else
#include <strings.h>
#define PALMETTO_STRICMP strcasecmp
#endif

namespace rt::opt_in_switches {

// Parser idioms observed in port/src and port/shims. The exact semantics are exercised by
// port/test/opt_in_switches_test.cpp for the values unset, empty, "0", "1" and invalid text.
enum class Kind : std::uint8_t {
    exact_one,     // on iff the value is exactly "1" ("true", "01", " 1" stay off)
    switch_on,     // on iff non-empty and the first character is not '0' ("01" and "0x" are off)
    text_switch,   // on unless empty, "0", "off" or "no" (case-insensitive)
    any_set,       // on iff the variable is present at all: an empty value counts as ON
    value_int,     // a decimal number with a caller-supplied legacy fallback; unset/invalid keeps it
    value_string,  // a non-empty string (path or spelling) with a caller-supplied legacy fallback
    disable_value, // documented DEFAULT-ON switch: on unless the value is "0", "off" or "no"
};

inline constexpr const char* kind_name(Kind kind) noexcept {
    switch (kind) {
    case Kind::exact_one: return "exact_one";
    case Kind::switch_on: return "switch_on";
    case Kind::text_switch: return "text_switch";
    case Kind::any_set: return "any_set";
    case Kind::value_int: return "value_int";
    case Kind::value_string: return "value_string";
    case Kind::disable_value: return "disable_value";
    }
    return "unknown";
}

// The reference parser for one kind: no host calls, no copy, case-insensitive only where the tree is.
inline bool flag_parse(Kind kind, const char* value) noexcept {
    switch (kind) {
    case Kind::exact_one:
        return value != nullptr && std::strcmp(value, "1") == 0;
    case Kind::switch_on:
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    case Kind::text_switch:
    case Kind::disable_value: {
        // text_switch is off for the three documented off spellings; disable_value is that test inverted,
        // so an unset variable keeps the documented default-on behaviour.
        const bool unset = value == nullptr;
        const bool off = !unset && (std::strcmp(value, "0") == 0 || PALMETTO_STRICMP(value, "off") == 0 ||
                                    PALMETTO_STRICMP(value, "no") == 0);
        if (kind == Kind::disable_value) return !off;
        return !unset && value[0] != '\0' && !off;
    }
    case Kind::any_set:
        return value != nullptr;
    case Kind::value_int:
    case Kind::value_string:
        // A value switch is consulted when a non-empty value is present; unset or empty leaves the caller's
        // legacy default in place (never an enabled flag).
        return value != nullptr && value[0] != '\0';
    }
    return false;
}

// Decimal reader for value_int switches: the caller's fallback on unset, empty or invalid text.
inline long long integer_or(const char* value, long long fallback) noexcept {
    if (value == nullptr || value[0] == '\0') return fallback;
    char* end = nullptr;
    const long long parsed = std::strtoll(value, &end, 10);
    if (end == value || *end != '\0') return fallback;
    return parsed;
}

struct Switch {
    const char* name;  // the exact environment variable name
    Kind kind;         // the parser idiom recorded at `site`
    const char* site;  // "<repo-relative file>:<line>" of the first read
    bool wired;        // that file is include-reachable from a port/src or port/shims .cpp
};

inline constexpr Switch kSwitches[] = {
    {"PALMETTO_ALLOC_TRACE", Kind::any_set, "port/shims/xmem.cpp:211", true},
    {"PALMETTO_ALLOC_WATCH", Kind::switch_on, "port/src/main.cpp:3275", true},
    {"PALMETTO_ALLOC_WATCH_FREE", Kind::switch_on, "port/src/main.cpp:3288", true},
    {"PALMETTO_ANALYTICS_FORM", Kind::any_set, "port/shims/xgameruntime.cpp:1382", true},
    {"PALMETTO_ARENA_LOG", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:9844", true},
    {"PALMETTO_ARENA_MB", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:9774", true},
    {"PALMETTO_AUDIO_DECODE", Kind::switch_on, "port/shims/audio_decode.h:148", true},
    {"PALMETTO_AUDIO_EFFECTS", Kind::switch_on, "port/shims/xapu_effects.h:15", true},
    {"PALMETTO_AUDIO_SUBMIT_PROBE", Kind::exact_one, "port/src/main.cpp:10434", true},
    {"PALMETTO_BACK_IMAGES", Kind::any_set, "port/src/main.cpp:10401", true},
    {"PALMETTO_BASE_RESERVED", Kind::value_int, "port/src/base_reservation.h:38", true},
    {"PALMETTO_BINDLESS_DEGRADATION", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:9622", true},
    {"PALMETTO_BINDLESS_IDLE_WAIT", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:9746", true},
    {"PALMETTO_BINK_FASTFORWARD", Kind::any_set, "port/src/main.cpp:9807", true},
    {"PALMETTO_BINK_PROBE", Kind::switch_on, "port/src/main.cpp:10592", true},
    {"PALMETTO_BINK_WITNESS", Kind::any_set, "port/src/main.cpp:9806", true},
    {"PALMETTO_BVH_ROUTE", Kind::value_string, "port/src/xdxr_route.h:177", true},
    {"PALMETTO_CAPTURE", Kind::value_string, "port/src/vulkan_present.h:192", true},
    {"PALMETTO_CAPTURE_EVERY", Kind::value_int, "port/src/vulkan_present.h:199", true},
    {"PALMETTO_CAPTURE_LIMIT", Kind::value_int, "port/src/vulkan_present.h:200", true},
    {"PALMETTO_CARD_COPY_GATE", Kind::switch_on, "port/src/vulkan_backend.h:1106", true},
    {"PALMETTO_CARD_COPY_GATE_REFRESH", Kind::switch_on, "port/src/vulkan_backend.h:1107", true},
    {"PALMETTO_CARD_COPY_SKIP_NO_SHADER", Kind::switch_on, "port/src/vulkan_backend.h:1104", true},
    {"PALMETTO_CARD_COPY_TRACE", Kind::switch_on, "port/src/vulkan_backend.h:1103", true},
    {"PALMETTO_CARD_COPY_WATCH", Kind::switch_on, "port/src/vulkan_backend.h:1105", true},
    {"PALMETTO_COLOUR_PATH", Kind::switch_on, "port/src/vulkan_replay.h:2014", true},
    {"PALMETTO_COMPAT_DIR", Kind::value_string, "port/src/pe_loader.cpp:1166", true},
    {"PALMETTO_CONSOLE_TYPE", Kind::any_set, "port/src/pe_loader.cpp:2292", true},
    {"PALMETTO_CONTENT_CULL_VARIANT", Kind::any_set, "port/src/vulkan_replay.h:1593", true},
    {"PALMETTO_CONTENT_PACKAGES", Kind::any_set, "port/shims/xgameruntime.cpp:1436", true},
    {"PALMETTO_CONTENT_PROBE", Kind::any_set, "port/src/vulkan_replay.h:1527", true},
    {"PALMETTO_CONTENT_PROBE_ARM", Kind::any_set, "port/src/vulkan_replay.h:1560", true},
    {"PALMETTO_CONTENT_PROBE_DRAWS", Kind::any_set, "port/src/vulkan_replay.h:1537", true},
    {"PALMETTO_CRASH_REPORT", Kind::any_set, "port/shims/startup_trace.h:355", true},
    {"PALMETTO_CRASH_REPORT_ALL", Kind::any_set, "port/shims/startup_trace.h:356", true},
    {"PALMETTO_CRASH_SAVE", Kind::any_set, "port/shims/startup_trace.h:643", true},
    {"PALMETTO_CRASH_SAVE_DIR", Kind::value_string, "port/shims/startup_trace.h:646", true},
    {"PALMETTO_CRASH_STACK", Kind::any_set, "port/shims/startup_trace.h:258", true},
    {"PALMETTO_D3D12", Kind::value_string, "port/shims/d3d12_xs.cpp:165", true},
    {"PALMETTO_D3D12X_QUERY_HEAP_READINESS", Kind::exact_one, "port/src/query_heap_readiness.h:13", true},
    {"PALMETTO_D3D12X_STRICT_VIEWPORTS", Kind::exact_one, "port/src/viewport_replay_readiness.h:13", true},
    {"PALMETTO_D3D12_RESIDENCY_BUDGET", Kind::exact_one, "port/src/d3d12_residency_budget.h:22", false},
    {"PALMETTO_DEVICE_HEALTH", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:1285", true},
    {"PALMETTO_DEVICE_TYPE", Kind::value_int, "port/shims/xgameruntime.cpp:1233", true},
    {"PALMETTO_DIRECT_HEAP_STRICT", Kind::exact_one, "port/shims/d3d12_com.cpp:426", true},
    {"PALMETTO_DISPLAY_DEFERRAL", Kind::value_string, "port/shims/xgameruntime_offline.cpp:1154", true},
    {"PALMETTO_DSTORAGE_TRACE", Kind::switch_on, "port/shims/dstorage.cpp:105", true},
    {"PALMETTO_DS_ARRAYS", Kind::switch_on, "port/shims/dstorage.cpp:1419", true},
    {"PALMETTO_DS_BCPACK", Kind::text_switch, "port/shims/dstorage.cpp:188", true},
    {"PALMETTO_DS_BCPACK_PREDICTOR", Kind::any_set, "port/shims/dstorage.cpp:191", true},
    {"PALMETTO_DS_LIGHTBAR", Kind::value_string, "port/shims/gameinput.cpp:163", true},
    {"PALMETTO_DS_LOAD_TRACE", Kind::value_int, "port/shims/dstorage.cpp:317", true},
    {"PALMETTO_DS_ORDER", Kind::value_int, "port/shims/dstorage.cpp:507", true},
    {"PALMETTO_DS_PAGES", Kind::text_switch, "port/shims/dstorage.cpp:185", true},
    {"PALMETTO_DS_PAGES_ARM", Kind::switch_on, "port/shims/dstorage.cpp:3697", true},
    {"PALMETTO_DS_PAGES_VIEW", Kind::text_switch, "port/shims/dstorage.cpp:186", true},
    {"PALMETTO_DS_PIN", Kind::text_switch, "port/shims/dstorage.cpp:183", true},
    {"PALMETTO_DS_PIN_RESIDENT", Kind::text_switch, "port/shims/dstorage.cpp:184", true},
    {"PALMETTO_DS_REAP_ORDER", Kind::value_int, "port/shims/dstorage.cpp:1829", true},
    {"PALMETTO_DS_RECORD", Kind::value_int, "port/shims/dstorage.cpp:625", true},
    {"PALMETTO_DS_STACK", Kind::switch_on, "port/shims/dstorage.cpp:400", true},
    {"PALMETTO_DS_STAGING", Kind::value_int, "port/shims/dstorage.cpp:182", true},
    {"PALMETTO_DS_TRIGGERS", Kind::value_string, "port/shims/gameinput.cpp:153", true},
    {"PALMETTO_DS_TRIGGER_FREQ", Kind::value_string, "port/shims/gameinput.cpp:161", true},
    {"PALMETTO_DS_TRIGGER_GAIN", Kind::value_string, "port/shims/gameinput.cpp:157", true},
    {"PALMETTO_DS_UNSHUFFLE", Kind::text_switch, "port/shims/dstorage.cpp:187", true},
    {"PALMETTO_DUMP_BYTES", Kind::exact_one, "port/src/main.cpp:10245", true},
    {"PALMETTO_DUMP_PIPELINES", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:7593", true},
    {"PALMETTO_DUMP_REGS", Kind::any_set, "port/shims/startup_trace.h:249", true},
    {"PALMETTO_DXGI_MODE_ANSWER", Kind::value_string, "port/shims/dxgi_com.cpp:825", true},
    {"PALMETTO_EXECUTOR_WATCH", Kind::exact_one, "port/src/main.cpp:1492", true},
    {"PALMETTO_FAKE_INPUT", Kind::any_set, "port/shims/gameinput.cpp:1981", true},
    {"PALMETTO_FAULT_MEM", Kind::any_set, "port/shims/startup_trace.h:268", true},
    {"PALMETTO_FAULT_MEM_RVA", Kind::value_int, "port/shims/startup_trace.h:308", true},
    {"PALMETTO_FILE_TRACE", Kind::any_set, "port/shims/file_system.cpp:718", true},
    {"PALMETTO_FIRST_RUN_SCRIPT", Kind::any_set, "port/shims/gameinput.cpp:2070", true},
    {"PALMETTO_FRAME_DUMP", Kind::switch_on, "port/src/vulkan_replay.h:5585", true},
    {"PALMETTO_FRAME_DUMP_AT_DRAW", Kind::value_int, "port/src/vulkan_replay.h:5595", true},
    {"PALMETTO_FRAME_DUMP_CONTROL", Kind::switch_on, "port/src/vulkan_replay.h:5617", true},
    {"PALMETTO_FRAME_DUMP_CONTROL_PS", Kind::value_int, "port/src/vulkan_backend.h:2267", true},
    {"PALMETTO_FRAME_DUMP_CONTROL_VS", Kind::value_int, "port/src/vulkan_backend.h:2266", true},
    {"PALMETTO_FRAME_DUMP_PS_CONTROL", Kind::switch_on, "port/src/vulkan_replay.h:5633", true},
    {"PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE", Kind::switch_on, "port/src/vulkan_replay.h:5647", true},
    {"PALMETTO_FRAME_DUMP_SAMPLE", Kind::switch_on, "port/src/vulkan_replay.h:1451", true},
    {"PALMETTO_FRAME_DUMP_WINDOW", Kind::value_int, "port/src/vulkan_replay.h:5605", true},
    {"PALMETTO_GAMEINPUT_THREAD", Kind::disable_value, "port/shims/gameinput.cpp:177", true},
    {"PALMETTO_GAMERTAG", Kind::any_set, "port/shims/xgameruntime_user.cpp:82", true},
    {"PALMETTO_GAME_DIR", Kind::value_string, "port/shims/xgameruntime.cpp:1800", true},
    {"PALMETTO_GPU_VIEW_PAIR", Kind::any_set, "port/shims/xmem.cpp:730", true},
    {"PALMETTO_GPU_VIEW_PAIR_TRACE", Kind::any_set, "port/shims/xmem.cpp:731", true},
    {"PALMETTO_GRAPHICS_CONSOLE_ROOT", Kind::switch_on, "port/src/vulkan_replay.h:1822", true},
    {"PALMETTO_HDR", Kind::value_string, "port/shims/xgameruntime_offline.cpp:575", true},
    {"PALMETTO_IMAGE_DEFERRED", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:1261", true},
    {"PALMETTO_IMAGE_MEM_POOL", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:1250", true},
    {"PALMETTO_IMAGE_MEM_TRACE", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:1239", true},
    {"PALMETTO_INPUT_TRACE", Kind::value_string, "port/shims/gameinput.cpp:210", true},
    {"PALMETTO_KEYBOARD_PAD", Kind::value_string, "port/shims/gameinput.cpp:178", true},
    {"PALMETTO_LIST_SLOT_TRACE", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:1885", true},
    {"PALMETTO_LOADER_BEGINEND", Kind::switch_on, "port/src/main.cpp:815", true},
    {"PALMETTO_LOADER_CENSUS", Kind::exact_one, "port/src/loader_census.h:38", true},
    {"PALMETTO_LOADER_HOOK", Kind::switch_on, "port/src/main.cpp:10489", true},
    {"PALMETTO_LOADER_POLLBP", Kind::switch_on, "port/src/main.cpp:835", true},
    {"PALMETTO_LOADER_WATCH", Kind::switch_on, "port/src/main.cpp:10565", true},
    {"PALMETTO_LOAD_TRACE", Kind::value_int, "port/shims/d3d12_xs_device.cpp:3453", true},
    {"PALMETTO_LOG_GAME_EVENTS", Kind::exact_one, "port/shims/xgameruntime_offline.cpp:129", true},
    {"PALMETTO_MEM_TRACE", Kind::value_int, "port/shims/xmem.cpp:202", true},
    {"PALMETTO_MESH_STREAM", Kind::any_set, "port/shims/d3d12_xs_device.cpp:7307", true},
    {"PALMETTO_MOUSE_SENSITIVITY", Kind::value_string, "port/shims/gameinput.cpp:159", true},
    {"PALMETTO_MOVIE_PROBE", Kind::exact_one, "port/src/main.cpp:10339", true},
    {"PALMETTO_NETWORK_HINT", Kind::value_string, "port/shims/xgameruntime.cpp:481", true},
    {"PALMETTO_OBJECT_WRITE_WATCH", Kind::exact_one, "port/src/main.cpp:1807", true},
    {"PALMETTO_PACE_CONTENT", Kind::any_set, "port/src/vulkan_backend.h:3505", true},
    {"PALMETTO_PACE_DELAY_US", Kind::any_set, "port/src/vulkan_backend.h:3504", true},
    {"PALMETTO_PACE_IDLE", Kind::any_set, "port/src/vulkan_backend.h:3504", true},
    {"PALMETTO_PACKAGED", Kind::value_int, "port/shims/xgameruntime.cpp:1536", true},
    {"PALMETTO_PACKAGE_ID", Kind::any_set, "port/shims/xgameruntime.cpp:1402", true},
    {"PALMETTO_PACKAGE_READY", Kind::value_string, "port/shims/xgameruntime.cpp:1591", true},
    {"PALMETTO_PACKAGE_V4", Kind::value_string, "port/shims/xgameruntime.cpp:1573", true},
    {"PALMETTO_PAD_PRESENT", Kind::disable_value, "port/shims/gameinput_nosdl.h:876", true},
    {"PALMETTO_PAGE_PULL", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:3572", true},
    {"PALMETTO_PERSIST_PROFILE", Kind::value_string, "port/shims/xgameruntime.cpp:913", true},
    {"PALMETTO_PE_LOADER_SHADOW_PARSE", Kind::exact_one, "port/src/pe_loader.cpp:1518", true},
    {"PALMETTO_PE_LOADER_SHADOW_STRICT", Kind::exact_one, "port/src/pe_loader.cpp:1518", true},
    {"PALMETTO_PIPELINE_CACHE", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:495", true},
    {"PALMETTO_PIPELINE_CACHE_MAX_MB", Kind::value_int, "port/shims/d3d12_vulkan_device.cpp:512", true},
    {"PALMETTO_PIPELINE_CALL_STATS", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:1541", true},
    {"PALMETTO_PIPELINE_COMPILE_BOUND", Kind::value_int, "port/shims/d3d12_vulkan_device.cpp:1512", true},
    {"PALMETTO_PIPELINE_COMPILE_WORKERS", Kind::value_int, "port/shims/d3d12_vulkan_device.cpp:1501", true},
    {"PALMETTO_PIPELINE_DEFER_COMPILE", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:1491", true},
    {"PALMETTO_PIPELINE_HOLD", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:1570", true},
    {"PALMETTO_PIPELINE_HOLD_BOUND", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:1580", true},
    {"PALMETTO_PIPELINE_HOLD_BUDGET_MS", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:1591", true},
    {"PALMETTO_PIPELINE_LAZY_BUILD", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:1474", true},
    {"PALMETTO_PIPELINE_LIVE_CAP", Kind::value_int, "port/shims/d3d12_vulkan_device.cpp:1461", true},
    {"PALMETTO_PIPELINE_PREWARM", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:1528", true},
    {"PALMETTO_PIPELINE_PROFILE", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:241", true},
    {"PALMETTO_PIPELINE_RECIPE_CACHE", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:1141", true},
    {"PALMETTO_PIPELINE_TIMELINE", Kind::value_int, "port/shims/d3d12_xs_device.cpp:276", true},
    {"PALMETTO_PLANE_REFRESH", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:1735", true},
    {"PALMETTO_PLANE_REFRESH_TRACE", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:1736", true},
    {"PALMETTO_POKE_HOST", Kind::any_set, "port/shims/startup_trace.h:557", true},
    {"PALMETTO_POKE_LEA", Kind::exact_one, "port/src/main.cpp:10266", true},
    {"PALMETTO_POKE_LOG", Kind::exact_one, "port/src/main.cpp:10321", true},
    {"PALMETTO_PRESENT_ADOPT", Kind::exact_one, "port/shims/xbox_present.cpp:1017", true},
    {"PALMETTO_PRESENT_ATTEST", Kind::any_set, "port/src/main.cpp:10067", true},
    {"PALMETTO_PRESENT_DRIVEN", Kind::value_string, "port/shims/xbox_present.cpp:461", true},
    {"PALMETTO_PRESENT_FIT", Kind::any_set, "port/src/vulkan_present.h:207", true},
    {"PALMETTO_PRESENT_GEOMETRY", Kind::value_int, "port/src/vulkan_present.h:222", true},
    {"PALMETTO_PRESENT_HDR10", Kind::any_set, "port/src/vulkan_present.h:240", true},
    {"PALMETTO_PRESENT_HEIGHT", Kind::value_int, "port/shims/xbox_present.cpp:1171", true},
    {"PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY", Kind::value_string, "port/shims/xbox_present.cpp:372", true},
    {"PALMETTO_PRESENT_NO_TEAR", Kind::value_string, "port/shims/d3d12_vulkan_device.cpp:14372", true},
    {"PALMETTO_PRESENT_PACING", Kind::value_string, "port/shims/d3d12_xs_device.cpp:5133", true},
    {"PALMETTO_PRESENT_PER_ORIGIN", Kind::any_set, "port/shims/xbox_present.cpp:57", true},
    {"PALMETTO_PRESENT_PLANE_DUMP", Kind::switch_on, "port/shims/xbox_present.cpp:408", true},
    {"PALMETTO_PRESENT_PLANE_RULE", Kind::any_set, "port/shims/xbox_present.cpp:358", true},
    {"PALMETTO_PRESENT_STUB", Kind::any_set, "port/shims/d3d12_xs_device.cpp:4788", true},
    {"PALMETTO_PRESENT_SYNTHETIC", Kind::value_int, "port/src/vulkan_present.h:224", true},
    {"PALMETTO_PRESENT_UNCAP", Kind::value_string, "port/shims/xbox_present.cpp:538", true},
    {"PALMETTO_PRESENT_WIDTH", Kind::value_int, "port/shims/xbox_present.cpp:1170", true},
    {"PALMETTO_PRESENT_WINDOW", Kind::value_string, "port/shims/xbox_present.cpp:987", true},
    {"PALMETTO_PROMPTS", Kind::value_string, "port/shims/gameinput.cpp:185", true},
    {"PALMETTO_PROMPTS_SWITCH_MS", Kind::value_string, "port/shims/gameinput.cpp:195", true},
    {"PALMETTO_RASTER_CULL_MEASURED", Kind::switch_on, "port/src/graphics_pso.cpp:189", true},
    {"PALMETTO_RASTER_RECORD_MEASURED", Kind::any_set, "port/src/graphics_pso.cpp:427", true},
    {"PALMETTO_READBACK_AFTER", Kind::any_set, "port/src/vulkan_backend.h:1078", true},
    {"PALMETTO_READBACK_AT_DRAW", Kind::value_int, "port/src/vulkan_replay.h:1477", true},
    {"PALMETTO_READBACK_CENTRE", Kind::switch_on, "port/src/vulkan_backend.h:5017", true},
    {"PALMETTO_READBACK_FULL", Kind::any_set, "port/src/vulkan_replay.h:1494", true},
    {"PALMETTO_READBACK_PPM", Kind::value_int, "port/src/vulkan_backend.h:5402", true},
    {"PALMETTO_READBACK_PPM_LIMIT", Kind::value_int, "port/src/vulkan_backend.h:5405", true},
    {"PALMETTO_READ_TERMINAL", Kind::any_set, "port/shims/dstorage.cpp:1705", true},
    {"PALMETTO_REPLAY_MISS", Kind::switch_on, "port/src/vulkan_replay.h:1081", true},
    {"PALMETTO_RESOLVE", Kind::exact_one, "port/src/main.cpp:10184", true},
    {"PALMETTO_RIP_HIST", Kind::any_set, "port/src/main.cpp:10422", true},
    {"PALMETTO_RNG_SEED", Kind::any_set, "port/shims/bcrypt_rng.cpp:80", true},
    {"PALMETTO_ROOT_DATA_TRACE", Kind::switch_on, "port/shims/d3d12_com.cpp:2008", true},
    {"PALMETTO_ROOT_PACK_MEASURE", Kind::switch_on, "port/shims/d3d12_com.cpp:415", true},
    {"PALMETTO_RT_AS", Kind::value_string, "port/shims/d3d12_vulkan_device.cpp:14688", true},
    {"PALMETTO_RT_ENV_INIT", Kind::any_set, "port/shims/process.cpp:315", true},
    {"PALMETTO_RUMBLE", Kind::disable_value, "port/shims/gameinput.cpp:176", true},
    {"PALMETTO_S3DISPATCH", Kind::switch_on, "port/src/main.cpp:10539", true},
    {"PALMETTO_SAVE_DIR", Kind::value_string, "port/shims/xgameruntime_gamesave.cpp:180", true},
    {"PALMETTO_SDR_EOTF", Kind::value_string, "port/shims/xbox_present.cpp:523", true},
    {"PALMETTO_SEED_GLOBAL", Kind::exact_one, "port/src/main.cpp:10204", true},
    {"PALMETTO_SHADERS", Kind::value_string, "port/src/main.cpp:9955", true},
    {"PALMETTO_SIG_CLASS", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:6914", true},
    {"PALMETTO_SILENT_SPEECH", Kind::value_string, "port/shims/xgameruntime_offline.cpp:840", true},
    {"PALMETTO_SKIP_CPU_CHECK", Kind::exact_one, "port/src/host_cpu.cpp:73", true},
    {"PALMETTO_SKIP_INT3", Kind::any_set, "port/shims/startup_trace.h:782", true},
    {"PALMETTO_SLOT67_PROGRAM_LOG", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:7819", true},
    {"PALMETTO_SLOT70_CBV", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:5483", true},
    {"PALMETTO_SLOT70_CENSUS", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:5484", true},
    {"PALMETTO_SLOT70_IDENTITY_VA", Kind::value_int, "port/shims/d3d12_xs_device.cpp:5875", true},
    {"PALMETTO_SLOT70_PAIR", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:5485", true},
    {"PALMETTO_SLOT70_PROBE", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:5483", true},
    {"PALMETTO_SLOT70_PUBLISH", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:5484", true},
    {"PALMETTO_SLOT_WATCH", Kind::switch_on, "port/src/main.cpp:10531", true},
    {"PALMETTO_STAGE2_DISPATCH", Kind::value_int, "port/src/main.cpp:10471", true},
    {"PALMETTO_STAGE2_READY", Kind::value_int, "port/src/main.cpp:10481", true},
    {"PALMETTO_STAGE2_TRACKER", Kind::value_int, "port/src/main.cpp:10461", true},
    {"PALMETTO_STAGE2_WAKE", Kind::value_int, "port/src/main.cpp:10452", true},
    {"PALMETTO_STAGE3_PUMP", Kind::any_set, "port/src/main.cpp:10448", true},
    {"PALMETTO_STANDIN_PIPELINES", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:3599", true},
    {"PALMETTO_STATE_TRACE", Kind::exact_one, "port/src/main.cpp:10431", true},
    {"PALMETTO_STOP_INT3", Kind::any_set, "port/shims/startup_trace.h:765", true},
    {"PALMETTO_STORE_ASYNC_COMPLETE", Kind::value_string, "port/shims/xgameruntime_store.cpp:147", true},
    {"PALMETTO_STREAMOPEN", Kind::switch_on, "port/src/main.cpp:10557", true},
    {"PALMETTO_STREAMOPEN_WATCH", Kind::switch_on, "port/src/main.cpp:10558", true},
    {"PALMETTO_STREAMREAP", Kind::switch_on, "port/src/main.cpp:10546", true},
    {"PALMETTO_STREAMREAP_WATCH", Kind::switch_on, "port/src/main.cpp:10547", true},
    {"PALMETTO_STREAM_CONSUMER", Kind::switch_on, "port/shims/dstorage.cpp:753", true},
    {"PALMETTO_SUBMIT_OVERLAP", Kind::switch_on, "port/src/queue_holds.h:54", true},
    {"PALMETTO_SUBMIT_OVERLAP_COPY_BOUND", Kind::any_set, "port/src/queue_holds.h:57", true},
    {"PALMETTO_SUBMIT_OVERLAP_DIRECT_BOUND", Kind::switch_on, "port/src/queue_holds.h:56", true},
    {"PALMETTO_SUBMIT_TRACE", Kind::switch_on, "port/src/vulkan_backend.h:3930", true},
    {"PALMETTO_TEB_CHECK", Kind::any_set, "port/src/main.cpp:10226", true},
    {"PALMETTO_TEXTURE_BIND_FETCHED", Kind::any_set, "port/src/vulkan_replay.h:1760", true},
    {"PALMETTO_TEXTURE_IMPORT", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:3586", true},
    {"PALMETTO_TEXTURE_IMPORT_MAX", Kind::value_int, "port/shims/d3d12_vulkan_device.cpp:4184", true},
    {"PALMETTO_TEXTURE_PAGE_WATCH", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:2848", true},
    {"PALMETTO_THREAD_STACKS", Kind::any_set, "port/src/main.cpp:10426", true},
    {"PALMETTO_TIMELINE", Kind::switch_on, "port/src/main.cpp:10496", true},
    {"PALMETTO_TITLE_APPSTATE_CALLBACK", Kind::exact_one, "port/shims/appnotify.cpp:38", true},
    {"PALMETTO_TITLE_BOOTMEDIA_OPEN", Kind::exact_one, "port/src/main.cpp:10134", true},
    {"PALMETTO_TITLE_CLASS0A_SIZE", Kind::exact_one, "port/src/main.cpp:10047", true},
    {"PALMETTO_TITLE_FRAME_REQUEST", Kind::exact_one, "port/src/main.cpp:10157", true},
    {"PALMETTO_TITLE_OUTRECORD_STATE2", Kind::exact_one, "port/src/main.cpp:10066", true},
    {"PALMETTO_TITLE_PEEK", Kind::any_set, "port/shims/d3d12_xs_device.cpp:3580", true},
    {"PALMETTO_TITLE_PRESENT_KEYS", Kind::exact_one, "port/src/main.cpp:10094", true},
    {"PALMETTO_TITLE_PULL_COALESCE", Kind::switch_on, "port/src/vulkan_backend.h:1112", true},
    {"PALMETTO_TITLE_PULL_TRACE", Kind::switch_on, "port/src/vulkan_backend.h:1111", true},
    {"PALMETTO_TITLE_SESSION3_COMPLETE", Kind::exact_one, "port/src/main.cpp:10129", true},
    {"PALMETTO_TITLE_VA", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:3560", true},
    {"PALMETTO_TITLE_VA_WINDOWS", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:2003", true},
    {"PALMETTO_TITLE_WATCH", Kind::any_set, "port/shims/d3d12_xs_device.cpp:3511", true},
    {"PALMETTO_TITLE_WATCHDOG_HOLD", Kind::any_set, "port/src/main.cpp:10091", true},
    {"PALMETTO_TLS_GA_PATCH", Kind::any_set, "port/src/pe_loader.cpp:601", true},
    {"PALMETTO_TRACE_TLS", Kind::any_set, "port/src/pe_loader.cpp:552", true},
    {"PALMETTO_UI_INPUT_SCRIPT", Kind::exact_one, "port/src/ui_input_script.h:38", false},
    {"PALMETTO_UI_PROBE", Kind::exact_one, "port/src/main.cpp:10371", true},
    {"PALMETTO_UNIMPLEMENTED", Kind::any_set, "port/shims/registry.cpp:309", true},
    {"PALMETTO_UPLOAD_SYSMEM", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:9241", true},
    {"PALMETTO_UPSCALE_KIND7_CONSUMER", Kind::exact_one, "port/src/upscale_kind7.h:20", true},
    {"PALMETTO_UPSCALE_STANDIN", Kind::switch_on, "port/src/upscale_standin.h:193", true},
    {"PALMETTO_UPSCALE_STANDIN_CONTROL", Kind::switch_on, "port/src/upscale_standin.h:203", true},
    {"PALMETTO_USER_CHANGE_EVENT", Kind::value_string, "port/shims/xgameruntime_user.cpp:383", true},
    {"PALMETTO_USER_TRACE", Kind::value_string, "port/shims/platform_trace.h:31", true},
    {"PALMETTO_VERTEX_MATRIX_DESCRIPTOR", Kind::switch_on, "port/src/vulkan_replay.h:1768", true},
    {"PALMETTO_VERTEX_PATH", Kind::switch_on, "port/src/vulkan_replay.h:1731", true},
    {"PALMETTO_VERTEX_STREAM_DESCRIPTOR", Kind::switch_on, "port/src/vulkan_replay.h:1747", true},
    {"PALMETTO_VK_DEVICE", Kind::any_set, "port/src/vulkan_device_filter.h:22", true},
    {"PALMETTO_VK_GRAPHICS_FAMILY_ONLY", Kind::value_int, "port/shims/d3d12_vulkan_device.cpp:132", true},
    {"PALMETTO_VK_MAX_QUEUES_PER_FAMILY", Kind::value_int, "port/shims/d3d12_vulkan_device.cpp:141", true},
    {"PALMETTO_VK_VALIDATION", Kind::switch_on, "port/shims/d3d12_vulkan_device.cpp:14504", true},
    {"PALMETTO_WAIT_SAMPLE", Kind::any_set, "port/src/main.cpp:10413", true},
    {"PALMETTO_WAIT_TRACE", Kind::any_set, "port/src/main.cpp:10418", true},
    {"PALMETTO_WATCH_GLOBAL", Kind::exact_one, "port/src/main.cpp:10506", true},
    {"PALMETTO_WATCH_RVA_WRITE", Kind::value_int, "port/shims/startup_trace.h:447", true},
    {"PALMETTO_WINDOW_FIT", Kind::any_set, "port/shims/xbox_present.cpp:251", true},
    {"PALMETTO_WRITER_STACK_LOG", Kind::any_set, "port/src/main.cpp:2127", true},
    {"PALMETTO_WRITER_SWEEP", Kind::any_set, "port/src/main.cpp:2537", true},
    {"PALMETTO_WRITER_SWEEP_DELAY_MS", Kind::value_int, "port/src/main.cpp:2526", true},
    {"PALMETTO_WRITER_WATCH", Kind::exact_one, "port/src/main.cpp:2708", true},
    {"PALMETTO_XBOX_VIEW_SHAPE", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:3237", true},
    {"PALMETTO_XG_DUMP", Kind::switch_on, "port/shims/xg_xs.cpp:227", true},
    {"PALMETTO_XG_LAYOUT", Kind::value_string, "port/shims/xg_xs.cpp:208", true},
    {"PALMETTO_XG_LINEAR_ARRAYS", Kind::any_set, "port/shims/xg_xs.cpp:132", true},
    {"PALMETTO_XG_MOVIE_PLANES", Kind::value_string, "port/shims/xg_xs.cpp:111", true},
    {"PALMETTO_XG_TRACE", Kind::switch_on, "port/shims/xg_xs.cpp:226", true},
    {"PALMETTO_XMA2_DECODER_DLL", Kind::value_string, "port/shims/audio_decode.h:177", true},
    {"PALMETTO_XMEM_ALIAS_WATCH", Kind::any_set, "port/shims/d3d12_vulkan_device.cpp:1717", true},
    {"PALMETTO_XMEM_CALLERS", Kind::any_set, "port/shims/xmem.cpp:83", true},
    {"PALMETTO_XMEM_LAYOUT", Kind::any_set, "port/shims/xmem.cpp:2518", true},
    {"PALMETTO_XMEM_LAYOUT_CALLER", Kind::any_set, "port/shims/xmem.cpp:2519", true},
    {"PALMETTO_XMEM_NATIVE_WRITE_WATCH", Kind::any_set, "port/shims/xmem.cpp:2559", true},
    {"PALMETTO_XS_ALLOCATOR_CALLBACK", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:1441", true},
    {"PALMETTO_XS_ALLOCATOR_CALLBACK_CLASS", Kind::value_int, "port/shims/d3d12_xs_device.cpp:1468", true},
    {"PALMETTO_XS_ALLOCATOR_CALLBACK_PROBE", Kind::value_int, "port/shims/d3d12_xs_device.cpp:1479", true},
    {"PALMETTO_XS_ALLOCATOR_CALLBACK_SIZE", Kind::value_int, "port/shims/d3d12_xs_device.cpp:1456", true},
    {"PALMETTO_XS_ALLOCATOR_CALLBACK_TRACE", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:1446", true},
    {"PALMETTO_XS_BOTTOM_OF_PIPE_GPU", Kind::value_int, "port/shims/d3d12_xs_device.cpp:1967", true},
    {"PALMETTO_XS_CALLERS", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:531", true},
    {"PALMETTO_XS_CALLERS_PROFILE", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:542", true},
    {"PALMETTO_XS_CLEAR_BARRIER_PROBE", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:3652", true},
    {"PALMETTO_XS_CONSOLE_LIST_EXTRAS", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:2824", true},
    {"PALMETTO_XS_CONSOLE_LIST_TRACE", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:2829", true},
    {"PALMETTO_XS_FRAME_INTERVAL_SLOT", Kind::value_int, "port/shims/d3d12_xs_device.cpp:654", true},
    {"PALMETTO_XS_LIST_DUMP", Kind::any_set, "port/shims/d3d12_xs_device.cpp:4204", true},
    {"PALMETTO_XS_LIST_ID", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:4288", true},
    {"PALMETTO_XS_LIST_MUTE", Kind::value_int, "port/shims/d3d12_xs_device.cpp:4179", true},
    {"PALMETTO_XS_LIST_X_SLOT", Kind::value_int, "port/shims/d3d12_xs_device.cpp:658", true},
    {"PALMETTO_XS_MEMORY_WAIT_BOUND_MS", Kind::any_set, "port/shims/d3d12_xs_device.cpp:1965", true},
    {"PALMETTO_XS_MEMORY_WAIT_HOLD", Kind::any_set, "port/shims/d3d12_xs_device.cpp:1964", true},
    {"PALMETTO_XS_MEMORY_WAIT_MAX", Kind::any_set, "port/shims/d3d12_xs_device.cpp:1966", true},
    {"PALMETTO_XS_MEMORY_WAIT_TRACE", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:1987", true},
    {"PALMETTO_XS_QUEUE_X_SLOT", Kind::value_int, "port/shims/d3d12_xs_device.cpp:657", true},
    {"PALMETTO_XS_SCHEDULE_EVENT_SLOT", Kind::value_int, "port/shims/d3d12_xs_device.cpp:655", true},
    {"PALMETTO_XS_SIG41", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:748", true},
    {"PALMETTO_XS_SLOT57_LAYOUT_DUMP", Kind::switch_on, "port/shims/d3d12_xs_device.cpp:6941", true},
    {"PALMETTO_XS_WAIT_EVENT_SLOT", Kind::value_int, "port/shims/d3d12_xs_device.cpp:656", true},
};

inline constexpr std::size_t kSwitchCount = sizeof(kSwitches) / sizeof(kSwitches[0]);

// The only PALMETTO_* string literal in port/src or port/shims that is not a switch: a synthetic gameinput
// device-id payload written by memcpy, not read from the environment.
inline constexpr const char* kNotSwitches[] = {"PALMETTO_AGGREGATE_DEVICE_ID"};
inline constexpr std::size_t kNotSwitchCount = sizeof(kNotSwitches) / sizeof(kNotSwitches[0]);

// The scans the test repeats over the source: port/src and port/shims are the two roots it reads.
inline constexpr const char* kScanRoots[] = {"port/src", "port/shims"};
inline constexpr std::size_t kScanRootCount = sizeof(kScanRoots) / sizeof(kScanRoots[0]);

}  // namespace rt::opt_in_switches
