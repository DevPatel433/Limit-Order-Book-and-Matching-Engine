#include "lob/multi.hpp"

#include <cctype>
#include <istream>
#include <ostream>
#include <sstream>

namespace lob {

bool SymbolTable::valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxSymbolNameLength) return false;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_' || ch == '.' || ch == '-')) return false;
    }
    return true;
}

SymbolId SymbolTable::intern(std::string_view name) {
    if (!valid_name(name)) throw std::invalid_argument("invalid symbol name '" + std::string(name) + "'");
    if (const auto it = ids_.find(name); it != ids_.end()) return it->second;
    if (names_.size() >= kMaxSymbols) throw std::length_error("too many symbols");
    const auto id = static_cast<SymbolId>(names_.size());
    names_.emplace_back(name);
    ids_.emplace(std::string(name), id);
    return id;
}

std::optional<SymbolId> SymbolTable::find(std::string_view name) const {
    const auto it = ids_.find(name);
    if (it == ids_.end()) return std::nullopt;
    return it->second;
}

const std::string& SymbolTable::name(SymbolId id) const {
    if (id >= names_.size()) throw std::out_of_range("unknown symbol id");
    return names_[id];
}

namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// One line of the multi format. Returns false and fills `error` on a bad line; a blank or comment-only
// line succeeds with `out` left empty.
bool parse_multi_line(std::string_view line, std::size_t lineno, const std::optional<std::string>& default_symbol,
                      SymbolTable& symbols, std::optional<SymbolCommand>& out, ParseError& error) {
    out.reset();
    if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);

    std::size_t i = 0;
    while (i < line.size() && is_space(line[i])) ++i;
    if (i == line.size()) return true;  // blank or comment only

    auto fail = [&](std::string msg) {
        error = ParseError{lineno, std::move(msg)};
        return false;
    };

    std::string_view symbol_name;
    std::string_view rest = line.substr(i);
    if (line[i] == '@') {
        std::size_t j = i;
        while (j < line.size() && !is_space(line[j])) ++j;
        symbol_name = line.substr(i + 1, j - i - 1);
        rest = line.substr(j);
        if (!SymbolTable::valid_name(symbol_name))
            return fail("invalid symbol '@" + std::string(symbol_name) +
                        "' (1-16 characters from A-Z a-z 0-9 _ . -)");
        bool has_command = false;
        for (const char c : rest) has_command = has_command || !is_space(c);
        if (!has_command) return fail("'@" + std::string(symbol_name) + "' must be followed by a command");
    } else if (default_symbol) {
        symbol_name = *default_symbol;
    } else {
        return fail("missing @SYMBOL prefix (e.g. '@AAPL NEW 1 BUY 100 5')");
    }

    std::optional<Command> cmd;
    if (!parse_command_line(rest, lineno, cmd, error)) return false;
    if (!cmd) return true;  // unreachable for non-blank `rest`, but harmless

    SymbolId id;
    try {
        id = symbols.intern(symbol_name);
    } catch (const std::length_error&) {
        return fail("too many distinct symbols (limit " + std::to_string(kMaxSymbols) + ")");
    } catch (const std::invalid_argument&) {
        return fail("invalid default symbol '" + std::string(symbol_name) + "'");
    }
    out = SymbolCommand{id, *cmd};
    return true;
}

}  // namespace

MultiParseResult parse_multi_commands(std::istream& in, const std::optional<std::string>& default_symbol) {
    MultiParseResult result;
    std::string line;
    std::size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        std::optional<SymbolCommand> cmd;
        ParseError err;
        if (!parse_multi_line(line, lineno, default_symbol, result.symbols, cmd, err)) {
            result.error = std::move(err);
            return result;
        }
        if (cmd) result.commands.push_back(std::move(*cmd));
    }
    return result;
}

MultiParseResult parse_multi_commands(std::string_view text, const std::optional<std::string>& default_symbol) {
    std::istringstream in{std::string(text)};
    return parse_multi_commands(in, default_symbol);
}

std::string format_multi_command(const SymbolTable& symbols, const SymbolCommand& c) {
    return "@" + symbols.name(c.symbol) + " " + format_command(c.command);
}

void write_multi_commands(std::ostream& out, const SymbolTable& symbols, const std::vector<SymbolCommand>& commands) {
    for (const SymbolCommand& c : commands) out << format_multi_command(symbols, c) << '\n';
}

}  // namespace lob
