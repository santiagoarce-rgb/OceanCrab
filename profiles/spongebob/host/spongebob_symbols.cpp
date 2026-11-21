#include "spongebob_symbols.hpp"

#include "psprecomp/symbolizer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#ifndef SPONGEBOB_ANALYSIS_DIR
#define SPONGEBOB_ANALYSIS_DIR "profiles/spongebob/analysis"
#endif

namespace spongebob {
namespace {

// Function start -> name ("" when the function is known but unnamed).
std::map<std::uint32_t, std::string> g_symbols;
// Offsets beyond this are not attributed to the preceding function.
constexpr std::uint32_t kMaxOffset = 0x4000u;

void load_csv(const std::filesystem::path &path, int address_column, int name_column) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::string cols[3];
        std::size_t start = 0u;
        for (int i = 0; i <= std::max(address_column, name_column) && i < 3; ++i) {
            const std::size_t comma = line.find(',', start);
            cols[i] = line.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (comma == std::string::npos) break;
            start = comma + 1u;
        }
        const std::uint32_t address =
            static_cast<std::uint32_t>(std::strtoul(cols[address_column].c_str(), nullptr, 16));
        if (address == 0u) continue;
        std::string name = name_column >= 0 ? cols[name_column] : std::string{};
        auto [it, inserted] = g_symbols.emplace(address, name);
        if (!inserted && it->second.empty()) it->second = name;
    }
}

}  // namespace

void install_symbols(psprecomp::Runtime &) {
    const char *env = std::getenv("PSPRECOMP_SYMBOLS");
    if (env != nullptr && std::strcmp(env, "0") == 0) return;
    const std::filesystem::path analysis = SPONGEBOB_ANALYSIS_DIR;
    const std::filesystem::path symbols =
        env != nullptr && *env != '\0' ? std::filesystem::path(env) : analysis / "symbols" / "symbols.csv";
    load_csv(symbols, 0, 1);
    const std::size_t named = g_symbols.size();
    // Unnamed function starts bound the "+offset" attribution.
    load_csv(analysis / "ULUS10478_report_functions_auto.csv", 1, -1);
    if (g_symbols.empty()) return;
    std::cerr << "[symbols] " << named << " names, " << g_symbols.size() << " function starts\n";
    psprecomp::set_symbolizer([](std::uint32_t address) { return symbol_for(address); });
}

std::string symbol_for(std::uint32_t address) {
    auto it = g_symbols.upper_bound(address);
    if (it == g_symbols.begin()) return {};
    --it;
    const std::uint32_t offset = address - it->first;
    if (offset > kMaxOffset) return {};
    char buffer[32];
    std::string name = it->second;
    if (name.empty()) {
        std::snprintf(buffer, sizeof buffer, "sub_%08X", it->first);
        name = buffer;
    }
    if (offset != 0u) {
        std::snprintf(buffer, sizeof buffer, "+0x%X", offset);
        name += buffer;
    }
    return name;
}

}  // namespace spongebob
