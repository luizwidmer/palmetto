// Pure import/export binding planner for the PE-loader rewrite.
//
// The planner consumes validated pe::Image descriptions and an adapter-provided SymbolSource. It does not load modules,
// touch process memory, call Win32, or write an IAT: the caller applies the returned writes after deciding how to handle
// the named refusals. See docs/pe_loader_rewrite.md.
#pragma once

#include "pe_image.h"
#include "pe_loader.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rt::pe {

// Number of forwarded-export transitions allowed by default. This matches the legacy loader's maximum of eight.
inline constexpr std::size_t kDefaultMaxForwarderDepth = 8;

// A symbol key is either an exact, case-sensitive export name or a 16-bit ordinal.
struct SymbolQuery {
    bool by_ordinal = false;
    std::uint16_t ordinal = 0;
    std::string name;

    static SymbolQuery named(std::string name);
    static SymbolQuery ordinal_symbol(std::uint16_t ordinal);
    std::string display_name() const;  // name, or "#<ordinal>"
};

// One answer from the adapter. An empty forwarder means address is final; a non-empty forwarder is an
// IMAGE_EXPORT_DIRECTORY re-export string ("module.symbol" or "module.#ordinal") for the planner to follow. `binding`
// uses the current Loader vocabulary, while `owning_module` names the module that supplied the final address.
struct SymbolResolution {
    void* address = nullptr;
    Binding binding = Binding::Unimplemented;
    std::string owning_module;
    std::string forwarder;
};

// Adapter boundary for shims, mapped/bundled PE modules and platform host modules. Resolve one library/symbol pair.
// Return nullopt when that provider has no such export. Implementations can use resolve_export() for parsed PE exports;
// when they encounter a forwarder, return its string and let the pure planner follow it through this same interface.
class SymbolSource {
public:
    virtual ~SymbolSource() = default;
    virtual std::optional<SymbolResolution> resolve(std::string_view library, const SymbolQuery& symbol) = 0;
};

// Find a named or ordinal ExportSymbol. Named aliases remain separate entries; aliases that share an ordinal/RVA resolve
// to the same address through resolve_export(). The input export order does not affect which named alias is selected.
const ExportSymbol* find_export(std::span<const ExportSymbol> exports, const SymbolQuery& symbol) noexcept;

// Turn one parsed PE export into an adapter answer. `image_base` is the mapped base; forwarders are returned as strings
// without converting their RVA to a pointer. Returns nullopt for a missing export or an unusable direct address.
std::optional<SymbolResolution> resolve_export(std::span<const ExportSymbol> exports, const SymbolQuery& symbol,
                                               void* image_base, Binding binding, std::string_view owning_module);

enum class RefusalKind {
    MissingExport,
    ForwarderLoop,
    ForwarderDepthExceeded,
    OrdinalWithoutExport,
    InvalidForwarder,
};

std::string_view refusal_name(RefusalKind kind) noexcept;

struct IatWrite {
    std::uint32_t iat_rva = 0;
    std::string library;
    std::string symbol;  // name or "#<ordinal>"
    void* address = nullptr;
    Binding binding = Binding::Unimplemented;
    std::string owning_module;
    std::vector<std::string> forwarder_chain;  // empty for direct bindings
};

struct ImportRefusal {
    std::uint32_t iat_rva = 0;
    std::string library;
    std::string symbol;  // name or "#<ordinal>"
    RefusalKind kind = RefusalKind::MissingExport;
    std::vector<std::string> forwarder_chain;  // attempted lookups, including the missing/cyclic target
    std::string detail;                         // populated for malformed forwarder strings

    std::string describe() const;
};

struct ImportPlan {
    std::vector<IatWrite> writes;
    std::vector<ImportRefusal> refusals;
};

struct PlanOptions {
    // Maximum number of re-export edges followed. A value of zero refuses every forwarder.
    std::size_t max_forwarder_depth = kDefaultMaxForwarderDepth;
};

// Plans every symbol from the parsed import libraries. Repeated imports are deliberately kept: every IAT slot receives
// its own write/refusal, even when another slot requests the same symbol. Results are stably ordered by IAT RVA so the
// plan is deterministic while preserving input order for the (normally invalid) case of equal RVAs.
ImportPlan plan_imports(const std::vector<ImportLibrary>& imports, SymbolSource& source,
                        PlanOptions options = {});

}  // namespace rt::pe
