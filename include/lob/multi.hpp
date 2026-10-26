#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/apply.hpp"
#include "lob/commands.hpp"
#include "lob/events.hpp"
#include "lob/matching_engine.hpp"

namespace lob {

// Phase 7: multiple instruments. The single-book engine is NOT modified: every instrument gets its
// own independent BasicMatchingEngine, and this layer only routes commands to the right one.
//
// Order IDs are scoped PER SYMBOL: "order 1" in AAPL and "order 1" in MSFT are unrelated orders, and
// a cancel/amend names the symbol it applies to. (Rationale and the alternative: docs/phase7_design.md.)
using SymbolId = std::uint32_t;

// Hard cap on distinct symbols. Keeps the router's id-indexed vector small and gives a hostile or
// corrupt input a defined failure instead of a multi-gigabyte allocation.
inline constexpr SymbolId kMaxSymbols = 1u << 16;
inline constexpr std::size_t kMaxSymbolNameLength = 16;

// Interns symbol names to dense ids in order of first appearance (so ids, and everything keyed by them,
// are deterministic for a given input).
class SymbolTable {
public:
    // Valid names are 1..16 characters from [A-Za-z0-9_.-] and are case-sensitive ("aapl" != "AAPL").
    static bool valid_name(std::string_view name) noexcept;

    // Returns the id for `name`, adding it if new. Throws std::invalid_argument for an invalid name and
    // std::length_error beyond kMaxSymbols.
    SymbolId intern(std::string_view name);
    std::optional<SymbolId> find(std::string_view name) const;
    const std::string& name(SymbolId id) const;  // throws std::out_of_range
    std::size_t size() const noexcept { return names_.size(); }

private:
    std::vector<std::string> names_;
    std::map<std::string, SymbolId, std::less<>> ids_;
};

// A command addressed to one instrument.
struct SymbolCommand {
    SymbolId symbol{};
    Command command;
    friend bool operator==(const SymbolCommand&, const SymbolCommand&) = default;
};

// An engine event tagged with where and why it happened. `seq` is the 1-based index of the command
// (in the input stream, SNAPSHOT lines included) that produced it. Events of one command are contiguous
// and in the order the engine emitted them.
struct SymbolEvent {
    std::uint64_t seq{};
    SymbolId symbol{};
    Event event;
    friend bool operator==(const SymbolEvent&, const SymbolEvent&) = default;
};

// Routes commands to one engine per symbol, created on first use. Not thread-safe: in the sharded
// pipeline each shard owns its own BasicMultiEngine and only that shard's thread touches it.
template <class Policy>
class BasicMultiEngine {
public:
    using Engine = BasicMatchingEngine<Policy>;

    // The engine for `symbol`, created if needed. Throws std::out_of_range for symbol >= kMaxSymbols.
    Engine& engine(SymbolId symbol) {
        if (symbol >= kMaxSymbols) throw std::out_of_range("symbol id out of range");
        if (symbol >= engines_.size()) engines_.resize(static_cast<std::size_t>(symbol) + 1);
        auto& slot = engines_[symbol];
        if (!slot) slot = std::make_unique<Engine>();
        return *slot;
    }
    // nullptr if this symbol has never been used.
    const Engine* find(SymbolId symbol) const noexcept {
        return symbol < engines_.size() ? engines_[symbol].get() : nullptr;
    }

    // Appends the command's events to `out` (SNAPSHOT produces none), exactly like apply_command.
    void apply(SymbolId symbol, const Command& cmd, std::vector<Event>& out) {
        apply_command(engine(symbol), cmd, out);
    }

    std::size_t engine_count() const noexcept {
        std::size_t n = 0;
        for (const auto& e : engines_) n += e ? 1 : 0;
        return n;
    }

    // Calls f(SymbolId, const Engine&) in ascending symbol-id order.
    template <class F>
    void for_each(F&& f) const {
        for (std::size_t s = 0; s < engines_.size(); ++s)
            if (engines_[s]) f(static_cast<SymbolId>(s), *engines_[s]);
    }

private:
    std::vector<std::unique_ptr<Engine>> engines_;
};

using MultiEngine = BasicMultiEngine<StdPolicy>;
using PooledMultiEngine = BasicMultiEngine<PoolPolicy>;

// ---- Multi-instrument text format (see docs/replay_format.md) -------------------------------------
//   @<SYMBOL> <any single-book command>        e.g.  @AAPL NEW 1 BUY 100 5
// Lines without a prefix are an error unless a default symbol is supplied.

struct MultiParseResult {
    SymbolTable symbols;                 // every symbol that appeared, in order of first appearance
    std::vector<SymbolCommand> commands; // everything parsed before any error
    std::optional<ParseError> error;     // first error; parsing stops there
    bool ok() const noexcept { return !error.has_value(); }
};

// Strict, like parse_commands: the part after the prefix is parsed by the same code as a single-book
// line, so the two formats cannot drift apart.
MultiParseResult parse_multi_commands(std::istream& in,
                                      const std::optional<std::string>& default_symbol = std::nullopt);
MultiParseResult parse_multi_commands(std::string_view text,
                                      const std::optional<std::string>& default_symbol = std::nullopt);

// Canonical text form; parse_multi_commands(format_multi_command(...)) round-trips.
std::string format_multi_command(const SymbolTable& symbols, const SymbolCommand& c);
void write_multi_commands(std::ostream& out, const SymbolTable& symbols, const std::vector<SymbolCommand>& commands);

}  // namespace lob
