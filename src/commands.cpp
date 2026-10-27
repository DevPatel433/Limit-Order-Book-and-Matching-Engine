#include "lob/commands.hpp"

#include <cctype>
#include <charconv>
#include <istream>
#include <ostream>
#include <sstream>

namespace lob {

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

// Whole token must be a valid number of type T (no '+', no spaces, no overflow).
template <class T>
bool parse_num(std::string_view tok, T& out) {
    const char* end = tok.data() + tok.size();
    const auto [ptr, ec] = std::from_chars(tok.data(), end, out);
    return ec == std::errc() && ptr == end;
}

std::optional<Side> parse_side(std::string_view tok) {
    const std::string u = upper(tok);
    if (u == "BUY" || u == "B") return Side::Buy;
    if (u == "SELL" || u == "S") return Side::Sell;
    return std::nullopt;
}

std::optional<TimeInForce> parse_tif(std::string_view tok) {
    const std::string u = upper(tok);
    if (u == "GTC") return TimeInForce::GTC;
    if (u == "IOC") return TimeInForce::IOC;
    if (u == "FOK") return TimeInForce::FOK;
    return std::nullopt;
}

}  // namespace

// Returns false and fills `error` on a bad line. A blank/comment line yields cmd == nullopt.
bool parse_command_line(std::string_view line, std::size_t lineno, std::optional<Command>& cmd,
                ParseError& error) {
    cmd.reset();
    if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    const auto tok = tokenize(line);
    if (tok.empty()) return true;

    auto fail = [&](std::string msg) {
        error = ParseError{lineno, std::move(msg)};
        return false;
    };
    const auto quoted = [](std::string_view t) { return "'" + std::string(t) + "'"; };

    const std::string kw = upper(tok[0]);
    if (kw == "NEW") {
        if (tok.size() != 5 && tok.size() != 6)
            return fail("NEW expects: NEW <id> <BUY|SELL> <price|MKT> <quantity> [GTC|IOC|FOK]");
        Order o;
        if (!parse_num(tok[1], o.id)) return fail("invalid order id " + quoted(tok[1]));
        const auto side = parse_side(tok[2]);
        if (!side) return fail("invalid side " + quoted(tok[2]) + " (expected BUY or SELL)");
        o.side = *side;
        if (upper(tok[3]) == "MKT") {
            o.type = OrderType::Market;
            o.price = 0;
            o.tif = TimeInForce::IOC;  // default for market orders; GTC is parsed but the engine rejects it
        } else if (!parse_num(tok[3], o.price)) {
            return fail("invalid price " + quoted(tok[3]) + " (a number or MKT)");
        }
        if (!parse_num(tok[4], o.quantity)) return fail("invalid quantity " + quoted(tok[4]));
        if (tok.size() == 6) {
            const auto tif = parse_tif(tok[5]);
            if (!tif) return fail("invalid time in force " + quoted(tok[5]) + " (expected GTC, IOC or FOK)");
            o.tif = *tif;
        }
        cmd = NewCommand{o};
    } else if (kw == "CANCEL") {
        if (tok.size() != 2) return fail("CANCEL expects: CANCEL <id>");
        CancelCommand c;
        if (!parse_num(tok[1], c.id)) return fail("invalid order id " + quoted(tok[1]));
        cmd = c;
    } else if (kw == "AMEND") {
        if (tok.size() != 4) return fail("AMEND expects: AMEND <id> <price> <quantity>");
        AmendCommand a;
        if (!parse_num(tok[1], a.id)) return fail("invalid order id " + quoted(tok[1]));
        if (!parse_num(tok[2], a.price)) return fail("invalid price " + quoted(tok[2]));
        if (!parse_num(tok[3], a.quantity)) return fail("invalid quantity " + quoted(tok[3]));
        cmd = a;
    } else if (kw == "SNAPSHOT") {
        if (tok.size() > 2) return fail("SNAPSHOT expects: SNAPSHOT [depth]");
        SnapshotCommand s;
        if (tok.size() == 2) {
            std::size_t d = 0;
            if (!parse_num(tok[1], d)) return fail("invalid depth " + quoted(tok[1]));
            s.depth = d;
        }
        cmd = s;
    } else {
        return fail("unknown command " + quoted(tok[0]));
    }
    return true;
}

ParseResult parse_commands(std::istream& in) {
    ParseResult result;
    std::string line;
    std::size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        std::optional<Command> cmd;
        ParseError err;
        if (!parse_command_line(line, lineno, cmd, err)) {
            result.error = std::move(err);
            return result;
        }
        if (cmd) result.commands.push_back(*cmd);
    }
    return result;
}

ParseResult parse_commands(std::string_view text) {
    std::istringstream in{std::string(text)};
    return parse_commands(in);
}

std::string format_command(const Command& c) {
    std::ostringstream os;
    if (const auto* n = std::get_if<NewCommand>(&c)) {
        const Order& o = n->order;
        os << "NEW " << o.id << (o.side == Side::Buy ? " BUY " : " SELL ");
        if (o.type == OrderType::Market) os << "MKT";
        else os << o.price;
        os << ' ' << o.quantity;
        if (o.tif != TimeInForce::GTC || o.type == OrderType::Market) os << ' ' << to_string(o.tif);
    } else if (const auto* x = std::get_if<CancelCommand>(&c)) {
        os << "CANCEL " << x->id;
    } else if (const auto* a = std::get_if<AmendCommand>(&c)) {
        os << "AMEND " << a->id << ' ' << a->price << ' ' << a->quantity;
    } else {
        const auto& s = std::get<SnapshotCommand>(c);
        os << "SNAPSHOT";
        if (s.depth) os << ' ' << *s.depth;
    }
    return os.str();
}

void write_commands(std::ostream& out, const std::vector<Command>& commands) {
    for (const Command& c : commands) out << format_command(c) << '\n';
}

}  // namespace lob
