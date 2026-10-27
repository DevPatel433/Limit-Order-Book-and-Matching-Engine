#include "lob/multi_generator.hpp"

#include <random>
#include <stdexcept>

namespace lob {

GeneratedMulti generate_multi(const MultiGeneratorConfig& c) {
    if (c.symbols == 0 || c.symbols > kMaxSymbols) throw std::invalid_argument("symbols must be in [1, 65536]");
    if (c.hot_percent > 100) throw std::invalid_argument("hot_percent must be in [0, 100]");

    GeneratedMulti out;
    for (std::size_t s = 0; s < c.symbols; ++s) out.symbols.intern(c.prefix + std::to_string(s));  // throws if invalid

    // Step 1: which symbol owns each slot. A separate RNG stream from the per-symbol generators.
    std::mt19937_64 rng(c.base.seed ^ 0x9E3779B97F4A7C15ULL);
    const auto below = [&](std::uint64_t n) { return rng() % n; };
    std::vector<SymbolId> owner(c.base.count);
    std::vector<std::size_t> per_symbol(c.symbols, 0);
    for (std::size_t i = 0; i < c.base.count; ++i) {
        SymbolId s;
        if (c.symbols == 1) s = 0;
        else if (c.hot_percent != 0) s = below(100) < c.hot_percent ? 0 : static_cast<SymbolId>(1 + below(c.symbols - 1));
        else s = static_cast<SymbolId>(below(c.symbols));
        owner[i] = s;
        ++per_symbol[s];
    }

    // Step 2: one independent single-book stream per symbol.
    std::vector<std::vector<Command>> streams(c.symbols);
    for (std::size_t s = 0; s < c.symbols; ++s) {
        GeneratorConfig g = c.base;
        g.count = per_symbol[s];
        g.seed = c.base.seed * 1000003ULL + s + 1;  // unsigned wraparound is well defined
        streams[s] = generate(g);
    }

    // Step 3: interleave.
    std::vector<std::size_t> cursor(c.symbols, 0);
    out.commands.reserve(c.base.count);
    for (std::size_t i = 0; i < c.base.count; ++i) {
        const SymbolId s = owner[i];
        out.commands.push_back(SymbolCommand{s, streams[s][cursor[s]++]});
    }
    return out;
}

}  // namespace lob
