#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace psprecomp {

// Optional guest address -> symbol name resolver (e.g. a profile's symbol
// map), used only by diagnostics. Kept out of runtime.hpp on purpose: the
// generated units include that header, so touching it rebuilds the corpus.
using Symbolizer = std::function<std::string(std::uint32_t)>;

void set_symbolizer(Symbolizer symbolizer);

// "0xADDR <name+0xN>" when the resolver knows the address, else "0xADDR".
[[nodiscard]] std::string describe_address(std::uint32_t address);

}  // namespace psprecomp
