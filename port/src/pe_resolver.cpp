#include "pe_resolver.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace rt::pe {
namespace {

std::string lookup_label(std::string_view library, const SymbolQuery& symbol) {
    std::string label(library);
    label += '!';
    label += symbol.display_name();
    return label;
}

std::string canonical_library(std::string_view library) {
    std::string key;
    key.reserve(library.size() + 4);
    for (const char ch : library) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        key.push_back(byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : ch);
    }
    // PE forwarders commonly omit the extension. Match the loader's module-name convention so "other.Func" and
    // "other.dll.Func" participate in the same cycle check.
    if (key.size() < 4 || key.compare(key.size() - 4, 4, ".dll") != 0) key += ".dll";
    return key;
}

struct VisitKey {
    std::string library;
    bool by_ordinal = false;
    std::uint16_t ordinal = 0;
    std::string name;
};

bool same_key(const VisitKey& a, const VisitKey& b) {
    return a.library == b.library && a.by_ordinal == b.by_ordinal &&
           (a.by_ordinal ? a.ordinal == b.ordinal : a.name == b.name);
}

VisitKey make_key(std::string_view library, const SymbolQuery& symbol) {
    VisitKey key;
    key.library = canonical_library(library);
    key.by_ordinal = symbol.by_ordinal;
    key.ordinal = symbol.ordinal;
    if (!symbol.by_ordinal) key.name = symbol.name;
    return key;
}

bool parse_forwarder(std::string_view forwarder, std::string& library, SymbolQuery& symbol) {
    const std::size_t separator = forwarder.rfind('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= forwarder.size()) return false;

    library.assign(forwarder.substr(0, separator));
    const std::string_view target = forwarder.substr(separator + 1);
    if (target.front() != '#') {
        symbol = SymbolQuery::named(std::string(target));
        return true;
    }
    if (target.size() == 1) return false;

    std::uint32_t ordinal = 0;
    for (std::size_t i = 1; i < target.size(); ++i) {
        const char digit = target[i];
        if (digit < '0' || digit > '9') return false;
        const std::uint32_t value = static_cast<std::uint32_t>(digit - '0');
        if (ordinal > (std::numeric_limits<std::uint16_t>::max() - value) / 10) return false;
        ordinal = ordinal * 10 + value;
    }
    symbol = SymbolQuery::ordinal_symbol(static_cast<std::uint16_t>(ordinal));
    return true;
}

struct WalkResult {
    std::optional<SymbolResolution> resolution;
    RefusalKind refusal = RefusalKind::MissingExport;
    std::vector<std::string> chain;
    std::string detail;
};

WalkResult walk(SymbolSource& source, std::string_view library, const SymbolQuery& symbol, std::size_t hops,
                std::size_t max_hops, std::vector<VisitKey> visited, std::vector<std::string> chain) {
    VisitKey key = make_key(library, symbol);
    const std::string label = lookup_label(library, symbol);
    if (std::find_if(visited.begin(), visited.end(), [&](const VisitKey& previous) { return same_key(previous, key); }) !=
        visited.end()) {
        chain.push_back(label);
        return {std::nullopt, RefusalKind::ForwarderLoop, std::move(chain), {}};
    }
    visited.push_back(std::move(key));

    std::optional<SymbolResolution> answer = source.resolve(library, symbol);
    if (!answer) {
        chain.push_back(label);
        const RefusalKind missing = symbol.by_ordinal ? RefusalKind::OrdinalWithoutExport : RefusalKind::MissingExport;
        return {std::nullopt, missing, std::move(chain), {}};
    }

    if (!answer->forwarder.empty()) {
        std::string next_library;
        SymbolQuery next_symbol;
        if (!parse_forwarder(answer->forwarder, next_library, next_symbol)) {
            chain.push_back(label);
            return {std::nullopt, RefusalKind::InvalidForwarder, std::move(chain), answer->forwarder};
        }
        if (hops >= max_hops) {
            chain.push_back(label);
            chain.push_back(lookup_label(next_library, next_symbol));
            return {std::nullopt, RefusalKind::ForwarderDepthExceeded, std::move(chain), {}};
        }
        chain.push_back(label);
        return walk(source, next_library, next_symbol, hops + 1, max_hops, std::move(visited), std::move(chain));
    }

    if (!answer->address) {
        chain.push_back(label);
        const RefusalKind missing = symbol.by_ordinal ? RefusalKind::OrdinalWithoutExport : RefusalKind::MissingExport;
        return {std::nullopt, missing, std::move(chain), {}};
    }

    // Direct bindings have no forwarding trace. For a re-export chain, retain the importer-facing lookup and every
    // target lookup so diagnostics can show exactly how the final owner was reached.
    if (hops != 0) chain.push_back(label);
    else chain.clear();
    return {std::move(answer), RefusalKind::MissingExport, std::move(chain), {}};
}

bool write_less(const IatWrite& a, const IatWrite& b) { return a.iat_rva < b.iat_rva; }
bool refusal_less(const ImportRefusal& a, const ImportRefusal& b) { return a.iat_rva < b.iat_rva; }

}  // namespace

SymbolQuery SymbolQuery::named(std::string value) {
    SymbolQuery query;
    query.name = std::move(value);
    return query;
}

SymbolQuery SymbolQuery::ordinal_symbol(std::uint16_t value) {
    SymbolQuery query;
    query.by_ordinal = true;
    query.ordinal = value;
    return query;
}

std::string SymbolQuery::display_name() const {
    return by_ordinal ? "#" + std::to_string(ordinal) : name;
}

const ExportSymbol* find_export(std::span<const ExportSymbol> exports, const SymbolQuery& symbol) noexcept {
    const ExportSymbol* found = nullptr;
    for (const ExportSymbol& candidate : exports) {
        const bool matches = symbol.by_ordinal ? candidate.ordinal == symbol.ordinal
                                               : (candidate.by_name && candidate.name == symbol.name);
        if (!matches || candidate.rva == 0) continue;
        // A parsed PE table produces identical RVA/forwarder data for aliases sharing an ordinal. The explicit tie
        // breaker also makes this helper deterministic for synthetic or hand-built descriptions.
        if (!found || std::tie(candidate.rva, candidate.forwarder, candidate.name, candidate.by_name) <
                          std::tie(found->rva, found->forwarder, found->name, found->by_name)) {
            found = &candidate;
        }
    }
    return found;
}

std::optional<SymbolResolution> resolve_export(std::span<const ExportSymbol> exports, const SymbolQuery& symbol,
                                              void* image_base, Binding binding, std::string_view owning_module) {
    const ExportSymbol* exported = find_export(exports, symbol);
    if (!exported) return std::nullopt;

    SymbolResolution resolution;
    resolution.binding = binding;
    resolution.owning_module.assign(owning_module);
    if (exported->is_forwarder()) {
        resolution.forwarder = exported->forwarder;
        return resolution;
    }

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(image_base);
    if (!image_base || exported->rva > std::numeric_limits<std::uintptr_t>::max() - base) return std::nullopt;
    resolution.address = reinterpret_cast<void*>(base + exported->rva);
    return resolution;
}

std::string_view refusal_name(RefusalKind kind) noexcept {
    switch (kind) {
        case RefusalKind::MissingExport: return "missing export";
        case RefusalKind::ForwarderLoop: return "forwarder loop";
        case RefusalKind::ForwarderDepthExceeded: return "forwarder depth exceeded";
        case RefusalKind::OrdinalWithoutExport: return "ordinal without export";
        case RefusalKind::InvalidForwarder: return "invalid forwarder";
    }
    return "unknown refusal";
}

std::string ImportRefusal::describe() const {
    std::string message(refusal_name(kind));
    message += ": ";
    message += library;
    message += '!';
    message += symbol;
    if (!detail.empty()) {
        message += " (";
        message += detail;
        message += ')';
    }
    if (!forwarder_chain.empty()) {
        message += " [";
        for (std::size_t i = 0; i < forwarder_chain.size(); ++i) {
            if (i) message += " -> ";
            message += forwarder_chain[i];
        }
        message += ']';
    }
    return message;
}

ImportPlan plan_imports(const std::vector<ImportLibrary>& imports, SymbolSource& source, PlanOptions options) {
    ImportPlan plan;
    for (const ImportLibrary& library : imports) {
        for (const ImportSymbol& imported : library.symbols) {
            const SymbolQuery query = imported.by_ordinal ? SymbolQuery::ordinal_symbol(imported.ordinal)
                                                          : SymbolQuery::named(imported.name);
            WalkResult resolved = walk(source, library.name, query, 0, options.max_forwarder_depth, {}, {});
            if (resolved.resolution) {
                plan.writes.push_back({imported.iat_rva,
                                       library.name,
                                       query.display_name(),
                                       resolved.resolution->address,
                                       resolved.resolution->binding,
                                       std::move(resolved.resolution->owning_module),
                                       std::move(resolved.chain)});
            } else {
                plan.refusals.push_back({imported.iat_rva,
                                         library.name,
                                         query.display_name(),
                                         resolved.refusal,
                                         std::move(resolved.chain),
                                         std::move(resolved.detail)});
            }
        }
    }

    std::stable_sort(plan.writes.begin(), plan.writes.end(), write_less);
    std::stable_sort(plan.refusals.begin(), plan.refusals.end(), refusal_less);
    return plan;
}

}  // namespace rt::pe
