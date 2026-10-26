#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lob/order.hpp"

namespace lob {

// One line of a replay file. See docs/replay_format.md.
struct NewCommand {
    Order order;
    friend bool operator==(const NewCommand&, const NewCommand&) = default;
};
struct CancelCommand {
    OrderId id{};
    friend bool operator==(const CancelCommand&, const CancelCommand&) = default;
};
struct AmendCommand {
    OrderId id{};
    Price price{};
    Quantity quantity{};
    friend bool operator==(const AmendCommand&, const AmendCommand&) = default;
};
struct SnapshotCommand {
    std::optional<std::size_t> depth;  // empty -> use the replay's default depth
    friend bool operator==(const SnapshotCommand&, const SnapshotCommand&) = default;
};

using Command = std::variant<NewCommand, CancelCommand, AmendCommand, SnapshotCommand>;

struct ParseError {
    std::size_t line{};  // 1-based line number in the input
    std::string message;
};

struct ParseResult {
    std::vector<Command> commands;     // everything parsed before any error
    std::optional<ParseError> error;   // first error; parsing stops there
    bool ok() const noexcept { return !error.has_value(); }
};

// Parses ONE line (blank/comment lines succeed with `cmd` left empty). Returns false and fills `error`
// (with `lineno`) on a bad line. This is the single definition of the single-book line syntax; the
// multi-instrument parser strips its `@SYMBOL` prefix and delegates the rest here.
bool parse_command_line(std::string_view line, std::size_t lineno, std::optional<Command>& cmd,
                        ParseError& error);

// Strict parser: unknown keywords, wrong argument counts, non-numeric or
// out-of-range numbers, and trailing junk are all errors (never silently skipped).
ParseResult parse_commands(std::istream& in);
ParseResult parse_commands(std::string_view text);

// Canonical text form; parse_commands(format_command(c)) round-trips.
std::string format_command(const Command& c);
void write_commands(std::ostream& out, const std::vector<Command>& commands);

}  // namespace lob
