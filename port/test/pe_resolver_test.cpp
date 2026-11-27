// Pure, synthetic tests for the PE import/export resolver (docs/pe_loader_rewrite.md). No Windows loader calls or game
// data: module bases and export tables are small in-memory descriptions built by this test.
#include "pe_resolver.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void check_equal(std::uint64_t got, std::uint64_t want, const char* what) {
    check(got == want, what);
    if (got != want)
        std::fprintf(stderr, "     got %llu, want %llu\n", static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));
}

void check_string(const std::string& got, const char* want, const char* what) {
    check(got == want, what);
    if (got != want) std::fprintf(stderr, "     got '%s', want '%s'\n", got.c_str(), want);
}

void check_pointer(const void* got, const void* want, const char* what) {
    check(got == want, what);
    if (got != want) std::fprintf(stderr, "     got %p, want %p\n", got, want);
}

std::string normalized_library(std::string_view library) {
    std::string key;
    key.reserve(library.size() + 4);
    for (char ch : library) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        key.push_back(ch);
    }
    if (key.size() < 4 || key.compare(key.size() - 4, 4, ".dll") != 0) key += ".dll";
    return key;
}

rt::pe::ExportSymbol named_export(std::string name, std::uint32_t ordinal, std::uint32_t rva,
                                  std::string forwarder = {}) {
    rt::pe::ExportSymbol result;
    result.name = std::move(name);
    result.ordinal = ordinal;
    result.rva = rva;
    result.forwarder = std::move(forwarder);
    result.by_name = true;
    return result;
}

rt::pe::ExportSymbol ordinal_export(std::uint32_t ordinal, std::uint32_t rva, std::string forwarder = {}) {
    rt::pe::ExportSymbol result;
    result.ordinal = ordinal;
    result.rva = rva;
    result.forwarder = std::move(forwarder);
    return result;
}

rt::pe::ImportSymbol named_import(std::string name, std::uint32_t iat_rva) {
    rt::pe::ImportSymbol result;
    result.name = std::move(name);
    result.iat_rva = iat_rva;
    return result;
}

rt::pe::ImportSymbol ordinal_import(std::uint16_t ordinal, std::uint32_t iat_rva) {
    rt::pe::ImportSymbol result;
    result.ordinal = ordinal;
    result.iat_rva = iat_rva;
    result.by_ordinal = true;
    return result;
}

struct FakeModule {
    std::string owner;
    std::uintptr_t base = 0;
    rt::Binding binding = rt::Binding::Module;
    std::vector<rt::pe::ExportSymbol> exports;
};

class FakeSource final : public rt::pe::SymbolSource {
public:
    void add_module(std::string name, std::uintptr_t base, std::vector<rt::pe::ExportSymbol> exports,
                    rt::Binding binding = rt::Binding::Module) {
        FakeModule module;
        module.owner = name;
        module.base = base;
        module.binding = binding;
        module.exports = std::move(exports);
        modules.emplace(normalized_library(name), std::move(module));
    }

    std::optional<rt::pe::SymbolResolution> resolve(std::string_view library,
                                                     const rt::pe::SymbolQuery& symbol) override {
        queries.push_back(std::string(library) + "!" + symbol.display_name());
        const auto found = modules.find(normalized_library(library));
        if (found == modules.end()) return std::nullopt;
        const FakeModule& module = found->second;
        return rt::pe::resolve_export(module.exports, symbol, reinterpret_cast<void*>(module.base), module.binding,
                                      module.owner);
    }

    std::map<std::string, FakeModule> modules;
    std::vector<std::string> queries;
};

void test_import_planning_and_aliases() {
    using namespace rt::pe;
    FakeSource source;
    // Alpha and AlphaAlias model two name-table entries with the same export ordinal and function RVA.
    source.add_module("direct.dll", 0x10000000u,
                      {named_export("Alpha", 7, 0x100), named_export("AlphaAlias", 7, 0x100)}, rt::Binding::Module);
    source.add_module("forward.dll", 0x20000000u, {named_export("ViaName", 8, 0x210, "middle.Step")});
    source.add_module("middle.dll", 0x30000000u, {named_export("Step", 9, 0x220, "final.#42")});
    source.add_module("final.dll", 0x40000000u, {ordinal_export(42, 0x330)});

    std::vector<ImportLibrary> imports;
    // Deliberately provide the later IAT range first; the resulting plan is sorted by slot RVA.
    imports.push_back({"Direct.dll",
                       {named_import("Alpha", 0x3008), ordinal_import(7, 0x3010), named_import("AlphaAlias", 0x3018),
                        named_import("Alpha", 0x3020)}});
    imports.push_back({"Forward.dll", {named_import("ViaName", 0x2000)}});

    const ImportPlan plan = plan_imports(imports, source);
    check_equal(plan.writes.size(), 5, "each resolved import has its own IAT write, including duplicates");
    check(plan.refusals.empty(), "resolved imports have no refusals");
    if (plan.writes.size() != 5) return;

    check_equal(plan.writes[0].iat_rva, 0x2000, "writes are sorted by IAT RVA across libraries");
    check_equal(plan.writes[1].iat_rva, 0x3008, "first direct IAT slot follows the lower slot");
    check_equal(plan.writes[4].iat_rva, 0x3020, "last direct IAT slot is preserved");
    check_pointer(plan.writes[1].address, reinterpret_cast<void*>(std::uintptr_t{0x10000100}),
                  "by-name export resolves to mapped base plus RVA");
    check(plan.writes[1].binding == rt::Binding::Module, "the source Binding is preserved");
    check_string(plan.writes[1].owning_module, "direct.dll", "the owning module is preserved");
    check_pointer(plan.writes[2].address, plan.writes[3].address, "by-name and by-ordinal alias resolve to one address");
    check_pointer(plan.writes[3].address, plan.writes[4].address, "two imports of one name are both retained");
    check_equal(source.queries.size(), 7, "the source is asked once per import and per forwarder hop");
    check_equal(static_cast<std::uint64_t>(std::count(source.queries.begin(), source.queries.end(), "Direct.dll!Alpha")),
                2, "duplicate imports are not deduplicated before resolution");

    const IatWrite& forwarded = plan.writes.front();
    check_pointer(forwarded.address, reinterpret_cast<void*>(std::uintptr_t{0x40000330}),
                  "forwarder chain reaches an ordinal export");
    check_string(forwarded.owning_module, "final.dll", "forwarder result names its final owning module");
    check(forwarded.forwarder_chain.size() == 3, "forwarder chain records each lookup including final ordinal");
    if (forwarded.forwarder_chain.size() == 3) {
        check_string(forwarded.forwarder_chain[0], "Forward.dll!ViaName", "forwarder chain begins at the import");
        check_string(forwarded.forwarder_chain[2], "final!#42", "forwarder chain records the ordinal target");
    }
}

void test_named_refusals() {
    using namespace rt::pe;
    FakeSource source;
    source.add_module("present.dll", 0x50000000u, {named_export("Present", 1, 0x100)});
    const std::vector<ImportLibrary> imports = {
        {"present.dll", {named_import("Absent", 0x1010), ordinal_import(99, 0x1008)}}};
    const ImportPlan plan = plan_imports(imports, source);
    check(plan.writes.empty(), "missing exports do not produce IAT writes");
    check_equal(plan.refusals.size(), 2, "missing name and ordinal each produce a named refusal");
    if (plan.refusals.size() != 2) return;
    check_equal(plan.refusals[0].iat_rva, 0x1008, "refusals are sorted by IAT RVA");
    check(plan.refusals[0].kind == RefusalKind::OrdinalWithoutExport, "missing ordinal has its distinct refusal kind");
    check_string(plan.refusals[0].describe(), "ordinal without export: present.dll!#99 [present.dll!#99]",
                 "ordinal refusal has a useful deterministic description");
    check_equal(plan.refusals[1].iat_rva, 0x1010, "named refusal follows the lower slot");
    check(plan.refusals[1].kind == RefusalKind::MissingExport, "missing named export has its refusal kind");
    check(std::string(refusal_name(plan.refusals[1].kind)) == "missing export", "refusal kind has a stable name");
}

void test_forwarder_cycle_and_depth() {
    using namespace rt::pe;
    {
        FakeSource source;
        source.add_module("cycle_a.dll", 0x60000000u, {named_export("Loop", 1, 0x100, "cycle_b.Next")});
        source.add_module("cycle_b.dll", 0x61000000u, {named_export("Next", 2, 0x100, "CYCLE_A.dll.Loop")});
        const std::vector<ImportLibrary> imports = {{"cycle_a.dll", {named_import("Loop", 0x1100)}}};
        const ImportPlan plan = plan_imports(imports, source);
        check(plan.writes.empty(), "cyclic forwarder does not produce an IAT write");
        check_equal(plan.refusals.size(), 1, "cyclic forwarder produces one refusal");
        if (plan.refusals.size() == 1) {
            check(plan.refusals[0].kind == RefusalKind::ForwarderLoop, "cycle is reported as a forwarder loop");
            check(plan.refusals[0].forwarder_chain.size() == 3, "cycle refusal records the repeated target");
        }
    }
    {
        FakeSource source;
        source.add_module("depth_a.dll", 0x62000000u, {named_export("Start", 1, 0x100, "depth_b.Next")});
        source.add_module("depth_b.dll", 0x63000000u, {named_export("Next", 2, 0x100, "depth_c.More")});
        source.add_module("depth_c.dll", 0x64000000u, {named_export("More", 3, 0x100, "depth_d.Done")});
        source.add_module("depth_d.dll", 0x65000000u, {named_export("Done", 4, 0x100)});
        const std::vector<ImportLibrary> imports = {{"depth_a.dll", {named_import("Start", 0x1200)}}};
        const ImportPlan plan = plan_imports(imports, source, PlanOptions{2});
        check(plan.writes.empty(), "forwarder chain beyond configured limit does not bind");
        check_equal(plan.refusals.size(), 1, "overlong forwarder chain produces one refusal");
        if (plan.refusals.size() == 1) {
            check(plan.refusals[0].kind == RefusalKind::ForwarderDepthExceeded,
                  "overlong chain is reported as depth exceeded");
            check(std::string(refusal_name(plan.refusals[0].kind)) == "forwarder depth exceeded",
                  "depth refusal has a stable name");
        }
    }
}

void test_invalid_forwarder() {
    using namespace rt::pe;
    FakeSource source;
    source.add_module("bad.dll", 0x66000000u, {named_export("Bad", 1, 0x100, "missing-separator")});
    const std::vector<ImportLibrary> imports = {{"bad.dll", {named_import("Bad", 0x1300)}}};
    const ImportPlan plan = plan_imports(imports, source);
    check(plan.writes.empty(), "malformed forwarder does not produce an IAT write");
    check_equal(plan.refusals.size(), 1, "malformed forwarder has one refusal");
    if (plan.refusals.size() == 1) {
        check(plan.refusals[0].kind == RefusalKind::InvalidForwarder, "malformed forwarder is separately identified");
        check_string(plan.refusals[0].detail, "missing-separator", "malformed forwarder text is retained");
    }
}

}  // namespace

int main() {
    test_import_planning_and_aliases();
    test_named_refusals();
    test_forwarder_cycle_and_depth();
    test_invalid_forwarder();
    if (failures) {
        std::fprintf(stderr, "pe_resolver_test: %d failure(s) across %d checks\n", failures, checks);
        return 1;
    }
    std::printf("pe_resolver_test: OK (%d checks)\n", checks);
    return 0;
}
