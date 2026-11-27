// Deterministic, CPU-only mutation sweep for the PE32+ parsing core. Every fixture is synthetic.
#include "pe_image.h"
#include "pe_image_fixture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

std::size_t g_cases = 0;
std::size_t g_errors = 0;
std::size_t g_failures = 0;

bool invariant_failure(const std::string& label, const std::string& what) {
    std::fprintf(stderr, "FAIL %s: %s\n", label.c_str(), what.c_str());
    ++g_failures;
    return false;
}

bool check_invariants(const rt::pe::Image& image, const std::string& label) {
    const std::uint32_t image_size = image.size_of_image();
    if (!image_size) return invariant_failure(label, "successful parse has zero SizeOfImage");
    if (!image.rva_in_image(0, image.size_of_headers()))
        return invariant_failure(label, "headers are outside the image");
    if (image.sections().size() > 96) return invariant_failure(label, "section count exceeds the parser bound");

    for (std::size_t i = 0; i < image.sections().size(); ++i) {
        const auto& section = image.sections()[i];
        const std::uint64_t span = std::max<std::uint64_t>(section.virtual_size, section.mapped_raw_size());
        if (!span || !image.rva_in_image(section.virtual_address, span))
            return invariant_failure(label, "a section range is outside the image");
        const auto data = image.section_data(section);
        if (data.size() != section.mapped_raw_size())
            return invariant_failure(label, "section_data does not match the validated mapped raw size");
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = image.sections()[j];
            const std::uint64_t a0 = section.virtual_address;
            const std::uint64_t a1 = a0 + span;
            const std::uint64_t b0 = other.virtual_address;
            const std::uint64_t b1 = b0 + std::max<std::uint64_t>(other.virtual_size, other.mapped_raw_size());
            if (a0 < b1 && b0 < a1) return invariant_failure(label, "two accepted sections overlap");
        }
    }

    for (unsigned i = 0; i < rt::pe::Image::kDirectoryCount; ++i) {
        const auto& directory = image.directory(i);
        if (directory.present() && !image.rva_in_image(directory.rva, directory.size))
            return invariant_failure(label, "a present data directory is outside the image");
    }

    const auto& export_directory = image.directory(rt::pe::Image::kDirectoryExport);
    const std::uint64_t export_end = static_cast<std::uint64_t>(export_directory.rva) + export_directory.size;
    for (const auto& symbol : image.exports()) {
        if (!symbol.rva || !image.rva_in_image(symbol.rva, 1))
            return invariant_failure(label, "an emitted export RVA is outside the image");
        if (symbol.is_forwarder() &&
            (!export_directory.present() || symbol.rva < export_directory.rva ||
             static_cast<std::uint64_t>(symbol.rva) + symbol.forwarder.size() + 1 > export_end))
            return invariant_failure(label, "an accepted forwarder is not wholly inside the export directory");
    }

    if (image.imports().size() > 4096) return invariant_failure(label, "import library count exceeds the parser bound");
    std::size_t import_symbols = 0;
    for (const auto& library : image.imports()) {
        for (const auto& symbol : library.symbols) {
            ++import_symbols;
            if (!image.rva_in_image(symbol.iat_rva, 8))
                return invariant_failure(label, "an import IAT slot is outside the image");
        }
    }
    if (import_symbols > (1u << 16)) return invariant_failure(label, "import symbol count exceeds the parser bound");

    for (const auto& relocation : image.relocations()) {
        std::uint64_t width = 1;
        if (relocation.type == 1 || relocation.type == 2) width = 2;
        else if (relocation.type == rt::pe::Relocation::kHighLow) width = 4;
        else if (relocation.type == rt::pe::Relocation::kDir64) width = 8;
        if (!image.rva_in_image(relocation.rva, width))
            return invariant_failure(label, "a relocation target is outside the image");
    }

    const auto& tls = image.tls();
    if (tls.present) {
        if (tls.template_size && !image.rva_in_image(tls.template_rva, tls.template_size))
            return invariant_failure(label, "the TLS template is outside the image");
        if (tls.index_rva && !image.rva_in_image(tls.index_rva, 4))
            return invariant_failure(label, "the TLS index is outside the image");
        if (tls.callbacks_rva && !image.rva_in_image(tls.callbacks_rva, 8))
            return invariant_failure(label, "the TLS callback table is outside the image");
        for (const std::uint32_t callback : tls.callback_rvas)
            if (!image.rva_in_image(callback, 1)) return invariant_failure(label, "a TLS callback RVA is outside the image");
    }

    const auto& exceptions = image.exceptions();
    if (exceptions.present &&
        (!exceptions.size || exceptions.size % 12 || !image.rva_in_image(exceptions.rva, exceptions.size) ||
         exceptions.entry_count != exceptions.size / 12))
        return invariant_failure(label, "exception directory invariants do not hold");
    return true;
}

bool parse_or_error(const Bytes& bytes, const std::string& label) {
    ++g_cases;
    try {
        const auto image = rt::pe::Image::parse(bytes, "mutation.dll");
        return check_invariants(image, label);
    } catch (const rt::pe::Error&) {
        ++g_errors;
        return true;
    } catch (const std::exception& error) {
        return invariant_failure(label, std::string("unexpected non-PE exception: ") + error.what());
    } catch (...) {
        return invariant_failure(label, "unexpected non-standard exception");
    }
}

std::uint32_t next_random(std::uint32_t& state) {
    // xorshift32: fixed state and transition, independent of library RNG implementations.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

Bytes truncate_fixture(const Bytes& valid, std::size_t length) {
    Bytes bytes = valid;
    bytes.resize(length);
    if (length < pe_image_fixture::Builder::kSections + 80) return bytes;  // no complete section table yet
    const std::size_t text_bytes = length > 0x400 ? std::min<std::size_t>(0x200, length - 0x400) : 0;
    const std::size_t rdata_bytes = length > 0x600 ? std::min<std::size_t>(0x1000, length - 0x600) : 0;
    const auto put_u32 = [&bytes](std::size_t offset, std::uint32_t value) {
        if (offset + sizeof(value) <= bytes.size()) std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    // Shrink declared raw spans to the bytes retained so truncations can reach each table reader rather than always
    // stopping at the section-raw-file check. This is a second deterministic mutation in each truncate case.
    put_u32(pe_image_fixture::Builder::kSections + 16, static_cast<std::uint32_t>(text_bytes));
    put_u32(pe_image_fixture::Builder::kSections + 40 + 16, static_cast<std::uint32_t>(rdata_bytes));
    return bytes;
}

void run_truncations(const Bytes& valid) {
    // Every length in each window is tested, including exact boundaries and one-byte-short/one-byte-long cuts.
    const std::array<std::pair<std::size_t, std::size_t>, 10> windows = {{
        {0x00, 0x40},       // DOS header and e_lfanew
        {0x70, 0x1E0},      // NT, optional header, and both section headers
        {0x3F0, 0x610},     // headers -> .text/.rdata raw boundary
        {0x630, 0x680},     // export directory header/tables
        {0x6F0, 0x7D0},     // import descriptors and thunk/name tables
        {0x7F8, 0x820},     // relocation block
        {0x9F0, 0xA40},     // exception directory
        {0xB10, 0xB50},     // TLS callback table
        {0xCF0, 0xD30},     // TLS template
        {0x15F0, 0x1600},   // last bytes of the synthetic file
    }};
    for (std::size_t window = 0; window < windows.size(); ++window) {
        const auto [first, last] = windows[window];
        const std::size_t capped_last = std::min(last, valid.size());
        for (std::size_t length = first; length <= capped_last; ++length) {
            char label[96];
            std::snprintf(label, sizeof(label), "truncate[%zu] length=0x%zX", window, length);
            if (!parse_or_error(truncate_fixture(valid, length), label)) return;
        }
    }
}

void run_single_bit_mutations(const Bytes& valid) {
    for (std::size_t offset = 0; offset < valid.size(); ++offset) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            Bytes bytes = valid;
            bytes[offset] ^= static_cast<std::uint8_t>(1u << bit);
            char label[96];
            std::snprintf(label, sizeof(label), "flip-bit offset=0x%zX bit=%u", offset, bit);
            if (!parse_or_error(bytes, label)) return;
        }
    }
}

void run_fixed_seed_mutations(const Bytes& valid) {
    std::uint32_t state = 0x5045494Du;  // "PEIM"
    for (unsigned iteration = 0; iteration < 2048; ++iteration) {
        Bytes bytes = valid;
        const unsigned mutations = 1 + next_random(state) % 4;
        for (unsigned i = 0; i < mutations; ++i) {
            const std::size_t offset = next_random(state) % bytes.size();
            bytes[offset] = static_cast<std::uint8_t>(next_random(state) >> 24);
        }
        char label[96];
        std::snprintf(label, sizeof(label), "seed=0x5045494D iteration=%u", iteration);
        if (!parse_or_error(bytes, label)) return;
    }
}

}  // namespace

int main() {
    const pe_image_fixture::Builder builder;
    if (!parse_or_error(builder.bytes, "unmodified synthetic image")) return 1;
    run_truncations(builder.bytes);
    run_single_bit_mutations(builder.bytes);
    run_fixed_seed_mutations(builder.bytes);
    if (g_failures) {
        std::fprintf(stderr, "pe_image_fuzz_test: %zu failure(s) after %zu cases (%zu PE errors)\n", g_failures,
                     g_cases, g_errors);
        return 1;
    }
    std::printf("pe_image_fuzz_test: OK (%zu deterministic cases, %zu PE errors, seed 0x5045494D)\n", g_cases,
                g_errors);
    return 0;
}
