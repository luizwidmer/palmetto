// Pure, loader-free tests for the bounded PE-loader census formatter. Synthetic facts only; no game data, no host calls,
// no loader instance and no environment mutation. The expected strings are the shape the loader's insertion hunks
// emit; the numbers are the measured 103-arm corpus values from docs/pe_loader_rewrite.md's inherited-loader audit.
#include "../src/loader_census.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#define CHECK(condition) do {                                                            \
    if (!(condition)) {                                                                  \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                   \
        return 1;                                                                        \
    }                                                                                    \
    ++checks;                                                                            \
} while (0)

static int checks = 0;

namespace {

using namespace rt::loader_census;

std::string format_module(const ModuleFacts& facts) {
    char line[kMaxLineBytes];
    const std::size_t length = format_mapped(facts, line, sizeof line);
    if (!length) return {};
    return {line, length};
}

std::string format_bind(const BindFacts& facts) {
    char line[kMaxLineBytes];
    const std::size_t length = format_binds(facts, line, sizeof line);
    if (!length) return {};
    return {line, length};
}

std::string format_tls_line(const TlsFacts& facts) {
    char line[kMaxLineBytes];
    const std::size_t length = format_tls(facts, line, sizeof line);
    if (!length) return {};
    return {line, length};
}

std::string format_counters(const TlsCounters& counters) {
    char line[kMaxLineBytes];
    const std::size_t length = format_tls_counters(counters, line, sizeof line);
    if (!length) return {};
    return {line, length};
}

std::string format_failure(const ProtectFailure& failure) {
    char line[kMaxLineBytes];
    const std::size_t length = format_protect_failure(failure, line, sizeof line);
    if (!length) return {};
    return {line, length};
}

}  // namespace

int main() {
    // The switch is exact: only "1" turns diagnostics on.
    CHECK(!switch_enabled_from(nullptr));
    CHECK(!switch_enabled_from(""));
    CHECK(!switch_enabled_from("0"));
    CHECK(!switch_enabled_from("true"));
    CHECK(!switch_enabled_from("01"));
    CHECK(!switch_enabled_from(" 1"));
    CHECK(!switch_enabled_from("1 "));
    CHECK(!switch_enabled_from("yes"));
    CHECK(!switch_enabled_from("2"));
    CHECK(switch_enabled_from("1"));

    // Mapped line: the measured title facts (preferred base, not relocated) and a rebased DLL.
    ModuleFacts title;
    title.name = "game_scarlett_master.exe";
    title.base = 0x140000000ull;
    title.preferred_base = 0x140000000ull;
    title.size = 0xb06d000u;
    title.entry_rva = 0x56a1ec8u;
    title.relocated = false;
    CHECK(format_module(title) ==
          "[rt] loader: census v1 mapped game_scarlett_master.exe base=0x140000000 size=0xb06d000 "
          "preferred=0x140000000 relocated=false entry=+0x56a1ec8");

    ModuleFacts dll;
    dll.name = "Bink2Scarlett.dll";
    dll.base = 0x1f3a0000ull;
    dll.preferred_base = 0x180000000ull;
    dll.size = 0x44000u;
    dll.entry_rva = 0x2ac1cu;
    dll.relocated = true;
    CHECK(format_module(dll) ==
          "[rt] loader: census v1 mapped Bink2Scarlett.dll base=0x1f3a0000 size=0x44000 "
          "preferred=0x180000000 relocated=true entry=+0x2ac1c");

    // Bind summary: the title's measured 95 shim + 420 host + 72 module = 587 imports, 0 unimplemented.
    BindFacts binds;
    binds.name = "game_scarlett_master.exe";
    binds.counts = {95, 420, 72, 0};
    CHECK(binds.counts.total() == 587);
    CHECK(format_bind(binds) ==
          "[rt] loader: census v1 bound game_scarlett_master.exe total=587 shim=95 host=420 module=72 "
          "unimplemented=0");
    // total() is computed from its parts, so a caller cannot print a disagreeing total, and it cannot wrap at 32-bit.
    BindFacts saturated;
    saturated.name = "x.dll";
    saturated.counts = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    CHECK(saturated.counts.total() == 4ull * 0xFFFFFFFFull);
    CHECK(format_bind(saturated).find("total=17179869180") != std::string::npos);

    // TLS facts: the title's measured module index 6, slot 0 and /GA cell 2 with 53,246 rewritten reads.
    TlsFacts tls;
    tls.name = "game_scarlett_master.exe";
    tls.index = 6;
    tls.slot = 0;
    tls.ga_slot = 2;
    tls.ga_sites = 53246;
    CHECK(format_tls_line(tls) ==
          "[rt] loader: census v1 tls game_scarlett_master.exe index=6 slot=0 ga_slot=2 ga_sites=53246");
    // A module without static TLS prints 'none' per absent field, not a sentinel.
    TlsFacts no_tls;
    no_tls.name = "XboxBvhBuilder.dll";
    CHECK(format_tls_line(no_tls) ==
          "[rt] loader: census v1 tls XboxBvhBuilder.dll index=none slot=none ga_slot=none ga_sites=0");

    TlsCounters counters;
    counters.attach_events = 61;
    counters.detach_events = 58;
    counters.failures = 0;
    CHECK(format_counters(counters) ==
          "[rt] loader: census v1 tls-counters attach=61 detach=58 failures=0");

    // VirtualProtect failures: the headers call, a named section, an unnamed eight-byte section, and the bound line.
    ProtectFailure headers;
    headers.module = "tiny_dll.dll";
    headers.target = ProtectTarget::headers;
    headers.rva = 0;
    headers.size = 0x1000;
    headers.requested_protection = 0x2;
    headers.last_error = 5;
    CHECK(format_failure(headers) ==
          "[rt] loader: census v1 protect-failed module=tiny_dll.dll headers rva=0x0 size=0x1000 prot=0x2 error=5");

    ProtectFailure section;
    section.module = "tiny_dll.dll";
    section.target = ProtectTarget::section;
    section.section_index = 3;
    section.section_name = ".text";
    section.rva = 0x1000;
    section.size = 0x1234;
    section.requested_protection = 0x20;
    section.last_error = 87;
    CHECK(format_failure(section) ==
          "[rt] loader: census v1 protect-failed module=tiny_dll.dll section[3]=.text rva=0x1000 size=0x1234 "
          "prot=0x20 error=87");

    ProtectFailure unnamed = section;
    unnamed.section_index = 7;
    unnamed.section_name = {};
    CHECK(format_failure(unnamed) ==
          "[rt] loader: census v1 protect-failed module=tiny_dll.dll section[7] rva=0x1000 size=0x1234 "
          "prot=0x20 error=87");

    char omitted[kMaxLineBytes];
    const std::size_t omitted_length = format_protect_omitted(5, omitted, sizeof omitted);
    CHECK(omitted_length > 0);
    CHECK(std::string_view(omitted, omitted_length) ==
          "[rt] loader: census v1 protect-omitted count=5");

    // A name is bounded and single-line: a newline or control byte cannot split a census line.
    ModuleFacts hostile = title;
    hostile.name = "bad\nname.dll";
    CHECK(format_module(hostile).find("mapped bad?name.dll base=") != std::string::npos);
    CHECK(format_module(hostile).find('\n') == std::string::npos);

    std::string long_name(120, 'n');
    ModuleFacts truncated = title;
    truncated.name = long_name;
    const std::string truncated_line = format_module(truncated);
    CHECK(truncated_line.size() < kMaxLineBytes);
    CHECK(truncated_line.find("mapped " + std::string(96, 'n') + "... ") != std::string::npos);
    CHECK(truncated_line.find(std::string(97, 'n')) == std::string::npos);

    // Every formatter refuses invalid or too-small storage without writing a partial line past the capacity.
    std::vector<char> guarded(kMaxLineBytes + 8, static_cast<char>(0xAA));
    CHECK(format_mapped(title, nullptr, 0) == 0);
    CHECK(format_mapped(title, guarded.data(), 0) == 0);
    CHECK(format_mapped(title, guarded.data(), 16) == 0);
    // A truncated attempt may NUL-terminate inside the capacity (std::snprintf semantics) but must never write at or
    // past `capacity`; that is the guarantee the loading path needs, so check the guard bytes past it.
    bool untouched = true;
    for (std::size_t i = 16; i < guarded.size(); ++i) untouched = untouched && guarded[i] == static_cast<char>(0xAA);
    CHECK(untouched);

    ModuleFacts full = title;
    const std::size_t measured = format_mapped(full, guarded.data(), kMaxLineBytes);
    CHECK(measured > 0 && measured < kMaxLineBytes);
    CHECK(guarded[measured] == '\0');
    for (std::size_t i = 0; i < 4; ++i) CHECK(guarded[kMaxLineBytes + i] == static_cast<char>(0xAA));

    // Determinism: the same facts format to the same bytes.
    CHECK(format_module(title) == format_module(title));
    CHECK(format_bind(binds) == format_bind(binds));

    // Collector: disabled counts nothing; enabled counts exactly the notes; snapshot is stable.
    Collector disabled(false);
    disabled.note_attach();
    disabled.note_detach();
    disabled.note_failure();
    CHECK(!disabled.enabled());
    const TlsCounters off = disabled.snapshot();
    CHECK(off.attach_events == 0 && off.detach_events == 0 && off.failures == 0);

    Collector enabled(true);
    enabled.note_attach();
    enabled.note_attach();
    enabled.note_detach();
    enabled.note_failure();
    const TlsCounters on = enabled.snapshot();
    CHECK(on.attach_events == 2);
    CHECK(on.detach_events == 1);
    CHECK(on.failures == 1);
    CHECK(enabled.snapshot().attach_events == on.attach_events);

    std::printf("loader_census_test: OK (%d checks)\n", checks);
    return 0;
}