#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "lob/generator.hpp"
#include "lob/multi.hpp"

namespace lob {

// Seeded multi-instrument workload. Deterministic and portable for the same reason `generate` is
// (std::mt19937_64 + plain modulo). Construction:
//   1. a seeded RNG decides, for each of `base.count` slots, which symbol it belongs to;
//   2. each symbol's slots are filled from its own independent `generate()` stream (derived seed), so
//      per-symbol behaviour (crossings, cancels, every order type) is exactly the single-book workload;
//   3. the streams are interleaved in the order decided in step 1.
// Order IDs restart at 1 in every symbol (IDs are per-symbol), so the same ID routinely exists in
// several books at once; that is deliberate: it exercises isolation.
struct MultiGeneratorConfig {
    GeneratorConfig base;           // base.count is the TOTAL number of commands across all symbols
    std::size_t symbols{4};         // 1 .. kMaxSymbols
    unsigned hot_percent{0};        // 0 = uniform; otherwise symbol 0 receives this % of commands (1..100)
    std::string prefix{"SYM"};      // names are prefix + index, e.g. SYM0, SYM1, ...
};

struct GeneratedMulti {
    SymbolTable symbols;
    std::vector<SymbolCommand> commands;
};

// Throws std::invalid_argument for symbols == 0, symbols > kMaxSymbols, hot_percent > 100, or a prefix
// that cannot form valid symbol names.
GeneratedMulti generate_multi(const MultiGeneratorConfig& config);

}  // namespace lob
