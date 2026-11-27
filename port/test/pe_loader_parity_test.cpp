// Field-by-field parity between the legacy rt::Loader and the bounds-safe rt::pe::Image parser.
// The fixtures are constructed in pe_loader_fixture.h; this test uses no package/game files.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../src/pe_image.h"
#include "../src/pe_loader.h"
#include "pe_loader_fixture.h"

namespace {

namespace fs = std::filesystem;
namespace fixture = palmetto_pe_loader_fixture;

struct Row {
    std::string label;
    bool matched = false;
    std::string detail;
};

struct Discrepancy {
    std::string field;
    std::string legacy;
    std::string parser;
    std::string likely_wrong;
};

int g_failures = 0;
unsigned g_file_sequence = 0;
std::vector<Discrepancy> g_discrepancies;

void add_row(std::vector<Row>& rows, const char* label, bool matched, std::string detail) {
    rows.push_back({label, matched, std::move(detail)});
    const Row& row = rows.back();
    std::printf("  %-28s %s  %s\n", row.label.c_str(), matched ? "MATCH" : "MISMATCH", row.detail.c_str());
    if (!matched) ++g_failures;
}

void add_expected_discrepancy(const char* field, std::string legacy, std::string parser, std::string likely_wrong) {
    g_discrepancies.push_back({field, std::move(legacy), std::move(parser), std::move(likely_wrong)});
    const auto& d = g_discrepancies.back();
    std::printf("  %-28s DISCREPANCY  legacy: %s; parser: %s; likely wrong: %s\n", d.field.c_str(), d.legacy.c_str(),
                d.parser.c_str(), d.likely_wrong.c_str());
}

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("palmetto_pe_loader_parity_" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_);
        if (!fs::is_directory(path_)) throw std::runtime_error("cannot create the synthetic fixture directory");
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

fs::path write_image(const fs::path& dir, const std::string& name, const fixture::Bytes& bytes) {
    const fs::path path = dir / (std::to_string(++g_file_sequence) + "_" + name);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create synthetic fixture file");
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write synthetic fixture file");
    return path;
}

// Hold a real, committed allocation at a fresh preferred base. Loader::load must then use its
// relocation path, letting this test compare the original-file RVAs with the mapped legacy view.
class OccupiedPreferredBase {
public:
    OccupiedPreferredBase() {
        address_ = VirtualAlloc(nullptr, fixture::kImageSize, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
        if (!address_) throw std::runtime_error("VirtualAlloc could not reserve a synthetic preferred base");
        void* committed = VirtualAlloc(address_, fixture::kImageSize, MEM_COMMIT, PAGE_READWRITE);
        if (committed != address_) {
            VirtualFree(address_, 0, MEM_RELEASE);
            address_ = nullptr;
            throw std::runtime_error("VirtualAlloc could not commit the synthetic preferred base");
        }
    }
    ~OccupiedPreferredBase() {
        if (address_) VirtualFree(address_, 0, MEM_RELEASE);
    }
    OccupiedPreferredBase(const OccupiedPreferredBase&) = delete;
    OccupiedPreferredBase& operator=(const OccupiedPreferredBase&) = delete;
    std::uint64_t value() const { return reinterpret_cast<std::uint64_t>(address_); }

private:
    void* address_ = nullptr;
};

struct SectionShape {
    std::string name;
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_pointer = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t characteristics = 0;
    bool operator==(const SectionShape&) const = default;
};

std::vector<SectionShape> mapped_sections(const rt::Module& module) {
    std::vector<SectionShape> out;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    out.reserve(nt->FileHeader.NumberOfSections);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char name[9] = {};
        std::memcpy(name, section->Name, 8);
        out.push_back({name, section->VirtualAddress, section->Misc.VirtualSize, section->PointerToRawData,
                       section->SizeOfRawData, section->Characteristics});
    }
    return out;
}

std::vector<SectionShape> parsed_sections(const rt::pe::Image& image) {
    std::vector<SectionShape> out;
    out.reserve(image.sections().size());
    for (const auto& section : image.sections())
        out.push_back({section.name, section.virtual_address, section.virtual_size, section.raw_pointer, section.raw_size,
                       section.characteristics});
    return out;
}

struct ExportShape {
    bool is_forwarder = false;
    std::string forwarder;
    std::uint32_t rva = 0;
    bool operator==(const ExportShape&) const = default;
};

void insert_export(std::map<std::string, ExportShape>& out, const std::string& key, ExportShape value) {
    const auto [it, inserted] = out.emplace(key, value);
    if (!inserted && !(it->second == value))
        throw std::runtime_error("two synthetic export records assign different values to one key");
}

std::map<std::string, ExportShape> parsed_exports(const rt::pe::Image& image) {
    std::map<std::string, ExportShape> out;
    for (const auto& symbol : image.exports()) {
        const ExportShape value{symbol.is_forwarder(), symbol.forwarder, symbol.is_forwarder() ? 0u : symbol.rva};
        // Image exposes one record per named alias, with an ordinal on each. Materialize both
        // views so the map compares directly with Module::exports and Module::forwarders.
        if (symbol.by_name) insert_export(out, symbol.name, value);
        insert_export(out, "#" + std::to_string(symbol.ordinal), value);
    }
    return out;
}

std::map<std::string, ExportShape> legacy_exports(const rt::Module& module) {
    std::map<std::string, ExportShape> out;
    const auto base = reinterpret_cast<std::uintptr_t>(module.base);
    for (const auto& [key, address] : module.exports) {
        const auto value = reinterpret_cast<std::uintptr_t>(address);
        if (value < base || value - base >= module.size) throw std::runtime_error("legacy export pointer is outside image");
        insert_export(out, key, {false, {}, static_cast<std::uint32_t>(value - base)});
    }
    for (const auto& [key, forwarder] : module.forwarders) insert_export(out, key, {true, forwarder, 0});
    return out;
}

std::string format_export_keys(const std::map<std::string, ExportShape>& exports) {
    std::string out;
    for (const auto& [key, value] : exports) {
        if (!out.empty()) out += ", ";
        out += key;
        out += value.is_forwarder ? "=>" + value.forwarder : "@" + std::to_string(value.rva);
    }
    return out;
}

using ImportKey = std::pair<std::string, std::string>;
using RelocationKey = std::pair<std::uint16_t, std::uint32_t>;

std::multiset<ImportKey> parsed_imports(const rt::pe::Image& image) {
    std::multiset<ImportKey> out;
    for (const auto& library : image.imports()) {
        for (const auto& symbol : library.symbols) {
            const std::string name = symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name;
            out.emplace(rt::lower(library.name), name);
        }
    }
    return out;
}

std::multiset<ImportKey> legacy_imports(const rt::Module& module) {
    std::multiset<ImportKey> out;
    for (const auto& record : module.imports) out.emplace(rt::lower(record.dll), record.name);
    return out;
}

std::vector<RelocationKey> parsed_relocations(const rt::pe::Image& image) {
    std::vector<RelocationKey> out;
    out.reserve(image.relocations().size());
    for (const auto& relocation : image.relocations()) out.emplace_back(relocation.type, relocation.rva);
    return out;
}

std::vector<RelocationKey> mapped_relocations(const rt::Module& module) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    std::vector<RelocationKey> out;
    if (!directory.VirtualAddress || !directory.Size) return out;
    if (directory.VirtualAddress > module.size || directory.Size > module.size - directory.VirtualAddress)
        throw std::runtime_error("legacy relocation directory lies outside mapped image");
    std::uint32_t offset = 0;
    while (offset < directory.Size) {
        if (directory.Size - offset < sizeof(IMAGE_BASE_RELOCATION))
            throw std::runtime_error("legacy relocation block header is truncated");
        const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(module.base + directory.VirtualAddress + offset);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock > directory.Size - offset ||
            (block->SizeOfBlock & 1u))
            throw std::runtime_error("legacy relocation block has an invalid size");
        const auto* entries = reinterpret_cast<const std::uint16_t*>(block + 1);
        const std::size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(std::uint16_t);
        for (std::size_t i = 0; i < count; ++i)
            out.emplace_back(static_cast<std::uint16_t>(entries[i] >> 12),
                             block->VirtualAddress + (entries[i] & 0x0FFFu));
        offset += block->SizeOfBlock;
    }
    return out;
}

struct TlsShape {
    bool present = false;
    std::uint32_t template_rva = 0;
    std::uint32_t template_size = 0;
    std::uint32_t zero_fill = 0;
    std::uint32_t index_rva = 0;
    std::uint32_t callbacks_rva = 0;
    std::vector<std::uint32_t> callback_rvas;
    bool operator==(const TlsShape&) const = default;
};

std::uint32_t mapped_va_to_rva(const rt::Module& module, std::uint64_t va, const char* field) {
    if (!va) return 0;
    const auto base = reinterpret_cast<std::uint64_t>(module.base);
    if (va < base || va - base >= module.size)
        throw std::runtime_error(std::string("legacy TLS ") + field + " is outside mapped image");
    return static_cast<std::uint32_t>(va - base);
}

TlsShape mapped_tls(const rt::Module& module) {
    TlsShape out;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!directory.VirtualAddress || !directory.Size) return out;
    if (directory.Size < sizeof(IMAGE_TLS_DIRECTORY64) || directory.VirtualAddress > module.size ||
        sizeof(IMAGE_TLS_DIRECTORY64) > module.size - directory.VirtualAddress)
        throw std::runtime_error("legacy TLS directory lies outside mapped image");
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(module.base + directory.VirtualAddress);
    out.present = true;
    const auto start = tls->StartAddressOfRawData;
    const auto end = tls->EndAddressOfRawData;
    if (end < start) throw std::runtime_error("legacy TLS template ends before it starts");
    out.template_size = static_cast<std::uint32_t>(end - start);
    out.template_rva = mapped_va_to_rva(module, start, "template");
    (void)mapped_va_to_rva(module, end, "template end");
    out.zero_fill = tls->SizeOfZeroFill;
    out.index_rva = mapped_va_to_rva(module, tls->AddressOfIndex, "index");
    out.callbacks_rva = mapped_va_to_rva(module, tls->AddressOfCallBacks, "callback table");
    if (tls->AddressOfCallBacks) {
        const auto* callbacks = reinterpret_cast<const std::uint64_t*>(module.base + out.callbacks_rva);
        for (unsigned i = 0; i < 1024; ++i) {
            if (out.callbacks_rva > module.size || (i + 1) * sizeof(std::uint64_t) > module.size - out.callbacks_rva)
                throw std::runtime_error("legacy TLS callback table runs outside mapped image");
            if (!callbacks[i]) return out;
            out.callback_rvas.push_back(mapped_va_to_rva(module, callbacks[i], "callback"));
        }
        throw std::runtime_error("legacy TLS callback table has no terminator");
    }
    return out;
}

bool compare_fixture(const fs::path& dir, const std::string& filename, bool is_dll, bool with_tls) {
    std::printf("\n-- %s (%s, TLS %s) --\n", filename.c_str(), is_dll ? "DLL" : "EXE", with_tls ? "present" : "absent");
    OccupiedPreferredBase held_base;
    const fixture::Bytes bytes = fixture::make_image(held_base.value(), is_dll, with_tls);
    const rt::pe::Image parsed = rt::pe::Image::parse(bytes, filename);
    const fs::path path = write_image(dir, filename, bytes);
    std::vector<Row> rows;
    bool valid = true;

    try {
        rt::Loader loader(dir);
        rt::Module& module = loader.load(path);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
        const bool legacy_is_dll = (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;

        const bool mapped_elsewhere = module.base != reinterpret_cast<const std::uint8_t*>(parsed.image_base());
        const bool header_match = module.base && module.preferred_base == parsed.image_base() &&
                                  module.size == parsed.size_of_image() && module.entry_rva == parsed.entry_point() &&
                                  legacy_is_dll == parsed.is_dll() && mapped_elsewhere;
        add_row(rows, "preferred base/size/entry/DLL", header_match,
                "preferred=" + std::to_string(module.preferred_base) + " size=" + std::to_string(module.size) +
                    " entry=" + std::to_string(module.entry_rva) + " dll=" + (legacy_is_dll ? "true" : "false") +
                    " mapped_elsewhere=" + (mapped_elsewhere ? "yes" : "no"));
        valid = valid && header_match;

        const auto sections_old = mapped_sections(module);
        const auto sections_new = parsed_sections(parsed);
        const bool sections_match = sections_old == sections_new;
        std::string section_summary = std::to_string(sections_old.size()) + " sections";
        for (const auto& section : sections_new)
            section_summary += " [" + section.name + ": VA=" + std::to_string(section.virtual_address) +
                               " VS=" + std::to_string(section.virtual_size) + " raw=" +
                               std::to_string(section.raw_pointer) + "/" + std::to_string(section.raw_size) +
                               " chars=" + std::to_string(section.characteristics) + "]";
        add_row(rows, "section table", sections_match, std::move(section_summary));
        valid = valid && sections_match;

        const auto exports_old = legacy_exports(module);
        const auto exports_new = parsed_exports(parsed);
        const bool exports_match = exports_old == exports_new;
        add_row(rows, "exports/aliases/forwarders", exports_match, format_export_keys(exports_new));
        valid = valid && exports_match;

        const auto imports_old = legacy_imports(module);
        const auto imports_new = parsed_imports(parsed);
        bool iat_slots_valid = true;
        std::string import_summary;
        unsigned slot_count = 0;
        for (const auto& library : parsed.imports()) {
            for (const auto& symbol : library.symbols) {
                const std::string key = rt::lower(library.name) + "!" +
                                        (symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name);
                if (!import_summary.empty()) import_summary += ", ";
                import_summary += key + "@" + std::to_string(symbol.iat_rva);
                if (symbol.iat_rva > module.size || sizeof(std::uint64_t) > module.size - symbol.iat_rva) {
                    iat_slots_valid = false;
                    continue;
                }
                std::uint64_t bound = 0;
                std::memcpy(&bound, module.base + symbol.iat_rva, sizeof(bound));
                if (!bound) iat_slots_valid = false;
                ++slot_count;
            }
        }
        const bool imports_match = imports_old == imports_new && iat_slots_valid && slot_count == 2;
        add_row(rows, "imports + bound IAT slots", imports_match,
                import_summary + " (legacy records=" + std::to_string(imports_old.size()) +
                    ", nonzero slots=" + std::to_string(slot_count) + ")");
        valid = valid && imports_match;

        const auto relocations_old = mapped_relocations(module);
        const auto relocations_new = parsed_relocations(parsed);
        std::uint64_t relocated_pointer = 0;
        std::memcpy(&relocated_pointer, module.base + 0x1010, sizeof(relocated_pointer));
        const bool relocation_target_moved = relocated_pointer == reinterpret_cast<std::uint64_t>(module.base + 0x1020);
        const bool relocations_match = relocations_old == relocations_new && module.relocated && relocation_target_moved;
        add_row(rows, "relocations type + RVA", relocations_match,
                std::to_string(relocations_new.size()) + " entries; relocation target=" +
                    std::to_string(relocated_pointer) + (module.relocated ? " (rebased)" : " (not rebased)"));
        valid = valid && relocations_match;

        const TlsShape tls_old = mapped_tls(module);
        const auto& parsed_tls = parsed.tls();
        const TlsShape tls_new{parsed_tls.present, parsed_tls.template_rva, parsed_tls.template_size,
                               parsed_tls.zero_fill, parsed_tls.index_rva, parsed_tls.callbacks_rva,
                               parsed_tls.callback_rvas};
        const bool tls_shape_match = tls_old == tls_new;
        add_row(rows, "TLS RVAs/template/callbacks", tls_shape_match,
                tls_old.present ? "template=" + std::to_string(tls_old.template_rva) + "/" +
                                      std::to_string(tls_old.template_size) + " zero=" +
                                      std::to_string(tls_old.zero_fill) + " index=" +
                                      std::to_string(tls_old.index_rva) + " callbacks=" +
                                      std::to_string(tls_old.callback_rvas.size())
                                : "TLS absent");
        valid = valid && tls_shape_match;
        if (with_tls) {
            bool tls_runtime_match = module.tls_index != rt::kNoTlsIndex && module.tls_template &&
                                     module.tls_template == module.base + tls_new.template_rva &&
                                     module.tls_raw_size == tls_new.template_size &&
                                     module.tls_zero_fill == tls_new.zero_fill &&
                                     module.tls_block_size == tls_new.template_size + tls_new.zero_fill;
            std::uint32_t written_index = rt::kNoTlsIndex;
            if (tls_new.index_rva && tls_new.index_rva <= module.size - sizeof(written_index))
                std::memcpy(&written_index, module.base + tls_new.index_rva, sizeof(written_index));
            tls_runtime_match = tls_runtime_match && written_index == module.tls_index && module.tls_block;
            if (tls_runtime_match) {
                const auto* block = static_cast<const std::uint8_t*>(module.tls_block);
                tls_runtime_match = std::memcmp(block, module.base + tls_new.template_rva, tls_new.template_size) == 0;
                for (std::uint32_t i = 0; tls_runtime_match && i < tls_new.zero_fill; ++i)
                    tls_runtime_match = block[tls_new.template_size + i] == 0;
            }
            add_row(rows, "TLS installed template/index", tls_runtime_match,
                    "legacy index=" + std::to_string(module.tls_index) + " file index RVA=" +
                        std::to_string(tls_new.index_rva) + " template bytes=" + std::to_string(module.tls_raw_size) +
                        " zero-fill=" + std::to_string(module.tls_zero_fill));
            valid = valid && tls_runtime_match;
        } else {
            const bool no_legacy_tls = module.tls_index == rt::kNoTlsIndex && !module.tls_block;
            add_row(rows, "no TLS installation", no_legacy_tls, "legacy TLS index=" + std::to_string(module.tls_index));
            valid = valid && no_legacy_tls;
        }

        std::printf("  legacy mapped base=%p preferred=%p relocated=%s (actual base is old-loader-only state)\n",
                    static_cast<void*>(module.base), reinterpret_cast<void*>(static_cast<std::uintptr_t>(module.preferred_base)),
                    module.relocated ? "yes" : "no");
    } catch (const std::exception& e) {
        add_row(rows, "legacy Loader::load", false, e.what());
        valid = false;
    }

    std::printf("  %s: %zu comparison rows, %s\n", filename.c_str(), rows.size(), valid ? "PASS" : "FAIL");
    return valid;
}

void compare_empty_named_export(const fs::path& dir) {
    std::printf("\n-- existing legacy empty-export behavior --\n");
    OccupiedPreferredBase held_base;
    const auto bytes = fixture::make_empty_named_export_image(held_base.value());
    const auto path = write_image(dir, "empty_named_export.dll", bytes);
    bool parser_accepted = false;
    std::string parser_result;
    try {
        (void)rt::pe::Image::parse(bytes, "empty_named_export.dll");
        parser_accepted = true;
        parser_result = "accepted";
    } catch (const std::exception& e) {
        parser_result = e.what();
    }

    bool legacy_accepted = false;
    bool legacy_kept_empty_key_absent = false;
    std::string legacy_result;
    try {
        rt::Loader loader(dir);
        rt::Module& module = loader.load(path);
        legacy_accepted = true;
        legacy_kept_empty_key_absent = module.exports.count("Empty") == 0 && module.forwarders.count("Empty") == 0 &&
                                        module.exports.count("#7") == 0 && module.forwarders.count("#7") == 0;
        legacy_result = legacy_kept_empty_key_absent ? "accepted; empty name/ordinal omitted"
                                                    : "accepted; empty export was exposed";
    } catch (const std::exception& e) {
        legacy_result = e.what();
    }

    if (legacy_accepted && legacy_kept_empty_key_absent && !parser_accepted) {
        add_expected_discrepancy("empty named export slot", legacy_result, parser_result,
                                 "parser likely too strict: legacy loader intentionally omits a zero function RVA");
    } else {
        std::vector<Row> rows;
        add_row(rows, "empty named export slot", legacy_accepted == parser_accepted && legacy_kept_empty_key_absent,
                "legacy=" + legacy_result + "; parser=" + parser_result);
    }
}

}  // namespace

int main() {
    std::printf("pe_loader_parity_test: synthetic rt::Loader vs rt::pe::Image\n");
    std::printf("No game data; import names use one host API and one missing synthetic library.\n");
    std::printf("Mapped-only observations (actual base, binding pointers, TLS installation) are reported separately;\n"
                "legacy Loader::load returns a mapped/bound Module, not a file-only descriptor.\n");

    try {
        TempDir temp;
        bool all_match = true;
        all_match = compare_fixture(temp.path(), "parity.dll", /*is_dll=*/true, /*with_tls=*/true) && all_match;
        all_match = compare_fixture(temp.path(), "parity.exe", /*is_dll=*/false, /*with_tls=*/false) && all_match;
        std::printf("\ncore fixture comparisons: %s\n", all_match ? "PASS" : "FAIL");
        compare_empty_named_export(temp.path());
    } catch (const std::exception& e) {
        ++g_failures;
        std::printf("FAIL unexpected exception: %s\n", e.what());
    }

    std::printf("\n-- discrepancy summary --\n");
    if (g_discrepancies.empty()) {
        std::printf("  none observed\n");
    } else {
        for (const auto& discrepancy : g_discrepancies)
            std::printf("  %s | old: %s | parser: %s | likely wrong: %s\n", discrepancy.field.c_str(),
                        discrepancy.legacy.c_str(), discrepancy.parser.c_str(), discrepancy.likely_wrong.c_str());
    }
    std::printf("\npe_loader_parity_test: %s (%zu expected discrepancy/discrepancies, %d failure(s))\n",
                g_failures ? "FAILED" : "OK", g_discrepancies.size(), g_failures);
    return g_failures ? 1 : 0;
}
