// Pure wiring test for the opt-in switch registry (docs/opt_in_switches.md).
//
// It re-derives the set of PALMETTO_* environment switches from the source under port/src and port/shims and
// fails when that set differs from rt::opt_in_switches::kSwitches in either direction. It also re-derives
// whether every recorded call site is include-reachable from a compiled port translation unit (the table's
// `wired` column), checks that every recorded site file still spells its switch, and exercises the parser
// semantics of every kind for the values unset, empty, "0", "1" and invalid text.
//
// Synthetic only: no build, no boot, no GPU, no game data, and it sets no PALMETTO_* variable. The source tree
// is located from PALMETTO_REPO_ROOT (set by port/CMakeLists.txt), then PALMETTO_SOURCE_ROOT, then by walking up
// from the working directory and from this file. A run that cannot locate port/src fails with a named message
// instead of reporting a green result from nothing.

#include "../src/opt_in_switches.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace rt::opt_in_switches;

const char* current_case = "startup";
int checks = 0;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", current_case, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

// ---- source tree -------------------------------------------------------------------------------------------------

bool looks_like_repo(const fs::path& candidate) {
    std::error_code ec;
    return fs::is_regular_file(candidate / "port" / "src" / "opt_in_switches.h", ec) &&
           fs::is_directory(candidate / "port" / "shims", ec);
}

fs::path search_upwards(fs::path start) {
    std::error_code ec;
    if (start.empty()) return {};
    start = fs::weakly_canonical(start, ec);
    if (ec) return {};
    for (int depth = 0; depth < 8 && !start.empty(); ++depth) {
        if (looks_like_repo(start)) return start;
        if (!start.has_parent_path() || start.parent_path() == start) break;
        start = start.parent_path();
    }
    return {};
}

fs::path find_repo_root() {
    std::error_code ec;
#if defined(PALMETTO_REPO_ROOT)
    if (fs::path found = search_upwards(PALMETTO_REPO_ROOT); !found.empty()) return found;
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // std::getenv is the tree's switch idiom; the test reads one path
#endif
    if (const char* from_environment = std::getenv("PALMETTO_SOURCE_ROOT"); from_environment && *from_environment) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        if (fs::path found = search_upwards(from_environment); !found.empty()) return found;
    }
    if (fs::path found = search_upwards(fs::current_path(ec)); !found.empty()) return found;
    if (fs::path found = search_upwards(__FILE__); !found.empty()) return found;
    return {};
}

void collect_sources(const fs::path& root, std::vector<fs::path>& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string extension = it->path().extension().string();
        if (extension == ".h" || extension == ".cpp" || extension == ".c") out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
}

// Every string literal that is exactly PALMETTO_<A-Z0-9_>. Escaped characters make the literal something else,
// so a message like "PALMETTO_X=1 turns it on" is not a switch name. Wide literals (L"...") are matched too.
void add_names_from_file(const fs::path& file, std::set<std::string>& out) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return;
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string content;
        bool escaped = false;
        std::size_t j = i + 1;
        for (; j < text.size(); ++j) {
            if (text[j] == '\\') { escaped = true; ++j; continue; }
            if (text[j] == '"') break;
            content.push_back(text[j]);
        }
        i = j;
        if (escaped || content.rfind("PALMETTO_", 0) != 0 || content.size() <= 9) continue;
        bool name_like = true;
        for (const char c : content)
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) name_like = false;
        if (name_like) out.insert(content);
    }
}

// ---- include reachability ----------------------------------------------------------------------------------------

std::vector<std::string> parse_includes(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::vector<std::string> includes;
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t hash = line.find('#');
        if (hash == std::string::npos) continue;
        const std::size_t include = line.find("include", hash);
        if (include == std::string::npos) continue;
        const std::size_t quote = line.find('"', include);
        if (quote == std::string::npos) continue;
        const std::size_t close = line.find('"', quote + 1);
        if (close == std::string::npos) continue;
        includes.push_back(line.substr(quote + 1, close - quote - 1));
    }
    return includes;
}

std::string key_of(const fs::path& file) {
    std::error_code ec;
    const fs::path normalized = fs::weakly_canonical(file, ec);
    return (ec ? file : normalized).generic_string();
}

// The set of scanned files that a compiled translation unit (a .cpp/.c under port/src or port/shims) reaches
// through #include. Header-only slices that only a test includes are not in this set.
std::set<std::string> include_reachable(const fs::path& repo, const std::vector<fs::path>& sources) {
    const fs::path roots[] = {repo / "port" / "src", repo / "port" / "shims"};
    std::map<std::string, std::vector<std::string>> graph;
    std::vector<fs::path> compiled;
    for (const fs::path& root : roots) {
        std::vector<fs::path> files;
        collect_sources(root, files);
        for (const fs::path& file : files) {
            const std::string key = key_of(file);
            std::vector<std::string>& edges = graph[key];
            for (const std::string& include : parse_includes(file)) {
                const fs::path candidates[] = {file.parent_path() / include, repo / "port" / "src" / include,
                                               repo / "port" / "shims" / include};
                std::error_code ec;
                for (const fs::path& candidate : candidates) {
                    if (fs::is_regular_file(candidate, ec)) {
                        edges.push_back(key_of(candidate));
                        break;
                    }
                }
            }
            const std::string extension = file.extension().string();
            if (extension == ".cpp" || extension == ".c") compiled.push_back(file);
        }
    }
    std::set<std::string> reached;
    std::vector<std::string> stack;
    for (const fs::path& file : compiled) stack.push_back(key_of(file));
    while (!stack.empty()) {
        const std::string file = stack.back();
        stack.pop_back();
        if (!reached.insert(file).second) continue;
        const auto edges = graph.find(file);
        if (edges == graph.end()) continue;
        for (const std::string& next : edges->second) stack.push_back(next);
    }
    (void)sources;
    return reached;
}

// ---- cases -------------------------------------------------------------------------------------------------------

int parser_semantics_case() {
    current_case = "parser semantics";
    // exact "1": no alias, no whitespace, no truthy spelling.
    CHECK(!flag_parse(Kind::exact_one, nullptr));
    CHECK(!flag_parse(Kind::exact_one, ""));
    CHECK(!flag_parse(Kind::exact_one, "0"));
    CHECK(!flag_parse(Kind::exact_one, "00"));
    CHECK(!flag_parse(Kind::exact_one, "01"));
    CHECK(!flag_parse(Kind::exact_one, "true"));
    CHECK(!flag_parse(Kind::exact_one, " 1"));
    CHECK(!flag_parse(Kind::exact_one, "1 "));
    CHECK(flag_parse(Kind::exact_one, "1"));
    // first character not '0': the tree's switch_on/read_flag idiom.
    CHECK(!flag_parse(Kind::switch_on, nullptr));
    CHECK(!flag_parse(Kind::switch_on, ""));
    CHECK(!flag_parse(Kind::switch_on, "0"));
    CHECK(!flag_parse(Kind::switch_on, "00"));
    CHECK(!flag_parse(Kind::switch_on, "01"));
    CHECK(flag_parse(Kind::switch_on, "1"));
    CHECK(flag_parse(Kind::switch_on, "2"));
    CHECK(flag_parse(Kind::switch_on, "false"));
    // text switch: only the three documented off spellings are off.
    CHECK(!flag_parse(Kind::text_switch, nullptr));
    CHECK(!flag_parse(Kind::text_switch, ""));
    CHECK(!flag_parse(Kind::text_switch, "0"));
    CHECK(!flag_parse(Kind::text_switch, "off"));
    CHECK(!flag_parse(Kind::text_switch, "OFF"));
    CHECK(!flag_parse(Kind::text_switch, "no"));
    CHECK(!flag_parse(Kind::text_switch, "No"));
    CHECK(flag_parse(Kind::text_switch, "1"));
    CHECK(flag_parse(Kind::text_switch, "yes"));
    CHECK(flag_parse(Kind::text_switch, "true"));
    // any non-null value: an empty value is ON (the documented inconsistency vs the other flag idioms).
    CHECK(!flag_parse(Kind::any_set, nullptr));
    CHECK(flag_parse(Kind::any_set, ""));
    CHECK(flag_parse(Kind::any_set, "0"));
    CHECK(flag_parse(Kind::any_set, "1"));
    // value switches are consulted, never read as flags.
    CHECK(!flag_parse(Kind::value_int, nullptr));
    CHECK(!flag_parse(Kind::value_int, ""));
    CHECK(flag_parse(Kind::value_int, "0"));
    CHECK(flag_parse(Kind::value_int, "garbage"));
    CHECK(!flag_parse(Kind::value_string, nullptr));
    CHECK(!flag_parse(Kind::value_string, ""));
    CHECK(flag_parse(Kind::value_string, "/tmp/out"));
    // default-on switch: unset and empty keep the documented default, "0"/off/no do not.
    CHECK(flag_parse(Kind::disable_value, nullptr));
    CHECK(flag_parse(Kind::disable_value, ""));
    CHECK(flag_parse(Kind::disable_value, "1"));
    CHECK(!flag_parse(Kind::disable_value, "0"));
    CHECK(!flag_parse(Kind::disable_value, "off"));
    CHECK(!flag_parse(Kind::disable_value, "no"));
    // decimal reader: the fallback on unset/empty/invalid, the value otherwise (strtoll skips leading blanks).
    CHECK(integer_or(nullptr, 7) == 7);
    CHECK(integer_or("", 7) == 7);
    CHECK(integer_or("12", 7) == 12);
    CHECK(integer_or("0", 7) == 0);
    CHECK(integer_or("-3", 7) == -3);
    CHECK(integer_or("12x", 7) == 7);
    CHECK(integer_or("x12", 7) == 7);
    CHECK(integer_or("12 ", 7) == 7);
    CHECK(integer_or(" 5", 7) == 5);
    return 0;
}

int table_shape_case() {
    current_case = "table shape";
    CHECK(kSwitchCount == 303);
    CHECK(kNotSwitchCount == 1);
    CHECK(kScanRootCount == 2);
    std::set<std::string> names;
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        const Switch& row = kSwitches[i];
        CHECK(row.name != nullptr && std::strncmp(row.name, "PALMETTO_", 9) == 0);
        CHECK(std::strlen(row.name) > 9);
        CHECK(row.site != nullptr && std::strlen(row.site) > 0);
        CHECK(std::strchr(row.site, ':') != nullptr);
        CHECK(names.insert(row.name).second);
        if (i > 0) CHECK(std::strcmp(kSwitches[i - 1].name, row.name) < 0);
        // the kind enum is closed: every row maps to a named kind.
        const char* const name = kind_name(row.kind);
        CHECK(std::strcmp(name, "unknown") != 0);
    }
    return 0;
}

int kind_census_case() {
    current_case = "kind census";
    std::size_t counts[7] = {0, 0, 0, 0, 0, 0, 0};
    std::size_t wired = 0;
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        counts[static_cast<std::size_t>(kSwitches[i].kind)] += 1;
        if (kSwitches[i].wired) ++wired;
    }
    CHECK(counts[static_cast<std::size_t>(Kind::exact_one)] == 33);
    CHECK(counts[static_cast<std::size_t>(Kind::switch_on)] == 89);
    CHECK(counts[static_cast<std::size_t>(Kind::text_switch)] == 6);
    CHECK(counts[static_cast<std::size_t>(Kind::any_set)] == 83);
    CHECK(counts[static_cast<std::size_t>(Kind::value_int)] == 50);
    CHECK(counts[static_cast<std::size_t>(Kind::value_string)] == 39);
    CHECK(counts[static_cast<std::size_t>(Kind::disable_value)] == 3);
    CHECK(counts[0] + counts[1] + counts[2] + counts[3] + counts[4] + counts[5] + counts[6] == kSwitchCount);
    CHECK(wired == 301);
    return 0;
}

int default_path_case() {
    current_case = "default path";
    // Unset behaviour is the byte-identical contract: every flag kind is off when unset, and both value kinds
    // are not consulted (the caller keeps its legacy value). Only the three documented default-on switches are on.
    // An EMPTY value is different for any_set: that idiom counts a present-but-empty variable as ON, which is the
    // one flag family where an empty environment value changes the default path (reported in docs/opt_in_switches.md).
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        const Switch& row = kSwitches[i];
        if (row.kind == Kind::disable_value) {
            CHECK(flag_parse(row.kind, nullptr));
            CHECK(flag_parse(row.kind, ""));
            continue;
        }
        CHECK(!flag_parse(row.kind, nullptr));
        if (row.kind == Kind::any_set) {
            CHECK(flag_parse(row.kind, ""));
            continue;
        }
        CHECK(!flag_parse(row.kind, ""));
    }
    return 0;
}

int source_scan_case(const fs::path& repo) {
    current_case = "source scan";
    std::vector<fs::path> files;
    for (std::size_t i = 0; i < kScanRootCount; ++i) collect_sources(repo / kScanRoots[i], files);
    CHECK(!files.empty());
    std::set<std::string> scanned;
    for (const fs::path& file : files) add_names_from_file(file, scanned);
    for (std::size_t i = 0; i < kNotSwitchCount; ++i) scanned.erase(kNotSwitches[i]);
    std::set<std::string> table;
    for (std::size_t i = 0; i < kSwitchCount; ++i) table.insert(kSwitches[i].name);
    CHECK(scanned.size() == table.size());
    std::size_t reported = 0;
    for (const std::string& name : scanned) {
        if (table.count(name) == 0) {
            std::fprintf(stderr, "MISSING from opt_in_switches.h: %s%s\n", name.c_str(),
                         reported++ >= 8 ? " (further rows not printed)" : "");
            CHECK(false);
        }
    }
    for (const std::string& name : table) {
        if (scanned.count(name) == 0) {
            std::fprintf(stderr, "STALE table row (no PALMETTO_ literal in port/src or port/shims): %s%s\n",
                         name.c_str(), reported++ >= 8 ? " (further rows not printed)" : "");
            CHECK(false);
        }
    }
    return 0;
}

int site_case(const fs::path& repo) {
    current_case = "recorded sites";
    std::size_t missing = 0;
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        const Switch& row = kSwitches[i];
        const std::string site = row.site;
        const std::size_t colon = site.rfind(':');
        CHECK(colon != std::string::npos);
        const fs::path file = repo / site.substr(0, colon);
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            std::fprintf(stderr, "site file gone: %s (%s)\n", site.c_str(), row.name);
            ++missing;
            continue;
        }
        std::ifstream stream(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        if (text.find(row.name) == std::string::npos) {
            std::fprintf(stderr, "site no longer names the switch: %s (%s)\n", site.c_str(), row.name);
            ++missing;
        }
    }
    CHECK(missing == 0);
    return 0;
}

int wiring_case(const fs::path& repo) {
    current_case = "include reachability";
    std::vector<fs::path> sources;
    const std::set<std::string> reached = include_reachable(repo, sources);
    CHECK(!reached.empty());
    std::size_t mismatched = 0;
    for (std::size_t i = 0; i < kSwitchCount; ++i) {
        const Switch& row = kSwitches[i];
        const std::string site = row.site;
        const fs::path file = repo / site.substr(0, site.rfind(':'));
        const bool reachable = reached.count(key_of(file)) != 0;
        if (reachable != row.wired) {
            std::fprintf(stderr, "wired mismatch: %s site %s table=%s source=%s\n", row.name, row.site,
                         row.wired ? "true" : "false", reachable ? "true" : "false");
            ++mismatched;
        }
    }
    CHECK(mismatched == 0);
    // The two verified unwired rows are the remaining deliberate pure slices (round 21 wired query-heap
    // readiness at CreateQueryHeap): their parser is compiled and tested but no runtime call site includes them.
    std::size_t unwired = 0;
    for (std::size_t i = 0; i < kSwitchCount; ++i) unwired += kSwitches[i].wired ? 0 : 1;
    CHECK(unwired == 2);
    return 0;
}

}  // namespace

int main() {
    const fs::path repo = find_repo_root();
    if (repo.empty()) {
        std::fprintf(stderr,
                     "REFUSED [source tree] cannot locate port/src; pass -DPALMETTO_REPO_ROOT=<checkout> or set "
                     "PALMETTO_SOURCE_ROOT (docs/opt_in_switches.md)\n");
        return 1;
    }
    if (parser_semantics_case() != 0) return 1;
    if (table_shape_case() != 0) return 1;
    if (kind_census_case() != 0) return 1;
    if (default_path_case() != 0) return 1;
    if (source_scan_case(repo) != 0) return 1;
    if (site_case(repo) != 0) return 1;
    if (wiring_case(repo) != 0) return 1;
    std::printf("opt_in_switches: PASS (%d checks, %zu switches, source root %s)\n", checks, kSwitchCount,
                repo.generic_string().c_str());
    return 0;
}
